/*
 * dua.c - DUA protocol session implementation
 */

#include <comatose/dua.h>
#include <comatose/coma.h>

#include <stdlib.h>
#include <string.h>

#define DUA_DEFAULT_TIMEOUT_MS  3000
#define DUA_RESP_BUF_SIZE       1024

struct dua_session {
	coma_conn_t *conn;
	uint32_t next_sender_id;
	uint8_t resp_buf[DUA_RESP_BUF_SIZE];
};

/*
 * session lifecycle
 */

dua_session_t *dua_open(void)
{
	coma_conn_t *conn = coma_connect("dua");
	if (!conn)
		return NULL;

	dua_session_t *sess = calloc(1, sizeof(*sess));
	if (!sess) {
		coma_close(conn);
		return NULL;
	}

	sess->conn = conn;
	sess->next_sender_id = 1;
	return sess;
}

void dua_close(dua_session_t *sess)
{
	if (!sess)
		return;
	coma_close(sess->conn);
	free(sess);
}

int dua_fd(const dua_session_t *sess)
{
	return sess ? coma_fd(sess->conn) : -1;
}

/*
 * low-level message building
 */

size_t dua_msg_init(void *buf, size_t buflen,
                    uint32_t sender_id, uint32_t cmd, uint32_t num_params)
{
	if (buflen < DUA_MSG_HEADER_SIZE)
		return 0;

	struct dua_msg *msg = buf;
	msg->sender_id = sender_id;
	msg->flags = 0;
	msg->cmd = cmd;
	msg->num_params = num_params;
	return DUA_MSG_HEADER_SIZE;
}

size_t dua_msg_pack_u32(void *buf, size_t offset, uint32_t val)
{
	uint32_t *p = (uint32_t *)((uint8_t *)buf + offset);
	*p = val;
	return offset + sizeof(uint32_t);
}

comatose_result_t dua_transact(dua_session_t *sess,
                               const void *req, size_t req_len,
                               void *resp, size_t *resp_len,
                               int timeout_ms)
{
	if (!sess)
		return COMATOSE_ERR_INVALID;
	if (timeout_ms < 0)
		timeout_ms = DUA_DEFAULT_TIMEOUT_MS;
	return coma_transact(sess->conn, req, req_len, resp, resp_len, timeout_ms);
}

/*
 * helper: build and send a simple DUA command with uint32 params
 */
static comatose_result_t dua_simple_cmd(dua_session_t *sess,
                                        uint32_t cmd, uint32_t num_params, ...)
{
	uint8_t buf[DUA_MSG_BUF_SIZE];
	size_t off = dua_msg_init(buf, sizeof(buf),
	                          sess->next_sender_id++, cmd, num_params);

	__builtin_va_list ap;
	__builtin_va_start(ap, num_params);
	for (uint32_t i = 0; i < num_params; i++)
		off = dua_msg_pack_u32(buf, off, __builtin_va_arg(ap, uint32_t));
	__builtin_va_end(ap);

	size_t resp_len = DUA_RESP_BUF_SIZE;
	comatose_result_t ret = dua_transact(sess, buf, off,
	                                     sess->resp_buf, &resp_len, -1);
	/*
	 * TODO: parse response buffer to extract DUA result code.
	 * the exact response format needs hardware validation.
	 * for now we just return the transport-level result.
	 */
	return ret;
}

/*
 * high-level commands
 */

comatose_result_t dua_appl_init(dua_session_t *sess)
{
	return dua_simple_cmd(sess, DUA_CMD_APPL_INIT, 0);
}

comatose_result_t dua_unit_allocate(dua_session_t *sess,
                                    enum dua_unit_type type,
                                    int instance_spec,
                                    dua_uid_t *out_uid)
{
	if (!sess || !out_uid)
		return COMATOSE_ERR_INVALID;

	comatose_result_t ret = dua_simple_cmd(sess, DUA_CMD_UNIT_ALLOCATE, 2,
	                                       (uint32_t)(int16_t)type,
	                                       (uint32_t)instance_spec);
	if (ret != COMATOSE_OK)
		return ret;

	/*
	 * TODO: extract allocated UID from response.
	 * for now, construct the expected UID from type + spec.
	 * this is a placeholder until we can validate on hardware.
	 */
	if (instance_spec >= 0)
		*out_uid = DUA_UID_MAKE(type, instance_spec);
	else
		*out_uid = DUA_UID_MAKE(type, 0); /* placeholder */

	return COMATOSE_OK;
}

comatose_result_t dua_unit_free(dua_session_t *sess, dua_uid_t uid)
{
	return dua_simple_cmd(sess, DUA_CMD_UNIT_FREE, 1,
	                      (uint32_t)(int16_t)uid);
}

comatose_result_t dua_unit_set(dua_session_t *sess,
                               dua_uid_t uid, int elem,
                               int param,
                               const void *data, size_t data_len)
{
	if (!sess)
		return COMATOSE_ERR_INVALID;

	/*
	 * unit set message layout:
	 *   params[0] = uid (as int16 sign-extended to uint32)
	 *   params[1] = elem
	 *   params[2] = param code
	 * followed by data payload (format depends on num_params field)
	 *
	 * when num_params == 4 and data fits in a uint32, the value
	 * goes in params[3]. otherwise num_params == 3 and data follows
	 * as a variable-length blob.
	 */
	uint8_t buf[DUA_MSG_BUF_SIZE];
	size_t off;

	if (data_len <= sizeof(uint32_t) && data) {
		/* inline the value as params[3] */
		uint32_t val = 0;
		memcpy(&val, data, data_len);
		off = dua_msg_init(buf, sizeof(buf),
		                   sess->next_sender_id++, DUA_CMD_UNIT_SET, 4);
		off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
		off = dua_msg_pack_u32(buf, off, (uint32_t)elem);
		off = dua_msg_pack_u32(buf, off, (uint32_t)param);
		off = dua_msg_pack_u32(buf, off, val);
	} else {
		/* data follows as variable-length payload */
		off = dua_msg_init(buf, sizeof(buf),
		                   sess->next_sender_id++, DUA_CMD_UNIT_SET, 3);
		off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
		off = dua_msg_pack_u32(buf, off, (uint32_t)elem);
		off = dua_msg_pack_u32(buf, off, (uint32_t)param);
		if (data && data_len > 0 && off + data_len <= sizeof(buf)) {
			memcpy(buf + off, data, data_len);
			off += data_len;
		}
	}

	size_t resp_len = DUA_RESP_BUF_SIZE;
	return dua_transact(sess, buf, off, sess->resp_buf, &resp_len, -1);
}

comatose_result_t dua_unit_get(dua_session_t *sess,
                               dua_uid_t uid, int elem,
                               int param,
                               void *data, size_t *data_len)
{
	if (!sess || !data || !data_len)
		return COMATOSE_ERR_INVALID;

	comatose_result_t ret = dua_simple_cmd(sess, DUA_CMD_UNIT_GET, 3,
	                                       (uint32_t)(int16_t)uid,
	                                       (uint32_t)elem,
	                                       (uint32_t)param);
	/*
	 * TODO: extract response data from sess->resp_buf.
	 * needs hardware validation to determine response layout.
	 */
	(void)data;
	*data_len = 0;
	return ret;
}

comatose_result_t dua_conn_create(dua_session_t *sess, dua_conn_t *out_conn)
{
	if (!sess || !out_conn)
		return COMATOSE_ERR_INVALID;

	comatose_result_t ret = dua_simple_cmd(sess, DUA_CMD_CONN_CREATE, 0);
	/* TODO: extract connId from response */
	*out_conn = -1;
	return ret;
}

comatose_result_t dua_conn_delete(dua_session_t *sess, dua_conn_t conn)
{
	return dua_simple_cmd(sess, DUA_CMD_CONN_DELETE, 1, (uint32_t)conn);
}

comatose_result_t dua_unit_connect(dua_session_t *sess,
                                   dua_uid_t uid, dua_conn_t conn)
{
	return dua_simple_cmd(sess, DUA_CMD_UNIT_CONNECT, 2,
	                      (uint32_t)(int16_t)uid, (uint32_t)conn);
}

comatose_result_t dua_unit_disconnect(dua_session_t *sess,
                                      dua_uid_t uid, dua_conn_t conn)
{
	return dua_simple_cmd(sess, DUA_CMD_UNIT_DISCONNECT, 2,
	                      (uint32_t)(int16_t)uid, (uint32_t)conn);
}

comatose_result_t dua_conn_merge(dua_session_t *sess,
                                 dua_conn_t conn1, dua_conn_t conn2)
{
	return dua_simple_cmd(sess, DUA_CMD_CONN_MERGE, 2,
	                      (uint32_t)conn1, (uint32_t)conn2);
}

comatose_result_t dua_conn_unmerge(dua_session_t *sess, dua_conn_t conn)
{
	return dua_simple_cmd(sess, DUA_CMD_CONN_UNMERGE, 1, (uint32_t)conn);
}

comatose_result_t dua_set_umt_mode(dua_session_t *sess,
                                   dua_uid_t uid, uint32_t mode)
{
	return dua_unit_set(sess, uid, -1, DUA_PARAM_UMT_EXEC_GEN,
	                    &mode, sizeof(mode));
}
