/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Variable delay on an already-encoded stream (audio + video).
 *
 * Every packet coming from the encoders is kept in a timeline, indexed by arrival time.
 * A pacer thread releases the packet at `cursor` once it is `delay` old. Changing the delay
 * moves the cursor to a video keyframe (back = viewers see the last seconds again, forward =
 * skipped) and rewrites timestamps so they keep increasing: the RTMP connection never breaks.
 */

#define SWD_MAX_DELAY_SEC 120

typedef void (*swd_send_fn)(void *ctx, struct encoder_packet *packet);

struct swd_delay;

struct swd_state {
	bool active;              /* a delayed stream is running */
	uint32_t requested_sec;   /* delay asked for */
	uint32_t effective_ms;    /* age of the last packet sent (what viewers actually get) */
	uint32_t buffered_ms;     /* history kept in memory */
	bool pending;             /* waiting for a keyframe to reduce the delay */
};

struct swd_delay *swd_create(swd_send_fn send, void *ctx);
void swd_destroy(struct swd_delay *d);

/* Output lifecycle */
void swd_start(struct swd_delay *d, uint32_t initial_delay_sec);
void swd_stop(struct swd_delay *d);

/* Called from the output's encoded_packet callback (takes its own reference) */
void swd_push(struct swd_delay *d, struct encoder_packet *packet);

void swd_set_delay(struct swd_delay *d, uint32_t seconds);
void swd_get_state(struct swd_delay *d, struct swd_state *out);

/* The single running delayed output, for hotkeys / obs-websocket (NULL when not streaming) */
void swd_set_running(struct swd_delay *d);
void swd_clear_running(struct swd_delay *d);

/* Delay wanted by the user: applied at stream start and to the running output */
void swd_request_delay(uint32_t seconds);
uint32_t swd_requested_delay(void);
void swd_get_global_state(struct swd_state *out);

#ifdef __cplusplus
}
#endif
