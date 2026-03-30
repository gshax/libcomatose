/*
 * tdm_brute - try all channel counts for TDM grant after DUA setup
 * reloads CSS between each attempt to avoid oops accumulation
 */
#include <comatose/comatose.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

static comatose_result_t do_dua_setup(void)
{
	dua_session_t *sess = dua_open();
	if (!sess) return COMATOSE_ERR_SOCKET;

	comatose_result_t r;
	r = dua_init_hw(sess);
	if (r != COMATOSE_OK) { dua_close(sess); return r; }
	r = dua_appl_init(sess);
	if (r != COMATOSE_OK) { dua_close(sess); return r; }

	/* allocate all 8 FXS */
	dua_uid_t fxs[8];
	for (int i = 0; i < 8; i++) {
		r = dua_unit_allocate(sess, DUA_UT_FXS, i, &fxs[i]);
		if (r != COMATOSE_OK) { dua_close(sess); return r; }
		usleep(50000);
	}

	/* UMT FXS init all */
	for (int i = 0; i < 8; i++) {
		r = dua_set_umt_mode(sess, fxs[i], DUA_UMT_FXS_INIT);
		if (r != COMATOSE_OK) { dua_close(sess); return r; }
		usleep(50000);
	}

	/* TDM assignment */
	r = dua_set_tdm_assignment(sess, fxs[0], 0, 8);
	if (r != COMATOSE_OK) {
		fprintf(stderr, "tdm_assignment failed: %d\n", r);
		dua_close(sess);
		return r;
	}
	fprintf(stderr, "tdm_assignment: OK\n");

	dua_close(sess);
	return COMATOSE_OK;
}

int main(int argc, char *argv[])
{
	int ch_start = 0, ch_end = 16;
	if (argc >= 3) {
		ch_start = atoi(argv[1]);
		ch_end = atoi(argv[2]);
	}

	fprintf(stderr, "--- DUA setup ---\n");
	comatose_result_t r = do_dua_setup();
	if (r != COMATOSE_OK) {
		fprintf(stderr, "DUA setup failed: %d\n", r);
		return 1;
	}
	fprintf(stderr, "DUA setup OK, now brute-forcing TDM grants...\n\n");

	/* blast all channel counts at once via the kernel module */
	{
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "0 8000 %d 16", ch_end);
		FILE *f = fopen("/proc/comatose/tdm_brute", "w");
		if (!f) {
			fprintf(stderr, "can't open /proc/comatose/tdm_brute\n");
			fprintf(stderr, "(did you insmod comatose_tdm.ko?)\n");
			return 1;
		}
		fprintf(f, "%s\n", cmd);
		fclose(f);
		fprintf(stderr, "sent grants for channels 1-%d\n", ch_end);
	}

	/* wait a bit for CSS to process */
	sleep(2);

	/* check CSS state */
	{
		FILE *f = fopen("/sys/devices/platform/8000000.css/state", "r");
		if (f) {
			char state[32] = {0};
			if (fgets(state, sizeof(state), f)) {
				state[strcspn(state, "\n")] = 0;
				fprintf(stderr, "CSS state: %s\n", state);
			}
			fclose(f);
		}
	}

	fprintf(stderr, "check dmesg for nack/ack results\n");
	return 0;
}
