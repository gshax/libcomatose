/*
 * bgsc.c - BGSC (background scheduler) implementation
 *
 * replaces stock app_dsp's ARM-side DSP framework executor. app_dsp is not a
 * regular COMA API consumer — it's an outsourced internal subsystem of the CSS
 * that runs BG processing levels 1-3 on the ARM core. the CSS runs level 0
 * internally.
 *
 * the ARM must:
 *   1. allocate ring buffer + codec state area from the shared memory pool
 *   2. read the element descriptor table (shm+0xb854) to find control words
 *   3. run the dispatch loop: read control words, clear pending bits per
 *      protocol, send decoder ack when the CSS signals a frame
 *   4. send infrastructure messages: "level ready" at startup, periodic
 *      heartbeat ticks (~80ms)
 *
 * for L16 (linear 16-bit PCM), the stock calc functions are all effectively
 * no-ops. the only real per-frame work is the decoder acknowledgement:
 * when the CSS sets bit 0 of decoder element+8, the ARM sends a type=1 ring
 * buffer message and clears element+8. this handshake drives frame production.
 *
 * element table layout: 368 entries at shm+0xb854, organized as groups of
 * ~24 per SPVOIPNDA unit. within each group, specific positions have defined
 * roles (from live FTAB dump of stock app_dsp):
 *   elem[0]  = codec (dsp_sko for L16: pure no-op)
 *   elem[6]  = encoder status
 *   elem[21] = decoder (frame counter / ack handshake)
 *   elem[22] = buffer management
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

/* element descriptor table in shared memory */
#define ELEM_TABLE_OFF     0xb854
#define ELEM_TABLE_COUNT   368

/* group layout constants */
#define ELEMS_PER_GROUP    24
#define MAX_GROUPS         16

/* well-known element positions within each group (from live FTAB analysis) */
#define ELEM_ENCODER        6   /* encoder: needs init during activation */
#define ELEM_DECODER       21   /* decoder: frame counter ack handshake */
#define ELEM_BUFFER        22   /* buffer: byte store during activation */

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
 * ring buffer — ARM→CSS messages in shared memory
 *
 * header: 4 x uint32 at the allocated area:
 *   [0] base_vaddr, [1] end_vaddr, [2] write_vaddr, [3] read_vaddr
 * data area follows immediately.
 *
 * message format: [instance_id, (type << 13) | num_params, params...]
 */

struct ringbuf {
	void     *shm;
	uint32_t  hdr_off;
	uint32_t  data_off;
	uint32_t  data_end;
	uint32_t  write_off;
};

static void ringbuf_init(struct ringbuf *rb, void *shm,
                         uint32_t hdr_off, uint32_t data_off, int num_entries)
{
	rb->shm = shm;
	rb->hdr_off = hdr_off;
	rb->data_off = data_off;
	rb->data_end = data_off + (uint32_t)num_entries * 4;
	rb->write_off = data_off;

	uintptr_t base_vaddr = (uintptr_t)shm + data_off;
	uintptr_t end_vaddr  = (uintptr_t)shm + rb->data_end;

	shm_write32(shm, hdr_off + 0x00, (uint32_t)base_vaddr);
	shm_write32(shm, hdr_off + 0x04, (uint32_t)end_vaddr);
	shm_write32(shm, hdr_off + 0x08, (uint32_t)base_vaddr);
	shm_write32(shm, hdr_off + 0x0c, (uint32_t)base_vaddr);
}

static void ringbuf_write_word(struct ringbuf *rb, uint32_t val)
{
	if (rb->write_off >= rb->data_end)
		rb->write_off = rb->data_off;
	shm_write32(rb->shm, rb->write_off, val);
	rb->write_off += 4;
}

static void ringbuf_commit(struct ringbuf *rb)
{
	uintptr_t wp_vaddr = (uintptr_t)rb->shm + rb->write_off;
	shm_write32(rb->shm, rb->hdr_off + 0x08, (uint32_t)wp_vaddr);
}

/* [0, (1<<13)|1, level] */
static void ringbuf_send_level_ready(struct ringbuf *rb, int level)
{
	ringbuf_write_word(rb, 0);
	ringbuf_write_word(rb, (1 << 13) | 1);
	ringbuf_write_word(rb, (uint32_t)level);
	ringbuf_commit(rb);
}

/* [0, (4<<13)|0] */
static void ringbuf_send_tick(struct ringbuf *rb)
{
	ringbuf_write_word(rb, 0);
	ringbuf_write_word(rb, (4 << 13) | 0);
	ringbuf_commit(rb);
}

/* decoder ack: [elem+8_vaddr, (1<<13)|0] — type=1, 0 params
 * stock app_dsp sends this when element+8 bit 0 is set by the CSS.
 * the instance_id is the virtual address of element+8, which the CSS
 * translates through the shared memory MMU mapping. */
static void ringbuf_send_decoder_ack(struct ringbuf *rb,
                                     volatile uint32_t *elem8_ptr)
{
	ringbuf_write_word(rb, (uint32_t)(uintptr_t)elem8_ptr);
	ringbuf_write_word(rb, (1 << 13) | 0);
	ringbuf_commit(rb);
}

/*
 * element group — one per SPVOIPNDA unit
 */
struct elem_group {
	volatile uint32_t *elems[ELEMS_PER_GROUP];
	int num_elems;
};

struct bgsc_ctx {
	void *shm;
	uintptr_t shm_mmap_base;

	struct ringbuf rb;

	struct elem_group groups[MAX_GROUPS];
	int num_groups;

	uint32_t frame_tick;
	pthread_t threads[3];
	volatile int running;
};

/*
 * encoder element initialization — replicates FUN_00086f50 in stock app_dsp.
 *
 * when the CSS activates an encoder element (bits 1-4 set with bit 4 for
 * POST_CALC_2), the stock dispatch calls the encoder's pending calc which
 * initializes ~30 fields in the encoder's element descriptor in shared memory.
 * the CSS reads these fields to confirm the encoder is ready.
 *
 * the values below are constants extracted from FUN_00086f50's decompilation.
 * param cw: pointer to the encoder element's control word in shared memory.
 */
static void encoder_init(volatile uint32_t *cw)
{
	volatile uint16_t *h = (volatile uint16_t *)cw;
	/* FUN_00086f50 field writes (offsets from cw, as uint16): */
	h[2]  = 1;       /* +4:  active flag */
	h[3]  = 1;       /* +6:  active flag */
	h[4]  = 0;       /* +8:  clear */
	h[5]  = 0x10;    /* +0xa: config */
	h[7]  = 0;       /* +0xe */
	h[8]  = 0;       /* +0x10 */
	h[9]  = 0;       /* +0x12 */
	h[10] = 0;       /* +0x14 */

	uint16_t s = h[2]; /* = 1 */
	h[24] = 6000;    /* +0x30: sample rate related */
	h[25] = 0x0b;    /* +0x32 */
	h[26] = 0x19;    /* +0x34 */
	h[27] = 10;      /* +0x36 */
	h[28] = 0x21;    /* +0x38 */
	h[31] = 0xf0;    /* +0x3e */
	h[15] = 0;       /* +0x1e */
	h[16] = 0;       /* +0x20 */
	h[17] = 0;       /* +0x22 */
	h[18] = 0;       /* +0x24 */
	h[19] = 0;       /* +0x26 */
	h[20] = 0;       /* +0x28 */
	h[21] = 0;       /* +0x2a */
	h[22] = 5000;    /* +0x2c */
	h[23] = 5000;    /* +0x2e */
	h[29] = 0;       /* +0x3a */
	h[30] = 0;       /* +0x3c */

	/* uint32 writes */
	volatile uint32_t *w = (volatile uint32_t *)cw;
	w[16] = 0;       /* +0x40 */
	w[17] = 0;       /* +0x44 */
	w[18] = 0;       /* +0x48 */
	w[40] = 0;       /* +0xa0 */
	w[41] = 0;       /* +0xa4 */
	w[42] = 0;       /* +0xa8 */

	h[11] = 0x7fa1;  /* +0x16: threshold */
	h[12] = s << 4;  /* +0x18 */
	h[13] = s << 7;  /* +0x1a */
	h[14] = s * 0x1e;/* +0x1c */

	/* clear 0x54 bytes at +0x4c */
	memset((void *)(cw + 0x4c/4), 0, 0x54);

	h[54] = 0;       /* +0x6c */
	h[61] = h[13];   /* +0x7a = value from +0x1a */
	h[62] = h[14];   /* +0x7c = value from +0x1c */

	__sync_synchronize();
}

/*
 * FTAB dispatch — process control words for all element groups
 *
 * the stock dispatch (dfl_process_flow_asm at 0x885b0) iterates the FTAB and
 * for each entry:
 *   - ACTIVE (bit 0):      tail-call calc_func+0x0c (noop for L16/dsp_sko)
 *   - INACTIVE (bits 0-4 = 0): tail-call calc_func+0x08 (noop accumulator)
 *   - PENDING (bits 1-4):  call calc_func+0x08, post-calc, set cw
 *
 * for L16, all calc functions are effectively no-ops. the only real work is:
 *   1. clearing pending control words per protocol: *cw = (val & 6) ? 1 : 0
 *   2. decoder ack: when elem[21]+8 has bit 0 set, send type=1 ring buffer
 *      message and clear element+8 to 0
 */
static void ftab_dispatch(struct bgsc_ctx *ctx)
{
	ctx->frame_tick++;

	/* periodic state dump — every ~30 seconds (6000 * 5ms) */
	if ((ctx->frame_tick % 6000) == 0) {
		fprintf(stderr, "bgsc: === state dump tick %u ===\n",
		        ctx->frame_tick);

		/* ring buffer health: is the CSS reading our messages? */
		uint32_t rb_write = shm_read32(ctx->shm,
			ctx->rb.hdr_off + 0x08);
		uint32_t rb_read = shm_read32(ctx->shm,
			ctx->rb.hdr_off + 0x0c);
		fprintf(stderr, "  ringbuf: write=0x%x read=0x%x %s\n",
		        rb_write, rb_read,
		        (rb_write == rb_read) ? "(caught up)" : "(LAGGING)");

		/* dump key elements from group 0 */
		if (ctx->num_groups > 0) {
			struct elem_group *g0 = &ctx->groups[0];
			fprintf(stderr, "  group0 has %d elems\n",
			        g0->num_elems);
			for (int ei = 0; ei < g0->num_elems; ei++) {
				volatile uint32_t *e = g0->elems[ei];
				if (!e) continue;
				uint32_t cw = e[0];
				/* only dump active or interesting elements */
				if (cw == 0 && ei != 0 && ei != 6 &&
				    ei != 21 && ei != 22)
					continue;
				uint32_t off = (uint32_t)(
					(uintptr_t)e - (uintptr_t)ctx->shm);
				fprintf(stderr, "  e[%d] shm+0x%x:"
				        " cw=%u +4=0x%x +8=0x%x"
				        " +c=0x%x +10=0x%x\n",
				        ei, off, cw,
				        e[1], e[2], e[3], e[4]);
			}
		}
		fprintf(stderr, "bgsc: === end dump ===\n");
	}

	for (int g = 0; g < ctx->num_groups; g++) {
		struct elem_group *grp = &ctx->groups[g];

		/* process all control words in this group */
		for (int i = 0; i < grp->num_elems; i++) {
			volatile uint32_t *cw = grp->elems[i];
			if (!cw)
				continue;

			uint32_t val = *cw;

			/* skip pointer-sized values (data fields, not control words) */
			if (val > 0x3ff && val != 0)
				continue;

			if (val > 1) {
				/* PENDING: bits 1-4 set by CSS. */
				uint32_t new_val = (val & 6) ? 1 : 0;
				uint32_t shm_off = (uint32_t)(
					(uintptr_t)cw - (uintptr_t)ctx->shm);

				fprintf(stderr, "bgsc: [%u] g%d e%d "
				        "shm+0x%x cw=0x%x->0x%x"
				        " +4=0x%x +8=0x%x +c=0x%x\n",
				        ctx->frame_tick, g, i,
				        shm_off, val, new_val,
				        *(cw+1), *(cw+2), *(cw+3));

				/* encoder init: elem[6] per group.
				 * replicates FUN_00086f50 in stock app_dsp. */
				if (i == ELEM_ENCODER) {
					fprintf(stderr, "bgsc:   -> encoder_init\n");
					encoder_init(cw);
				}

				*cw = new_val;
				__sync_synchronize();

				/* send type=0 completion event.
				 *
				 * static analysis says stock dsp_sko doesn't
				 * send this, but empirically the CSS needs SOME
				 * ring buffer signal during activation to
				 * proceed past SETCODEC. the helper trampolines
				 * have complex stack-dependent behavior that may
				 * cause additional messages in stock that we
				 * can't easily replicate. this is the simplest
				 * message that unblocks the CSS. */
				uint32_t inst_addr = (uint32_t)(uintptr_t)(cw + 1);
				ringbuf_write_word(&ctx->rb, inst_addr);
				ringbuf_write_word(&ctx->rb, 0);
				ringbuf_commit(&ctx->rb);
				fprintf(stderr, "bgsc:   -> sent type=0 "
				        "completion [0x%x, 0]\n", inst_addr);

				/* POST_CALC_2 (startup): when bit 4 is set,
				 * check element+8 and send ack if needed.
				 * replicates FUN_00088514/FUN_00088548. */
				if (val & 0x10) {
					volatile uint32_t *elem8 = cw + 2;
					uint32_t val8 = *elem8;
					if (val8 & 1) {
						ringbuf_send_decoder_ack(
							&ctx->rb, elem8);
						*elem8 = 0;
						__sync_synchronize();
						fprintf(stderr, "bgsc:   -> "
						        "sent type=1 ack\n");
					}
				}
			}
			else if (val == 1) {
				/* ACTIVE: log first time per element */
			}
			/* INACTIVE (val == 0): nothing to do for L16 */
		}

		/* decoder handling — elem[21] per group.
		 *
		 * two mechanisms:
		 *
		 * 1. type=3 "decoder ready" — periodic signal (~10ms) that
		 *    kicks the CSS into producing frames. stock codec calc
		 *    functions send this via FUN_0008438c. for L16 (dsp_sko
		 *    noop), we send it manually since no calc function runs.
		 *    format: [elem+4_addr, (3<<13)|1, 0]
		 *
		 * 2. type=1 "frame ack" — sent when CSS sets elem+8 bit 0
		 *    to acknowledge frame receipt. replicates FUN_0008857c.
		 *    format: [elem+8_addr, (1<<13)|0]
		 */
		if (grp->num_elems > ELEM_DECODER) {
			volatile uint32_t *dec = grp->elems[ELEM_DECODER];
			if (dec && (*dec == 1)) {
				/* type=3: decoder ready, every ~10ms */
				if ((ctx->frame_tick & 1) == 0) {
					uint32_t inst = (uint32_t)
						(uintptr_t)(dec + 1);
					ringbuf_write_word(&ctx->rb, inst);
					ringbuf_write_word(&ctx->rb,
						(3 << 13) | 1);
					ringbuf_write_word(&ctx->rb, 0);
					ringbuf_commit(&ctx->rb);
				}

				/* type=1: frame ack */
				volatile uint32_t *elem8 = dec + 2;
				uint32_t val8 = *elem8;
				if (val8 & 1) {
					if (ctx->frame_tick < 200 ||
					    (ctx->frame_tick & 0xff) == 0)
						fprintf(stderr, "bgsc: [%u] "
						        "g%d dec_ack +8=0x%x\n",
						        ctx->frame_tick,
						        g, val8);
					ringbuf_send_decoder_ack(
						&ctx->rb, elem8);
					*elem8 = 0;
					__sync_synchronize();
				}
			}
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

		/* level 1 sends periodic heartbeat.
		 * stock does this from level 0 (which the CSS runs internally),
		 * so we send from level 1 as the lowest ARM-side level. */
		if (level == 1) {
			tick_count++;
			if (tick_count >= 16) { /* every ~80ms */
				ringbuf_send_tick(&ctx->rb);
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

	/* write version tag (stock app_dsp writes shm+0x14 = version) */
	shm_write32(shm_ptr, SHM_VERSION_TAG, ver);

	/* allocate ring buffer from shared memory pool.
	 * layout: [header: 0x10 bytes] [data: 487 x 4 = 0x79c bytes]
	 * header uses userspace virtual addresses for CSS MMU translation. */
	uint32_t hdr_off = alloc_wm;
	uint32_t data_off = alloc_wm + RINGBUF_HDR_SIZE;
	uint32_t data_end_off = data_off + RINGBUF_ENTRIES * 4;

	shm_write32(shm_ptr, SHM_SAVED_ALLOC, hdr_off);
	shm_write32(shm_ptr, SHM_CODEC_STATE, data_off);
	memset((char *)shm_ptr + data_off, 0, RINGBUF_ENTRIES * 4);

	ringbuf_init(&ctx->rb, shm_ptr, hdr_off, data_off, RINGBUF_ENTRIES);
	shm_write32(shm_ptr, SHM_ALLOC_WM, data_end_off);

	fprintf(stderr, "bgsc: ring buffer at shm+0x%x\n", hdr_off);

	/* read element descriptor table at shm+0xb854.
	 *
	 * the CSS creates element descriptors during DUA init and stores
	 * pointers to them here. each pointer is a userspace virtual address.
	 * we organize them into groups of ELEMS_PER_GROUP for dispatch. */
	ctx->num_groups = 0;
	int elem_idx = 0;
	struct elem_group *grp = NULL;

	for (int i = 0; i < ELEM_TABLE_COUNT; i++) {
		uint32_t ptr = shm_read32(shm_ptr, ELEM_TABLE_OFF + i * 4);
		if (ptr == 0)
			break;

		uintptr_t elem_vaddr = (uintptr_t)ptr;
		uintptr_t shm_start = ctx->shm_mmap_base;
		if (elem_vaddr < shm_start || elem_vaddr >= shm_start + 0x100000) {
			fprintf(stderr, "bgsc: elem[%d] ptr=0x%08x OUT OF RANGE\n",
			        i, ptr);
			continue;
		}

		/* start a new group every ELEMS_PER_GROUP entries */
		if (elem_idx % ELEMS_PER_GROUP == 0) {
			if (ctx->num_groups >= MAX_GROUPS)
				break;
			grp = &ctx->groups[ctx->num_groups++];
			grp->num_elems = 0;
		}

		uint32_t elem_off = (uint32_t)(elem_vaddr - shm_start);
		grp->elems[grp->num_elems] =
			(volatile uint32_t *)((char *)shm_ptr + elem_off);
		grp->num_elems++;
		elem_idx++;
	}

	fprintf(stderr, "bgsc: %d elements in %d groups",
	        elem_idx, ctx->num_groups);
	if (ctx->num_groups > 0)
		fprintf(stderr, " (%d elems/group)\n",
		        ctx->groups[0].num_elems);
	else
		fprintf(stderr, "\n");

	/* log first group's decoder element for debugging */
	if (ctx->num_groups > 0 && ctx->groups[0].num_elems > ELEM_DECODER) {
		volatile uint32_t *dec = ctx->groups[0].elems[ELEM_DECODER];
		if (dec) {
			uint32_t off = (uint32_t)((uintptr_t)dec - (uintptr_t)shm_ptr);
			fprintf(stderr, "bgsc: group[0] decoder at shm+0x%x"
			        " (cw=0x%x, +8=0x%x)\n",
			        off, dec[0], dec[2]);
		}
	}

	return ctx;
}

int bgsc_start(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return -1;

	ctx->running = 1;

	/* send "level ready" messages for all 4 levels (0-3).
	 * stock app_dsp sends these after the BG level state machine
	 * completes its 2→3→4→5→0x200→1 transition. we send them
	 * immediately since we start directly in IDLE state. */
	for (int i = 0; i < 4; i++) {
		ringbuf_send_level_ready(&ctx->rb, i);
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
			/* retry without SCHED_FIFO if not running as root */
			pthread_attr_init(&attr);
			ret = pthread_create(&ctx->threads[i], &attr, bg_thread, ta);
			if (ret != 0) {
				free(ta);
				fprintf(stderr, "bgsc: failed to create level %d thread\n",
				        i + 1);
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
