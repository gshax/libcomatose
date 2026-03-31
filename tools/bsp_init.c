/*
 * bsp_init - initialize BSP and FXS ports using libcomatose
 *
 * usage: bsp_init [--activate] [--no-reset]
 *   --activate: set all lines to active (powered) after init
 *   --no-reset: skip BSP reset assert/deassert
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
	int activate = 0;
	int skip_reset = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--activate") == 0)
			activate = 1;
		else if (strcmp(argv[i], "--no-reset") == 0)
			skip_reset = 1;
	}

	tapi_port_t *ports[COMATOSE_MAX_FXS_PORTS] = {0};
	ht_bsp_init_result_t info;

	int num_fxs = tapi_init_all(ports, COMATOSE_MAX_FXS_PORTS, &info, skip_reset);
	if (num_fxs < 0) {
		fprintf(stderr, "tapi_init_all failed\n");
		return 1;
	}

	printf("%dx%d SLIC channels, %dx%d DAA channels, %d REN\n",
	       info.slic_count, info.slic_channels,
	       info.daa_count, info.daa_channels,
	       info.ren);

	for (int i = 0; i < num_fxs; i++) {
		if (activate) {
			comatose_result_t r = tapi_line_feed_set(ports[i],
				IFX_TAPI_LINE_FEED_ACTIVE);
			if (r != COMATOSE_OK)
				fprintf(stderr, "line_feed_set(%d, ACTIVE) failed: %d\n", i, r);
			else
				printf("port %d: active\n", i);
		} else {
			printf("port %d: initialized\n", i);
		}
	}

	tapi_close_all(ports, num_fxs);
	return 0;
}
