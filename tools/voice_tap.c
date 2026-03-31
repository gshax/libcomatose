/*
 * voice_tap - capture/inject audio from an FXS port via /dev/voiceN
 *
 * sets up the full DUA + TDM pipeline, opens a voice session with G.711u,
 * and streams raw ulaw samples to/from stdout/stdin.
 *
 * prerequisite: run `bsp_init` once after boot to initialize the BSP/SLICs.
 *               do NOT re-run bsp_init between invocations (causes SLIC desync).
 *               requires comatose_tdm.ko loaded and a clean TDM state (reboot).
 *
 * usage: voice_tap [options] [port]
 *   port: FXS port number (0-7, default 0)
 *   --raw:         strip RTP/CFIFO headers, output only codec payload bytes
 *   --rx-only:     only capture (don't write to dec_fifo)
 *   --dump:        hex dump packets to stderr instead of writing stdout
 *   --no-dua-init: skip DUA init_hw/appl_init (use when app_dsp is running)
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>

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

#define TRY(expr, label) do { \
	comatose_result_t _r = (expr); \
	if (_r != COMATOSE_OK) { \
		fprintf(stderr, "%s failed: %s\n", label, result_str(_r)); \
		goto cleanup; \
	} \
} while(0)

#define TRY_DUA(expr, label) do { \
	comatose_result_t _r = (expr); \
	if (_r != COMATOSE_OK) { \
		fprintf(stderr, "%s failed: %s (dua_err=%d)\n", \
		        label, result_str(_r), dua_last_error(sess)); \
		goto cleanup; \
	} \
} while(0)

#define TOTAL_FXS  8
#define TOTAL_VOIP 16

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

int main(int argc, char *argv[])
{
	int port = 0;
	int raw_mode = 0;
	int rx_only = 1;  /* default: capture only for now */
	int dump_mode = 0;
	int no_dua_init = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--raw") == 0)
			raw_mode = 1;
		else if (strcmp(argv[i], "--rx-only") == 0)
			rx_only = 1;
		else if (strcmp(argv[i], "--dump") == 0)
			dump_mode = 1;
		else if (strcmp(argv[i], "--no-dua-init") == 0)
			no_dua_init = 1;
		else if (argv[i][0] != '-')
			port = atoi(argv[i]);
	}

	if (port < 0 || port >= TOTAL_FXS) {
		fprintf(stderr, "invalid port %d (0-%d)\n", port, TOTAL_FXS - 1);
		return 1;
	}

	fprintf(stderr, "=== voice_tap: port %d ===\n", port);
	if (raw_mode)
		fprintf(stderr, "  mode: raw payload output\n");
	if (dump_mode)
		fprintf(stderr, "  mode: hex dump to stderr\n");
	if (no_dua_init)
		fprintf(stderr, "  skipping DUA init (app_dsp mode)\n");

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	tapi_port_t *tapi_ports[TOTAL_FXS] = {0};
	dua_session_t *sess = NULL;
	dua_uid_t all_fxs[TOTAL_FXS];
	dua_conn_t fxs_conn[TOTAL_FXS];
	dua_uid_t voip_uid = 0;
	int voice_fd = -1;
	int voip_instance = port;  /* map 1:1 for simplicity */

	/* --- TAPI: open ports --- */
	fprintf(stderr, "\n--- TAPI ---\n");
	for (int i = 0; i < TOTAL_FXS; i++)
		TRY(tapi_port_open(&tapi_ports[i], i), "port_open");
	fprintf(stderr, "  %d ports opened\n", TOTAL_FXS);

	/* --- DUA: init --- */
	fprintf(stderr, "\n--- DUA init ---\n");
	sess = dua_open();
	if (!sess) { fprintf(stderr, "dua_open failed\n"); goto cleanup; }

	if (no_dua_init) {
		fprintf(stderr, "  (skipped — relying on app_dsp)\n");
	} else {
		TRY_DUA(dua_init_hw(sess), "init_hw");
		TRY_DUA(dua_appl_init(sess), "appl_init");
	}

	/* --- FXS units: allocate, mode 1, connect --- */
	fprintf(stderr, "\n--- FXS units ---\n");
	for (int i = 0; i < TOTAL_FXS; i++) {
		TRY_DUA(dua_unit_allocate(sess, DUA_UT_FXS, i, &all_fxs[i]),
		        "fxs_alloc");
		TRY_DUA(dua_set_umt_mode(sess, all_fxs[i], DUA_UMT_FXS_DSP_PIPELINE),
		        "fxs_mode1");
		TRY_DUA(dua_unit_connect(sess, all_fxs[i], -3),
		        "fxs_connect");
		fxs_conn[i] = (dua_conn_t)dua_last_async_elem(sess);
		fprintf(stderr, "  FXS[%d] uid=0x%04x conn=%d\n", i,
		        (unsigned)all_fxs[i] & 0xffff, fxs_conn[i]);
	}

	/* --- VOIP unit: allocate, set narrowband mode, connect to FXS --- */
	fprintf(stderr, "\n--- VOIP unit ---\n");
	TRY_DUA(dua_unit_allocate(sess, DUA_UT_SPVOIPNDA, voip_instance, &voip_uid),
	        "voip_alloc");
	fprintf(stderr, "  VOIP uid=0x%04x (instance %d)\n",
	        (unsigned)voip_uid & 0xffff, voip_instance);

	TRY_DUA(dua_set_umt_mode(sess, voip_uid, DUA_UMT_SPVOIP_NB_20MS),
	        "voip_mode1");
	fprintf(stderr, "  VOIP mode set: narrowband 20ms\n");

	TRY_DUA(dua_unit_connect(sess, voip_uid, fxs_conn[port]),
	        "voip_connect");
	fprintf(stderr, "  VOIP connected to FXS[%d] conn %d\n", port, fxs_conn[port]);

	/* --- TDM --- */
	fprintf(stderr, "\n--- TDM ---\n");
	TRY_DUA(dua_set_tdm_assignment(sess, all_fxs[0], 0, TOTAL_FXS),
	        "tdm_assign");
	{
		FILE *f = fopen("/proc/gs/css_own_tdm0", "w");
		if (!f) {
			fprintf(stderr, "tdm grant: can't open proc: %s\n", strerror(errno));
			goto cleanup;
		}
		fwrite("1", 1, 1, f);
		fclose(f);
		fprintf(stderr, "  tdm grant: OK (check dmesg!)\n");
	}

	/* --- activate SLIC line --- */
	fprintf(stderr, "\n--- activating line %d ---\n", port);
	tapi_line_feed_set(tapi_ports[port], IFX_TAPI_LINE_FEED_ACTIVE);

	/* --- open voice device --- */
	{
		char devpath[32];
		snprintf(devpath, sizeof(devpath), "/dev/voice%d", voip_instance);
		fprintf(stderr, "\n--- opening %s ---\n", devpath);
		voice_fd = open(devpath, O_RDWR);
		if (voice_fd < 0) {
			fprintf(stderr, "  open(%s): %s\n", devpath, strerror(errno));
			fprintf(stderr, "  (device may not exist — check /proc/devices for 'voice' major)\n");
			goto cleanup;
		}
		fprintf(stderr, "  fd=%d\n", voice_fd);
	}

	/* --- configure voice session: G.711u, 20ms --- */
	{
		rtp_session_config config;
		memset(&config, 0, sizeof(config));

		config.codec.tx_pt = RTP_PT_G711U;
		config.codec.tx_pt_event = 0xff;  /* no events */
		config.codec.rx_pt_event = 0xff;
		config.codec.duration = 20;
		config.codec.opts = RTP_CODEC_OPT_NONE;
		strncpy(config.codec.CodecStr, "pcmu/8000",
		        sizeof(config.codec.CodecStr) - 1);

		/* rx codec list: accept G.711u */
		config.codec.rx_list[0].rx_pt = RTP_PT_G711U;
		strncpy(config.codec.rx_list[0].CodecStr, "pcmu/8000",
		        sizeof(config.codec.rx_list[0].CodecStr) - 1);
		/* mark remaining rx slots as unused */
		for (int i = 1; i < VOICE_MAX_CODECS; i++)
			config.codec.rx_list[i].rx_pt = (char)0xff;

		config.opts = RTP_OPT_NONE;
		config.audio_mode = RTP_MODE_ACTIVE;
		config.lib_rtp_mode = RTP_APP_VOIP_USER;
		config.voip_line_id = voip_instance;
		config.session_id = voip_instance;
		config.SymmRTPTxPktCnt = 10;

		fprintf(stderr, "\n--- VOICE_IOCSETCODEC ---\n");
		fprintf(stderr, "  codec: G.711u, 20ms, voip_line_id=%d, session_id=%d\n",
		        config.voip_line_id, config.session_id);
		fprintf(stderr, "  sizeof(rtp_session_config) = %zu\n", sizeof(config));

		int ret = ioctl(voice_fd, VOICE_IOCSETCODEC, &config);
		if (ret < 0) {
			fprintf(stderr, "  VOICE_IOCSETCODEC failed: %s (errno=%d)\n",
			        strerror(errno), errno);
			goto cleanup;
		}
		fprintf(stderr, "  OK!\n");
	}

	/* --- main capture loop --- */
	fprintf(stderr, "\n=== capturing from port %d — Ctrl+C to stop ===\n\n", port);

	{
		uint8_t pkt[2048];
		unsigned long pkt_count = 0;
		struct pollfd pfd = {
			.fd = voice_fd,
			.events = POLLIN,
		};

		while (running) {
			int pr = poll(&pfd, 1, 500);
			if (pr < 0) {
				if (errno == EINTR) continue;
				fprintf(stderr, "poll error: %s\n", strerror(errno));
				break;
			}
			if (pr == 0)
				continue;  /* timeout, check running flag */

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
				/* strip rtp_packet_header (12B) + RTP header (12B) */
				size_t hdr_size = sizeof(struct rtp_packet_header) + RTP_HDR_SIZE;
				if ((size_t)n > hdr_size) {
					size_t payload_len = (size_t)n - hdr_size;
					fwrite(pkt + hdr_size, 1, payload_len, stdout);
					fflush(stdout);
				}
			} else {
				/* write full packet (headers + payload) */
				fwrite(pkt, 1, (size_t)n, stdout);
				fflush(stdout);
			}

			if (pkt_count <= 5 || (pkt_count % 500) == 0)
				fprintf(stderr, "  [%lu packets, last %zd bytes]\n",
				        pkt_count, n);
		}

		fprintf(stderr, "\n--- captured %lu packets ---\n", pkt_count);
	}

	/* --- teardown --- */
	fprintf(stderr, "\n--- teardown ---\n");

cleanup:
	/* stop voice session */
	if (voice_fd >= 0) {
		ioctl(voice_fd, VOICE_IOCSTOP_SESSION);
		close(voice_fd);
		fprintf(stderr, "  voice session closed\n");
	}

	/* deactivate SLIC */
	for (int i = 0; i < TOTAL_FXS; i++) {
		if (tapi_ports[i])
			tapi_line_feed_set(tapi_ports[i], IFX_TAPI_LINE_FEED_STANDBY);
	}

	/* disconnect and free VOIP unit */
	if (sess && voip_uid) {
		dua_unit_disconnect(sess, voip_uid, fxs_conn[port]);
		dua_unit_free(sess, voip_uid);
	}

	/* disconnect and free FXS units */
	if (sess) {
		for (int i = TOTAL_FXS - 1; i >= 0; i--) {
			dua_unit_disconnect(sess, all_fxs[i], fxs_conn[i]);
			dua_unit_free(sess, all_fxs[i]);
		}
		fprintf(stderr, "  units freed\n");
	}

	if (sess) dua_close(sess);
	for (int i = 0; i < TOTAL_FXS; i++) {
		if (tapi_ports[i])
			tapi_port_close(tapi_ports[i]);
	}
	fprintf(stderr, "done\n");
	return 0;
}
