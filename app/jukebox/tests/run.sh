#!/bin/sh
# Host selftest: tone fixtures with art, tags, a lyrics file and an untagged song; then the app renders to PNGs.
# Output (PNGs) goes to tests/out/. Needs ffmpeg and the host's SDL dummy drivers.
set -e
cd "$(dirname "$0")"
OUT=$PWD/out; M=$OUT/music; rm -rf "$OUT"; mkdir -p "$M"
cover() { ffmpeg -v error -y -f lavfi -i "$2" -frames:v 1 "$OUT/$1.jpg"; }
cover red "gradients=s=600x600:c0=0xd7263d:c1=0xf9c74f:x0=0:y0=0:x1=600:y1=600:d=1,format=yuvj420p"
cover blue "gradients=s=500x500:c0=0x0b3d91:c1=0x4cc9f0:x0=0:y0=500:x1=500:y1=0:d=1,format=yuvj420p"
cover grey "gradients=s=400x400:c0=0x222222:c1=0x999999:x0=0:y0=0:x1=400:y1=400:d=1,format=yuvj420p"
tone() { # file freq secs title artist album no [cover]
	mkdir -p "$(dirname "$1")"
	if [ -n "$8" ]; then
		ffmpeg -v error -y -f lavfi -i "sine=f=$2:d=$3,aeval=val(0)*0.5+0.25*sin(2*PI*$2*2*t)|val(0)*0.5:c=stereo" -i "$OUT/$8.jpg" \
			-map 0 -map 1 -c:a libmp3lame -c:v copy -id3v2_version 3 -disposition:v attached_pic \
			-metadata title="$4" -metadata artist="$5" -metadata album="$6" -metadata track="$7" -metadata date=2019 "$1"
	else
		ffmpeg -v error -y -f lavfi -i "sine=f=$2:d=$3" -metadata title="$4" -metadata artist="$5" -metadata album="$6" -metadata track="$7" "$1"
	fi
}
tone "$M/Sunset Drive/01.mp3" 220 12 "Neon Boulevard" "The Midnight Kites" "Sunset Drive" 1 red
tone "$M/Sunset Drive/02.mp3" 262 12 "Rear View" "The Midnight Kites" "Sunset Drive" 2 red
tone "$M/Sunset Drive/03.mp3" 330 12 "Last Exit before the Ocean, Part Two" "The Midnight Kites" "Sunset Drive" 3 red
tone "$M/Deep End/01.mp3" 110 12 "Pressure" "Marlowe" "Deep End" 1 blue
tone "$M/Deep End/02.mp3" 147 12 "Sonnenschein ☀ und Mondlicht" "Marlowe" "Deep End" 2 blue
tone "$M/Mono/a.mp3" 440 12 "Greyscale" "Static Choir" "Mono" 1 grey
ffmpeg -v error -y -f lavfi -i "sine=f=523:d=10" -c:a flac "$M/loose/untagged song.flac" 2>/dev/null || { mkdir -p "$M/loose"; ffmpeg -v error -y -f lavfi -i "sine=f=523:d=10" "$M/loose/untagged song.flac"; }
cat > "$M/Sunset Drive/01.lrc" <<'LRC'
[ti:Neon Boulevard]
[00:00.50]Headlights pull the horizon in
[00:03.00]The radio is almost a prayer
[00:05.50]Every sign says keep on going
[00:08.00]So we do
LRC
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export JUKEBOX_MUSIC=$M BANANA_APP_DATA=$OUT/data JUKEBOX_SELFTEST=$OUT JUKEBOX_SIZE=${JUKEBOX_SIZE:-640x557}
[ -n "$JUKEBOX_FONTS" ] || JUKEBOX_FONTS=$(cd ../../../ui/assets && echo "$PWD/rnds/fonts:$PWD/banana/fonts")
export JUKEBOX_FONTS
../jukebox
