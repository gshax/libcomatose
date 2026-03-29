/*
 * comatose/dua_defs.h - DUA protocol definitions (header-only)
 *
 * all values reverse-engineered from the CSS firmware (_css.elf) DWARF symbols
 * and the master name-number lookup table at 0x022ba59c (2993 entries).
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_DUA_DEFS_H
#define COMATOSE_DUA_DEFS_H

#include <stdint.h>

/*
 * DUA unit types
 */
enum dua_unit_type {
	DUA_UT_SPVOIPNDA = 0,  /* VoIP speech processing (16 instances) */
	DUA_UT_TRACE     = 1,  /* debug/tracing (3 instances) */
	DUA_UT_FXS       = 2,  /* FXS/SLIC port (8 instances) */
};

/*
 * DUA message commands
 *
 * these are the cmd field values in struct dua_msg, dispatched by
 * dua_process() on the CSS side.
 *
 * numeric commands (low-level, use UIDs directly):
 */
enum dua_cmd {
	/* connection/unit management (recovered from switch8 table at 0x022700a5) */
	DUA_CMD_INIT_REQ           = 0x01, /* p_dua_InitReq - line/channel init */
	DUA_CMD_GO_IDLE            = 0x02, /* p_da_GoIdleReq - DSP idle mode */
	/* 0x03 is a noop */
	DUA_CMD_UNIT_CONNECT       = 0x04, /* p_dua_UnitConnectReq(uid, connId) */
	DUA_CMD_UNIT_DISCONNECT    = 0x05, /* p_dua_UnitDisconnectReq(uid, connId) */
	DUA_CMD_CONN_CREATE        = 0x06, /* p_dua_ConnCreateReq() -> connId */
	DUA_CMD_CONN_DELETE        = 0x07, /* p_dua_ConnDeleteReq(connId) */
	DUA_CMD_CONN_MERGE         = 0x08, /* p_dua_ConnMergeReq(conn1, conn2) */
	DUA_CMD_CONN_UNMERGE       = 0x09, /* p_dua_ConnUnmergeReq(connId) */
	DUA_CMD_UNIT_ALLOCATE      = 0x0a, /* p_dua_UnitAllocateReq(type, spec) */
	/* 0x0b unused */
	DUA_CMD_UNIT_FREE          = 0x0c, /* p_dua_UnitFreeReq(uid) */
	DUA_CMD_UNIT_SET           = 0x0d, /* p_dua_UnitSetReq(uid, elem, param, ...) */
	DUA_CMD_UNIT_GET           = 0x0e, /* p_dua_UnitGetReq(uid, elem, param, ...) */
	DUA_CMD_DIAL_DTMF          = 0x0f, /* p_dua_DialDTMFReq(uid, ...) */
	DUA_CMD_STOP_DTMF          = 0x10, /* p_dua_StopDTMF(uid, elem) */
	DUA_CMD_PLAY_MELODY        = 0x11, /* p_dua_PlayMelodyReq(uid, ...) */
	DUA_CMD_STOP_MELODY        = 0x12, /* p_dua_StopMelody(uid, elem) */
	DUA_CMD_APPL_INIT          = 0x13, /* p_dua_ApplInit() - call first! */

	/* string-based commands (high-level, use type name strings) */
	DUA_CMD_SC_NUMBER          = 0x4a, /* p_duasc_Number(name) */
	DUA_CMD_SC_NAME            = 0x4b, /* p_duasc_Name(uid) */
	/* 0x4c-0x54: string-based conn/unit ops (second switch table) */
	DUA_CMD_SC_CONN_UNMERGE    = 0x55, /* p_duasc_ConnUnmergeReq(connId) */
	DUA_CMD_SC_UNIT_ALLOCATE   = 0x56, /* p_duasc_UnitAllocateReq(type_str, flags) */
	DUA_CMD_SC_UNIT_FREE       = 0x57, /* p_duasc_UnitFreeReq(type_str, flags) */
	DUA_CMD_SC_UNIT_SET        = 0x58, /* p_duasc_UnitSetReq(type, id, elem, param, ...) */
	DUA_CMD_SC_UNIT_GET        = 0x59, /* p_duasc_UnitGetReq(type, id, elem, param, ...) */

	/* enumeration / discovery */
	DUA_CMD_SC_ENUM_UT         = 0x7b, /* p_duasc_EnumUT - enumerate unit types */
	DUA_CMD_SC_ENUM_UE         = 0x7c, /* p_duasc_EnumUE - enumerate unit elements */
	DUA_CMD_SC_ENUM_UE_PARAM   = 0x7d, /* p_duasc_EnumUEParam - enumerate params */

	/* debug */
	DUA_CMD_TRACE_CFG          = 0x88, /* text_trace_configure */
};

/*
 * DUA parameter codes (for UnitSet/UnitGet)
 *
 * codes 0x0000-0xFFFF are direct DSP register offsets, accessed via
 * p_da_SetDSPValue(). codes 0x10000+ are unit-level parameters.
 */
enum dua_param {
	/* special low-level params */
	DUA_PARAM_MIN_CODE            = -5,
	DUA_PARAM_SIMBMP              = -4,    /* simulate bitmap */
	DUA_PARAM_SIMDSP              = -3,    /* simulate DSP */
	DUA_PARAM_SETTOG              = -2,    /* set tone generator */

	/* DSP address range (0x0000-0xFFFF) -- not enumerated here,
	   use raw integer values for direct DSP register access */

	/* unit-level params */
	DUA_PARAM_UNIT                = 0x10000,
	DUA_PARAM_PIN_CAPS            = 0x10001, /* pin capabilities */
	DUA_PARAM_PIN_SET_WEIGHTS     = 0x10002,
	DUA_PARAM_PIN_RES_WEIGHTS     = 0x10003,
	DUA_PARAM_PIN_SET_CONNATTR    = 0x10004,
	DUA_PARAM_PIN_RES_CONNATTR    = 0x10005,
	DUA_PARAM_PIN_MUTE            = 0x10006, /* mute a pin */
	DUA_PARAM_PIN_SOFTMUTE        = 0x10007, /* gradual mute */
	DUA_PARAM_PIN_VOLUME          = 0x10008, /* set volume level */
	DUA_PARAM_SFSWITCH            = 0x1007f, /* subflow switch */
	DUA_PARAM_ISWITCH             = 0x10080, /* input switch */
	DUA_PARAM_DSP_ARRAY_VALUE     = 0x10081,
	DUA_PARAM_DSP_STRUCT_VALUE    = 0x10082,
	DUA_PARAM_USM_DO              = 0x100ff, /* invoke unit state machine */
	DUA_PARAM_UMT_EXEC_GEN        = 0x10100, /* execute generic UMT mode */
	DUA_PARAM_UMT_EXEC_STAT       = 0x10101, /* execute static UMT */
	DUA_PARAM_UMT_EXEC_DYN        = 0x10102, /* execute dynamic UMT */
	DUA_PARAM_UMT_IMMEDIATE        = 0x10103,
	DUA_PARAM_UMT_LOAD_DYN         = 0x10104, /* load dynamic UMT data */
	DUA_PARAM_GET_FREE_UNITS       = 0x10105,
	DUA_PARAM_UNIT_GET_NUM         = 0x10106,
	DUA_PARAM_GET_CONN_OF_UID      = 0x10107,
	DUA_PARAM_GET_UID_OF_CONN      = 0x10108,
	DUA_PARAM_DA_PARAMS            = 0x10109,
	DUA_PARAM_CBK_FUNC             = 0x1010a, /* register async callback */
	DUA_PARAM_CBK_PARAM            = 0x1010b,
	DUA_PARAM_SETFUNC              = 0x1010c,
	DUA_PARAM_GETFUNC              = 0x1010d,
	DUA_PARAM_MEM8                 = 0x10110, /* read/write 8-bit memory */
	DUA_PARAM_MEM16                = 0x10111,
	DUA_PARAM_MEM32                = 0x10112,
	DUA_PARAM_PRIVATE              = 0x18000, /* start of private params */
	DUA_PARAM_LAST_PREDEF          = 0x1ffff,
};

/*
 * DUA event codes (for callbacks/responses)
 */
enum dua_event {
	/* core events */
	DUAEV_OK                       = 0,
	DUAEV_READY                    = 1,
	DUAEV_INIT_IND                 = 2,
	DUAEV_CONNCREATE_IND           = 3,
	DUAEV_CONNDELETE_IND           = 4,
	DUAEV_UNITCONNECT_IND          = 5,
	DUAEV_UNITDISCONNECT_IND       = 6,
	DUAEV_CONNMERGE_IND            = 7,
	DUAEV_CONNUNMERGE_IND          = 8,
	DUAEV_UNITALLOCATE_IND         = 9,
	DUAEV_UNITFREE_IND             = 10,
	DUAEV_SET_IND                  = 11,
	DUAEV_GET_IND                  = 12,
	DUAEV_PIN_MUTED                = 13,
	DUAEV_PIN_UNMUTED              = 14,

	/* caller ID events */
	DUAEV_CID_START                = 0x1000,
	DUAEV_CID_IND                  = 0x1001,
	DUAEV_CID_PROGRESS             = 0x1002,
	DUAEV_CID_TIMEOUT              = 0x1003,

	/* caller ID transmit */
	DUAEV_CIT_START                = 0x1010,
	DUAEV_CIT_IND                  = 0x1011,
	DUAEV_CIT_ERROR                = 0x1012,
	DUAEV_CIT_TIMER                = 0x1013,

	/* call progress detection */
	DUAEV_CPD_START                = 0x1020,
	DUAEV_CPD_CONT_IND             = 0x1021,
	DUAEV_CPD_BUSY_IND             = 0x1022,
	DUAEV_CPD_TIMEOUT              = 0x1023,
	DUAEV_CPD_TIMER                = 0x1024,

	/* DTMF / frequency collection */
	DUAEV_DFC_START                = 0x1030,
	DUAEV_DFC_DTMF_IND            = 0x1031, /* DTMF digit detected */
	DUAEV_DFC_DTMF_TIMEOUT        = 0x1032,
	DUAEV_DFC_CAS_IND             = 0x1033,
	DUAEV_DFC_CAS_TIMEOUT         = 0x1034,

	/* general tone detection */
	DUAEV_GTD_START                = 0x1040,
	DUAEV_GTD_FAX_IND             = 0x1041, /* fax tone detected */
	DUAEV_GTD_FAX_TIMEOUT         = 0x1042,
	DUAEV_GTD_MODEM_IND           = 0x1043, /* modem tone detected */
	DUAEV_GTD_MODEM_TIMEOUT       = 0x1044,

	/* ring cadence detection */
	DUAEV_RCD_START                = 0x1050,
	DUAEV_RCD_DTMF_IND            = 0x1051,
	DUAEV_RCD_DTMF_TIMEOUT        = 0x1052,

	/* ring detection */
	DUAEV_RGD_START                = 0x1060,
	DUAEV_RGD_RING_START           = 0x1061, /* ring started */
	DUAEV_RGD_RING_END            = 0x1062, /* ring ended */
	DUAEV_RGD_RING_RPAS           = 0x1063,
	DUAEV_RGD_LINE_REVERSAL       = 0x1064, /* line polarity reversal */

	/* SLIC control */
	DUAEV_SLC_START                = 0x1070,
	DUAEV_SLC_IND                  = 0x1071,
	DUAEV_SLC_TIMEOUT              = 0x1072,
	DUAEV_SLC_TIMER                = 0x1073,

	DUAEV_USM_LAST                 = 0x1fff,

	/* param events */
	DUAEV_PARAM_BASE               = 0x2000,
	DUAEV_PARAM_VOIP_RECV_PACKET   = 0x2001,
	DUAEV_PARAM_VOIP_RTCP_REPORT   = 0x2002,
	DUAEV_PARAM_VOIP_DTMF          = 0x2003,
	DUAEV_MEM_DUMP                 = 0x2004,
	DUAEV_PARAM_LAST               = 0x2fff,

	/* warnings */
	DUAEV_WARNING_BASE             = 0x3000,
	DUAEV_CW_OVERFLOW_WARN         = 0x3001,
	DUAEV_UNIT_NOT_CONNECTED       = 0x3002,
	DUAEV_SET_PIN_CAPS_SKIPPED     = 0x3003,
	DUAEV_SOFTMUTE_TIMEOUT         = 0x3004,
	DUAEV_MAX_ARCID_OFL            = 0x3005,
	DUAEV_MAX_ART_OR_PINART_OFL    = 0x3006,
	DUAEV_UMOP_WAITING             = 0x3007,
	DUAEV_WARNING_LAST             = 0x3fff,

	/* trace events */
	DUAEV_TRACE_DA                 = 0x4000,
	DUAEV_TRACE_DSP                = 0x4001,
	DUAEV_LAST                     = 0x4002,
};

/*
 * DUA error codes (negative return values)
 */
enum dua_error {
	DUA_ERR_NONE                   = -3,   /* sentinel */
	DUA_ERR_UNDEF                  = -4,
	DUA_INIT_FAIL                  = -5,
	DUA_NO_FREE_CONNECTION         = -6,
	DUA_UNIT_CONN                  = -7,   /* unit already connected */
	DUA_UNIT_FREE_ERR              = -8,   /* unit already free */
	DUA_INVALID_UNIT               = -9,
	DUA_CONNECT_ERROR              = -10,
	DUA_DISCONN_ERROR              = -11,
	DUA_RANGE_ERROR                = -12,
	DUA_UNITS_OVERFLOW             = -13,
	DUA_STATUS_NOT_READY           = -14,
	DUA_NO_FREE_UNIT               = -15,
	DUA_PARAMVALUE_RANGE           = -16,
	DUA_PARAMTYPE_WRONG            = -17,
	DUA_MEMORY_LOW                 = -18,
	DUA_BAD_DYN_MODE_DEF           = -19,
	DUA_INVALID_ELEM               = -20,
	DUA_BUFFERSIZE_ERROR           = -21,
	DUA_INVALID_UNITTYPE           = -22,
	DUA_INVALID_PIN                = -23,
	DUA_INVALID_DSPINST            = -24,
	DUA_NO_DSPINST_FOR_UNITELEM    = -25,
	DUA_INVALID_GEN_MODE           = -26,
	DUA_GETFUNC_REQUIRED           = -27,
	DUA_SETFUNC_REQUIRED           = -28,
	DUA_REQ_PENDING                = -29,
	DUA_NOT_CAPABLE                = -30,
	DUA_INVALID_CONNECTION         = -31,
	DUA_FATAL                      = -32,
	DUA_UNIT_ERROR_TEXT            = -33,
	DUA_UNIT_ERROR_USM             = -34,
	DUA_UNIT_NOT_CLONEABLE         = -35,
	DUA_INVALID_PARAMCODE          = -36,
	DUA_MISSING_PARAM              = -37,
};

/*
 * DUASM state machine operations
 * (internal to CSS, but useful for understanding protocol flow)
 */
enum dua_sm {
	DUASM_CONNCREATE       = 0,
	DUASM_CONNDELETE       = 1,
	DUASM_UNITCONNECT      = 2,
	DUASM_UNITDISCONNECT   = 3,
	DUASM_CONNMERGE        = 4,
	DUASM_CONNUNMERGE      = 5,
	DUASM_UNITALLOCATE     = 6,
	DUASM_UNITFREE         = 7,
	DUASM_UNITSET          = 8,
	DUASM_UNITGET          = 9,
};

/*
 * UMT (unit mode table) generic mode indices
 */

/* UT_SPVOIPNDA modes */
#define DUA_UMT_SPVOIP_FULL_INIT     0  /* full cold-start pipeline init */
#define DUA_UMT_SPVOIP_NB_20MS       1  /* narrowband, 20ms (160 samples @ 8kHz) */
#define DUA_UMT_SPVOIP_NB_40MS       2  /* narrowband, 40ms (320 samples @ 8kHz) */
#define DUA_UMT_SPVOIP_NB_60MS       3  /* narrowband, 60ms (480 samples @ 8kHz) */
#define DUA_UMT_SPVOIP_WB_20MS       4  /* wideband, 20ms (320 samples @ 16kHz) */
#define DUA_UMT_SPVOIP_WB_40MS       5  /* wideband, 40ms (640 samples @ 16kHz) */
#define DUA_UMT_SPVOIP_WB_40MS_ALT   6  /* wideband, 40ms (variant) */
#define DUA_UMT_SPVOIP_MINIMAL       7  /* minimal/passthrough (no codec install) */

/* UT_FXS modes */
#define DUA_UMT_FXS_INIT             0  /* full FXS init */
#define DUA_UMT_FXS_CFG_A            1  /* FXS reconfiguration A */
#define DUA_UMT_FXS_CFG_B            2  /* FXS reconfiguration B */

/* pin element IDs (each unit has two pins for bidirectional audio) */
#define DUA_PIN_ELEM_A               35
#define DUA_PIN_ELEM_B               40

/*
 * DUA message wire format
 *
 * sent over AF_COMA sockets to the "dua" service. the message is wrapped
 * in a struct cmsg by the COMA transport layer.
 */
struct dua_msg {
	uint32_t sender_id;   /* caller/session identifier */
	uint32_t flags;       /* flags/version (usually 0) */
	uint32_t cmd;         /* command from enum dua_cmd */
	uint32_t num_params;  /* number of uint32 params that follow */
	uint32_t params[];    /* command-specific parameters */
};

#define DUA_MSG_HEADER_SIZE  16  /* sizeof the fixed part (without params) */

#endif /* COMATOSE_DUA_DEFS_H */
