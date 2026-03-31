/*
 * dua_intercom - full DUA + TDM init, then intercom between two FXS ports
 *
 * prerequisite: run `bsp_init` once after boot to initialize the BSP/SLICs.
 *               do NOT re-run bsp_init between invocations (causes SLIC desync).
 *
 * usage: dua_intercom [port_a port_b]
 *   defaults to ports 0 and 1 (first two physical FXS jacks)
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

int main(int argc, char *argv[])
{
	int port_a = 0, port_b = 1;
	int quick_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--quick") == 0 || strcmp(argv[i], "-q") == 0)
			quick_mode = 1;
		else if (i + 1 < argc && argv[i][0] != '-') {
			port_a = atoi(argv[i]);
			port_b = atoi(argv[i + 1]);
			i++;
		}
	}

	fprintf(stderr, "=== dua_intercom: port %d <-> port %d ===\n\n", port_a, port_b);

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	tapi_port_t *tapi_ports[TOTAL_FXS] = {0};
	dua_session_t *sess = NULL;
	dua_uid_t all_fxs[TOTAL_FXS];
	dua_conn_t fxs_conn[TOTAL_FXS]; /* conn_ids from connect(-3) */
	dua_uid_t all_voip[TOTAL_VOIP];
	int merged = 0;

	/* --- TAPI: open ports (assume bsp_init already ran) --- */
	fprintf(stderr, "--- TAPI ---\n");
	for (int i = 0; i < TOTAL_FXS; i++)
		TRY(tapi_port_open(&tapi_ports[i], i), "port_open");
	fprintf(stderr, "  %d ports opened\n", TOTAL_FXS);

	/* --- DUA: init --- */
	fprintf(stderr, "\n--- DUA init ---\n");
	sess = dua_open();
	if (!sess) { fprintf(stderr, "dua_open failed\n"); goto cleanup; }

	TRY_DUA(dua_init_hw(sess), "init_hw");
	TRY_DUA(dua_appl_init(sess), "appl_init");

	/* --- FXS units: allocate, mode 1, connect --- */
	fprintf(stderr, "\n--- FXS units ---\n");
	for (int i = 0; i < TOTAL_FXS; i++) {
		TRY_DUA(dua_unit_allocate(sess, DUA_UT_FXS, i, &all_fxs[i]),
		        "fxs_alloc");
		TRY_DUA(dua_set_umt_mode(sess, all_fxs[i], DUA_UMT_FXS_DSP_PIPELINE),
		        "fxs_mode1");
		TRY_DUA(dua_unit_connect(sess, all_fxs[i], -3),
		        "fxs_connect");
		/* the connect(-3) async callback has the assigned conn_id in elem */
		fxs_conn[i] = (dua_conn_t)dua_last_async_elem(sess);
		fprintf(stderr, "  FXS[%d] uid=0x%04x conn=%d\n", i,
		        (unsigned)all_fxs[i] & 0xffff, fxs_conn[i]);
	}

	/* --- VOIP units: allocate + connect to FXS connections --- */
	fprintf(stderr, "\n--- VOIP units ---\n");
	for (int i = 0; i < TOTAL_VOIP; i++) {
		TRY_DUA(dua_unit_allocate(sess, DUA_UT_SPVOIPNDA, i, &all_voip[i]),
		        "voip_alloc");
		/* stock connects each VOIP to its paired FXS connection:
		 * VOIP[0,1] → FXS[0], VOIP[2,3] → FXS[1], etc. */
		dua_conn_t target_conn = fxs_conn[i / 2];
		TRY_DUA(dua_unit_connect(sess, all_voip[i], target_conn),
		        "voip_connect");
		fprintf(stderr, "  VOIP[%d] uid=0x%04x -> conn %d\n", i,
		        (unsigned)all_voip[i] & 0xffff, target_conn);
	}

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
		fprintf(stderr, "tdm grant: OK (check dmesg!)\n");
	}

	/* --- merge connections for intercom --- */
	fprintf(stderr, "\n--- intercom routing ---\n");
	fprintf(stderr, "  merging conn %d (port %d) + conn %d (port %d)\n",
	        fxs_conn[port_a], port_a, fxs_conn[port_b], port_b);
	TRY_DUA(dua_conn_merge(sess, fxs_conn[port_a], fxs_conn[port_b]),
	        "conn_merge");
	merged = 1;

	/* --- activate SLIC lines --- */
	fprintf(stderr, "\n--- activating lines ---\n");
	for (int i = 0; i < TOTAL_FXS; i++)
		tapi_line_feed_set(tapi_ports[i], IFX_TAPI_LINE_FEED_ACTIVE);
	fprintf(stderr, "  all lines active\n");

	/* --- hold (unless --quick) --- */
	if (quick_mode) {
		fprintf(stderr, "\n=== quick mode: skipping hold, going to teardown ===\n");
		usleep(500000); /* let CSS settle */
	} else {
		fprintf(stderr, "\n=== intercom active! pick up ports %d and %d ===\n",
		        port_a, port_b);
		fprintf(stderr, "Ctrl+C to tear down\n\n");

		while (running)
			sleep(1);
	}

	/* --- teardown (reverse order) --- */
	fprintf(stderr, "\n--- teardown ---\n");

	/* deactivate lines */
	for (int i = 0; i < TOTAL_FXS; i++) {
		if (tapi_ports[i])
			tapi_line_feed_set(tapi_ports[i], IFX_TAPI_LINE_FEED_STANDBY);
	}
	fprintf(stderr, "  lines standby\n");

	/* unmerge intercom */
	if (merged)
		dua_conn_unmerge(sess, fxs_conn[port_a]);

	/* disconnect and free VOIP units */
	for (int i = TOTAL_VOIP - 1; i >= 0; i--) {
		dua_unit_disconnect(sess, all_voip[i], fxs_conn[i / 2]);
		dua_unit_free(sess, all_voip[i]);
	}

	/* disconnect and free FXS units */
	for (int i = TOTAL_FXS - 1; i >= 0; i--) {
		dua_unit_disconnect(sess, all_fxs[i], fxs_conn[i]);
		dua_unit_free(sess, all_fxs[i]);
	}
	fprintf(stderr, "  units freed\n");

cleanup:
	if (sess) dua_close(sess);
	for (int i = 0; i < TOTAL_FXS; i++) {
		if (tapi_ports[i]) {
			tapi_line_feed_set(tapi_ports[i], IFX_TAPI_LINE_FEED_STANDBY);
			tapi_port_close(tapi_ports[i]);
		}
	}
	fprintf(stderr, "done\n");
	return 0;
}
