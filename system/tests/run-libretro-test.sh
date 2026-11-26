#!/bin/sh
# Host regression for native dispatch, installed-core resolution and typed boot resume.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LAUNCH="$ROOT/system/rootfs/usr/libexec/banana"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -I"$ROOT/ui/src" \
    "$ROOT/system/tests/check-run-command.cpp" -o "$W/check-run-command" &&
    "$W/check-run-command" && ok "custom systems command substitutes quoted ROM and system exactly" ||
    bad "custom systems command substitution regression"
mkdir -p "$W/libexec" "$W/bin" "$W/libretro" "$W/run"
for c in gpsp mgba gambatte fceumm pcsx_rearmed; do : > "$W/libretro/${c}_libretro.so"; done
ROM="$W/game with spaces.gba"
: > "$ROM"
sed "s#/usr/libexec/banana#$W/libexec#g" "$LAUNCH/libretro-core.sh" > "$W/libexec/libretro-core.sh"

export BANANA_LIBRETRO_DIR="$W/libretro" BANANA_SETTINGS_FILE="$W/settings.conf"
export BANANA_IMAGE_SETTINGS_FILE="$W/image-settings.conf" BANANA_RETRO_RULES_FILE="$W/rules.conf"
export BANANA_RETROARCH_CFG="$W/retroarch.cfg"
touch "$BANANA_SETTINGS_FILE" "$BANANA_IMAGE_SETTINGS_FILE" "$BANANA_RETRO_RULES_FILE"
(
    . "$W/libexec/libretro-core.sh"
    banana_resolve_core gpsp gba "" && [ "$banana_core_name" = gpsp ] || exit 1
    printf 'core.gba = mgba\n' > "$BANANA_SETTINGS_FILE"
    banana_resolve_core gpsp gba "" && [ "$banana_core_name" = mgba ] || exit 2
    banana_resolve_core gpsp gba gambatte && [ "$banana_core_name" = gambatte ] || exit 3
    banana_resolve_core swanstation psx "" && [ "$banana_core_name" = pcsx_rearmed ] || exit 4
    banana_native_eligible gba mgba "$ROM" || exit 5
    banana_native_eligible gba gpsp "$ROM" && exit 6
    ! banana_native_eligible gba mgba "$W/game.zip" || exit 7
) && ok "shared core resolution precedence and explicit eligibility" || bad "shared core resolution or eligibility"

sed -e "s#/usr/libexec/banana#$W/libexec#g" \
    -e "s#/usr/lib/libretro#$W/libretro#g" \
    -e "s#/usr/bin/banana-retro#$W/bin/banana-retro#g" \
    -e "s#/run/banana#$W/run#g" "$LAUNCH/run-libretro" > "$W/libexec/run-libretro"
chmod +x "$W/libexec/run-libretro"
cat > "$W/libexec/ra-secrets" <<'EOF'
#!/bin/sh
if [ "${2-}" = ra_user ] && [ -n "${RA_USER-}" ]; then printf '%s\n' "$RA_USER"; fi
exit 0
EOF
cat > "$W/libexec/run-retroarch" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$W/fallback.log"
exit 0
EOF
chmod +x "$W/libexec/ra-secrets" "$W/libexec/run-retroarch"
cat > "$W/bin/banana-retro" <<'EOF'
#!/bin/sh
if [ -n "${NATIVE_ARGS_LOG-}" ]; then printf '%s\n' "$@" > "$NATIVE_ARGS_LOG"; fi
case ${NATIVE_MODE:-fail} in
    environment)
        [ "$WAYLAND_DISPLAY" = banana-0 ] &&
            [ "$XDG_RUNTIME_DIR" = "$BANANA_RUN_DIR/wayland" ] || exit 1
        : > "$BANANA_RETRO_READY_FILE"
        exit 49 ;;
    boot-ready)
        grep -Fq " starting $2" "$BANANA_RUN_DIR/native-session" || exit 51
        : > "$BANANA_RETRO_READY_FILE"
        exit 52 ;;
    committed) : > "$BANANA_RETRO_COMMITTED_FILE"; exit 43 ;;
    ready-exit)
        : > "$BANANA_RETRO_READY_FILE"
        exit 44 ;;
    ready-wait)
        : > "$BANANA_RETRO_READY_FILE"
        while [ ! -e "$NATIVE_RELEASE" ]; do sleep 0.01; done
        exit 45
        ;;
    never-ready)
        echo $$ > "$NATIVE_PID_MARKER"
        trap 'exit 46' TERM
        while :; do sleep 1; done
        ;;
    ignore-term)
        echo $$ > "$NATIVE_PID_MARKER"
        trap '' TERM
        exec sleep 30
        ;;
    late-commit)
        echo $$ > "$NATIVE_PID_MARKER"
        trap ': > "$BANANA_RETRO_COMMITTED_FILE"; exit 47' TERM
        while :; do sleep 1; done
        ;;
    late-ready)
        echo $$ > "$NATIVE_PID_MARKER"
        trap ': > "$BANANA_RETRO_READY_FILE"; exit 48' TERM
        while :; do sleep 1; done
        ;;
    reject)
        printf 'resume-rejected\n' > "$BANANA_RETRO_STARTUP_ERROR_FILE"
        exit 1 ;;
    *) exit 42 ;;
esac
EOF
chmod +x "$W/bin/banana-retro"

launch() {
    BANANA_SYSTEM=gba BANANA_CORE=mgba BANANA_LIBRETRO_DIR="$W/libretro" \
    BANANA_SETTINGS_FILE="$W/settings.conf" BANANA_IMAGE_SETTINGS_FILE="$W/image-settings.conf" \
    BANANA_RETRO_RULES_FILE="$W/rules.conf" BANANA_RETROARCH_CFG="$W/retroarch.cfg" \
    BANANA_RUN_DIR="$W/run" BANANA_RETRO_SESSION_ID=0123456789abcdef0123456789abcdef \
    BANANA_RETRO_CONTROL_UI_PID=$$ BANANA_SHADER=${BANANA_SHADER-} PATH="$W/bin:$PATH" NATIVE_MODE=${NATIVE_MODE:-fail} \
    "$W/libexec/run-libretro" --resolved mgba "$W/libretro/mgba_libretro.so" "$ROM"
}
rm -f "$W/fallback.log"
launch >"$W/out" 2>&1
grep -q -- '--resolved-core' "$W/fallback.log" && ok "runtime rule disabled routes through RetroArch" || bad "rule rollback did not route through RetroArch"

printf 'runtime.gba.mgba = banana-retro\n' > "$BANANA_RETRO_RULES_FILE"
printf 'runtime.gba.gpsp = banana-retro\n' >> "$BANANA_RETRO_RULES_FILE"
# Enter through the installed systems.conf launcher with a clean display environment.
sed -e "s#/usr/libexec/banana#$W/libexec#g" "$LAUNCH/run-retroarch" > "$W/entry"
rm -f "$W/fallback.log"
( unset WAYLAND_DISPLAY XDG_RUNTIME_DIR
  BANANA_RUN_DIR="$W/run" BANANA_SYSTEM=gba BANANA_CORE=mgba NATIVE_MODE=environment \
    sh "$W/entry" gpsp "$ROM" ) >"$W/out" 2>&1
status=$?
[ "$status" -eq 49 ] && [ ! -s "$W/fallback.log" ] &&
    ok "native child inherits the production compositor endpoint" ||
    bad "native compositor environment missing"

rm -f "$W/fallback.log"
launch >"$W/out" 2>&1
if grep -q -- '--resolved-core' "$W/fallback.log" && grep -q 'startup error=process-exit' "$W/out"; then
    ok "normal native startup falls back safely before readiness"
else bad "normal pre-readiness fallback missing"; fi

TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
boot_launch() {
    BANANA_SYSTEM=${BOOT_TEST_SYSTEM:-gba} BANANA_CORE=${BOOT_TEST_CORE-} BANANA_RUN_DIR="$W/run" \
    NATIVE_ARGS_LOG="$W/native.argv" NATIVE_MODE=${NATIVE_MODE:-boot-ready} \
        sh "$W/entry" --boot-resume "$@"
}
boot_rejected() {
    message=$1
    shift
    rm -f "$W/native.argv" "$W/fallback.log"
    boot_launch "$@" >"$W/out" 2>&1
    status=$?
    [ "$status" -eq "${WANT_STATUS:-78}" ] && [ ! -e "$W/native.argv" ] && [ ! -e "$W/fallback.log" ] &&
        grep -Fq "$message" "$W/out"
}
rm -f "$W/fallback.log"
NATIVE_MODE=boot-ready boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
printf '%s\n' "$W/libretro/mgba_libretro.so" "$ROM" --wayland --resume-checkpoint "$TOKEN" > "$W/expected.argv"
if [ "$status" -eq 52 ] && [ ! -e "$W/fallback.log" ] && cmp -s "$W/native.argv" "$W/expected.argv"; then
    ok "typed boot publishes starting metadata first and passes the exact resolved core, ROM and token"
else bad "typed launch did not preserve its boot context (status $status)"; cat "$W/out"; fi

# Before readiness a typed boot exits 78 when retrying cannot help (ui/src/app.h resumeExitRejected) and 75 when a
# later boot may succeed (resumeExitRetry); it never falls back to RetroArch either way.
NATIVE_MODE=fail boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
if [ "$status" -eq 75 ] && [ ! -e "$W/fallback.log" ] && grep -q 'refusing runtime switch' "$W/out"; then
    ok "typed checkpoint startup failure is retryable and does not switch runtimes"
else bad "typed checkpoint startup failure was not classified retryable (status $status)"; cat "$W/out"; fi

NATIVE_MODE=reject boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
if [ "$status" -eq 78 ] && [ ! -e "$W/fallback.log" ] && grep -q 'error=resume-rejected' "$W/out"; then
    ok "a checkpoint the runtime rejects for good exits as a permanent rejection"
else bad "runtime-rejected checkpoint was not a permanent rejection (status $status)"; cat "$W/out"; fi

NATIVE_MODE=committed boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
if [ "$status" -eq 75 ] && [ ! -e "$W/fallback.log" ]; then
    ok "a commit attempt before readiness keeps the typed boot retryable without switching runtimes"
else bad "typed boot commit-before-readiness status was $status"; cat "$W/out"; fi

rm -f "$W/native.pid" "$W/run"/*
NATIVE_PID_MARKER="$W/native.pid" NATIVE_MODE=never-ready boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
child=$(cat "$W/native.pid" 2>/dev/null || :)
if [ "$status" -eq 75 ] && [ ! -e "$W/fallback.log" ] && [ -n "$child" ] && [ "$child" -gt 1 ] 2>/dev/null &&
   ! kill -0 "$child" 2>/dev/null; then
    ok "a typed boot that never becomes ready spawned a child, reaped it, and stays retryable"
else bad "typed boot that never became ready exited $status (child ${child:-never spawned})"; cat "$W/out"; fi

# The UI confirms a boot resume from this metadata: the session must reach "ready" for a typed boot too.
rm -f "$W/run"/* "$W/release"
NATIVE_RELEASE="$W/release" NATIVE_MODE=ready-wait boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1 &
supervisor=$!
tries=0
while [ "$tries" -lt 100 ]; do
    if [ -f "$W/run/native-session" ] && grep -q ' ready ' "$W/run/native-session"; then break; fi
    kill -0 "$supervisor" 2>/dev/null || break
    sleep 0.05
    tries=$((tries + 1))
done
if grep -q '^v1 [0-9a-f]\{32\} [0-9]* [0-9]* mgba [0-9]* ready ' "$W/run/native-session" 2>/dev/null; then
    ok "a typed boot publishes the ready session metadata the menu confirms the resume from"
else bad "typed boot never published ready metadata"; cat "$W/out"; fi
: > "$W/release"
wait "$supervisor"
status=$?
[ "$status" -eq 45 ] && ok "after readiness a typed boot exits with the runtime's own status" ||
    bad "post-readiness typed boot status was $status"

rm -f "$W/run"/*
typed_session=0123456789abcdef0123456789abcdef
BANANA_RETRO_SESSION_ID="$typed_session" BANANA_RETRO_TRACKED_GAME=1 NATIVE_MODE=boot-ready \
    boot_launch gba mgba "$TOKEN" gpsp "$ROM" >"$W/out" 2>&1
status=$?
if [ "$status" -eq 52 ] && [ -f "$W/run/native-ready-$typed_session" ] &&
   [ ! -e "$W/run/native-session" ] && [ ! -e "$W/fallback.log" ]; then
    ok "rapid typed exit leaves exact-session readiness for the tracking UI"
else bad "rapid typed exit lost readiness (status $status)"; cat "$W/out"; fi

BOOT_TEST_CORE=gpsp boot_rejected 'current installed core does not match' gba mgba "$TOKEN" gpsp "$ROM" &&
    ok "current core change refuses native boot without starting a runtime" ||
    bad "typed boot ignored changed installed core"
BOOT_TEST_SYSTEM=gbc boot_rejected 'current system does not match' gb gambatte "$TOKEN" gambatte "$ROM" &&
    ok "system change refuses native boot even with the same core" ||
    bad "typed boot ignored changed system"
BANANA_SHADER=custom boot_rejected 'profile is no longer eligible or enabled' gba mgba "$TOKEN" gpsp "$ROM" &&
    ok "changed feature eligibility refuses native boot without fallback" ||
    bad "typed boot ignored changed shader"
cp "$BANANA_RETRO_RULES_FILE" "$W/enabled-rules"
: > "$BANANA_RETRO_RULES_FILE"
boot_rejected 'profile is no longer eligible or enabled' gba mgba "$TOKEN" gpsp "$ROM" &&
    ok "removed native opt-in refuses typed boot without fallback" ||
    bad "typed boot ignored opt-out"
mv "$W/enabled-rules" "$BANANA_RETRO_RULES_FILE"

boot_rejected 'invalid native boot resume context' gba mgba "${TOKEN}X" gpsp "$ROM" &&
boot_rejected 'invalid native boot resume context' gba mgba "$TOKEN" gpsp "$ROM" extra &&
boot_rejected 'invalid native boot resume context' gba mgba "$TOKEN" gpsp relative.gba &&
    ok "malformed token, extra argument, and relative ROM are rejected" ||
    bad "malformed typed boot ran a runtime"

mkdir "$W/run/native-session"
WANT_STATUS=75 boot_rejected 'cannot reset native session files' gba mgba "$TOKEN" gpsp "$ROM" &&
    ok "runtime cannot start without publishing native-session" ||
    bad "typed boot started despite unusable session metadata"
rmdir "$W/run/native-session"

BANANA_SYSTEM=gba BANANA_CORE=mgba BANANA_RUN_DIR="$W/run" NATIVE_ARGS_LOG="$W/native.argv" NATIVE_MODE=ready-exit \
    sh "$W/entry" gpsp "$ROM" >"$W/out" 2>&1
status=$?
printf '%s\n' "$W/libretro/mgba_libretro.so" "$ROM" --wayland > "$W/expected.argv"
[ "$status" -eq 44 ] && [ ! -e "$W/fallback.log" ] && cmp -s "$W/native.argv" "$W/expected.argv" &&
    ok "ordinary launch after typed boot does not inherit its token" ||
    bad "typed boot changed normal runtime arguments"

tracked_launch() {
    BANANA_SYSTEM=gba BANANA_CORE=mgba BANANA_ROM="${GAME_PATH:-$ROM}" BANANA_RUN_DIR="$W/run" \
    BANANA_RETRO_TRACKED_GAME=1 NATIVE_MODE=$1 NATIVE_ARGS_LOG="$W/native.argv" setsid -w sh -c '
        printf "%s\n" "$BANANA_ROM" > "$BANANA_RUN_DIR/game.path"
        printf "%s\n" "$$" > "$BANANA_RUN_DIR/game.pid"
        exec "$@"
    ' sh "$W/libexec/run-libretro" --resolved mgba "$W/libretro/mgba_libretro.so" "${LAUNCH_ROM:-$ROM}"
}
printf 'normal gba\n' > "$W/run/game.native"
tracked_launch ready-exit >"$W/out" 2>&1
status=$?
if [ "$status" -eq 44 ] && grep -qx 'native gba' "$W/run/game.native" &&
    [ ! -e "$W/run/native-session" ] && [ ! -e "$W/fallback.log" ]; then
    ok "native readiness promotes tracked intent through later wrapper exit"
else bad "exited native game left stale normal intent (status $status)"; cat "$W/out"; fi

# The UI's published path and the launcher's argument may spell one file differently.
ln -s "$ROM" "$W/alias.gba"
: > "$W/other.gba"
for spelling in "$W/alias.gba" "$W/./game with spaces.gba" "$W//game with spaces.gba"; do
    printf 'normal gba\n' > "$W/run/game.native"
    rm -f "$W/native.argv"
    GAME_PATH=$spelling tracked_launch ready-exit >"$W/out" 2>&1
    status=$?
    printf '%s\n' "$W/libretro/mgba_libretro.so" "$spelling" --wayland > "$W/expected.argv"
    if [ "$status" -eq 44 ] && grep -qx 'native gba' "$W/run/game.native" &&
        [ ! -e "$W/run/native-session" ] && [ ! -e "$W/fallback.log" ] && cmp -s "$W/native.argv" "$W/expected.argv"; then
        ok "equivalent ROM spelling '${spelling#"$W"/}' promotes the intent and the runtime gets the registered spelling"
    else bad "equivalent ROM spelling '${spelling#"$W"/}' was not promoted or not published as registered (status $status)"; cat "$W/out"; fi
done
# Two distinct targets whose names differ only by a trailing newline must never compare equal (command substitution
# would strip it).
nl='
'
mkdir "$W/nl"
: > "$W/nl/A"
: > "$W/nl/A$nl"
ln -s "$W/nl/A" "$W/nl/first.gba"
ln -s "$W/nl/A$nl" "$W/nl/second.gba"
printf 'normal gba\n' > "$W/run/game.native"
GAME_PATH="$W/nl/first.gba" LAUNCH_ROM="$W/nl/second.gba" tracked_launch ready-exit >"$W/out" 2>&1
status=$?
if [ "$status" -eq 1 ] && grep -qx 'normal gba' "$W/run/game.native" && [ ! -e "$W/fallback.log" ]; then
    ok "ROM targets differing by a trailing newline are never promoted as the same file"
else bad "newline-variant ROM was treated as the registered file (status $status)"; cat "$W/out"; fi
printf 'normal gba\n' > "$W/run/game.native"
GAME_PATH="$W/other.gba" tracked_launch ready-exit >"$W/out" 2>&1
status=$?
if [ "$status" -eq 1 ] && grep -qx 'normal gba' "$W/run/game.native" && [ ! -e "$W/fallback.log" ]; then
    ok "a different ROM file is never promoted"
else bad "a different ROM file was promoted or fell back (status $status)"; cat "$W/out"; fi
rm -f "$W/run/game.pid" "$W/run/game.native" "$W/run/game.path" "$W/run/native-session"

printf 'normal gba\n' > "$W/run/game.native"
tracked_launch fail >"$W/out" 2>&1
status=$?
if [ "$status" -eq 0 ] && grep -qx 'normal gba' "$W/run/game.native" &&
    [ ! -e "$W/run/native-session" ] && [ -e "$W/fallback.log" ]; then
    ok "true pre-readiness fallback retains normal intent"
else bad "fallback misclassified a RetroArch session as native (status $status)"; cat "$W/out"; fi

rm -f "$W/fallback.log"
printf 'normal gbc\n' > "$W/run/game.native"
tracked_launch ready-exit >"$W/out" 2>&1
status=$?
if [ "$status" -eq 1 ] && grep -qx 'normal gbc' "$W/run/game.native" &&
    grep -q ' starting ' "$W/run/native-session" && [ ! -e "$W/fallback.log" ]; then
    ok "failed promotion leaves native metadata for the UI to reap without switching runtimes"
else bad "failed promotion overwrote another intent or lost native identity (status $status)"; cat "$W/out"; fi
rm -f "$W/run/game.pid" "$W/run/game.native" "$W/run/game.path" "$W/run/native-session"

rm -f "$W/fallback.log"
BANANA_SHADER=custom NATIVE_MODE=ready-exit launch >"$W/out" 2>&1
grep -q -- '--resolved-core' "$W/fallback.log" && ok "a selected RetroArch shader keeps the RetroArch path" || bad "shader compatibility route changed"
rm -f "$W/fallback.log"
RA_USER=player NATIVE_MODE=ready-exit launch >"$W/out" 2>&1
grep -q -- '--resolved-core' "$W/fallback.log" && ok "a configured RetroAchievements account keeps the RetroArch path" || bad "achievement compatibility route changed"

rm -f "$W/fallback.log" "$W/run"/*
NATIVE_MODE=committed launch >"$W/out" 2>&1
if [ ! -s "$W/fallback.log" ]; then ok "commit attempt before readiness forbids fallback"; else bad "fallback occurred after native commit attempt"; fi

rm -f "$W/fallback.log" "$W/run"/*
NATIVE_MODE=ready-exit launch >"$W/out" 2>&1
status=$?
if [ "$status" -eq 44 ] && [ ! -s "$W/fallback.log" ]; then ok "rapid exit after readiness does not switch runtimes"; else bad "post-readiness exit switched or lost status ($status)"; cat "$W/out"; fi

for mode in never-ready ignore-term; do
    rm -f "$W/fallback.log" "$W/native.pid" "$W/run"/*
    NATIVE_PID_MARKER="$W/native.pid" NATIVE_MODE=$mode launch >"$W/out" 2>&1
    status=$?
    child=$(cat "$W/native.pid" 2>/dev/null || :)
    if [ "$status" -eq 0 ] && [ -s "$W/fallback.log" ] &&
       [ -n "$child" ] && [ "$child" -gt 1 ] 2>/dev/null && ! kill -0 "$child" 2>/dev/null &&
       ! grep -q 'refusing fallback' "$W/out"; then
        ok "$mode startup child is terminated and reaped before fallback"
    else
        bad "$mode startup fallback was not safely bounded (status $status, child ${child:-unknown})"
        cat "$W/out"
    fi
done

# Terminating the launcher during startup must never exec the fallback.
rm -f "$W/native.pid" "$W/fallback.log"
BANANA_SYSTEM=gba BANANA_RUN_DIR="$W/run" NATIVE_PID_MARKER="$W/native.pid" \
    NATIVE_MODE=ignore-term "$W/libexec/run-libretro" --resolved mgba "$W/libretro/mgba_libretro.so" "$ROM" >"$W/out" 2>&1 &
wrapper=$!
tries=0
while [ ! -s "$W/native.pid" ] && [ "$tries" -lt 100 ]; do sleep 0.02; tries=$((tries+1)); done
kill -TERM "$wrapper" && wait "$wrapper"
status=$?
child=$(cat "$W/native.pid")
if [ "$status" -ne 0 ] && [ ! -s "$W/fallback.log" ] && [ -n "$child" ] && [ "$child" -gt 1 ] 2>/dev/null &&
   ! kill -0 "$child" 2>/dev/null; then
    ok "cancelled startup reaps a TERM-resistant child without fallback"
else bad "cancelled startup left a running game or switched runtimes"; fi

for mode in late-commit late-ready; do
    rm -f "$W/fallback.log" "$W/native.pid" "$W/run"/*
    NATIVE_PID_MARKER="$W/native.pid" NATIVE_MODE=$mode launch >"$W/out" 2>&1
    child=$(cat "$W/native.pid" 2>/dev/null || :)
    if [ ! -s "$W/fallback.log" ] && [ -n "$child" ] && [ "$child" -gt 1 ] 2>/dev/null && ! kill -0 "$child" 2>/dev/null; then
        ok "$mode marker discovered after termination prevents fallback"
    else
        bad "$mode termination marker allowed fallback or left child alive"
        cat "$W/out"
    fi
done

rm -f "$W/run"/* "$W/fallback.log" "$W/release"
NATIVE_RELEASE="$W/release" NATIVE_MODE=ready-wait launch >"$W/out" 2>&1 &
supervisor=$!
tries=0
while [ "$tries" -lt 100 ]; do
    if [ -f "$W/run/native-session" ] && grep -q ' ready ' "$W/run/native-session"; then break; fi
    kill -0 "$supervisor" 2>/dev/null || break
    sleep 0.05
    tries=$((tries + 1))
done
if [ -f "$W/run/native-session" ] && grep -q '^v1 0123456789abcdef0123456789abcdef ' "$W/run/native-session" &&
   grep -q ' ready ' "$W/run/native-session"; then
    ok "ready session publishes token, PID start identity, core and path"
else bad "ready session metadata was not published"; cat "$W/out"; fi
: > "$W/release"
wait "$supervisor"; status=$?
[ "$status" -eq 45 ] && [ ! -s "$W/fallback.log" ] && [ ! -e "$W/run/native-session" ] &&
    ok "mid-session crash is reaped, metadata is cleared and no fallback occurs" ||
    bad "native supervisor reap, crash cleanup or fallback behavior"

echo "run-libretro-test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
