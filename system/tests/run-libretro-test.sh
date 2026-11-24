#!/bin/sh
# Host regression for Task 08's opt-in dispatcher and shared installed-core resolver.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LAUNCH="$ROOT/system/rootfs/usr/libexec/banana"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
mkdir -p "$W/libexec" "$W/bin" "$W/libretro" "$W/run"
for c in gpsp mgba gambatte fceumm pcsx_rearmed; do : > "$W/libretro/${c}_libretro.so"; done
ROM="$W/game with spaces.gba"
: > "$ROM"
cp "$LAUNCH/libretro-core.sh" "$W/libexec/libretro-core.sh"

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
case ${NATIVE_MODE:-fail} in
    committed) : > "$BANANA_RETRO_COMMITTED_FILE"; exit 43 ;;
    ready-exit)
        : > "$BANANA_RETRO_READY_FILE"
        exit 44 ;;
    ready-wait)
        : > "$BANANA_RETRO_READY_FILE"
        while [ ! -e "$NATIVE_RELEASE" ]; do sleep 0.01; done
        exit 45
        ;;
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
rm -f "$W/fallback.log"
launch >"$W/out" 2>&1
if grep -q -- '--resolved-core' "$W/fallback.log" && grep -q 'startup error=process-exit' "$W/out"; then
    ok "typed failed startup falls back safely before readiness"
else bad "typed pre-readiness fallback missing"; fi

rm -f "$W/fallback.log"
BANANA_SHADER=custom launch >"$W/out" 2>&1
grep -q -- '--resolved-core' "$W/fallback.log" && ok "a selected RetroArch shader keeps the RetroArch path" || bad "shader compatibility route changed"
rm -f "$W/fallback.log"
RA_USER=player launch >"$W/out" 2>&1
grep -q -- '--resolved-core' "$W/fallback.log" && ok "a configured RetroAchievements account keeps the RetroArch path" || bad "achievement compatibility route changed"

rm -f "$W/fallback.log" "$W/run"/*
NATIVE_MODE=committed launch >"$W/out" 2>&1
if [ ! -s "$W/fallback.log" ]; then ok "commit attempt before readiness forbids fallback"; else bad "fallback occurred after native commit attempt"; fi

rm -f "$W/fallback.log" "$W/run"/*
NATIVE_MODE=ready-exit launch >"$W/out" 2>&1
status=$?
if [ "$status" -eq 44 ] && [ ! -s "$W/fallback.log" ]; then ok "rapid exit after readiness does not switch runtimes"; else bad "post-readiness exit switched or lost status ($status)"; cat "$W/out"; fi

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
