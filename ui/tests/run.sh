#!/bin/sh
# banana-ui's headless test: a test library, then the menu's three views, checked for what
# must hold (the right view after each button, a game launched from the ready screen, frames rendered) and
# timed. The PNGs are left in $OUT for a look.
#
# The key screens are also a golden test: each is compared to a committed baseline (ui/tests/golden), so a change
# that moves or drops a theme element fails here rather than only on the handheld. UPDATE_GOLDEN=1 rewrites the
# baselines; the comparison is coarse (32x24 grey) so a different freetype version does not fail it.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
export BANANA_THEMES="$HERE/../system/rootfs/usr/share/banana/themes"
OUT=${OUT:-$HERE/tests/out}
FX=$OUT/fixture
rm -rf "$OUT"; mkdir -p "$OUT"
"$HERE/tests/fixture.sh" "$FX" >/dev/null
fail=0
# the panel rotation's transpose (src/turn.h): the tiled path must match the scalar loop for any rectangle. QEMU's
# panel reports no orientation, so this is what covers the handheld's 90-degree panel.
${CXX:-c++} -O2 -std=c++17 -Wno-unused-function -o "$OUT/turn-test" "$HERE/tests/turn-test.cpp" 2> "$OUT/turn-build.log" ||
    { echo "FAIL: can't build turn-test (see $OUT/turn-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/turn-test" > "$OUT/turn.log" 2>&1 || { echo "FAIL: turn-test"; cat "$OUT/turn.log"; fail=1; }
# the system shader's mask pattern, its four panel rotations, the composed base and the colour tables (src/shader_mask.h)
${CXX:-c++} -O2 -std=c++17 -Wno-unused-function $(pkg-config --cflags libdrm 2>/dev/null) -o "$OUT/shader-mask-test" "$HERE/tests/shader-mask-test.cpp" 2> "$OUT/shader-mask-build.log" ||
    { echo "FAIL: can't build shader-mask-test (see $OUT/shader-mask-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/shader-mask-test" > "$OUT/shader-mask.log" 2>&1 || { echo "FAIL: shader-mask-test"; cat "$OUT/shader-mask.log"; fail=1; }
# a bad setting goes back to its default, and the lists it is checked against stay in bounds (under ASan)
SD=$OUT/settings-defaults; rm -rf "$SD"; mkdir -p "$SD"
${CXX:-c++} -O1 -g -std=c++17 -fsanitize=address -I"$HERE/src" -o "$OUT/settings-defaults-test" "$HERE/tests/settings-defaults-test.cpp" "$HERE/src/settings_defaults.cpp" 2> "$OUT/settings-defaults-build.log" ||
    { echo "FAIL: can't build settings-defaults-test (see $OUT/settings-defaults-build.log)"; fail=1; }
[ "$fail" = 1 ] || ASAN_OPTIONS=detect_leaks=0 "$OUT/settings-defaults-test" "$SD" > "$OUT/settings-defaults.log" 2>&1 || { echo "FAIL: settings-defaults-test"; cat "$OUT/settings-defaults.log"; fail=1; }
# the zero-heap atomic request (src/kms.h): the kernel's object groups and counts must come out as it reads them
${CXX:-c++} -O2 -std=c++17 -I"$HERE/src" $(pkg-config --cflags libdrm 2>/dev/null) -o "$OUT/atomic-buf-test" "$HERE/tests/atomic-buf-test.cpp" 2> "$OUT/atomic-buf-build.log" ||
    { echo "FAIL: can't build atomic-buf-test (see $OUT/atomic-buf-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/atomic-buf-test" > "$OUT/atomic-buf.log" 2>&1 || { echo "FAIL: atomic-buf-test"; cat "$OUT/atomic-buf.log"; fail=1; }
# the Phase 3B buffer rules: the one format/modifier/plane pair the compositor accepts (linear, one plane)
${CXX:-c++} -O2 -std=c++17 $(pkg-config --cflags libdrm 2>/dev/null) -o "$OUT/dmabuf-test" "$HERE/tests/dmabuf-test.cpp" 2> "$OUT/dmabuf-build.log" ||
    { echo "FAIL: can't build dmabuf-test (see $OUT/dmabuf-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/dmabuf-test" > "$OUT/dmabuf.log" 2>&1 || { echo "FAIL: dmabuf-test"; cat "$OUT/dmabuf.log"; fail=1; }
# the Phase 3C rotation map must be turn.h's 90-degree turn, and the quad's UVs must interpolate to it
${CXX:-c++} -O2 -std=c++17 -o "$OUT/rotation-test" "$HERE/tests/rotation-test.cpp" 2> "$OUT/rotation-build.log" ||
    { echo "FAIL: can't build rotation-test (see $OUT/rotation-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/rotation-test" > "$OUT/rotation.log" 2>&1 || { echo "FAIL: rotation-test"; cat "$OUT/rotation.log"; fail=1; }
# the screen frames: which one a game gets, where its hole is, and the art the image ships
${CXX:-c++} -O2 -std=c++17 -Wno-unused-function -o "$OUT/frame-test" "$HERE/tests/frame-test.cpp" "$HERE/src/frame.cpp" 2> "$OUT/frame-build.log" ||
    { echo "FAIL: can't build frame-test (see $OUT/frame-build.log)"; fail=1; }
[ "$fail" = 1 ] || "$OUT/frame-test" "$HERE/../system/rootfs/usr/share/banana/overlays" "$OUT/frame-banana.png" > "$OUT/frame.log" 2>&1 || { echo "FAIL: frame-test"; cat "$OUT/frame.log"; fail=1; }
# BananaTube's player under the compositor: real mpv against the real Wayland server (KMS and EGL faked)
if command -v mpv >/dev/null && [ -f "$HERE/build/wayland/viewporter-server-protocol.h" ]; then
	WLT=$OUT/wl; mkdir -p "$WLT"; chmod 700 "$WLT"
	wayland-scanner client-header "$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-client-protocol.h"
	${CXX:-c++} -O1 -std=c++17 -DBANANA_WAYLAND_DIR="\"$WLT\"" -I"$OUT" -I"$HERE/build/wayland" -I"$HERE/src" \
		$(pkg-config --cflags libdrm wayland-server wayland-client xkbcommon egl glesv2 gbm) \
		-o "$OUT/compositor-player-test" "$HERE/tests/compositor-player-test.cpp" "$HERE/src/compositor.cpp" \
		"$HERE"/build/wayland/*.o $(pkg-config --libs wayland-server wayland-client xkbcommon libdrm) 2> "$OUT/player-build.log" ||
		{ echo "FAIL: can't build compositor-player-test (see $OUT/player-build.log)"; fail=1; }
	[ "$fail" = 1 ] || "$OUT/compositor-player-test" "$WLT" > "$OUT/player.log" 2>&1 || { echo "FAIL: compositor-player-test"; cat "$OUT/player.log"; fail=1; }
else
	echo "skip: compositor-player-test (needs mpv and a built banana-ui)"
fi
# A golden theme test: the key screens compared to committed baselines, so a change that moves or drops an element
# fails here instead of only being seen on the handheld. The comparison is at 32x24 grey -- a layout change is a big
# difference (an added block scores ~0.06), while a freetype version's rasterization stays under 0.02 -- and
# UPDATE_GOLDEN=1 rewrites the baselines. They live in ui/tests/golden (one 32x24 grey PNG per screen per size).
GOLDEN=$HERE/tests/golden
golden() {	# golden <name> <png>
	f=$2
	[ -s "$f" ] || return 0
	g=$GOLDEN/$size-$1.png
	convert "$f" -resize 32x24! -colorspace Gray "$OUT/golden-now.png" 2>/dev/null
	if [ -n "$UPDATE_GOLDEN" ] || [ ! -s "$g" ]; then mkdir -p "$GOLDEN"; cp "$OUT/golden-now.png" "$g"; return 0; fi
	d=$(compare -metric RMSE "$g" "$OUT/golden-now.png" null: 2>&1 | sed 's/.*(\(.*\))/\1/')
	awk -v d="$d" 'BEGIN { if (d + 0 > 0.04) exit 1 }' || { echo "FAIL $size: golden $1 changed (RMSE $d)"; fail=1; }
}
for size in 640x557; do
    log=$OUT/$size.log
    "$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
        --splash "$HERE/assets/splash" --banana "$HERE/assets/banana" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "boot; splash boot-drop 200; splash boot-word 700; splash boot-click 1100 1; home 0; sysinfo 0; apps; wait 2400; shot home; bench 120; press a; wait 2400; shot lib; press down; wait 2400; shot lib2; bench 120; press a; wait 3000; shot ready; press b; press b; battery 50; battery 19; shot battery-low; press left; battery 18; battery 4; wait 2400; shot battery-empty; battery 4 charging; battery 19" \
        2> "$log" || { echo "FAIL $size: banana-ui exited $?"; fail=1; continue; }
    check() { grep -q "$1" "$log" || { echo "FAIL $size: no '$1'"; fail=1; }; }
    check "press a: used, view 1"          # home -> the library
    check "press down: used, view 1 sys 0 game 1"
    check "sysinfo ngp: .*achievements 17/80"     # the shelf's RetroAchievements totals (fixture: 12/40 + 5/40)
    check "apps .*: hello (Hello App), 1 entries"  # the Apps shelf: a scanned manifest (fixture.sh)
    check "press a: used, view 2"          # library -> ready screen
    check "launch 1: .*Monkey Puzzle League.ngp"
    check "splash boot-click at 1100 ms"
    check "battery 19%: notice Battery low: 19%"
    check "press left: notice hidden"
    check "battery 4%: notice Battery nearly empty: 4%"
    check "battery 4%: notice hidden"      # the charger on
    [ "$(grep -c 'notice Battery low: 19%' "$log")" = 2 ] || { echo "FAIL $size: the notice didn't come back after charging"; fail=1; }
    grep -q "battery 18%" "$log" && { echo "FAIL $size: a second notice in the same step"; fail=1; }
    # the in-game pill's card: the menu publishes the colours its own "osd" box draws with, so RetroArch's pill
    # and this screen agree (the dark palette's default card, since no theme is set here)
    style=$FX/data/osd-style
    [ "$(sed -n '1p' "$style" 2>/dev/null)" = "bg 211f26" ] || { echo "FAIL $size: osd-style bg '$(sed -n '1p' "$style" 2>/dev/null)'"; fail=1; }
    [ "$(sed -n '2p' "$style" 2>/dev/null)" = "edge f1ede2" ] || { echo "FAIL $size: osd-style edge '$(sed -n '2p' "$style" 2>/dev/null)'"; fail=1; }
    [ "$(sed -n '3p' "$style" 2>/dev/null)" = "crisp 0" ] || { echo "FAIL $size: osd-style crisp '$(sed -n '3p' "$style" 2>/dev/null)'"; fail=1; }
    # the splash is one screen now (no second panel, no console/cartridge): only -top is written for it
    for s in boot-drop boot-word boot-click; do
        f=$OUT/$size-$s.png
        [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
        [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
    done
    for s in home lib lib2 ready battery-low battery-empty; do
        f=$OUT/$size-$s.png
        [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
        # a frame that is one flat colour means nothing was drawn
        [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
    done
    # golden: the key screens against the committed baselines
    for s in home lib ready battery-low; do
        golden "$s" "$OUT/$size-$s.png"
    done
    # The achievement toasts: what a game unlocked (buildroot/patches/retroarch/0013) is drawn over home, not only
    # over Settings, one band per unlock with the badge RetroArch downloaded in the tile; and the plain /run/banana/toast
    # hook. Two runs, since a toast holds the panel for seconds of real time and would hide the next one.
    tlog=$OUT/$size-toast.log
    "$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; toast 1|Trophy won|By finding the sword|$FX/roms/ngp/media/box/Peel Quest.png; wait 16; shot toast; wait 1" 2> "$tlog" ||
        { echo "FAIL $size: banana-ui (toast, badge) exited $?"; fail=1; }
    grep -q 'toast: queued "Trophy won" (badge yes)' "$tlog" || { echo "FAIL $size: the achievement toast wasn't queued"; fail=1; }
    [ -s "$OUT/$size-toast.png" ] && [ "$(convert "$OUT/$size-toast.png" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: the toast frame is blank"; fail=1; }
    plog=$OUT/$size-toast-plain.log
    "$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; toast 2|Something failed|A plain message; wait 16; shot toast-plain; wait 1" 2> "$plog" ||
        { echo "FAIL $size: banana-ui (toast, plain) exited $?"; fail=1; }
    grep -q 'toast: queued "Something failed" (badge none)' "$plog" || { echo "FAIL $size: the plain toast wasn't queued"; fail=1; }
    [ -s "$OUT/$size-toast-plain.png" ] || { echo "FAIL $size: no plain toast frame"; fail=1; }
    # Wi-Fi: Settings > Network, the networks from a stand-in wifi script, a password typed by touch, joining,
    # forgetting, an open network and a WPA3-only one (Settings opens on its groups: R four times is Network)
    wlog=$OUT/$size-wifi.log
    rm -f "$FX/data/wifi.conf"
    # touch coords are in layout px
    key() { for rc in "$@"; do awk -v rc=$rc 'BEGIN { split(rc, a, ","); printf "touch %.0f %.0f; ", 18 + a[2] * 60.4 + 27, 80 + a[1] * 66 + 29 }'; done; }
    BANANA_WIFI="$HERE/tests/fake-wifi.sh" FAKE_WIFI_CONF="$FX/data/wifi.conf" "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 \
        --assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; press start; press r; press r; press r; press a; press down; shot wifi-row; press down; shot share-row; press up; press a; shot wifi-list; press a; \
            $(key 3,4 2,0 3,5) press start; shot wifi-short; $(key 2,0 3,5 2,0 0,0 0,1) shot wifi-keys; press start; wait 2500; shot wifi-joined; \
            press y; press down; press down; press a; wait 2500; press up; press a; press x; $(key 1,0 1,1 1,2 1,3 1,4 1,5 4,0 1,6 1,7) press start; \
            wait 2500; shot wifi-end" 2> "$wlog" || { echo "FAIL $size: banana-ui (Wi-Fi) exited $?"; fail=1; }
    conf=$FX/data/wifi.conf
    [ "$(sed -n 's/^[[:space:]]*ssid=//p' "$conf" | tr '\n' '|')" = '"Neighbour 5G"|"Cafe Free"|' ] || { echo "FAIL $size: wifi.conf networks: $(grep ssid= "$conf" | tr -d '\t' | tr '\n' ' ')"; fail=1; }
    grep -q 'psk="QWERTYui"' "$conf" && grep -q 'key_mgmt=SAE' "$conf" && grep -q 'ieee80211w=2' "$conf" || { echo "FAIL $size: the WPA3 network's block"; fail=1; }
    grep -q 'key_mgmt=NONE' "$conf" || { echo "FAIL $size: the open network's block"; fail=1; }
    grep -q 'priority=2' "$conf" || { echo "FAIL $size: the last network joined isn't first"; fail=1; }
    for s in wifi-row wifi-list wifi-short wifi-keys wifi-joined wifi-end; do
        [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }
    done
    # The first-time setup: opened as on a first boot, a RetroAchievements account typed by touch (name, then the
    # password), past the Box art step (not started: no network here), Finish writes setup-done; the accounts file is what the launchers read (no login with the server
# here: BANANA_RA_CHECK=0)
    slog=$OUT/$size-setup.log
    rm -f "$FX/data/setup-done" "$FX/data/accounts.conf"
    BANANA_RA_CHECK=0 BANANA_WIFI="$HERE/tests/fake-wifi.sh" FAKE_WIFI_CONF="$FX/data/wifi.conf" "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 \
        --assets "$HERE/assets/rnds" --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; setup; shot setup; press down; press right; shot setup-tz; press down; shot setup-ra; press a; shot setup-name; \
            $(key 1,0 1,1 1,2) press start; shot setup-pass; $(key 2,0 2,1) press start; shot setup-signed; press down; shot setup-boxart; press down; press a; shot setup-done" 2> "$slog" \
        || { echo "FAIL $size: banana-ui (setup) exited $?"; fail=1; }
    grep -q "setup open" "$slog" || { echo "FAIL $size: the setup didn't open"; fail=1; }
    [ -f "$FX/data/setup-done" ] || { echo "FAIL $size: Finish didn't write setup-done"; fail=1; }
    grep -q '^ra_user=qwe$' "$FX/data/accounts.conf" && grep -q '^ra_password=as$' "$FX/data/accounts.conf" || { echo "FAIL $size: accounts.conf: $(cat "$FX/data/accounts.conf" 2>/dev/null | tr '\n' ' ')"; fail=1; }
    for s in setup setup-tz setup-ra setup-name setup-pass setup-signed; do
        [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }
    done
    rm -f "$FX/data/setup-done" "$FX/data/accounts.conf"
    # Favorites starred itself (first on the shelf, ahead of every system): a GBA favourite launches with GBA's
    # emulator, not the shelf's one along (its systems' places moved when Favorites went in before them)
    flog=$OUT/$size-favorites.log
    cp "$FX/data/favorites.txt" "$OUT/favorites.txt.keep"
    echo "gba/Orchard Tactics.gba" > "$FX/data/favorites.txt"; echo favorites > "$FX/data/favorite-systems.txt"
    "$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "lib 0 0; press a; wait 3000; wait 1" 2> "$flog" || { echo "FAIL $size: banana-ui (favorites) exited $?"; fail=1; }
    grep -q "launch 0: .*Orchard Tactics.gba (system gba)" "$flog" || { echo "FAIL $size: the favourite: $(grep launch "$flog")"; fail=1; }
    mv "$OUT/favorites.txt.keep" "$FX/data/favorites.txt"; rm -f "$FX/data/favorite-systems.txt"
    # X on a game in the library: the game's own options (emulator, shader, its save states) over the library.
    # Changing the shader writes banana/pergame.conf (run-retroarch's BANANA_SHADER); the states page lists the
    # game's states, Y keeps a backup and A twice deletes one.
    xlog=$OUT/$size-xgame.log
    rm -f "$FX/data/pergame.conf"
    mkdir -p "$FX/states/Mednafen"; head -c 4096 /dev/zero > "$FX/states/Mednafen/Banana Kart.state"
    rm -f "$FX/states/Mednafen/"*bak-*
    BANANA_STATES="$FX/states" "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "lib 0 0; press x; wait 300; shot xgame; press down; press right; wait 100; shot xgame2; press down; press a; wait 200; shot states; press y; wait 150; press a; press a; wait 150; shot states2; press b; press b; wait 1" 2> "$xlog" ||
        { echo "FAIL $size: banana-ui (x on a game) exited $?"; fail=1; }
    grep -q "press x: game options open" "$xlog" || { echo "FAIL $size: X did not open the game options"; fail=1; }
    grep -q "settings open" "$xlog" && { echo "FAIL $size: X opened the settings list"; fail=1; }
    { grep -q "^ngp/" "$FX/data/pergame.conf" && grep -q "$(printf '\t')off$" "$FX/data/pergame.conf"; } ||
        { echo "FAIL $size: the per-game shader was not saved: $(cat "$FX/data/pergame.conf" 2>/dev/null)"; fail=1; }
    [ ! -f "$FX/states/Mednafen/Banana Kart.state" ] || { echo "FAIL $size: the state was not deleted"; fail=1; }
    ls "$FX/states/Mednafen/" 2>/dev/null | grep -q 'Banana Kart.state.bak-' || { echo "FAIL $size: no backup was made"; fail=1; }
    [ -s "$OUT/$size-xgame.png" ] || { echo "FAIL $size: no xgame shot"; fail=1; }
    for s in states states2; do [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }; done
    # Background Music: the Display & sound row (the eighth, after Style, Theme, Language, Brightness, Volume, Night
    # light and Menu sounds) toggles settings.conf's bgm, off then back on
    grep -q '^bgm = ' "$FX/data/settings.conf" && sed -i 's/^bgm = .*/bgm = on/' "$FX/data/settings.conf" || printf 'bgm = on\n' >> "$FX/data/settings.conf"
    blog=$OUT/$size-bgm.log
    run_bgm() { "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" --script "$1" 2>> "$blog" ||
        { echo "FAIL $size: banana-ui (bgm) exited $?"; fail=1; }; }
    # the Shaders group is the seventh tile; its rows save at once, and the strength slider stops at its end
    sed -i '/^shader_/d' "$FX/data/settings.conf"
    run_bgm "home 0; press start; press r; press r; press r; press r; press r; press r; press a; press right; press down; press right; press right; press down; press right; shot shaders"
    grep -q '^shader_mask = crt240' "$FX/data/settings.conf" || { echo "FAIL $size: Screen mask didn't save: $(grep '^shader_mask =' "$FX/data/settings.conf")"; fail=1; }
    grep -q '^shader_mask_intensity = 70' "$FX/data/settings.conf" || { echo "FAIL $size: Mask strength didn't save: $(grep '^shader_mask_intensity' "$FX/data/settings.conf")"; fail=1; }
    grep -q '^shader_color = trinitron' "$FX/data/settings.conf" || { echo "FAIL $size: Screen colour didn't save: $(grep '^shader_color' "$FX/data/settings.conf")"; fail=1; }
    run_bgm "home 0; press start; press r; press r; press r; press r; press r; press r; press a; press down; press right; press right; press right; press right; shot shaders-max"
    grep -q '^shader_mask_intensity = 100' "$FX/data/settings.conf" || { echo "FAIL $size: Mask strength didn't stop at 100: $(grep '^shader_mask_intensity' "$FX/data/settings.conf")"; fail=1; }
    sed -i '/^shader_/d' "$FX/data/settings.conf"
    run_bgm "home 0; press start; press a; press down; press down; press down; press down; press down; press down; press down; press right; shot bgm-off"
    grep -q '^bgm = off' "$FX/data/settings.conf" || { echo "FAIL $size: Background Music Off didn't save: $(grep '^bgm' "$FX/data/settings.conf")"; fail=1; }
    run_bgm "home 0; press start; press a; press down; press down; press down; press down; press down; press down; press down; press right; shot bgm-on"
    grep -q '^bgm = on' "$FX/data/settings.conf" || { echo "FAIL $size: Background Music On didn't save: $(grep '^bgm' "$FX/data/settings.conf")"; fail=1; }
    for s in bgm-off bgm-on; do [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }; done
    # the AMOLED burn-in guard follows bananad.conf: 2 min idle dims the panel to 30%, 90 s later it blanks, and the
    # first press restores it and is swallowed (it must not act on the menu)
    glog=$OUT/$size-guard.log
    printf 'dim_minutes = 2\ndim_percent = 30\n' > "$FX/data/bananad.conf"
    printf '200\n' > "$FX/bl"; printf '255\n' > "$FX/blmax"
    "$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; guard $FX/bl $FX/blmax; idle 120000; wait 1; shot guard-dim; idle 90000; wait 1; shot guard-blank; press a; wait 1; shot guard-wake" 2> "$glog" ||
        { echo "FAIL $size: banana-ui (guard) exited $?"; fail=1; }
    grep -q "guard: idle +120000 ms -> stage 1, level 76" "$glog" || { echo "FAIL $size: the guard didn't dim to 30%: $(grep 'guard: idle' "$glog" | tr '\n' '|')"; fail=1; }
    grep -q "guard: idle +90000 ms -> stage 2, level 0" "$glog" || { echo "FAIL $size: the guard didn't blank the panel"; fail=1; }
    grep -q "press a: swallowed by the burn-in guard" "$glog" || { echo "FAIL $size: waking the panel wasn't swallowed"; fail=1; }
    [ "$(cat "$FX/bl")" = 200 ] || { echo "FAIL $size: the guard didn't restore the brightness: $(cat "$FX/bl")"; fail=1; }
    for s in guard-dim guard-blank guard-wake; do [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }; done
    # Settings > System's two resets: the system one puts the menu's own keys back (theme, sounds, ...) and leaves
    # the emulator's (core.gba) alone; then the emulator one puts those back and leaves the menu's alone
    rlog=$OUT/$size-reset.log
    for kv in "theme = light" "sounds = off" "core.gba = mgba"; do
        k=${kv%% *}
        grep -q "^$k = " "$FX/data/settings.conf" && sed -i "s/^$k = .*/$kv/" "$FX/data/settings.conf" || printf '%s\n' "$kv" >> "$FX/data/settings.conf"
    done
    # groups: Display & sound, Sleep & power, RetroArch, Network, System; System's rows
    # from Time zone: down twelve to Reset system settings (Back up/Restore, Previous version, SSH, ADB debugging,
    # Mass storage, the on-screen info row and First-time setup are before it), A twice
    run_reset() { "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" --script "$1" 2>> "$rlog" ||
        { echo "FAIL $size: banana-ui (reset) exited $?"; fail=1; }; }
    run_reset "home 0; press start; press r; press r; press r; press r; press a; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press a; press a; wait 300; shot reset-system"
    grep -q '^theme = dark' "$FX/data/settings.conf" || { echo "FAIL $size: system reset didn't restore theme: $(grep '^theme' "$FX/data/settings.conf")"; fail=1; }
    grep -q '^sounds = on' "$FX/data/settings.conf" || { echo "FAIL $size: system reset didn't restore sounds"; fail=1; }
    grep -q '^core.gba = mgba' "$FX/data/settings.conf" || { echo "FAIL $size: system reset wiped an emulator core: $(grep '^core.gba' "$FX/data/settings.conf")"; fail=1; }
    # down thirteen from Time zone to Reset emulator settings, A twice (theme and sounds are already back)
    run_reset "home 0; press start; press r; press r; press r; press r; press a; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press down; press a; press a; wait 300; shot reset-emulator"
    grep -q '^core.gba = gpsp' "$FX/data/settings.conf" || { echo "FAIL $size: emulator reset didn't restore core.gba: $(grep '^core.gba' "$FX/data/settings.conf")"; fail=1; }
    grep -q '^theme = dark' "$FX/data/settings.conf" || { echo "FAIL $size: emulator reset wiped a system key"; fail=1; }
    for s in reset-system reset-emulator; do
        [ -s "$OUT/$size-$s.png" ] || { echo "FAIL $size: no $s shot"; fail=1; }
    done
    # Settings > System's backup rows run the tool off the loop: Back up takes one A, Restore two (and then restarts
    # the menu, which ends the headless run). The tool is a stand-in that records how it was called.
    bmark=$FX/backup-called; rm -f "$bmark"
    printf '#!/bin/sh\necho "$*" >> %s\necho "ok test.tar.gz"\n' "$bmark" > "$FX/backup-tool"
    chmod +x "$FX/backup-tool"
    BANANA_BACKUP="$FX/backup-tool" "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; press start; press r; press r; press r; press r; press a; press down; press down; press a; press a; wait 600; \
            press down; press a; press a; wait 600" 2>> "$rlog" ||
        { echo "FAIL $size: banana-ui (backup rows) exited $?"; fail=1; }
    grep -qx create "$bmark" 2>/dev/null || { echo "FAIL $size: Back up did not run the tool"; fail=1; }
    grep -qx restore "$bmark" 2>/dev/null || { echo "FAIL $size: Restore did not run the tool"; fail=1; }
    # saved-network priority: the Network page's L/R swaps a saved network's place in wifi.conf (higher/lower),
    # and a network that is not saved is left alone
    printf 'network={\n\tssid="Home"\n\tpsk="12345678"\n\tpriority=1\n}\nnetwork={\n\tssid="Cafe"\n\tpsk="12345678"\n\tpriority=2\n}\n' > "$FX/data/wifi.conf"
    "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" --script "wifi Home up; wifi Nobody up" \
        2>> "$rlog" || { echo "FAIL $size: banana-ui (wifi priority) exited $?"; fail=1; }
    grep -A3 'ssid="Home"' "$FX/data/wifi.conf" | grep -q 'priority=2' || { echo "FAIL $size: Home did not move up"; fail=1; }
    grep -A3 'ssid="Cafe"' "$FX/data/wifi.conf" | grep -q 'priority=1' || { echo "FAIL $size: Cafe did not move down"; fail=1; }
    grep -q 'wifi Nobody up: no change' "$rlog" || { echo "FAIL $size: an unsaved network changed the file"; fail=1; }
    # the old WEP: its key is written as key_mgmt=NONE + wep_key0, a bad key is refused, and the scan flags decide
    "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "wifi-add WepNet abcde wep; wifi-add BadKey abc wep; wifi-scan WEP; wifi-scan WPA2-PSK-CCMP" \
        2>> "$rlog" || { echo "FAIL $size: banana-ui (wep) exited $?"; fail=1; }
    grep -A5 'ssid="WepNet"' "$FX/data/wifi.conf" | grep -q 'key_mgmt=NONE' || { echo "FAIL $size: no key_mgmt=NONE for WEP"; fail=1; }
    grep -A5 'ssid="WepNet"' "$FX/data/wifi.conf" | grep -q 'wep_key0="abcde"' || { echo "FAIL $size: no wep_key0"; fail=1; }
    grep -A5 'ssid="WepNet"' "$FX/data/wifi.conf" | grep -q 'wep_tx_keyidx=0' || { echo "FAIL $size: no wep_tx_keyidx"; fail=1; }
    grep -q 'ssid="BadKey"' "$FX/data/wifi.conf" && { echo "FAIL $size: a 3-character WEP key was accepted"; fail=1; }
    grep -q 'wifi-scan WEP: secured=1 wep=1 unsupported=0' "$rlog" || { echo "FAIL $size: WEP scan flags are wrong"; fail=1; }
    grep -q 'wifi-scan WPA2-PSK-CCMP: secured=1 wep=0 unsupported=0' "$rlog" || { echo "FAIL $size: WPA scan flags are wrong"; fail=1; }
    # resume: the boot decision (ready / missing / blocked) and the two consumers, through the script verb
    ROM="$FX/roms/ngp/Peel Quest.ngp"; RR=$FX/data/resume; RT=$FX/data/resume-tries
    resume_run() {	# resume_run <marker text or ""> <tries or ""> [take|cancel]
        rm -f "$RR" "$RT"; [ -n "$1" ] && echo "$1" > "$RR"; [ -n "$2" ] && echo "$2" > "$RT"
        "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
            --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" --script "resume $3" 2>> "$rlog" ||
            { echo "FAIL $size: banana-ui (resume) exited $?"; fail=1; }; }
    resume_run "$ROM" ""
    grep -q "resume: ready $ROM" "$rlog" || { echo "FAIL $size: a real ROM is not ready to resume"; fail=1; }
    [ -f "$RR" ] || { echo "FAIL $size: the plan consumed the marker"; fail=1; }
    resume_run "$ROM" "" take
    [ ! -e "$RR" ] && [ "$(cat "$RT" 2>/dev/null)" = 1 ] || { echo "FAIL $size: take did not consume and count"; fail=1; }
    resume_run "/nope/Missing.ngp" ""
    grep -q 'resume: missing' "$rlog" || { echo "FAIL $size: a missing ROM was not reported"; fail=1; }
    [ ! -e "$RR" ] || { echo "FAIL $size: a missing ROM left the marker"; fail=1; }
    resume_run "$ROM" 3
    grep -q 'resume: blocked' "$rlog" || { echo "FAIL $size: the tries cap did not block"; fail=1; }
    [ ! -e "$RR" ] && [ ! -e "$RT" ] || { echo "FAIL $size: blocked did not clear both files"; fail=1; }
    resume_run "$ROM" "" cancel
    [ ! -e "$RR" ] && [ ! -e "$RT" ] || { echo "FAIL $size: cancel did not clear both files"; fail=1; }
    # the Settings screen in German: the catalog in ui/assets/lang loads (BANANA_LANG_DIR) and the menu draws with it
    sed -i '/^lang = /d' "$FX/data/settings.conf"; echo 'lang = de' >> "$FX/data/settings.conf"
    BANANA_LANG_DIR="$HERE/assets/lang" "$HERE/banana-ui" --headless "$OUT/$size-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" --script "home 0; press start; wait 300; shot settings-de" \
        2>> "$rlog" || { echo "FAIL $size: banana-ui (german) exited $?"; fail=1; }
    grep -q 'i18n: de (1[0-9][0-9] strings' "$rlog" || { echo "FAIL $size: the German catalog did not load"; fail=1; }
    sed -i '/^lang = /d' "$FX/data/settings.conf"; echo 'lang = en' >> "$FX/data/settings.conf"
    # a layout theme (the image's pocket.json, the handheld's own): every screen drawn from its JSON, home ->
    # library -> a launch, and a tap on a system. It is the single-panel layout, so it runs at its own 640x557.
    # The menu reads the theme from the card's settings.conf (--theme is the real launcher's), so point it at the
    # file for this run and put it back (the settings cases above check for dark).
    llog=$OUT/$size-layout.log
    sed -i "s|^ *theme *=.*|theme = file:$HERE/../system/rootfs/usr/share/banana/themes/pocket.json|" "$FX/data/settings.conf"
    "$HERE/banana-ui" --headless "$OUT/$size-layout-" --size 640x557 --assets "$HERE/assets/rnds" --banana "$HERE/assets/banana" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; wait 1000; shot home; press right; wait 1000; shot home2; touch 209 290; wait 1000; press a; wait 1000; shot lib; \
            press down; wait 600; shot lib2; press a; wait 350; shot launch; wait 1000; wait 1" 2> "$llog" || { echo "FAIL $size: banana-ui (layout) exited $?"; fail=1; }
    sed -i "s|^ *theme *=.*|theme = dark|" "$FX/data/settings.conf"
    grep -q "theme:" "$llog" && { echo "FAIL $size: the layout theme: $(grep 'theme:' "$llog" | head -3)"; fail=1; }
    grep -q "touch 209 290: used" "$llog" || { echo "FAIL $size: the layout theme's tap on a system"; fail=1; }
    grep -q "press down: used, view 1 sys 0 game 1" "$llog" || { echo "FAIL $size: the layout theme's own buttons (down: the next game)"; fail=1; }
    grep -q "launch 1: .*Monkey Puzzle League.ngp" "$llog" || { echo "FAIL $size: the layout theme's launch: $(grep launch "$llog")"; fail=1; }
    # the Pocket layout is single-screen (one panel): only the "top" canvas has anything on it
    for s in home home2 lib lib2 launch; do
        f=$OUT/$size-layout-$s.png
        [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
        [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
    done
    # golden: the layout theme's key screens too
    for s in home lib launch; do golden "layout-$s" "$OUT/$size-layout-$s.png"; done
    # Banana Street: a layout theme on its own 310-wide canvas ("canvas"), drawn at the handheld's real 1240x1080 so
    # the whole-pixel scale (exactly 4) is the one the panel gets. Walk one shop, open it, move down the shelf.
    stlog=$OUT/$size-street.log
    sed -i "s|^ *theme *=.*|theme = file:$HERE/../system/rootfs/usr/share/banana/themes/street.json|" "$FX/data/settings.conf"
    # The time of day is pinned (BANANA_PHASE) so the art, and the golden, don't follow the clock. L and R jump a maker's
    # block: from Game Boy Advance (Nintendo) to Ports (the next maker), and back to the first of the Nintendo block.
    BANANA_PHASE=dusk "$HERE/banana-ui" --headless "$OUT/$size-street-" --size 1240x1080 --assets "$HERE/assets/rnds" --banana "$HERE/assets/banana" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; wait 1000; shot home; press right; wait 1000; shot home2; press r; wait 600; press l; wait 600; press a; wait 1000; shot lib; press down; wait 600; shot lib2; press a; wait 350; shot launch; wait 1000" \
        2> "$stlog" || { echo "FAIL $size: banana-ui (street) exited $?"; fail=1; }
    sed -i "s|^ *theme *=.*|theme = dark|" "$FX/data/settings.conf"
    grep -q "theme:" "$stlog" && { echo "FAIL $size: the street theme: $(grep 'theme:' "$stlog" | head -3)"; fail=1; }
    grep -q "press down: used, view 1 sys 1 game 1" "$stlog" || { echo "FAIL $size: the street's shelf (down: the next game)"; fail=1; }
    grep -q "press r: used, view 0 sys 4 " "$stlog" || { echo "FAIL $size: the street's R (the next maker's block): $(grep 'press r' "$stlog")"; fail=1; }
    grep -q "press l: used, view 0 sys 1 " "$stlog" || { echo "FAIL $size: the street's L (back to the block's first): $(grep 'press l' "$stlog")"; fail=1; }
    for s in home home2 lib lib2; do
        f=$OUT/$size-street-$s.png
        [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
        [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
    done
    for s in home lib; do golden "street-$s" "$OUT/$size-street-$s.png"; done
    # halfway through the launch the street's mosaic has the picture in square blocks (8 to 16 units, 32 to 64 px
    # here): the shot equals itself sampled down to one pixel a block and back up, for one of those block sizes
    f=$OUT/$size-street-launch.png; mos=0
    for n in 32 36 40 44 48 52 56 60 64; do
        w=$((1240 / n * n)); h=$((1080 / n * n))
        convert "$f" -crop ${w}x${h}+0+0 +repage "$OUT/mos-a.png" 2>/dev/null
        convert "$OUT/mos-a.png" -sample $((w / n))x$((h / n))! -sample ${w}x${h}! "$OUT/mos-b.png" 2>/dev/null
        [ "$(compare -metric AE "$OUT/mos-a.png" "$OUT/mos-b.png" null: 2>&1 | cut -d' ' -f1)" = 0 ] && { mos=$n; break; }
    done
    [ "$mos" != 0 ] || { echo "FAIL $size: the street's launch isn't a mosaic"; fail=1; }
    # its Settings are the workshop (the SNES style's painter): open Display & sound, step down to Night light (Style,
    # Theme, Language, Brightness, Volume, then it) and change it: saved like the Classic screen's
    sed -i "s|^ *theme *=.*|theme = file:$HERE/../system/rootfs/usr/share/banana/themes/street.json|" "$FX/data/settings.conf"
    sed -i '/^night_light *=/d' "$FX/data/settings.conf"
    BANANA_PHASE=dusk "$HERE/banana-ui" --headless "$OUT/$size-street-" --size 1240x1080 --assets "$HERE/assets/rnds" --banana "$HERE/assets/banana" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; wait 600; press start; wait 300; shot workshop; press a; press down; press down; press down; press down; press down; press right; wait 300; shot workshop2" \
        2>> "$stlog" || { echo "FAIL $size: banana-ui (workshop) exited $?"; fail=1; }
    sed -i "s|^ *theme *=.*|theme = dark|" "$FX/data/settings.conf"
    grep -q '^night_light = warm' "$FX/data/settings.conf" || { echo "FAIL $size: the workshop's Night light didn't save: $(grep '^night_light' "$FX/data/settings.conf")"; fail=1; }
    sed -i '/^night_light *=/d' "$FX/data/settings.conf"
    golden "street-workshop" "$OUT/$size-street-workshop.png"
    # its keyboard is the phone book and its pause menu the ticket. Type q, w, then Shift and S; a tap on the page's E
    # key (row 1, col 2: 84,97 on the 310 grid, 173,200 in layout px) types it, so the page's keys are where taps land.
    # Then B deletes the E: "qwS" is left, and START looks it up.
    sed -i "s|^ *theme *=.*|theme = file:$HERE/../system/rootfs/usr/share/banana/themes/street.json|" "$FX/data/settings.conf"
    BANANA_PHASE=dusk "$HERE/banana-ui" --headless "$OUT/$size-street-" --size 1240x1080 --assets "$HERE/assets/rnds" --banana "$HERE/assets/banana" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "home 0; wait 600; search; wait 200; press a; press right; press a; press x; press down; press a; touch 173 200; wait 100; shot phonebook; press b; press start; wait 300; pause ticket 1" \
        2>> "$stlog" || { echo "FAIL $size: banana-ui (phone book) exited $?"; fail=1; }
    sed -i "s|^ *theme *=.*|theme = dark|" "$FX/data/settings.conf"
    grep -q 'search "qwS":' "$stlog" || { echo "FAIL $size: the phone book's search: $(grep 'search' "$stlog" | tail -2)"; fail=1; }
    grep -q 'pause ticket: card 16,16 1208x1048' "$stlog" || { echo "FAIL $size: the ticket's card: $(grep 'pause ticket' "$stlog")"; fail=1; }
    golden "street-phonebook" "$OUT/$size-street-phonebook.png"
    golden "street-ticket" "$OUT/$size-street-ticket.png"
    # the other SNES neighborhoods (street-*.json, generated from street.json by tools/gen-street.py): each draws its
    # street, a shop, the workshop and the ticket with no theme error. Loaded as "file:" themes, which the Theme row
    # lists (it used to be empty for one, and the workshop crashed reading it).
    for hood in harbor blossom sundown oldtown; do
        hlog=$OUT/$size-street-$hood.log
        sed -i "s|^ *theme *=.*|theme = file:$HERE/../system/rootfs/usr/share/banana/themes/street-$hood.json|" "$FX/data/settings.conf"
        BANANA_PHASE=day "$HERE/banana-ui" --headless "$OUT/$size-street-$hood-" --size 1240x1080 --assets "$HERE/assets/rnds" --banana "$HERE/assets/banana" \
            --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
            --script "home 0; wait 800; shot home; press a; wait 900; shot lib; press b; wait 600; press start; wait 300; press a; shot workshop; pause ticket 1" \
            2> "$hlog" || { echo "FAIL $size: banana-ui ($hood) exited $?"; fail=1; }
        grep -q "theme:" "$hlog" && { echo "FAIL $size: the $hood theme: $(grep 'theme:' "$hlog" | head -3)"; fail=1; }
        for s in home lib workshop ticket; do
            f=$OUT/$size-street-$hood-$s.png
            [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
            [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
        done
        golden "street-$hood-home" "$OUT/$size-street-$hood-home.png"
    done
    sed -i "s|^ *theme *=.*|theme = dark|" "$FX/data/settings.conf"
    # The one panel offers the layout themes only: a colour theme (Dark/Light) is forced to the Pocket layout there,
    # so they were rows that changed nothing. From a stored dark the Theme row reads Pocket, and stepping right
    # picks the next layout (not Light) -- which the value it writes proves.
    tllog=$OUT/$size-themelist.log
    mkdir -p "$FX/data/themes"
    cp -f "$HERE/../system/rootfs/usr/share/banana/themes/"*.json "$FX/data/themes/"
    printf 'theme = dark\n' > "$FX/data/settings.conf"
    BANANA_THEMES="$FX/data/themes" "$HERE/banana-ui" --headless "$OUT/$size-themelist-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "press start; press a; press down; press right; shot list; wait 1" 2> "$tllog" ||
        { echo "FAIL $size: banana-ui (theme list) exited $?"; fail=1; }
    grep -q '^theme = user:pocket-ocean$' "$FX/data/settings.conf" || { echo "FAIL $size: the one-panel theme list: $(grep '^theme' "$FX/data/settings.conf")"; fail=1; }
    # Style (the row above Theme): Classic -> SNES picks Banana Street and remembers the Classic theme in use; back to
    # Classic restores it (not Pocket, the default)
    sylog=$OUT/$size-style.log
    printf 'theme = user:pocket-terminal\n' > "$FX/data/settings.conf"
    BANANA_THEMES="$FX/data/themes" "$HERE/banana-ui" --headless "$OUT/$size-style-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "press start; press a; press right; shot snes; wait 1" 2> "$sylog" || { echo "FAIL $size: banana-ui (style) exited $?"; fail=1; }
    grep -q '^theme = user:street$' "$FX/data/settings.conf" || { echo "FAIL $size: Style SNES: $(grep '^theme' "$FX/data/settings.conf" | tr '\n' ' ')"; fail=1; }
    grep -q '^theme_classic = user:pocket-terminal$' "$FX/data/settings.conf" || { echo "FAIL $size: Style didn't remember the Classic theme"; fail=1; }
    BANANA_THEMES="$FX/data/themes" "$HERE/banana-ui" --headless "$OUT/$size-style-" --size 640x557 --assets "$HERE/assets/rnds" \
        --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
        --script "press start; press a; press left; shot classic; wait 1" 2>> "$sylog" || { echo "FAIL $size: banana-ui (style back) exited $?"; fail=1; }
    grep -q '^theme = user:pocket-terminal$' "$FX/data/settings.conf" || { echo "FAIL $size: Style back to Classic: $(grep '^theme' "$FX/data/settings.conf" | tr '\n' ' ')"; fail=1; }
    [ -s "$OUT/$size-themelist-list.png" ] || { echo "FAIL $size: no themelist shot"; fail=1; }
    rm -rf "$FX/data/themes"; printf 'theme = dark\n' > "$FX/data/settings.conf"
    grep 'bench:' "$log" | sed "s/^banana-ui: /$size /"
    grep 'boot-metrics:' "$log" | sed "s/^banana-ui: /$size /"
done
# Settings: the menu has one screen (the RPC's panel), so the settings pages and their shots are rendered alone at
# 640x557.
size=640x557
slog=$OUT/$size-settings.log
"$HERE/banana-ui" --headless "$OUT/$size-" --size $size --assets "$HERE/assets/rnds" \
    --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
    --script "home 0; press start; shot settings; press a; press down; shot settings-list; press r; press r; press r; press r; shot settings-system; press right; shot settings-tz; press b; press b; wait 1" 2> "$slog" ||
    { echo "FAIL $size: banana-ui (settings) exited $?"; fail=1; }
grep -q "press start: settings open" "$slog" || { echo "FAIL $size: settings didn't open"; fail=1; }
grep -q "press b: settings closed" "$slog" || { echo "FAIL $size: settings didn't close"; fail=1; }
grep -q '^timezone = [^U]' "$FX/data/settings.conf" || { echo "FAIL $size: the time zone wasn't saved"; fail=1; }
for s in settings settings-list settings-system settings-tz; do
    f=$OUT/$size-$s.png
    [ -s "$f" ] || { echo "FAIL $size: no $f"; fail=1; continue; }
    [ "$(convert "$f" -format '%[fx:standard_deviation>0.01]' info:)" = 1 ] || { echo "FAIL $size: $f is blank"; fail=1; }
done
for s in settings settings-system; do golden "$s" "$OUT/$size-$s.png"; done
[ $fail = 0 ] && echo "banana-ui: all checks passed ($OUT)"
exit $fail
