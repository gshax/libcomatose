/*
 * bgsc.c - minimal BGSC (background scheduler) implementation
 *
 * replaces stock app_dsp's ARM-side codec processing for L16 passthrough.
 *
 * the BGSC system has 4 BG levels (0-3). level 0 runs on the CSS, levels
 * 1-3 run on the ARM. each level has a flow table (FTAB) with codec entries
 * and a state machine that progresses through startup to IDLE.
 *
 * this implementation focuses on the infrastructure: state machine transitions,
 * ring buffer messaging, heartbeat ticks, and FTAB entry dispatch. it does
 * NOT implement actual codec functions — L16 (linear PCM passthrough) is
 * handled by the CSS directly once the infrastructure is running.
 *
 * shared memory layout (from RE of app_dsp and live dumps):
 *
 * pool header:
 *   +0x04: pool_size (uint32, set by dua_init_hw)
 *   +0x08: alloc_watermark (uint32, advances as CSS allocates)
 *   +0x10: dsp_version (uint32, checked by p_dsp_app_startup)
 *   +0x14: version_tag (uint32, written by app_dsp after init)
 *   +0x28: saved_alloc_ptr (uint32, set by app_dsp)
 *   +0x2c: codec_state_ptr (uint32, set by app_dsp)
 *
 * app_dsp allocations (starting at alloc_watermark):
 *   +0x00: ARM→CSS ring buffer header (0x10 bytes)
 *   +0x10: codec state area (0x79c bytes)
 */

#include <comatose/bgsc.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

/* shared memory pool header offsets */
#define SHM_POOL_SIZE      0x04
#define SHM_ALLOC_WM       0x08
#define SHM_DSP_VERSION    0x10
#define SHM_VERSION_TAG    0x14
#define SHM_SAVED_ALLOC    0x28
#define SHM_CODEC_STATE    0x2c

/* expected DSP version (from p_dsp_app_startup) */
#define DSP_VERSION_EXPECTED  0x11400154

/* allocation sizes */
#define RINGBUF_HDR_SIZE   0x10
#define CODEC_STATE_SIZE   0x79c
#define RINGBUF_ENTRIES    487  /* 0x1e7 */

/* ring buffer structure in shared memory */
struct shm_ringbuf {
	uint32_t *base;
	uint32_t *end;
	uint32_t *write_ptr;
	uint32_t *read_ptr;
};

/* FTAB entry (flow table entry) */
struct ftab_entry {
	uint32_t *control_word;
	void     *param1;
	void     *param2;
	void     *calc_func;
};

/* per-level DFL processing state */
struct dfl_level {
	uint32_t *flow_table;    /* pointer to flow table in shared memory */
	int       state;         /* state machine state */
	uint32_t  tick_divisor;  /* heartbeat tick divisor (level 0 only) */
	uint32_t  tick_counter;  /* current tick count */
};

struct bgsc_ctx {
	void *shm;                    /* shared memory base */
	uint32_t *ringbuf_area;       /* our allocation in shm */
	struct shm_ringbuf *css2arm;  /* CSS→ARM ring buffer (set up by CSS) */
	struct shm_ringbuf arm2css;   /* ARM→CSS ring buffer (we init) */

	struct dfl_level levels[4];
	pthread_t threads[3];         /* BG levels 1-3 */
	volatile int running;
};

static inline uint32_t shm_read32(void *shm, int offset)
{
	return *(volatile uint32_t *)((char *)shm + offset);
}

static inline void shm_write32(void *shm, int offset, uint32_t val)
{
	*(volatile uint32_t *)((char *)shm + offset) = val;
	__sync_synchronize();
}

/*
 * ARM→CSS ring buffer messaging
 */
static void ringbuf_init(struct shm_ringbuf *rb, uint32_t *base, int num_entries)
{
	rb->base = base;
	rb->end = base + num_entries;
	rb->write_ptr = base;
	rb->read_ptr = base;
}

/* send a short message (instance, type, param) — 3 words */
static int ringbuf_send_short(struct shm_ringbuf *rb,
                              uint32_t instance, int type, uint32_t param)
{
	uint32_t *wp = rb->write_ptr;

	/* check space (simplified — no wrap check for initial impl) */
	if (wp + 3 >= rb->end)
		wp = rb->base;  /* wrap */

	wp[0] = instance;
	wp[1] = ((uint32_t)type << 13) | 1;
	wp[2] = param;
	__sync_synchronize();
	rb->write_ptr = wp + 3;
	return 0;
}

/* send heartbeat tick — 2 words (no params) */
static int ringbuf_send_tick(struct shm_ringbuf *rb)
{
	uint32_t *wp = rb->write_ptr;

	if (wp + 2 >= rb->end)
		wp = rb->base;

	wp[0] = 0;
	wp[1] = (4 << 13) | 0;
	__sync_synchronize();
	rb->write_ptr = wp + 2;
	return 0;
}

/*
 * FTAB dispatch — iterate flow table entries and process control words
 */
static void ftab_dispatch(uint32_t *flow_table)
{
	if (!flow_table)
		return;

	/* FTAB list is at flow_table + 0x0c (offset in words: 3) */
	struct ftab_entry *list = *(struct ftab_entry **)(flow_table + 3);
	if (!list)
		return;

	struct ftab_entry *cur = list;
	while (cur->control_word != NULL) {
		volatile uint32_t *cw = (volatile uint32_t *)cur->control_word;
		uint32_t val = *cw;

		if (val & 0x1f) {
			/* entry has pending work — process it.
			 * for L16 passthrough: the CSS handles the actual data movement.
			 * we just need to acknowledge the control word. */

			/* clear activation bits, set to "active" or "done" */
			if (val & 1) {
				/* already active — call would go to calc_func.
				 * for our purposes, just leave it running. */
			} else {
				/* pending activation (bits 5, 7) — acknowledge it */
				*cw = 1;
				__sync_synchronize();
			}
		}

		/* advance to next entry (entries are 16 bytes = 4 words) */
		cur++;
	}
}

/*
 * dfl_calc — per-level state machine + dispatch
 */
static void dfl_calc(struct bgsc_ctx *ctx, int level)
{
	struct dfl_level *lv = &ctx->levels[level];
	int state = lv->state;

	/* state machine progression: 2 → 3 → 4/5 → 0x200 → 1 (IDLE) */
	switch (state) {
	case 2:
		lv->state = 3;
		break;

	case 3:
		/* for levels 1-3: set up I-switch bits on FTAB entries */
		if (level == 0)
			lv->state = 4;
		else
			lv->state = 0x200;
		break;

	case 4:
		lv->state = 5;
		break;

	case 5:
		lv->state = 0x200;
		break;

	case 0x200:
		/* send "level ready" message to CSS */
		ringbuf_send_short(&ctx->arm2css, 0, 1, (uint32_t)level);
		lv->state = 1;
		break;

	case 1:
		/* IDLE — dispatch FTAB entries */
		break;

	default:
		break;
	}

	/* dispatch flow table if not in startup transition */
	if (lv->state == 1 || (state >= 0x100 && state < 0x200)) {
		ftab_dispatch(lv->flow_table);
	}

	/* level 0 only: periodic heartbeat */
	if (level == 0 && lv->tick_divisor > 0) {
		lv->tick_counter--;
		if (lv->tick_counter == 0) {
			lv->tick_counter = lv->tick_divisor;
			ringbuf_send_tick(&ctx->arm2css);
		}
	}
}

/*
 * BG thread — calls dfl_calc periodically
 */
struct thread_args {
	bgsc_ctx_t *ctx;
	int level;
};

static void *bg_thread(void *arg)
{
	struct thread_args *ta = arg;
	bgsc_ctx_t *ctx = ta->ctx;
	int level = ta->level;
	free(ta);

	/* 5ms period for all levels (conservative — stock uses variable rates) */
	struct timespec period = { .tv_sec = 0, .tv_nsec = 5000000 };

	fprintf(stderr, "bgsc: level %d thread started\n", level);

	while (ctx->running) {
		dfl_calc(ctx, level);
		nanosleep(&period, NULL);
	}

	fprintf(stderr, "bgsc: level %d thread stopped\n", level);
	return NULL;
}

/*
 * public API
 */

bgsc_ctx_t *bgsc_init(void *shm_ptr)
{
	if (!shm_ptr)
		return NULL;

	bgsc_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->shm = shm_ptr;

	/* read current allocation watermark */
	uint32_t alloc_wm = shm_read32(shm_ptr, SHM_ALLOC_WM);
	fprintf(stderr, "bgsc: shm alloc watermark at 0x%x\n", alloc_wm);

	/* check DSP version */
	uint32_t ver = shm_read32(shm_ptr, SHM_DSP_VERSION);
	fprintf(stderr, "bgsc: DSP version 0x%08x\n", ver);

	/* save current watermark (like app_dsp does at +0x28) */
	shm_write32(shm_ptr, SHM_SAVED_ALLOC, alloc_wm);

	/* allocate ring buffer header (0x10 bytes) from pool */
	ctx->ringbuf_area = (uint32_t *)((char *)shm_ptr + alloc_wm);
	alloc_wm += RINGBUF_HDR_SIZE;

	/* save codec state pointer (like app_dsp does at +0x2c) */
	shm_write32(shm_ptr, SHM_CODEC_STATE, alloc_wm);

	/* allocate codec state area (0x79c bytes) */
	uint32_t *codec_state = (uint32_t *)((char *)shm_ptr + alloc_wm);
	memset(codec_state, 0, CODEC_STATE_SIZE);
	alloc_wm += CODEC_STATE_SIZE;

	/* update watermark */
	shm_write32(shm_ptr, SHM_ALLOC_WM, alloc_wm);

	/* init ARM→CSS ring buffer in the allocated area */
	ringbuf_init(&ctx->arm2css, ctx->ringbuf_area, RINGBUF_ENTRIES);

	/* write version tag (like app_dsp: shm+0x14 = version) */
	shm_write32(shm_ptr, SHM_VERSION_TAG, ver);

	/* init level state machines — all start at state 2 */
	for (int i = 0; i < 4; i++) {
		ctx->levels[i].state = 2;
		ctx->levels[i].tick_divisor = 0x10;  /* heartbeat every 16 ticks */
		ctx->levels[i].tick_counter = 0x10;
	}

	/* try to find flow table pointers from shared memory.
	 * the CSS writes these during DUA init. the flow table
	 * pointer locations depend on the CSS firmware layout.
	 * for now, we leave them NULL and rely on the CSS to
	 * manage FTAB dispatch internally for L16. */

	fprintf(stderr, "bgsc: initialized (ringbuf at shm+0x%lx, codec_state at shm+0x%lx)\n",
	        (unsigned long)((char *)ctx->ringbuf_area - (char *)shm_ptr),
	        (unsigned long)((char *)codec_state - (char *)shm_ptr));

	return ctx;
}

int bgsc_start(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return -1;

	ctx->running = 1;

	/* run level 0 state machine inline (CSS level, just needs messages) */
	for (int i = 0; i < 10; i++)
		dfl_calc(ctx, 0);

	/* spawn threads for levels 1-3 */
	for (int i = 0; i < 3; i++) {
		struct thread_args *ta = malloc(sizeof(*ta));
		if (!ta) return -1;
		ta->ctx = ctx;
		ta->level = i + 1;

		pthread_attr_t attr;
		pthread_attr_init(&attr);

		/* try to set realtime priority (may fail without CAP_SYS_NICE) */
		struct sched_param sp = { .sched_priority = 50 - i };
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_attr_setschedparam(&attr, &sp);
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

		int ret = pthread_create(&ctx->threads[i], &attr, bg_thread, ta);
		if (ret != 0) {
			/* retry without RT priority */
			pthread_attr_init(&attr);
			ret = pthread_create(&ctx->threads[i], &attr, bg_thread, ta);
			if (ret != 0) {
				free(ta);
				fprintf(stderr, "bgsc: failed to create level %d thread\n", i + 1);
				ctx->running = 0;
				return -1;
			}
		}
		pthread_attr_destroy(&attr);
	}

	fprintf(stderr, "bgsc: 3 BG threads started\n");
	return 0;
}

void bgsc_stop(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return;

	ctx->running = 0;

	for (int i = 0; i < 3; i++) {
		if (ctx->threads[i])
			pthread_join(ctx->threads[i], NULL);
	}

	fprintf(stderr, "bgsc: stopped\n");
	free(ctx);
}
