/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * Streaming service whose only job is to make OBS use our delayed RTMP output.
 * OBS picks the stream output from the service (`get_output_type`); the Settings > Stream page
 * does not list third-party services, so the plugin swaps it in from the Tools menu.
 */

#include <obs-module.h>
#include <util/dstr.h>

#define SERVICE_ID "snipewatch_delay_service"

struct delay_service {
	struct dstr server;
	struct dstr key;
};

static const char *svc_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SnipeWatchDelayService");
}

static void svc_update(void *data, obs_data_t *settings)
{
	struct delay_service *s = data;
	dstr_copy(&s->server, obs_data_get_string(settings, "server"));
	dstr_copy(&s->key, obs_data_get_string(settings, "key"));
}

static void *svc_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);
	struct delay_service *s = bzalloc(sizeof(*s));
	svc_update(s, settings);
	return s;
}

static void svc_destroy(void *data)
{
	struct delay_service *s = data;
	dstr_free(&s->server);
	dstr_free(&s->key);
	bfree(s);
}

static obs_properties_t *svc_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "server", obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "key", obs_module_text("StreamKey"), OBS_TEXT_PASSWORD);
	return props;
}

static const char *svc_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return "snipewatch_delay_output";
}

static const char *svc_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "RTMP";
}

static const char *svc_connect_info(void *data, uint32_t type)
{
	struct delay_service *s = data;
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return s->server.array;
	case OBS_SERVICE_CONNECT_INFO_STREAM_KEY:
		return s->key.array;
	default:
		return NULL;
	}
}

static const char *svc_url(void *data)
{
	return svc_connect_info(data, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
}

static const char *svc_key(void *data)
{
	return svc_connect_info(data, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
}

static bool svc_can_connect(void *data)
{
	struct delay_service *s = data;
	return !dstr_is_empty(&s->server) && !dstr_is_empty(&s->key);
}

static const char *video_codecs[] = {"h264", NULL};
static const char *audio_codecs[] = {"aac", NULL};

static const char **svc_video_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	return video_codecs;
}

static const char **svc_audio_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	return audio_codecs;
}

struct obs_service_info swd_service_info = {
	.id = SERVICE_ID,
	.get_name = svc_name,
	.create = svc_create,
	.destroy = svc_destroy,
	.update = svc_update,
	.get_properties = svc_properties,
	.get_url = svc_url,
	.get_key = svc_key,
	.get_output_type = svc_output_type,
	.get_protocol = svc_protocol,
	.get_connect_info = svc_connect_info,
	.can_try_to_connect = svc_can_connect,
	.get_supported_video_codecs = svc_video_codecs,
	.get_supported_audio_codecs = svc_audio_codecs,
};
