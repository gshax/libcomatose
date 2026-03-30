/*
 * tdm_diag - minimal DUA setup + TDM diagnostics via css_shell
 *
 * does the bare minimum DUA setup (alloc FXS + TDM assignment) then
 * exits cleanly so the CSS isn't left in a busy state.
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	if (!coma_css_ready()) {
		fprintf(stderr, "CSS not ready!\n");
		return 1;
	}

	fprintf(stderr, "--- DUA setup ---\n");
	dua_session_t *sess = dua_open();
	if (!sess) {
		fprintf(stderr, "dua_open failed\n");
		return 1;
	}

	comatose_result_t r;

	r = dua_init_hw(sess);
	fprintf(stderr, "init_hw: %s (err=%d)\n", r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
	if (r != COMATOSE_OK) goto done;

	r = dua_appl_init(sess);
	fprintf(stderr, "appl_init: %s\n", r == COMATOSE_OK ? "OK" : "FAIL");
	if (r != COMATOSE_OK) goto done;

	/* allocate all 8 FXS */
	dua_uid_t fxs[8];
	for (int i = 0; i < 8; i++) {
		r = dua_unit_allocate(sess, DUA_UT_FXS, i, &fxs[i]);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "alloc FXS[%d] failed (err=%d)\n", i, dua_last_error(sess));
			goto done;
		}
		usleep(50000);
	}
	fprintf(stderr, "all 8 FXS allocated\n");

	/* UMT FXS init */
	for (int i = 0; i < 8; i++) {
		r = dua_set_umt_mode(sess, fxs[i], DUA_UMT_FXS_INIT);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "umt FXS[%d] failed\n", i);
			goto done;
		}
		usleep(50000);
	}
	fprintf(stderr, "all 8 FXS UMT init OK\n");

	/* TDM assignment */
	r = dua_set_tdm_assignment(sess, fxs[0], 0, 8);
	fprintf(stderr, "tdm_assignment: %s (err=%d)\n",
	        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));

done:
	fprintf(stderr, "closing DUA session...\n");
	dua_close(sess);
	fprintf(stderr, "done. CSS state: %s\n",
	        coma_css_ready() ? "ready" : "NOT READY");
	return r != COMATOSE_OK;
}
