/*
 * comatose/coma.h - COMA socket transport API
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_COMA_H
#define COMATOSE_COMA_H

#include <comatose/types.h>
#include <sys/types.h>

/* AF_COMA protocol family, as defined by the DSPG COMA kernel module */
#define PF_COMA  43
#define AF_COMA  PF_COMA

/* maximum service name length (including null terminator) */
#define COMA_SERVICE_NAME_MAX  16

/* COMA socket address structure (mirrors kernel sockaddr_coma) */
struct sockaddr_coma {
	unsigned short family;
	char service[COMA_SERVICE_NAME_MAX];
};

/*
 * COMA message header (mirrors kernel struct cmsg)
 *
 * messages are variable-length: header followed by params then payload.
 * total message size = sizeof(header) + params_size + payload_size.
 */
struct cmsg {
	int type;
	unsigned int params_size;
	unsigned int payload_size;
	char body[];
};

#define CMSG_HEADER_SIZE  12  /* offsetof(struct cmsg, body) */

static inline void *cmsg_params(struct cmsg *msg) {
	return msg->body;
}

static inline void *cmsg_payload(struct cmsg *msg) {
	return msg->body + msg->params_size;
}

/* opaque connection handle */
typedef struct coma_conn coma_conn_t;

/* check if the CSS is loaded and not in panic state.
 * returns 1 if CSS is ready, 0 if not. */
int coma_css_ready(void);

/* connect to a named COMA service (e.g. "dua", "voice").
 * requires CAP_SYS_ADMIN or CAP_NET_ADMIN. */
coma_conn_t *coma_connect(const char *service);

/* disconnect and free. */
void coma_close(coma_conn_t *conn);

/* get the underlying fd for poll/select/epoll. */
int coma_fd(const coma_conn_t *conn);

/* send a raw message. returns bytes sent or -1 on error. */
ssize_t coma_send(coma_conn_t *conn, const void *msg, size_t len);

/* receive a raw message into buf. returns bytes received or -1 on error. */
ssize_t coma_recv(coma_conn_t *conn, void *buf, size_t buflen);

/* send a request and wait for a single response.
 * resp_len is in/out: on entry, buffer size; on exit, actual response size.
 * timeout_ms: 0 = non-blocking, -1 = infinite, >0 = milliseconds. */
comatose_result_t coma_transact(coma_conn_t *conn,
                                const void *req, size_t req_len,
                                void *resp_buf, size_t *resp_len,
                                int timeout_ms);

#endif /* COMATOSE_COMA_H */
