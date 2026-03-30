/*
 * coma.c - COMA socket transport implementation
 *
 * IMPORTANT: the COMA kernel socket has a quirk where recv() returns
 * EOPNOTSUPP but recvmsg() works fine. always use recvmsg() internally.
 */

#include <comatose/coma.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>

struct coma_conn {
	int fd;
	char service[COMA_SERVICE_NAME_MAX];
};

int coma_css_ready(void)
{
	FILE *f = fopen("/sys/devices/platform/8000000.css/state", "r");
	if (!f)
		return 0;
	char state[32] = {0};
	if (!fgets(state, sizeof(state), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	/* trim newline */
	state[strcspn(state, "\n")] = 0;
	return strcmp(state, "loaded") == 0;
}

coma_conn_t *coma_connect(const char *service)
{
	if (!service || strlen(service) >= COMA_SERVICE_NAME_MAX)
		return NULL;

	int fd = socket(PF_COMA, SOCK_SEQPACKET, 0);
	if (fd < 0) {
		fprintf(stderr, "coma: socket() failed: %s\n", strerror(errno));
		return NULL;
	}

	struct sockaddr_coma addr;
	memset(&addr, 0, sizeof(addr));
	addr.family = AF_COMA;
	strncpy(addr.service, service, COMA_SERVICE_NAME_MAX - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "coma: connect(\"%s\") failed: %s\n",
		        service, strerror(errno));
		close(fd);
		return NULL;
	}

	coma_conn_t *conn = calloc(1, sizeof(*conn));
	if (!conn) {
		close(fd);
		return NULL;
	}

	conn->fd = fd;
	strncpy(conn->service, service, COMA_SERVICE_NAME_MAX - 1);
	return conn;
}

void coma_close(coma_conn_t *conn)
{
	if (!conn)
		return;
	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
}

int coma_fd(const coma_conn_t *conn)
{
	return conn ? conn->fd : -1;
}

ssize_t coma_send(coma_conn_t *conn, const void *msg, size_t len)
{
	if (!conn || conn->fd < 0)
		return -1;
	return send(conn->fd, msg, len, 0);
}

ssize_t coma_recv(coma_conn_t *conn, void *buf, size_t buflen)
{
	if (!conn || conn->fd < 0)
		return -1;

	/*
	 * must use recvmsg() — the COMA kernel socket's recvmsg handler
	 * works correctly, but plain recv() hits an EOPNOTSUPP path.
	 */
	struct iovec iov = { .iov_base = buf, .iov_len = buflen };
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;

	return recvmsg(conn->fd, &mh, 0);
}

comatose_result_t coma_transact(coma_conn_t *conn,
                                const void *req, size_t req_len,
                                void *resp_buf, size_t *resp_len,
                                int timeout_ms)
{
	if (!conn || !req || !resp_buf || !resp_len)
		return COMATOSE_ERR_INVALID;

	ssize_t sent = coma_send(conn, req, req_len);
	if (sent < 0)
		return COMATOSE_ERR_SOCKET;

	struct pollfd pfd = {
		.fd = conn->fd,
		.events = POLLIN,
	};

	int ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0)
		return COMATOSE_ERR_SOCKET;
	if (ret == 0)
		return COMATOSE_ERR_TIMEOUT;

	ssize_t received = coma_recv(conn, resp_buf, *resp_len);
	if (received < 0)
		return COMATOSE_ERR_SOCKET;

	*resp_len = (size_t)received;
	return COMATOSE_OK;
}
