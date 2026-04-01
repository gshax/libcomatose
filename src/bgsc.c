/*
 * bgsc.c - BGSC (background scheduler) implementation for L16 passthrough
 *
 * replaces stock app_dsp's ARM-side DSP processing. for L16 (linear 16-bit
 * PCM), the ARM codec function is literally a no-op — the CSS handles data
 * movement internally. the ARM only needs to:
 *
 *   1. initialize shared memory (ring buffer header, codec state area,
 *      version tag)
 *   2. build the FTAB (flow table) in process memory with correct control
 *      word pointers into shared memory
 *   3. run the dispatch loop: read control words, acknowledge activation
 *      bits, maintain state machine
 *   4. send ring buffer messages: "level ready" after startup, periodic
 *      heartbeat ticks
 *
 * the FTAB has 320 entries organized as 16 groups of 20 (one group per
 * SPVOIPNDA unit). within each group, entry[0] is the codec slot and
 * entries[1-19] are signal processing helpers. for L16, ALL entries are
 * no-ops — we just process control words without calling any functions.
 *
 * control word offsets in shared memory follow a regular pattern:
 * group N starts at base_offset + N * 0xb6c, with 20 fixed relative
 * offsets within each group.
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

/* allocation sizes */
#define RINGBUF_HDR_SIZE   0x10
#define CODEC_STATE_SIZE   0x79c
#define RINGBUF_ENTRIES    487   /* 0x1e7 */

/*
 * element descriptor table location in shared memory.
 * the CSS creates 368 DSP element descriptors during DUA init and stores
 * pointers to them in a table at a FIXED offset (shm+0xb854). each pointer
 * is a userspace virtual address pointing into shared memory. the first
 * word at each pointed-to location is the control word for that element.
 *
 * the table has 368 entries organized as groups of ~23 per SPVOIPNDA unit,
 * but the exact within-group offsets and group stride vary between boots
 * (depends on CSS allocation order). we read the table at runtime.
 */
#define ELEM_TABLE_OFF     0xb854  /* fixed offset in shared memory */
#define ELEM_TABLE_COUNT   368     /* number of elements */

struct bgsc_ctx {
	void *shm;
	uintptr_t shm_mmap_base;       /* userspace mmap address of shm */

	/* ring buffer state */
	uint32_t rb_hdr_off;
	uint32_t rb_data_off;
	uint32_t rb_data_end;
	uint32_t rb_write_off;

	/* control word pointers — read from element table at runtime */
	volatile uint32_t *control_words[ELEM_TABLE_COUNT];
	int num_control_words;
	uint32_t frame_tick;            /* dispatch cycle counter */

	pthread_t threads[3];           /* BG levels 1-3 */
	volatile int running;
};

static inline uint32_t shm_read32(void *shm, uint32_t offset)
{
	return *(volatile uint32_t *)((char *)shm + offset);
}

static inline void shm_write32(void *shm, uint32_t offset, uint32_t val)
{
	*(volatile uint32_t *)((char *)shm + offset) = val;
	__sync_synchronize();
}

/*
 * ring buffer — writes directly to shared memory so CSS can read
 *
 * the ring buffer header is 4 uint32 values at the allocated area:
 *   [0] = base pointer (shm-relative)
 *   [1] = end pointer (shm-relative)
 *   [2] = write pointer (shm-relative, updated by ARM)
 *   [3] = read pointer (shm-relative, updated by CSS)
 *
 * message data follows immediately after the header.
 */

struct ringbuf {
	void     *shm;          /* shared memory base */
	uint32_t  hdr_off;      /* offset of header in shm */
	uint32_t  data_off;     /* offset of data area in shm */
	uint32_t  data_end;     /* end offset of data area */
	uint32_t  write_off;    /* current write offset */
};

static void ringbuf_init(struct ringbuf *rb, void *shm,
                         uint32_t hdr_off, uint32_t data_off, int num_entries)
{
	rb->shm = shm;
	rb->hdr_off = hdr_off;
	rb->data_off = data_off;
	rb->data_end = data_off + (uint32_t)num_entries * 4;
	rb->write_off = data_off;

	/* write header to shared memory */
	shm_write32(shm, hdr_off + 0x00, data_off);      /* base */
	shm_write32(shm, hdr_off + 0x04, rb->data_end);  /* end */
	shm_write32(shm, hdr_off + 0x08, data_off);      /* write_ptr */
	shm_write32(shm, hdr_off + 0x0c, data_off);      /* read_ptr */
}

static void ringbuf_write_word(struct ringbuf *rb, uint32_t val)
{
	if (rb->write_off >= rb->data_end)
		rb->write_off = rb->data_off;  /* wrap */
	shm_write32(rb->shm, rb->write_off, val);
	rb->write_off += 4;
}

static void ringbuf_commit(struct ringbuf *rb)
{
	/* update write pointer in header so CSS sees the new data */
	shm_write32(rb->shm, rb->hdr_off + 0x08, rb->write_off);
}

/* send "level ready" message: [0, (1<<13)|1, level] */
static void ringbuf_send_level_ready(struct ringbuf *rb, int level)
{
	ringbuf_write_word(rb, 0);
	ringbuf_write_word(rb, (1 << 13) | 1);
	ringbuf_write_word(rb, (uint32_t)level);
	ringbuf_commit(rb);
}

/* send heartbeat tick: [0, (4<<13)|0] */
static void ringbuf_send_tick(struct ringbuf *rb)
{
	ringbuf_write_word(rb, 0);
	ringbuf_write_word(rb, (4 << 13) | 0);
	ringbuf_commit(rb);
}

/*
 * FTAB dispatch — process control words from the element table
 *
 * the CSS creates 368 DSP element descriptors during DUA init. each
 * descriptor has a control word as its first field. we read the control
 * word pointers from the element table at shm+0xb854 during init.
 *
 * when the CSS needs the ARM to process something, it sets bits on a
 * control word. our dispatch reads these words and acknowledges them.
 * for L16 passthrough, no actual codec work is needed — just protocol.
 *
 * dispatch protocol (from RE of dfl_process_flow):
 *   bit 0 (0x01): ACTIVE
 *   bits 1-4: pending work (0x14 = SwitchInstance activation)
 *   bit 5 (0x20): PREPARE_RUN
 *   bit 7 (0x80): ALT_PREPARE
 *   after processing: set to (val & 6) ? 1 : 0
 */
static void ftab_dispatch(struct bgsc_ctx *ctx)
{
	ctx->frame_tick++;

	/*
	 * stock dispatch (dfl_process_flow_asm) processes ALL entries:
	 *
	 *   val & 1 (ACTIVE):     call calc_func+0x0c (no-op), continue
	 *   (val & 0x1f) == 0:    call calc_func+0x08 (signal processing), continue
	 *   bits 1-4 set:         call calc_func+0x08, post-calc, set cw
	 *
	 * critically: val==0 entries ARE processed! the +0x08 function reads
	 * element+8 and accumulates (val << 4) across all entries. this
	 * accumulation produces a frame counter/pointer that the CSS uses.
	 *
	 * the dispatch uses tail calls (bx, not blx) with LR preset to
	 * loop top, so it iterates through ALL entries before returning.
	 */

	uint32_t accumulator = 0;

	for (int i = 0; i < ctx->num_control_words; i++) {
		volatile uint32_t *cw = ctx->control_words[i];
		if (!cw)
			continue;

		uint32_t val = *cw;

		/* skip large values (data, not control words) */
		if (val > 0x3ff && val != 0)
			continue;

		if (val > 1) {
			/* bits 1-4: activation (0x14 from SwitchInstance, etc.)
			 * transition and send completion event */
			uint32_t new_val = (val & 6) ? 1 : 0;
			*cw = new_val;

			/* set elem+4 = 1 only if it's currently 0.
			 * elem[6] and elem[21] need +4 = 1 (status flag).
			 * elem[22] has a signal block POINTER at +4 that
			 * we must not overwrite. checking for 0 protects
			 * against corrupting pre-existing pointer values. */
			if (*(cw + 1) == 0) {
				*(cw + 1) = 1;
			}

			/* pre-set buffer ready flag for encoder channels.
			 * real buffer pointers are shm addresses (0xb6xxxxxx),
			 * not small values like 0x000200a2. */
			uint32_t buf_addr = *(cw + 5);
			if (buf_addr > 0xb0000000) {
				*(cw + 4) = 0x00010001;
			}
			__sync_synchronize();
			usleep(1000);

			/* send type=0 completion event */
			uintptr_t cw_vaddr = (uintptr_t)cw;
			uint32_t inst_addr = (uint32_t)(cw_vaddr + 4);
			if (ctx->rb_write_off + 8 >= ctx->rb_data_end)
				ctx->rb_write_off = ctx->rb_data_off;
			shm_write32(ctx->shm, ctx->rb_write_off, inst_addr);
			ctx->rb_write_off += 4;
			shm_write32(ctx->shm, ctx->rb_write_off, 0);
			ctx->rb_write_off += 4;
			uintptr_t wp_vaddr = (uintptr_t)ctx->shm + ctx->rb_write_off;
			shm_write32(ctx->shm, ctx->rb_hdr_off + 0x08, (uint32_t)wp_vaddr);
		}
		else if (val == 1) {
			/* ACTIVE: stock dispatch calls no-op calc and continues.
			 * leave control word at 1. */

			/* also do the signal processing accumulation
			 * (same as val==0 path — read element+8, accumulate) */
			uint32_t elem8 = *(cw + 2); /* element+8 */
			accumulator += elem8 << 4;

			/* manage buffer ready flag */
			volatile uint16_t *ready = (volatile uint16_t *)(cw + 4);
			uint16_t flag = *ready;
			if (flag == 4) {
				*ready = 1;
				__sync_synchronize();
			}
		}
		else { /* val == 0 */
			/* stock dispatch calls calc_func+0x08 (main processing)
			 * which reads element+8 and accumulates across entries.
			 * this is the signal block pointer accumulation that
			 * tells the CSS framework where audio data is. */
			uint32_t elem8 = *(cw + 2); /* element+8 */
			accumulator += elem8 << 4;
		}
	}

	(void)accumulator;

	/* send periodic type=3 event for the DECODER element.
	 *
	 * from observing stock app_dsp: it sends [inst_addr, 0x6001, 0]
	 * where inst_addr = decoder_element_address + 4, every ~10ms.
	 * the decoder callback in p_auc_CB_handler (0x6000 branch)
	 * triggers the CSS to produce the next encoder frame.
	 *
	 * elem[21] is the decoder for SPVOIPNDA unit 0 (first group).
	 * message format: type=3, 1 param = 0. */
	if (ctx->num_control_words > 21 && (ctx->frame_tick & 1) == 0) {
		volatile uint32_t *dec_cw = ctx->control_words[21];
		if (dec_cw && *dec_cw == 1) {
			uintptr_t dec_vaddr = (uintptr_t)dec_cw;
			uint32_t inst_addr = (uint32_t)(dec_vaddr + 4);

			if (ctx->rb_write_off + 12 >= ctx->rb_data_end)
				ctx->rb_write_off = ctx->rb_data_off;
			shm_write32(ctx->shm, ctx->rb_write_off, inst_addr);
			ctx->rb_write_off += 4;
			shm_write32(ctx->shm, ctx->rb_write_off, (3 << 13) | 1); /* type=3, 1 param */
			ctx->rb_write_off += 4;
			shm_write32(ctx->shm, ctx->rb_write_off, 0); /* param = 0 */
			ctx->rb_write_off += 4;
			uintptr_t wp_vaddr = (uintptr_t)ctx->shm + ctx->rb_write_off;
			shm_write32(ctx->shm, ctx->rb_hdr_off + 0x08, (uint32_t)wp_vaddr);
		}
	}
}

/*
 * BG thread
 */
struct thread_args {
	bgsc_ctx_t *ctx;
	int level;
};

static void send_heartbeat(struct bgsc_ctx *ctx)
{
	/* heartbeat: [0, (4<<13)|0] — 2 words */
	if (ctx->rb_write_off + 8 >= ctx->rb_data_end)
		ctx->rb_write_off = ctx->rb_data_off; /* wrap */
	shm_write32(ctx->shm, ctx->rb_write_off, 0);
	ctx->rb_write_off += 4;
	shm_write32(ctx->shm, ctx->rb_write_off, (4 << 13) | 0);
	ctx->rb_write_off += 4;
	/* update write pointer in header */
	uintptr_t wp_vaddr = (uintptr_t)ctx->shm + ctx->rb_write_off;
	shm_write32(ctx->shm, ctx->rb_hdr_off + 0x08, (uint32_t)wp_vaddr);
}

static void *bg_thread(void *arg)
{
	struct thread_args *ta = arg;
	bgsc_ctx_t *ctx = ta->ctx;
	int level = ta->level;
	free(ta);

	struct timespec period = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
	int tick_count = 0;

	fprintf(stderr, "bgsc: level %d thread started\n", level);

	while (ctx->running) {
		ftab_dispatch(ctx);

		/* level 1 sends periodic heartbeat (stock app_dsp does this
		 * from level 0, but since CSS runs level 0 and we run 1-3,
		 * we send it from level 1) */
		if (level == 1) {
			tick_count++;
			if (tick_count >= 16) { /* every ~80ms (16 * 5ms) */
				send_heartbeat(ctx);
				tick_count = 0;
			}
		}

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

	ctx->shm_mmap_base = (uintptr_t)shm_ptr;

	/* read shared memory state */
	uint32_t alloc_wm = shm_read32(shm_ptr, SHM_ALLOC_WM);
	uint32_t ver = shm_read32(shm_ptr, SHM_DSP_VERSION);
	fprintf(stderr, "bgsc: shm alloc watermark=0x%x, DSP version=0x%08x\n",
	        alloc_wm, ver);

	/* write version tag (like app_dsp: shm+0x14 = version) */
	shm_write32(shm_ptr, SHM_VERSION_TAG, ver);

	/* allocate ring buffer from shared memory pool.
	 * layout: [header: 0x10] [data: 0x79c]
	 * header contains USERSPACE VIRTUAL ADDRESSES for CSS MMU translation. */
	uint32_t hdr_off = alloc_wm;
	uint32_t data_off = alloc_wm + RINGBUF_HDR_SIZE;
	uint32_t data_end_off = data_off + RINGBUF_ENTRIES * 4;

	shm_write32(shm_ptr, SHM_SAVED_ALLOC, hdr_off);
	shm_write32(shm_ptr, SHM_CODEC_STATE, data_off);
	memset((char *)shm_ptr + data_off, 0, RINGBUF_ENTRIES * 4);

	uintptr_t data_vaddr = (uintptr_t)shm_ptr + data_off;
	uintptr_t data_end_vaddr = (uintptr_t)shm_ptr + data_end_off;
	shm_write32(shm_ptr, hdr_off + 0x00, (uint32_t)data_vaddr);
	shm_write32(shm_ptr, hdr_off + 0x04, (uint32_t)data_end_vaddr);
	shm_write32(shm_ptr, hdr_off + 0x08, (uint32_t)data_vaddr);
	shm_write32(shm_ptr, hdr_off + 0x0c, (uint32_t)data_vaddr);

	shm_write32(shm_ptr, SHM_ALLOC_WM, data_end_off);

	ctx->rb_hdr_off = hdr_off;
	ctx->rb_data_off = data_off;
	ctx->rb_data_end = data_end_off;
	ctx->rb_write_off = data_off;

	fprintf(stderr, "bgsc: ring buffer at shm+0x%x (vaddr=0x%x)\n",
	        hdr_off, (uint32_t)data_vaddr);

	/* read element descriptor table at shm+0xb854.
	 * the CSS creates 368 DSP element descriptors during DUA init.
	 * each table entry is a userspace virtual address pointing to a
	 * descriptor in shared memory. the first word of each descriptor
	 * is the control word we need to monitor. */
	ctx->num_control_words = 0;
	fprintf(stderr, "bgsc: shm mmap base = 0x%08x\n",
	        (uint32_t)ctx->shm_mmap_base);

	for (int i = 0; i < ELEM_TABLE_COUNT; i++) {
		uint32_t ptr = shm_read32(shm_ptr, ELEM_TABLE_OFF + i * 4);
		if (ptr == 0)
			break;
		/* convert userspace vaddr to local pointer */
		uintptr_t elem_vaddr = (uintptr_t)ptr;
		uintptr_t shm_start = ctx->shm_mmap_base;
		if (elem_vaddr < shm_start || elem_vaddr >= shm_start + 0x100000) {
			if (i < 5)
				fprintf(stderr, "bgsc: elem[%d] ptr=0x%08x OUT OF RANGE (shm=0x%08x)\n",
				        i, ptr, (uint32_t)shm_start);
			continue;
		}
		uint32_t elem_off = (uint32_t)(elem_vaddr - shm_start);
		ctx->control_words[ctx->num_control_words] =
			(volatile uint32_t *)((char *)shm_ptr + elem_off);
		if (i < 5)
			fprintf(stderr, "bgsc: elem[%d] ptr=0x%08x -> shm+0x%05x (cw=0x%08x)\n",
			        i, ptr, elem_off,
			        *(volatile uint32_t *)((char *)shm_ptr + elem_off));
		ctx->num_control_words++;
	}
	fprintf(stderr, "bgsc: read %d control words from element table\n",
	        ctx->num_control_words);

	return ctx;
}

int bgsc_start(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return -1;

	ctx->running = 1;

	/* send "level ready" messages for all 4 levels */
	for (int i = 0; i < 4; i++) {
		/* write message: [0, (1<<13)|1, level] */
		shm_write32(ctx->shm, ctx->rb_write_off, 0);
		ctx->rb_write_off += 4;
		shm_write32(ctx->shm, ctx->rb_write_off, (1 << 13) | 1);
		ctx->rb_write_off += 4;
		shm_write32(ctx->shm, ctx->rb_write_off, (uint32_t)i);
		ctx->rb_write_off += 4;

		/* update write pointer in header (as userspace vaddr) */
		uintptr_t wp_vaddr = (uintptr_t)ctx->shm + ctx->rb_write_off;
		shm_write32(ctx->shm, ctx->rb_hdr_off + 0x08, (uint32_t)wp_vaddr);

		fprintf(stderr, "bgsc: sent level %d ready\n", i);
	}

	/* spawn threads for levels 1-3 */
	for (int i = 0; i < 3; i++) {
		struct thread_args *ta = malloc(sizeof(*ta));
		if (!ta) return -1;
		ta->ctx = ctx;
		ta->level = i + 1;

		pthread_attr_t attr;
		pthread_attr_init(&attr);

		struct sched_param sp = { .sched_priority = 50 - i };
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_attr_setschedparam(&attr, &sp);
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

		int ret = pthread_create(&ctx->threads[i], &attr, bg_thread, ta);
		if (ret != 0) {
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
