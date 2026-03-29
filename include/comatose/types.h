/*
 * comatose/types.h - common types, result codes, UID helpers
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_TYPES_H
#define COMATOSE_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* HT818 hardware limits */
#define COMATOSE_MAX_FXS_PORTS  8
#define COMATOSE_MAX_VOIP_CH    16
#define COMATOSE_MAX_CONNECTIONS 16

/* library result codes */
typedef enum {
	COMATOSE_OK = 0,
	COMATOSE_ERR_SOCKET,   /* socket/connect/send/recv failure (check errno) */
	COMATOSE_ERR_IOCTL,    /* ioctl failure (check errno) */
	COMATOSE_ERR_TIMEOUT,  /* response timeout */
	COMATOSE_ERR_PROTOCOL, /* unexpected response format */
	COMATOSE_ERR_DUA,      /* DUA returned an error code */
	COMATOSE_ERR_INVALID,  /* invalid argument */
	COMATOSE_ERR_NOMEM,    /* allocation failure */
} comatose_result_t;

/* DUA unit ID - encodes unit type and instance index */
typedef int16_t dua_uid_t;

/* DUA connection ID */
typedef int32_t dua_conn_t;

/*
 * UID encoding: (unit_type << 8) | instance_index
 *
 * unit types:
 *   0 = UT_SPVOIPNDA (VoIP DSP), 16 instances -> UIDs 0x0000-0x000F
 *   1 = UT_TRACE,                  3 instances -> UIDs 0x0100-0x0102
 *   2 = UT_FXS (SLIC),            8 instances -> UIDs 0x0200-0x0207
 */
#define DUA_UID_MAKE(type, idx)  ((dua_uid_t)(((type) << 8) | ((idx) & 0xFF)))
#define DUA_UID_TYPE(uid)        (((uid) >> 8) & 0xFF)
#define DUA_UID_INDEX(uid)       ((uid) & 0xFF)

/* convenience constructors for the two unit types we care about */
#define DUA_UID_FXS(port)        DUA_UID_MAKE(2, (port))
#define DUA_UID_VOIP(ch)         DUA_UID_MAKE(0, (ch))

#endif /* COMATOSE_TYPES_H */
