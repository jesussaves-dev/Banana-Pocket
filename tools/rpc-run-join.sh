#!/bin/sh
# Pair the log of tools/rpc-emu-bench.sh with the log of tools/rpc-power-sample.sh for ONE run.
#
#   sh tools/rpc-run-join.sh bench.log power.log
#
# Accepted only when both runs were valid, carry the same real run id (RPC_RUN_ID, not "none"), each recorded exactly
# one measurement window, and the windows agree at both ends. The result is valid=yes with both windows, or
# valid=no with reasons and exit 3; bad arguments or unreadable logs exit 2.
#
# Time is whole epoch seconds, truncated (`date +%s`), and the two scripts read their start and end from separate
# clock reads, so every boundary is uncertain by up to 1 s and a pair of boundaries by up to 2 s (measurement
# uncertainty; a window of N s is N-1 to N+1 s of real time).
#   RPC_JOIN_TOL=<n>        (default 2) slack, in seconds, for coverage: the power window may start up to n s after
#                           the benchmark window starts and end up to n s before it ends. Larger than the 1-2 s of
#                           truncation the clocks can show is a looser claim, not a better one.
#   RPC_JOIN_MAX_EXTRA=<n>  (default 10) the most, in seconds, the power window may begin before or end after the
#                           benchmark window (the time to start and stop two commands by hand); the whole power
#                           window may exceed the benchmark window by at most 2n s. Beyond that the energy and
#                           average power include time the benchmark did not run, so there is no joined figure.
# The energy in the power log is for the whole power window, which is at least the benchmark window less the slack;
# only a power run started just before and stopped just after the benchmark gives a per-run figure, and it stays an
# estimate with the uncertainty above. Native frame statistics, when present, cover the native process's lifetime
# from the start of paced playback to playback end, not this window; the join does not align them with it.
[ $# -eq 2 ] || { echo "usage: rpc-run-join.sh bench.log power.log" >&2; exit 2; }
bench=$1 power=$2
[ -r "$bench" ] && [ -r "$power" ] || { echo "rpc-run-join: cannot read $bench or $power" >&2; exit 2; }
tol=${RPC_JOIN_TOL:-2}
case "$tol" in ''|*[!0-9]*) echo "rpc-run-join: RPC_JOIN_TOL must be a non-negative integer" >&2; exit 2 ;; esac
extra=${RPC_JOIN_MAX_EXTRA:-10}
case "$extra" in ''|*[!0-9]*) echo "rpc-run-join: RPC_JOIN_MAX_EXTRA must be a non-negative integer" >&2; exit 2 ;; esac

reasons=
# bench: exactly one "# window run_id=<id> start=<s> end=<e> collect_end=<c>" and a "# valid=yes" verdict
nbw=$(grep -c '^# window run_id=' "$bench")
grep -q '^# valid=yes$' "$bench" || reasons="$reasons bench-invalid"
[ "$nbw" = 1 ] || reasons="$reasons bench-window-count=$nbw"
bid=$(sed -n 's/^# window run_id=\([^ ]*\) .*/\1/p' "$bench" | head -n1)
bs=$(sed -n 's/^# window .* start=\([0-9]*\) .*/\1/p' "$bench" | head -n1)
be=$(sed -n 's/^# window .* end=\([0-9]*\) .*/\1/p' "$bench" | head -n1)
# power: exactly one "#   run_id=<id> window_start=<s> window_end=<e>" and "valid=yes" in the summary
npw=$(grep -c '^#   run_id=.* window_start=' "$power")
grep -q '^#   valid=yes$' "$power" || reasons="$reasons power-invalid"
grep -q '^# valid=no reasons=output-error$' "$power" && reasons="$reasons power-output-error"
[ "$npw" = 1 ] || reasons="$reasons power-window-count=$npw"
pid_=$(sed -n 's/^#   run_id=\([^ ]*\) window_start=.*/\1/p' "$power" | head -n1)
ps_=$(sed -n 's/^#   run_id=.* window_start=\([0-9]*\) .*/\1/p' "$power" | head -n1)
pe_=$(sed -n 's/^#   run_id=.* window_end=\([0-9]*\).*/\1/p' "$power" | head -n1)

if [ -z "$bid" ] || [ -z "$pid_" ]; then reasons="$reasons run-id-missing"
elif [ "$bid" != "$pid_" ]; then reasons="$reasons run-id-mismatch"
elif [ "$bid" = none ]; then reasons="$reasons run-id-none"; fi
if [ -z "$bs" ] || [ -z "$be" ] || [ -z "$ps_" ] || [ -z "$pe_" ]; then reasons="$reasons window-missing"
else
	[ "$bs" -lt "$be" ] && [ "$ps_" -lt "$pe_" ] || reasons="$reasons window-empty"
	[ "$ps_" -le $((bs + tol)) ] && [ "$pe_" -ge $((be - tol)) ] || reasons="$reasons window-not-covered"
	# and not by much more: the energy figure is for the whole power window, so a long lead or tail is energy
	# that was not the benchmark's
	[ $((bs - ps_)) -le "$extra" ] || reasons="$reasons power-start-too-early"
	[ $((pe_ - be)) -le "$extra" ] || reasons="$reasons power-end-too-late"
	[ $((pe_ - ps_ - (be - bs))) -le $((2 * extra)) ] || reasons="$reasons power-window-too-long"
fi
if [ -n "$reasons" ]; then
	echo "# joined run_id=${bid:-?} valid=no reasons=${reasons# }"
	exit 3
fi
echo "# joined run_id=$bid bench_window=$bs-$be power_window=$ps_-$pe_ valid=yes"
grep -E '^#   (avg_mW|energy_mWh|elapsed_s)' "$power" 2>/dev/null || :
