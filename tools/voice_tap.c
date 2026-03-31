/*
 * voice_tap - capture/inject audio from an FXS port via /dev/voiceN
 *
 * two modes of operation:
 *   standalone: does full BSP + DUA + TDM init (original behavior)
 *   client:     --client mode, assumes comatose_dsp is managing hardware
 *
 * usage: voice_tap [options] [port]
 *   port: FXS port number (0-7, default 0)
 *   --raw:     strip RTP/CFIFO headers, output only codec payload bytes
 *   --dump:    hex dump packets to stderr instead of writing stdout
 *   --client:  client mode — just open /dev/voiceN, no hardware setup
 *   --l16:     use L16/16000 codec (default)
 *   --g711u:   use G.711u codec instead of L16
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>

static volatile int running = 1;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static const char *result_str(comatose_result_t r)
{
	switch (r) {
	case COMATOSE_OK:           return "OK";
	case COMATOSE_ERR_SOCKET:   return "ERR_SOCKET";
	case COMATOSE_ERR_IOCTL:    return "ERR_IOCTL";
	case COMATOSE_ERR_TIMEOUT:  return "ERR_TIMEOUT";
	case COMATOSE_ERR_PROTOCOL: return "ERR_PROTOCOL";
	case COMATOSE_ERR_DUA:      return "ERR_DUA";
	case COMATOSE_ERR_INVALID:  return "ERR_INVALID";
	case COMATOSE_ERR_NOMEM:    return "ERR_NOMEM";
	default:                    return "???";
	}
}

/* RTP header size (fixed, no CSRC) */
#define RTP_HDR_SIZE 12

static void hexdump(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		fprintf(stderr, "%02x ", data[i]);
		if ((i & 15) == 15)
			fprintf(stderr, "\n");
	}
	if (len & 15)
		fprintf(stderr, "\n");
}

static void capture_loop(int voice_fd, int port, int raw_mode, int dump_mode)
{
	uint8_t pkt[2048];
	unsigned long pkt_count = 0;
	struct pollfd pfd = {
		.fd = voice_fd,
		.events = POLLIN,
	};

	fprintf(stderr, "\n=== capturing from port %d — Ctrl+C to stop ===\n\n", port);

	while (running) {
		int pr = poll(&pfd, 1, 500);
		if (pr < 0) {
			if (errno == EINTR) continue;
			fprintf(stderr, "poll error: %s\n", strerror(errno));
			break;
		}
		if (pr == 0)
			continue;

		ssize_t n = read(voice_fd, pkt, sizeof(pkt));
		if (n < 0) {
			if (errno == EINTR) continue;
			fprintf(stderr, "read error: %s (errno=%d)\n",
			        strerror(errno), errno);
			break;
		}
		if (n == 0)
			continue;

		pkt_count++;

		if (dump_mode) {
			struct rtp_packet_header *hdr = (struct rtp_packet_header *)pkt;
			fprintf(stderr, "[pkt %lu] %zd bytes, type=%u, time=%u\n",
			        pkt_count, n, hdr->packetType, hdr->receiptTime);
			hexdump(pkt, (size_t)n);
		} else if (raw_mode) {
			size_t hdr_size = sizeof(struct rtp_packet_header) + RTP_HDR_SIZE;
			if ((size_t)n > hdr_size) {
				fwrite(pkt + hdr_size, 1, (size_t)n - hdr_size, stdout);
				fflush(stdout);
			}
		} else {
			fwrite(pkt, 1, (size_t)n, stdout);
			fflush(stdout);
		}

		if (pkt_count <= 5 || (pkt_count % 500) == 0)
			fprintf(stderr, "  [%lu packets, last %zd bytes]\n",
			        pkt_count, n);
	}

	fprintf(stderr, "\n--- captured %lu packets ---\n", pkt_count);
}

/*
 * client mode: comatose_dsp owns the hardware, we just open /dev/voiceN
 */
static int run_client(int port, int raw_mode, int dump_mode, int use_g711u)
{
	fprintf(stderr, "=== voice_tap: port %d (client mode) ===\n", port);

	/* activate line (comatose_dsp leaves them in standby) */
	tapi_port_t *tp = NULL;
	tapi_port_open(&tp, port);
	if (tp)
		tapi_line_feed_set(tp, IFX_TAPI_LINE_FEED_ACTIVE);

	int voice_fd = voice_open(port);
	if (voice_fd < 0) {
		fprintf(stderr, "voice_open(%d) failed: %s\n", port, strerror(errno));
		if (tp) { tapi_line_feed_set(tp, IFX_TAPI_LINE_FEED_STANDBY); tapi_port_close(tp); }
		return 1;
	}

	rtp_session_config cfg;
	if (use_g711u)
		voice_config_g711u(&cfg, port, port);
	else
		voice_config_l16(&cfg, port, port);
	comatose_result_t r = voice_set_codec(voice_fd, &cfg);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "voice_set_codec failed: %s (errno=%d)\n",
		        result_str(r), errno);
		voice_close(voice_fd);
		if (tp) { tapi_line_feed_set(tp, IFX_TAPI_LINE_FEED_STANDBY); tapi_port_close(tp); }
		return 1;
	}
	fprintf(stderr, "  codec: %s, 20ms\n", use_g711u ? "G.711u" : "L16/16000");

	capture_loop(voice_fd, port, raw_mode, dump_mode);

	voice_close(voice_fd);
	if (tp) { tapi_line_feed_set(tp, IFX_TAPI_LINE_FEED_STANDBY); tapi_port_close(tp); }
	fprintf(stderr, "done\n");
	return 0;
}

/*
 * standalone mode: full hardware init (original behavior, using library APIs)
 */
static int run_standalone(int port, int raw_mode, int dump_mode, int use_g711u)
{
	fprintf(stderr, "=== voice_tap: port %d (standalone) ===\n", port);

	tapi_port_t *ports[COMATOSE_MAX_FXS_PORTS] = {0};
	dua_session_t *sess = NULL;
	comatose_hw_state_t hw = {0};
	int voice_fd = -1;
	int num_fxs = 0;

	/* BSP + TAPI */
	fprintf(stderr, "\n--- TAPI ---\n");
	num_fxs = tapi_init_all(ports, COMATOSE_MAX_FXS_PORTS, NULL, 0);
	if (num_fxs < 0) {
		fprintf(stderr, "tapi_init_all failed\n");
		goto cleanup;
	}
	fprintf(stderr, "  %d ports opened\n", num_fxs);

	/* DUA */
	fprintf(stderr, "\n--- DUA ---\n");
	sess = dua_open();
	if (!sess) { fprintf(stderr, "dua_open failed\n"); goto cleanup; }

	comatose_result_t r = dua_init_hw(sess);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "init_hw failed: %s\n", result_str(r));
		goto cleanup;
	}
	r = dua_appl_init(sess);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "appl_init failed: %s\n", result_str(r));
		goto cleanup;
	}

	/* units + TDM */
	fprintf(stderr, "\n--- units + TDM ---\n");
	r = dua_full_init(sess, num_fxs, num_fxs, &hw);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "dua_full_init failed: %s (dua_err=%d)\n",
		        result_str(r), dua_last_error(sess));
		goto cleanup;
	}
	for (int i = 0; i < hw.num_fxs; i++)
		fprintf(stderr, "  [%d] FXS=0x%04x VOIP=0x%04x conn=%d\n",
		        i, (unsigned)hw.fxs_uids[i] & 0xffff,
		        (unsigned)hw.voip_uids[i] & 0xffff, hw.fxs_conns[i]);

	/* activate target line */
	tapi_line_feed_set(ports[port], IFX_TAPI_LINE_FEED_ACTIVE);

	/* voice session */
	voice_fd = voice_open(port);
	if (voice_fd < 0) {
		fprintf(stderr, "voice_open(%d) failed: %s\n", port, strerror(errno));
		goto cleanup;
	}

	rtp_session_config cfg;
	if (use_g711u)
		voice_config_g711u(&cfg, port, port);
	else
		voice_config_l16(&cfg, port, port);
	r = voice_set_codec(voice_fd, &cfg);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "voice_set_codec failed: %s (errno=%d)\n",
		        result_str(r), errno);
		goto cleanup;
	}
	fprintf(stderr, "  codec: %s, 20ms\n", use_g711u ? "G.711u" : "L16/16000");

	capture_loop(voice_fd, port, raw_mode, dump_mode);

cleanup:
	voice_close(voice_fd);
	for (int i = 0; i < num_fxs; i++)
		if (ports[i]) tapi_line_feed_set(ports[i], IFX_TAPI_LINE_FEED_STANDBY);
	if (sess && hw.num_fxs > 0)
		dua_full_teardown(sess, &hw);
	if (sess) dua_close(sess);
	tapi_close_all(ports, num_fxs);
	fprintf(stderr, "done\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int port = 0;
	int raw_mode = 0;
	int dump_mode = 0;
	int client_mode = 0;
	int use_g711u = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--raw") == 0)
			raw_mode = 1;
		else if (strcmp(argv[i], "--dump") == 0)
			dump_mode = 1;
		else if (strcmp(argv[i], "--client") == 0)
			client_mode = 1;
		else if (strcmp(argv[i], "--g711u") == 0)
			use_g711u = 1;
		else if (strcmp(argv[i], "--l16") == 0)
			use_g711u = 0;
		else if (argv[i][0] != '-')
			port = atoi(argv[i]);
	}

	if (port < 0 || port >= COMATOSE_MAX_FXS_PORTS) {
		fprintf(stderr, "invalid port %d (0-%d)\n", port, COMATOSE_MAX_FXS_PORTS - 1);
		return 1;
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	if (client_mode)
		return run_client(port, raw_mode, dump_mode, use_g711u);
	else
		return run_standalone(port, raw_mode, dump_mode, use_g711u);
}
