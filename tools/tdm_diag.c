/*
 * tdm_diag - TDM assignment diagnostic tool
 *
 * tests UMT bytecode execution via LOAD_DYN + EXEC_DYN and IMMEDIATE paths.
 * uses a memory canary to detect if clearTDMAssignment actually runs.
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* build a UMT blob: clearTDM(tdm_id) + END */
static size_t build_clear_blob(uint8_t *buf, uint32_t tdm_id)
{
	uint8_t *p = buf;
	/* opcode 23, elem 3 (CLEAR_TDM), params: tdm_id, 0, 0 */
	*(uint16_t *)p = (uint16_t)(((3 & 0x7ff) << 5) | (23 & 0x1f));
	p += 2;
	*(uint32_t *)p = tdm_id; p += 4;
	*(uint32_t *)p = 0; p += 4;
	*(uint32_t *)p = 0; p += 4;
	/* END (opcode 31) */
	*(uint16_t *)p = (uint16_t)(31 & 0x1f);
	p += 2;
	return (size_t)(p - buf);
}

static void write_canary(void)
{
	(void)system("/tmp/css_shell -c 'memory set 0x02439630 1 aa' > /dev/null 2>&1");
	(void)system("/tmp/css_shell -c 'memory set 0x02439631 1 bb' > /dev/null 2>&1");
}

static void check_canary(const char *label)
{
	fprintf(stderr, "%s canary check: ", label);
	fflush(stderr);
	(void)system("/tmp/css_shell -c 'memory dump 0x02439630 4'");
}

int main(void)
{
	if (!coma_css_ready()) {
		fprintf(stderr, "CSS not ready!\n");
		return 1;
	}

	fprintf(stderr, "--- DUA setup ---\n");
	dua_session_t *sess = dua_open();
	if (!sess) { fprintf(stderr, "dua_open failed\n"); return 1; }

	comatose_result_t r;

	r = dua_init_hw(sess);
	fprintf(stderr, "init_hw: %s (err=%d)\n", r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
	if (r != COMATOSE_OK) goto done;

	r = dua_appl_init(sess);
	fprintf(stderr, "appl_init: %s\n", r == COMATOSE_OK ? "OK" : "FAIL");
	if (r != COMATOSE_OK) goto done;

	/* allocate FXS[0] only */
	dua_uid_t fxs0;
	r = dua_unit_allocate(sess, DUA_UT_FXS, 0, &fxs0);
	fprintf(stderr, "alloc FXS[0]: %s (uid=0x%04x)\n",
	        r == COMATOSE_OK ? "OK" : "FAIL", (unsigned)fxs0);
	if (r != COMATOSE_OK) goto done;

	/* stock firmware runs UMT mode 1 (DSP pipeline setup) on each FXS
	 * unit before TDM assignment — this creates the FIFOs that TDM maps to */
	r = dua_set_umt_mode(sess, fxs0, DUA_UMT_FXS_DSP_PIPELINE);
	fprintf(stderr, "umt_mode1(uid=0x%x): %s (err=%d)\n",
	        (unsigned)fxs0, r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));

	/* try adding UnitConnectReq(uid, -3) — stock firmware does this */
	r = dua_unit_connect(sess, fxs0, -3);
	fprintf(stderr, "connect(uid=0x%x, -3): %s (err=%d)\n",
	        (unsigned)fxs0, r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));

	uint8_t blob[64];
	size_t bsz = build_clear_blob(blob, 0);

	/* --- CRASH TEST: clearTDM(tdm_id=0xFF) should panic CSS if bytecode executes --- */
	fprintf(stderr, "\n=== CRASH TEST: clearTDM(tdm_id=255) via IMMEDIATE ===\n");
	fprintf(stderr, "if CSS panics, bytecode IS executing. if CSS stays ready, it is NOT.\n");
	{
		uint8_t crash_blob[64];
		size_t crash_bsz = build_clear_blob(crash_blob, 0xFF);
		r = dua_unit_set(sess, fxs0, -2, DUA_PARAM_UMT_IMMEDIATE,
		                 crash_blob, crash_bsz);
		fprintf(stderr, "crash_test: %s (err=%d)\n",
		        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
		usleep(200000);
		fprintf(stderr, "CSS state: %s\n",
		        coma_css_ready() ? "READY (no execution)" : "PANIC (executed!)");
		if (!coma_css_ready()) goto done;
	}

	/* --- test 2: UMT_LOAD_DYN + UMT_EXEC_DYN --- */
	fprintf(stderr, "\n=== TEST 2: LOAD_DYN + EXEC_DYN ===\n");
	write_canary();
	check_canary("before");

	r = dua_unit_set(sess, fxs0, 0, DUA_PARAM_UMT_LOAD_DYN, blob, bsz);
	fprintf(stderr, "load_dyn(slot=0): %s (err=%d)\n",
	        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));

	if (r == COMATOSE_OK) {
		uint32_t mode = 0;
		r = dua_unit_set(sess, fxs0, -1, DUA_PARAM_UMT_EXEC_DYN, &mode, sizeof(mode));
		fprintf(stderr, "exec_dyn(mode=0): %s (err=%d)\n",
		        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
	}
	check_canary("after ");

	/* --- test 3: full TDM assignment via IMMEDIATE --- */
	fprintf(stderr, "\n=== TEST 3: full TDM assignment ===\n");
	write_canary();
	check_canary("before");
	r = dua_set_tdm_assignment(sess, fxs0, 0, 8);
	fprintf(stderr, "tdm_assign: %s (err=%d)\n",
	        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
	check_canary("after ");

	/* --- test 4: write blob to shared memory, patch dynamic table --- */
	fprintf(stderr, "\n=== TEST 4: blob in shared memory ===\n");
	{
		void *shm = dua_shm_ptr(sess);
		uint32_t shm_addr = (uint32_t)(uintptr_t)shm;
		uint32_t blob_css_addr = shm_addr + 0x100;

		memcpy((char *)shm + 0x100, blob, bsz);
		__sync_synchronize();
		fprintf(stderr, "blob at shm+0x100 (css addr 0x%08x)\n", blob_css_addr);

		/* verify CSS can see it */
		char cmd[128];
		snprintf(cmd, sizeof(cmd),
		         "/tmp/css_shell -c 'memory dump 0x%08x 16'", blob_css_addr);
		(void)system(cmd);

		/* patch dynamic_table[0] → shared memory blob */
		snprintf(cmd, sizeof(cmd),
		         "/tmp/css_shell -c 'memory set 0x022e2458 4 %02x %02x %02x %02x'",
		         blob_css_addr & 0xff, (blob_css_addr >> 8) & 0xff,
		         (blob_css_addr >> 16) & 0xff, (blob_css_addr >> 24) & 0xff);
		(void)system(cmd);

		write_canary();
		check_canary("before");
		uint32_t mode = 0;
		r = dua_unit_set(sess, fxs0, -1, DUA_PARAM_UMT_EXEC_DYN, &mode, sizeof(mode));
		fprintf(stderr, "exec_dyn(shm): %s (err=%d)\n",
		        r == COMATOSE_OK ? "OK" : "FAIL", dua_last_error(sess));
		check_canary("after ");
	}

done:
	fprintf(stderr, "\nclosing DUA session...\n");
	dua_close(sess);
	fprintf(stderr, "CSS state: %s\n", coma_css_ready() ? "ready" : "NOT READY");
	return 0;
}
