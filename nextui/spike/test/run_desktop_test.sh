#!/bin/sh
# Desktop self-test: a mock Jellyfin serves a generated HLS stream and MP3, and the test app signs in with
# Quick Connect, plays video, switches to audio only and back, then quits. Needs ffmpeg, SDL2 and FFmpeg dev libs.
# AUDIO_OK=0 makes the mock refuse the audio endpoint, to exercise the fallback.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/media" && cd "$HERE/media"
if [ ! -f main.m3u8 ]; then
  ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=24 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 40 -c:v libx264 -profile:v main -pix_fmt yuv420p -g 48 -c:a aac -b:a 128k -ac 2 \
    -f hls -hls_time 4 -hls_playlist_type vod -hls_segment_filename 'seg%03d.ts' main.m3u8
  printf '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1400000,RESOLUTION=640x360\nmain.m3u8\n' > master.m3u8
  ffmpeg -loglevel error -y -f lavfi -i sine=frequency=330:sample_rate=44100 -t 40 -c:a libmp3lame -b:a 128k audio.mp3
fi
python3 "$HERE/mock_jellyfin.py" 18096 & MOCK=$!
trap 'kill $MOCK' EXIT
sleep 1
OUT=$(mktemp -d)
SS_SERVER=127.0.0.1:18096 SS_AUTOTEST=1 SS_WINDOWED=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout 60 "$HERE/../sleep-shuffle-test" "$OUT" "$OUT"
cat "$OUT/SleepShuffleTest.txt"
grep -q "signed in as jeff" "$OUT/SleepShuffleTest.txt"
grep -q "switching to audio only" "$OUT/SleepShuffleTest.txt"
grep -q "switching to video" "$OUT/SleepShuffleTest.txt"
echo "PASS"
