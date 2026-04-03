/*
 * tapi.c - TAPI ioctl wrapper implementation
 */

#include <comatose/tapi.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct tapi_bsp {
	int fd;
};

struct tapi_port {
	int fd;
	int index;
};

/*
 * helper: create a device node if it doesn't exist
 */
static int ensure_dev_node(const char *path, int major, int minor)
{
	struct stat st;
	if (stat(path, &st) == 0)
		return 0; /* already exists */

	if (mknod(path, S_IFCHR | 0660, makedev(major, minor)) < 0) {
		if (errno == EEXIST)
			return 0;
		return -1;
	}
	return 0;
}

/*
 * helper: look up a char device major number from /proc/devices
 * returns major number or -1 on failure
 */
static int find_major(const char *name)
{
	FILE *f = fopen("/proc/devices", "r");
	if (!f)
		return -1;

	char line[128];
	int major = -1;
	while (fgets(line, sizeof(line), f)) {
		int num;
		char devname[64];
		if (sscanf(line, " %d %63s", &num, devname) == 2) {
			if (strcmp(devname, name) == 0) {
				major = num;
				break;
			}
		}
	}
	fclose(f);
	return major;
}

/*
 * BSP
 */

comatose_result_t tapi_bsp_init(tapi_bsp_t **out, ht_bsp_init_result_t *info)
{
	if (!out)
		return COMATOSE_ERR_INVALID;

	/* look up BSP major dynamically since it varies between environments */
	int bsp_major = find_major("slic_bsp");
	if (bsp_major < 0)
		bsp_major = TAPI_MAJOR_BSP; /* fallback to compiled-in default */

	if (ensure_dev_node(TAPI_DEV_BSP, bsp_major, 0) < 0)
		return COMATOSE_ERR_IOCTL;

	int fd = open(TAPI_DEV_BSP, O_RDWR);
	if (fd < 0)
		return COMATOSE_ERR_IOCTL;

	ht_bsp_init_result_t result;
	if (ioctl(fd, HT_BSP_INIT, &result) < 0) {
		close(fd);
		return COMATOSE_ERR_IOCTL;
	}

	/* reset sequence: assert, wait, clear */
	if (ioctl(fd, HT_BSP_RESET_ASSERT, 0) < 0) {
		close(fd);
		return COMATOSE_ERR_IOCTL;
	}
	usleep(500000); /* 500ms (gs_ata waits a full second, this should suffice) */
	if (ioctl(fd, HT_BSP_RESET_CLEAR, 0) < 0) {
		close(fd);
		return COMATOSE_ERR_IOCTL;
	}

	tapi_bsp_t *bsp = calloc(1, sizeof(*bsp));
	if (!bsp) {
		close(fd);
		return COMATOSE_ERR_NOMEM;
	}
	bsp->fd = fd;

	if (info)
		*info = result;
	*out = bsp;
	return COMATOSE_OK;
}

/*
 * consolidated init / teardown
 */

int tapi_init_all(tapi_port_t **ports, int max_ports,
                  ht_bsp_init_result_t *bsp_info, int skip_reset)
{
	if (!ports || max_ports < 1)
		return -1;

	/* BSP init (discover hardware config) */
	tapi_bsp_t *bsp = NULL;
	ht_bsp_init_result_t info;

	if (skip_reset) {
		/* still need to query BSP for port count, but skip reset */
		int bsp_major = find_major("slic_bsp");
		if (bsp_major < 0)
			bsp_major = TAPI_MAJOR_BSP;
		if (ensure_dev_node(TAPI_DEV_BSP, bsp_major, 0) < 0)
			return -1;
		int fd = open(TAPI_DEV_BSP, O_RDWR);
		if (fd < 0)
			return -1;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, HT_BSP_INIT, &info) < 0) {
			close(fd);
			return -1;
		}
		close(fd);
	} else {
		comatose_result_t r = tapi_bsp_init(&bsp, &info);
		if (r != COMATOSE_OK)
			return -1;
		tapi_bsp_close(bsp);
	}

	if (bsp_info)
		*bsp_info = info;

	int num_fxs = info.slic_count * info.slic_channels;
	if (num_fxs > max_ports)
		num_fxs = max_ports;
	if (num_fxs > COMATOSE_MAX_FXS_PORTS)
		num_fxs = COMATOSE_MAX_FXS_PORTS;

	/* open and initialize all FXS ports */
	for (int i = 0; i < num_fxs; i++) {
		comatose_result_t r = tapi_port_open(&ports[i], i);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "tapi_init_all: port %d open failed\n", i);
			for (int j = i - 1; j >= 0; j--)
				tapi_port_close(ports[j]);
			return -1;
		}
		r = tapi_port_init(ports[i]);
		if (r != COMATOSE_OK) {
			fprintf(stderr, "tapi_init_all: port %d init failed\n", i);
			for (int j = i; j >= 0; j--)
				tapi_port_close(ports[j]);
			return -1;
		}
	}

	return num_fxs;
}

void tapi_close_all(tapi_port_t **ports, int num_ports)
{
	if (!ports)
		return;
	for (int i = 0; i < num_ports; i++) {
		if (ports[i]) {
			tapi_line_feed_set(ports[i], IFX_TAPI_LINE_FEED_DISABLED);
			tapi_port_close(ports[i]);
			ports[i] = NULL;
		}
	}
}

void tapi_bsp_close(tapi_bsp_t *bsp)
{
	if (!bsp)
		return;
	if (bsp->fd >= 0)
		close(bsp->fd);
	free(bsp);
}

/*
 * per-port FXS
 */

comatose_result_t tapi_port_open(tapi_port_t **out, int port_index)
{
	if (!out || port_index < 0 || port_index >= COMATOSE_MAX_FXS_PORTS)
		return COMATOSE_ERR_INVALID;

	char path[64];
	snprintf(path, sizeof(path), TAPI_DEV_FXS_FMT, port_index);

	if (ensure_dev_node(path, TAPI_MAJOR_FXS, port_index) < 0)
		return COMATOSE_ERR_IOCTL;

	int fd = open(path, O_RDWR);
	if (fd < 0)
		return COMATOSE_ERR_IOCTL;

	tapi_port_t *port = calloc(1, sizeof(*port));
	if (!port) {
		close(fd);
		return COMATOSE_ERR_NOMEM;
	}
	port->fd = fd;
	port->index = port_index;

	*out = port;
	return COMATOSE_OK;
}

comatose_result_t tapi_port_init(tapi_port_t *port)
{
	if (!port)
		return COMATOSE_ERR_INVALID;

	if (ioctl(port->fd, IFX_TAPI_CH_INIT, 0) < 0)
		return COMATOSE_ERR_IOCTL;

	IFX_TAPI_LINE_TYPE_CFG_t linecfg = {
		.lineType = IFX_TAPI_LINE_TYPE_FXS,
		.nDaaCh = 0,
	};
	if (ioctl(port->fd, IFX_TAPI_LINE_TYPE_SET, &linecfg) < 0)
		return COMATOSE_ERR_IOCTL;

	return COMATOSE_OK;
}

void tapi_port_close(tapi_port_t *port)
{
	if (!port)
		return;
	if (port->fd >= 0)
		close(port->fd);
	free(port);
}

int tapi_port_fd(const tapi_port_t *port)
{
	return port ? port->fd : -1;
}

/*
 * line control
 */

comatose_result_t tapi_line_feed_set(tapi_port_t *port, int feed_state)
{
	if (!port)
		return COMATOSE_ERR_INVALID;
	if (ioctl(port->fd, IFX_TAPI_LINE_FEED_SET, feed_state) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

comatose_result_t tapi_hook_status_get(tapi_port_t *port, int *status)
{
	if (!port || !status)
		return COMATOSE_ERR_INVALID;
	int raw = 0;
	if (ioctl(port->fd, IFX_TAPI_LINE_HOOK_STATUS_GET, &raw) < 0)
		return COMATOSE_ERR_IOCTL;
	*status = (raw != 0) ? 1 : 0;
	return COMATOSE_OK;
}

/*
 * ring control
 */

comatose_result_t tapi_ring_cadence_set(tapi_port_t *port,
                                        const tapi_ring_cadence_t *cadence)
{
	if (!port)
		return COMATOSE_ERR_INVALID;

	tapi_ring_cadence_t cad;
	if (cadence) {
		cad = *cadence;
	} else {
		/* standard NA cadence: 2s on / 4s off
		 * 2s = 40 bits of 1, 4s = 80 bits of 0, total = 120 bits
		 * data[0..4] = 0xff (40 bits), rest = 0 */
		memset(&cad, 0, sizeof(cad));
		cad.data[0] = 0xff;
		cad.data[1] = 0xff;
		cad.data[2] = 0xff;
		cad.data[3] = 0xff;
		cad.data[4] = 0xff;
		cad.nr = 120;
		cad.initialNr = 0;
	}

	if (ioctl(port->fd, IFX_TAPI_RING_CADENCE_HR_SET, &cad) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

comatose_result_t tapi_ring_start(tapi_port_t *port)
{
	if (!port)
		return COMATOSE_ERR_INVALID;
	if (ioctl(port->fd, IFX_TAPI_RING_START, 0) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

comatose_result_t tapi_ring_stop(tapi_port_t *port)
{
	if (!port)
		return COMATOSE_ERR_INVALID;
	if (ioctl(port->fd, IFX_TAPI_RING_STOP, 0) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

/*
 * tones
 */

comatose_result_t tapi_tone_local_play(tapi_port_t *port, int tone_code)
{
	if (!port)
		return COMATOSE_ERR_INVALID;
	if (ioctl(port->fd, IFX_TAPI_TONE_LOCAL_PLAY, tone_code) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

comatose_result_t tapi_tone_stop(tapi_port_t *port)
{
	if (!port)
		return COMATOSE_ERR_INVALID;
	if (ioctl(port->fd, IFX_TAPI_TONE_STOP, 0) < 0)
		return COMATOSE_ERR_IOCTL;
	return COMATOSE_OK;
}

/*
 * events
 */

comatose_result_t tapi_event_get(tapi_port_t *port, tapi_event_t *evt)
{
	if (!port || !evt)
		return COMATOSE_ERR_INVALID;
	memset(evt, 0, sizeof(*evt));
	if (ioctl(port->fd, IFX_TAPI_EVENT_GET, evt) < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return COMATOSE_ERR_TIMEOUT;
		return COMATOSE_ERR_IOCTL;
	}
	/* id == 0 means no event was pending */
	if (evt->id == 0)
		return COMATOSE_ERR_TIMEOUT;
	return COMATOSE_OK;
}
