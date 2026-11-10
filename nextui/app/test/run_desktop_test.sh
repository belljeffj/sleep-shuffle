#!/bin/sh
# Desktop self-test: a mock Jellyfin 12 with two shows, and the app driven through sign-in, picking shows,
# Mix, video off and on, the menu, skip intro, next and stop. Screenshots go to test/shots.
# SS_TIMER_SECS=10 instead tests the sleep timer: fade, stop, wake, +15 min.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/media" "$HERE/shots" "$HERE/fakesd/.system/res"
cd "$HERE/media"
if [ ! -f full.ts ]; then
  ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=720x404:rate=24 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 40 -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 48 -c:a aac -b:a 128k -ac 2 -f mpegts full.ts
  ffmpeg -loglevel error -y -i full.ts -c copy -f hls -hls_time 4 -hls_playlist_type vod -hls_segment_filename 'seg%03d.ts' main.m3u8
  printf '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1400000,RESOLUTION=720x404\nmain.m3u8\n' > master.m3u8
  ffmpeg -loglevel error -y -f lavfi -i sine=frequency=330:sample_rate=48000 -t 40 -c:a libmp3lame -b:a 128k audio.mp3
fi
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
[ -f "$FONT" ] && cp "$FONT" "$HERE/fakesd/.system/res/font1.ttf"
python3 "$HERE/mock_jellyfin.py" 18097 & MOCK=$!
trap 'kill $MOCK' EXIT
sleep 1
RUN=$(mktemp -d)
printf 'lan=127.0.0.1:18097/jellyfin\n' > "$RUN/config.txt"
SDCARD_PATH="$HERE/fakesd" SS_SHOTS="$HERE/shots" SS_AUTOTEST=1 SS_WINDOWED=1 SS_SIZE=720x480 \
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 "$HERE/../sleep-shuffle" "$RUN" "$RUN" "$HERE/.."
cat "$RUN/SleepShuffle.txt"
grep -q "signed in as Jeff" "$RUN/SleepShuffle.txt"
if [ -n "${SS_TIMER_SECS:-}" ]; then
  grep -q "sleep timer ended" "$RUN/SleepShuffle.txt"
  grep -q "+15 min" "$RUN/SleepShuffle.txt"
else
  grep -q "switching to audio only" "$RUN/SleepShuffle.txt"
  grep -q "switching to video" "$RUN/SleepShuffle.txt"
fi
! grep -q CRASH "$RUN/SleepShuffle.txt"
echo "PASS"
