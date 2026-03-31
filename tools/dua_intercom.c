/*
 * dua_intercom - intercom between two FXS ports
 *
 * sets up the full DUA + TDM pipeline, merges two FXS port connections
 * for bidirectional audio. audio stays within the CSS (no voice devices).
 *
 * prerequisite: run `bsp_init` once after boot to initialize the BSP/SLICs.
 *               do NOT re-run bsp_init between invocations (causes SLIC desync).
 *
 * usage: dua_intercom [options] [port_a port_b]
 *   defaults to ports 0 and 1
 *   --quick: skip hold, go straight to teardown
 *   --client: skip BSP/DUA/TDM init (comatose_dsp manages hardware)
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

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

int main(int argc, char *argv[])
{
	int port_a = 0, port_b = 1;
	int quick_mode = 0;
	int client_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--quick") == 0 || strcmp(argv[i], "-q") == 0)
			quick_mode = 1;
		else if (strcmp(argv[i], "--client") == 0)
			client_mode = 1;
		else if (i + 1 < argc && argv[i][0] != '-') {
			port_a = atoi(argv[i]);
			port_b = atoi(argv[i + 1]);
			i++;
		}
	}

	fprintf(stderr, "=== dua_intercom: port %d <-> port %d%s ===\n\n",
	        port_a, port_b, client_mode ? " (client)" : "");

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	tapi_port_t *ports[COMATOSE_MAX_FXS_PORTS] = {0};
	dua_session_t *sess = NULL;
	comatose_hw_state_t hw = {0};
	int num_fxs = 0;
	int merged = 0;
	int owns_hw = !client_mode;

	if (client_mode) {
		/* client mode: just open the two ports for line control */
		tapi_port_open(&ports[port_a], port_a);
		tapi_port_open(&ports[port_b], port_b);
		num_fxs = COMATOSE_MAX_FXS_PORTS; /* for cleanup bounds */

		/* in client mode, we still need DUA for conn_merge.
		 * connect to the DUA service but skip init. */
		sess = dua_open();
		if (!sess) {
			fprintf(stderr, "dua_open failed\n");
			goto cleanup;
		}
	} else {
		/* standalone: full init */
		fprintf(stderr, "--- TAPI ---\n");
		num_fxs = tapi_init_all(ports, COMATOSE_MAX_FXS_PORTS, NULL, 0);
		if (num_fxs < 0) {
			fprintf(stderr, "tapi_init_all failed\n");
			goto cleanup;
		}
		fprintf(stderr, "  %d ports opened\n", num_fxs);

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

		fprintf(stderr, "\n--- units + TDM ---\n");
		r = dua_full_init(sess, num_fxs, num_fxs, &hw);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "dua_full_init failed: %s (dua_err=%d)\n",
			        result_str(r), dua_last_error(sess));
			goto cleanup;
		}
		for (int i = 0; i < hw.num_fxs; i++)
			fprintf(stderr, "  FXS[%d] uid=0x%04x conn=%d\n", i,
			        (unsigned)hw.fxs_uids[i] & 0xffff, hw.fxs_conns[i]);
	}

	/* merge connections for intercom */
	fprintf(stderr, "\n--- intercom routing ---\n");
	{
		dua_conn_t conn_a, conn_b;

		if (client_mode) {
			/* in client mode, query connection IDs from DUA.
			 * for now, assume comatose_dsp uses the same conn layout
			 * (conn_id = FXS index + 1, typically). this is fragile —
			 * a proper solution would be a query API. */
			fprintf(stderr, "  (client mode: conn merge not yet supported)\n");
			fprintf(stderr, "  intercom requires conn_ids from comatose_dsp\n");
			goto hold;
		} else {
			conn_a = hw.fxs_conns[port_a];
			conn_b = hw.fxs_conns[port_b];
		}

		fprintf(stderr, "  merging conn %d (port %d) + conn %d (port %d)\n",
		        conn_a, port_a, conn_b, port_b);
		comatose_result_t r = dua_conn_merge(sess, conn_a, conn_b);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "conn_merge failed: %s (dua_err=%d)\n",
			        result_str(r), dua_last_error(sess));
			goto cleanup;
		}
		merged = 1;
	}

	/* activate lines */
	fprintf(stderr, "\n--- activating lines ---\n");
	if (ports[port_a])
		tapi_line_feed_set(ports[port_a], IFX_TAPI_LINE_FEED_ACTIVE);
	if (ports[port_b])
		tapi_line_feed_set(ports[port_b], IFX_TAPI_LINE_FEED_ACTIVE);
	fprintf(stderr, "  ports %d and %d active\n", port_a, port_b);

hold:
	if (quick_mode) {
		fprintf(stderr, "\n=== quick mode: skipping hold ===\n");
		usleep(500000);
	} else {
		fprintf(stderr, "\n=== intercom active! pick up ports %d and %d ===\n",
		        port_a, port_b);
		fprintf(stderr, "Ctrl+C to tear down\n\n");
		while (running)
			sleep(1);
	}

	fprintf(stderr, "\n--- teardown ---\n");

cleanup:
	/* deactivate lines */
	if (ports[port_a])
		tapi_line_feed_set(ports[port_a], IFX_TAPI_LINE_FEED_STANDBY);
	if (ports[port_b])
		tapi_line_feed_set(ports[port_b], IFX_TAPI_LINE_FEED_STANDBY);

	/* unmerge */
	if (merged && sess)
		dua_conn_unmerge(sess, hw.fxs_conns[port_a]);

	/* DUA teardown (only if we own it) */
	if (owns_hw && sess && hw.num_fxs > 0)
		dua_full_teardown(sess, &hw);
	if (sess)
		dua_close(sess);

	/* TAPI teardown */
	for (int i = 0; i < COMATOSE_MAX_FXS_PORTS; i++) {
		if (ports[i])
			tapi_port_close(ports[i]);
	}

	fprintf(stderr, "done\n");
	return 0;
}
