/*
 * comatose/voice_defs.h - voice/RTP service definitions (header-only)
 *
 * message types, constants, and struct definitions for the COMA "voice" service.
 * structs must match the kernel's voice.h layout exactly (ARM32, packed).
 *
 * derived from the GPL kernel headers cmsg-voice.h and voice.h
 * (linux-4.9.0/drivers/staging/dspg/coma/).
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_VOICE_DEFS_H
#define COMATOSE_VOICE_DEFS_H

#include <stdint.h>
#include <sys/ioctl.h>

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
 * RTP audio mode
 */
#define RTP_MODE_SEND_ONLY  1
#define RTP_MODE_REC_ONLY   2
#define RTP_MODE_ACTIVE     3
#define RTP_MODE_INACTIVE   4

/*
 * RTP call mode
 */
#define RTP_APP_VOIP_USER    0
#define RTP_APP_VOIP_KERNEL  1
#define RTP_APP_RTPSTACK_APP 2
#define RTP_APP_VOIP_APP     3

/*
 * CFIFO packet type tags
 */
#define CFIFO_RTP_PACKET   1
#define CFIFO_RTCP_PACKET  2

/*
 * struct definitions — layout must match kernel voice.h exactly.
 * the kernel header uses #pragma pack(1).
 */
#pragma pack(push, 1)

#define VOICE_MAX_CODECS         6
#define VOICE_MAX_DYN_CODEC_LEN  15
#define VOICE_SRTP_KEY_LEN       48
#define VOICE_SRTP_MKI_LEN       256
#define VOICE_MAX_REDUNDANT_CODEC 3

/* comfort noise options */
typedef struct {
	int mode_rx;
	int mode_tx;
	int max_sid_update;
	int vad_detect_level;
	int vad_hangover;
	int level_rx;
	int level_tx;
} rtp_cng_opts;

/* rx codec list entry */
typedef struct {
	char rx_pt;
	char CodecStr[VOICE_MAX_DYN_CODEC_LEN];
} rx_pt_list;

/* VBR codec params (union placeholder — largest member is 52 bytes) */
typedef union {
	int u8PlaceHolderArray[32];
} vbr_codec;

/* session event config */
typedef struct {
	unsigned short report_ssrc_change;
	unsigned short report_codec_payload_change;
} rtp_session_event_config;

/* codec configuration */
typedef struct {
	int duration;
	int opts;
	int Timestamp;
	int ssrc;
	int rx_pt_event;
	int tx_pt;
	int tx_pt_event;
	rtp_cng_opts cng;
	char CodecStr[VOICE_MAX_DYN_CODEC_LEN + 1];
	rx_pt_list rx_list[VOICE_MAX_CODECS];
	vbr_codec vbrCodecParam;
} rtp_codec;

/* jitter buffer config */
typedef struct {
	int max_len;
	int min_len;
	int type;
	int Th_resync;
	int target_delay;
	int monitoring_interval;
	int stepsize_reset_time;
	int slope;
	int pos_adapt_step_size;
	int pos_adapt_step_size_m;
} rtp_jib_config;

/* T.38 config */
typedef struct {
	unsigned int lsRedundancy;
	unsigned int hsRedundancy;
	unsigned int ecnOn;
} t38_config;

/* SRTP config */
typedef struct {
	unsigned int opts;
	unsigned char key_loc[VOICE_SRTP_KEY_LEN];
	unsigned char key_dist[VOICE_SRTP_KEY_LEN];
	unsigned int mki_length;
	unsigned char mki_value[VOICE_SRTP_MKI_LEN];
} srtp_session_config;

/* RTP redundancy config (RFC 2198) */
typedef struct {
	unsigned int rtp_redundancy_mode;
	unsigned int rtp_redundancy_level;
	unsigned int rtp_redundant_tx_ptype_audio;
	unsigned int rtp_redundant_rx_ptype_audio;
	unsigned int rtp_redundant_tx_ptype_dtmf;
	unsigned int rtp_redundant_rx_ptype_dtmf;
	rx_pt_list rtp_redundant_codec_list[VOICE_MAX_REDUNDANT_CODEC];
} rtp_redundancy_config;

/* packet loss adaptation config */
typedef struct {
	unsigned int packet_loss_increase_threshold;
	unsigned int packet_loss_decrease_threshold;
	unsigned int positive_adaptation_rate;
	unsigned int negative_adaptation_rate;
	unsigned int max_adaptation_limit;
} rtp_pkt_loss_detection_adaptation_config;

/* RTCP session config */
typedef struct {
	int rtcp_interval;
	unsigned int opts;
	char sdesItem[8][50];
	unsigned int rtcpFbType;
	unsigned int fb_bw;
	unsigned int fb_trr_interval;
	rtp_pkt_loss_detection_adaptation_config adaptation_config;
	unsigned int max_rtt;
	unsigned int gmin;
	unsigned int thinning;
	unsigned int rbType;
	srtp_session_config srtp_config;
} rtcp_session_config;

/*
 * rtp_session_config — the big one.
 * passed to VOICE_IOCSETCODEC to configure and start a voice session.
 */
typedef struct {
	unsigned int dtmf2833numEndPackets;
	int opts;
	unsigned int SymmRTPTxPktCnt;
	unsigned int current_time;
	int audio_mode;
	int media_loop_level;
	int rtcp_mux;
	int lib_rtp_mode;
	rtp_jib_config jib_config;
	t38_config t38_cfg;
	rtp_codec codec;
	int sid_update;
	unsigned int rtpRetransmissionMode;
	unsigned int rtp_retransmission_buffer_size;
	srtp_session_config srtp_config;
	rtp_redundancy_config redundancy_config;
	int voip_line_id;
	int session_id;
	rtp_session_event_config rtp_ses_event_config;
} rtp_session_config;

/*
 * CFIFO packet header — prepended to every packet in the enc/dec FIFOs.
 * on ARM32, unsigned long = 4 bytes.
 */
struct rtp_packet_header {
	uint32_t packetType;   /* CFIFO_RTP_PACKET or CFIFO_RTCP_PACKET */
	uint32_t ttl;
	uint32_t receiptTime;  /* milliseconds */
};

#pragma pack(pop)

/*
 * /dev/voiceN ioctl definitions
 */
#define VOICE_IOC_MAGIC  'V'

#define VOICE_IOCSETCODEC       _IOW(VOICE_IOC_MAGIC, 0, rtp_session_config *)
#define VOICE_IOCFLUSH          _IO (VOICE_IOC_MAGIC, 1)
#define VOICE_IOCSTOP_SESSION   _IO (VOICE_IOC_MAGIC, 8)

/* additional ioctls (not needed for basic voice_tap, but defined for completeness) */
#define VOICE_IOCSENDDTMF       _IOW(VOICE_IOC_MAGIC, 3, int *)
#define VOICE_IOCGETDTMF        _IOR(VOICE_IOC_MAGIC, 4, int *)
#define VOICE_IOCUPDATE_SESSION _IOW(VOICE_IOC_MAGIC, 6, rtp_session_config *)
#define VOICE_IOCGET_FIFO_INFO  _IOR(VOICE_IOC_MAGIC, 10, int *)
#define VOICE_IOCGET_LAST_CSS_ERROR _IOR(VOICE_IOC_MAGIC, 11, int *)

#endif /* COMATOSE_VOICE_DEFS_H */
