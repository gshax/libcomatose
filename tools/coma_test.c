/*
 * coma_test - test connecting to various COMA services
 *
 * tries to connect to kernel-registered services (voice, tdm) and
 * then to "dua" to see which causes the CSS panic.
 */

#include <comatose/coma.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

static int try_connect(const char *service)
{
	fprintf(stderr, "connecting to \"%s\"... ", service);

	int fd = socket(PF_COMA, SOCK_SEQPACKET, 0);
	if (fd < 0) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_coma addr;
	memset(&addr, 0, sizeof(addr));
	addr.family = AF_COMA;
	strncpy(addr.service, service, COMA_SERVICE_NAME_MAX - 1);

	int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "connect() failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	fprintf(stderr, "OK (fd=%d)\n", fd);
	return fd;
}

int main(int argc, char *argv[])
{
	if (argc > 1) {
		/* connect to a specific service */
		int fd = try_connect(argv[1]);
		if (fd >= 0) {
			fprintf(stderr, "connected! sleeping 2s then closing...\n");
			sleep(2);
			close(fd);
		}
		return fd < 0 ? 1 : 0;
	}

	/* default: try kernel services first, then dua */
	fprintf(stderr, "=== testing COMA service connectivity ===\n\n");

	int fd_voice = try_connect("voice");
	if (fd_voice >= 0) {
		fprintf(stderr, "  voice service works!\n\n");
		close(fd_voice);
	}

	fprintf(stderr, "sleeping 1s...\n");
	sleep(1);

	int fd_tdm = try_connect("tdm");
	if (fd_tdm >= 0) {
		fprintf(stderr, "  tdm service works!\n\n");
		close(fd_tdm);
	}

	fprintf(stderr, "sleeping 1s...\n");
	sleep(1);

	fprintf(stderr, "about to try \"dua\" (this may cause CSS panic)...\n");
	int fd_dua = try_connect("dua");
	if (fd_dua >= 0) {
		fprintf(stderr, "  dua service connected! trying to send ApplInit...\n");

		/* try sending a minimal ApplInit */
		uint8_t msg[] = {
			0x01, 0x00, 0x00, 0x00,  /* sender_id */
			0x00, 0x00, 0x00, 0x00,  /* flags */
			0x13, 0x00, 0x00, 0x00,  /* cmd = ApplInit */
			0x00, 0x00, 0x00, 0x00,  /* num_params = 0 */
		};
		ssize_t sent = send(fd_dua, msg, sizeof(msg), 0);
		fprintf(stderr, "  sent %zd bytes\n", sent);

		uint8_t resp[256];
		/* wait a bit for response */
		sleep(1);
		ssize_t n = recv(fd_dua, resp, sizeof(resp), MSG_DONTWAIT);
		if (n > 0) {
			fprintf(stderr, "  got %zd byte response: ", n);
			for (ssize_t i = 0; i < n && i < 32; i++)
				fprintf(stderr, "%02x ", resp[i]);
			fprintf(stderr, "\n");
		} else {
			fprintf(stderr, "  no response (n=%zd, errno=%s)\n", n, strerror(errno));
		}

		close(fd_dua);
	}

	fprintf(stderr, "\ndone.\n");
	return 0;
}
