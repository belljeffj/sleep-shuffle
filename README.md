# Sleep Shuffle

- **Written by:** AI (Claude)
- **What it is:** Shuffles episodes of chosen TV shows from a Jellyfin server, built for falling asleep. Nothing is reported back to Jellyfin.

## Versions
- **Android** (`android/`): the main app. Kotlin, Jetpack Compose, Media3. See `android/README.md`.
- **Anbernic RG SP** (`nextui/`): a NextUI Tools pak for H700 handhelds. In progress.
- **Shared rules:** `SPEC.md`. Both versions follow it.

## Releases
- `android-v*` tags build a signed APK through `.github/workflows/android-release.yml`. The app checks for these on launch and offers to update.
- `nextui-v*` tags will build the pak zip.
