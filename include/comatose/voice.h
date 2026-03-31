/*
 * comatose/voice.h - /dev/voiceN wrapper API
 *
 * Copyright (C) 2026 myriad research
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef COMATOSE_VOICE_H
#define COMATOSE_VOICE_H

#include <comatose/types.h>
#include <comatose/voice_defs.h>

/* open a voice session. session_id maps to /dev/voiceN minor number.
 * the kernel automatically sends GET_SESSION + SET_SESSION_FIFOS to the CSS.
 * returns fd >= 0 on success, -1 on error. */
int voice_open(int session_id);

/* stop session and close fd. safe to call with fd < 0 (no-op). */
void voice_close(int fd);

/* configure codec and start the session.
 * this triggers CMSG_VOICE_REQUEST_START_SESSION on the CSS.
 * requires BGSC codec threads to be running (app_dsp or comatose_dsp). */
comatose_result_t voice_set_codec(int fd, const rtp_session_config *cfg);

/* stop an active session without closing the fd. */
comatose_result_t voice_stop(int fd);

/* build a minimal rtp_session_config for G.711u (PCMU, 8kHz, 20ms).
 * cfg is zeroed and populated with sensible defaults.
 * line_id: VOIP unit instance index.
 * session_id: voice device minor number (typically == line_id). */
void voice_config_g711u(rtp_session_config *cfg, int line_id, int session_id);

/* build a minimal rtp_session_config for L16 (linear 16-bit PCM, 16kHz, 20ms).
 * uses dynamic payload type 96. this is the target codec for comatose_dsp's
 * BGSC implementation (trivial passthrough, no actual encoding needed). */
void voice_config_l16(rtp_session_config *cfg, int line_id, int session_id);

#endif /* COMATOSE_VOICE_H */
