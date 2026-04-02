/*
 * comatose/bgsc_defs.h - BGSC protocol definitions (header-only)
 *
 * definitions for the CSS↔ARM BGSC (background scheduler) protocol, including
 * shared memory element descriptors, control word semantics, ring buffer
 * message types, and dispatch roles.
 *
 * this is the only open source implementation of this protocol. all values are
 * reverse-engineered from:
 *   - CSS firmware DWARF symbols (dfl_process_flow, dfl_set_I_switch, etc.)
 *   - full disassembly of the ARM dispatch loop (dfl_process_flow_asm @ 0x885b0)
 *   - stock app_dsp shared memory dumps + live hardware testing
 *   - static analysis of dfl_module_startup's 21 callee functions
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_BGSC_DEFS_H
#define COMATOSE_BGSC_DEFS_H

#include <stdint.h>

/*
 * ── control word (CW) bit definitions ──────────────────────────────────
 *
 * every element descriptor starts with a 32-bit control word that governs
 * how the dfl_process_flow dispatch loop processes the element. the dispatch
 * checks bits in this specific priority order (from ARM disassembly):
 *
 *   1. if bit 0 set → ACTIVE: tail-call the element's calc function
 *   2. if bits 0-4 all clear → INACTIVE: tail-call the idle handler
 *   3. otherwise → PENDING: call pending handler, post-calc, then update CW
 *
 * the CSS writes values like 0x10 or 0x12 to activate elements. the ARM
 * dispatch clears PENDING bits after processing: *cw = (cw & 0x06) ? 1 : 0.
 * bits 5+ are I-switch/mode flags that should NOT be cleared by the ARM.
 */

/* individual control word bits */
#define DFL_CW_ACTIVE        0x001  /* bit 0: element is active (running) */
#define DFL_CW_REPEAT        0x002  /* bit 1: stay on current FTAB entry */
#define DFL_CW_PERSIST        0x004  /* bit 2: keep active after pending clear */
#define DFL_CW_POST_CALC_1   0x008  /* bit 3: call post-calc handler 1 */
#define DFL_CW_POST_CALC_2   0x010  /* bit 4: call post-calc handler 2 (startup) */
#define DFL_CW_PREPARE_RUN   0x020  /* bit 5: I-switch prepare (→ active after shift) */
#define DFL_CW_ALT_PREPARE   0x080  /* bit 7: alternate I-switch prepare */
#define DFL_CW_MODE_100      0x100  /* bit 8: mode flag (set by p_dspa_msg_CallBack) */
#define DFL_CW_MODE_200      0x200  /* bit 9: mode flag (set by p_dspa_msg_CallBack) */

/*
 * mask for "PENDING" check: bits 1-4. if any of these are set (and bit 0 is
 * not), the dispatch takes the PENDING path. the ARM must clear these after
 * processing to signal completion.
 *
 * IMPORTANT: bits 5+ (PREPARE_RUN, ALT_PREPARE, MODE_*) are I-switch flags
 * set by the CSS during state machine transitions. the ARM must NEVER clear
 * these — they are processed by the CSS level 0 dispatch.
 */
#define DFL_CW_PENDING_MASK  0x01e  /* bits 1-4: pending operation flags */

/*
 * mask for "activate" check during PENDING clear: bits 1-2 (REPEAT|PERSIST).
 * if either is set, the element becomes ACTIVE (cw=1) after clearing.
 * if neither is set, the element becomes INACTIVE (cw=0).
 *
 * stock protocol: *cw = (cw & DFL_CW_ACTIVATE_MASK) ? 1 : 0
 */
#define DFL_CW_ACTIVATE_MASK 0x006  /* bits 1-2: becomes active when set */

/*
 * ── ring buffer message types ──────────────────────────────────────────
 *
 * ARM→CSS messages in shared memory ring buffer. each message is:
 *   [instance_addr, (type << 13) | num_params, param0, param1, ...]
 *
 * the CSS reads these via p_adsp_check_msg_fifo and dispatches to:
 *   - p_dsp_CallBack for type 1 (level ready) and type 4 (heartbeat)
 *   - p_auc_CB_handler for type 0 (completion) during SETCODEC
 *   - decoder handler for type 3 (decoder ready) during frame production
 */
#define DFL_MSG_TYPE_COMPLETION    0  /* element activation complete */
#define DFL_MSG_TYPE_LEVEL_READY   1  /* BG level init complete */
#define DFL_MSG_TYPE_DECODER_ACK   1  /* decoder frame acknowledgement (same type!) */
#define DFL_MSG_TYPE_DECODER_READY 3  /* kick CSS encoder frame production */
#define DFL_MSG_TYPE_HEARTBEAT     4  /* periodic ARM→CSS keepalive */

/* encode a ring buffer message header word */
#define DFL_MSG_HDR(type, nparams) (((type) << 13) | (nparams))

/*
 * ── buffer element constants ───────────────────────────────────────────
 */

/*
 * dual-buffer differentiation offset. during SETCODEC activation, the buffer
 * element's buf_ptr_a (+0x04) is shifted by this amount from buf_ptr_b (+0x08)
 * to create separate input/output audio buffers. without this offset, both
 * pointers reference the same memory and the encoder reads stale init data
 * instead of live TDM audio.
 *
 * stock value observed across multiple sessions. likely sizeof one audio
 * frame buffer (320 bytes = 160 samples × 2 bytes at 8kHz/20ms).
 */
#define DFL_BUFFER_DUAL_OFFSET  0x140

/*
 * ── element dispatch roles ─────────────────────────────────────────────
 *
 * each element in the shared memory descriptor table is classified into a
 * dispatch role that determines how the ARM dispatch loop handles it.
 *
 * the fundamental rule: CSS-level elements (BG level 0) must NEVER be
 * modified by the ARM dispatch. the CSS level 0 dispatch processes these
 * for signal routing (SSW, SSR, SU2 modules that move TDM audio through
 * the DSP pipeline). ARM dispatch handles levels 1-3 only.
 */
enum dfl_elem_role {
	DFL_ROLE_UNKNOWN = 0,   /* unclassified — observe only, never modify */
	DFL_ROLE_CSS,           /* CSS level 0 — never touch (signal routing) */
	DFL_ROLE_CODEC_HDR,     /* group header (elem[0], elem[23]) — never touch */
	DFL_ROLE_ENCODER,       /* elem[6]: encoder activation during SETCODEC */
	DFL_ROLE_DECODER,       /* elem[21]: frame ack + periodic decoder ready */
	DFL_ROLE_BUFFER,        /* elem[22]: dual-buffer setup during activation */
	DFL_ROLE_ARM_NOOP,      /* ARM level 1 element, confirmed noop for L16 */
};

/*
 * ── element descriptor structures ──────────────────────────────────────
 *
 * element descriptors live in shared memory (mapped via /dev/sharedmem).
 * the CSS creates them during DUA init and fills CSS-managed fields.
 * the ARM's module startup fills the remaining fields.
 *
 * field ownership legend:
 *   [CW]  = control word: written by CSS (activation), cleared by ARM
 *   [CSS] = CSS-managed: written during DUA init, do NOT overwrite
 *   [ARM] = ARM-managed: written by module startup, WE populate these
 */
struct dfl_elem_hdr {
	uint32_t cw;            /* +0x00 [CW]  control word (see CW bit defs) */
	uint32_t config;        /* +0x04 [ARM] module config flag */
	uint32_t field_08;      /* +0x08 */
	uint32_t field_0c;      /* +0x0c */
	uint32_t group_type;    /* +0x10 [ARM] BG level group type */
	uint32_t css_data;      /* +0x14 [CSS] per-element CSS data pointer */
	uint32_t buf_config;    /* +0x18 [ARM] buffer config, typically 0x100 */
	uint32_t css_flow;      /* +0x1c [CSS] flow table / group descriptor ptr */
	uint32_t arm_module;    /* +0x20 [ARM] module registration address */
	uint32_t field_24;      /* +0x24 */
};

/* codec config element — elem[0] per group (also elem[23] = next group's).
 * the config field (DFL_CODEC_REGISTERED = 0x0e) signals to the CSS that
 * ARM-side codecs are registered for this group. cw is always 1 (active). */
struct dfl_elem_codec {
	struct dfl_elem_hdr hdr;
};

/* encoder element — elem[6] per group.
 * largest descriptor (~232 bytes in stock). during SETCODEC activation
 * (cw = DFL_CW_POST_CALC_2 | DFL_CW_PERSIST), the ARM clears the CW
 * and sends a completion event. */
struct dfl_elem_encoder {
	struct dfl_elem_hdr hdr;
	uint32_t frame_config;  /* +0x28 [ARM] u16 pair: (buf_size, frame_size) */
	                        /*       stock: 0x005000a0 = u16(160, 80) */
};

/* decoder element — elem[21] per group.
 * small (~44 bytes). activated to DFL_CW_POST_CALC_2 | DFL_CW_PERSIST
 * during SETCODEC. the ARM sends periodic decoder-ready events and per-frame
 * acks via the ring buffer. */
struct dfl_elem_decoder {
	struct dfl_elem_hdr hdr;
	uint32_t frame_config;  /* +0x28 [ARM] u16 pair: (buf_size, frame_size) */
};

/* buffer element — elem[22] per group.
 * ALL fields are CSS-managed. the ARM applies DFL_BUFFER_DUAL_OFFSET to
 * buf_ptr_a during activation but otherwise must not write. */
struct dfl_elem_buffer {
	uint32_t cw;            /* +0x00 [CW]  control word */
	uint32_t buf_ptr_a;     /* +0x04 [CSS] audio buffer A (shifted during activation) */
	uint32_t buf_ptr_b;     /* +0x08 [CSS] audio buffer B (base pointer) */
	uint32_t buf_ptr_c;     /* +0x0c [CSS] audio buffer C */
	uint32_t buf_ptr_d;     /* +0x10 [CSS] audio buffer D */
	uint32_t buf_config;    /* +0x14 [CSS] config (0x52 in stock baseline) */
};

/*
 * well-known element positions within each 24-element group.
 * indices into the element table at shm+0xb854. consistent across boots.
 */
enum dfl_elem_index {
	DFL_ELEM_CODEC      = 0,   /* codec config / group header */
	DFL_ELEM_ENCODER    = 6,   /* encoder (largest descriptor) */
	DFL_ELEM_DECODER    = 21,  /* decoder (frame counter / ack) */
	DFL_ELEM_BUFFER     = 22,  /* buffer (CSS-managed dual-buffer) */
	DFL_ELEM_NEXT_CODEC = 23,  /* next group's codec config */
};

/*
 * group type constants — written to dfl_elem_hdr.group_type.
 * encodes the BG processing level as a u16 pair.
 */
#define DFL_GROUP_TYPE_LEVEL0  0x00040004  /* CSS level 0 (signal routing) */
#define DFL_GROUP_TYPE_LEVEL1  0x00010001  /* ARM level 1 (codec processing) */

/* codec registration flag for elem[0].hdr.config.
 * signals CSS that ARM codecs are available for this group. */
#define DFL_CODEC_REGISTERED   0x0e

/* default frame config for encoder/decoder elements.
 * u16 pair: (frame_samples=160, frame_bytes=80) for 8kHz narrowband. */
#define DFL_FRAME_CONFIG_DEFAULT  0x005000a0

#endif /* COMATOSE_BGSC_DEFS_H */
