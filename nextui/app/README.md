# Sleep Shuffle for NextUI (Anbernic RG SP)

- **Written by:** AI (Claude)
- **What it is:** The RG SP version of Sleep Shuffle. It shuffles episodes of your chosen shows from Jellyfin, with a sleep timer, and follows the same rules as the Android app (`../../SPEC.md`).
- **Runs on:** NextUI on Anbernic H700 handhelds, built for the RG SP (720x480). Installs as a Tools pak.

## Install
1. Download `SleepShuffle-h700.zip` from the newest `nextui-v*` release.
2. Unzip it at the root of the NextUI SD card. It adds `Tools/h700/Sleep Shuffle.pak`.
3. Open Tools > Sleep Shuffle. The first time, type your Jellyfin home address, approve the Quick Connect code from the Jellyfin app on your phone, and pick your shows.
4. Later updates: Settings > Check for updates, then A to install. The app restarts by itself.

Quick Connect must be on in Jellyfin: Dashboard > General > Enable Quick Connect.

## Controls
- **Lists:** Up and down move, A selects, B goes back. Left and right change a value (sleep timer, repeat window, dimming).
- **Playing:** With nothing on screen, the first button press only shows the controls. Then:
  - A: video off (audio only; the screen turns off after 5 seconds) or video on.
  - Y: next episode.
  - X: menu (sleep timer, skip intro, never shuffle this one, dim, stop).
  - B: stop and go home.
- **Skip intro:** while an intro or recap is playing, X skips it straight away.
- **Sleep timer:** the last 45 seconds fade out sound and picture. Press A during the fade for 15 more minutes. When it ends, the screen turns off; any button wakes it, then A gives 15 more minutes.
- **Power button or closing the lid:** pauses and sleeps, like a game. Playback picks up where it left off.

## Where things are kept
- `/SleepShuffle/` on the SD card: `config.txt` (addresses, sign-in token, shows, settings), `history.txt`, `banned.txt`. Nothing is reported to Jellyfin.
- The log is `SleepShuffle.txt` in NextUI's logs folder (`.userdata/h700/logs`).

## Not yet
- The Tailscale address works only as plain `http://`. A Jellyfin reached through an `https://` Tailscale gateway needs HTTPS support, which is not built in yet.

## Building
- `make` builds a desktop version (SDL2 and FFmpeg dev packages). `test/run_desktop_test.sh` runs it against a mock Jellyfin and saves screenshots.
- Device builds come from `.github/workflows/nextui-release.yml` on a `nextui-v*` tag.
