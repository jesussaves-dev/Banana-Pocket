#!/bin/sh
# Host check of the native pilot-profile gate, using the production helpers in libretro-core.sh.
# Software evidence only: it does not qualify any profile on hardware.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CORE_SH="$ROOT/system/rootfs/usr/libexec/banana/libretro-core.sh"
DEFAULTS="$ROOT/system/banana-retro/config/defaults.conf"

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT HUP INT TERM

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }

[ -f "$CORE_SH" ] || { echo "check-profiles: missing $CORE_SH" >&2; exit 2; }
[ -f "$DEFAULTS" ] || { echo "check-profiles: missing $DEFAULTS" >&2; exit 2; }

# Production helper with its absolute helper path redirected into the sandbox.
mkdir -p "$W/libexec" "$W/content"
sed "s#/usr/libexec/banana#$W/libexec#g" "$CORE_SH" > "$W/libexec/libretro-core.sh"

# Deterministic ra-secrets stub: `get ra_user` prints the contents of $W/ra_user (empty when absent).
cat > "$W/libexec/ra-secrets" <<EOF
#!/bin/sh
[ "\${1-}" = get ] && [ "\${2-}" = ra_user ] || exit 2
[ -f "$W/ra_user" ] && cat "$W/ra_user"
exit 0
EOF
chmod +x "$W/libexec/ra-secrets"

export BANANA_RETROARCH_CFG="$W/retroarch.cfg" BANANA_RETRO_RULES_FILE="$W/rules.conf"
unset BANANA_SHADER
: > "$BANANA_RETROARCH_CFG"
: > "$BANANA_RETRO_RULES_FILE"
rm -f "$W/ra_user"

# Fixtures: one content file per extension, plus names that must never be eligible.
for ext in gba GBA gb GB gbc GBC nes NES zip sfc; do : > "$W/content/game.$ext"; done
mkdir "$W/content/dir.gba"

# Each probe runs in a subshell so environment and file changes never leak between cases.
eligible() { ( . "$W/libexec/libretro-core.sh" && banana_native_eligible "$@" ); }
opted_in() { ( . "$W/libexec/libretro-core.sh" && banana_native_opted_in "$@" ); }

expect_eligible() {
    desc=$1; shift
    if eligible "$@"; then ok "eligible: $desc"; else bad "expected eligible: $desc ($*)"; fi
}
expect_rejected() {
    desc=$1; shift
    if eligible "$@"; then bad "expected rejected: $desc ($*)"; else ok "rejected: $desc"; fi
}

C="$W/content"

echo "pilot pairs"
expect_eligible "gba mgba .gba" gba mgba "$C/game.gba"
expect_eligible "gb gambatte .gb" gb gambatte "$C/game.gb"
expect_eligible "gbc gambatte .gbc" gbc gambatte "$C/game.gbc"
expect_eligible "nes fceumm .nes" nes fceumm "$C/game.nes"
expect_eligible "gba mgba .GBA" gba mgba "$C/game.GBA"
expect_eligible "gb gambatte .GB" gb gambatte "$C/game.GB"
expect_eligible "gbc gambatte .GBC" gbc gambatte "$C/game.GBC"
expect_eligible "nes fceumm .NES" nes fceumm "$C/game.NES"

echo "unsupported combinations"
expect_rejected "gba gpsp" gba gpsp "$C/game.gba"
expect_rejected "gb mgba" gb mgba "$C/game.gb"
expect_rejected "nes mgba" nes mgba "$C/game.nes"
expect_rejected "gbc fceumm" gbc fceumm "$C/game.gbc"
expect_rejected "psx pcsx_rearmed with a pilot extension" psx pcsx_rearmed "$C/game.gba"
expect_rejected "snes snes9x .sfc" snes snes9x "$C/game.sfc"
expect_rejected "empty system" "" mgba "$C/game.gba"
expect_rejected "empty core" gba "" "$C/game.gba"
expect_rejected "gb gambatte with unrelated .sfc content" gb gambatte "$C/game.sfc"

echo "per-system extensions (systems.conf: gba=gba, gb=gb, gbc=gbc+gb, nes=nes)"
expect_eligible "gbc gambatte accepts .gb" gbc gambatte "$C/game.gb"
expect_eligible "gbc gambatte accepts .GB" gbc gambatte "$C/game.GB"
for ext in gb GB gbc GBC nes NES; do
    expect_rejected "gba mgba rejects .$ext" gba mgba "$C/game.$ext"
done
for ext in gba GBA gbc GBC nes NES; do
    expect_rejected "gb gambatte rejects .$ext" gb gambatte "$C/game.$ext"
done
for ext in gba GBA nes NES; do
    expect_rejected "gbc gambatte rejects .$ext" gbc gambatte "$C/game.$ext"
done
for ext in gba GBA gb GB gbc GBC; do
    expect_rejected "nes fceumm rejects .$ext" nes fceumm "$C/game.$ext"
done
expect_rejected "delimiter in system cannot forge a pair" "gba:mgba" "" "$C/game.gba"

echo "archives and missing content"
expect_rejected "gba mgba .zip" gba mgba "$C/game.zip"
expect_rejected "gb gambatte .zip" gb gambatte "$C/game.zip"
expect_rejected "gbc gambatte .zip" gbc gambatte "$C/game.zip"
expect_rejected "nes fceumm .zip" nes fceumm "$C/game.zip"
expect_rejected "gba mgba missing file" gba mgba "$C/absent.gba"
expect_rejected "gb gambatte missing file" gb gambatte "$C/absent.gb"
expect_rejected "gbc gambatte missing file" gbc gambatte "$C/absent.gbc"
expect_rejected "nes fceumm missing file" nes fceumm "$C/absent.nes"
expect_rejected "directory named like a ROM" gba mgba "$C/dir.gba"
expect_rejected "empty content path" gba mgba ""

echo "shader exclusions"
if (BANANA_SHADER=custom; eligible gba mgba "$C/game.gba"); then
    bad "BANANA_SHADER must exclude the native profile"
else ok "rejected: BANANA_SHADER set"; fi
printf 'video_shader_enable = "true"\n' > "$BANANA_RETROARCH_CFG"
expect_rejected "retroarch.cfg shader enabled (true)" gba mgba "$C/game.gba"
printf 'video_shader_enable = "1"\n' > "$BANANA_RETROARCH_CFG"
expect_rejected "retroarch.cfg shader enabled (1)" gba mgba "$C/game.gba"
printf 'video_shader_enable = "false"\n' > "$BANANA_RETROARCH_CFG"
expect_eligible "retroarch.cfg shader disabled" gba mgba "$C/game.gba"
: > "$BANANA_RETROARCH_CFG"

echo "RetroAchievements user"
: > "$W/ra_user"
expect_eligible "empty ra_user" nes fceumm "$C/game.nes"
printf 'someone\n' > "$W/ra_user"
for pair in "gba mgba game.gba" "gb gambatte game.gb" "gbc gambatte game.gbc" "nes fceumm game.nes"; do
    set -- $pair
    expect_rejected "populated ra_user: $1 $2" "$1" "$2" "$C/$3"
done
rm -f "$W/ra_user"
expect_eligible "no ra_user stored" gba mgba "$C/game.gba"

echo "stock opt-in state"
PAIRS="gba:mgba gb:gambatte gbc:gambatte nes:fceumm"
: > "$BANANA_RETRO_RULES_FILE"
for p in $PAIRS; do
    if opted_in "${p%%:*}" "${p#*:}"; then bad "empty rules must not opt in $p"
    else ok "not opted in with empty rules: $p"; fi
done
rm -f "$BANANA_RETRO_RULES_FILE"
for p in $PAIRS; do
    if opted_in "${p%%:*}" "${p#*:}"; then bad "missing rules file must not opt in $p"
    else ok "not opted in with missing rules file: $p"; fi
done
: > "$BANANA_RETRO_RULES_FILE"

echo "explicit opt-in is per pair"
for want in $PAIRS; do
    printf 'runtime.%s.%s = banana-retro\n' "${want%%:*}" "${want#*:}" > "$BANANA_RETRO_RULES_FILE"
    for p in $PAIRS; do
        if opted_in "${p%%:*}" "${p#*:}"; then got=yes; else got=no; fi
        if [ "$p" = "$want" ]; then exp=yes; else exp=no; fi
        if [ "$got" = "$exp" ]; then ok "rule for $want: $p opted_in=$got"
        else bad "rule for $want: $p opted_in=$got, expected $exp"; fi
    done
done
printf 'runtime.gba.mgba = retroarch\n' > "$BANANA_RETRO_RULES_FILE"
if opted_in gba mgba; then bad "a non-banana-retro rule value must not opt in"
else ok "rule value retroarch does not opt in"; fi
printf 'runtime.gba.mgba = banana-retro\nruntime.gba.mgba = retroarch\n' > "$BANANA_RETRO_RULES_FILE"
if opted_in gba mgba; then bad "the last rule must win"
else ok "last conflicting rule wins"; fi
printf 'runtime.gba.mgba=banana-retro\n' > "$BANANA_RETRO_RULES_FILE"
if opted_in gba mgba; then ok "rule without spaces around = opts in"
else bad "rule without spaces around = should opt in"; fi
: > "$BANANA_RETRO_RULES_FILE"

echo "defaults.conf is inert"
active=$(grep -Ev '^[[:space:]]*(#|$)' "$DEFAULTS" || true)
if [ -z "$active" ]; then ok "defaults.conf has no active lines"
else bad "defaults.conf has active lines: $active"; fi
if printf '%s\n' "$active" | grep -Eq '^[[:space:]]*runtime\.'; then
    bad "defaults.conf enables a runtime rule"
else ok "defaults.conf enables no runtime rule"; fi

echo "check-profiles: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
