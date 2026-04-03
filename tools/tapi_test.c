/*
 * tapi_test - swiss army knife for TAPI hardware testing
 *
 * exercises the TAPI stack (line feed, ringing, hook events, DTMF, etc.)
 * without touching the COMA/DUA stack. safe to run alongside app_dsp.
 *
 * by default, just opens port fds without sending any init ioctls.
 * use --init for a full BSP query + CH_INIT + LINE_TYPE_SET per port.
 *
 * usage: tapi_test [port] [options]
 *   port              FXS port number (0-7), omit for all ports
 *   --init            full TAPI init (BSP query + CH_INIT + LINE_TYPE_SET)
 *   --linefeed <val>  set line feed: active, disabled, reversed, oht,
 *                     oht-rev, or a numeric ioctl value (0-24)
 *   --ring on|off     start or stop ringing
 *   --tone <code>     play local tone (numeric index), 'stop' to stop
 *   --monitor         enter event monitoring loop (ctrl-c to exit)
 *
 * examples:
 *   tapi_test 0 --monitor             # monitor events on port 0
 *   tapi_test 0 --linefeed active     # activate port 0
 *   tapi_test --ring on 3             # ring port 3
 *   tapi_test --linefeed active --monitor  # activate all, then monitor
 *   tapi_test --init                  # full init, print status, exit
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <errno.h>

static volatile int running = 1;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static int parse_linefeed(const char *s)
{
	if (strcmp(s, "active") == 0)   return IFX_TAPI_LINE_FEED_ACTIVE;
	if (strcmp(s, "reversed") == 0) return IFX_TAPI_LINE_FEED_ACTIVE_REV;
	if (strcmp(s, "standby") == 0)  return IFX_TAPI_LINE_FEED_STANDBY;
	if (strcmp(s, "disabled") == 0) return IFX_TAPI_LINE_FEED_DISABLED;
	if (strcmp(s, "ring") == 0)     return IFX_TAPI_LINE_FEED_RING_BURST;
	if (strcmp(s, "oht") == 0)      return IFX_TAPI_LINE_FEED_FWD_OHT;
	if (strcmp(s, "oht-rev") == 0)  return IFX_TAPI_LINE_FEED_REV_OHT;

	char *end;
	long v = strtol(s, &end, 0);
	if (*end == '\0' && v >= 0 && v <= 24)
		return (int)v;

	return -1;
}

static const char *linefeed_name(int v)
{
	switch (v) {
	case 0:  return "ACTIVE (ProSLIC FWD_ACTIVE)";
	case 1:  return "ACTIVE_REV (ProSLIC REV_ACTIVE)";
	case 2:  return "STANDBY (ProSLIC FWD_ACTIVE — same as active!)";
	case 4:  return "DISABLED (ProSLIC OPEN)";
	case 10: return "RING_BURST (ProSLIC RINGING)";
	case 11: return "RING_PAUSE (ProSLIC FWD_ACTIVE)";
	case 21: return "FWD_OHT (ProSLIC FWD_OHT)";
	case 22: return "REV_OHT (ProSLIC REV_OHT)";
	case 23: return "GS_REVERSED (ProSLIC REV_ACTIVE)";
	case 24: return "GS_OPEN_NOIRQ (ProSLIC OPEN + no hook IRQ)";
	default: return "unknown";
	}
}

static void print_event(int port_idx, const tapi_event_t *evt,
                        struct timespec *t0)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = (now.tv_sec - t0->tv_sec) +
	                 (now.tv_nsec - t0->tv_nsec) / 1e9;

	switch (evt->id) {
	case TAPI_EVENT_FXS_ONHOOK:
		fprintf(stderr, "[%7.3f] port %d: ON-HOOK\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FXS_OFFHOOK:
		fprintf(stderr, "[%7.3f] port %d: OFF-HOOK\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FXS_FLASH:
		fprintf(stderr, "[%7.3f] port %d: FLASH\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FXS_RINGBURST_END:
		fprintf(stderr, "[%7.3f] port %d: RING-BURST-END\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FXS_RINGING_END:
		fprintf(stderr, "[%7.3f] port %d: RINGING-END\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_DTMF_DIGIT: {
		/* data format: byte2=ASCII, byte1=digit index, byte0=action
		 * digit index: 1-9, 0xa='0', 0xb='*', 0xc='#' */
		char ascii = (char)((evt->data.value >> 16) & 0xff);
		int idx = (evt->data.value >> 8) & 0xff;
		int action = evt->data.value & 0xff;
		if (ascii >= 0x20 && ascii < 0x7f)
			fprintf(stderr, "[%7.3f] port %d: DTMF '%c' "
			        "(idx=%d act=%d)\n",
			        elapsed, port_idx, ascii, idx, action);
		else
			fprintf(stderr, "[%7.3f] port %d: DTMF "
			        "idx=%d act=%d (raw=0x%x)\n",
			        elapsed, port_idx, idx, action,
			        evt->data.value);
		break;
	}
	case TAPI_EVENT_PULSE_DIGIT: {
		/* data format: byte1=pulse count
		 * 1-9 = digits 1-9, 0xb(11) = digit 0
		 * (standard pulse: 0 = 10 pulses, but GS reports 11?) */
		int count = (evt->data.value >> 8) & 0xff;
		char ch;
		if (count >= 1 && count <= 9)
			ch = '0' + count;
		else if (count == 10 || count == 11)
			ch = '0';
		else
			ch = '?';
		fprintf(stderr, "[%7.3f] port %d: PULSE '%c' "
		        "(count=%d raw=0x%x)\n",
		        elapsed, port_idx, ch, count,
		        evt->data.value);
		break;
	}
	case TAPI_EVENT_FAULT_OVERTEMP:
		fprintf(stderr, "[%7.3f] port %d: FAULT: OVERTEMP\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FAULT_OVERCURRENT:
		fprintf(stderr, "[%7.3f] port %d: FAULT: OVERCURRENT\n",
		        elapsed, port_idx);
		break;
	case TAPI_EVENT_FAULT_OVERVOLTAGE:
		fprintf(stderr, "[%7.3f] port %d: FAULT: OVERVOLTAGE\n",
		        elapsed, port_idx);
		break;
	default:
		fprintf(stderr, "[%7.3f] port %d: UNKNOWN(0x%08x) "
		        "data=0x%08x\n",
		        elapsed, port_idx, evt->id, evt->data.value);
		break;
	}
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: tapi_test [port] [options]\n"
	    "  port              FXS port (0-7), omit for all\n"
	    "  --init            full TAPI init (BSP + CH_INIT + LINE_TYPE_SET)\n"
	    "  --linefeed <val>  set line feed state\n"
	    "                    names: active, disabled, reversed,\n"
	    "                           standby, ring, oht, oht-rev\n"
	    "                    or numeric ioctl value (0-24)\n"
	    "  --ring on|off     start/stop ringing\n"
	    "  --tone <code>     play tone (numeric), 'stop' to stop\n"
	    "  --monitor         event monitoring loop\n"
	);
}

int main(int argc, char *argv[])
{
	int selected_port = -1; /* -1 = all ports */
	int do_init = 0;
	int do_monitor = 0;
	int linefeed_val = -1;
	int ring_on = -1;      /* -1 = not set, 0 = off, 1 = on */
	int tone_code = -2;    /* -2 = not set, -1 = stop */

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--init") == 0) {
			do_init = 1;
		} else if (strcmp(argv[i], "--monitor") == 0) {
			do_monitor = 1;
		} else if (strcmp(argv[i], "--linefeed") == 0 && i + 1 < argc) {
			linefeed_val = parse_linefeed(argv[++i]);
			if (linefeed_val < 0) {
				fprintf(stderr, "bad linefeed value: %s\n",
				        argv[i]);
				return 1;
			}
		} else if (strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "on") == 0)
				ring_on = 1;
			else if (strcmp(argv[i], "off") == 0)
				ring_on = 0;
			else {
				fprintf(stderr, "bad ring value: %s "
				        "(expected on/off)\n", argv[i]);
				return 1;
			}
		} else if (strcmp(argv[i], "--tone") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "stop") == 0)
				tone_code = -1;
			else
				tone_code = atoi(argv[i]);
		} else if (strcmp(argv[i], "--help") == 0 ||
		           strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		} else if (argv[i][0] >= '0' && argv[i][0] <= '9' &&
		           selected_port < 0) {
			selected_port = atoi(argv[i]);
			if (selected_port < 0 ||
			    selected_port >= COMATOSE_MAX_FXS_PORTS) {
				fprintf(stderr, "bad port: %d (0-%d)\n",
				        selected_port,
				        COMATOSE_MAX_FXS_PORTS - 1);
				return 1;
			}
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage();
			return 1;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	/* --- init --- */
	tapi_port_t *ports[COMATOSE_MAX_FXS_PORTS] = {0};
	int num_fxs = 0;

	if (do_init) {
		/* full init: BSP query + CH_INIT + LINE_TYPE_SET per port */
		ht_bsp_init_result_t info;
		num_fxs = tapi_init_all(ports, COMATOSE_MAX_FXS_PORTS,
		                        &info, 1 /* skip_reset */);
		if (num_fxs < 0) {
			fprintf(stderr, "tapi_init_all failed\n");
			return 1;
		}
		fprintf(stderr, "=== tapi_test (full init) ===\n");
		fprintf(stderr, "  FXS: %d ports (slic: %d x %d)\n",
		        num_fxs, info.slic_count, info.slic_channels);
		if (info.daa_count > 0)
			fprintf(stderr, "  FXO: %d ports (not supported)\n",
			        info.daa_count * info.daa_channels);
	} else {
		/* default: just open fd(s), no init ioctls */
		fprintf(stderr, "=== tapi_test ===\n");
		if (selected_port >= 0) {
			comatose_result_t r = tapi_port_open(
				&ports[selected_port], selected_port);
			if (r != COMATOSE_OK) {
				fprintf(stderr, "port %d open failed\n",
				        selected_port);
				return 1;
			}
			num_fxs = selected_port + 1;
		} else {
			for (int i = 0; i < COMATOSE_MAX_FXS_PORTS; i++) {
				if (tapi_port_open(&ports[i], i) ==
				    COMATOSE_OK)
					num_fxs = i + 1;
				else
					break;
			}
		}
		fprintf(stderr, "  opened %d port(s)\n", num_fxs);
	}

	if (num_fxs == 0) {
		fprintf(stderr, "no ports available\n");
		return 1;
	}

	/* determine port range to operate on */
	int first_port = (selected_port >= 0) ? selected_port : 0;
	int last_port = (selected_port >= 0) ? selected_port : num_fxs - 1;

	/* --- print hook status --- */
	for (int i = first_port; i <= last_port; i++) {
		if (!ports[i]) continue;
		int hook = 0;
		comatose_result_t r = tapi_hook_status_get(ports[i], &hook);
		if (r == COMATOSE_OK)
			fprintf(stderr, "  port %d: %s\n", i,
			        hook ? "OFF-HOOK" : "ON-HOOK");
		else
			fprintf(stderr, "  port %d: hook status failed\n", i);
	}

	/* --- linefeed --- */
	if (linefeed_val >= 0) {
		fprintf(stderr, "\n--- linefeed ---\n");
		for (int i = first_port; i <= last_port; i++) {
			if (!ports[i]) continue;
			comatose_result_t r = tapi_line_feed_set(
				ports[i], linefeed_val);
			if (r == COMATOSE_OK)
				fprintf(stderr, "  port %d: set %d = %s\n",
				        i, linefeed_val,
				        linefeed_name(linefeed_val));
			else
				fprintf(stderr, "  port %d: FAILED\n", i);
		}
	}

	/* --- ring --- */
	if (ring_on >= 0) {
		fprintf(stderr, "\n--- ring ---\n");
		for (int i = first_port; i <= last_port; i++) {
			if (!ports[i]) continue;
			comatose_result_t r;
			if (ring_on) {
				/* match gs_ata sequence: cadence first, then
				 * start. do NOT set line feed — the driver
				 * manages it internally during ringing. */
				tapi_ring_cadence_set(ports[i], NULL);
				r = tapi_ring_start(ports[i]);
			} else {
				r = tapi_ring_stop(ports[i]);
			}
			fprintf(stderr, "  port %d: ring %s — %s\n", i,
			        ring_on ? "START" : "STOP",
			        r == COMATOSE_OK ? "ok" : "FAILED");
		}
	}

	/* --- tone --- */
	if (tone_code >= -1) {
		fprintf(stderr, "\n--- tone ---\n");
		for (int i = first_port; i <= last_port; i++) {
			if (!ports[i]) continue;
			comatose_result_t r;
			if (tone_code < 0) {
				r = tapi_tone_stop(ports[i]);
				fprintf(stderr, "  port %d: tone STOP — %s\n",
				        i, r == COMATOSE_OK ? "ok" : "FAILED");
			} else {
				r = tapi_tone_local_play(ports[i], tone_code);
				fprintf(stderr, "  port %d: tone %d — %s\n",
				        i, tone_code,
				        r == COMATOSE_OK ? "ok" : "FAILED");
			}
		}
	}

	/* --- monitor --- */
	if (do_monitor) {
		fprintf(stderr, "\n--- monitor (ctrl-c to stop) ---\n");

		/* build pollfd array */
		struct pollfd fds[COMATOSE_MAX_FXS_PORTS];
		int nfds = 0;
		int fd_to_port[COMATOSE_MAX_FXS_PORTS];

		for (int i = first_port; i <= last_port; i++) {
			if (!ports[i]) continue;
			fds[nfds].fd = tapi_port_fd(ports[i]);
			fds[nfds].events = POLLIN;
			fd_to_port[nfds] = i;
			nfds++;
		}

		struct timespec t0;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		while (running) {
			int pr = poll(fds, nfds, 500);
			if (pr < 0) {
				if (errno == EINTR) continue;
				fprintf(stderr, "poll error: %s\n",
				        strerror(errno));
				break;
			}
			if (pr == 0) continue;

			for (int f = 0; f < nfds; f++) {
				if (!(fds[f].revents & POLLIN))
					continue;

				int port_idx = fd_to_port[f];
				tapi_event_t evt;
				do {
					comatose_result_t r = tapi_event_get(
						ports[port_idx], &evt);
					if (r != COMATOSE_OK)
						break;
					print_event(port_idx, &evt, &t0);
				} while (evt.more);
			}
		}

		fprintf(stderr, "\n--- stopping ---\n");
	}

	/* --- cleanup --- */
	for (int i = first_port; i <= last_port; i++) {
		if (!ports[i]) continue;
		/* stop ringing if we started it */
		if (ring_on == 1)
			tapi_ring_stop(ports[i]);
		/* stop tone if we started one */
		if (tone_code >= 0)
			tapi_tone_stop(ports[i]);
		/* don't touch line state — we're a diagnostic tool running
		 * alongside app_dsp, and disabling lines on exit would
		 * pull battery from under the stock stack's feet. */
		tapi_port_close(ports[i]);
		ports[i] = NULL;
	}

	fprintf(stderr, "done\n");
	return 0;
}
