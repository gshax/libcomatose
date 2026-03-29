/*
 * comatose/voice_defs.h - voice/RTP service definitions (header-only)
 *
 * message types and constants for the COMA "voice" service.
 * derived from the GPL kernel header cmsg-voice.h and voice.h.
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_VOICE_DEFS_H
#define COMATOSE_VOICE_DEFS_H

/*
 * voice service message types (from cmsg-voice.h)
 */
enum cmsg_voice_type {
	CMSG_VOICE_REQUEST_GET_SESSION = 0,
	CMSG_VOICE_REPLY_GET_SESSION,
	CMSG_VOICE_REQUEST_SET_SESSION_FIFOS,
	CMSG_VOICE_REPLY_SET_SESSION_FIFOS,
	CMSG_VOICE_REQUEST_START_SESSION,
	CMSG_VOICE_REPLY_START_SESSION,
	CMSG_VOICE_REQUEST_STOP_SESSION,
	CMSG_VOICE_REPLY_STOP_SESSION,
	CMSG_VOICE_REQUEST_FREE_SESSION,
	CMSG_VOICE_REPLY_FREE_SESSION,
	CMSG_VOICE_REQUEST_SEND_DTMF,
	CMSG_VOICE_REPLY_SEND_DTMF,
	CMSG_VOICE_RECEIVE_DTMF,
	CMSG_VOICE_REQUEST_START_RTCP,
	CMSG_VOICE_REPLY_START_RTCP,
	CMSG_VOICE_REQUEST_STOP_RTCP,
	CMSG_VOICE_REPLY_STOP_RTCP,
	CMSG_VOICE_REQUEST_REPORT_RTCP,
	CMSG_VOICE_REPLY_REPORT_RTCP,
	CMSG_VOICE_REQUEST_UPDATE_SESSION,
	CMSG_VOICE_REPLY_UPDATE_SESSION,
	CMSG_VOICE_DATA,
	CMSG_VOICE_REQUEST_SEND_EVT,
	CMSG_VOICE_REPLY_SEND_EVT,
	CMSG_VOICE_REQUEST_UPDATE_RTCP,
	CMSG_VOICE_REPLY_UPDATE_RTCP,
};

/*
 * RTP payload type constants (from voice.h)
 */
#define RTP_PT_G711U       0
#define RTP_PT_G726        2
#define RTP_PT_G723        4
#define RTP_PT_G711A       8
#define RTP_PT_G722        9
#define RTP_PT_G729        18
#define RTP_PT_DYN_FIRST   96
#define RTP_PT_ILBC        98
#define RTP_PT_EVT_TEL     101
#define RTP_PT_DYN_LAST    127

/*
 * RTP session option flags
 */
#define RTP_OPT_NONE               0x00000000
#define RTP_OPT_SYMMETRIC_RSP      0x00000200
#define RTP_OPT_DTMF               0x00002000
#define RTP_OPT_PCMEXT_SESSION     0x00040000
#define RTP_OPT_APP                0x00080000
#define RTP_OPT_USE_JIB            0x00100000
#define RTP_OPT_RTCP_ON            0x00200000
#define RTP_OPT_USERSPACE          0x00400000
#define RTP_OPT_RX_HOLD            0x10000000
#define RTP_OPT_TX_HOLD            0x20000000
#define RTP_OPT_TX_TONE            0x40000000
#define RTP_OPT_T38_SESSION        0x80000000

/*
 * RTP codec option flags
 */
#define RTP_CODEC_OPT_NONE               0x00000000
#define RTP_CODEC_OPT_G723_5K3           0x00000001
#define RTP_CODEC_OPT_PLC                0x00000002
#define RTP_CODEC_OPT_VAD                0x00000004
#define RTP_CODEC_OPT_G726_NIBBLE_REV    0x00000008
#define RTP_CODEC_OPT_G726_16K           0x00000010
#define RTP_CODEC_OPT_G726_24K           0x00000020
#define RTP_CODEC_OPT_G726_32K           0x00000040
#define RTP_CODEC_OPT_G726_40K           0x00000080
#define RTP_CODEC_OPT_ILBC_15K2          0x00000100
#define RTP_CODEC_OPT_AMRWB_24K          0x00800000

/*
 * note: the full rtp_session_config, rtp_codec, rtp_jib_config, etc. structs
 * are very large and their exact layout needs hardware validation.
 * they will be added once we can test on the device.
 *
 * in the meantime, the raw voice.h from tools_old/gs_tapi_tools/drv/coma_voice.h
 * can be referenced for the complete struct definitions.
 */

#endif /* COMATOSE_VOICE_DEFS_H */
