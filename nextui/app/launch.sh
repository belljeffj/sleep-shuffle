#!/bin/sh
# Sleep Shuffle for NextUI (H700). Settings, sign-in, history and bans live in /SleepShuffle at the
# root of the SD card, outside the pak, so updates never touch them.
PAK_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SD="${SDCARD_PATH:-/mnt/SDCARD}"
DATA="$SD/SleepShuffle"
LOG_DIR="${LOGS_PATH:-$DATA}"
# NextUI's sleep helper needs SYSTEM_PATH
export SYSTEM_PATH="${SYSTEM_PATH:-$SD/.system/${PLATFORM:-h700}}"
mkdir -p "$DATA" "$LOG_DIR"
LOG="$LOG_DIR/SleepShuffle.txt"
if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 2000000 ]; then mv -f "$LOG" "$LOG.old"; fi
cd "$PAK_DIR" || exit 1

# The whole loop is read before it runs, so the update step below can safely replace this file.
while true; do
  echo "=== launch $(date) PLATFORM=${PLATFORM:-?} DEVICE=${DEVICE:-?} ===" >> "$LOG"
  ./sleep-shuffle "$DATA" "$LOG_DIR" "$PAK_DIR" >> "$LOG" 2>&1
  rc=$?
  echo "=== exit code $rc ===" >> "$LOG"
  if [ "$rc" = 42 ] && [ -f "$DATA/update/ready" ]; then
    # exit code 42: the app downloaded an update into $DATA/update/pak; install it and restart
    cp -f "$DATA/update/pak/"* "$PAK_DIR/" && rm -rf "$DATA/update" && sync
    echo "=== updated, restarting ===" >> "$LOG"
    exec sh "$PAK_DIR/launch.sh"
  fi
  break
done
sync
