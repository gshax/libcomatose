/*
 * comatose/bgsc_defs.h - BGSC element descriptor definitions (header-only)
 *
 * structure definitions for the CSS shared memory element descriptors.
 * these are populated by the ARM-side module startup (replacing stock
 * app_dsp's dfl_module_startup at FUN_00075810) and read by the CSS
 * via the (instance, offset) parameter system.
 *
 * all offsets and field meanings are reverse-engineered from:
 *   - stock app_dsp shared memory dumps (shm_decode analysis)
 *   - CSS firmware DWARF symbols (dfl_get_param_addr, WRITE_DSP_PARAM)
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
 * element descriptor common header
 *
 * present at the start of every element in shared memory. the CSS creates
 * the descriptor during DUA init and fills CSS-managed fields. the ARM's
 * module startup fills the remaining fields during codec registration.
 *
 * field ownership:
 *   [CW]  = control word, written by both CSS (activation) and ARM (clearing)
 *   [CSS] = CSS-managed, written during DUA init — do NOT overwrite
 *   [ARM] = ARM-managed, written by module startup — WE populate these
 *   [CFG] = configuration constant
 */
struct dfl_elem_hdr {
	uint32_t cw;            /* +0x00 [CW]  control word (I-switch value) */
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

/*
 * codec config element — elem[0] per group (also elem[23] = next group's)
 *
 * the codec config element is the "group header." its config field (0x0e)
 * tells the CSS that ARM-side codecs are registered for this group.
 * stock: elem[0].config = 0x0e, elem[0].cw = 1 (always active).
 */
struct dfl_elem_codec {
	struct dfl_elem_hdr hdr;
};

/*
 * encoder element — elem[6] per group
 *
 * the largest descriptor (~232 bytes in stock). contains codec init data,
 * function table pointers, and buffer configuration for the encoder path.
 *
 * during SETCODEC activation (cw = 0x14), the CSS expects the ARM to have
 * populated at minimum the common header fields. the CSS then writes
 * +4=1, +8=1 after session start.
 */
struct dfl_elem_encoder {
	struct dfl_elem_hdr hdr;
	uint32_t frame_config;  /* +0x28 [ARM] u16 pair: (buf_size, frame_size) */
	                        /*       stock: 0x005000a0 = u16(160, 80) */
	/* codec init fields follow at +0x2c through +0xe8.
	 * see research/claudes_notes/codec_table_investigation.md for
	 * the full field map. for L16 (noop codec), these may not be
	 * required — the CSS might only check the common header. */
};

/*
 * decoder element — elem[21] per group
 *
 * relatively small (~44 bytes). the decoder's control word gets activated
 * to 0x14 during SETCODEC. the ARM sends type=3 decoder-ready events to
 * kick frame production, and type=1 acks for the frame counter handshake.
 */
struct dfl_elem_decoder {
	struct dfl_elem_hdr hdr;
	uint32_t frame_config;  /* +0x28 [ARM] u16 pair: (buf_size, frame_size) */
	                        /*       stock: 0x005000a0 = u16(160, 80) */
};

/*
 * buffer element — elem[22] per group
 *
 * ALL fields are CSS-managed. the ARM must NOT write to this descriptor.
 * the CSS populates buffer pointers during DUA init and differentiates
 * them (+4 shifts by 0x140 from +8) during session start.
 *
 * defined here for documentation and shm_decode usage only.
 */
struct dfl_elem_buffer {
	uint32_t cw;            /* +0x00 [CW] control word */
	uint32_t buf_ptr_a;     /* +0x04 [CSS] buffer pointer A */
	uint32_t buf_ptr_b;     /* +0x08 [CSS] buffer pointer B */
	uint32_t buf_ptr_c;     /* +0x0c [CSS] buffer pointer C */
	uint32_t buf_ptr_d;     /* +0x10 [CSS] buffer pointer D */
	uint32_t buf_config;    /* +0x14 [CSS] config (0x52 in stock baseline) */
};

/*
 * well-known element positions within each 24-element group.
 * these indices into the element table at shm+0xb854 are consistent
 * across boots (confirmed by multiple stock and BGSC dumps).
 */
enum dfl_elem_index {
	DFL_ELEM_CODEC      = 0,   /* codec config / group header */
	/* elem[1]:  helper (connection/routing) */
	/* elem[2]:  G.711 decoder or similar large codec module */
	/* elem[3]:  passthrough / large sparse module */
	/* elem[4]:  signal processing module */
	/* elem[5]:  output helper */
	DFL_ELEM_ENCODER    = 6,   /* encoder (largest descriptor) */
	/* elem[7]:  config entry (+4=0x0c, 8 bytes only) */
	/* elem[8]:  helper */
	/* elem[9]:  helper */
	/* elem[10]: config entry (+4=0x0c, 8 bytes only) */
	/* elem[11]: helper */
	/* elem[12]: buffer manager (has audio buffer ptrs at shm+0xbe14!) */
	/* elem[13-20]: various helpers, level 1 elements */
	DFL_ELEM_DECODER    = 21,  /* decoder (frame counter / ack) */
	DFL_ELEM_BUFFER     = 22,  /* buffer (all CSS-managed, don't touch!) */
	DFL_ELEM_NEXT_CODEC = 23,  /* next group's codec config */
};

/*
 * group type constants — written to dfl_elem_hdr.group_type.
 * stock uses 0x00040004 for elements processed at BG level 0,
 * and 0x00010001 for elements at BG level 1.
 * the uint16 pair encoding suggests (level, level) or (count, count).
 */
#define DFL_GROUP_TYPE_LEVEL0  0x00040004
#define DFL_GROUP_TYPE_LEVEL1  0x00010001

/*
 * codec config flag — written to elem[0].hdr.config.
 * value 0x0e signals to the CSS that ARM-side codecs are registered
 * for this SPVOIPNDA group. without this, the CSS doesn't route audio.
 */
#define DFL_CODEC_REGISTERED   0x0e

/*
 * frame config constant for encoder/decoder.
 * uint16 pair: (frame_samples=160, frame_bytes=80) for 8kHz/10ms.
 * or possibly (buf_size=160, frame_size=80) for L16.
 */
#define DFL_FRAME_CONFIG_DEFAULT  0x005000a0

#endif /* COMATOSE_BGSC_DEFS_H */
