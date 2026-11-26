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
	--script 'lib 1 0; press x; wait 300; press x; wait 300; press down; press down; press down; press down; press a; wait 750; shot import-preview; press a; wait 750; shot import-result' \
	2> "$CASE/ui.log"

grep -q 'press x: game options open (gba/Orchard Tactics.gba)' "$CASE/ui.log"
cmp "$CASE/expected.srm" "$CASE/legacy/Orchard Tactics.srm"
cmp "$CASE/expected.rtc" "$CASE/legacy/Orchard Tactics.rtc"
MEMORY=$(find "$FX/data/retro/saves" -type f -name memory -print)
[ -n "$MEMORY" ]
[ "$(printf '%s\n' "$MEMORY" | wc -l)" -eq 1 ]
cmp "$CASE/expected.memory" "$MEMORY"

# The cold-boot menu reads bananad's marker, counts an attempt, and settles it through the same app helpers
# main.cpp calls. A counted attempt keeps the record: only readiness, a permanent rejection, a cancel or the retry
# limit consumes it, and a transient failure never does.
RESUME_ROM=$FX/roms/gba/Orchard\ Tactics.gba
RESUME_TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
resume_ui() {	# resume_ui <log name> <script>
	"$HERE/banana-ui" --headless "$CASE/$1-" --size 640x557 \
		--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
		--roms "$FX/roms" --data "$FX/data" --script "$2" 2> "$CASE/$1.log"
}
typed_marker() { printf 'banana-retro-v1\ngba:mgba\n%s\n%s\n' "$RESUME_TOKEN" "$RESUME_ROM" > "$FX/data/resume"; }
resume_kept() { [ -e "$FX/data/resume" ] && [ "$(sed -n 2p "$FX/data/resume-tries")" -eq "$1" ]; }
resume_gone() { [ ! -e "$FX/data/resume" ] && [ ! -e "$FX/data/resume-tries" ]; }

# the old path-only marker: kept by the count, consumed once the game is known started
printf '%s\n' "$RESUME_ROM" > "$FX/data/resume"
resume_ui resume 'resume begin'
grep -Fq "resume: ready $RESUME_ROM" "$CASE/resume.log"
grep -Fq 'resume: attempt counted' "$CASE/resume.log"
resume_kept 1
resume_ui resume-ready 'resume ready'
resume_gone

# a typed record: a retryable failure keeps record and count, however the launcher ended
typed_marker
resume_ui retry 'resume begin; resume end exit 75'
grep -Fq 'resume: native profile gba:mgba' "$CASE/retry.log"
resume_kept 1
resume_ui killed 'resume end signal 9; resume end exit 1; resume end exit 127'
resume_kept 1
# readiness consumes it, and spends the counter
resume_ui ready 'resume ready'
resume_gone
# a permanent rejection forgets it at once
typed_marker
resume_ui rejected 'resume begin; resume end exit 78'
resume_gone
# a clean exit without readiness still needs retry
typed_marker
resume_ui ran 'resume begin; resume end exit 0'
resume_kept 1
resume_ui abandoned 'resume cancel'
resume_gone
# cancelling the counted attempt forgets it
typed_marker
resume_ui cancelled 'resume begin; resume cancel'
resume_gone
# bounded retry: three transient failures keep the record, the next boot refuses to start a fourth attempt
typed_marker
resume_ui exhausted 'resume begin; resume end exit 75; resume begin; resume end exit 75; resume begin; resume end exit 75'
resume_kept 3
resume_ui blocked 'resume begin'
grep -Fq 'resume: blocked' "$CASE/blocked.log"
if grep -Fq 'attempt counted' "$CASE/blocked.log"; then echo "FAIL: a blocked record started an attempt" >&2; exit 1; fi
resume_gone
printf '%s\n' "$FX/roms/gba/missing.gba" > "$FX/data/resume"
"$HERE/banana-ui" --headless "$CASE/stale-resume-" --size 640x557 \
	--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
	--roms "$FX/roms" --data "$FX/data" --script 'resume' 2> "$CASE/stale-resume.log"
grep -q 'resume: missing' "$CASE/stale-resume.log"
[ ! -e "$FX/data/resume" ]
[ ! -e "$FX/data/resume-tries" ]
# A held request names a game, not a position (the real Library and the main loop's own resolver): a rescan that
# re-sorts the shelf between the press and the start cannot make it start another game, and a game that is gone
# starts nothing.
NEWROM=$FX/roms/gba/Aardvark.gba
resume_ui press "launch 1 0 add $NEWROM; launch 1 0 remove $NEWROM"
grep -Fq "launch: press 1/0 starts gba:$RESUME_ROM" "$CASE/press.log"
grep -Fq "launch: index 0 would start $NEWROM" "$CASE/press.log"
grep -Fq 'launch: press 1/0 names no game now' "$CASE/press.log"
[ ! -e "$NEWROM" ]
# A source replacement after preview invalidates the first confirmation. The
# UI must preview the changed bytes again before it submits a second import.
CHANGED=$CASE/changed
mkdir -p "$CHANGED/cores" "$CHANGED/legacy" "$CHANGED/roms/gba" "$CHANGED/data/retro"
cp "$BUILD/fixture.so" "$CHANGED/cores/mgba_libretro.so"
printf 'BRC1\n' > "$CHANGED/roms/gba/Changed.gba"
printf 'core.gba = mgba\n' > "$CHANGED/data/settings.conf"
printf 'runtime.gba.mgba = banana-retro\n' > "$CHANGED/data/retro/runtime.conf"
printf 'savefile_directory = "%s"\n' "$CHANGED/legacy" > "$CHANGED/retroarch.cfg"
printf '\001\002\003\004' > "$CHANGED/legacy/Changed.srm"
printf '\005\006' > "$CHANGED/legacy/Changed.rtc"
cat > "$CHANGED/tool" <<'TOOL'
#!/bin/sh
set -u
echo "$1" >> "$CALLS"
if [ "$1" = --legacy-preview ]; then
	"$REAL_TOOL" "$@"
	rc=$?
	if [ "$rc" -eq 0 ] && [ ! -e "$MUTATED" ]; then
		: > "$MUTATED"
		printf '\011\010\007\006' > "$MUTATE_SAVE"
	fi
	exit "$rc"
fi
exec "$REAL_TOOL" "$@"
TOOL
chmod +x "$CHANGED/tool"
export BANANA_LIBRETRO="$CHANGED/cores"
export BANANA_RETRO_TOOL="$CHANGED/tool"
export BANANA_RETROARCH_CFG="$CHANGED/retroarch.cfg"
export REAL_TOOL="$BUILD/banana-retro"
export CALLS="$CHANGED/calls"
export MUTATED="$CHANGED/mutated"
export MUTATE_SAVE="$CHANGED/legacy/Changed.srm"
"$HERE/banana-ui" --headless "$CASE/changed-ui-" --size 640x557 \
	--assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" \
	--roms "$CHANGED/roms" --data "$CHANGED/data" \
	--script 'lib 1 0; press x; wait 300; press x; wait 300; press down; press down; press down; press down; press a; wait 750; press a; wait 750; press a; wait 750; press a; wait 750' \
	2> "$CHANGED/ui.log"
[ "$(grep -c '^--legacy-preview$' "$CALLS")" -eq 2 ]
[ "$(grep -c '^--legacy-import$' "$CALLS")" -eq 2 ]
printf '\011\010\007\006\005\006' > "$CHANGED/expected.memory"
MEMORY=$(find "$CHANGED/data/retro/saves" -type f -name memory -print)
[ -n "$MEMORY" ]
cmp "$CHANGED/expected.memory" "$MEMORY"
echo 'banana-retro UI legacy import and resume checks passed'
