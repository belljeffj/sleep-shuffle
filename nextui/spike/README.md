# RG SP test pak

- **Written by:** AI (Claude)
- **What it is:** A throwaway test, not the real app. It checks three things on the Anbernic RG SP before the real NextUI version is built:
  - Quick Connect sign-in works from the handheld.
  - The H700 plays a 640x480 H.264 stream from Jellyfin smoothly, and how warm and battery-hungry that is.
  - Jellyfin serves an audio-only stream for a TV episode, so turning video off really saves work.

## Install
1. Download `SleepShuffleTest-h700.zip` from the newest `nextui-test-*` pre-release.
2. Unzip it at the root of the NextUI SD card. It adds `Tools/h700/Sleep Shuffle Test.pak`.
3. Create `SleepShuffle/config.txt` at the root of the card with one line, your Jellyfin home address:
   `server=192.168.86.20:8096` (the test reads this file; the real app will let you type it on the device).
4. In Jellyfin, turn on Quick Connect: Dashboard > General > Enable Quick Connect.

## Use
- Open Tools > Sleep Shuffle Test. Enter the code it shows in the Jellyfin app on your phone (Settings > Quick Connect).
- It plays a random episode. A switches between video and audio only (the screen turns off after 5 seconds of audio only; any button wakes it). X shows stats, Y plays another episode, B or START quits.

## What to send back
- `SleepShuffleTest.txt` from NextUI's logs folder (or `SleepShuffle/` on the card). It records the buttons you pressed, decode speed, dropped frames, temperature and battery every 10 seconds.
- Whether the video looked smooth, whether audio and picture stayed in sync, and how warm the device felt.

## Desktop test
`make` builds it for Linux with SDL2 and FFmpeg dev packages. `test/run_desktop_test.sh` runs it against a mock Jellyfin.
