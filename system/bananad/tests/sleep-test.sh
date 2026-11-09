#!/bin/sh
# bananad's sleep, tested without the handheld: bananad runs in its own mount and PID namespaces against a made-up
# /sys (the battery, the backlight, the RTC, /sys/power) and /dev/input (FIFOs that carry real
# struct input_event records). /sys/power/state is a FIFO: bananad's write of "mem" blocks until the test reads it,
# which is the handheld being asleep; the test decides what the world looks like when it "wakes". A stand-in game
# (its process group must be stopped while asleep) and a stand-in banana-ui (it must get SIGUSR1 after a wake).
#
#   sudo system/bananad/tests/sleep-test.sh        (root: it mounts in a private namespace; nothing outside changes)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOTFS=$(cd "$HERE/../rootfs" && pwd)
if [ "$1" != --inside ]; then
    W=$(mktemp -d)
    cc -O2 -o "$W/bananad" "$HERE/bananad.c" -lasound
    cc -O2 -o "$W/banana-ui" -x c - <<'EOF'
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
/* the menu's part of the handover: it publishes what its PCM is doing, and closes it on SIGHUP -- after 50 ms, so the
 * test can tell bananad waited for the note (not a fixed delay) */
static void state(const char *s) { FILE *f = fopen("/run/banana/sound-state", "w"); if (f) { fputs(s, f); fclose(f); } }
static void on(int s) { FILE *f = fopen("/run/test/ui-signals", "a"); if (f) { fprintf(f, "SIGUSR1\n"); fclose(f); } }
static void onhup(int s) { usleep(50000); state("closed (test)\n"); }
int main(void) { signal(SIGUSR1, on); signal(SIGHUP, onhup); state("open (test)\n"); for (;;) pause(); }
EOF
    mkdir -p /data 2>/dev/null || true
    exec unshare -m -p -f --mount-proc sh "$0" --inside "$W" "$ROOTFS"
fi

# ---- inside: pid 1 is this shell, so bananad's "shut down" (SIGUSR2 to init) comes here ------------------------
W=$2 ROOTFS=$3
POWEROFF=0
trap 'POWEROFF=1' USR2
mount -t tmpfs none /sys/power
mount -t tmpfs none /sys/class
mount -t tmpfs none /dev/input 2>/dev/null || { mkdir -p /dev/input; mount -t tmpfs none /dev/input; }
mount -t tmpfs none /run
mount -t tmpfs none /data
mkdir -p /etc/banana && mount -t tmpfs none /etc/banana && cp -r "$ROOTFS/etc/banana/." /etc/banana/
mkdir -p /run/banana /run/test /data/banana/logs /sys/class/rtc/rtc0 /sys/class/power_supply/battery
echo Battery > /sys/class/power_supply/battery/type
echo Discharging > /sys/class/power_supply/battery/status
echo 50 > /sys/class/power_supply/battery/capacity
: > /sys/class/rtc/rtc0/wakealarm
echo "s2idle [deep]" > /sys/power/mem_sleep
echo 0 > /sys/power/pm_async
for b in backlight0 backlight1; do mkdir -p /sys/class/backlight/$b; echo 255 > /sys/class/backlight/$b/max_brightness; echo 200 > /sys/class/backlight/$b/brightness; echo 0 > /sys/class/backlight/$b/bl_power; done
mkfifo /sys/power/state /dev/input/event0
exec 3<>/dev/input/event0       # a writer that stays, so bananad's poll doesn't see a hang-up

# the sleep.d hooks' world. In the image /usr/libexec/banana holds the helpers they call; on this host it does not,
# so every hook would take its "[ -x ... ] || exit 0" guard and no-op -- which is what let a broken hook ship. The
# three the hooks use are stand-ins that record what they were asked, so the hooks themselves are what is tested.
# 10-wifi also needs an interface and a wpa pid, 30-usb a bound UDC.
# the path has to exist to be a mount point, and /usr is read-only here: cover /usr with a private tmpfs and bind the
# host's own bin/lib back in (python3 and the shell keep working). A host that has /usr/libexec (Debian/Ubuntu, CI)
# skips this.
if [ ! -d /usr/libexec ]; then
    # /usr is read-only and has no libexec: build the tree in /run (writable), bind the host's own bin/lib into it,
    # then bind that over /usr -- the mount points exist before /usr is covered, so nothing is lost
    mkdir -p /run/newusr/libexec/banana
    for b in bin lib lib64 sbin; do
        if [ -d /usr/$b ]; then mkdir -p /run/newusr/$b; mount --bind /usr/$b /run/newusr/$b; fi
    done
    mount --rbind /run/newusr /usr      # rbind: a plain bind would not carry the bin/lib mounts above
fi
mkdir -p /usr/libexec/banana
for h in wifi sound-server usb-gadget; do
    {
        echo '#!/bin/sh'
        echo 'printf "%s%s\n" "$(basename "$0")" "${1:+ $*}" >> /run/test/hook-calls'
    } > /usr/libexec/banana/$h
    chmod +x /usr/libexec/banana/$h
done
mkdir -p /sys/class/net/wlan0
: > /run/banana/wpa.pid
mount -t tmpfs none /sys/kernel
mkdir -p /sys/kernel/config/usb_gadget/banana
echo a600000.usb > /sys/kernel/config/usb_gadget/banana/UDC

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
# an input event: type code value (then a SYN_REPORT)
ev() { python3 -c "
import struct, sys
t, c, v = (int(x) for x in sys.argv[1:4])
sys.stdout.buffer.write(struct.pack('llHHi', 0, 0, t, c, v) + struct.pack('llHHi', 0, 0, 0, 0, 0))" "$@" >&3; }
EV_KEY=1 KEY_POWER=116

# The RetroArch stand-in for hibernation: its command FIFO (the daemon's stdin channel) and a state writer. What it does
# with SAVE_STATE is /run/test/ra-mode: ok (half the file, pause, the rest -- so the size changes and then settles),
# none (never writes it). The state path is the one RetroArch would use (savestate_directory + content name + .state).
mkdir -p /data/states /data/retroarch
printf 'savestate_directory = "/data/states"\nstate_slot = "0"\n' > /data/retroarch/retroarch.cfg
rm -f /run/banana/retroarch.cmd; mkfifo /run/banana/retroarch.cmd
: > /run/test/ra-mode; : > /run/test/ra-cmds
(
    exec 4<>/run/banana/retroarch.cmd
    while IFS= read -r cmd <&4; do
        printf '%s\n' "$cmd" >> /run/test/ra-cmds
        if [ "$cmd" = SAVE_STATE ]; then
            rom=$(cat /run/banana/game.path 2>/dev/null)
            if [ -n "$rom" ]; then
                base=${rom##*/}; base=${base%.*}
                case $(cat /run/test/ra-mode 2>/dev/null) in
                ok) printf 'half' > "/data/states/$base.state"; sleep 0.2; printf ' rest' >> "/data/states/$base.state" ;;
                esac
            fi
        fi
    done
) &

# press and release power: bananad sleeps on the release
power() { ev $EV_KEY $KEY_POWER 1; sleep 0.1; ev $EV_KEY $KEY_POWER 0; }
# is bananad blocked writing "mem" (asleep)? wait up to 3 s
# (blocked opening the FIFO: its wait channel is wait_for_partner)
asleep() { for i in $(seq 30); do [ "$(cat /proc/$BD/wchan 2>/dev/null)" = wait_for_partner ] && return 0; sleep 0.1; done; return 1; }
# read "mem": the write returns, it is awake. A real power-key wake carries a KEY_POWER press (the spurious-wake
# guard re-sleeps a wake with no key and no cable), so wake() queues one before releasing the FIFO; an RTC wake
# (the battery check's alarm) has no key. 0.4.54 added the guard after this test, which is why wake() has the press.
wake() { ev $EV_KEY $KEY_POWER 1; head -c 3 /sys/power/state > /dev/null; sleep 0.4; }
rtc_wake() { head -c 3 /sys/power/state > /dev/null; sleep 0.4; }
gstate() { awk '{print $3}' /proc/$GAME/stat; }
start() {   # start [bananad.conf lines]
    echo "s2idle [deep]" > /sys/power/mem_sleep     # the kernel's file always lists both
    printf '%s\n' "autosleep_minutes = 0" "sleep_check_hours = 0" "$@" > /data/banana/bananad.conf
    "$W/bananad" & BD=$!
    sleep 0.6
}
stop() { kill $BD 2>/dev/null; wait $BD 2>/dev/null || true; }

"$W/banana-ui" & UI=$!
setsid sleep 600 & GAME=$!
sleep 0.2
echo $GAME > /run/banana/game.pid

echo "1. the power button sleeps and wakes it"
start
power
if asleep; then ok "suspended on the power button"; else bad "no suspend on the power button"; fi
grep -qx SAVE_STATE /run/test/ra-cmds && bad "freeze mode wrote SAVE_STATE" || ok "freeze mode writes no SAVE_STATE"
[ "$(gstate)" = T ] && ok "game stopped while asleep" || bad "game not stopped ($(gstate))"
[ "$(cat /sys/power/pm_async)" = 1 ] && ok "devices resume in parallel (pm_async 1)" || bad "pm_async $(cat /sys/power/pm_async)"
grep -q "\[deep\]" /sys/power/mem_sleep && ok "deep sleep chosen" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
grep -q 'going to sleep (power button)' /data/banana/logs/sleep-state && ok "the pre-sleep note was written" || bad "no sleep-state note"
wake
[ "$(gstate)" != T ] && ok "game running again after the wake" || bad "game still stopped"
rm -f /run/banana/game.pid; sleep 0.1
grep -q 'awake (power button)' /data/banana/logs/sleep.log && ok "sleep.log: $(tail -n1 /data/banana/logs/sleep.log | cut -c21-)" || bad "no wake in sleep.log"
grep -q 'battery 50% -> 50%' /data/banana/logs/sleep.log && ok "the sleep's battery cost is in sleep.log" || bad "no battery change in sleep.log"
stop

echo "1b. the sleep.d hooks run around the sleep (the daemon's handover to Wi-Fi, sound and USB)"
rm -f /run/test/hook-calls
start
power
asleep && ok "suspended for the hook check" || bad "no suspend for the hook check"
wake
sleep 0.8                       # the post hooks are backgrounded
pre=$(grep -E '^(wifi|sound-server|usb-gadget) down$' /run/test/hook-calls 2>/dev/null | tr '\n' ' ')
[ "$pre" = "wifi down sound-server down usb-gadget down " ] && ok "the pre hooks ran, in name order" || bad "pre hooks: '${pre:-none}'"
grep -qx 'wifi up' /run/test/hook-calls 2>/dev/null && ok "the post hook brought Wi-Fi back" || bad "no wifi up"
grep -qx 'sound-server up' /run/test/hook-calls 2>/dev/null && ok "the post hook brought the sound server back" || bad "no sound-server up"
grep -qx 'usb-gadget' /run/test/hook-calls 2>/dev/null && ok "the post hook bound the USB gadget again" || bad "no usb-gadget re-bind"
stop

echo "1c. savestate: SAVE_STATE, a verified state, the game closed, and the state handed back after the wake"
printf 'ok\n' > /run/test/ra-mode
: > /run/test/ra-cmds
rm -f /data/banana/resume /data/banana/resume-tries /run/banana/load-pending /run/banana/resume-now
setsid sleep 600 & HG=$!; sleep 0.1
echo $HG > /run/banana/game.pid
echo '/data/roms/gba/Test Game.zip' > /run/banana/game.path
start "suspend_mode = savestate"
power
asleep && ok "suspended after a verified save" || bad "no suspend after the save"
grep -qx SAVE_STATE /run/test/ra-cmds && ok "SAVE_STATE went down the pipe before the close" || bad "no SAVE_STATE: $(tr '\n' ' ' < /run/test/ra-cmds)"
grep -q 'hibernate: state saved to /data/states/Test Game.state, game closed' /data/banana/logs/sleep.log && ok "the finished state was verified" || bad "state not verified: $(tail -n1 /data/banana/logs/sleep.log)"
grep -qx '/data/roms/gba/Test Game.zip' /data/banana/resume 2>/dev/null && ok "the ROM was persisted" || bad "no resume marker"
[ ! -e /data/banana/resume.tmp ] && ok "the marker was renamed into place" || bad "a .tmp resume file was left"
[ ! -e /run/banana/shutting-down ] && ok "hibernation is not a shutdown" || bad "shutting-down was created"
wake
for i in $(seq 20); do [ -f /run/banana/load-pending ] && break; sleep 0.1; done
[ -f /run/banana/load-pending ] && ok "a pending state after the wake" || bad "no load-pending after the wake"
[ -f /run/banana/resume-now ] && ok "the menu was asked to resume" || bad "no resume-now marker"
[ ! -e /data/banana/resume-tries ] && ok "no resume try was counted for a wake" || bad "resume-tries was created"
setsid sleep 600 & HG2=$!; sleep 0.1
echo $HG2 > /run/banana/game.pid
for i in $(seq 40); do grep -qx LOAD_STATE /run/test/ra-cmds 2>/dev/null && break; sleep 0.1; done
[ "$(grep -c '^LOAD_STATE$' /run/test/ra-cmds)" = 1 ] && ok "LOAD_STATE delivered once, after the launch" || bad "LOAD_STATE $(grep -c '^LOAD_STATE$' /run/test/ra-cmds) times"
[ ! -e /run/banana/load-pending ] && ok "the pending state was consumed" || bad "load-pending was left behind"
kill $HG2 2>/dev/null; rm -f /run/banana/game.pid
stop

echo "1d. savestate is strict: no state within 1500 ms means no sleep, and the game keeps running"
printf 'none\n' > /run/test/ra-mode
: > /run/test/ra-cmds
rm -f /data/banana/resume /run/banana/load-pending /run/banana/resume-now
setsid sleep 600 & SB=$!; sleep 0.1
echo $SB > /run/banana/game.pid
echo '/data/roms/gba/Test Game.zip' > /run/banana/game.path
start "suspend_mode = savestate"
power
sleep 2.2
asleep 2>/dev/null && bad "it slept without a state" || ok "refused to sleep without a verified state"
[ "$(awk '{print $3}' /proc/$SB/stat)" != T ] && ok "the game kept running" || bad "the game was stopped"
[ ! -e /data/banana/resume ] && ok "no resume marker was written" || bad "a resume marker was left"
grep -q 'no finished state at /data/states/Test Game.state within 1500 ms' /data/banana/logs/sleep.log && ok "the timeout was logged" || bad "timeout not logged: $(tail -n1 /data/banana/logs/sleep.log)"
kill $SB 2>/dev/null; rm -f /run/banana/game.pid
stop

echo "1e. hybrid: a save that fails falls back to the freeze"
printf 'none\n' > /run/test/ra-mode
rm -f /data/banana/resume /run/banana/load-pending /run/banana/resume-now
setsid sleep 600 & HY=$!; sleep 0.1
echo $HY > /run/banana/game.pid
echo '/data/roms/gba/Test Game.zip' > /run/banana/game.path
start "suspend_mode = hybrid"
power
sleep 2
asleep && ok "slept the fallback way" || bad "no sleep after the fallback"
[ "$(awk '{print $3}' /proc/$HY/stat)" = T ] && ok "the game was frozen" || bad "the game was not frozen"
grep -q 'hibernate failed: freezing the game instead' /data/banana/logs/sleep.log && ok "the fallback was logged" || bad "fallback not logged"
[ ! -e /data/banana/resume ] && ok "a frozen sleep leaves no resume marker" || bad "a resume marker was left"
wake; sleep 0.2
[ "$(awk '{print $3}' /proc/$HY/stat)" != T ] && ok "the game ran on after the wake" || bad "the game stayed frozen"
kill $HY 2>/dev/null; rm -f /run/banana/game.pid
stop

echo "1f. a state is never loaded into another ROM"
printf 'ok\n' > /run/test/ra-mode
: > /run/test/ra-cmds
rm -f /data/banana/resume /run/banana/load-pending /run/banana/resume-now
setsid sleep 600 & A=$!; sleep 0.1
echo $A > /run/banana/game.pid
echo '/data/roms/gba/Test Game.zip' > /run/banana/game.path
start "suspend_mode = savestate"
power
asleep && ok "hibernated for the mismatch check" || bad "no hibernation sleep"
wake
for i in $(seq 20); do [ -f /run/banana/load-pending ] && break; sleep 0.1; done
setsid sleep 600 & B=$!; sleep 0.1
echo $B > /run/banana/game.pid
echo '/data/roms/gba/Other.gba' > /run/banana/game.path
sleep 0.8
grep -qx LOAD_STATE /run/test/ra-cmds && bad "a state was loaded into another ROM" || ok "no state for a different ROM"
[ ! -e /run/banana/load-pending ] && ok "the pending state was dropped" || bad "load-pending survived"
grep -q 'started instead of' /data/banana/logs/sleep.log && ok "the mismatch was logged" || bad "mismatch not logged"
kill $B 2>/dev/null; rm -f /run/banana/game.pid
stop

echo "1g. a failed suspend drops the hibernation markers (the state file stays)"
printf 'ok\n' > /run/test/ra-mode
: > /run/test/ra-cmds
rm -f /data/banana/resume /run/banana/load-pending /run/banana/resume-now
setsid sleep 600 & F=$!; sleep 0.1
echo $F > /run/banana/game.pid
echo '/data/roms/gba/Test Game.zip' > /run/banana/game.path
start "suspend_mode = savestate"
rm -f /sys/power/state; mkdir /sys/power/state		# every write there fails
power
sleep 2.5
[ ! -e /data/banana/resume ] && ok "the resume request was dropped" || bad "a resume marker survived a failed suspend"
[ ! -e /run/banana/load-pending ] && [ ! -e /run/banana/resume-now ] && ok "no wake markers were left" || bad "wake markers left behind"
[ -f '/data/states/Test Game.state' ] && ok "the state file stayed on the card" || bad "the state file was deleted"
grep -q 'suspend failed, the resume request is dropped' /data/banana/logs/sleep.log && ok "the drop was logged" || bad "drop not logged"
stop
rmdir /sys/power/state; mkfifo /sys/power/state

echo "2. the battery guard: an RTC wake with the battery nearly empty shuts down cleanly"
echo $GAME > /run/banana/game.pid
echo /data/roms/gba/Test.gba > /run/banana/game.path
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
power
asleep && ok "suspended with the RTC alarm set ($(cat /sys/class/rtc/rtc0/wakealarm))" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake      # the alarm fired (the RTC clears it): the RTC woke it
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "shut down cleanly (init asked to power off)" || bad "no shutdown at 3%"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
grep -q 'battery check: 3%' /data/banana/logs/sleep.log && ok "logged the battery check" || bad "battery check not logged"
grep -qx /data/roms/gba/Test.gba /data/banana/resume && ok "the interrupted game is remembered" || bad "no resume marker"
stop
POWEROFF=0

echo "2b. a shutdown from the menu leaves no resume marker"
rm -f /run/banana/game.pid /run/banana/game.path
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
power
asleep && ok "suspended from the menu" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ ! -e /data/banana/resume ] && ok "nothing to resume after a menu shutdown" || bad "a menu shutdown left a resume marker"
stop
POWEROFF=0
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "3. an RTC wake with enough battery goes back to sleep"
echo 60 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002"
power
asleep && ok "suspended" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake
if asleep; then ok "back to sleep after the battery check"; else bad "awake after the battery check"; fi
[ "$(gstate)" = T ] && ok "the game stayed stopped" || bad "the game ran during the check"
wake; sleep 0.2
grep -q 'battery checked' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop

echo "4. a suspend that fails: the safer ways, then the panel dark"
rm -f /sys/power/state; : > /sys/power/state; chmod 000 /sys/power/state
# root can write a mode-000 file; a directory can't be written at all
rm -f /sys/power/state; mkdir /sys/power/state
start
power
sleep 1.5
grep -q 'trying the safer way' /data/banana/logs/sleep.log && ok "fell back after the failure" || bad "no fallback"
[ "$(cat /sys/power/mem_sleep)" = s2idle ] && ok "s2idle tried" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panel dark after the failed suspend" || bad "backlight bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
stop
rmdir /sys/power/state; mkfifo /sys/power/state

echo "5. idle in the menu: the panel dims, and comes back at the first press"
rm -f /run/banana/game.pid
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
start "dim_minutes = 0.02" "dim_percent = 30"      # 1.2 s
sleep 2.5
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 60 ] && ok "dimmed to 30% after the idle time" || bad "not dimmed: brightness $(cat /sys/class/backlight/backlight1/brightness)"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3         # BTN_SOUTH: any button
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "back to 200 at the press" || bad "still dim: $(cat /sys/class/backlight/backlight1/brightness)"
echo $GAME > /run/banana/game.pid; sleep 2
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "never dimmed while a game runs" || bad "dimmed during a game"
rm -f /run/banana/game.pid
stop

echo "6. charging: the idle timer does not suspend while it charges"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
start "autosleep_minutes = 0.02"
sleep 2
asleep 2>/dev/null && bad "it suspended while charging" || ok "no idle suspend while charging"
stop
rm -rf /sys/class/power_supply/charger

echo "7. no game: the RTC alarm is set for the battery check, and the wake tells banana-ui to draw again"
rm -f /run/banana/game.pid
rm -f /run/test/ui-signals
start "sleep_check_hours = 4"
power
asleep && ok "suspended" || bad "no suspend"
[ -n "$(cat /sys/class/rtc/rtc0/wakealarm)" ] && ok "an RTC alarm for the battery check set ($(cat /sys/class/rtc/rtc0/wakealarm))" || bad "no RTC alarm set"
wake
sleep 0.4
grep -q SIGUSR1 /run/test/ui-signals 2>/dev/null && ok "banana-ui was told to draw again (SIGUSR1)" || bad "banana-ui was not signalled after the wake"
grep -q 'menu: released the sound device in' /data/banana/logs/sleep.log && ok "waited for the menu to hand the device over" || bad "no handover wait logged"
grep -q 'sound device: closed (test)' /data/banana/logs/sleep-state && ok "the note recorded the closed device" || bad "the note did not record the handover"
grep -q 'awake' /data/banana/logs/sleep.log && ok "the wake was logged" || bad "no wake logged"
stop

echo "8. the vibrate trigger: a file under /run/banana pulses it (no FF input here, so a no-op, but it must be consumed)"
rm -f /run/banana/game.pid
start
: > /run/banana/vibrate
sleep 0.4
[ ! -e /run/banana/vibrate ] && ok "the trigger was consumed" || bad "the vibrate trigger was not consumed"
power
asleep && ok "still sleeps after a pulse" || bad "no suspend after a pulse"; wake
stop

echo "9. sleep_skip: a skipped SIGSTOP leaves the game running through the sleep (the debug bisect)"
kill -CONT $GAME 2>/dev/null || true
echo $GAME > /run/banana/game.pid
start "sleep_check_hours = 0" "sleep_skip = stop"
power
asleep && ok "suspended with the stop skipped" || bad "no suspend"
[ "$(gstate)" != T ] && ok "the game kept running (stop skipped)" || bad "the game was stopped anyway"
wake; sleep 0.2
grep -q "skipping 'stop'" /data/banana/logs/sleep.log && ok "logged the skipped step" || bad "the skip was not logged"
stop

echo "10. no RTC wakealarm (the RPC's ADSP-owned alarm): a low discharging battery refuses to sleep"
rm -f /sys/class/rtc/rtc0/wakealarm
echo Discharging > /sys/class/power_supply/battery/status
echo 10 > /sys/class/power_supply/battery/capacity
rm -f /run/banana/game.pid
start "sleep_floor_percent = 15"
power
sleep 0.5
asleep 2>/dev/null && bad "it slept a low battery with no wake source" || ok "refused to sleep the low battery"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "the panel went dark instead" || bad "panel not dark ($(cat /sys/class/backlight/backlight0/bl_power))"
grep -q 'no RTC wake alarm' /data/banana/logs/sleep.log && ok "logged the refusal" || bad "refusal not logged"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3      # BTN_SOUTH: the SoC never slept, so a press lights it again
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "a press brings the panel back" || bad "panel stayed dark"
stop

kill $UI $GAME 2>/dev/null || true
echo "sleep test: $pass passed, $fail failed"
[ $fail = 0 ]
