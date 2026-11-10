# Sleep Shuffle behavior spec

- **Written by:** AI (Claude)
- **Purpose:** The rules both versions follow. The Android app (`android/`) and the NextUI pak for the Anbernic RG SP (`nextui/`) share no code, so this file is what keeps them behaving the same. Change it first, then both apps.

## Shuffle
- You pick one show, or a mix of all chosen shows. The mix picks a show evenly at random first, then an episode from that show.
- An episode is skipped if it is among the last N distinct episodes played from its own show (default 50, range 0 to 300 in steps of 10). The window shrinks to one less than the number of pickable episodes, so a small show never runs dry.
- Banned episodes and episodes already queued are never picked. Only if a show has nothing else left does it fall back to any unbanned episode.
- History keeps the last 600 plays.
- Specials (season 0) and episodes with no file (missing or virtual) are not picked.

## Privacy
- Nothing is reported to Jellyfin: no played marks, no resume points, no Continue Watching entries.
- History and bans live on the device only. Devices do not share them.
- The password is never stored. Only the Jellyfin access token is kept.

## Server
- A home address is tried first and a Tailscale address second. Either may be blank. The choice is re-checked when playback drops.
- RG SP: plain `http://` only for now. A Tailscale gateway on `https://` needs HTTPS support that the RG SP build does not have yet.

## Sleep timer
- Options: preset minutes, custom minutes, a number of episodes (1 = stop at the end of this one), or a clock time (today, or tomorrow if that time has passed).
- In the last 45 seconds, volume and picture fade out, then playback stops. During the fade, a "+15 min" action adds 15 minutes and restores volume.

## Playback
- Intros and recaps are skipped using Jellyfin 10.10+ media segments, or the Intro Skipper plugin's older API.
- If the device can't play the original file, it asks Jellyfin for a transcoded stream.

## Updates
- Releases are published on this repo's GitHub Releases. Android tags are `android-v<version>`. NextUI tags are `nextui-v<version>`. Each updater ignores the other's tags.

## Platform differences
- **Android:** touch lock, orientation lock, lock-screen and notification controls, extra on-screen dimmer. It checks for updates on launch.
- **RG SP (NextUI):** While video plays with no controls showing, the first press of any button only brings up the controls over the still-playing video; buttons act on the controls after that. Face buttons follow the SNES labels printed on the device (A right, B bottom, X top, Y left), not SDL's positional names. Quick Connect sign-in, always-transcoded stream sized to the screen (720x480 on the RG SP), a video-off toggle (audio-only stream, screen off), an optional darkening of the picture, and a manual "Check for updates" in Settings. Closing the lid or pressing power pauses and sleeps, like a game, and playback resumes where it was. Switching video on or off opens a fresh stream from the current position instead of seeking. Skip intro is X while the intro plays, or from the player menu. Never shuffle is in the player menu (hold A).

## RG SP findings from the test pak (Nov 2026)
- Jellyfin 12 rejects the `api_key` query parameter. Use `ApiKey=` (Jellyfin carries it into HLS sub-playlists) and the `Authorization` header.
- Jellyfin serves 720x404 Baseline H.264 at about 1.3 Mbps. The H700 plays it at a steady 24 fps with no dropped frames and no audio underruns over 3.5 minutes, at about 1.4 of 4 cores and 51 to 54 C.
- The audio-only endpoint (`/Audio/{id}/stream.mp3`) works for TV episodes and opens in about 1 second.
- Seeking inside the HLS stream after switching back to video stalled for more than 8 seconds while Jellyfin restarted its transcode. The real app should open a fresh stream starting at the position instead.
- The RG SP has no `/sys/class/backlight/*/bl_power`. NextUI blanks the screen through `/sys/class/graphics/fb0/blank`.
