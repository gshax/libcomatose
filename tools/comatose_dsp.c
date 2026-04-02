/*
 * comatose_dsp - all-in-one hardware support daemon
 *
 * initializes the full audio subsystem (BSP, DUA, FXS/VOIP units, TDM)
 * and keeps it alive for client applications that use /dev/voiceN.
 *
 * runs in the foreground; use an init system for daemonization.
 *
 * usage: comatose_dsp [options]
 *   --no-bsp-reset    skip BSP reset assert/deassert
 *   --no-dua-init     skip DUA shared memory + init_hw/appl_init
 *   --no-tdm-grant    skip TDM grant (procfs write)
 *   --no-bgsc         don't start BGSC codec threads
 *   --fxs-count N     override discovered FXS port count
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

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
	int no_bsp_reset = 0;
	int no_dua_init = 0;
	int no_tdm_grant = 0;
	int no_bgsc = 0;
	int bgsc_only = 0;
	int fxs_override = -1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-bsp-reset") == 0)
			no_bsp_reset = 1;
		else if (strcmp(argv[i], "--no-dua-init") == 0)
			no_dua_init = 1;
		else if (strcmp(argv[i], "--no-tdm-grant") == 0)
			no_tdm_grant = 1;
		else if (strcmp(argv[i], "--no-bgsc") == 0)
			no_bgsc = 1;
		else if (strcmp(argv[i], "--bgsc-only") == 0)
			bgsc_only = 1;
		else if (strcmp(argv[i], "--fxs-count") == 0 && i + 1 < argc)
			fxs_override = atoi(argv[++i]);
		else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return 1;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	fprintf(stderr, "=== comatose_dsp ===\n");

	tapi_port_t *ports[COMATOSE_MAX_FXS_PORTS] = {0};
	dua_session_t *sess = NULL;
	comatose_hw_state_t hw = {0};
	bgsc_ctx_t *bgsc = NULL;
	int num_fxs = 0;
	int ret = 1;

	/* --- phase 1: BSP + TAPI --- */
	{
		ht_bsp_init_result_t bsp_info;
		fprintf(stderr, "\n--- BSP ---\n");

		int n = tapi_init_all(ports, COMATOSE_MAX_FXS_PORTS,
		                      &bsp_info, no_bsp_reset);
		if (n < 0) {
			fprintf(stderr, "  BSP/TAPI init failed\n");
			goto cleanup;
		}
		num_fxs = n;

		if (fxs_override > 0 && fxs_override <= num_fxs) {
			fprintf(stderr, "  FXS count override: %d -> %d\n",
			        num_fxs, fxs_override);
			num_fxs = fxs_override;
		}

		fprintf(stderr, "  FXS: %d ports (slic: %d x %d)\n",
		        num_fxs, bsp_info.slic_count, bsp_info.slic_channels);
		if (bsp_info.daa_count > 0)
			fprintf(stderr, "  FXO: %d ports detected (not yet supported)\n",
			        bsp_info.daa_count * bsp_info.daa_channels);
		if (no_bsp_reset)
			fprintf(stderr, "  (BSP reset skipped)\n");
	}

	/* --- phase 2: DUA --- */
	fprintf(stderr, "\n--- DUA ---\n");
	sess = dua_open();
	if (!sess) {
		fprintf(stderr, "  dua_open failed\n");
		goto cleanup;
	}

	if (no_dua_init) {
		fprintf(stderr, "  (DUA init skipped)\n");
	} else {
		comatose_result_t r = dua_init_hw(sess);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "  init_hw failed: %s\n", result_str(r));
			goto cleanup;
		}
		r = dua_appl_init(sess);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "  appl_init failed: %s\n", result_str(r));
			goto cleanup;
		}
		fprintf(stderr, "  DUA initialized\n");
	}

	/* --- phase 3: unit allocation + TDM --- */
	{
		fprintf(stderr, "\n--- units + TDM ---\n");

		/* 1:1 VOIP-to-FXS mapping */
		int num_voip = num_fxs;

		comatose_result_t r = dua_full_init(sess, num_fxs, num_voip, &hw);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "  dua_full_init failed: %s (dua_err=%d)\n",
			        result_str(r), dua_last_error(sess));
			goto cleanup;
		}

		fprintf(stderr, "  %d FXS + %d VOIP units allocated\n",
		        hw.num_fxs, hw.num_voip);
		for (int i = 0; i < hw.num_fxs; i++)
			fprintf(stderr, "    [%d] FXS=0x%04x conn=%d VOIP=0x%04x\n",
			        i, (unsigned)hw.fxs_uids[i] & 0xffff, hw.fxs_conns[i],
			        (unsigned)hw.voip_uids[i] & 0xffff);

		if (no_tdm_grant && !hw.tdm_granted)
			fprintf(stderr, "  (TDM grant skipped)\n");
		else if (hw.tdm_granted)
			fprintf(stderr, "  TDM granted (check dmesg)\n");
	}

	/* --- phase 4: BGSC --- */
	if (no_bgsc) {
		fprintf(stderr, "\n--- BGSC ---\n");
		fprintf(stderr, "  (skipped — rely on external app_dsp)\n");
	} else {
		fprintf(stderr, "\n--- BGSC ---\n");
		void *shm = NULL;
		if (bgsc_only) {
			/* mmap shared memory without DUA init — for use after
			 * stock app_dsp has already populated the shared memory
			 * (kill -9 app_dsp, then run with --bgsc-only) */
			int fd = open("/dev/sharedmem", O_RDWR);
			if (fd >= 0) {
				shm = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE,
				           MAP_SHARED, fd, 0);
				if (shm == MAP_FAILED) shm = NULL;
				fprintf(stderr, "  shm mmap: %p (fd=%d)\n", shm, fd);
			}
		} else {
			shm = no_dua_init ? NULL : dua_shm_ptr(sess);
		}
		if (!shm) {
			fprintf(stderr, "  no shared memory pointer\n");
			goto cleanup;
		}
		bgsc = bgsc_init(shm);
		if (!bgsc) {
			fprintf(stderr, "  bgsc_init failed\n");
			goto cleanup;
		}
		if (bgsc_start(bgsc) != 0) {
			fprintf(stderr, "  bgsc_start failed\n");
			goto cleanup;
		}
	}

	/* --- ready --- */
	fprintf(stderr, "\n=== comatose_dsp ready (%d FXS ports) ===\n", num_fxs);
	fprintf(stderr, "Ctrl+C to shut down\n\n");

	ret = 0;

	/* event loop: poll COMA socket for CSS→ARM events.
	 *
	 * the CSS sends DUA async callbacks for DSP framework events.
	 * critical event 0xf2 = "init phase complete": we respond with
	 * level-ready messages to advance the module readiness cascade
	 * toward RouteCODEC (which enables audio routing).
	 *
	 * the CSS handles its own I-switch setup internally via
	 * p_dspa_msg_CallBack — the ARM must NOT set I-switch bits on
	 * CSS-level elements. */
	{
		int evt_count = 0;
		int f2_count = 0;
		const int F2_MAX = 5; /* respond to at most 5 0xf2 events */

		while (running) {
			if (sess) {
				struct dua_css_event evt;
				int r = dua_poll_event(sess, &evt);
				if (r > 0) {
					evt_count++;
					fprintf(stderr, "css_event[%d]: "
					        "uid=0x%x elem=%d "
					        "result=0x%x type=%d\n",
					        evt_count, evt.uid,
					        evt.elem, evt.result,
					        evt.type);

					if (evt.result == 0xf2 && bgsc &&
					    f2_count < F2_MAX) {
						f2_count++;
						fprintf(stderr,
						        "  -> level-ready "
						        "(%d/%d)\n",
						        f2_count, F2_MAX);
						bgsc_notify_ready(bgsc);
					}
				}
			}
			usleep(10000);
		}
	}

	/* --- teardown --- */
	fprintf(stderr, "\n--- shutting down ---\n");

cleanup:
	/* stop BGSC */
	if (bgsc)
		bgsc_stop(bgsc);

	/* standby all lines */
	for (int i = 0; i < num_fxs; i++) {
		if (ports[i])
			tapi_line_feed_set(ports[i], IFX_TAPI_LINE_FEED_STANDBY);
	}

	/* DUA teardown */
	if (sess && hw.num_fxs > 0)
		dua_full_teardown(sess, &hw);
	if (sess)
		dua_close(sess);

	/* TAPI teardown */
	tapi_close_all(ports, num_fxs);

	fprintf(stderr, "done\n");
	return ret;
}
