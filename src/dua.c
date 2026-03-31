/*
 * dua.c - DUA protocol session implementation
 *
 * the DUA service sends two types of messages:
 *   cmd=0x81: synchronous response matched by sender_id
 *   cmd=0x7f: async callback (event notifications)
 *
 * we use sender_id to correlate requests with responses, and drain
 * any async callbacks that arrive between send and response.
 */

#include <comatose/dua.h>
#include <comatose/coma.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DUA_DEFAULT_TIMEOUT_MS  5000
#define DUA_RESP_BUF_SIZE       1024

#define DUA_RESP_CMD_SYNC   0x81
#define DUA_RESP_CMD_ASYNC  0x7f

struct dua_session {
	coma_conn_t *conn;
	uint32_t next_sender_id;
	uint8_t resp_buf[DUA_RESP_BUF_SIZE];
	size_t resp_len;
	int32_t last_dua_error;
	/* async callback result from last completed operation */
	int32_t last_async_result;
	dua_uid_t last_async_uid;
	int32_t last_async_elem;
	/* shared memory state */
	int shm_fd;
	void *shm_ptr;
	size_t shm_size;
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
	sess->shm_fd = -1;
	sess->shm_ptr = MAP_FAILED;
	return sess;
}

void dua_close(dua_session_t *sess)
{
	if (!sess)
		return;
	coma_close(sess->conn);
	if (sess->shm_ptr != MAP_FAILED)
		munmap(sess->shm_ptr, sess->shm_size);
	if (sess->shm_fd >= 0)
		close(sess->shm_fd);
	free(sess);
}

int dua_fd(const dua_session_t *sess)
{
	return sess ? coma_fd(sess->conn) : -1;
}

int32_t dua_last_error(const dua_session_t *sess)
{
	return sess ? sess->last_dua_error : 0;
}

const uint8_t *dua_resp_buf(const dua_session_t *sess)
{
	return sess ? sess->resp_buf : NULL;
}

size_t dua_resp_len(const dua_session_t *sess)
{
	return sess ? sess->resp_len : 0;
}

int32_t dua_last_async_result(const dua_session_t *sess)
{
	return sess ? sess->last_async_result : 0;
}

int32_t dua_last_async_elem(const dua_session_t *sess)
{
	return sess ? sess->last_async_elem : 0;
}

void *dua_shm_ptr(const dua_session_t *sess)
{
	return sess ? sess->shm_ptr : NULL;
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
 * async callback decoding
 *
 * the CSS sends async callbacks (cmd=0x7f) with 4 params:
 *   p[0] = 0xdeadbeef (our callback reference from InitReq)
 *   p[1] = uid
 *   p[2] = elem
 *   p[3] = result * 0x100 + response_type
 *
 * response_type: 1=connect, 2=set/allocate/free, 3=init
 * result: >= 0 success (e.g. 0x0b=SET_IND, 0x09=ALLOCATE_IND), < 0 DUA error
 */
static int32_t dua_decode_async_result(uint32_t p3)
{
	return (int32_t)(p3 & 0xFFFFFF00u) >> 8;
}

static void dua_log_async(const struct dua_msg *resp)
{
	if (resp->num_params >= 4) {
		int32_t result = dua_decode_async_result(resp->params[3]);
		fprintf(stderr, "  [async] uid=0x%x elem=%d result=%d (p3=0x%x)\n",
		        resp->params[1], (int32_t)resp->params[2],
		        result, resp->params[3]);
	}
}

/*
 * receive one message from the CSS, store in session resp_buf.
 * returns the cmd field, or -1 on error/timeout.
 */
static int dua_recv_one(dua_session_t *sess, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = coma_fd(sess->conn),
		.events = POLLIN,
	};

	int ret = poll(&pfd, 1, timeout_ms);
	if (ret <= 0)
		return -1;

	ssize_t n = coma_recv(sess->conn, sess->resp_buf, DUA_RESP_BUF_SIZE);
	if (n < 0)
		return -1;
	sess->resp_len = (size_t)n;

	if (sess->resp_len < DUA_MSG_HEADER_SIZE)
		return -1;

	return (int)((struct dua_msg *)sess->resp_buf)->cmd;
}

/*
 * request/reply engine
 *
 * sends a DUA message and waits for BOTH:
 *   1. the sync response (cmd=0x81) confirming receipt
 *   2. the async callback (cmd=0x7f) with the actual operation result
 *
 * match_uid: uid to match in async callback p[1], or -1 to skip async wait.
 *
 * this unified loop prevents the race where the async callback arrives
 * during the sync wait and gets discarded.
 */
/*
 * async callback response types (low byte of p[3])
 */
#define DUA_ASYNC_TYPE_CONNECT  1   /* UnitConnect/Disconnect, ConnCreate/Delete/Merge */
#define DUA_ASYNC_TYPE_SET      2   /* UnitSet, UnitAllocate, UnitFree */
#define DUA_ASYNC_TYPE_INIT     3   /* InitReq, ApplInit */

static comatose_result_t dua_send_and_wait(dua_session_t *sess,
                                           const void *msg, size_t msg_len,
                                           uint32_t expected_sender_id,
                                           int32_t match_uid,
                                           int match_type,
                                           int timeout_ms)
{
	if (timeout_ms < 0)
		timeout_ms = DUA_DEFAULT_TIMEOUT_MS;

	ssize_t sent = coma_send(sess->conn, msg, msg_len);
	if (sent < 0)
		return COMATOSE_ERR_SOCKET;

	int got_sync = 0;
	int got_async = (match_uid == -1); /* skip async if no uid to match */
	comatose_result_t async_result = COMATOSE_OK;

	for (int attempts = 0; attempts < 64; attempts++) {
		int cmd = dua_recv_one(sess, timeout_ms);
		if (cmd < 0)
			return got_sync ? COMATOSE_ERR_TIMEOUT : COMATOSE_ERR_SOCKET;

		struct dua_msg *resp = (struct dua_msg *)sess->resp_buf;

		if (cmd == DUA_RESP_CMD_SYNC) {
			if (resp->sender_id != expected_sender_id)
				continue; /* stale */

			/* check for immediate rejection */
			if (resp->num_params >= 1 &&
			    sess->resp_len >= DUA_MSG_HEADER_SIZE + 4) {
				int32_t result = (int32_t)resp->params[0];
				if (result < 0) {
					sess->last_dua_error = result;
					return COMATOSE_ERR_DUA;
				}
			}
			got_sync = 1;
			if (got_async)
				return async_result;
			continue;
		}

		if (cmd == DUA_RESP_CMD_ASYNC && resp->num_params >= 4 &&
		    resp->params[0] == 0xdeadbeef) {
			dua_log_async(resp);

			int32_t cb_uid = (int32_t)resp->params[1];
			int cb_type = (int)(resp->params[3] & 0xFF);

			/* skip warnings (result >= 0x1000) and param events —
			 * they share type codes with real completions but aren't
			 * the operation result we're waiting for */
			int32_t result = dua_decode_async_result(resp->params[3]);
			if (!got_async && cb_uid == match_uid &&
			    (match_type < 0 || cb_type == match_type) &&
			    (result < 0 || result < 0x1000)) {
				sess->last_async_result = result;
				sess->last_async_uid = (dua_uid_t)resp->params[1];
				sess->last_async_elem = (int32_t)resp->params[2];
				sess->last_dua_error = result < 0 ? result : 0;
				async_result = result >= 0 ? COMATOSE_OK : COMATOSE_ERR_DUA;
				got_async = 1;
				if (got_sync)
					return async_result;
			}
			continue;
		}
	}

	return COMATOSE_ERR_TIMEOUT;
}

/* get a param from the last response */
static uint32_t dua_resp_param(const dua_session_t *sess, unsigned int idx)
{
	const struct dua_msg *resp = (const struct dua_msg *)sess->resp_buf;
	size_t needed = DUA_MSG_HEADER_SIZE + (idx + 1) * sizeof(uint32_t);
	if (idx >= resp->num_params || sess->resp_len < needed)
		return 0;
	return resp->params[idx];
}

/*
 * helper: build and send a simple DUA command, wait for matched response
 */
static comatose_result_t dua_simple_cmd(dua_session_t *sess,
                                        uint32_t cmd, uint32_t num_params, ...)
{
	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint32_t sid = sess->next_sender_id++;

	size_t off = dua_msg_init(buf, sizeof(buf), sid, cmd, num_params);

	__builtin_va_list ap;
	__builtin_va_start(ap, num_params);
	for (uint32_t i = 0; i < num_params; i++)
		off = dua_msg_pack_u32(buf, off, __builtin_va_arg(ap, uint32_t));
	__builtin_va_end(ap);

	return dua_send_and_wait(sess, buf, off, sid, -1, -1, DUA_DEFAULT_TIMEOUT_MS);
}

/*
 * high-level commands
 */

#define SHAREDMEM_IOCTL_INIT  0xc0045302

comatose_result_t dua_init_hw(dua_session_t *sess)
{
	if (!sess)
		return COMATOSE_ERR_INVALID;

	/* open shared memory device */
	sess->shm_fd = open("/dev/sharedmem", O_RDWR);
	if (sess->shm_fd < 0)
		return COMATOSE_ERR_IOCTL;

	/* trigger CMSG_SHAREDMEM_INIT on the CSS (sets up MMU mapping) */
	uint32_t shm_ioctl[4] = {0};
	if (ioctl(sess->shm_fd, SHAREDMEM_IOCTL_INIT, &shm_ioctl) < 0) {
		close(sess->shm_fd);
		sess->shm_fd = -1;
		return COMATOSE_ERR_IOCTL;
	}

	sess->shm_size = shm_ioctl[1];
	if (sess->shm_size == 0)
		sess->shm_size = 0x80000;

	/* mmap shared memory into our address space */
	sess->shm_ptr = mmap(NULL, sess->shm_size, PROT_READ | PROT_WRITE,
	                      MAP_SHARED, sess->shm_fd, 0);
	if (sess->shm_ptr == MAP_FAILED) {
		close(sess->shm_fd);
		sess->shm_fd = -1;
		return COMATOSE_ERR_IOCTL;
	}

	/* zero and set up header (matches app_dsp behavior) */
	memset(sess->shm_ptr, 0, sess->shm_size);
	*(uint32_t *)((char *)sess->shm_ptr + 4) = (uint32_t)sess->shm_size;
	*(uint32_t *)((char *)sess->shm_ptr + 8) = 0x30;
	__sync_synchronize();

	/* wait for CSS to finish processing the sharedmem init */
	usleep(100000);

	/*
	 * send DUA_CMD_INIT_REQ (cmd 0x01) with 5 params:
	 *   params[0] = 5          (version/type marker)
	 *   params[1] = 0xffffffff
	 *   params[2] = 0xffffffff
	 *   params[3] = 0xdeadbeef (callback reference)
	 *   params[4] = shared_mem_ptr
	 */
	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint32_t sid = sess->next_sender_id++;
	size_t off = dua_msg_init(buf, sizeof(buf), sid, DUA_CMD_INIT_REQ, 5);
	off = dua_msg_pack_u32(buf, off, 5);
	off = dua_msg_pack_u32(buf, off, 0xffffffff);
	off = dua_msg_pack_u32(buf, off, 0xffffffff);
	off = dua_msg_pack_u32(buf, off, 0xdeadbeef);
	off = dua_msg_pack_u32(buf, off, (uint32_t)(uintptr_t)sess->shm_ptr);

	return dua_send_and_wait(sess, buf, off, sid, -1, -1, DUA_DEFAULT_TIMEOUT_MS);
}

comatose_result_t dua_appl_init(dua_session_t *sess)
{
	comatose_result_t ret = dua_simple_cmd(sess, DUA_CMD_APPL_INIT, 0);
	if (ret != COMATOSE_OK)
		return ret;

	/* drain async callbacks from InitReq/ApplInit.
	 * these arrive asynchronously and will confuse the uid-matching
	 * in dua_send_and_wait if they're still queued. */
	for (int i = 0; i < 16; i++) {
		int cmd = dua_recv_one(sess, 500);
		if (cmd < 0)
			break; /* no more pending messages */
		struct dua_msg *resp = (struct dua_msg *)sess->resp_buf;
		if (cmd == DUA_RESP_CMD_ASYNC)
			dua_log_async(resp);
	}
	fprintf(stderr, "  (drained init callbacks)\n");

	return COMATOSE_OK;
}

comatose_result_t dua_unit_allocate(dua_session_t *sess,
                                    enum dua_unit_type type,
                                    int instance_spec,
                                    dua_uid_t *out_uid)
{
	if (!sess || !out_uid)
		return COMATOSE_ERR_INVALID;

	dua_uid_t expected = DUA_UID_MAKE(type, instance_spec >= 0 ? instance_spec : 0);

	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint32_t sid = sess->next_sender_id++;
	size_t off = dua_msg_init(buf, sizeof(buf), sid, DUA_CMD_UNIT_ALLOCATE, 2);
	off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)type);
	off = dua_msg_pack_u32(buf, off, (uint32_t)instance_spec);

	/* wait for both sync response AND the UNITALLOCATE_IND async callback */
	comatose_result_t ret = dua_send_and_wait(sess, buf, off, sid,
	                                          (int32_t)expected,
	                                          DUA_ASYNC_TYPE_SET,
	                                          DUA_DEFAULT_TIMEOUT_MS);

	if (ret == COMATOSE_OK)
		*out_uid = sess->last_async_uid;
	else if (instance_spec >= 0)
		*out_uid = DUA_UID_MAKE(type, instance_spec);
	else
		*out_uid = DUA_UID_MAKE(type, 0);

	return ret;
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
	 * DUA UnitSet wire format:
	 *   num_params=4, params={uid,elem,param,value} — when data fits in a uint32
	 *   num_params=3, params={uid,elem,param}, then length-prefixed data blob:
	 *     [pad to 4-byte alignment] [uint32: data_size] [data bytes] [pad]
	 *
	 * the length-prefixed format was discovered from libcordless's
	 * dua_send_message → FUN_0002462c packing function.
	 */
	uint8_t buf[1024];
	uint32_t sid = sess->next_sender_id++;
	size_t off;

	if (data_len <= sizeof(uint32_t) && data) {
		uint32_t val = 0;
		memcpy(&val, data, data_len);
		off = dua_msg_init(buf, sizeof(buf), sid, DUA_CMD_UNIT_SET, 4);
		off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
		off = dua_msg_pack_u32(buf, off, (uint32_t)elem);
		off = dua_msg_pack_u32(buf, off, (uint32_t)param);
		off = dua_msg_pack_u32(buf, off, val);
	} else {
		off = dua_msg_init(buf, sizeof(buf), sid, DUA_CMD_UNIT_SET, 3);
		off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
		off = dua_msg_pack_u32(buf, off, (uint32_t)elem);
		off = dua_msg_pack_u32(buf, off, (uint32_t)param);
		if (data && data_len > 0) {
			/* align to 4 bytes */
			size_t pad = (4 - (off & 3)) & 3;
			memset(buf + off, 0, pad);
			off += pad;
			/* length prefix */
			off = dua_msg_pack_u32(buf, off, (uint32_t)data_len);
			/* data */
			if (off + data_len > sizeof(buf))
				return COMATOSE_ERR_INVALID;
			memcpy(buf + off, data, data_len);
			off += data_len;
			/* pad to 4 bytes */
			pad = (4 - (off & 3)) & 3;
			memset(buf + off, 0, pad);
			off += pad;
		}
	}

	/* debug: dump wire bytes for large-blob UnitSet */
	if (data_len > 4) {
		fprintf(stderr, "dua_unit_set: uid=0x%04x elem=%d param=0x%x data_len=%zu off=%zu sid=%u\n",
		        (unsigned)uid, elem, param, data_len, off, sid);
		fprintf(stderr, "  msg[0..31]:");
		for (size_t i = 0; i < 32 && i < off; i++)
			fprintf(stderr, " %02x", buf[i]);
		fprintf(stderr, "\n  blob[0..15]:");
		if (data) {
			const uint8_t *d = data;
			for (size_t i = 0; i < 16 && i < data_len; i++)
				fprintf(stderr, " %02x", d[i]);
		}
		fprintf(stderr, "\n");
	}

	return dua_send_and_wait(sess, buf, off, sid,
	                        (int32_t)(int16_t)uid,
	                        DUA_ASYNC_TYPE_SET,
	                        DUA_DEFAULT_TIMEOUT_MS);
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
	if (ret != COMATOSE_OK) {
		*data_len = 0;
		return ret;
	}

	const struct dua_msg *resp = (const struct dua_msg *)sess->resp_buf;
	if (resp->num_params >= 2) {
		size_t copy = (resp->num_params - 1) * sizeof(uint32_t);
		if (copy > *data_len)
			copy = *data_len;
		memcpy(data, &resp->params[1], copy);
		*data_len = copy;
	} else {
		*data_len = 0;
	}

	return COMATOSE_OK;
}

comatose_result_t dua_conn_create(dua_session_t *sess, dua_conn_t *out_conn)
{
	if (!sess || !out_conn)
		return COMATOSE_ERR_INVALID;

	comatose_result_t ret = dua_simple_cmd(sess, DUA_CMD_CONN_CREATE, 0);
	if (ret != COMATOSE_OK) {
		*out_conn = -1;
		return ret;
	}

	*out_conn = (dua_conn_t)dua_resp_param(sess, 0);
	return COMATOSE_OK;
}

comatose_result_t dua_conn_delete(dua_session_t *sess, dua_conn_t conn)
{
	return dua_simple_cmd(sess, DUA_CMD_CONN_DELETE, 1, (uint32_t)conn);
}

comatose_result_t dua_unit_connect(dua_session_t *sess,
                                   dua_uid_t uid, dua_conn_t conn)
{
	uint8_t buf[DUA_MSG_BUF_SIZE];
	uint32_t sid = sess->next_sender_id++;
	size_t off = dua_msg_init(buf, sizeof(buf), sid, DUA_CMD_UNIT_CONNECT, 2);
	off = dua_msg_pack_u32(buf, off, (uint32_t)(int16_t)uid);
	off = dua_msg_pack_u32(buf, off, (uint32_t)conn);

	/* the async callback elem field contains the assigned conn_id,
	 * readable via sess->last_async_elem after return. */
	return dua_send_and_wait(sess, buf, off, sid,
	                         (int32_t)(int16_t)uid,
	                         DUA_ASYNC_TYPE_CONNECT,
	                         DUA_DEFAULT_TIMEOUT_MS);
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
	/* stock firmware uses elem=-2 for UMT mode execution.
	 * elem is not used by GenSetFunc for UMT dispatch, but we match
	 * the stock behavior exactly (elem=-2 = SETTOG). */
	return dua_unit_set(sess, uid, -2, DUA_PARAM_UMT_EXEC_GEN,
	                    &mode, sizeof(mode));
}

/*
 * TDM channel assignment via UMT_IMMEDIATE bytecode
 *
 * builds and sends a UMT bytecode program that maps TDM timeslots to
 * CSS DSP FIFOs. the bytecode uses opcode 23 (EXEC_FUNC) to call
 * CSS functions: clearTDMAssignment, startTDMAssignment,
 * assignTDMChannelToFIFO, commitTDMAssignment.
 *
 * FIFO numbering (from RE of stock firmware):
 *   RX FIFO for channel N = 0x080d + N * 0x12
 *   TX FIFO for channel N = 0x0810 + N * 0x12
 */

/* UMT bytecode instruction header: opcode in bits[4:0], elem in bits[15:5] */
static inline uint16_t umt_hdr(int opcode, int elem)
{
	return (uint16_t)(((elem & 0x7ff) << 5) | (opcode & 0x1f));
}

/* emit opcode 23 (EXEC_FUNC): 2-byte header + 3x u32 params = 14 bytes */
static uint8_t *emit_exec_func(uint8_t *p, int elem,
                               uint32_t p0, uint32_t p1, uint32_t p2)
{
	*(uint16_t *)p = umt_hdr(23, elem); p += 2;
	*(uint32_t *)p = p0; p += 4;
	*(uint32_t *)p = p1; p += 4;
	*(uint32_t *)p = p2; p += 4;
	return p;
}

#define UMOP_ASSIGN_FIFO   0
#define UMOP_CLEAR_TDM     3
#define UMOP_COMMIT_TDM    4
#define UMOP_START_TDM     5
#define TDM_DIR_RX         1
#define TDM_DIR_TX         2
#define TDM_RX_FIFO_BASE   0x080d
#define TDM_TX_FIFO_BASE   0x0810
#define TDM_FIFO_STRIDE    0x12

comatose_result_t dua_set_tdm_assignment(dua_session_t *sess,
										 dua_uid_t uid,
                                         int tdm_id, int num_channels)
{
	if (!sess || num_channels < 0 || num_channels > 16)
		return COMATOSE_ERR_INVALID;

	/* max blob size: 2 + 14*(2 + 2*num_channels + 1) + 2
	 * for 8 channels: 14*(2+16+1) + 2 = 268 bytes */
	uint8_t blob[512];
	uint8_t *p = blob;

	p = emit_exec_func(p, UMOP_CLEAR_TDM, (uint32_t)tdm_id, 0, 0);
	p = emit_exec_func(p, UMOP_START_TDM, 0, 0, 0);

	for (int ch = 0; ch < num_channels; ch++) {
		uint16_t tx_fifo = TDM_TX_FIFO_BASE + ch * TDM_FIFO_STRIDE;
		uint16_t rx_fifo = TDM_RX_FIFO_BASE + ch * TDM_FIFO_STRIDE;

		p = emit_exec_func(p, UMOP_ASSIGN_FIFO, (uint32_t)tdm_id, (uint32_t)ch,
		                   (TDM_DIR_TX << 16) | tx_fifo);
		p = emit_exec_func(p, UMOP_ASSIGN_FIFO, (uint32_t)tdm_id, (uint32_t)ch,
		                   (TDM_DIR_RX << 16) | rx_fifo);
	}

	p = emit_exec_func(p, UMOP_COMMIT_TDM, 0, 0, 0);
	*(uint16_t *)p = umt_hdr(31, 0); p += 2; /* END */

	size_t blob_size = (size_t)(p - blob);

	return dua_unit_set(sess, uid, -2, DUA_PARAM_UMT_IMMEDIATE,
	                    blob, blob_size);
}
