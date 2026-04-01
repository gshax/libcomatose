/*
 * comatose/bgsc.h - BGSC (background scheduler) codec processing
 *
 * minimal replacement for app_dsp's ARM-side codec processing.
 * handles shared memory initialization, BG level state machine,
 * FTAB dispatch, and CSS heartbeat messaging.
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_BGSC_H
#define COMATOSE_BGSC_H

#include <comatose/types.h>
#include <stdint.h>

/* opaque handle */
typedef struct bgsc_ctx bgsc_ctx_t;

/* initialize BGSC: open shared memory, set up ring buffers and codec state.
 * must be called AFTER dua_init_hw (which creates the shared memory pool).
 * shm_ptr: the shared memory pointer from dua_shm_ptr().
 * returns context handle, or NULL on failure. */
bgsc_ctx_t *bgsc_init(void *shm_ptr);

/* start the 3 BG level threads. returns 0 on success. */
int bgsc_start(bgsc_ctx_t *ctx);

/* stop threads and clean up. */
void bgsc_stop(bgsc_ctx_t *ctx);

/* send "level ready" messages for all 4 BG levels.
 * called when the CSS signals init phase complete (event 0xf2)
 * to advance the module readiness cascade toward RouteCODEC. */
void bgsc_notify_ready(bgsc_ctx_t *ctx);

#endif /* COMATOSE_BGSC_H */
