#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
HELPER="$ROOT/system/rootfs/usr/libexec/banana/stop-native-session"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

${CC:-cc} ${CFLAGS:--O2} -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-I"$HERE/include" -o "$WORK/bananad" "$HERE/bananad.c" -lasound -lm
"$WORK/bananad" --selftest retro-lifecycle
${CC:-cc} ${CFLAGS:--O2} -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-I"$HERE/include" -o "$WORK/check-checkpoint" "$HERE/tests/check-checkpoint.c" -lasound -lm
"$WORK/check-checkpoint"
sh -n "$ROOT/system/rootfs/etc/init.d/rcK"
sh -n "$HELPER"

# rcK latches the shutdown before it signals anything. Run its real opening (up to the native-session stop) against a
# scratch run directory, with every process-affecting command replaced by a probe that records whether the latch
# existed at the moment it was called: banana-ui must never be able to reap a game without it.
RCK_RUN="$WORK/rcK-run"
RCK_DATA="$WORK/rcK-data"
RCK_LOG="$WORK/rcK-order.log"
mkdir -p "$RCK_DATA"
for game in 4242 none; do
	rm -rf "$RCK_RUN"
	: > "$RCK_LOG"
	mkdir -p "$RCK_RUN"
	[ "$game" = none ] || echo "$game" > "$RCK_RUN/game.pid"
	{
		cat <<EOF
probe() { if [ -e "$RCK_RUN/shutting-down" ]; then echo "\$1 latched" >> "$RCK_LOG"; else echo "\$1 EARLY" >> "$RCK_LOG"; fi; }
kill() { probe "kill \$*"; }
killall() { probe "killall \$*"; }
stop_native() { probe stop-native; }
EOF
		sed -n '1,/stop-native-session$/p' "$ROOT/system/rootfs/etc/init.d/rcK" |
			sed -e "s#/run/banana#$RCK_RUN#g" -e "s#/data/banana#$RCK_DATA#g" \
				-e 's#/usr/libexec/banana/stop-native-session#stop_native#'
	} > "$WORK/rcK-head.sh"
	sh "$WORK/rcK-head.sh"
	[ -e "$RCK_RUN/shutting-down" ] || { echo "rcK did not latch the shutdown" >&2; exit 1; }
	grep -q 'killall -TERM banana-ui retroarch latched' "$RCK_LOG" || { echo "rcK killall not observed under the latch" >&2; exit 1; }
	grep -q 'stop-native latched' "$RCK_LOG" || { echo "rcK stop-native not observed under the latch" >&2; exit 1; }
	if [ "$game" != none ]; then
		grep -q 'kill -TERM -4242 latched' "$RCK_LOG" || { echo "rcK game kill not observed under the latch" >&2; exit 1; }
	fi
	if grep -q EARLY "$RCK_LOG"; then
		echo "rcK signalled before latching the shutdown:" >&2
		cat "$RCK_LOG" >&2
		exit 1
	fi
done
echo "rcK latches the shutdown before it signals anything: ok"

# The latch is created under resume.lock. A writer inside the lock (it has already passed its latch check) finishes
# before the latch exists; a lock that stays held cannot stop the shutdown, and rcK's wait is bounded.
command -v flock >/dev/null || { echo "flock is required for the rcK latch-order test" >&2; exit 1; }
for hold in 0.1 1.5; do
	rm -rf "$RCK_RUN" "$WORK/holder.ready" "$WORK/holder.result"
	mkdir -p "$RCK_RUN"
	python3 - "$RCK_DATA/resume.lock" "$RCK_RUN/shutting-down" "$WORK/holder.ready" "$WORK/holder.result" "$hold" <<'PY' &
import fcntl, os, sys, time
lock, latch, ready, result, hold = sys.argv[1:5] + [float(sys.argv[5])]
fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
fcntl.flock(fd, fcntl.LOCK_EX)
open(ready, "w").close()
time.sleep(hold)
with open(result, "w") as out:
    out.write("latched" if os.path.exists(latch) else "absent")
fcntl.flock(fd, fcntl.LOCK_UN)
PY
	holder=$!
	n=0
	while [ ! -e "$WORK/holder.ready" ]; do
		n=$((n + 1)); [ "$n" -lt 500 ] || { echo "lock holder did not start" >&2; exit 1; }
		sleep 0.01
	done
	{
		echo 'kill() { :; }; killall() { :; }; stop_native() { :; }'
		sed -n '1,/stop-native-session$/p' "$ROOT/system/rootfs/etc/init.d/rcK" |
			sed -e "s#/run/banana#$RCK_RUN#g" -e "s#/data/banana#$RCK_DATA#g" \
				-e 's#/usr/libexec/banana/stop-native-session#stop_native#'
	} > "$WORK/rcK-head.sh"
	start=$(date +%s.%N)
	sh "$WORK/rcK-head.sh"
	end=$(date +%s.%N)
	[ -e "$RCK_RUN/shutting-down" ] || { echo "rcK did not latch the shutdown with the lock held" >&2; exit 1; }
	wait "$holder"
	took_ms=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%d", (b - a) * 1000 }')
	if [ "$hold" = 0.1 ]; then
		[ "$(cat "$WORK/holder.result")" = absent ] ||
			{ echo "rcK latched while a writer held resume.lock" >&2; exit 1; }
	else
		[ "$(cat "$WORK/holder.result")" = latched ] ||
			{ echo "rcK waited past its bound for resume.lock" >&2; exit 1; }
		[ "$took_ms" -lt 1200 ] || { echo "rcK latch wait is unbounded: ${took_ms} ms" >&2; exit 1; }
	fi
done
echo "rcK latches under resume.lock, bounded: ok"

python3 "$HERE/tests/check-rcK-native-child.py" "$HELPER" "$WORK"

echo "retro-lifecycle-test: passed"
