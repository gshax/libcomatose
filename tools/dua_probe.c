/*
 * dua_probe - cautiously probe the DUA service with proper init sequence
 *
 * the correct initialization order (from app_dsp RE):
 *   1. open /dev/sharedmem, ioctl to init, mmap, zero, set header
 *   2. open COMA socket to "dua"
 *   3. send DUA_CMD_INIT_REQ (0x01) with shared memory pointer
 *   4. send DUA_CMD_APPL_INIT (0x13)
 *   5. now DUA is ready for unit operations
 */

#include <comatose/coma.h>
#include <comatose/dua_defs.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

static void hexdump(const char *label, const void *data, size_t len)
{
	const uint8_t *p = data;
	fprintf(stderr, "%s (%zu bytes):\n  ", label, len);
	for (size_t i = 0; i < len && i < 128; i++) {
		fprintf(stderr, "%02x ", p[i]);
		if ((i & 15) == 15 && i + 1 < len)
			fprintf(stderr, "\n  ");
	}
	fprintf(stderr, "\n");
}

static int try_recv(int fd, void *buf, size_t buflen, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		fprintf(stderr, "  poll error: %s\n", strerror(errno));
		return -1;
	}
	if (ret == 0) {
		fprintf(stderr, "  no response after %dms\n", timeout_ms);
		return 0;
	}
	fprintf(stderr, "  poll revents=0x%x\n", pfd.revents);

	/* use recvmsg to see msg_flags */
	struct iovec iov = { .iov_base = buf, .iov_len = buflen };
	struct msghdr mh;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;

	ssize_t n = recvmsg(fd, &mh, 0);
	if (n < 0) {
		fprintf(stderr, "  recvmsg error (errno=%d): %s\n", errno, strerror(errno));
		/* try plain recv too for comparison */
		n = recv(fd, buf, buflen, 0);
		fprintf(stderr, "  recv fallback (errno=%d): %s, n=%zd\n", errno, strerror(errno), n);
		return -1;
	}
	fprintf(stderr, "  recvmsg OK, msg_flags=0x%x\n", mh.msg_flags);
	hexdump("  response", buf, (size_t)n);

	/* try to decode as dua_msg */
	if ((size_t)n >= DUA_MSG_HEADER_SIZE) {
		struct dua_msg *r = (struct dua_msg *)buf;
		fprintf(stderr, "  sender_id=%u cmd=0x%x num_params=%u",
		        r->sender_id, r->cmd, r->num_params);
		if (r->num_params >= 1 && (size_t)n >= DUA_MSG_HEADER_SIZE + 4)
			fprintf(stderr, " params[0]=0x%x(%d)", r->params[0], (int32_t)r->params[0]);
		fprintf(stderr, "\n");
		/* string after params? */
		size_t str_off = DUA_MSG_HEADER_SIZE + r->num_params * 4;
		if ((size_t)n > str_off + 1) {
			const char *s = (const char *)buf + str_off;
			if (s[0] != '\0')
				fprintf(stderr, "  string: \"%s\"\n", s);
		}
	}
	return (int)n;
}

/* drain all pending messages, printing each one */
static void drain_responses(int fd, int timeout_ms)
{
	uint8_t resp[1024];
	for (int i = 0; i < 16; i++) {
		int n = try_recv(fd, resp, sizeof(resp), timeout_ms);
		if (n <= 0) break;
	}
}

static ssize_t send_dua(int fd, uint8_t *buf, uint32_t sender_id,
                        uint32_t cmd, uint32_t num_params, const uint32_t *params)
{
	struct dua_msg *msg = (struct dua_msg *)buf;
	msg->sender_id = sender_id;
	msg->flags = 0;
	msg->cmd = cmd;
	msg->num_params = num_params;
	for (uint32_t i = 0; i < num_params; i++)
		msg->params[i] = params[i];

	size_t len = DUA_MSG_HEADER_SIZE + num_params * sizeof(uint32_t);
	hexdump("sending", buf, len);

	ssize_t sent = send(fd, buf, len, 0);
	if (sent < 0)
		fprintf(stderr, "send failed: %s\n", strerror(errno));
	else
		fprintf(stderr, "sent %zd bytes\n", sent);
	return sent;
}

static void print_css_state(void)
{
	FILE *f = fopen("/sys/devices/platform/8000000.css/state", "r");
	if (f) {
		char s[32];
		if (fgets(s, sizeof(s), f)) {
			s[strcspn(s, "\n")] = 0;
			fprintf(stderr, "CSS state: %s\n", s);
		}
		fclose(f);
	}
	f = fopen("/sys/devices/platform/8000000.css/services", "r");
	if (f) {
		char line[64];
		while (fgets(line, sizeof(line), f))
			fprintf(stderr, "  svc: %s", line);
		fclose(f);
	}
}

int main(void)
{
	uint8_t buf[256];
	int fd = -1;
	void *shm = MAP_FAILED;
	int shmfd = -1;
	size_t shm_size = 0;

	fprintf(stderr, "=== dua_probe: proper init sequence ===\n\n");
	print_css_state();
	fprintf(stderr, "\n");

	/*
	 * step 1: shared memory init
	 */
	fprintf(stderr, "--- step 1: shared memory init ---\n");
	shmfd = open("/dev/sharedmem", O_RDWR);
	if (shmfd < 0) {
		fprintf(stderr, "open /dev/sharedmem: %s\n", strerror(errno));
		return 1;
	}

	uint32_t shm_ioctl[4] = {0};
	if (ioctl(shmfd, 0xc0045302, &shm_ioctl) < 0) {
		fprintf(stderr, "sharedmem ioctl: %s\n", strerror(errno));
		goto done;
	}
	fprintf(stderr, "sharedmem ioctl OK: phys=0x%x size=0x%x\n",
	        shm_ioctl[0], shm_ioctl[1]);

	shm_size = shm_ioctl[1];
	if (shm_size == 0) shm_size = 0x80000;

	shm = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (shm == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		goto done;
	}
	fprintf(stderr, "mmap: %p (%zu bytes)\n", shm, shm_size);

	/* zero and set header like app_dsp does */
	memset(shm, 0, shm_size);
	*(uint32_t *)((char *)shm + 4) = (uint32_t)shm_size;
	*(uint32_t *)((char *)shm + 8) = 0x30;
	__sync_synchronize();
	fprintf(stderr, "shared memory zeroed and header set\n\n");

	/*
	 * step 2: connect to DUA service
	 */
	fprintf(stderr, "--- step 2: connect to DUA ---\n");
	{
		fd = socket(PF_COMA, SOCK_SEQPACKET, 0);
		if (fd < 0) {
			fprintf(stderr, "socket: %s\n", strerror(errno));
			goto done;
		}
		struct sockaddr_coma addr = {0};
		addr.family = AF_COMA;
		strncpy(addr.service, "dua", COMA_SERVICE_NAME_MAX - 1);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			fprintf(stderr, "connect: %s\n", strerror(errno));
			goto done;
		}
		fprintf(stderr, "connected to dua (fd=%d)\n", fd);
	}

	/* let CSS register the service */
	fprintf(stderr, "waiting 1s for CSS...\n");
	sleep(1);
	print_css_state();
	fprintf(stderr, "\n");

	/*
	 * step 3: DUA_CMD_INIT_REQ (cmd 0x01)
	 *
	 * from app_dsp: sends cmd=1 with 5 params:
	 *   params[0] = 5          (unknown, maybe version/type)
	 *   params[1] = 0xffffffff (-1)
	 *   params[2] = 0xffffffff (-1)
	 *   params[3] = 0xdeadbeef (callback reference / magic)
	 *   params[4] = shared_mem_ptr (userspace address)
	 */
	fprintf(stderr, "--- step 3: DUA InitReq (cmd 0x01) ---\n");
	{
		uint32_t params[] = {
			5,
			0xffffffff,
			0xffffffff,
			0xdeadbeef,
			(uint32_t)(uintptr_t)shm,
		};
		if (send_dua(fd, buf, 1, DUA_CMD_INIT_REQ, 5, params) < 0)
			goto done;

		fprintf(stderr, "waiting for response...\n");
		drain_responses(fd, 5000);
	}

	/* check CSS still alive */
	fprintf(stderr, "\n");
	print_css_state();
	fprintf(stderr, "\n");

	/* reconnect socket — the async callback flood may have broken recv */
	fprintf(stderr, "--- reconnecting socket ---\n");
	close(fd);
	{
		fd = socket(PF_COMA, SOCK_SEQPACKET, 0);
		struct sockaddr_coma addr = {0};
		addr.family = AF_COMA;
		strncpy(addr.service, "dua", COMA_SERVICE_NAME_MAX - 1);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			fprintf(stderr, "reconnect failed: %s\n", strerror(errno));
			goto done;
		}
		fprintf(stderr, "reconnected (fd=%d)\n\n", fd);
	}

	/*
	 * step 4: DUA_CMD_APPL_INIT (cmd 0x13)
	 */
	fprintf(stderr, "--- step 4: ApplInit (cmd 0x13) ---\n");
	{
		if (send_dua(fd, buf, 2, DUA_CMD_APPL_INIT, 0, NULL) < 0)
			goto done;

		fprintf(stderr, "waiting for response...\n");
		drain_responses(fd, 5000);
	}

	fprintf(stderr, "\n");
	print_css_state();
	fprintf(stderr, "\n");

	/*
	 * step 5: try EnumUT (read-only, safe)
	 */
	fprintf(stderr, "--- step 5: EnumUT(0) ---\n");
	{
		uint32_t params[] = { 0 };
		if (send_dua(fd, buf, 3, DUA_CMD_SC_ENUM_UT, 1, params) < 0)
			goto done;

		fprintf(stderr, "waiting for response...\n");
		drain_responses(fd, 5000);
	}

	fprintf(stderr, "\n");
	print_css_state();

done:
	fprintf(stderr, "\ncleaning up\n");
	if (fd >= 0) close(fd);
	if (shm != MAP_FAILED) munmap(shm, shm_size);
	if (shmfd >= 0) close(shmfd);
	return 0;
}
