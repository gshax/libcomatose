/*
 * comatose/tapi.h - TAPI ioctl wrapper API
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_TAPI_H
#define COMATOSE_TAPI_H

#include <comatose/types.h>
#include <comatose/tapi_defs.h>

/* opaque handles */
typedef struct tapi_bsp tapi_bsp_t;
typedef struct tapi_port tapi_port_t;

/*
 * BSP initialization
 */

/* initialize the BSP driver. creates device node if needed, opens
 * /dev/slic_bsp, runs HT_BSP_INIT, performs the reset sequence.
 * if info is non-NULL, populates it with model/SLIC configuration. */
comatose_result_t tapi_bsp_init(tapi_bsp_t **out, ht_bsp_init_result_t *info);

/* clean up BSP handle. */
void tapi_bsp_close(tapi_bsp_t *bsp);

/*
 * consolidated init / teardown
 */

/* BSP init + open all discovered FXS ports in one call.
 * ports: array of COMATOSE_MAX_FXS_PORTS pointers (caller provides).
 * max_ports: size of the ports array.
 * bsp_info: if non-NULL, populated with BSP discovery results (SLIC/DAA counts).
 * skip_reset: if true, skip the BSP reset assert/clear sequence.
 * returns number of FXS ports opened (>= 0), or -1 on fatal error. */
int tapi_init_all(tapi_port_t **ports, int max_ports,
                  ht_bsp_init_result_t *bsp_info, int skip_reset);

/* standby all lines and close all ports. num_ports from tapi_init_all return. */
void tapi_close_all(tapi_port_t **ports, int num_ports);

/*
 * per-port FXS control
 */

/* open and initialize an FXS port. creates /dev/fxsN device node if needed,
 * runs CH_INIT and LINE_TYPE_SET. port_index is 0-based. */
comatose_result_t tapi_port_open(tapi_port_t **out, int port_index);

/* close an FXS port. */
void tapi_port_close(tapi_port_t *port);

/* get the underlying fd (for poll/select on events). */
int tapi_port_fd(const tapi_port_t *port);

/*
 * line control
 */

/* set line feed state. see enum tapi_line_feed. */
comatose_result_t tapi_line_feed_set(tapi_port_t *port, int feed_state);

/* get hook status. *status: 0 = on-hook, 1 = off-hook. */
comatose_result_t tapi_hook_status_get(tapi_port_t *port, int *status);

/*
 * ring control
 */

/* start ringing on a port. */
comatose_result_t tapi_ring_start(tapi_port_t *port);

/* stop ringing on a port. */
comatose_result_t tapi_ring_stop(tapi_port_t *port);

/*
 * tone generation
 */

/* play a local tone (tone_code is a TAPI tone table index). */
comatose_result_t tapi_tone_local_play(tapi_port_t *port, int tone_code);

/* stop tone playback. */
comatose_result_t tapi_tone_stop(tapi_port_t *port);

/*
 * event handling
 *
 * note: the event structure is currently opaque until we nail down the
 * exact grandstream-mutated IFX_TAPI_EVENT_t layout. use tapi_port_fd()
 * with poll() to detect when events are pending, then read them with
 * tapi_event_get(). the raw event buffer can be inspected for now.
 */

/* get next pending event. returns COMATOSE_OK if an event was available,
 * COMATOSE_ERR_TIMEOUT if none pending. buf must be at least 64 bytes. */
comatose_result_t tapi_event_get(tapi_port_t *port, void *buf, size_t buflen);

#endif /* COMATOSE_TAPI_H */
