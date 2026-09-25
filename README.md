# SnipeWatch Delay for OBS

Change your stream delay **while live**, without the stream dropping. Part of the SnipeWatch anti stream-sniping tools of [app.cs2rouen.fr](https://app.cs2rouen.fr).

## How it works

OBS's built-in stream delay is locked when the stream starts. This plugin adds its own RTMP output (derived from OBS's `obs-outputs`) with a variable delay in front of the send queue:

- every encoded packet (audio + video) is kept in a timeline;
- a pacer releases each packet once it is `delay` seconds old;
- **raising** the delay rewinds to a keyframe: viewers see the last seconds again, nothing new leaks;
- **lowering** it skips ahead to a keyframe;
- timestamps are rewritten so they keep increasing: Twitch never sees a break.

Nothing is re-encoded. Memory: about 1 MB per second of history at 6 Mbps (up to 120 s).

## Spike: how to test

1. Install the plugin (copy the build output into OBS's plugin folder), restart OBS.
2. Configure your stream normally (Settings → Stream, Twitch, **rtmp://** server).
3. **Tools → SnipeWatch Delay: enable delay mode** (keeps your server and key).
4. Settings → Hotkeys: bind *SnipeWatch: 30 s delay* / *60 s* / *no delay*.
5. Start streaming — ideally a Twitch bandwidth test (append `?bandwidthtest=true` to the stream key) — and watch from another device while pressing the hotkeys.
6. **Tools → SnipeWatch Delay: restore original service** to go back.

Remote control: obs-websocket vendor `snipewatch-delay`, requests `SetDelay {"seconds": 30}` and `GetState`, event `DelayChanged`.

## Link with SnipeWatch (v0.2)

With a personal token (SnipeWatch → Settings → SnipeWatch → *Plugin token*, pasted via **Tools → SnipeWatch Delay: connect to SnipeWatch**), the plugin asks the app every 3 s while live (30 s otherwise) which delay to apply. The app decides from:

- **chat commands** (`!delay`, `!delay30`, `!delay 60`, `!stopdelay`, `!nodelay`), from the streamer and, if allowed, mods / VIPs;
- **automatic triggers**: start of each FACEIT match, suspect nickname detected by SnipeWatch, end of the match.

What the plugin sends: its token, the delay it applies, whether it is streaming and in delay mode, its version. What it receives: a number of seconds. Nothing else (see `src/remote.c`). **Tools → SnipeWatch Delay: status** shows the connection state.

## Spike result (25 Sept. 2026)

Tested live on Twitch by the author with many back-and-forth changes (0 ↔ 30 ↔ 60 s):
no disconnection (Twitch Inspector), audio/video stay in sync. Viewers see a raise after ~6 s
and a removal after ~12 s: that is the Twitch player buffer and pipeline, plus waiting for the
next keyframe (every 2 s) when lowering the delay.

## Spike limitations

- Plain RTMP only (no RTMPS), H.264 recommended.
- Editing Settings → Stream while delay mode is on replaces the service: re-enable delay mode afterwards.
- Stopping the stream drops the delayed tail (last `delay` seconds).
- Windows build only for now.

## License

GPL-2.0-or-later. The RTMP output code comes from [OBS Studio](https://github.com/obsproject/obs-studio) (`plugins/obs-outputs`, GPL-2.0-or-later; librtmp LGPL-2.1).
