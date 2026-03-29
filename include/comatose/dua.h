/*
 * comatose/dua.h - DUA protocol session API
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_DUA_H
#define COMATOSE_DUA_H

#include <comatose/types.h>
#include <comatose/dua_defs.h>

/* opaque DUA session handle */
typedef struct dua_session dua_session_t;

/*
 * session lifecycle
 */

/* open a DUA session (connects AF_COMA socket to "dua" service). */
dua_session_t *dua_open(void);

/* close and free. */
void dua_close(dua_session_t *sess);

/* get fd for poll/select. */
int dua_fd(const dua_session_t *sess);

/*
 * initialization
 */

/* send ApplInit (cmd 0x13). must be called before other DUA operations. */
comatose_result_t dua_appl_init(dua_session_t *sess);

/*
 * unit management
 */

/* allocate a unit. instance_spec: -1 = auto-assign, >=0 = specific instance.
 * on success, *out_uid receives the allocated UID. */
comatose_result_t dua_unit_allocate(dua_session_t *sess,
                                    enum dua_unit_type type,
                                    int instance_spec,
                                    dua_uid_t *out_uid);

/* free a unit. */
comatose_result_t dua_unit_free(dua_session_t *sess, dua_uid_t uid);

/* set a unit parameter.
 * elem: element index (-1 for unit-level).
 * param: DUA_PARAM_* code.
 * data/data_len: parameter value payload. */
comatose_result_t dua_unit_set(dua_session_t *sess,
                               dua_uid_t uid, int elem,
                               int param,
                               const void *data, size_t data_len);

/* get a unit parameter.
 * data_len is in/out: on entry, buffer size; on exit, actual data size. */
comatose_result_t dua_unit_get(dua_session_t *sess,
                               dua_uid_t uid, int elem,
                               int param,
                               void *data, size_t *data_len);

/*
 * connection management
 */

/* create a new connection. on success, *out_conn receives the connection ID. */
comatose_result_t dua_conn_create(dua_session_t *sess, dua_conn_t *out_conn);

/* delete a connection. */
comatose_result_t dua_conn_delete(dua_session_t *sess, dua_conn_t conn);

/* connect a unit to a connection. */
comatose_result_t dua_unit_connect(dua_session_t *sess,
                                   dua_uid_t uid, dua_conn_t conn);

/* disconnect a unit from a connection. */
comatose_result_t dua_unit_disconnect(dua_session_t *sess,
                                      dua_uid_t uid, dua_conn_t conn);

/* merge two connections (conferencing). */
comatose_result_t dua_conn_merge(dua_session_t *sess,
                                 dua_conn_t conn1, dua_conn_t conn2);

/* unmerge a previously merged connection. */
comatose_result_t dua_conn_unmerge(dua_session_t *sess, dua_conn_t conn);

/*
 * convenience wrappers
 */

/* set UMT (DSP pipeline) mode on a unit.
 * shorthand for dua_unit_set(sess, uid, -1, DUA_PARAM_UMT_EXEC_GEN, &mode, 4). */
comatose_result_t dua_set_umt_mode(dua_session_t *sess,
                                   dua_uid_t uid, uint32_t mode);

/*
 * low-level message building (for exploration / discovery commands)
 */

/* max DUA message buffer size (header + generous param space) */
#define DUA_MSG_BUF_SIZE  256

/* initialize a DUA message in buf. returns offset past the header (16). */
size_t dua_msg_init(void *buf, size_t buflen,
                    uint32_t sender_id, uint32_t cmd, uint32_t num_params);

/* append a uint32 param at the given offset. returns new offset. */
size_t dua_msg_pack_u32(void *buf, size_t offset, uint32_t val);

/* send a raw DUA message and receive response.
 * resp_len is in/out. timeout_ms: -1 = default (3000ms). */
comatose_result_t dua_transact(dua_session_t *sess,
                               const void *req, size_t req_len,
                               void *resp, size_t *resp_len,
                               int timeout_ms);

#endif /* COMATOSE_DUA_H */
