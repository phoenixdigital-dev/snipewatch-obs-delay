/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Spike: change the stream delay while live, without the RTMP connection dropping.
 *
 * - Tools menu: switch the streaming service to "SnipeWatch Delay" (keeps your server and key),
 *   and switch back to the original one.
 * - Hotkeys: delay 30 s / 60 s / no delay.
 * - obs-websocket vendor "snipewatch-delay": SetDelay {seconds}, GetState, event DelayChanged.
 * - Link with the SnipeWatch app (remote.c): chat commands and automatic triggers, via a personal token.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "delay-buffer.h"
#include "remote.h"
#include "third-party/obs-websocket-api.h"

/* Qt dialogs (settings-dialog.cpp) */
void swd_open_token_dialog(void);
void swd_open_status_dialog(void);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_output_info swd_output_info;
extern struct obs_service_info swd_service_info;

#define SERVICE_ID "snipewatch_delay_service"
#define ORIGINAL_SERVICE_FILE "original-service.json"

static obs_websocket_vendor vendor = NULL;
static obs_hotkey_id hk_30 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_60 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_off = OBS_INVALID_HOTKEY_ID;

/* ------------------------------------------------------------------------- */
/* Delay changes (shared by hotkeys and obs-websocket)                       */

static void emit_state(void)
{
	if (!vendor)
		return;
	struct swd_state st;
	swd_get_global_state(&st);
	obs_data_t *ev = obs_data_create();
	obs_data_set_bool(ev, "active", st.active);
	obs_data_set_int(ev, "requestedSec", st.requested_sec);
	obs_data_set_int(ev, "effectiveMs", st.effective_ms);
	obs_websocket_vendor_emit_event(vendor, "DelayChanged", ev);
	obs_data_release(ev);
}

static void change_delay(uint32_t seconds)
{
	swd_request_delay(seconds);
	emit_state();
}

/* ------------------------------------------------------------------------- */
/* Tools menu: swap the streaming service                                    */

/* UI thread only: is the delayed service the active streaming service? */
static void refresh_delay_mode(void)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	swd_remote_set_delay_mode(svc && strcmp(obs_service_get_id(svc), SERVICE_ID) == 0);
}

static void enable_delay_mode(void *unused)
{
	UNUSED_PARAMETER(unused);
	if (obs_frontend_streaming_active()) {
		obs_log(LOG_WARNING, "cannot switch service while streaming");
		return;
	}
	obs_service_t *cur = obs_frontend_get_streaming_service();
	if (!cur)
		return;
	if (strcmp(obs_service_get_id(cur), SERVICE_ID) == 0) {
		obs_log(LOG_INFO, "delay mode already enabled");
		return;
	}

	const char *url = obs_service_get_connect_info(cur, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(cur, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	if (!url || !*url || !key || !*key) {
		obs_log(LOG_WARNING, "current service has no server/key, configure the stream first");
		return;
	}
	if (strncmp(url, "rtmp://", 7) != 0) {
		obs_log(LOG_WARNING, "only rtmp:// servers are supported by the spike (got %s)", url);
		return;
	}

	/* Keep the original service to restore it later */
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	obs_data_t *orig = obs_data_create();
	obs_data_set_string(orig, "type", obs_service_get_id(cur));
	obs_data_t *orig_settings = obs_service_get_settings(cur);
	obs_data_set_obj(orig, "settings", orig_settings);
	obs_data_release(orig_settings);
	char *path = obs_module_config_path(ORIGINAL_SERVICE_FILE);
	obs_data_save_json_safe(orig, path, "tmp", "bak");
	bfree(path);
	obs_data_release(orig);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", url);
	obs_data_set_string(settings, "key", key);
	obs_service_t *svc = obs_service_create(SERVICE_ID, "SnipeWatch Delay", settings, NULL);
	obs_data_release(settings);
	if (!svc)
		return;

	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	refresh_delay_mode();
	obs_log(LOG_INFO, "delay mode enabled (server %s)", url);
}

static void disable_delay_mode(void *unused)
{
	UNUSED_PARAMETER(unused);
	if (obs_frontend_streaming_active()) {
		obs_log(LOG_WARNING, "cannot switch service while streaming");
		return;
	}
	char *path = obs_module_config_path(ORIGINAL_SERVICE_FILE);
	obs_data_t *orig = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!orig) {
		obs_log(LOG_WARNING, "no original service saved");
		return;
	}
	obs_data_t *settings = obs_data_get_obj(orig, "settings");
	obs_service_t *svc = obs_service_create(obs_data_get_string(orig, "type"), "default_service", settings, NULL);
	obs_data_release(settings);
	obs_data_release(orig);
	if (!svc)
		return;
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	refresh_delay_mode();
	obs_log(LOG_INFO, "original streaming service restored");
}

/* ------------------------------------------------------------------------- */
/* Frontend events                                                           */

static void frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		refresh_delay_mode();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		refresh_delay_mode();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		swd_remote_set_streaming(true);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		swd_remote_set_streaming(false);
		/* The delay is a live thing: next stream starts without one unless asked again. */
		swd_request_delay(0);
		break;
	default:
		break;
	}
}

static void menu_token(void *unused)
{
	UNUSED_PARAMETER(unused);
	swd_open_token_dialog();
}

static void menu_status(void *unused)
{
	UNUSED_PARAMETER(unused);
	refresh_delay_mode();
	swd_open_status_dialog();
}

/* ------------------------------------------------------------------------- */
/* Hotkeys                                                                   */

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	if (id == hk_30)
		change_delay(30);
	else if (id == hk_60)
		change_delay(60);
	else if (id == hk_off)
		change_delay(0);
}

static void save_hotkeys(obs_data_t *save_data, bool saving, void *unused)
{
	UNUSED_PARAMETER(unused);
	if (saving) {
		obs_data_t *obj = obs_data_create();
		obs_data_array_t *a30 = obs_hotkey_save(hk_30);
		obs_data_array_t *a60 = obs_hotkey_save(hk_60);
		obs_data_array_t *aoff = obs_hotkey_save(hk_off);
		obs_data_set_array(obj, "delay30", a30);
		obs_data_set_array(obj, "delay60", a60);
		obs_data_set_array(obj, "delayOff", aoff);
		obs_data_array_release(a30);
		obs_data_array_release(a60);
		obs_data_array_release(aoff);
		obs_data_set_obj(save_data, "snipewatch-delay", obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(save_data, "snipewatch-delay");
		if (!obj)
			return;
		obs_data_array_t *a30 = obs_data_get_array(obj, "delay30");
		obs_data_array_t *a60 = obs_data_get_array(obj, "delay60");
		obs_data_array_t *aoff = obs_data_get_array(obj, "delayOff");
		obs_hotkey_load(hk_30, a30);
		obs_hotkey_load(hk_60, a60);
		obs_hotkey_load(hk_off, aoff);
		obs_data_array_release(a30);
		obs_data_array_release(a60);
		obs_data_array_release(aoff);
		obs_data_release(obj);
	}
}

/* ------------------------------------------------------------------------- */
/* obs-websocket vendor requests                                             */

static void ws_set_delay(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(priv);
	long long seconds = obs_data_get_int(request, "seconds");
	if (seconds < 0 || seconds > SWD_MAX_DELAY_SEC) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "seconds must be between 0 and 120");
		return;
	}
	change_delay((uint32_t)seconds);
	obs_data_set_bool(response, "ok", true);
}

static void ws_get_state(obs_data_t *request, obs_data_t *response, void *priv)
{
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(priv);
	struct swd_state st;
	swd_get_global_state(&st);
	obs_data_set_bool(response, "active", st.active);
	obs_data_set_int(response, "requestedSec", st.requested_sec);
	obs_data_set_int(response, "effectiveMs", st.effective_ms);
	obs_data_set_int(response, "bufferedMs", st.buffered_ms);
	obs_data_set_bool(response, "pending", st.pending);
	obs_service_t *svc = obs_frontend_get_streaming_service();
	obs_data_set_bool(response, "delayModeEnabled", svc && strcmp(obs_service_get_id(svc), SERVICE_ID) == 0);
}

/* ------------------------------------------------------------------------- */

bool obs_module_load(void)
{
	obs_register_output(&swd_output_info);
	obs_register_service(&swd_service_info);

	hk_30 = obs_hotkey_register_frontend("snipewatch_delay_30", obs_module_text("HotkeyDelay30"), hotkey_cb, NULL);
	hk_60 = obs_hotkey_register_frontend("snipewatch_delay_60", obs_module_text("HotkeyDelay60"), hotkey_cb, NULL);
	hk_off = obs_hotkey_register_frontend("snipewatch_delay_off", obs_module_text("HotkeyDelayOff"), hotkey_cb, NULL);
	obs_frontend_add_save_callback(save_hotkeys, NULL);

	obs_frontend_add_tools_menu_item(obs_module_text("MenuEnable"), enable_delay_mode, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("MenuDisable"), disable_delay_mode, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("MenuToken"), menu_token, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("MenuStatus"), menu_status, NULL);
	obs_frontend_add_event_callback(frontend_event, NULL);

	swd_remote_start(change_delay);

	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	vendor = obs_websocket_register_vendor("snipewatch-delay");
	if (!vendor) {
		obs_log(LOG_WARNING, "obs-websocket not available: remote control disabled");
		return;
	}
	obs_websocket_vendor_register_request(vendor, "SetDelay", ws_set_delay, NULL);
	obs_websocket_vendor_register_request(vendor, "GetState", ws_get_state, NULL);
	obs_log(LOG_INFO, "obs-websocket vendor 'snipewatch-delay' registered");
}

void obs_module_unload(void)
{
	swd_remote_stop();
	obs_frontend_remove_event_callback(frontend_event, NULL);
	obs_frontend_remove_save_callback(save_hotkeys, NULL);
	obs_hotkey_unregister(hk_30);
	obs_hotkey_unregister(hk_60);
	obs_hotkey_unregister(hk_off);
	obs_log(LOG_INFO, "plugin unloaded");
}
