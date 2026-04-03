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

/* ring cadence */
#define IFX_TAPI_RING_CADENCE_HR_SET      0x7103

/*
 * ring cadence structure (for IFX_TAPI_RING_CADENCE_HR_SET)
 *
 * bitmap-encoded cadence pattern. each bit = 50ms.
 * 1 = ring, 0 = silent. periodic pattern repeats until RING_STOP.
 */
#define TAPI_RING_CADENCE_MAX_BYTES 40

typedef struct {
	uint8_t data[TAPI_RING_CADENCE_MAX_BYTES]; /* periodic cadence bitmap */
	int32_t nr;                                 /* valid bits in data[] */
	uint8_t initial[TAPI_RING_CADENCE_MAX_BYTES]; /* initial cadence (once) */
	int32_t initialNr;                          /* valid bits in initial[] */
} tapi_ring_cadence_t;

/* diagnostics */
#define VINETIC_GR909_START               0x4e0f
#define VINETIC_GR909_RESULT              0x4e11

/*
 * line feed states (for IFX_TAPI_LINE_FEED_SET ioctl 0x7101)
 *
 * these are the ioctl-level values accepted by grandstream's drv_tapi.ko,
 * which passes them to drv_silabs.ko (IFX_TAPI_LL_ALM_CPE_Line_Mode_Set
 * at 0x000401d4) for ProSLIC register mapping.
 *
 * NOTE: gs_ata uses its OWN internal enum and remaps in Nuvoton::setLineState
 * (FUN_00020eec) before calling the ioctl. the gs_ata enum is:
 *   gs_ata 0 → ioctl 4  (DISABLED)
 *   gs_ata 1 → ioctl 2  (STANDBY — but see note below)
 *   gs_ata 2 → ioctl 0  (ACTIVE)
 *   gs_ata 3 → ioctl 1  (ACTIVE_REV)
 *   gs_ata 4 → ioctl 23 (GS-specific reversed mode)
 *
 * drv_silabs.ko ProSLIC register mapping (Si32260 LINEFEED reg 0x1E):
 *   ioctl 0  (ACTIVE)     → ProSLIC 1 (FWD_ACTIVE)
 *   ioctl 1  (ACTIVE_REV) → ProSLIC 5 (REV_ACTIVE)
 *   ioctl 2  (STANDBY)    → ProSLIC 1 (FWD_ACTIVE) ← SAME as ACTIVE!
 *   ioctl 4  (DISABLED)   → ProSLIC 0 (OPEN)
 *   ioctl 10 (RING_BURST) → ProSLIC 4 (RINGING)
 *   ioctl 11 (RING_PAUSE) → ProSLIC 1 (FWD_ACTIVE)
 *   ioctl 21              → ProSLIC 2 (FWD_OHT)
 *   ioctl 22              → ProSLIC 6 (REV_OHT)
 *   ioctl 23 (GS custom)  → ProSLIC 5 (REV_ACTIVE)
 *   ioctl 24              → ProSLIC 0 (OPEN) + disable hook IRQ
 *
 * STANDBY (2) maps to the same ProSLIC state as ACTIVE (0). grandstream
 * does not implement a low-power standby mode — the line runs at full
 * battery in both states. drv_tapi.ko still tracks the logical state
 * internally for polarity management.
 */
enum tapi_line_feed {
	IFX_TAPI_LINE_FEED_ACTIVE       = 0,  /* ProSLIC FWD_ACTIVE — battery on, phone works */
	IFX_TAPI_LINE_FEED_ACTIVE_REV   = 1,  /* ProSLIC REV_ACTIVE — reversed polarity */
	IFX_TAPI_LINE_FEED_STANDBY      = 2,  /* ProSLIC FWD_ACTIVE — same as ACTIVE electrically */
	IFX_TAPI_LINE_FEED_HIGH_IMPEDANCE = 3,  /* rejected by drv_silabs (returns -1) */
	IFX_TAPI_LINE_FEED_DISABLED     = 4,  /* ProSLIC OPEN — line off */
	/* 5: obsolete (GROUND_START) */
	IFX_TAPI_LINE_FEED_NORMAL_AUTO  = 6,  /* same as ACTIVE */
	IFX_TAPI_LINE_FEED_REVERSED_AUTO = 7,  /* same as ACTIVE_REV */
	IFX_TAPI_LINE_FEED_NORMAL_LOW   = 8,  /* rejected by drv_silabs default case */
	IFX_TAPI_LINE_FEED_REVERSED_LOW = 9,  /* rejected by drv_silabs default case */
	IFX_TAPI_LINE_FEED_RING_BURST   = 10, /* ProSLIC RINGING */
	IFX_TAPI_LINE_FEED_RING_PAUSE   = 11, /* ProSLIC FWD_ACTIVE */
	IFX_TAPI_LINE_FEED_FWD_OHT      = 21, /* ProSLIC FWD_OHT (on-hook transmission) */
	IFX_TAPI_LINE_FEED_REV_OHT      = 22, /* ProSLIC REV_OHT */
	IFX_TAPI_LINE_FEED_GS_REVERSED  = 23, /* GS custom: ProSLIC REV_ACTIVE */
	IFX_TAPI_LINE_FEED_GS_OPEN_NOIRQ = 24, /* GS custom: ProSLIC OPEN + disable hook IRQ */
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

/*
 * TAPI event IDs (IFX_TAPI_EVENT_GET ioctl 0x71c0)
 *
 * verified against Get_IFX_TAPI_EVENT_ID_Str() in GS drv_tapi.ko — these
 * match the upstream Infineon SDK values exactly.
 *
 * event ID = type (upper bits) | subtype (lower 16 bits)
 */

/* event type masks */
#define TAPI_EVENT_TYPE_FXS          0x20000000
#define TAPI_EVENT_TYPE_FXO          0x21000000
#define TAPI_EVENT_TYPE_PULSE        0x30000000
#define TAPI_EVENT_TYPE_DTMF         0x31000000
#define TAPI_EVENT_TYPE_CID          0x32000000
#define TAPI_EVENT_TYPE_TONE_GEN     0x33000000
#define TAPI_EVENT_TYPE_TONE_DET     0x34000000
#define TAPI_EVENT_TYPE_FAULT_LINE   0xF2000000
#define TAPI_EVENT_TYPE_MASK         0xFF000000

/* FXS events */
#define TAPI_EVENT_FXS_RINGBURST_END 0x20000002
#define TAPI_EVENT_FXS_RINGING_END   0x20000003
#define TAPI_EVENT_FXS_ONHOOK        0x20000004
#define TAPI_EVENT_FXS_OFFHOOK       0x20000005
#define TAPI_EVENT_FXS_FLASH         0x20000006

/* digit detection
 *
 * DTMF data.value format (verified on hardware):
 *   byte 2 (bits 23-16): ASCII character ('0'-'9', '*', '#')
 *   byte 1 (bits 15-8):  digit index (1-9, 0xa='0', 0xb='*', 0xc='#')
 *   byte 0 (bits 7-0):   action (1 = key down, presumably 0 = key up)
 *
 * pulse data.value format:
 *   byte 1 (bits 15-8): pulse count (1-9 = digits 1-9, 11 = digit 0)
 *   byte 0 (bits 7-0):  0x00
 */
#define TAPI_EVENT_PULSE_DIGIT       0x30000001
#define TAPI_EVENT_DTMF_DIGIT        0x31000001

/* line fault events */
#define TAPI_EVENT_FAULT_OVERTEMP    0xF2000005
#define TAPI_EVENT_FAULT_OVERCURRENT 0xF2000006
#define TAPI_EVENT_FAULT_OVERVOLTAGE 0xF2000007

/*
 * TAPI event structure (returned by IFX_TAPI_EVENT_GET)
 *
 * layout verified from IFX_TAPI_EventFifoGet() in GS drv_tapi.ko:
 * copies exactly 4 x uint32 (16 bytes) from the event FIFO.
 * TAPI_Phone_GetEvent() then overwrites the 'more' field.
 */
typedef struct {
	uint32_t id;       /* event ID (type | subtype), see defines above */
	uint16_t ch;       /* channel that generated the event */
	uint16_t more;     /* nonzero if more events pending in FIFO */
	union {
		uint32_t value;    /* generic value (e.g. DTMF digit ASCII) */
		uint32_t raw[2];   /* raw event data (8 bytes) */
	} data;
} tapi_event_t;

#endif /* COMATOSE_TAPI_DEFS_H */
