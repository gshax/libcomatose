/*
 * comatose/tapi_defs.h - TAPI ioctl definitions (header-only)
 *
 * cherry-picked from the Infineon/Lantiq TAPI 2.x SDK headers as mutated
 * by grandstream's internal fork for the DVF99 platform.
 *
 * BSP definitions derived from old ht_bsp.h.
 *
 * Copyright (C) 2025-2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_TAPI_DEFS_H
#define COMATOSE_TAPI_DEFS_H

#include <stdint.h>

/*
 * device paths and major numbers
 */
#define TAPI_DEV_BSP         "/dev/slic_bsp"
#define TAPI_DEV_FXS_FMT     "/dev/fxs%02i"
#define TAPI_DEV_VOICE_FMT   "/dev/voice%02i"
#define TAPI_DEV_SHMEM       "/dev/sharedmem"

#define TAPI_MAJOR_BSP       122
#define TAPI_MAJOR_FXS       121
#define TAPI_MAJOR_VOICE     245
#define TAPI_MAJOR_SHMEM     249

/*
 * BSP ioctls (/dev/slic_bsp)
 */
#define HT_BSP_PRINT_VERSION   0x0
#define HT_BSP_INIT            0x1
#define HT_BSP_NOOP            0x2
#define HT_BSP_RESET_ASSERT    0x3
#define HT_BSP_RESET_CLEAR     0x4

typedef struct {
	char *model;          /* kernel pointer to model name string */
	int slic_count;
	int slic_channels;
	int daa_count;
	int daa_channels;
	int ren;              /* max ringer equivalence number */
	int unk0;
	int unk1;
} ht_bsp_init_result_t;

/*
 * FXS ioctls (/dev/fxsXX)
 *
 * these numbers are grandstream's mutated VINETIC values and do NOT match
 * any publicly available version of the Infineon/Lantiq TAPI SDK.
 */

/* channel initialization */
#define IFX_TAPI_CH_INIT                  0x710f

/* line feed control */
#define IFX_TAPI_LINE_FEED_SET            0x7101
#define IFX_TAPI_LINE_HOOK_STATUS_GET     0x7184
#define IFX_TAPI_LINE_HOOK_VT_SET         0x712e
#define IFX_TAPI_LINE_DC_FEED_SET         0x7149
#define IFX_TAPI_LINE_IMPEDANCE_SET       0x7109
#define IFX_TAPI_LINE_TYPE_SET            0x7107

/* ring control */
#define IFX_TAPI_RING_CFG_SET             0x7102
#define IFX_TAPI_RING_CADENCE_HR_SET      0x7103
#define IFX_TAPI_RING_START               0x7187
#define IFX_TAPI_RING_STOP                0x7188
#define IFX_TAPI_RING_MAX_SET             0x7185

/* PCM / audio */
#define IFX_TAPI_PCM_CFG_SET              0x7104
#define IFX_TAPI_PCM_ACTIVATION_SET       0x7106
#define IFX_TAPI_PCM_VOLUME_SET           0x7145

/* tones */
#define IFX_TAPI_TONE_LOCAL_PLAY          0x719b
#define IFX_TAPI_TONE_NET_PLAY            0x71c5
#define IFX_TAPI_TONE_STOP                0x71a4

/* caller ID */
#define IFX_TAPI_CID_CFG_SET              0x71b0
#define IFX_TAPI_CID_TX_SEQ_START         0x71b2
#define IFX_TAPI_CID_TX_INFO_STOP         0x71b5

/* events */
#define IFX_TAPI_EVENT_GET                0x71c0
#define IFX_TAPI_EVENT_ENABLE             0x71c1
#define IFX_TAPI_EVENT_DISABLE            0x71c2

/* diagnostics */
#define VINETIC_GR909_START               0x4e0f
#define VINETIC_GR909_RESULT              0x4e11

/*
 * line feed states (for IFX_TAPI_LINE_FEED_SET)
 */
enum tapi_line_feed {
	IFX_TAPI_LINE_FEED_STANDBY      = 0,
	IFX_TAPI_LINE_FEED_ACTIVE       = 1,
	IFX_TAPI_LINE_FEED_ACTIVE_REV   = 2,  /* reversed polarity */
	IFX_TAPI_LINE_FEED_DISABLED     = 3,
	/* additional states may exist -- these are the known-used ones */
};

/*
 * line type configuration
 */
enum tapi_line_type {
	IFX_TAPI_LINE_TYPE_FXS = 0,
	IFX_TAPI_LINE_TYPE_FXO = 1,
};

typedef struct {
	int lineType;   /* enum tapi_line_type */
	int nDaaCh;     /* DAA channel (0 for FXS) */
} IFX_TAPI_LINE_TYPE_CFG_t;

#endif /* COMATOSE_TAPI_DEFS_H */
