#!/bin/sh
# A small library for headless runs: Neo Geo Pocket games (with box art for some, a zipped ROM for one), a few other
# systems, play stats and a favourite. Art is drawn here (no game art is shipped).
#   tests/fixture.sh <dir>     then: banana-ui --roms <dir>/roms --data <dir>/data --systems ../system/rootfs/etc/banana/systems.conf ...
set -e
D=${1:?usage: fixture.sh <dir>}
rm -rf "$D"; mkdir -p "$D/roms/ngp/media/box" "$D/roms/gba" "$D/roms/snes/media/box" "$D/roms/gb" "$D/data"
box() {   # box <file> <colour> <text>
    convert -size 420x380 "gradient:$2-black" -gravity center -fill white -pointsize 34 -annotate 0 "$3" "$1"
}
for g in "Banana Kart" "Peel Quest" "Split Racers (USA)" "Monkey Puzzle League" "Yellow Planet [!]"; do
    head -c 4096 /dev/zero > "$D/roms/ngp/$g.ngp"
done
# Yellow Planet is kept zipped
rm "$D/roms/ngp/Yellow Planet [!].ngp"
python3 - "$D/roms/ngp/Yellow Planet [!].zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('Yellow Planet (Europe).ngp', b'\0' * 4096)
PY
box "$D/roms/ngp/media/box/Banana Kart.png" '#d8b020' 'Banana Kart'
box "$D/roms/ngp/media/box/Peel Quest.png" '#3d9e9a' 'Peel Quest'
box "$D/roms/ngp/media/box/Split Racers (USA).png" '#c84040' 'Split Racers'
for g in "Orchard Tactics" "Tiny Fruit Advance"; do head -c 1024 /dev/zero > "$D/roms/gba/$g.gba"; done
for g in "Super Smoothie World"; do head -c 1024 /dev/zero > "$D/roms/snes/$g.sfc"; done
box "$D/roms/snes/media/box/Super Smoothie World.png" '#8060c0' 'Smoothie World'
head -c 1024 /dev/zero > "$D/roms/gb/Pocket Peel.gb"
now=$(date +%s)
printf 'ngp/Banana Kart.ngp\t14\t79920\t%s\t12\t40\nngp/Peel Quest.ngp\t3\t5400\t%s\t5\t40\ngba/Orchard Tactics.gba\t1\t900\t%s\n' \
    $((now - 3600)) $((now - 90000)) $((now - 900000)) > "$D/data/playstats.tsv"
echo "ngp/Peel Quest.ngp" > "$D/data/favorites.txt"
# an app for the Apps shelf: a manifest the registry scans (Library::scanApps) and the marker file it lists as the
# app's one entry. Nothing here is executed -- the harness only checks the tile is on the shelf (the `apps` op).
mkdir -p "$D/data/apps/hello"
cat > "$D/data/apps/hello/app.json" <<'JSON'
{ "id": "hello", "name": "Hello App", "maker": "Banana", "run": "/usr/libexec/banana/run-bananatube %ROM%", "ext": "app", "perf": "balanced", "accent": "#12ab34" }
JSON
touch "$D/data/apps/hello/Hello.app"
# a save state for one ngp game (RetroArch keeps them sorted per core), so the states page has something to list
mkdir -p "$D/states/Mednafen"
head -c 4096 /dev/zero > "$D/states/Mednafen/Banana Kart.state"
echo "fixture in $D"
