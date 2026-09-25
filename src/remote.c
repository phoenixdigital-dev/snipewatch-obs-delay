/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "remote.h"
#include "delay-buffer.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#define CONFIG_FILE "remote.json"
#define POLL_LIVE_MS 3000
#define POLL_IDLE_MS 30000
#define POLL_ERROR_MS 10000

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static struct dstr token;
static struct dstr server;
static volatile bool streaming = false;
static volatile bool delay_mode = false;

static swd_remote_apply_fn apply_cb = NULL;
static pthread_t thread;
static bool thread_created = false;
static volatile bool running = false;
static os_event_t *wake = NULL;

/* last exchange, for the status dialog */
static time_t last_ok = 0;
static int last_http = 0;
static struct dstr last_error;
static long long last_desired = -1;
static struct dstr last_reason;
static bool last_control = false;

/* ------------------------------------------------------------------------- */
/* Config                                                                    */

static void load_config(void)
{
	char *path = obs_module_config_path(CONFIG_FILE);
	obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	dstr_copy(&server, SWD_DEFAULT_SERVER);
	if (!d)
		return;
	dstr_copy(&token, obs_data_get_string(d, "token"));
	const char *s = obs_data_get_string(d, "server");
	if (s && *s)
		dstr_copy(&server, s);
	obs_data_release(d);
}

static void save_config(void)
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "token", token.array ? token.array : "");
	obs_data_set_string(d, "server", server.array ? server.array : SWD_DEFAULT_SERVER);
	char *path = obs_module_config_path(CONFIG_FILE);
	obs_data_save_json_safe(d, path, "tmp", "bak");
	bfree(path);
	obs_data_release(d);
}

void swd_remote_set_token(const char *t)
{
	pthread_mutex_lock(&mutex);
	dstr_copy(&token, t ? t : "");
	dstr_free(&last_error);
	last_ok = 0;
	save_config();
	pthread_mutex_unlock(&mutex);
	if (wake)
		os_event_signal(wake);
	obs_log(LOG_INFO, "SnipeWatch token %s", t && *t ? "updated" : "cleared");
}

bool swd_remote_has_token(void)
{
	pthread_mutex_lock(&mutex);
	bool has = !dstr_is_empty(&token);
	pthread_mutex_unlock(&mutex);
	return has;
}

void swd_remote_set_streaming(bool s)
{
	streaming = s;
	if (wake)
		os_event_signal(wake);
}

void swd_remote_set_delay_mode(bool enabled)
{
	delay_mode = enabled;
}

/* ------------------------------------------------------------------------- */
/* HTTP (WinHTTP on Windows: built into the system, nothing to bundle)       */

#ifdef _WIN32
static wchar_t *to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = bmalloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

/* GET url with a bearer token. Returns the HTTP status (0 on network error), body in `out`. */
static int http_get(const char *url, const char *bearer, struct dstr *out, struct dstr *err)
{
	int status = 0;
	wchar_t *wurl = to_wide(url);
	URL_COMPONENTS uc = {0};
	wchar_t host[256] = {0};
	wchar_t path[2048] = {0};
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 255;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2047;
	HINTERNET session = NULL, conn = NULL, req = NULL;

	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
		dstr_copy(err, "adresse du serveur invalide");
		goto done;
	}
	session = WinHttpOpen(L"SnipeWatchDelay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			      WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		dstr_copy(err, "WinHTTP indisponible");
		goto done;
	}
	WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
	conn = WinHttpConnect(session, host, uc.nPort, 0);
	if (!conn) {
		dstr_copy(err, "connexion impossible");
		goto done;
	}
	req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
				 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (!req) {
		dstr_copy(err, "requête impossible");
		goto done;
	}
	struct dstr hdr = {0};
	dstr_printf(&hdr, "Authorization: Bearer %s\r\nAccept: application/json\r\n", bearer);
	wchar_t *whdr = to_wide(hdr.array);
	dstr_free(&hdr);
	BOOL sent = WinHttpSendRequest(req, whdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		    WinHttpReceiveResponse(req, NULL);
	bfree(whdr);
	if (!sent) {
		dstr_printf(err, "serveur injoignable (erreur %lu)", GetLastError());
		goto done;
	}

	DWORD code = 0, size = sizeof(code);
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
			    &size, WINHTTP_NO_HEADER_INDEX);
	status = (int)code;

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
			break;
		char *buf = bmalloc(avail + 1);
		DWORD read = 0;
		if (WinHttpReadData(req, buf, avail, &read) && read > 0)
			dstr_ncat(out, buf, read);
		bfree(buf);
		if (out->len > 64 * 1024)
			break;
	}

done:
	if (req)
		WinHttpCloseHandle(req);
	if (conn)
		WinHttpCloseHandle(conn);
	if (session)
		WinHttpCloseHandle(session);
	bfree(wurl);
	return status;
}
#else
static int http_get(const char *url, const char *bearer, struct dstr *out, struct dstr *err)
{
	UNUSED_PARAMETER(url);
	UNUSED_PARAMETER(bearer);
	UNUSED_PARAMETER(out);
	dstr_copy(err, "connexion à SnipeWatch disponible sous Windows uniquement pour l'instant");
	return 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* Poll loop                                                                 */

static int poll_once(void)
{
	struct dstr tok = {0}, srv = {0};
	pthread_mutex_lock(&mutex);
	dstr_copy_dstr(&tok, &token);
	dstr_copy_dstr(&srv, &server);
	pthread_mutex_unlock(&mutex);

	if (dstr_is_empty(&tok)) {
		dstr_free(&tok);
		dstr_free(&srv);
		return POLL_IDLE_MS;
	}

	bool live = streaming;
	struct dstr url = {0}, body = {0}, err = {0};
	dstr_printf(&url, "%s/api/plugin/state?applied=%u&streaming=%d&mode=%d&v=%s", srv.array, swd_requested_delay(),
		    live ? 1 : 0, delay_mode ? 1 : 0, PLUGIN_VERSION);
	int status = http_get(url.array, tok.array, &body, &err);
	int next = live ? POLL_LIVE_MS : POLL_IDLE_MS;

	pthread_mutex_lock(&mutex);
	last_http = status;
	if (status == 200) {
		obs_data_t *d = obs_data_create_from_json(body.array ? body.array : "{}");
		if (d) {
			last_ok = time(NULL);
			dstr_free(&last_error);
			last_control = obs_data_get_bool(d, "control");
			last_desired = obs_data_has_user_value(d, "desiredSec") ? obs_data_get_int(d, "desiredSec") : -1;
			dstr_copy(&last_reason, obs_data_get_string(d, "reason"));
			long long poll = obs_data_get_int(d, "pollMs");
			if (poll >= 1000 && poll <= 120000)
				next = (int)poll;
			obs_data_release(d);
		}
	} else if (status == 401) {
		dstr_copy(&last_error, "jeton refusé : génère-en un nouveau dans SnipeWatch (Réglages)");
		next = POLL_IDLE_MS;
	} else {
		if (dstr_is_empty(&err))
			dstr_printf(&err, "réponse HTTP %d", status);
		dstr_copy_dstr(&last_error, &err);
		next = POLL_ERROR_MS;
	}
	bool control = last_control && status == 200;
	long long desired = last_desired;
	pthread_mutex_unlock(&mutex);

	/* The server decides only while live; the delay is always 0 when the stream starts otherwise. */
	if (control && live && desired >= 0 && (uint32_t)desired != swd_requested_delay() && apply_cb)
		apply_cb((uint32_t)desired);

	dstr_free(&url);
	dstr_free(&body);
	dstr_free(&err);
	dstr_free(&tok);
	dstr_free(&srv);
	return next;
}

static void *poll_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("snipewatch-remote");
	int wait_ms = 2000; /* let OBS finish loading */
	while (running) {
		os_event_timedwait(wake, (unsigned long)wait_ms);
		if (!running)
			break;
		wait_ms = poll_once();
	}
	return NULL;
}

void swd_remote_start(swd_remote_apply_fn apply)
{
	apply_cb = apply;
	load_config();
	os_event_init(&wake, OS_EVENT_TYPE_AUTO);
	running = true;
	thread_created = pthread_create(&thread, NULL, poll_thread, NULL) == 0;
}

void swd_remote_stop(void)
{
	if (thread_created) {
		running = false;
		os_event_signal(wake);
		pthread_join(thread, NULL);
		thread_created = false;
	}
	if (wake) {
		os_event_destroy(wake);
		wake = NULL;
	}
	dstr_free(&token);
	dstr_free(&server);
	dstr_free(&last_error);
	dstr_free(&last_reason);
}

void swd_remote_status(char *buf, size_t size)
{
	struct swd_state st;
	swd_get_global_state(&st);

	pthread_mutex_lock(&mutex);
	struct dstr s = {0};
	if (dstr_is_empty(&token)) {
		dstr_copy(&s, "Pas encore connecté à SnipeWatch : colle ton jeton (Outils → « connexion à SnipeWatch »).");
	} else if (last_ok) {
		long ago = (long)(time(NULL) - last_ok);
		dstr_printf(&s, "Connecté à SnipeWatch (dernier échange il y a %ld s).", ago);
		if (!last_control)
			dstr_cat(&s, "\nLe délai à distance est désactivé dans les réglages SnipeWatch.");
		else if (last_desired >= 0)
			dstr_catf(&s, "\nDélai demandé par SnipeWatch : %lld s%s%s%s.", last_desired,
				  dstr_is_empty(&last_reason) ? "" : " (", dstr_is_empty(&last_reason) ? "" : last_reason.array,
				  dstr_is_empty(&last_reason) ? "" : ")");
	} else {
		dstr_copy(&s, "Connexion à SnipeWatch en cours…");
	}
	if (!dstr_is_empty(&last_error))
		dstr_catf(&s, "\nDernière erreur : %s", last_error.array);
	pthread_mutex_unlock(&mutex);

	dstr_catf(&s, "\n\nMode délai (service de stream) : %s.", delay_mode ? "actif" : "inactif");
	dstr_catf(&s, "\nStream : %s.", streaming ? "en cours" : "arrêté");
	if (st.active)
		dstr_catf(&s, "\nDélai appliqué : %u s (effectif %.1f s).", st.requested_sec, st.effective_ms / 1000.0);
	else
		dstr_catf(&s, "\nDélai au prochain démarrage : %u s.", st.requested_sec);

	snprintf(buf, size, "%s", s.array ? s.array : "");
	dstr_free(&s);
}
