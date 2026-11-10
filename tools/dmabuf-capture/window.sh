#!/bin/sh
# window.sh: the exclusive-display window for the Phase 2 dmabuf capture, on the handheld.
#
#   window.sh --name sdl2 [--timeout 120] -- /data/dmabuf-capture --drm --seconds 20 --client /data/sdl2-gl-client 12
#
# It stops the menu (the capture server needs DRM master for wl_drm authentication), sets autosleep/dim off and
# restores them transactionally, runs the command in its own session, and always restores the launcher: on normal
# exit, on failure, on SIGTERM/HUP. A reboot is the last resort (the bind mount is per-boot).
#
# This is a test harness for docs/PLAN_DRM_COMPOSITOR.md phase 2; it does not touch cage or any launcher.

set -u
NAME=dmabuf
TIMEOUT=120
FOREGROUND=0
while [ $# -gt 0 ]; do
	case $1 in
	--name) NAME=$2; shift 2 ;;
	--timeout) TIMEOUT=$2; shift 2 ;;
	--foreground) FOREGROUND=1; shift ;;
	--) shift; break ;;
	*) break ;;
	esac
done
[ $# -gt 0 ] || { echo "usage: $0 --name NAME [--timeout SEC] [--foreground] -- CMD..." >&2; exit 1; }

# detach by default: an SSH drop must not kill the restore path (the probe window does the same)
if [ "${DMABUF_DETACHED:-0}" != 1 ] && [ "$FOREGROUND" != 1 ]; then
	mkdir -p /data/banana/logs 2>/dev/null
	ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%dT%H%M%S)
	log="/data/banana/logs/dmabuf-window-$ts.log"
	set -- --name "$NAME" --timeout "$TIMEOUT" -- "$@"
	if command -v setsid >/dev/null 2>&1; then
		DMABUF_DETACHED=1 setsid "$0" "$@" </dev/null >>"$log" 2>&1 &
	else
		DMABUF_DETACHED=1 nohup "$0" "$@" </dev/null >>"$log" 2>&1 &
	fi
	echo "$log"
	exit 0
fi

TARGET=/usr/bin/banana-ui-session
STANDIN=/tmp/banana-dmabuf-standin
SPIDFILE=/tmp/banana-dmabuf-standin.pid
CONF=/data/banana/bananad.conf
LOGDIR=/data/banana/logs
RUNDIR=/run/banana/dmabuf
CMD_PIDFILE=/tmp/banana-dmabuf-cmd.pid
BACKUP=
WROTE_SUM=
HAD_CONF=0
STANDIN_PID=
CPID=
BOUND=0
STOPPED=0
RESTORED=0
REPORT=

say() { echo "dmabuf-window: $*"; }
fail() { say "$*"; exit 1; }

stop_cmd() {
	[ -n "${CPID:-}" ] && kill -TERM "-$CPID" 2>/dev/null
	i=0
	while [ $i -lt 30 ] && [ -n "${CPID:-}" ] && kill -0 "$CPID" 2>/dev/null; do sleep 0.1; i=$((i+1)); done
	[ -n "${CPID:-}" ] && kill -KILL "-$CPID" 2>/dev/null
	[ -n "${CPID:-}" ] && wait "$CPID" 2>/dev/null
	CPID=
	rm -f "$CMD_PIDFILE"
}

restore() {
	[ "$RESTORED" = 1 ] && return 0
	RESTORED=1
	stop_cmd
	[ "$BOUND" = 1 ] && say "restoring the launcher"
	if [ "$BOUND" = 1 ]; then
		umount "$TARGET" 2>/dev/null || umount -l "$TARGET" 2>/dev/null
		[ -n "$STANDIN_PID" ] && kill -TERM "$STANDIN_PID" 2>/dev/null
		[ -n "$STANDIN_PID" ] || for p in $(pgrep -f "$TARGET" 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
		rm -f "$SPIDFILE"
	fi
	if [ -f "$CONF" ] && [ -n "$WROTE_SUM" ]; then
		now=$(sha256sum "$CONF" 2>/dev/null | cut -d' ' -f1)
		if [ "$now" = "$WROTE_SUM" ]; then
			if [ "$HAD_CONF" = 1 ] && [ -f "$BACKUP" ]; then cp -f "$BACKUP" "$CONF.tmp" && mv -f "$CONF.tmp" "$CONF"
			else rm -f "$CONF"; fi
			say "bananad.conf restored"
		else
			sed -i '/# dmabuf-window begin/,/# dmabuf-window end/d' "$CONF" 2>/dev/null
			say "bananad.conf changed during the window; kept it, removed only our block"
		fi
	fi
	for p in $(pidof bananad 2>/dev/null); do kill -HUP "$p" 2>/dev/null; done
	sync
	if [ "$STOPPED" = 1 ]; then
		i=0
		while [ $i -lt 40 ] && ! pidof banana-ui >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); done
		if pidof banana-ui >/dev/null 2>&1; then say "menu back (pid $(pidof banana-ui))"
		else say "WARNING: the menu did not come back; reboot (hold Power) to restore it"; fi
	fi
}
on_signal() { restore; exit 130; }
trap restore EXIT
trap on_signal INT TERM HUP

[ "$(id -u)" = 0 ] || fail "run as root"
[ -e "$TARGET" ] || fail "$TARGET is missing"
[ -e /run/banana/game.pid ] && fail "a game is running; quit to the menu first"
[ "$(cat /run/banana/usb-msc.mode 2>/dev/null)" = rw ] && fail "the card is in USB transfer mode"
mkdir -p "$LOGDIR" "$RUNDIR" || fail "cannot create $LOGDIR or $RUNDIR"
chmod 700 "$RUNDIR"
cap=$(cat /sys/class/power_supply/*/capacity 2>/dev/null | sort -n | head -n 1)
charging=0
for f in /sys/class/power_supply/*/online; do [ "$(cat "$f" 2>/dev/null)" = 1 ] && charging=1; done
[ "$charging" = 0 ] && [ -n "${cap:-}" ] && [ "$cap" -lt 40 ] && fail "battery $cap% and not charging"

ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%dT%H%M%S)
REPORT="$LOGDIR/dmabuf-$NAME-$ts.txt"
say "report: $REPORT"

# config: no idle sleep/dim while the menu is down, restored only if untouched
if [ -f "$CONF" ]; then BACKUP=/tmp/banana-dmabuf-bananad.$$; cp -f "$CONF" "$BACKUP" || fail "cannot back up $CONF"; HAD_CONF=1; fi
{
	echo
	echo "# dmabuf-window begin"
	echo "autosleep_minutes = 0"
	echo "dim_minutes = 0"
	echo "# dmabuf-window end"
} >> "$CONF" || fail "cannot write $CONF"
WROTE_SUM=$(sha256sum "$CONF" | cut -d' ' -f1)
for p in $(pidof bananad 2>/dev/null); do kill -HUP "$p" 2>/dev/null; done
sleep 0.5

# stand-in: init execs the bind target, so the script publishes its own pid
cat > "$STANDIN" <<EOF
#!/bin/sh
echo \$\$ > "$SPIDFILE"
while :; do sleep 3600; done
EOF
chmod +x "$STANDIN" || fail "cannot make $STANDIN"
rm -f "$SPIDFILE"
mount --bind "$STANDIN" "$TARGET" || fail "bind mount failed; menu untouched"
BOUND=1
grep -q " $TARGET " /proc/mounts || fail "bind mount not visible; aborting"
for p in $(pidof banana-ui 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
STOPPED=1
i=0
while [ $i -lt 40 ] && pidof banana-ui >/dev/null 2>&1; do sleep 0.2; i=$((i+1)); done
pidof banana-ui >/dev/null 2>&1 && fail "the menu did not stop; nothing was tested"
i=0
while [ $i -lt 50 ] && [ ! -s "$SPIDFILE" ]; do sleep 0.1; i=$((i+1)); done
STANDIN_PID=$(cat "$SPIDFILE" 2>/dev/null)
[ -n "$STANDIN_PID" ] && kill -0 "$STANDIN_PID" 2>/dev/null || fail "init did not start the stand-in"
say "window open (stand-in pid $STANDIN_PID); running $*"

# the command in its own session, so the server and the client it spawns stop together
export XDG_RUNTIME_DIR="$RUNDIR"
export DMABUF_CMD_PIDFILE="$CMD_PIDFILE"
rm -f "$CMD_PIDFILE"
setsid sh -c 'echo $$ > "$DMABUF_CMD_PIDFILE"; exec "$@"' sh "$@" > "$REPORT" 2>&1 &
CPID=$!
i=0
while [ $i -lt 20 ] && [ ! -s "$CMD_PIDFILE" ]; do sleep 0.1; i=$((i+1)); done
[ -s "$CMD_PIDFILE" ] && CPID=$(cat "$CMD_PIDFILE")
( sleep "$TIMEOUT"; [ -n "$CPID" ] && kill -TERM "-$CPID" 2>/dev/null; sleep 5; [ -n "$CPID" ] && kill -KILL "-$CPID" 2>/dev/null ) & WD=$!
wait "$CPID" 2>/dev/null
rc=$?
kill "$WD" 2>/dev/null
CPID=
rm -f "$CMD_PIDFILE"
echo "command exit: $rc" >> "$REPORT"
say "command finished (exit $rc)"
restore
exit $rc
