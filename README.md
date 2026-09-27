# SnipeWatch Delay for OBS

Change your stream delay **while live**, without the stream dropping. Part of the SnipeWatch anti stream-sniping tools of [GetBetterCS](https://www.getbettercs.com).

## How it works

OBS's built-in stream delay is locked when the stream starts. This plugin adds its own RTMP output (derived from OBS's `obs-outputs`) with a variable delay in front of the send queue:

- every encoded packet (audio + video) is kept in a timeline;
- a pacer releases each packet once it is `delay` seconds old;
- **raising** the delay rewinds to a keyframe: viewers see the last seconds again, nothing new leaks;
- **lowering** it skips ahead to a keyframe;
- timestamps are rewritten so they keep increasing: Twitch never sees a break.

Nothing is re-encoded. Memory: about 1 MB per second of history at 6 Mbps (up to 120 s).

## Install (Windows)

Requirements: Windows 10 or 11 (64-bit), OBS Studio 31 or later. Download the files from the [Releases](https://github.com/phoenixdigital-dev/snipewatch-obs-delay/releases) page only: they are built by GitHub Actions from this source code, and their SHA-256 checksums are in the release notes.

> **Not code-signed yet.** The plugin and its installer don't carry a code-signing certificate for now (planned). Windows SmartScreen will show *"Windows protected your PC"* for the installer: click **More info → Run anyway**. Before that, you can check the file is the one published here: in PowerShell, `Get-FileHash .\snipewatch-obs-delay-<version>-windows-x64-setup.exe` must print the SHA-256 listed in the release notes. If you'd rather not run an unsigned installer, use the manual install below: it's just copying a folder.

**Option A: installer**

1. Close OBS.
2. Run `snipewatch-obs-delay-<version>-windows-x64-setup.exe` (administrator rights are needed to write to `C:\ProgramData`).
3. Open OBS: the **Tools → SnipeWatch Delay** menu is there.

It installs into `C:\ProgramData\obs-studio\plugins\snipewatch-obs-delay` (OBS's folder for third-party plugins, kept when OBS updates) and removes an older manual copy from OBS's own folder. Uninstall from *Settings → Apps → GetBetterCS Delay (plugin OBS)*.

**Option B: manual install (zip)**

1. Close OBS.
2. Download `snipewatch-obs-delay-<version>-windows-x64.zip` and extract it: it contains a `snipewatch-obs-delay` folder.
3. Copy that folder into `C:\ProgramData\obs-studio\plugins\` (create `plugins` if it doesn't exist; Windows asks for administrator rights). You should end up with:
   ```
   C:\ProgramData\obs-studio\plugins\snipewatch-obs-delay\bin\64bit\snipewatch-obs-delay.dll
   C:\ProgramData\obs-studio\plugins\snipewatch-obs-delay\data\locale\en-US.ini
   ```
4. If you had copied an older version into OBS's own folder, delete `C:\Program Files\obs-studio\obs-plugins\64bit\snipewatch-obs-delay.dll` and `C:\Program Files\obs-studio\data\obs-plugins\snipewatch-obs-delay` (otherwise OBS loads the plugin twice).
5. Open OBS: the **Tools → SnipeWatch Delay** menu is there. To uninstall, delete the `snipewatch-obs-delay` folder.

### Installation (français)

Windows 10 ou 11 64 bits, OBS Studio 31 ou plus. Télécharge les fichiers uniquement depuis la page [Releases](https://github.com/phoenixdigital-dev/snipewatch-obs-delay/releases) : ils sont compilés par GitHub Actions depuis ce code source, et leurs empreintes SHA-256 sont dans les notes de version.

> **Pas encore signé.** Le plugin et son installeur n'ont pas encore de certificat de signature (prévu). Windows SmartScreen affichera « Windows a protégé votre ordinateur » : clique sur **Informations complémentaires → Exécuter quand même**. Pour vérifier le fichier avant : dans PowerShell, `Get-FileHash .\snipewatch-obs-delay-<version>-windows-x64-setup.exe` doit afficher l'empreinte SHA-256 des notes de version. Si tu préfères ne pas lancer un installeur non signé, fais l'installation manuelle : c'est une simple copie de dossier.

- **Installeur** : ferme OBS, lance `…-windows-x64-setup.exe` (droits administrateur), rouvre OBS : le menu **Outils → SnipeWatch Delay** apparaît. Désinstallation : *Paramètres → Applications → GetBetterCS Delay (plugin OBS)*.
- **Manuel (zip)** : ferme OBS, extrais `…-windows-x64.zip`, copie le dossier `snipewatch-obs-delay` dans `C:\ProgramData\obs-studio\plugins\` (crée `plugins` s'il n'existe pas). Tu dois obtenir `C:\ProgramData\obs-studio\plugins\snipewatch-obs-delay\bin\64bit\snipewatch-obs-delay.dll`. Si une ancienne version avait été copiée dans le dossier d'OBS, supprime `C:\Program Files\obs-studio\obs-plugins\64bit\snipewatch-obs-delay.dll` et `C:\Program Files\obs-studio\data\obs-plugins\snipewatch-obs-delay`. Rouvre OBS. Pour désinstaller, supprime le dossier `snipewatch-obs-delay`.

## Spike: how to test

1. Install the plugin (see *Install* above).
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
