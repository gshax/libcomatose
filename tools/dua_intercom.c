/*
 * dua_intercom - connect two FXS ports through the DUA for intercom audio
 *
 * allocates ALL FXS units (required for CSS TDM geometry), then sets up
 * SPVOIPNDA units for the two desired ports, connects them, and merges.
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
	fprintf(stderr, "%s: OK\n", label); \
} while(0)

#define TRY_DUA(expr, label) do { \
	comatose_result_t _r = (expr); \
	if (_r != COMATOSE_OK) { \
		fprintf(stderr, "%s failed: %s (dua_err=%d)\n", \
		        label, result_str(_r), dua_last_error(sess)); \
		goto cleanup; \
	} \
	fprintf(stderr, "%s: OK\n", label); \
} while(0)

int main(int argc, char *argv[])
{
	int port_a = 0, port_b = 1;

	if (argc >= 3) {
		port_a = atoi(argv[1]);
		port_b = atoi(argv[2]);
	}

	fprintf(stderr, "=== dua_intercom: connecting port %d <-> port %d ===\n\n",
	        port_a, port_b);

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	tapi_bsp_t *bsp = NULL;
	tapi_port_t *fxs_a = NULL, *fxs_b = NULL;
	dua_session_t *sess = NULL;
	ht_bsp_init_result_t bsp_info;

	/* --- TAPI: init BSP and activate lines --- */
	fprintf(stderr, "--- TAPI setup ---\n");

	TRY(tapi_bsp_init(&bsp, &bsp_info), "bsp_init");
	int total_fxs = bsp_info.slic_count * bsp_info.slic_channels;
	if (total_fxs > COMATOSE_MAX_FXS_PORTS)
		total_fxs = COMATOSE_MAX_FXS_PORTS;
	fprintf(stderr, "  %dx%d SLICs = %d FXS ports, %d REN\n",
	        bsp_info.slic_count, bsp_info.slic_channels, total_fxs, bsp_info.ren);

	/* open and activate the two ports we care about */
	TRY(tapi_port_open(&fxs_a, port_a), "port_open(A)");
	TRY(tapi_port_open(&fxs_b, port_b), "port_open(B)");
	TRY(tapi_line_feed_set(fxs_a, IFX_TAPI_LINE_FEED_ACTIVE), "line_feed(A)");
	TRY(tapi_line_feed_set(fxs_b, IFX_TAPI_LINE_FEED_ACTIVE), "line_feed(B)");

	/* --- DUA: init --- */
	fprintf(stderr, "\n--- DUA setup ---\n");

	sess = dua_open();
	if (!sess) {
		fprintf(stderr, "dua_open failed\n");
		goto cleanup;
	}
	fprintf(stderr, "dua_open: OK\n");

	TRY_DUA(dua_init_hw(sess), "init_hw");
	TRY_DUA(dua_appl_init(sess), "appl_init");

	/* --- allocate ALL FXS units ---
	 * the CSS needs the full TDM bus geometry (all ports) before
	 * it can accept a TDM grant. the HT818 has a fixed TDM bus
	 * with total_fxs timeslots, determined by the SLIC hardware. */
	fprintf(stderr, "\n--- allocating all %d FXS units ---\n", total_fxs);
	dua_uid_t all_fxs[COMATOSE_MAX_FXS_PORTS];
	for (int i = 0; i < total_fxs; i++) {
		comatose_result_t r = dua_unit_allocate(sess, DUA_UT_FXS, i, &all_fxs[i]);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "alloc FXS[%d] failed: %s (dua_err=%d)\n",
			        i, result_str(r), dua_last_error(sess));
			goto cleanup;
		}
		fprintf(stderr, "  FXS[%d] uid=0x%04x\n", i, (unsigned)all_fxs[i] & 0xffff);
		usleep(50000);
	}

	/* UMT FXS init on all ports */
	fprintf(stderr, "\n--- UMT FXS init (all ports) ---\n");
	for (int i = 0; i < total_fxs; i++) {
		comatose_result_t r = dua_set_umt_mode(sess, all_fxs[i], DUA_UMT_FXS_INIT);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "umt FXS[%d] failed: %s (dua_err=%d)\n",
			        i, result_str(r), dua_last_error(sess));
			goto cleanup;
		}
		usleep(50000);
	}
	fprintf(stderr, "  all %d FXS UMT init OK\n", total_fxs);

	/* --- TDM assignment + grant ---
	 * first send the UMT bytecode that maps TDM timeslots to DSP FIFOs,
	 * then grant the TDM bus to the CSS via our kernel module. */
	fprintf(stderr, "\n--- TDM setup ---\n");

	TRY_DUA(dua_set_tdm_assignment(sess, all_fxs[0], 0, total_fxs),
	        "tdm_assignment");

	{
		FILE *f = fopen("/proc/comatose/tdm_grant", "w");
		if (!f) {
			fprintf(stderr, "failed to open /proc/comatose/tdm_grant: %s\n",
			        strerror(errno));
			fprintf(stderr, "(did you insmod comatose_tdm.ko?)\n");
		} else {
			/* after dua_set_tdm_assignment, the CSS's TDM instance has
		 * the channel count set at offset 0x28. use total_fxs. */
		fprintf(f, "0 8000 %d 16\n", total_fxs);
			fclose(f);
			fprintf(stderr, "TDM0 granted to CSS (ch=%d)\n", total_fxs);
		}
		usleep(200000);
	}

	/* --- allocate VOIP units for the two intercom ports --- */
	fprintf(stderr, "\n--- VOIP units ---\n");
	dua_uid_t voip_a, voip_b;

	TRY_DUA(dua_unit_allocate(sess, DUA_UT_SPVOIPNDA, 0, &voip_a), "alloc VOIP_A");
	fprintf(stderr, "  voip_a uid = 0x%04x\n", (unsigned)voip_a & 0xffff);
	usleep(50000);

	TRY_DUA(dua_unit_allocate(sess, DUA_UT_SPVOIPNDA, 1, &voip_b), "alloc VOIP_B");
	fprintf(stderr, "  voip_b uid = 0x%04x\n", (unsigned)voip_b & 0xffff);

	TRY_DUA(dua_set_umt_mode(sess, voip_a, DUA_UMT_SPVOIP_NB_20MS), "umt VOIP_A");
	TRY_DUA(dua_set_umt_mode(sess, voip_b, DUA_UMT_SPVOIP_NB_20MS), "umt VOIP_B");

	/* --- create connections and wire up --- */
	fprintf(stderr, "\n--- connections ---\n");

	dua_uid_t fxs_uid_a = all_fxs[port_a];
	dua_uid_t fxs_uid_b = all_fxs[port_b];
	dua_conn_t conn_a, conn_b;

	TRY_DUA(dua_conn_create(sess, &conn_a), "conn_create A");
	usleep(50000);
	TRY_DUA(dua_conn_create(sess, &conn_b), "conn_create B");
	usleep(50000);

	TRY_DUA(dua_unit_connect(sess, fxs_uid_a, conn_a), "FXS_A -> conn_a");
	usleep(100000);
	TRY_DUA(dua_unit_connect(sess, voip_a, conn_a), "VOIP_A -> conn_a");
	usleep(100000);
	TRY_DUA(dua_unit_connect(sess, fxs_uid_b, conn_b), "FXS_B -> conn_b");
	usleep(100000);
	TRY_DUA(dua_unit_connect(sess, voip_b, conn_b), "VOIP_B -> conn_b");
	usleep(100000);

	/* merge the two connections */
	fprintf(stderr, "\n--- merge ---\n");
	TRY_DUA(dua_conn_merge(sess, conn_a, conn_b), "conn_merge A+B");

	fprintf(stderr, "\n=== intercom active! pick up both phones and try talking ===\n");
	fprintf(stderr, "press Ctrl+C to tear down\n\n");

	while (running)
		sleep(1);

	fprintf(stderr, "\n--- tearing down (best effort) ---\n");
	dua_conn_unmerge(sess, conn_a);
	dua_unit_disconnect(sess, voip_a, conn_a);
	dua_unit_disconnect(sess, fxs_uid_a, conn_a);
	dua_unit_disconnect(sess, voip_b, conn_b);
	dua_unit_disconnect(sess, fxs_uid_b, conn_b);
	dua_conn_delete(sess, conn_a);
	dua_conn_delete(sess, conn_b);
	dua_unit_free(sess, voip_a);
	dua_unit_free(sess, voip_b);
	for (int i = 0; i < total_fxs; i++)
		dua_unit_free(sess, all_fxs[i]);
	fprintf(stderr, "teardown complete\n");

cleanup:
	if (sess) dua_close(sess);
	if (fxs_a) {
		tapi_line_feed_set(fxs_a, IFX_TAPI_LINE_FEED_STANDBY);
		tapi_port_close(fxs_a);
	}
	if (fxs_b) {
		tapi_line_feed_set(fxs_b, IFX_TAPI_LINE_FEED_STANDBY);
		tapi_port_close(fxs_b);
	}
	if (bsp) tapi_bsp_close(bsp);

	return 0;
}
