#!/bin/sh
# Generate the promo's original music and mux it into the silent render.
# Usage: tools/promo/mux.sh <silent.mp4> <out.mp4>
set -eu
in=$1; out=$2
here=$(dirname "$0")
wav=$(mktemp -t promo-music.XXXXXX.wav)
trap 'rm -f "$wav"' EXIT
python3 -I "$here/gen-promo-music.py" "$wav"
ffmpeg -y -v error -i "$in" -i "$wav" -map 0:v -map 1:a -c:v copy -c:a aac -b:a 192k -shortest "$out"
