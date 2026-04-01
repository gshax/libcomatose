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
#include <comatose/bgsc_defs.h>

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
 * ARM module registration tables.
 *
 * the CSS stores these addresses as opaque tokens in element descriptors
 * and passes them back during dispatch. stock app_dsp uses real function
 * pointers into its BSS (0x000cxxxx) and codec tables (0x0011aaxx).
 *
 * for L16, the dispatch never dereferences these. we use static tables
 * so the addresses are valid and stable, and distinctive for debugging.
 *
 * stock address categories:
 *   0x000c3ff8  → BG level processing struct (one global)
 *   0x000c4xxx  → per-group processing config (varies per group)
 *   0x0011aaxx  → global codec function tables (WAD/WAE/G1E/G1D etc.)
 *   0x0012xxxx  → module-specific data
 */
static uint32_t arm_bg_level_stub = 0xC0DE0100;

/* per-group config stubs — stock has different 0x000c4xxx addrs per group.
 * we provide one stub per group since the CSS might use them as indices. */
static uint32_t arm_group_config[MAX_GROUPS][4]; /* zeroed, used as opaque tokens */

/* codec function table stubs — stock has entries at 0x0011aaxx.
 * the CSS stores and passes these back during dispatch. for L16 noop. */
static uint32_t arm_codec_table[8]; /* one slot per unique function entry */

/* module-specific data stub — stock has 0x0012xxxx entries in elem[1] */
static uint32_t arm_module_data[4];

/*
 * module_startup — populate element descriptors with codec registration data.
 *
 * replaces stock app_dsp's dfl_module_startup (FUN_00075810, 36KB, 21 callees).
 * writes the common header fields and codec-specific configuration that the CSS
 * reads via its (instance, offset) parameter system to configure audio routing.
 *
 * stock writes ~2137 lines worth of data across all element descriptors. we
 * start with the minimum viable set (common header + codec config) and add
 * more fields iteratively based on what the CSS requires.
 */
/*
 * write a uint32 to an element descriptor at a byte offset,
 * but only if the current value is zero (don't overwrite CSS data).
 */
static inline void elem_set_if_zero(volatile uint32_t *elem,
                                    unsigned byte_off, uint32_t val)
{
	volatile uint32_t *p = (volatile uint32_t *)((char *)elem + byte_off);
	if (*p == 0)
		*p = val;
}

static void module_startup(struct bgsc_ctx *ctx)
{
	uint32_t bg_addr = (uint32_t)(uintptr_t)&arm_bg_level_stub;
	uint32_t ct_base = (uint32_t)(uintptr_t)&arm_codec_table[0];
	uint32_t md_base = (uint32_t)(uintptr_t)&arm_module_data[0];

	for (int g = 0; g < ctx->num_groups; g++) {
		struct elem_group *grp = &ctx->groups[g];
		uint32_t gc_base = (uint32_t)(uintptr_t)&arm_group_config[g][0];

		/* elem[0]: codec config — mark codecs as registered */
		if (grp->num_elems > DFL_ELEM_CODEC && grp->elems[DFL_ELEM_CODEC]) {
			struct dfl_elem_codec *codec =
				(struct dfl_elem_codec *)grp->elems[DFL_ELEM_CODEC];
			codec->hdr.config = DFL_CODEC_REGISTERED;
		}

		/* elem[1]: routing helper — has a unique layout (no common hdr).
		 * stock has ARM addrs at +0x14, +0x1c, +0x40. */
		if (grp->num_elems > 1 && grp->elems[1]) {
			volatile uint32_t *e1 = grp->elems[1];
			elem_set_if_zero(e1, 0x14, md_base);      /* module data */
			elem_set_if_zero(e1, 0x1c, bg_addr);       /* BG level struct */
			elem_set_if_zero(e1, 0x40, md_base + 4);   /* module data */
		}

		/* elements 2-21 (except buffer and next-codec): common header */
		for (int i = 2; i < grp->num_elems; i++) {
			if (i == DFL_ELEM_BUFFER || i == DFL_ELEM_NEXT_CODEC)
				continue;
			volatile uint32_t *elem = grp->elems[i];
			if (!elem) continue;

			struct dfl_elem_hdr *hdr = (struct dfl_elem_hdr *)elem;

			if (hdr->group_type == 0)
				hdr->group_type = (i < 10)
					? DFL_GROUP_TYPE_LEVEL0
					: DFL_GROUP_TYPE_LEVEL1;
			if (hdr->buf_config == 0)
				hdr->buf_config = 0x100;
			if (hdr->arm_module == 0)
				hdr->arm_module = bg_addr;
		}

		/* elem[6] (encoder): frame config + codec function table.
		 * stock has 15 ARM addrs at +0x074 through +0x0e4. these are
		 * per-group config addrs (0x000c4xxx) and codec table addrs
		 * (0x0011aaxx). for L16 they're opaque tokens, never called. */
		if (grp->num_elems > DFL_ELEM_ENCODER && grp->elems[DFL_ELEM_ENCODER]) {
			volatile uint32_t *enc = grp->elems[DFL_ELEM_ENCODER];
			elem_set_if_zero(enc, 0x28, DFL_FRAME_CONFIG_DEFAULT);
			/* codec function table slots (stock pattern from shm dump) */
			elem_set_if_zero(enc, 0x074, gc_base);     /* per-group */
			elem_set_if_zero(enc, 0x078, ct_base);     /* codec table */
			elem_set_if_zero(enc, 0x07c, ct_base + 4); /* codec table */
			elem_set_if_zero(enc, 0x080, ct_base + 8); /* codec table */
			elem_set_if_zero(enc, 0x084, ct_base);     /* codec table */
			elem_set_if_zero(enc, 0x088, ct_base + 12);/* codec table */
			elem_set_if_zero(enc, 0x08c, gc_base);     /* per-group */
			elem_set_if_zero(enc, 0x0b4, gc_base + 4); /* per-group */
			elem_set_if_zero(enc, 0x0bc, ct_base);     /* codec table */
			elem_set_if_zero(enc, 0x0c0, gc_base);     /* per-group */
			elem_set_if_zero(enc, 0x0c4, gc_base + 4); /* per-group */
			elem_set_if_zero(enc, 0x0cc, ct_base);     /* codec table */
			elem_set_if_zero(enc, 0x0d0, gc_base);     /* per-group */
			elem_set_if_zero(enc, 0x0d4, gc_base + 4); /* per-group */
			elem_set_if_zero(enc, 0x0e0, ct_base);     /* codec table */
			elem_set_if_zero(enc, 0x0e4, gc_base);     /* per-group */
		}

		/* elem[2] (codec module): similar function table at +0xa4-0xfc */
		if (grp->num_elems > 2 && grp->elems[2]) {
			volatile uint32_t *e2 = grp->elems[2];
			elem_set_if_zero(e2, 0x0a4, gc_base);
			elem_set_if_zero(e2, 0x0a8, ct_base);
			elem_set_if_zero(e2, 0x0ac, ct_base + 4);
			elem_set_if_zero(e2, 0x0b0, ct_base + 8);
			elem_set_if_zero(e2, 0x0b4, ct_base);
			elem_set_if_zero(e2, 0x0b8, ct_base + 12);
			elem_set_if_zero(e2, 0x0bc, gc_base);
			elem_set_if_zero(e2, 0x0e4, gc_base + 4);
			elem_set_if_zero(e2, 0x0ec, ct_base);
			elem_set_if_zero(e2, 0x0f0, gc_base);
			elem_set_if_zero(e2, 0x0f4, gc_base + 4);
			elem_set_if_zero(e2, 0x0fc, ct_base);
		}

		/* elem[21] (decoder): frame config */
		if (grp->num_elems > DFL_ELEM_DECODER && grp->elems[DFL_ELEM_DECODER]) {
			volatile uint32_t *dec = grp->elems[DFL_ELEM_DECODER];
			elem_set_if_zero(dec, 0x28, DFL_FRAME_CONFIG_DEFAULT);
		}
	}

	__sync_synchronize();
	fprintf(stderr, "bgsc: module_startup done (%d groups)\n", ctx->num_groups);
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

				/* buffer element (elem[22]): dual-buffer setup.
				 *
				 * in stock, the CSS differentiates +4 and +8 during
				 * SETCODEC activation (0x12): +4 shifts by 0x140
				 * from +8 to create separate input/output buffers.
				 * this only happens for the 0x12 activation (where
				 * the element becomes ACTIVE), not the initial 0x10.
				 *
				 * since the CSS isn't doing this for us, manually
				 * apply the 0x140 offset to +4. */
				if (i == DFL_ELEM_BUFFER && new_val == 1) {
					/* only during 0x12 (becomes active) */
					struct dfl_elem_buffer *buf =
						(struct dfl_elem_buffer *)cw;
					uint32_t base = buf->buf_ptr_b; /* +8: base ptr */
					if (base > 0xb0000000) {
						buf->buf_ptr_a = base + 0x140;
						buf->buf_ptr_c = base + 0x144;
						__sync_synchronize();
						fprintf(stderr,
						        "bgsc:   -> buf dual: "
						        "+4=0x%x +8=0x%x "
						        "(delta=0x%x)\n",
						        buf->buf_ptr_a,
						        buf->buf_ptr_b,
						        buf->buf_ptr_a - base);
					}
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

	/* detect mmap base used when element table was created.
	 * if we mmapped at a different address (e.g. --bgsc-only after
	 * killing stock app_dsp), rebase the pointers. */
	uint32_t first_ptr = shm_read32(shm_ptr, ELEM_TABLE_OFF);
	intptr_t rebase = 0;
	if (first_ptr != 0) {
		uintptr_t orig_base = (uintptr_t)first_ptr - 0x30;
		rebase = (intptr_t)ctx->shm_mmap_base - (intptr_t)orig_base;
		if (rebase != 0)
			fprintf(stderr, "bgsc: rebasing elem ptrs by %+d\n",
			        (int)rebase);
	}

	for (int i = 0; i < ELEM_TABLE_COUNT; i++) {
		uint32_t ptr = shm_read32(shm_ptr, ELEM_TABLE_OFF + i * 4);
		if (ptr == 0)
			break;

		uintptr_t elem_vaddr = (uintptr_t)ptr + rebase;
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

	/* populate element descriptors with codec registration data */
	module_startup(ctx);

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
