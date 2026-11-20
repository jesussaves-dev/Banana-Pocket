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
/* and it turns the panel off first, answering in panel-state */
static void panel(const char *s) { FILE *f = fopen("/run/banana/panel-state", "w"); if (f) { fputs(s, f); fclose(f); } }
static void onhup(int s) { panel("off"); usleep(50000); state("closed (test)\n"); }
int main(void) { signal(SIGUSR1, on); signal(SIGHUP, onhup); state("open (test)\n"); for (;;) pause(); }
EOF
    # an LD_PRELOAD shim for bananad: a real sysfs answers EBUSY to a stale wakeup_count, which a tmpfs cannot, so the
    # shim fails the open of it that many times (the number in /run/test/wk-busy); and reboot() only records the call,
    # so a test can watch the shutdown watchdog fire without anything powering off
    cat > "$W/shim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
static int busy(const char *path)
{
	if (strcmp(path, "/sys/power/wakeup_count")) return 0;
	FILE *f = fopen("/run/test/wk-busy", "r");
	int n = 0;
	if (!f) return 0;
	if (fscanf(f, "%d", &n) != 1) n = 0;
	fclose(f);
	if (n <= 0) return 0;
	f = fopen("/run/test/wk-busy", "w");
	if (f) { fprintf(f, "%d\n", n - 1); fclose(f); }
	return 1;
}
#define OPEN(NAME) int NAME(const char *p, int fl, ...) { \
	static int (*real)(const char *, int, ...); \
	if (!real) real = dlsym(RTLD_NEXT, #NAME); \
	if ((fl & O_ACCMODE) != O_RDONLY && busy(p)) { errno = EBUSY; return -1; } \
	va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap); \
	return real(p, fl, m); }
OPEN(open)
OPEN(open64)
int reboot(int cmd)
{
	FILE *f = fopen("/run/test/reboot", "a");
	if (f) { fprintf(f, "reboot %#x\n", (unsigned)cmd); fclose(f); }
	return 0;
}
EOF
    cc -shared -fPIC -O2 -o "$W/shim.so" "$W/shim.c" -ldl
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
# the real one marks the Speaker sink ready at the end of "up" (bananad's wake waits for it) and clears it on "down"
cat >> /usr/libexec/banana/sound-server <<'EOF'
case $1 in up) : > /run/banana/sound-ready ;; down) rm -f /run/banana/sound-ready ;; esac
EOF
: > /run/banana/sound-ready
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

# The game's stdin channel: after a wake bananad reinitialises the game's audio driver through it (the stream died
# with the sound server, which is stopped for the sleep).
mkfifo /run/banana/retroarch.cmd 2>/dev/null || true
: > /run/test/ra-cmds
# SAVE_STATE_AUTO (patch 0015) is answered the way RetroArch does, in /run/banana/autosave.
( exec 4<>/run/banana/retroarch.cmd; while IFS= read -r cmd <&4; do printf '%s\n' "$cmd" >> /run/test/ra-cmds
    [ "$cmd" = SAVE_STATE_AUTO ] && echo ok > /run/banana/autosave; done ) &

# press and release power: bananad sleeps on the release
power() { ev $EV_KEY $KEY_POWER 1; sleep 0.1; ev $EV_KEY $KEY_POWER 0; }
# is bananad blocked writing "mem" (asleep)? wait up to 3 s
# (blocked opening the FIFO: its wait channel is wait_for_partner)
asleep() { for i in $(seq 30); do [ "$(cat /proc/$BD/wchan 2>/dev/null)" = wait_for_partner ] && return 0; sleep 0.1; done; return 1; }
# read "mem": the write returns, it is awake. A real power-key wake carries a KEY_POWER press (the spurious-wake
# guard re-sleeps a wake with no key and no cable), so wake() queues one before releasing the FIFO; an RTC wake
# (the battery check's alarm) has no key. 0.4.54 added the guard after this test, which is why wake() has the press.
# (the wake log now comes after the sound server's wait, so wait for a new "awake" line rather than a fixed pause)
wake() { ev $EV_KEY $KEY_POWER 1; head -c 3 /sys/power/state > /dev/null
    n=$(grep -ac 'awake (' /data/banana/logs/sleep.log 2>/dev/null || true); n=${n:-0}
    i=0; while [ $i -lt 30 ]; do m=$(grep -ac 'awake (' /data/banana/logs/sleep.log 2>/dev/null || true); m=${m:-0}
        [ "$m" -gt "$n" ] && break; sleep 0.1; i=$((i+1)); done; }
rtc_wake() { head -c 3 /sys/power/state > /dev/null; sleep 0.4; }
gstate() { awk '{print $3}' /proc/$GAME/stat; }
start() {   # start [bananad.conf lines]
    pkill -x bananad 2>/dev/null || true; sleep 0.2         # one daemon at a time (a leftover would read the same keys)
    echo "s2idle [deep]" > /sys/power/mem_sleep     # the kernel's file always lists both
    printf '%s\n' "autosleep_minutes = 0" "sleep_check_hours = 0" "$@" > /data/banana/bananad.conf
    LD_PRELOAD="$W/shim.so" "$W/bananad" & BD=$!
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
[ "$(gstate)" = T ] && ok "game stopped while asleep" || bad "game not stopped ($(gstate))"
[ "$(cat /sys/power/pm_async)" = 1 ] && ok "devices resume in parallel (pm_async 1)" || bad "pm_async $(cat /sys/power/pm_async)"
grep -q "\[deep\]" /sys/power/mem_sleep && ok "deep sleep chosen" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
grep -q 'going to sleep (power button)' /data/banana/logs/sleep-state && ok "the pre-sleep note was written" || bad "no sleep-state note"
wake
[ "$(gstate)" != T ] && ok "game running again after the wake" || bad "game still stopped"
for i in $(seq 30); do grep -qx AUDIO_REINIT /run/test/ra-cmds 2>/dev/null && break; sleep 0.1; done
grep -qx AUDIO_REINIT /run/test/ra-cmds && ok "the game's audio driver was reinitialised after the wake" || bad "no AUDIO_REINIT after the wake"
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

echo "1j. wake_audio_fast keeps the sound server up through the sleep"
rm -f /run/test/hook-calls
start "wake_audio_fast = on"
: > /run/test/ra-cmds
power
asleep && ok "suspended with the server kept" || bad "no suspend with the server kept"
grep -qx 'sound-server down' /run/test/hook-calls 2>/dev/null && bad "the sound server was stopped" || ok "the sound server was not stopped"
wake; sleep 0.4
grep -qx AUDIO_REINIT /run/test/ra-cmds 2>/dev/null && bad "the game's audio was reinitialised anyway: $(grep -a 'audio:' /data/banana/logs/sleep.log | tail -n 2 | tr '\n' '|')" || ok "no reinit: the server (and the game's stream) never went away"
stop

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

echo "2c. a pending wake event (EBUSY on wakeup_count) aborts the suspend and retries"
echo 7 > /sys/power/wakeup_count
: > /data/banana/logs/sleep.log
echo 2 > /run/test/wk-busy
start
power
asleep && ok "suspended after the pending wake event cleared" || bad "no suspend after two EBUSY"
[ "$(grep -c 'suspend aborted: a wake event was pending' /data/banana/logs/sleep.log)" = 2 ] && ok "two aborts logged" || bad "aborts logged: $(grep -c 'suspend aborted' /data/banana/logs/sleep.log)"
wake
stop
: > /data/banana/logs/sleep.log
echo 100 > /run/test/wk-busy
start
power
asleep && ok "still suspends when the event never clears" || bad "no suspend under a permanent EBUSY"
[ "$(grep -c 'suspend aborted: a wake event was pending' /data/banana/logs/sleep.log)" = 5 ] && ok "gave up after 5 aborts" || bad "aborts logged: $(grep -c 'suspend aborted' /data/banana/logs/sleep.log)"
wake
stop
rm -f /run/test/wk-busy /sys/power/wakeup_count

echo "2d. a shutdown that init never completes is forced after 8 s, panel dark and muted first"
rm -f /run/test/reboot
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
power
asleep && ok "suspended" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "init was asked to power off" || bad "no shutdown at 3%"
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 0 ] && ok "panel dark during the shutdown" || bad "backlight $(cat /sys/class/backlight/backlight1/brightness)"
sleep 3
[ ! -e /run/test/reboot ] && ok "no forced power-off while init has time" || bad "forced power-off after only 3 s"
for i in $(seq 100); do [ -e /run/test/reboot ] && break; sleep 0.1; done
grep -q 'reboot 0x4321fedc' /run/test/reboot 2>/dev/null && ok "forced RB_POWER_OFF once init hung" || bad "no forced power-off: $(cat /run/test/reboot 2>/dev/null)"
stop
POWEROFF=0
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
echo "2e. low battery and no RTC alarm: dark but awake, parked at the minimum clocks until the next press"
mount -t tmpfs none /sys/devices/system/cpu
for p in 0:300000:1800000 6:500000:2400000; do
    n=${p%%:*}; r=${p#*:}; lo=${r%%:*}; hi=${r#*:}
    d=/sys/devices/system/cpu/cpufreq/policy$n; mkdir -p $d
    echo $lo > $d/cpuinfo_min_freq; echo $lo > $d/scaling_min_freq; echo $hi > $d/scaling_max_freq; echo schedutil > $d/scaling_governor
done
for i in 1 2 3 4 5 6 7; do mkdir -p /sys/devices/system/cpu/cpu$i; echo 1 > /sys/devices/system/cpu/cpu$i/online; done
mkdir -p /sys/class/devfreq/3d00000.gpu; echo simple_ondemand > /sys/class/devfreq/3d00000.gpu/governor
rm -f /run/banana/game.pid /sys/class/rtc/rtc0/wakealarm
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_floor_percent = 5"
power
sleep 1
grep -q 'dark, but watching the battery' /data/banana/logs/sleep.log && ok "refused to sleep, panel dark" || bad "no low-battery refusal"
C=/sys/devices/system/cpu
[ "$(cat $C/cpufreq/policy0/scaling_max_freq)" = 300000 ] && [ "$(cat $C/cpufreq/policy6/scaling_max_freq)" = 500000 ] && [ "$(cat $C/cpufreq/policy6/scaling_min_freq)" = 500000 ] && ok "both policies at their minimum" || bad "policies: $(cat $C/cpufreq/policy0/scaling_max_freq) $(cat $C/cpufreq/policy6/scaling_max_freq)"
[ "$(cat $C/cpu1/online)$(cat $C/cpu7/online)" = 00 ] && ok "cpu1-7 offline" || bad "cpus still online"
[ "$(cat /sys/class/devfreq/3d00000.gpu/governor)" = powersave ] && ok "GPU at its lowest step" || bad "gpu governor $(cat /sys/class/devfreq/3d00000.gpu/governor)"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.5
[ "$(cat $C/cpufreq/policy0/scaling_max_freq)" = 1800000 ] && [ "$(cat $C/cpufreq/policy6/scaling_max_freq)" = 2400000 ] && [ "$(cat $C/cpufreq/policy6/scaling_min_freq)" = 500000 ] && ok "clocks restored at the press" || bad "clocks: $(cat $C/cpufreq/policy0/scaling_max_freq) $(cat $C/cpufreq/policy6/scaling_max_freq)"
[ "$(cat $C/cpu1/online)$(cat $C/cpu7/online)" = 11 ] && ok "cores back online" || bad "cores still offline"
[ "$(cat /sys/class/devfreq/3d00000.gpu/governor)" = simple_ondemand ] && [ "$(cat $C/cpufreq/policy0/scaling_governor)" = schedutil ] && ok "governors restored" || bad "governors not restored"
stop
umount /sys/devices/system/cpu
rm -rf /sys/class/devfreq
: > /sys/class/rtc/rtc0/wakealarm
echo 50 > /sys/class/power_supply/battery/capacity

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

echo "11. Quick resume on: the game writes its .auto state before it is stopped; a frozen game is not asked"
mkdir -p /data/retroarch
echo 'savestate_auto_save = "true"' > /data/retroarch/retroarch.cfg
echo Charging > /sys/class/power_supply/battery/status
echo 50 > /sys/class/power_supply/battery/capacity
kill -CONT $GAME 2>/dev/null || true
echo $GAME > /run/banana/game.pid
: > /run/test/ra-cmds
start
power
asleep && ok "suspended" || bad "no suspend"
grep -q '^SAVE_STATE_AUTO$' /run/test/ra-cmds && ok "the game was asked for its state" || bad "SAVE_STATE_AUTO not sent"
grep -q 'state: saved before the sleep in .* ms (ok)' /data/banana/logs/sleep.log && ok "logged the answer" || bad "no saved-state line"
wake; sleep 0.2
grep -q 'audio: the server is not ready' /data/banana/logs/sleep.log && bad "the wake waited out sound-ready" || ok "the wake found the server ready"
grep -q '^AUDIO_REINIT$' /run/test/ra-cmds && ok "the game's audio was reinitialised" || bad "no AUDIO_REINIT"
stop
: > /run/test/ra-cmds
kill -STOP $GAME
n=$(grep -c 'state: saved' /data/banana/logs/sleep.log)
start
power
asleep && ok "suspended with the game frozen" || bad "no suspend"
grep -q '^SAVE_STATE_AUTO$' /run/test/ra-cmds && bad "a frozen game was asked" || ok "a frozen game was not asked"
[ "$(grep -c 'state: saved' /data/banana/logs/sleep.log)" = "$n" ] && ok "no wait on a game that cannot answer" || bad "a saved-state line for a frozen game"
wake
stop
rm -f /data/retroarch/retroarch.cfg

kill $UI $GAME 2>/dev/null || true
echo "sleep test: $pass passed, $fail failed"
[ $fail = 0 ]
