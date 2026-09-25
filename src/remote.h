/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Link with the SnipeWatch app (app.cs2rouen.fr): the plugin asks "which delay now?" and
 * reports what it applies. Chat commands and automatic triggers happen on the server.
 * Only a personal token is sent; only a number of seconds comes back.
 */

#define SWD_DEFAULT_SERVER "https://app.cs2rouen.fr"

/* Called by the delay setter (plugin-main.c) when the server asks for a new delay. */
typedef void (*swd_remote_apply_fn)(uint32_t seconds);

void swd_remote_start(swd_remote_apply_fn apply);
void swd_remote_stop(void);

void swd_remote_set_token(const char *token);
bool swd_remote_has_token(void);

/* Kept up to date from the UI thread (frontend events). */
void swd_remote_set_streaming(bool streaming);
void swd_remote_set_delay_mode(bool enabled);

/* Human-readable status for the Tools menu dialog (French). */
void swd_remote_status(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
