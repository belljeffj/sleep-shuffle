#!/bin/sh
# Sleep Shuffle updater, run by the app (Settings > Check for updates).
#   update.sh check <data dir>           prints "latest <tag>" for the newest nextui-v* release
#   update.sh download <tag> <data dir>  stages that release in <data dir>/update and prints "ready"
# Uses NextUI's curl. The app restarts with exit code 42 and launch.sh swaps the files.
REPO=belljeffj/sleep-shuffle
ASSET=SleepShuffle-h700.zip
SYS="${SYSTEM_PATH:-${SDCARD_PATH:-/mnt/SDCARD}/.system/h700}"

CURL=""
for c in curl "$SYS/bin/curl"; do
  if command -v "$c" >/dev/null 2>&1; then CURL="$c"; break; fi
done
if [ -z "$CURL" ]; then echo "error: no curl"; exit 1; fi
CA=""
for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt \
         "$SYS/etc/ssl/certs/ca-certificates.crt" "$SYS/certs/ca-certificates.crt"; do
  if [ -f "$f" ]; then CA="$f"; break; fi
done
fetch() {
  if [ -n "$CA" ]; then "$CURL" -fsSL --connect-timeout 10 --max-time 180 --cacert "$CA" "$@"
  else "$CURL" -fsSL --connect-timeout 10 --max-time 180 "$@"; fi
}

case "$1" in
check)
  DATA="$2"
  # GitHub returns indented JSON: each release's own keys sit 4 spaces in, and tag_name comes before prerelease
  fetch -H "Accept: application/vnd.github+json" "https://api.github.com/repos/$REPO/releases?per_page=30" \
    > "$DATA/releases.json" || { echo "error: fetch"; exit 1; }
  tag=$(awk -F'"' '/^    "tag_name":/ {tag=$4} /^    "prerelease": false/ && tag ~ /^nextui-v/ {print tag; exit}' "$DATA/releases.json")
  rm -f "$DATA/releases.json"
  if [ -z "$tag" ]; then echo "error: no release"; exit 1; fi
  echo "latest $tag"
  ;;
download)
  TAG="$2"; DATA="$3"; DIR="$DATA/update"
  case "$TAG" in nextui-v*) ;; *) echo "error: bad tag"; exit 1 ;; esac
  rm -rf "$DIR" && mkdir -p "$DIR" || { echo "error: mkdir"; exit 1; }
  fetch -o "$DIR/$ASSET" "https://github.com/$REPO/releases/download/$TAG/$ASSET" || { echo "error: download"; exit 1; }
  if command -v unzip >/dev/null 2>&1; then UNZIP="unzip -o -q"; else UNZIP="busybox unzip -o -q"; fi
  $UNZIP "$DIR/$ASSET" -d "$DIR/x" || { echo "error: unzip"; exit 1; }
  SRC="$DIR/x/Tools/h700/Sleep Shuffle.pak"
  [ -f "$SRC/sleep-shuffle" ] && [ -f "$SRC/launch.sh" ] || { echo "error: bad zip"; exit 1; }
  mv "$SRC" "$DIR/pak" && rm -rf "$DIR/x" "$DIR/$ASSET" && sync
  touch "$DIR/ready"
  echo "ready"
  ;;
*) echo "error: usage"; exit 1 ;;
esac
