#!/bin/sh
# drm-probe-window: the exclusive DRM-master window for tools/drm-plane-probe.
#
# The atomic matrix needs DRM master, and the menu (banana-ui) holds it. This script stops the menu for the length
# of the probe, then puts it back -- and does that from a detached process with a trap, so a dropped SSH session,
# a Ctrl-C, a probe failure or a timeout all restore the launcher.
#
#   drm-probe-window [--probe /data/drm-plane-probe] [--connector DSI-1] [--card /dev/dri/card0]
#                    [--commit <git hash>] [--foreground]
#
#   (no --foreground) it detaches with setsid, prints its log path, and returns; tail that log for the result.
#   --foreground         run in this shell (a console, not a flaky SSH session)
#
# What it does, in order:
#   1. checks: root, the probe, the launcher, no game, /data not handed to a computer, battery/charger
#   2. appends autosleep_minutes = 0 and dim_minutes = 0 to bananad.conf and HUPs bananad (a menu-less handheld
#      would otherwise idle-suspend in the middle of the probe), remembering the file's exact bytes
#   3. writes a uniquely named stand-in and bind-mounts it over /usr/bin/banana-ui-session, verifies the mount,
#      then stops the menu; init respawns the stand-in and its pid is recorded
#   4. starts the probe under `timeout` through a one-line supervisor that writes the probe's pid before exec,
#      so the real probe -- not the timeout wrapper -- is the process this script stops; the report carries
#      board, SoC/panel (from the probe), kernel, driver, UTC time, probe commit and sha256
#   5. restores: stops the probe and waits for it to exit *first* (no DRM master held), unmounts (so init
#      respawns the real launcher), kills the stand-in, restores bananad.conf only if nobody else changed it,
#      and verifies banana-ui is back
#
# Ultimate recovery: if this process is killed with SIGKILL (or the handheld loses power) the stand-in can stay in
# the respawn slot; a reboot always restores the normal system (the bind mount is per-boot).
#
# It does not touch cage, the launchers, the kernel or the image; the known-good fallback is untouched.

set -u

PROBE=/data/drm-plane-probe
CONNECTOR=
CARD=
COMMIT=unknown
FOREGROUND=0
TARGET=/usr/bin/banana-ui-session
STANDIN=/tmp/banana-drm-probe-standin
CONF=/data/banana/bananad.conf
LOGDIR=/data/banana/logs
TIMEOUT=180

while [ $# -gt 0 ]; do
	case $1 in
	--probe) PROBE=$2; shift 2 ;;
	--connector) CONNECTOR=$2; shift 2 ;;
	--card) CARD=$2; shift 2 ;;
	--commit) COMMIT=$2; shift 2 ;;
	--foreground) FOREGROUND=1; shift ;;
	*) echo "usage: $0 [--probe PATH] [--connector NAME] [--card PATH] [--commit HASH] [--foreground]" >&2; exit 1 ;;
	esac
done

# ---- detach --------------------------------------------------------------------------------------------------------

if [ "${DRM_PROBE_DETACHED:-0}" != 1 ] && [ "$FOREGROUND" != 1 ]; then
	mkdir -p "$LOGDIR" 2>/dev/null
	ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%dT%H%M%S)
	log="$LOGDIR/drm-probe-window-$ts.log"
	# the options were consumed by the parser above: hand the child a canonical list, not "$@"
	set -- --probe "$PROBE" --commit "$COMMIT"
	[ -n "$CONNECTOR" ] && set -- "$@" --connector "$CONNECTOR"
	[ -n "$CARD" ] && set -- "$@" --card "$CARD"
	if command -v setsid >/dev/null 2>&1; then
		DRM_PROBE_DETACHED=1 setsid "$0" "$@" </dev/null >>"$log" 2>&1 &
	else
		DRM_PROBE_DETACHED=1 nohup "$0" "$@" </dev/null >>"$log" 2>&1 &
	fi
	echo "$log"
	exit 0
fi

# From here on this is the detached process. Everything it starts is restored by the trap below.

REPORT=
BACKUP=
WROTE_SUM=
STANDIN_PID=
STANDIN_PIDFILE=/tmp/banana-drm-probe-standin.pid
TPID=
PROBE_PIDFILE=/tmp/banana-drm-probe.pid
RESTORED=0
HAD_CONF=0
BOUND=0
MENU_STOPPED=0

say() { echo "drm-probe-window: $*"; }

# BusyBox timeout (1.37, the image) execs the program in the timeout process itself, while GNU timeout forks it:
# `$!` is the probe on BusyBox but the timeout wrapper on a host. So the probe is started through a tiny shell that
# records its own pid and then execs the probe; that pid is exact on both. Stop it, wait for it to go, and only
# then let the menu take DRM master back.
stop_probe() {
	[ -n "${TPID:-}" ] || [ -s "$PROBE_PIDFILE" ] || return 0
	p=
	i=0
	while [ $i -lt 20 ] && [ ! -s "$PROBE_PIDFILE" ]; do sleep 0.1; i=$((i+1)); done	# the exec may just be starting
	[ -s "$PROBE_PIDFILE" ] && p=$(cat "$PROBE_PIDFILE" 2>/dev/null)
	if [ -n "$p" ]; then
		kill -TERM "$p" 2>/dev/null
		i=0
		while [ $i -lt 50 ] && kill -0 "$p" 2>/dev/null; do sleep 0.1; i=$((i+1)); done
		if kill -0 "$p" 2>/dev/null; then kill -KILL "$p" 2>/dev/null; fi
		i=0
		while [ $i -lt 20 ] && kill -0 "$p" 2>/dev/null; do sleep 0.1; i=$((i+1)); done
	fi
	if [ -n "${TPID:-}" ]; then
		kill -TERM "$TPID" 2>/dev/null
		wait "$TPID" 2>/dev/null
		TPID=
	fi
	rm -f "$PROBE_PIDFILE"
	if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
		say "WARNING: probe pid $p did not exit; DRM master may still be held"
	elif [ -n "$p" ]; then
		say "probe stopped (pid $p)"
	else
		say "probe stopped (pid unknown: no pidfile; the timeout process was stopped)"
	fi
}

restore() {
	[ "$RESTORED" = 1 ] && return 0
	RESTORED=1
	# stop the probe first: the menu cannot take DRM master while it is still alive
	stop_probe
	[ "$BOUND" = 1 ] && say "restoring the launcher"
	# 1. unmount first: while the bind is in place, init would respawn the stand-in again
	if [ "$BOUND" = 1 ]; then
		umount "$TARGET" 2>/dev/null || umount -l "$TARGET" 2>/dev/null
		# 2. stop the stand-in. Its argv is the bind target (/usr/bin/banana-ui-session), not the stand-in
		# path, so the pid it publishes is the only reliable handle. If an older run died before writing it,
		# fall back to the target path (nothing else runs it while the menu is stopped).
		if [ -n "$STANDIN_PID" ]; then
			kill -TERM "$STANDIN_PID" 2>/dev/null
		else
			for p in $(pgrep -f "$TARGET" 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
		fi
		rm -f "$STANDIN_PIDFILE"
	fi
	# 3. bananad.conf: restore only if the file is still exactly what this script wrote; otherwise remove just
	#    this script's block, so a change made by someone else during the window is never overwritten
	if [ -f "$CONF" ] && [ -n "$WROTE_SUM" ]; then
		now=$(sha256sum "$CONF" 2>/dev/null | cut -d' ' -f1)
		if [ "$now" = "$WROTE_SUM" ]; then
			if [ "$HAD_CONF" = 1 ] && [ -f "$BACKUP" ]; then
				cp -f "$BACKUP" "$CONF.tmp" && mv -f "$CONF.tmp" "$CONF"
			else
				rm -f "$CONF"
			fi
			say "bananad.conf restored"
		else
			sed -i '/# drm-probe-window begin/,/# drm-probe-window end/d' "$CONF" 2>/dev/null
			say "bananad.conf changed during the window: kept it, removed only this script's block (backup: ${BACKUP:-none})"
		fi
	fi
	for p in $(pidof bananad 2>/dev/null); do kill -HUP "$p" 2>/dev/null; done
	sync
	# 4. the menu must be back; init respawns it now that the bind is gone
	if [ "$MENU_STOPPED" = 1 ]; then
		i=0
		while [ $i -lt 40 ] && ! pidof banana-ui >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); done
		if pidof banana-ui >/dev/null 2>&1; then
			say "menu back (pid $(pidof banana-ui))"
		else
			say "WARNING: the menu did not come back. Reboot (hold Power) to restore it; the bind mount is per-boot."
		fi
	fi
	sync
}

on_signal() { restore; exit 130; }
trap restore EXIT
trap on_signal INT TERM HUP

# ---- preconditions -------------------------------------------------------------------------------------------------

fail() { say "$*"; exit 1; }

[ "$(id -u)" = 0 ] || fail "run as root"
[ -x "$PROBE" ] || fail "$PROBE is missing or not executable (copy it to /data first)"
[ -e "$TARGET" ] || fail "$TARGET is missing"
if [ -e /run/banana/game.pid ]; then fail "a game is running (or its marker is stale); quit to the menu first"; fi
if [ "$(cat /run/banana/usb-msc.mode 2>/dev/null)" = rw ]; then fail "the card is in USB transfer mode; eject it first"; fi
mkdir -p "$LOGDIR" || fail "cannot make $LOGDIR"
command -v timeout >/dev/null 2>&1 || fail "no timeout command"

# not on a nearly empty battery: a shutdown mid-window is a frozen screen, not a test
cap=$(cat /sys/class/power_supply/*/capacity 2>/dev/null | sort -n | head -n 1)
charging=0
for f in /sys/class/power_supply/*/online; do
	[ "$(cat "$f" 2>/dev/null)" = 1 ] && charging=1
done
if [ "$charging" = 0 ] && [ -n "$cap" ] && [ "$cap" -lt 40 ]; then
	fail "battery $cap% and not charging; put it on the charger (a window that shuts down leaves a frozen screen)"
fi

# ---- the probe report header ---------------------------------------------------------------------------------------

ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%dT%H%M%S)
board=$(cat /etc/banana/board 2>/dev/null || echo unknown)
REPORT="$LOGDIR/drm-probe-$board-$ts.txt"
{
	echo "banana DRM hardware report"
	echo "captured (UTC): $(date -u '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date)"
	echo "board: $board"
	echo "version: $(cat /etc/banana/version 2>/dev/null || echo unknown)"
	echo "kernel: $(uname -a)"
	echo "probe commit: $COMMIT"
	echo "probe sha256: $(sha256sum "$PROBE" | cut -d' ' -f1)"
	echo "probe args: --take-master${CARD:+ --card $CARD}${CONNECTOR:+ --connector $CONNECTOR}"
	echo
} > "$REPORT" || fail "cannot write $REPORT"

# ---- the window ----------------------------------------------------------------------------------------------------

# 1. no idle sleep or dim while the menu is down. Keep the exact original for a transactional restore.
if [ -f "$CONF" ]; then
	BACKUP="/tmp/banana-bananad.conf.$$"
	cp -f "$CONF" "$BACKUP" || fail "cannot back up $CONF"
	HAD_CONF=1
fi
{
	echo
	echo "# drm-probe-window begin"
	echo "autosleep_minutes = 0"
	echo "dim_minutes = 0"
	echo "# drm-probe-window end"
} >> "$CONF" || fail "cannot write $CONF"
WROTE_SUM=$(sha256sum "$CONF" | cut -d' ' -f1)
for p in $(pidof bananad 2>/dev/null); do kill -HUP "$p" 2>/dev/null; done
sleep 0.5

# 2. the stand-in: init execs the bind target, so its argv is /usr/bin/banana-ui-session, not this file; it
# publishes its own pid before looping. A unique path keeps it apart from anything else on the card.
cat > "$STANDIN" <<EOF
#!/bin/sh
# drm-probe-window: hold the launcher's respawn slot while the probe owns DRM
echo \$\$ > "$STANDIN_PIDFILE"
while :; do sleep 3600; done
EOF
chmod +x "$STANDIN" || fail "cannot make $STANDIN"
rm -f "$STANDIN_PIDFILE"

mount --bind "$STANDIN" "$TARGET" || fail "bind mount failed; the menu was not stopped"
BOUND=1
grep -q " $TARGET " /proc/mounts || fail "bind mount not visible; aborting before stopping the menu"

# 3. stop the menu; init starts the stand-in
for p in $(pidof banana-ui 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
i=0
while [ $i -lt 40 ] && pidof banana-ui >/dev/null 2>&1; do sleep 0.2; i=$((i+1)); done
MENU_STOPPED=1
pidof banana-ui >/dev/null 2>&1 && fail "the menu did not stop; nothing was tested"

i=0
while [ $i -lt 50 ] && [ ! -s "$STANDIN_PIDFILE" ]; do sleep 0.1; i=$((i+1)); done
[ -s "$STANDIN_PIDFILE" ] && STANDIN_PID=$(cat "$STANDIN_PIDFILE" 2>/dev/null)
[ -n "$STANDIN_PID" ] && kill -0 "$STANDIN_PID" 2>/dev/null || fail "init did not start the stand-in; aborting"
say "exclusive window: stand-in pid $STANDIN_PID, menu stopped"

# 4. the matrix
set -- --take-master
[ -n "$CARD" ] && set -- "$@" --card "$CARD"
[ -n "$CONNECTOR" ] && set -- "$@" --connector "$CONNECTOR"
say "running: timeout $TIMEOUT $PROBE $*"
rm -f "$PROBE_PIDFILE"
DRM_PROBE_PID="$PROBE_PIDFILE" DRM_PROBE_BIN="$PROBE" \
	timeout "$TIMEOUT" sh -c 'echo $$ > "$DRM_PROBE_PID"; exec "$DRM_PROBE_BIN" "$@"' sh "$@" \
	>> "$REPORT" 2>&1 &
TPID=$!
wait "$TPID"
rc=$?
TPID=
rm -f "$PROBE_PIDFILE"
echo
echo "probe exit: $rc (0 = decision, 2 = HOLD/incomplete; 143/137 = timeout or signal)" >> "$REPORT"
say "probe finished (exit $rc); report: $REPORT"

restore
[ "$rc" = 0 ] && exit 0
[ "$rc" = 1 ] && exit 1
exit 2
