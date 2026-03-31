/*
 * bsp_init - initialize BSP and FXS ports using libcomatose
 *
 * usage: bsp_init [--activate]
 *   without --activate: init only, leave lines in standby
 *   with --activate: also set all lines to active (powered)
 */

#include <comatose/comatose.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
	int activate = 0;
	if (argc > 1 && strcmp(argv[1], "--activate") == 0)
		activate = 1;

	/* init BSP */
	tapi_bsp_t *bsp;
	ht_bsp_init_result_t info;
	comatose_result_t ret = tapi_bsp_init(&bsp, &info);
	if (ret != COMATOSE_OK) {
		fprintf(stderr, "tapi_bsp_init failed: %d\n", ret);
		return 1;
	}

	printf("%dx%d SLIC channels, %dx%d DAA channels, %d REN\n",
	       info.slic_count, info.slic_channels,
	       info.daa_count, info.daa_channels,
	       info.ren);

	int total_fxs = info.slic_count * info.slic_channels;
	if (total_fxs > COMATOSE_MAX_FXS_PORTS)
		total_fxs = COMATOSE_MAX_FXS_PORTS;

	/* init each FXS port */
	for (int i = 0; i < total_fxs; i++) {
		tapi_port_t *port;
		ret = tapi_port_open(&port, i);
		if (ret != COMATOSE_OK) {
			fprintf(stderr, "tapi_port_open(%d) failed: %d\n", i, ret);
			continue;
		}

		if (activate) {
			ret = tapi_line_feed_set(port, IFX_TAPI_LINE_FEED_ACTIVE);
			if (ret != COMATOSE_OK)
				fprintf(stderr, "line_feed_set(%d, ACTIVE) failed: %d\n", i, ret);
			else
				printf("port %d: active\n", i);
		} else {
			printf("port %d: initialized\n", i);
		}

		/* don't close -- in a real app we'd keep these open */
		tapi_port_close(port);
	}

	tapi_bsp_close(bsp);
	return 0;
}
