#!/bin/sh
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: legacy-import-test.sh <output-dir>}
mkdir -p "$OUT"
CASE=$(mktemp -d "$OUT/legacy-import.XXXXXX")
BUILD=$CASE/build
FX=$CASE/fixture
mkdir -p "$BUILD" "$CASE/cores" "$CASE/legacy" "$FX/roms/gba" "$FX/data/retro"
make -s -C "$HERE/../system/banana-retro" OUT="$BUILD" \
	"$BUILD/banana-retro" "$BUILD/fixture.so"
cp "$BUILD/fixture.so" "$CASE/cores/mgba_libretro.so"
printf 'BRC1\n' > "$FX/roms/gba/Orchard Tactics.gba"
printf 'core.gba = mgba\n' > "$FX/data/settings.conf"
printf 'runtime.gba.mgba = banana-retro\n' > "$FX/data/retro/runtime.conf"
printf 'savefile_directory = "%s"\n' "$CASE/legacy" > "$CASE/retroarch.cfg"
printf '\001\002\003\004' > "$CASE/legacy/Orchard Tactics.srm"
printf '\005\006' > "$CASE/legacy/Orchard Tactics.rtc"
cp "$CASE/legacy/Orchard Tactics.srm" "$CASE/expected.srm"
cp "$CASE/legacy/Orchard Tactics.rtc" "$CASE/expected.rtc"
printf '\001\002\003\004\005\006' > "$CASE/expected.memory"

BANANA_LIBRETRO="$CASE/cores" \
BANANA_RETRO_TOOL="$BUILD/banana-retro" \
BANANA_RETROARCH_CFG="$CASE/retroarch.cfg" \
	"$HERE/banana-ui" --headless "$CASE/ui-" --size 640x557 \
	--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
	--roms "$FX/roms" --data "$FX/data" \
	--script 'lib 1 0; press x; wait 300; press x; wait 300; press down; press down; press down; press down; press a; wait 100; shot import-preview; press a; wait 100; shot import-result' \
	2> "$CASE/ui.log"

grep -q 'press x: game options open (gba/Orchard Tactics.gba)' "$CASE/ui.log"
cmp "$CASE/expected.srm" "$CASE/legacy/Orchard Tactics.srm"
cmp "$CASE/expected.rtc" "$CASE/legacy/Orchard Tactics.rtc"
MEMORY=$(find "$FX/data/retro/saves" -type f -name memory -print)
[ -n "$MEMORY" ]
[ "$(printf '%s\n' "$MEMORY" | wc -l)" -eq 1 ]
cmp "$CASE/expected.memory" "$MEMORY"

# The cold-boot menu reads bananad's existing path marker, counts a launch, and
# clears stale/missing markers through the same app helpers used by main.cpp.
RESUME_ROM=$FX/roms/gba/Orchard\ Tactics.gba
printf '%s\n' "$RESUME_ROM" > "$FX/data/resume"
"$HERE/banana-ui" --headless "$CASE/resume-" --size 640x557 \
	--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
	--roms "$FX/roms" --data "$FX/data" --script 'resume take' 2> "$CASE/resume.log"
grep -Fq "resume: ready $RESUME_ROM" "$CASE/resume.log"
[ ! -e "$FX/data/resume" ]
[ "$(cat "$FX/data/resume-tries")" -eq 1 ]
printf '%s\n' "$FX/roms/gba/missing.gba" > "$FX/data/resume"
"$HERE/banana-ui" --headless "$CASE/stale-resume-" --size 640x557 \
	--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
	--roms "$FX/roms" --data "$FX/data" --script 'resume' 2> "$CASE/stale-resume.log"
grep -q 'resume: missing' "$CASE/stale-resume.log"
[ ! -e "$FX/data/resume" ]
[ ! -e "$FX/data/resume-tries" ]
echo 'banana-retro UI legacy import and resume checks passed'
