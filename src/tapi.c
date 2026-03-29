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
 * BSP
 */

comatose_result_t tapi_bsp_init(tapi_bsp_t **out, ht_bsp_init_result_t *info)
{
	if (!out)
		return COMATOSE_ERR_INVALID;

	if (ensure_dev_node(TAPI_DEV_BSP, TAPI_MAJOR_BSP, 0) < 0)
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

	if (ioctl(fd, IFX_TAPI_CH_INIT, 0) < 0) {
		close(fd);
		return COMATOSE_ERR_IOCTL;
	}

	IFX_TAPI_LINE_TYPE_CFG_t linecfg = {
		.lineType = IFX_TAPI_LINE_TYPE_FXS,
		.nDaaCh = 0,
	};
	if (ioctl(fd, IFX_TAPI_LINE_TYPE_SET, &linecfg) < 0) {
		close(fd);
		return COMATOSE_ERR_IOCTL;
	}

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

comatose_result_t tapi_event_get(tapi_port_t *port, void *buf, size_t buflen)
{
	if (!port || !buf || buflen < 4)
		return COMATOSE_ERR_INVALID;
	memset(buf, 0, buflen);
	if (ioctl(port->fd, IFX_TAPI_EVENT_GET, buf) < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return COMATOSE_ERR_TIMEOUT;
		return COMATOSE_ERR_IOCTL;
	}
	return COMATOSE_OK;
}
