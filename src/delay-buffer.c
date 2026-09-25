/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "delay-buffer.h"

#include <plugin-support.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>

#define NS_PER_SEC 1000000000ULL
#define NS_PER_MS 1000000ULL
/* History kept beyond the current cursor, so the delay can be raised by rewinding. */
#define HISTORY_NS ((uint64_t)(SWD_MAX_DELAY_SEC + 5) * NS_PER_SEC)
#define PRUNE_EVERY_NS NS_PER_SEC
#define PACER_WAKE_MS 10

struct swd_entry {
	struct encoder_packet packet; /* our own reference */
	uint64_t arrival_ns;
};

struct swd_delay {
	swd_send_fn send;
	void *ctx;

	pthread_mutex_t mutex;
	DARRAY(struct swd_entry) entries;
	size_t cursor;          /* next entry to send */
	uint64_t delay_ns;      /* current target delay */
	uint64_t hold_delay_ns; /* delay kept while a forward jump waits for a keyframe */
	bool pending_skip;
	uint64_t pending_target_ns;

	/* timestamp continuity across jumps */
	int64_t offset_usec;
	bool resync;
	int64_t last_video_out_usec;
	int64_t last_audio_out_usec;
	int64_t frame_usec;
	uint64_t last_sent_arrival_ns;
	uint64_t last_prune_ns;

	pthread_t thread;
	bool thread_created;
	volatile bool running;
	os_event_t *wake;
};

/* ------------------------------------------------------------------------- */
/* Global control state (hotkeys, obs-websocket)                             */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct swd_delay *g_running = NULL;
static uint32_t g_requested_sec = 0;

void swd_set_running(struct swd_delay *d)
{
	pthread_mutex_lock(&g_mutex);
	g_running = d;
	pthread_mutex_unlock(&g_mutex);
}

void swd_clear_running(struct swd_delay *d)
{
	pthread_mutex_lock(&g_mutex);
	if (g_running == d)
		g_running = NULL;
	pthread_mutex_unlock(&g_mutex);
}

void swd_request_delay(uint32_t seconds)
{
	if (seconds > SWD_MAX_DELAY_SEC)
		seconds = SWD_MAX_DELAY_SEC;
	pthread_mutex_lock(&g_mutex);
	g_requested_sec = seconds;
	if (g_running)
		swd_set_delay(g_running, seconds);
	pthread_mutex_unlock(&g_mutex);
	obs_log(LOG_INFO, "delay requested: %u s", seconds);
}

uint32_t swd_requested_delay(void)
{
	pthread_mutex_lock(&g_mutex);
	uint32_t s = g_requested_sec;
	pthread_mutex_unlock(&g_mutex);
	return s;
}

void swd_get_global_state(struct swd_state *out)
{
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g_mutex);
	if (g_running)
		swd_get_state(g_running, out);
	else
		out->requested_sec = g_requested_sec;
	pthread_mutex_unlock(&g_mutex);
}

/* ------------------------------------------------------------------------- */

static inline bool is_keyframe(const struct swd_entry *e)
{
	return e->packet.type == OBS_ENCODER_VIDEO && e->packet.keyframe;
}

static inline int64_t usec_to_tb(int64_t usec, const struct encoder_packet *p)
{
	/* timebase: seconds = value * num / den */
	return (int64_t)llround((double)usec * (double)p->timebase_den / (1000000.0 * (double)p->timebase_num));
}

static void release_all(struct swd_delay *d)
{
	for (size_t i = 0; i < d->entries.num; i++)
		obs_encoder_packet_release(&d->entries.array[i].packet);
	da_free(d->entries);
	d->cursor = 0;
}

static void reset_locked(struct swd_delay *d, uint32_t delay_sec)
{
	release_all(d);
	d->delay_ns = (uint64_t)delay_sec * NS_PER_SEC;
	d->hold_delay_ns = d->delay_ns;
	d->pending_skip = false;
	d->offset_usec = 0;
	d->resync = false;
	d->last_video_out_usec = INT64_MIN;
	d->last_audio_out_usec = INT64_MIN;
	d->frame_usec = 16667;
	d->last_sent_arrival_ns = 0;
	d->last_prune_ns = 0;
}

/* Drop sent packets older than the rewind history. */
static void prune_locked(struct swd_delay *d, uint64_t now)
{
	if (now - d->last_prune_ns < PRUNE_EVERY_NS)
		return;
	d->last_prune_ns = now;

	size_t n = 0;
	while (n < d->cursor && now - d->entries.array[n].arrival_ns > HISTORY_NS)
		n++;
	if (!n)
		return;
	for (size_t i = 0; i < n; i++)
		obs_encoder_packet_release(&d->entries.array[i].packet);
	da_erase_range(d->entries, 0, n);
	d->cursor -= n;
}

static void jump_to_locked(struct swd_delay *d, size_t index)
{
	d->cursor = index;
	d->resync = true;
}

/* Last keyframe that arrived at or before `target`, anywhere in the timeline. */
static size_t find_keyframe_before(struct swd_delay *d, uint64_t target)
{
	for (size_t i = d->entries.num; i > 0; i--) {
		struct swd_entry *e = &d->entries.array[i - 1];
		if (is_keyframe(e) && e->arrival_ns <= target)
			return i - 1;
	}
	for (size_t i = 0; i < d->entries.num; i++) /* history too short: earliest keyframe */
		if (is_keyframe(&d->entries.array[i]))
			return i;
	return SIZE_MAX;
}

/* First not-yet-sent keyframe that arrived at or after `target`. */
static size_t find_keyframe_after(struct swd_delay *d, uint64_t target)
{
	for (size_t i = d->cursor; i < d->entries.num; i++) {
		struct swd_entry *e = &d->entries.array[i];
		if (is_keyframe(e) && e->arrival_ns >= target)
			return i;
	}
	return SIZE_MAX;
}

static void apply_delay_locked(struct swd_delay *d, uint64_t new_ns)
{
	uint64_t now = os_gettime_ns();
	uint64_t target = now > new_ns ? now - new_ns : 0;

	if (new_ns > d->delay_ns) {
		/* Raise: rewind to the keyframe closest to `target`. Viewers see the last seconds again. */
		size_t k = find_keyframe_before(d, target);
		if (k != SIZE_MAX && k < d->cursor)
			jump_to_locked(d, k);
		d->pending_skip = false;
		d->delay_ns = new_ns;
		d->hold_delay_ns = new_ns;
	} else if (new_ns < d->delay_ns) {
		/* Lower: skip ahead to a keyframe. If none is buffered yet, keep the old pace until one arrives. */
		size_t k = find_keyframe_after(d, target);
		if (k != SIZE_MAX) {
			if (k > d->cursor)
				jump_to_locked(d, k);
			d->pending_skip = false;
			d->hold_delay_ns = new_ns;
		} else {
			d->pending_skip = true;
			d->pending_target_ns = target;
			d->hold_delay_ns = d->delay_ns;
		}
		d->delay_ns = new_ns;
	}
}

void swd_set_delay(struct swd_delay *d, uint32_t seconds)
{
	if (!d)
		return;
	if (seconds > SWD_MAX_DELAY_SEC)
		seconds = SWD_MAX_DELAY_SEC;
	pthread_mutex_lock(&d->mutex);
	apply_delay_locked(d, (uint64_t)seconds * NS_PER_SEC);
	pthread_mutex_unlock(&d->mutex);
	os_event_signal(d->wake);
	obs_log(LOG_INFO, "delay set to %u s", seconds);
}

void swd_get_state(struct swd_delay *d, struct swd_state *out)
{
	memset(out, 0, sizeof(*out));
	if (!d)
		return;
	pthread_mutex_lock(&d->mutex);
	uint64_t now = os_gettime_ns();
	out->active = d->running;
	out->requested_sec = (uint32_t)(d->delay_ns / NS_PER_SEC);
	out->effective_ms = d->last_sent_arrival_ns ? (uint32_t)((now - d->last_sent_arrival_ns) / NS_PER_MS) : 0;
	out->buffered_ms = d->entries.num ? (uint32_t)((now - d->entries.array[0].arrival_ns) / NS_PER_MS) : 0;
	out->pending = d->pending_skip;
	pthread_mutex_unlock(&d->mutex);
}

void swd_push(struct swd_delay *d, struct encoder_packet *packet)
{
	struct swd_entry e;
	obs_encoder_packet_ref(&e.packet, packet);

	pthread_mutex_lock(&d->mutex);
	/* Stamped under the lock so the timeline stays ordered across audio and video encoder threads. */
	e.arrival_ns = os_gettime_ns();
	da_push_back(d->entries, &e);
	if (packet->type == OBS_ENCODER_VIDEO && packet->timebase_den > 0)
		d->frame_usec = (int64_t)(1000000LL * packet->timebase_num / packet->timebase_den);
	pthread_mutex_unlock(&d->mutex);

	os_event_signal(d->wake);
}

/* Rewrites timestamps for continuity. Returns false if the packet would go backwards (dropped). */
static bool prepare_out_locked(struct swd_delay *d, struct swd_entry *e, struct encoder_packet *out)
{
	*out = e->packet;

	if (d->resync) {
		/* First packet after a jump is a video keyframe: place it one frame after the last one sent. */
		int64_t base = d->last_video_out_usec == INT64_MIN ? e->packet.dts_usec + d->offset_usec
								     : d->last_video_out_usec + d->frame_usec;
		d->offset_usec = base - e->packet.dts_usec;
		d->resync = false;
	}

	int64_t out_usec = e->packet.dts_usec + d->offset_usec;
	int64_t *last = e->packet.type == OBS_ENCODER_VIDEO ? &d->last_video_out_usec : &d->last_audio_out_usec;
	if (*last != INT64_MIN && out_usec <= *last)
		return false;
	*last = out_usec;

	if (d->offset_usec) {
		int64_t off = usec_to_tb(d->offset_usec, &e->packet);
		out->dts += off;
		out->pts += off;
		out->dts_usec += d->offset_usec;
		out->sys_dts_usec += d->offset_usec;
	}
	return true;
}

static void *pacer_thread(void *data)
{
	struct swd_delay *d = data;
	os_set_thread_name("snipewatch-delay-pacer");

	DARRAY(struct encoder_packet) batch;
	da_init(batch);

	while (d->running) {
		os_event_timedwait(d->wake, PACER_WAKE_MS);

		pthread_mutex_lock(&d->mutex);
		/* Read the clock under the lock: every buffered packet is then older than `now`. */
		uint64_t now = os_gettime_ns();

		if (d->pending_skip) {
			size_t k = find_keyframe_after(d, d->pending_target_ns);
			if (k != SIZE_MAX) {
				if (k > d->cursor)
					jump_to_locked(d, k);
				d->pending_skip = false;
				d->hold_delay_ns = d->delay_ns;
			}
		}

		uint64_t pace = d->pending_skip ? d->hold_delay_ns : d->delay_ns;
		while (d->cursor < d->entries.num) {
			struct swd_entry *e = &d->entries.array[d->cursor];
			if (now - e->arrival_ns < pace)
				break;
			struct encoder_packet out;
			if (prepare_out_locked(d, e, &out)) {
				da_push_back(batch, &out);
				d->last_sent_arrival_ns = e->arrival_ns;
			}
			d->cursor++;
		}

		prune_locked(d, now);
		pthread_mutex_unlock(&d->mutex);

		/* Send outside the lock, in order. `out` shares the reference kept in the timeline;
		 * the output takes its own reference. */
		for (size_t i = 0; i < batch.num; i++)
			d->send(d->ctx, &batch.array[i]);
		da_resize(batch, 0);
	}

	da_free(batch);
	return NULL;
}

struct swd_delay *swd_create(swd_send_fn send, void *ctx)
{
	struct swd_delay *d = bzalloc(sizeof(*d));
	d->send = send;
	d->ctx = ctx;
	pthread_mutex_init(&d->mutex, NULL);
	os_event_init(&d->wake, OS_EVENT_TYPE_AUTO);
	da_init(d->entries);
	reset_locked(d, 0);
	return d;
}

void swd_start(struct swd_delay *d, uint32_t initial_delay_sec)
{
	swd_stop(d);
	pthread_mutex_lock(&d->mutex);
	reset_locked(d, initial_delay_sec);
	pthread_mutex_unlock(&d->mutex);
	d->running = true;
	d->thread_created = pthread_create(&d->thread, NULL, pacer_thread, d) == 0;
	if (!d->thread_created)
		d->running = false;
	obs_log(LOG_INFO, "delayed stream started (initial delay %u s)", initial_delay_sec);
}

void swd_stop(struct swd_delay *d)
{
	if (!d)
		return;
	if (d->thread_created) {
		d->running = false;
		os_event_signal(d->wake);
		pthread_join(d->thread, NULL);
		d->thread_created = false;
	}
	pthread_mutex_lock(&d->mutex);
	release_all(d);
	pthread_mutex_unlock(&d->mutex);
}

void swd_destroy(struct swd_delay *d)
{
	if (!d)
		return;
	swd_clear_running(d);
	swd_stop(d);
	os_event_destroy(d->wake);
	pthread_mutex_destroy(&d->mutex);
	bfree(d);
}
