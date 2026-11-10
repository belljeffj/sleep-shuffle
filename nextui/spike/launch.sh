#!/bin/sh
# Sleep Shuffle test pak for NextUI H700. Settings and sign-in live outside the pak, in /SleepShuffle
# at the root of the SD card, so replacing the pak never loses them.
PAK_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SD="${SDCARD_PATH:-/mnt/SDCARD}"
DATA="$SD/SleepShuffle"
LOG_DIR="${LOGS_PATH:-$DATA}"
mkdir -p "$DATA" "$LOG_DIR"
LOG="$LOG_DIR/SleepShuffleTest.txt"
echo "=== launch $(date) PLATFORM=${PLATFORM:-?} DEVICE=${DEVICE:-?} SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-} ===" >> "$LOG"
cd "$PAK_DIR" || exit 1
./sleep-shuffle-test "$DATA" "$LOG_DIR" >> "$LOG" 2>&1
echo "=== exit code $? ===" >> "$LOG"
sync
