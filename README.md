# Sleep Shuffle

- **Written by:** AI (Claude)
- **What it is:** An Android app that shuffles episodes of chosen shows (How I Met Your Mother and New Girl by default) from a Jellyfin server, built for falling asleep.
- **Stack:** Kotlin, Jetpack Compose, Media3 (ExoPlayer + MediaSession), minSdk 26, compileSdk 36.

## Features

- Pick a show, or mix them evenly, then shuffle. The last N episodes played per show are skipped (default 50, set in Settings).
- Home address first, Tailscale address as the fallback; re-checked automatically if playback drops.
- Nothing is reported to Jellyfin: no played marks, no resume points, no Continue Watching entries. History and bans live on the device.
- Sleep timer by preset minutes, custom minutes, number of episodes, or clock time. The last 45 seconds fade the volume and the screen, then playback stops. A large "+15 min" button appears during the fade.
- Bedtime UI: pure black, dim amber text, an extra dimmer below system brightness, orientation lock, touch lock (hold to unlock), and hold-to-confirm for banning an episode.
- Audio continues with the screen locked, with lock-screen and notification controls.
- Skip intro / recap uses Jellyfin 10.10+ media segments, or the Intro Skipper plugin's older API.
- If the phone can't decode the original file, it falls back to a server-transcoded H.264/AAC HLS stream.

## Build

```sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
echo "sdk.dir=/opt/homebrew/share/android-commandlinetools" > local.properties
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

Updates must be signed with the same key as the installed copy, so keep building on the same Mac (its `~/.android/debug.keystore`).

## Later

- Casting: the player sits behind a MediaSession, so adding Media3's Cast extension is a contained change.
