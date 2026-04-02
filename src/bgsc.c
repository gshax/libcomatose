/*
 * bgsc.c - BGSC (background scheduler) implementation
 *
 * replaces stock app_dsp's ARM-side DSP framework executor. the CSS firmware
 * has 4 "BG levels" of processing: level 0 runs on the CSS processor, levels
 * 1-3 are dispatched to the ARM. both sides use the same dfl_process_flow
 * dispatch mechanism operating on shared memory element descriptors.
 *
 * design principle: OPT-IN element handling. the ARM dispatch ONLY modifies
 * control words on elements it explicitly understands. CSS-level elements
 * (signal routing, codec hardware) are observed but NEVER touched — the CSS
 * level 0 dispatch handles those independently.
 *
 * for L16 (linear 16-bit PCM), the ARM's role is minimal:
 *   - acknowledge PENDING activations on ARM-level elements
 *   - send decoder-ready events to drive CSS frame production
 *   - send decoder frame acks for the per-frame handshake
 *   - send infrastructure messages (level-ready, heartbeat)
 *
 * the CSS level 0 dispatch handles ALL audio data movement (TDM reads,
 * signal routing, encoder writes). this was confirmed on hardware: TDM ISR
 * counters are zero even with stock app_dsp producing real audio.
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
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

/* ── shared memory pool header offsets ─────────────────────────────── */
#define SHM_POOL_SIZE      0x04
#define SHM_ALLOC_WM       0x08
#define SHM_DSP_VERSION    0x10
#define SHM_VERSION_TAG    0x14
#define SHM_SAVED_ALLOC    0x28
#define SHM_CODEC_STATE    0x2c

/* ── allocation sizes ──────────────────────────────────────────────── */
#define RINGBUF_HDR_SIZE   0x10
#define CODEC_STATE_SIZE   0x79c
#define RINGBUF_ENTRIES    487   /* 0x1e7 */

/* ── element descriptor table in shared memory ─────────────────────── */
#define ELEM_TABLE_OFF     0xb854
#define ELEM_TABLE_COUNT   368

/* ── group layout ──────────────────────────────────────────────────── */
#define ELEMS_PER_GROUP    24
#define MAX_GROUPS         16

/* ── dispatch timing ───────────────────────────────────────────────── */
#define DISPATCH_PERIOD_NS    5000000   /* 5ms per dispatch cycle */
#define HEARTBEAT_INTERVAL    16        /* ticks between heartbeats (~80ms) */
#define DECODER_READY_INTERVAL 2        /* ticks between decoder-ready (~10ms) */
#define STATE_DUMP_INTERVAL   6000      /* ticks between state dumps (~30s) */

/* ── shared memory access helpers ──────────────────────────────────── */

static inline uint32_t shm_read32(void *shm, uint32_t offset)
{
	return *(volatile uint32_t *)((char *)shm + offset);
}

static inline void shm_write32(void *shm, uint32_t offset, uint32_t val)
{
	*(volatile uint32_t *)((char *)shm + offset) = val;
	__sync_synchronize();
}

/* ── ring buffer (ARM→CSS messages in shared memory) ───────────────── */

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

/* send a ring buffer message with the standard header encoding */
static void ringbuf_send(struct ringbuf *rb, uint32_t instance,
                         int type, int nparams, const uint32_t *params)
{
	ringbuf_write_word(rb, instance);
	ringbuf_write_word(rb, DFL_MSG_HDR(type, nparams));
	for (int i = 0; i < nparams; i++)
		ringbuf_write_word(rb, params[i]);
	ringbuf_commit(rb);
}

static void ringbuf_send_level_ready(struct ringbuf *rb, int level)
{
	uint32_t param = (uint32_t)level;
	ringbuf_send(rb, 0, DFL_MSG_TYPE_LEVEL_READY, 1, &param);
}

static void ringbuf_send_heartbeat(struct ringbuf *rb)
{
	ringbuf_send(rb, 0, DFL_MSG_TYPE_HEARTBEAT, 0, NULL);
}

static void ringbuf_send_completion(struct ringbuf *rb,
                                    volatile uint32_t *elem_ptr)
{
	/* instance = address of elem+4 (CSS resolves via p_dspa_find_instance) */
	uint32_t inst = (uint32_t)(uintptr_t)(elem_ptr + 1);
	ringbuf_send(rb, inst, DFL_MSG_TYPE_COMPLETION, 0, NULL);
}

static void ringbuf_send_decoder_ready(struct ringbuf *rb,
                                       volatile uint32_t *decoder_ptr)
{
	uint32_t inst = (uint32_t)(uintptr_t)(decoder_ptr + 1);
	uint32_t param = 0;
	ringbuf_send(rb, inst, DFL_MSG_TYPE_DECODER_READY, 1, &param);
}

static void ringbuf_send_decoder_ack(struct ringbuf *rb,
                                     volatile uint32_t *elem8_ptr)
{
	uint32_t inst = (uint32_t)(uintptr_t)elem8_ptr;
	ringbuf_send(rb, inst, DFL_MSG_TYPE_DECODER_ACK, 0, NULL);
}

/* ── element info and group structures ─────────────────────────────── */

struct elem_info {
	volatile uint32_t *ptr;     /* pointer into shared memory */
	enum dfl_elem_role role;    /* dispatch role */
	uint32_t last_cw;           /* last observed CW (for change logging) */
	uint32_t shm_offset;       /* offset from shm base (for logging) */
};

struct elem_group {
	struct elem_info elems[ELEMS_PER_GROUP];
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

/* ── ARM module registration stubs ─────────────────────────────────── */

/*
 * the CSS stores ARM addresses as opaque tokens in element descriptors and
 * passes them back during dispatch. for L16, these are never dereferenced
 * by the ARM (all calc functions are noops). we provide stable addresses
 * that are distinctive in memory dumps for debugging.
 */
static uint32_t arm_bg_level_stub = 0xC0DE0100;
static uint32_t arm_group_config[MAX_GROUPS][4];
static uint32_t arm_codec_table[8];
static uint32_t arm_module_data[4];

/* ── element classification ────────────────────────────────────────── */

static const char *role_name(enum dfl_elem_role role)
{
	switch (role) {
	case DFL_ROLE_UNKNOWN:   return "unknown";
	case DFL_ROLE_CSS:       return "css";
	case DFL_ROLE_CODEC_HDR: return "codec_hdr";
	case DFL_ROLE_ENCODER:   return "encoder";
	case DFL_ROLE_DECODER:   return "decoder";
	case DFL_ROLE_BUFFER:    return "buffer";
	case DFL_ROLE_ARM_NOOP:  return "arm_noop";
	default:                 return "?";
	}
}

/*
 * classify each element's dispatch role based on its position in the group
 * and its group_type field. called once during init after module_startup
 * has populated the group_type values.
 */
static void classify_elements(struct bgsc_ctx *ctx)
{
	for (int g = 0; g < ctx->num_groups; g++) {
		struct elem_group *grp = &ctx->groups[g];

		for (int i = 0; i < grp->num_elems; i++) {
			struct elem_info *ei = &grp->elems[i];
			if (!ei->ptr) {
				ei->role = DFL_ROLE_UNKNOWN;
				continue;
			}

			/* classify by well-known position */
			switch (i) {
			case DFL_ELEM_CODEC:
			case DFL_ELEM_NEXT_CODEC:
				ei->role = DFL_ROLE_CODEC_HDR;
				break;
			case DFL_ELEM_ENCODER:
				ei->role = DFL_ROLE_ENCODER;
				break;
			case DFL_ELEM_DECODER:
				ei->role = DFL_ROLE_DECODER;
				break;
			case DFL_ELEM_BUFFER:
				ei->role = DFL_ROLE_BUFFER;
				break;
			default: {
				/* check group_type to distinguish CSS vs ARM */
				struct dfl_elem_hdr *hdr =
					(struct dfl_elem_hdr *)ei->ptr;
				uint32_t gt = hdr->group_type;
				if (gt == DFL_GROUP_TYPE_LEVEL0)
					ei->role = DFL_ROLE_CSS;
				else if (gt == DFL_GROUP_TYPE_LEVEL1)
					ei->role = DFL_ROLE_ARM_NOOP;
				else if (i <= 9)
					/* elements 1-9 without group_type:
					 * treat as CSS (safe default) */
					ei->role = DFL_ROLE_CSS;
				else
					ei->role = DFL_ROLE_ARM_NOOP;
				break;
			}
			}

			ei->last_cw = *ei->ptr;
		}

		/* log classification for first group */
		if (g == 0) {
			fprintf(stderr, "bgsc: group 0 classification:\n");
			for (int i = 0; i < grp->num_elems; i++) {
				struct elem_info *ei = &grp->elems[i];
				if (!ei->ptr) continue;
				fprintf(stderr, "  e[%2d] shm+0x%04x cw=%u %s\n",
				        i, ei->shm_offset, *ei->ptr,
				        role_name(ei->role));
			}
		}
	}
}

/* ── module startup (element descriptor population) ────────────────── */

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
		if (grp->num_elems > DFL_ELEM_CODEC && grp->elems[DFL_ELEM_CODEC].ptr) {
			struct dfl_elem_codec *codec =
				(struct dfl_elem_codec *)grp->elems[DFL_ELEM_CODEC].ptr;
			codec->hdr.config = DFL_CODEC_REGISTERED;
		}

		/* elem[1]: routing helper — unique layout, no common hdr */
		if (grp->num_elems > 1 && grp->elems[1].ptr) {
			volatile uint32_t *e1 = grp->elems[1].ptr;
			elem_set_if_zero(e1, 0x14, md_base);
			elem_set_if_zero(e1, 0x1c, bg_addr);
			elem_set_if_zero(e1, 0x40, md_base + 4);
		}

		/* elements 2-21 (except buffer and next-codec): common header */
		for (int i = 2; i < grp->num_elems; i++) {
			if (i == DFL_ELEM_BUFFER || i == DFL_ELEM_NEXT_CODEC)
				continue;
			volatile uint32_t *elem = grp->elems[i].ptr;
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

		/* elem[6] (encoder): frame config + codec function table */
		if (grp->num_elems > DFL_ELEM_ENCODER && grp->elems[DFL_ELEM_ENCODER].ptr) {
			volatile uint32_t *enc = grp->elems[DFL_ELEM_ENCODER].ptr;
			elem_set_if_zero(enc, 0x28, DFL_FRAME_CONFIG_DEFAULT);
			elem_set_if_zero(enc, 0x074, gc_base);
			elem_set_if_zero(enc, 0x078, ct_base);
			elem_set_if_zero(enc, 0x07c, ct_base + 4);
			elem_set_if_zero(enc, 0x080, ct_base + 8);
			elem_set_if_zero(enc, 0x084, ct_base);
			elem_set_if_zero(enc, 0x088, ct_base + 12);
			elem_set_if_zero(enc, 0x08c, gc_base);
			elem_set_if_zero(enc, 0x0b4, gc_base + 4);
			elem_set_if_zero(enc, 0x0bc, ct_base);
			elem_set_if_zero(enc, 0x0c0, gc_base);
			elem_set_if_zero(enc, 0x0c4, gc_base + 4);
			elem_set_if_zero(enc, 0x0cc, ct_base);
			elem_set_if_zero(enc, 0x0d0, gc_base);
			elem_set_if_zero(enc, 0x0d4, gc_base + 4);
			elem_set_if_zero(enc, 0x0e0, ct_base);
			elem_set_if_zero(enc, 0x0e4, gc_base);
		}

		/* elem[2] (codec module): function table */
		if (grp->num_elems > 2 && grp->elems[2].ptr) {
			volatile uint32_t *e2 = grp->elems[2].ptr;
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
		if (grp->num_elems > DFL_ELEM_DECODER && grp->elems[DFL_ELEM_DECODER].ptr) {
			volatile uint32_t *dec = grp->elems[DFL_ELEM_DECODER].ptr;
			elem_set_if_zero(dec, 0x28, DFL_FRAME_CONFIG_DEFAULT);
		}

		/* elem[12] (buffer manager): dual-buffer offset */
		if (grp->num_elems > 12 && grp->elems[12].ptr) {
			volatile uint32_t *e12 = grp->elems[12].ptr;
			volatile uint32_t *p90 = (volatile uint32_t *)
				((char *)e12 + 0x90);
			uint32_t val = *p90;
			if (val > 0xb0000000) {
				*p90 = val + DFL_BUFFER_DUAL_OFFSET;
				fprintf(stderr, "bgsc: g%d e12+0x90: "
				        "0x%x -> 0x%x (+0x%x)\n",
				        g, val, val + DFL_BUFFER_DUAL_OFFSET,
				        DFL_BUFFER_DUAL_OFFSET);
			}
		}
	}

	__sync_synchronize();
	fprintf(stderr, "bgsc: module_startup done (%d groups)\n",
	        ctx->num_groups);
}

/* ── PENDING control word clearing (stock protocol) ────────────────── */

/*
 * clear a PENDING control word per the stock dfl_process_flow protocol:
 *   if bits 1-2 (REPEAT|PERSIST) are set → element becomes ACTIVE (cw=1)
 *   otherwise → element becomes INACTIVE (cw=0)
 */
static inline uint32_t cw_clear_pending(uint32_t cw)
{
	return (cw & DFL_CW_ACTIVATE_MASK) ? DFL_CW_ACTIVE : 0;
}

/* ── per-role dispatch handlers ────────────────────────────────────── */

/*
 * handle_encoder — elem[6] per group
 *
 * during SETCODEC activation, the CSS sets the encoder CW to a PENDING
 * value (typically DFL_CW_POST_CALC_2 | DFL_CW_PERSIST = 0x14). the ARM
 * clears it and sends a completion event to unblock the CSS.
 *
 * for L16, the stock encoder calc function is a noop (dsp_sko: bx lr).
 * the encoder_init (FUN_00086f50) writes codec config fields during the
 * PENDING path, but for L16 these are noops. we just clear + ack.
 */
static void handle_encoder(struct bgsc_ctx *ctx, int g,
                           struct elem_info *ei)
{
	uint32_t cw = *ei->ptr;

	if (cw & DFL_CW_PENDING_MASK) {
		uint32_t new_cw = cw_clear_pending(cw);

		fprintf(stderr, "bgsc: [%u] g%d encoder cw=0x%x->0x%x\n",
		        ctx->frame_tick, g, cw, new_cw);

		*ei->ptr = new_cw;
		__sync_synchronize();

		ringbuf_send_completion(&ctx->rb, ei->ptr);

		/* POST_CALC_2: check elem+8 for pending ack */
		if (cw & DFL_CW_POST_CALC_2) {
			volatile uint32_t *elem8 = ei->ptr + 2;
			if (*elem8 & DFL_CW_ACTIVE) {
				ringbuf_send_decoder_ack(&ctx->rb, elem8);
				*elem8 = 0;
				__sync_synchronize();
			}
		}
	}
}

/*
 * handle_decoder — elem[21] per group
 *
 * two independent mechanisms:
 *
 * 1. PENDING activation (during SETCODEC): clear CW + send completion,
 *    same as encoder.
 *
 * 2. steady-state (CW == ACTIVE): two periodic tasks:
 *    a. decoder-ready: sent every DECODER_READY_INTERVAL ticks (~10ms)
 *       to kick the CSS into producing encoder output frames.
 *    b. frame ack: when CSS sets elem+8 bit 0, send a type=1 ack and
 *       clear elem+8. this is the per-frame handshake that drives
 *       continuous frame production.
 */
static void handle_decoder(struct bgsc_ctx *ctx, int g,
                           struct elem_info *ei)
{
	uint32_t cw = *ei->ptr;

	if (cw & DFL_CW_PENDING_MASK) {
		uint32_t new_cw = cw_clear_pending(cw);

		fprintf(stderr, "bgsc: [%u] g%d decoder cw=0x%x->0x%x\n",
		        ctx->frame_tick, g, cw, new_cw);

		*ei->ptr = new_cw;
		__sync_synchronize();

		ringbuf_send_completion(&ctx->rb, ei->ptr);

		if (cw & DFL_CW_POST_CALC_2) {
			volatile uint32_t *elem8 = ei->ptr + 2;
			if (*elem8 & DFL_CW_ACTIVE) {
				ringbuf_send_decoder_ack(&ctx->rb, elem8);
				*elem8 = 0;
				__sync_synchronize();
			}
		}
		return;
	}

	if (cw != DFL_CW_ACTIVE)
		return;

	/* decoder-ready: periodic signal to kick CSS frame production */
	if ((ctx->frame_tick % DECODER_READY_INTERVAL) == 0)
		ringbuf_send_decoder_ready(&ctx->rb, ei->ptr);

	/* frame ack: per-frame handshake via elem+8 */
	volatile uint32_t *elem8 = ei->ptr + 2;
	uint32_t val8 = *elem8;
	if (val8 & DFL_CW_ACTIVE) {
		ringbuf_send_decoder_ack(&ctx->rb, elem8);
		*elem8 = 0;
		__sync_synchronize();
	}
}

/*
 * handle_buffer — elem[22] per group
 *
 * during SETCODEC activation, the CSS sends two PENDING values:
 *   1. first activation (0x10): initial setup
 *   2. second activation (0x12): element becomes ACTIVE
 *
 * on the transition to ACTIVE (cw_clear_pending returns 1), we apply
 * DFL_BUFFER_DUAL_OFFSET to differentiate buf_ptr_a from buf_ptr_b.
 * this creates separate input/output audio buffers. without this, both
 * pointers reference the same memory and the encoder reads stale data.
 */
static void handle_buffer(struct bgsc_ctx *ctx, int g,
                          struct elem_info *ei)
{
	uint32_t cw = *ei->ptr;

	if (!(cw & DFL_CW_PENDING_MASK))
		return;

	uint32_t new_cw = cw_clear_pending(cw);

	fprintf(stderr, "bgsc: [%u] g%d buffer cw=0x%x->0x%x\n",
	        ctx->frame_tick, g, cw, new_cw);

	/* apply dual-buffer offset on transition to ACTIVE */
	if (new_cw == DFL_CW_ACTIVE) {
		struct dfl_elem_buffer *buf = (struct dfl_elem_buffer *)ei->ptr;
		uint32_t base = buf->buf_ptr_b;
		if (base > 0xb0000000) {
			buf->buf_ptr_a = base + DFL_BUFFER_DUAL_OFFSET;
			buf->buf_ptr_c = base + DFL_BUFFER_DUAL_OFFSET + 4;
			__sync_synchronize();
			fprintf(stderr, "bgsc:   buf dual: +4=0x%x +8=0x%x "
			        "(delta=0x%x)\n",
			        buf->buf_ptr_a, buf->buf_ptr_b,
			        DFL_BUFFER_DUAL_OFFSET);
		}
	}

	*ei->ptr = new_cw;
	__sync_synchronize();

	ringbuf_send_completion(&ctx->rb, ei->ptr);
}

/*
 * handle_arm_noop — ARM level 1 elements (e[10]-e[20])
 *
 * these are confirmed noops for L16. the stock calc functions either
 * return immediately or do trivial byte writes. we just need to clear
 * PENDING bits and send completion events per the protocol.
 *
 * when other codecs are implemented (e.g., G.711), some of these elements
 * will need real handlers. for now, clearing + ack is sufficient.
 */
static void handle_arm_noop(struct bgsc_ctx *ctx, int g,
                            struct elem_info *ei)
{
	uint32_t cw = *ei->ptr;

	if (!(cw & DFL_CW_PENDING_MASK))
		return;

	uint32_t new_cw = cw_clear_pending(cw);

	fprintf(stderr, "bgsc: [%u] g%d e[?] arm_noop shm+0x%04x "
	        "cw=0x%x->0x%x\n",
	        ctx->frame_tick, g, ei->shm_offset, cw, new_cw);

	*ei->ptr = new_cw;
	__sync_synchronize();

	ringbuf_send_completion(&ctx->rb, ei->ptr);

	if (cw & DFL_CW_POST_CALC_2) {
		volatile uint32_t *elem8 = ei->ptr + 2;
		if (*elem8 & DFL_CW_ACTIVE) {
			ringbuf_send_decoder_ack(&ctx->rb, elem8);
			*elem8 = 0;
			__sync_synchronize();
		}
	}
}

/* ── main dispatch loop ────────────────────────────────────────────── */

static void ftab_dispatch(struct bgsc_ctx *ctx)
{
	ctx->frame_tick++;

	/* periodic state dump */
	if ((ctx->frame_tick % STATE_DUMP_INTERVAL) == 0) {
		fprintf(stderr, "bgsc: === state dump tick %u ===\n",
		        ctx->frame_tick);

		uint32_t rb_write = shm_read32(ctx->shm,
			ctx->rb.hdr_off + 0x08);
		uint32_t rb_read = shm_read32(ctx->shm,
			ctx->rb.hdr_off + 0x0c);
		fprintf(stderr, "  ringbuf: write=0x%x read=0x%x %s\n",
		        rb_write, rb_read,
		        (rb_write == rb_read) ? "(caught up)" : "(LAGGING)");

		if (ctx->num_groups > 0) {
			struct elem_group *g0 = &ctx->groups[0];
			for (int i = 0; i < g0->num_elems; i++) {
				struct elem_info *ei = &g0->elems[i];
				if (!ei->ptr) continue;
				uint32_t cw = *ei->ptr;
				if (cw == 0 && ei->role != DFL_ROLE_ENCODER
				    && ei->role != DFL_ROLE_DECODER
				    && ei->role != DFL_ROLE_BUFFER)
					continue;
				fprintf(stderr, "  e[%2d] shm+0x%04x cw=%-3u "
				        "+4=0x%x +8=0x%x %s\n",
				        i, ei->shm_offset, cw,
				        ei->ptr[1], ei->ptr[2],
				        role_name(ei->role));
			}
		}
		fprintf(stderr, "bgsc: === end dump ===\n");
	}

	/* dispatch each element based on its role */
	for (int g = 0; g < ctx->num_groups; g++) {
		struct elem_group *grp = &ctx->groups[g];

		for (int i = 0; i < grp->num_elems; i++) {
			struct elem_info *ei = &grp->elems[i];
			if (!ei->ptr)
				continue;

			switch (ei->role) {
			case DFL_ROLE_CSS:
			case DFL_ROLE_CODEC_HDR:
			case DFL_ROLE_UNKNOWN: {
				/* observe only — NEVER modify.
				 * log changes for diagnostics. */
				uint32_t cw = *ei->ptr;
				if (cw != ei->last_cw) {
					/* rate-limit: log first occurrence
					 * and every 1000th change */
					if (ei->last_cw == 0 ||
					    (ctx->frame_tick % 1000) == 0)
						fprintf(stderr,
						        "bgsc: [%u] g%d e[%d] "
						        "%s shm+0x%04x "
						        "cw 0x%x->0x%x "
						        "(observed)\n",
						        ctx->frame_tick, g, i,
						        role_name(ei->role),
						        ei->shm_offset,
						        ei->last_cw, cw);
					ei->last_cw = cw;
				}
				break;
			}

			case DFL_ROLE_ENCODER:
				handle_encoder(ctx, g, ei);
				break;

			case DFL_ROLE_DECODER:
				handle_decoder(ctx, g, ei);
				break;

			case DFL_ROLE_BUFFER:
				handle_buffer(ctx, g, ei);
				break;

			case DFL_ROLE_ARM_NOOP:
				handle_arm_noop(ctx, g, ei);
				break;
			}
		}
	}
}

/* ── BG thread ─────────────────────────────────────────────────────── */

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

	struct timespec period = {
		.tv_sec = 0,
		.tv_nsec = DISPATCH_PERIOD_NS
	};
	int tick_count = 0;

	fprintf(stderr, "bgsc: level %d thread started\n", level);

	while (ctx->running) {
		ftab_dispatch(ctx);

		/* level 1 sends periodic heartbeat */
		if (level == 1) {
			tick_count++;
			if (tick_count >= HEARTBEAT_INTERVAL) {
				ringbuf_send_heartbeat(&ctx->rb);
				tick_count = 0;
			}
		}

		nanosleep(&period, NULL);
	}

	fprintf(stderr, "bgsc: level %d thread stopped\n", level);
	return NULL;
}

/* ── public API ────────────────────────────────────────────────────── */

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

	/* write version tag */
	shm_write32(shm_ptr, SHM_VERSION_TAG, ver);

	/* allocate ring buffer from shared memory pool */
	uint32_t hdr_off = alloc_wm;
	uint32_t data_off = alloc_wm + RINGBUF_HDR_SIZE;
	uint32_t data_end_off = data_off + RINGBUF_ENTRIES * 4;

	shm_write32(shm_ptr, SHM_SAVED_ALLOC, hdr_off);
	shm_write32(shm_ptr, SHM_CODEC_STATE, data_off);
	memset((char *)shm_ptr + data_off, 0, RINGBUF_ENTRIES * 4);

	ringbuf_init(&ctx->rb, shm_ptr, hdr_off, data_off, RINGBUF_ENTRIES);
	shm_write32(shm_ptr, SHM_ALLOC_WM, data_end_off);

	fprintf(stderr, "bgsc: ring buffer at shm+0x%x\n", hdr_off);

	/* read element descriptor table at shm+0xb854 */
	ctx->num_groups = 0;
	int elem_idx = 0;
	struct elem_group *grp = NULL;

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
		if (elem_vaddr < shm_start ||
		    elem_vaddr >= shm_start + 0x100000) {
			fprintf(stderr, "bgsc: elem[%d] ptr=0x%08x OUT OF RANGE\n",
			        i, ptr);
			continue;
		}

		if (elem_idx % ELEMS_PER_GROUP == 0) {
			if (ctx->num_groups >= MAX_GROUPS)
				break;
			grp = &ctx->groups[ctx->num_groups++];
			grp->num_elems = 0;
		}

		uint32_t elem_off = (uint32_t)(elem_vaddr - shm_start);
		struct elem_info *ei = &grp->elems[grp->num_elems];
		ei->ptr = (volatile uint32_t *)((char *)shm_ptr + elem_off);
		ei->shm_offset = elem_off;
		ei->role = DFL_ROLE_UNKNOWN; /* classified after module_startup */
		ei->last_cw = 0;
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

	/* classify elements by role (after module_startup sets group_type) */
	classify_elements(ctx);

	return ctx;
}

int bgsc_start(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return -1;

	ctx->running = 1;

	/* send level-ready for all 4 BG levels (0-3) */
	for (int i = 0; i < 4; i++) {
		ringbuf_send_level_ready(&ctx->rb, i);
		fprintf(stderr, "bgsc: sent level %d ready\n", i);
	}

	/* spawn threads for ARM BG levels 1-3 */
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

		int ret = pthread_create(&ctx->threads[i], &attr,
		                         bg_thread, ta);
		if (ret != 0) {
			pthread_attr_init(&attr);
			ret = pthread_create(&ctx->threads[i], &attr,
			                     bg_thread, ta);
			if (ret != 0) {
				free(ta);
				fprintf(stderr, "bgsc: failed to create "
				        "level %d thread\n", i + 1);
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

void bgsc_notify_ready(bgsc_ctx_t *ctx)
{
	if (!ctx)
		return;

	/* respond to CSS 0xf2 events by sending level-ready for all levels.
	 * the CSS uses this to advance the module readiness cascade toward
	 * RouteCODEC. we do NOT do I-switch setup here — the CSS handles
	 * its own I-switch setup via p_dspa_msg_CallBack. */
	for (int i = 0; i < 4; i++)
		ringbuf_send_level_ready(&ctx->rb, i);
}
