#!/bin/sh
# rpc-power-sample.sh, rpc-emu-bench.sh and rpc-run-join.sh on the host against a mock /sys and /proc. Power: unit
# conversion, validity, and energy integration against a deterministic clock with known expected energy. Bench: the
# collection protocol -- the process (pid + start time) locked for the whole window, a bounded post-window wait for
# its exit, a report bound to pid + start time + run id, and every way that can go wrong (early exit, replacement,
# pid reuse, ambiguity, stale or missing report, output errors) rejected. Join: one run id, covered windows.
# RPC_TEST_BENCH / RPC_TEST_POWER / RPC_TEST_JOIN point at other copies of the scripts (to run this against a
# previous version).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
W=$(mktemp -d)
# Stop whatever is still running (children of this shell: the bounded script under test) and remove the tree, on exit
# and on INT, TERM and HUP with the conventional status
cleanup() { pkill -TERM -P $$ 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
trap 'trap - EXIT; cleanup; exit 130' INT
trap 'trap - EXIT; cleanup; exit 143' TERM
trap 'trap - EXIT; cleanup; exit 129' HUP
S=$W/sys
fail() { echo "rpc-power-sample: FAIL: $*" | head -c 4096; echo; exit 1; }
# Every script under test runs bounded: address space (children inherit it), file size, wall time (killed after the
# grace). RPC_TEST_MEM_KB / RPC_TEST_SECS override the defaults. Use: env VAR=.. sh "$BND" sh script args
BND=$W/bounded.sh
cat > "$BND" <<'BNDEND'
ulimit -v "${RPC_TEST_MEM_KB:-524288}" || exit 125
ulimit -f 4096 || exit 125
exec timeout -k 2 "${RPC_TEST_SECS:-60}" "$@"
BNDEND
PWR=${RPC_TEST_POWER:-$ROOT/tools/rpc-power-sample.sh}
BENCH=${RPC_TEST_BENCH:-$ROOT/tools/rpc-emu-bench.sh}
JOIN=${RPC_TEST_JOIN:-$ROOT/tools/rpc-run-join.sh}

PS=$S/class/power_supply
mkdir -p "$PS/battery" "$PS/usb"
echo Battery > "$PS/battery/type"; echo Discharging > "$PS/battery/status"; echo 80 > "$PS/battery/capacity"
echo 4000000 > "$PS/battery/voltage_now"; echo -500000 > "$PS/battery/current_now"
echo 1500000 > "$PS/battery/charge_counter"
echo USB > "$PS/usb/type"; echo 0 > "$PS/usb/online"

# the deterministic clock: `clock.sh` prints the epoch, `clock.sh sleep N` advances it by N and runs clock.hook, if
# present, with the new time (to change the mock battery at a known instant). CLK_STEP replaces the step of the first
# sleep only; CLK_EVERY replaces the step of every sleep (sparse sampling). The state file is $CLKF.
CLK=$W/clock.sh; CLKF=$W/clock.now
cat > "$CLK" <<'CLOCKEND'
f=${CLKF:?}
case "$1" in
sleep) read -r t < "$f"; d=$2
	if [ -n "${CLK_EVERY:-}" ]; then d=$CLK_EVERY
	elif [ -n "${CLK_STEP:-}" ] && [ ! -f "$f.used" ]; then d=$CLK_STEP; : > "$f.used"; fi
	t=$((t + d)); echo "$t" > "$f"; [ -f "$f.hook" ] && sh "$f.hook" "$t" ;;
*) cat "$f" ;;
esac
CLOCKEND
T0=1000000

# run <expected rc> <label> [secs] [env...]: the sampler on the fake clock with RPC_OUT; stdout in $out
run() {
	want=$1; what=$2; n=${3:-1}; if [ $# -gt 3 ]; then shift 3; else shift $#; fi
	echo "${INIT:-$T0}" > "$CLKF"; rm -f "$CLKF.used"
	env RPC_SYS=$S RPC_OUT=$W/p.log RPC_CLOCK=$CLK CLKF=$CLKF "$@" sh "$BND" sh "$PWR" "$n" > "$W/run.out" 2>/dev/null; rc=$?
	out=$(head -c 65536 "$W/run.out")
	[ $rc -eq "$want" ] || fail "$what rc=$rc: $out"
}

run 0 "discharging run"
case "$out" in *"status=Discharging cap=80 v_mV=4000 i_mA=500 p_mW=2000 counter_uAh=1500000 online=none flags=ok"*) ;; *) fail "sample: $out" ;; esac
case "$out" in *"valid=yes"*"avg_mW=2000 "*|*"avg_mW=2000 "*"valid=yes"*) ;; *) fail "summary: $out" ;; esac
case "$out" in *"elapsed_s=1 requested_s=1"*) ;; *) fail "elapsed: $out" ;; esac
grep -q 'valid=yes' "$W/p.log" || fail "log lacks summary"
# 2000 mW for exactly one second is 2000/3600 = 0.56 mWh, and zero is a failure
case "$out" in *"energy_mWh=0.56 over 1 s"*) ;; *) fail "energy value: $out" ;; esac

# 2000 mW for 10 one-second samples: 20000 mWs = 5.56 mWh over exactly 10 s
run 0 "10 s run" 10
case "$out" in *"energy_mWh=5.56 over 10 s"*"run_id=none window_start=$T0 window_end=$((T0 + 10))"*) ;; *) fail "10 s energy/window: $out" ;; esac
case "$out" in *"elapsed_s=10 requested_s=10"*) ;; *) fail "10 s elapsed: $out" ;; esac
# a 3 s interval: samples at +0 +3 +6 +9, the last held until the stop at +12: 12 s of 2000 mW = 6.67 mWh
run 0 "3 s interval" 10 RPC_INTERVAL=3
case "$out" in *"energy_mWh=6.67 over 12 s"*"window_end=$((T0 + 12))"*) ;; *) fail "interval energy: $out" ;; esac
# power changing mid-run is time weighted: 5 s at 2000 mW then 5 s at 4000 mW = 30000 mWs = 8.33 mWh
printf '[ "$1" -ge %d ] && echo -1000000 > %s/battery/current_now\n' $((T0 + 5)) "$PS" > "$CLKF.hook"
run 0 "time weighted" 10
rm -f "$CLKF.hook"; echo -500000 > "$PS/battery/current_now"
case "$out" in *"time_weighted_avg_mW=3000"*"energy_mWh=8.33 over 10 s"*) ;; *) fail "time weighted energy: $out" ;; esac
# a clock that goes backwards is invalid, and so is a jump between samples
run 3 "clock backwards" 5 CLK_STEP=-1
case "$out" in *"clock-backwards"*"valid=no"*) ;; *) fail "clock backwards: $out" ;; esac
run 3 "sample gap" 20 CLK_STEP=10
case "$out" in *"sample-gap"*"valid=no"*) ;; *) fail "sample gap: $out" ;; esac
# the run id is recorded in the log, validated, and defaults to none
run 0 "run id" 2 RPC_RUN_ID=r-1.a_2
grep -q '^# run id=r-1.a_2 secs=2' "$W/p.log" || fail "log lacks run id header"
case "$out" in *"run_id=r-1.a_2 window_start="*) ;; *) fail "run id summary: $out" ;; esac
RPC_SYS=$S RPC_RUN_ID='bad id' sh "$PWR" 1 >/dev/null 2>&1; rc=$?
[ $rc -eq 2 ] || fail "bad run id rc=$rc"

# power_now, when the driver has it, is preferred (microwatts)
echo 1234000 > "$PS/battery/power_now"
run 0 "power_now run"
case "$out" in *"p_mW=1234 "*) ;; *) fail "power_now: $out" ;; esac
# a negative power_now is used by magnitude; an implausible one is flagged
echo -1234000 > "$PS/battery/power_now"
run 0 "negative power_now run"
case "$out" in *"p_mW=1234 "*) ;; *) fail "negative power_now: $out" ;; esac
echo 90000000 > "$PS/battery/power_now"
run 3 "implausible power"
case "$out" in *"range-p"*"valid=no"*) ;; *) fail "implausible power: $out" ;; esac
echo 0 > "$PS/battery/power_now"; echo 0 > "$PS/battery/current_now"
run 3 "zero power"
case "$out" in *"zero-power"*"valid=no"*) ;; *) fail "zero power: $out" ;; esac
rm "$PS/battery/power_now"; echo -500000 > "$PS/battery/current_now"

# ---- RPC_POWER_SRC: the power measurement source ----
# The frozen gauge of the G2 evidence (task11-preserve/g2): power_now stuck at 126433 mW (~106x) while voltage x current
# is 4250 mV x 280 mA = 1190 mW. Legacy default and gauge mode reject it; vi mode measures from voltage x current.
echo 4250000 > "$PS/battery/voltage_now"; echo -280000 > "$PS/battery/current_now"; echo 126433000 > "$PS/battery/power_now"
run 3 "default, frozen gauge"
case "$out" in *"flags=range-p req=default src=gauge gauge_mW=126433 warn=gauge-disagree"*"power_src_requested=default power_src=gauge"*"valid=no"*) ;; *) fail "default frozen gauge: $out" ;; esac
run 3 "gauge, frozen gauge" 1 RPC_POWER_SRC=gauge
case "$out" in *"flags=range-p req=gauge src=gauge"*"power_src_requested=gauge power_src=gauge"*"valid=no"*) ;; *) fail "gauge frozen: $out" ;; esac
run 0 "vi, frozen gauge" 1 RPC_POWER_SRC=vi
case "$out" in *"p_mW=1190 counter_uAh=1500000 online=none flags=ok req=vi src=vi gauge_mW=126433 warn=gauge-disagree"*) ;; *) fail "vi frozen sample: $out" ;; esac
case "$out" in *"avg_mW=1190 "*"power_src_requested=vi power_src=vi"*"gauge_disagreement_samples=1 of 1 comparable"*"valid=yes"*) ;; *) fail "vi frozen summary: $out" ;; esac
grep -q '^# run id=none secs=1 interval=1 power_src_requested=vi$' "$W/p.log" || fail "log lacks requested mode: $(head -n1 "$W/p.log")"
grep -q '^#   power_src_requested=vi power_src=vi$' "$W/p.log" || fail "log lacks source summary"
# vi never reads the gauge for the verdict: a missing, empty or garbage power_now changes nothing
rm "$PS/battery/power_now"
run 0 "vi, no gauge" 1 RPC_POWER_SRC=vi
case "$out" in *"p_mW=1190 "*"req=vi src=vi gauge_mW=- warn=n/a"*"valid=yes"*) ;; *) fail "vi without gauge: $out" ;; esac
echo abc > "$PS/battery/power_now"
run 0 "vi, garbage gauge" 1 RPC_POWER_SRC=vi
case "$out" in *"p_mW=1190 "*"src=vi gauge_mW=- warn=n/a"*"valid=yes"*) ;; *) fail "vi garbage gauge: $out" ;; esac
# the 20% disagreement is a diagnostic: exactly 20% (1428 vs 1190 would be 20%) is not over, one more mW is, and
# neither fails the run
echo 1428000 > "$PS/battery/power_now"
run 0 "vi, gauge at 20%" 1 RPC_POWER_SRC=vi
case "$out" in *"gauge_mW=1428 warn=none"*"gauge_disagreement_samples=0 of 1 comparable"*) ;; *) fail "20% boundary: $out" ;; esac
echo 1429000 > "$PS/battery/power_now"
run 0 "vi, gauge over 20%" 1 RPC_POWER_SRC=vi
case "$out" in *"gauge_mW=1429 warn=gauge-disagree"*"gauge_disagreement_samples=1 of 1 comparable"*"valid=yes"*) ;; *) fail "over 20%: $out" ;; esac
# A readable zero gauge is comparable and disagrees with a positive VI reading.
echo 0 > "$PS/battery/power_now"
run 0 "vi, zero gauge diagnostic" 1 RPC_POWER_SRC=vi
case "$out" in *"gauge_mW=0 warn=gauge-disagree"*"gauge_disagreement_samples=1 of 1 comparable"*"valid=yes"*) ;; *) fail "zero gauge diagnostic: $out" ;; esac
# gauge mode: a plausible power_now is used as is (and recorded as the source)
echo 1234000 > "$PS/battery/power_now"
run 0 "gauge, valid gauge" 1 RPC_POWER_SRC=gauge
case "$out" in *"p_mW=1234 "*"req=gauge src=gauge gauge_mW=1234 warn=none"*"power_src_requested=gauge power_src=gauge"*"valid=yes"*) ;; *) fail "gauge valid: $out" ;; esac
# gauge mode never falls back: valid voltage and current are present in all of these
rm "$PS/battery/power_now"
run 3 "gauge, missing gauge" 1 RPC_POWER_SRC=gauge
case "$out" in *"p_mW=- "*"flags=missing-p req=gauge src=- gauge_mW=- "*"power_src=none"*"valid=no"*) ;; *) fail "gauge missing: $out" ;; esac
echo 0 > "$PS/battery/power_now"
run 3 "gauge, zero gauge" 1 RPC_POWER_SRC=gauge
case "$out" in *"p_mW=0 "*"flags=zero-power req=gauge src=gauge"*"valid=no"*) ;; *) fail "gauge zero: $out" ;; esac
echo abc > "$PS/battery/power_now"
run 3 "gauge, garbage gauge" 1 RPC_POWER_SRC=gauge
case "$out" in *"flags=bad-reading req=gauge src=-"*"valid=no"*) ;; *) fail "gauge garbage: $out" ;; esac
echo 1234000 > "$PS/battery/power_now"
echo 4000 > "$PS/battery/voltage_now"
run 3 "gauge, bad voltage" 1 RPC_POWER_SRC=gauge
case "$out" in *"range-v"*"valid=no"*) ;; *) fail "gauge voltage range: $out" ;; esac
# the legacy default is unchanged: a positive gauge wins, a zero or missing one falls back to voltage x current
echo 4250000 > "$PS/battery/voltage_now"
run 0 "default, positive gauge"
case "$out" in *"p_mW=1234 "*"req=default src=gauge "*"power_src_requested=default power_src=gauge"*"valid=yes"*) ;; *) fail "default gauge: $out" ;; esac
echo 0 > "$PS/battery/power_now"
run 0 "default, zero gauge"
case "$out" in *"p_mW=1190 "*"req=default src=vi "*"power_src=vi"*"valid=yes"*) ;; *) fail "default zero-gauge fallback: $out" ;; esac
rm "$PS/battery/power_now"
run 0 "default, no gauge"
case "$out" in *"p_mW=1190 "*"req=default src=vi gauge_mW=- warn=n/a"*"valid=yes"*) ;; *) fail "default no gauge: $out" ;; esac
# vi mode: invalid voltage or current is rejected exactly as before, and never replaced by the gauge
echo 1234000 > "$PS/battery/power_now"
echo 4000 > "$PS/battery/voltage_now"
run 3 "vi, bad voltage" 1 RPC_POWER_SRC=vi
case "$out" in *"range-v"*"valid=no"*) ;; *) fail "vi voltage range: $out" ;; esac
echo 4250000 > "$PS/battery/voltage_now"; echo -50000000 > "$PS/battery/current_now"
run 3 "vi, huge current" 1 RPC_POWER_SRC=vi
case "$out" in *"range-i"*"valid=no"*) ;; *) fail "vi current range: $out" ;; esac
echo 0 > "$PS/battery/current_now"
run 3 "vi, zero current" 1 RPC_POWER_SRC=vi
case "$out" in *"p_mW=0 "*"zero-power"*"src=vi gauge_mW=1234"*"valid=no"*) ;; *) fail "vi zero current: $out" ;; esac
echo abc > "$PS/battery/current_now"
run 3 "vi, garbage current" 1 RPC_POWER_SRC=vi
case "$out" in *"bad-reading"*"src=-"*"valid=no"*) ;; *) fail "vi garbage current: $out" ;; esac
rm "$PS/battery/current_now"
run 3 "vi, missing current" 1 RPC_POWER_SRC=vi
case "$out" in *"p_mW=- "*"missing-i"*"src=-"*"valid=no"*) ;; *) fail "vi missing current (gauge present): $out" ;; esac
echo -500000 > "$PS/battery/current_now"; echo 4000000 > "$PS/battery/voltage_now"; rm "$PS/battery/power_now"
# the other protections hold in both explicit modes: a charger, a charging battery
echo 1234000 > "$PS/battery/power_now"
for m in vi gauge; do
	echo 1 > "$PS/usb/online"
	run 3 "$m, charger" 1 RPC_POWER_SRC=$m
	case "$out" in *"charger"*"valid=no"*) ;; *) fail "$m charger: $out" ;; esac
	echo 0 > "$PS/usb/online"; echo Charging > "$PS/battery/status"
	run 3 "$m, charging" 1 RPC_POWER_SRC=$m
	case "$out" in *"not-discharging"*"valid=no"*) ;; *) fail "$m charging: $out" ;; esac
	echo Discharging > "$PS/battery/status"
done
# output failures still end a vi or gauge run: stdout on /dev/full, and a device as the log
for m in vi gauge; do
	echo "$T0" > "$CLKF"; rm -f "$CLKF.used"
	env RPC_SYS=$S RPC_OUT=$W/so-$m.log RPC_CLOCK=$CLK CLKF=$CLKF RPC_POWER_SRC=$m sh "$PWR" 3 > /dev/full 2>/dev/null; rc=$?
	[ $rc -eq 3 ] || fail "$m stdout failure rc=$rc"
	grep -q '^#   valid=yes$' "$W/so-$m.log" && fail "$m stdout failure: log says valid=yes"
	grep -q 'output-error' "$W/so-$m.log" || fail "$m stdout failure: log lacks output-error"
	out=$(RPC_SYS=$S RPC_OUT=/dev/full RPC_POWER_SRC=$m sh "$PWR" 1 2>/dev/null); rc=$?
	[ $rc -eq 2 ] || fail "$m RPC_OUT=/dev/full rc=$rc"
done
rm "$PS/battery/power_now"
# unknown (and empty, and wrongly cased) modes are a usage error before anything is sampled
for bad in foo VI Gauge auto default ""; do
	out=$(RPC_SYS=$S RPC_POWER_SRC=$bad timeout 5 sh "$PWR" 1 2>&1); rc=$?
	[ $rc -eq 2 ] || fail "RPC_POWER_SRC='$bad' rc=$rc: $out"
	case "$out" in *"epoch status"*|*"valid="*) fail "RPC_POWER_SRC='$bad' sampled: $out" ;; esac
done
# the charge counter is supporting evidence: 1389 uAh x 4000 mV = 5.56 mWh against the 5.56 mWh integrated, reported
# with no verdict of its own; a counter that does not move does not invalidate the run either
printf '[ "$1" -ge %d ] && echo 1498611 > %s/battery/charge_counter\n' $((T0 + 5)) "$PS" > "$CLKF.hook"
run 0 "counter agreement" 10 RPC_POWER_SRC=vi
rm -f "$CLKF.hook"; echo 1500000 > "$PS/battery/charge_counter"
case "$out" in *"charge_counter_delta_uAh=-1389"*"counter_energy_mWh=5.56 counter_vs_energy_pct=100 (supporting evidence only)"*"valid=yes"*) ;; *) fail "counter agreement: $out" ;; esac
run 0 "counter flat" 10 RPC_POWER_SRC=vi
case "$out" in *"counter_energy_mWh=0.00 counter_vs_energy_pct=0 "*"valid=yes"*) ;; *) fail "counter flat: $out" ;; esac
# Signed endpoints must be differenced before taking the magnitude, including across zero.
echo -1500000 > "$PS/battery/charge_counter"
printf '[ "$1" -ge %d ] && echo -1501389 > %s/battery/charge_counter\n' $((T0 + 5)) "$PS" > "$CLKF.hook"
run 0 "negative counter" 10 RPC_POWER_SRC=vi
case "$out" in *"charge_counter_delta_uAh=-1389"*"counter_energy_mWh=5.56 counter_vs_energy_pct=100 "*) ;; *) fail "negative counter: $out" ;; esac
echo -1000 > "$PS/battery/charge_counter"
printf '[ "$1" -ge %d ] && echo 389 > %s/battery/charge_counter\n' $((T0 + 5)) "$PS" > "$CLKF.hook"
run 0 "counter crossing zero" 10 RPC_POWER_SRC=vi
case "$out" in *"charge_counter_delta_uAh=1389"*"counter_energy_mWh=5.56 counter_vs_energy_pct=100 "*) ;; *) fail "counter crossing zero: $out" ;; esac
echo -2147483648 > "$PS/battery/charge_counter"
printf '[ "$1" -ge %d ] && echo 2147483647 > %s/battery/charge_counter\n' $((T0 + 5)) "$PS" > "$CLKF.hook"
run 0 "counter signed limits" 10 RPC_POWER_SRC=vi
case "$out" in *"charge_counter_delta_uAh=4294967295"*"counter_energy_mWh=17179869.18 "*) ;; *) fail "counter signed limits: $out" ;; esac
rm -f "$CLKF.hook"; echo 2147483648 > "$PS/battery/charge_counter"
run 0 "counter out of range" 2 RPC_POWER_SRC=vi
case "$out" in *"counter_energy_mWh="*) fail "out-of-range counter used: $out" ;; esac
rm -f "$CLKF.hook"; echo 1500000 > "$PS/battery/charge_counter"

# a charger feeding the unit invalidates the run
echo 1 > "$PS/usb/online"
run 3 "charger"
case "$out" in *"online=usb"*"charger"*"valid=no"*) ;; *) fail "charger: $out" ;; esac
echo 0 > "$PS/usb/online"

# Charging, Full and Not charging are not discharge
for state in Charging Full "Not charging" ""; do
	echo "$state" > "$PS/battery/status"
	run 3 "status '$state'"
	case "$out" in *"not-discharging"*"valid=no"*) ;; *) fail "status '$state': $out" ;; esac
done
rm "$PS/battery/status"
run 3 "missing status"
case "$out" in *"not-discharging"*"valid=no"*) ;; *) fail "missing status: $out" ;; esac
echo Discharging > "$PS/battery/status"

# an out-of-range voltage (here millivolts where microvolts are expected) is flagged
echo 4000 > "$PS/battery/voltage_now"
run 3 "bad voltage"
case "$out" in *"range-v"*"valid=no"*) ;; *) fail "voltage range: $out" ;; esac
# a current beyond 10 A is flagged
echo 4000000 > "$PS/battery/voltage_now"; echo -50000000 > "$PS/battery/current_now"
run 3 "huge current"
case "$out" in *"range-i"*"valid=no"*) ;; *) fail "current range: $out" ;; esac

# missing, empty and non-numeric readings are never valid
echo -500000 > "$PS/battery/current_now"
rm "$PS/battery/current_now"
run 3 "missing current"
case "$out" in *"i_mA=-"*"missing-i"*"valid=no"*) ;; *) fail "missing current: $out" ;; esac
echo abc > "$PS/battery/current_now"
run 3 "non-numeric current"
case "$out" in *"bad-reading"*"valid=no"*) ;; *) fail "non-numeric current: $out" ;; esac
echo -500000 > "$PS/battery/current_now"
: > "$PS/battery/voltage_now"
run 3 "empty voltage"
case "$out" in *"v_mV=-"*"missing-v"*"valid=no"*) ;; *) fail "empty voltage: $out" ;; esac
echo 4000000 > "$PS/battery/voltage_now"
# fail closed without a log file too: unset, and /dev/null
echo abc > "$PS/battery/current_now"
out=$(RPC_SYS=$S sh "$PWR" 1 2>/dev/null); rc=$?
[ $rc -eq 3 ] || fail "invalid without RPC_OUT rc=$rc"
case "$out" in *"valid=no"*) ;; *) fail "no RPC_OUT summary: $out" ;; esac
echo -500000 > "$PS/battery/current_now"
out=$(RPC_SYS=$S sh "$PWR" 1 2>/dev/null); rc=$?
[ $rc -eq 0 ] || fail "valid without RPC_OUT rc=$rc: $out"
# a device accepts or refuses writes without keeping evidence: refused up front, never a valid run
for dev in /dev/null /dev/full; do
	out=$(RPC_SYS=$S RPC_OUT=$dev RPC_CLOCK=$CLK CLKF=$CLKF sh "$PWR" 1 2>/dev/null); rc=$?
	[ $rc -eq 2 ] || fail "valid readings with RPC_OUT=$dev rc=$rc: $out"
	case "$out" in *"valid=yes"*) fail "RPC_OUT=$dev: $out" ;; esac
done
# a stdout that fails: not success, and the log does not say valid=yes either
echo "$T0" > "$CLKF"; rm -f "$CLKF.used"
env RPC_SYS=$S RPC_OUT=$W/so.log RPC_CLOCK=$CLK CLKF=$CLKF sh "$PWR" 3 > /dev/full 2>/dev/null; rc=$?
[ $rc -eq 3 ] || fail "stdout failure rc=$rc"
grep -q '^#   valid=yes$' "$W/so.log" && fail "stdout failure: log says valid=yes: $(cat "$W/so.log")"
grep -q 'output-error' "$W/so.log" || fail "stdout failure: log lacks output-error"
# a log that starts failing during the run (writes beyond a size limit that is ignored as a signal): rc 3, and no
# valid=yes on stdout. The pipe keeps stdout out of the file size limit.
printf 'trap "" XFSZ\nulimit -f 1 || exit 125\nexec sh "$@"\n' > "$W/xfsz.sh"
echo "$T0" > "$CLKF"; rm -f "$CLKF.used"
( env RPC_SYS=$S RPC_OUT=$W/xf.log RPC_CLOCK=$CLK CLKF=$CLKF sh "$BND" sh "$W/xfsz.sh" "$PWR" 20 2>/dev/null; echo $? > "$W/xf.rc" ) | cat > "$W/xf.out"
[ "$(cat "$W/xf.rc")" = 3 ] || fail "log failing mid-run rc=$(cat "$W/xf.rc"): $(head -c 2048 "$W/xf.out")"
grep -q 'valid=yes' "$W/xf.out" && fail "log failing mid-run: stdout says valid=yes"
grep -q 'output-error' "$W/xf.out" || fail "log failing mid-run: stdout lacks output-error"
grep -q '^#   valid=yes$' "$W/xf.log" && fail "log failing mid-run: log says valid=yes"
# an explicitly named log that cannot be written is an error, not a silent downgrade
RPC_SYS=$S RPC_OUT=$W/no-such-dir/p.log sh "$PWR" 1 >/dev/null 2>&1; rc=$?
[ $rc -eq 2 ] || fail "unwritable RPC_OUT rc=$rc"

# bad durations would loop forever or never sample
for bad in abc 0 -5 1.5; do
	RPC_SYS=$S timeout 5 sh "$PWR" "$bad" >/dev/null 2>&1; rc=$?
	[ $rc -eq 2 ] || fail "secs '$bad' rc=$rc"
done
RPC_SYS=$S RPC_INTERVAL=0 timeout 5 sh "$PWR" 1 >/dev/null 2>&1; rc=$?
[ $rc -eq 2 ] || fail "interval 0 rc=$rc"

# ---- bench: mock processes on a virtual clock, one tree per scenario, run one at a time ----
# The bench reads time only through RPC_CLOCK (the same protocol as the power sampler): `clock.sh sleep N` advances the
# scenario's clock and runs its hook, which changes the mock /proc at an exact virtual second. Nothing sleeps and no
# result depends on the host's load or its second boundaries. The base is the real time, once, so that the mtimes of
# files the scenarios create (real time, never earlier) compare correctly with the bench's start.
BT0=$(date +%s)
{ echo "W=$W"; cat <<'LIBEND'
mkp() { # <proc dir> <pid> <comm> <starttime> [state]
	mkdir -p "$1/$2"; echo "$3" > "$1/$2/comm"; ln -sf "$W/emu-bin" "$1/$2/exe"
	echo "$2 ($3) ${5:-S} 1 1 1 0 -1 0 0 0 0 0 5 3 0 0 20 0 1 0 $4 0 0" > "$1/$2/stat"
}
telblock() { # <pid> <starttime> <run id>: what the native process prints as playback ends
	printf 'Telemetry run: pid=%s starttime=%s run_id=%s\nFrame telemetry: frames=5 intervals=4\nAudio status: input_rate=32768 output_rate=48000 accepted=1 underrun=0 xrun=0\n' "$1" "$2" "$3"
}
LIBEND
} > "$W/lib.sh"
. "$W/lib.sh"
printf 'core' > "$W/core.so"; printf 'rom' > "$W/game.gba"; printf 'binary' > "$W/emu-bin"
want_core=$(sha256sum "$W/core.so" | cut -d' ' -f1)
want_exe=$(sha256sum "$W/emu-bin" | cut -d' ' -f1)
newscen() { # <name>: tree with pid 100 (starttime 777), its clock hook
	mkdir -p "$W/$1/proc"; echo "0.10 0.10 0.10 1/1 1" > "$W/$1/proc/loadavg"; mkp "$W/$1/proc" 100 nothing 777
	echo ". $W/lib.sh" > "$W/$1/clock.now.hook"
}
at() { # <name> <seconds after the start> <command>: run it when the scenario's clock reaches that second
	printf '[ "$1" -ge %d ] && { %s; }\n' $((BT0 + $2)) "$3" >> "$W/$1/clock.now.hook"
}
fresh() { touch -d "@$((BT0 + 1))" "$1"; }   # a report newer than any run's start
launch() { # <name> <secs> [env...]: the bench to completion; stdout, stderr and the exit code under $W/<name>/
	n=$1; s=$2; shift 2
	echo "$BT0" > "$W/$n/clock.now"; rm -f "$W/$n/clock.now.used"
	env RPC_SYS=$S RPC_PROC=$W/$n/proc RPC_OUT=$W/$n/b.log RPC_EMU=nothing RPC_RUN_ID=run-$n RPC_COLLECT_SECS=6 \
		RPC_CLOCK=$CLK CLKF=$W/$n/clock.now \
		RPC_TELEMETRY_LOG=$W/$n/native.log RPC_LABEL=native-mgba RPC_CORE=$W/core.so RPC_ROM=$W/game.gba "$@" \
		sh "$BND" sh "$BENCH" "$s" > "$W/$n/out" 2> "$W/$n/err"; echo $? > "$W/$n/rc"
}
chk() { # <name> <rc> <text>...: exit code and text in the stdout
	n=$1; want=$2; shift 2
	got=$(cat "$W/$n/rc" 2>/dev/null); o=$(head -c 4096 "$W/$n/out" 2>/dev/null)
	[ "$got" = "$want" ] || fail "$n rc=$got want $want: $o stderr: $(head -c 1024 "$W/$n/err" 2>/dev/null)"
	# matched against the whole (size-limited) file, never the truncated diagnostic copy
	for t in "$@"; do grep -qF -- "$t" "$W/$n/out" || fail "$n lacks '$t': $o"; done
}
chkno() { # <name> <text>: text absent from the stdout
	case "$(cat "$W/$1/out")" in *"$2"*) fail "$1 has '$2': $(cat "$W/$1/out")" ;; esac
}
# the process exits normally after the window and just before that its report is written (the native order)
exit_after() { # <name> <seconds>
	at "$1" "$2" "telblock 100 777 run-$1 > \"\$W/$1/native.log\"; rm -rf \"\$W/$1/proc/100\""
}
gone() { at "$1" "$2" "rm -rf \"\$W/$1/proc/100\""; } # <name> <seconds>: the process exits

# normal: window 2 s, the report and the exit land 1 s after it closes, inside the collection wait
newscen ok; : > "$W/ok/native.log"; exit_after ok 3; launch ok 2
# premature exit: gone inside the 4 s window although a correct, fresh report is already there
newscen early; telblock 100 777 run-early > "$W/early/native.log"; fresh "$W/early/native.log"
gone early 1; launch early 4
# the same pid with another start time (a relaunch under a reused pid) mid-window
newscen reused; telblock 100 777 run-reused > "$W/reused/native.log"; fresh "$W/reused/native.log"
at reused 1 "mkp \"\$W/reused/proc\" 100 nothing 888"; launch reused 4
# a different pid takes over mid-window
newscen swapped; telblock 100 777 run-swapped > "$W/swapped/native.log"; fresh "$W/swapped/native.log"
at swapped 1 "rm -rf \"\$W/swapped/proc/100\"; mkp \"\$W/swapped/proc\" 101 nothing 999"; launch swapped 4
# a second matching process appears mid-window
newscen extra; telblock 100 777 run-extra > "$W/extra/native.log"; fresh "$W/extra/native.log"
at extra 1 "mkp \"\$W/extra/proc\" 101 nothing 999"; launch extra 4
# no process when the window opens, one appears later
newscen late; rm -rf "$W/late/proc/100"; at late 1 "mkp \"\$W/late/proc\" 100 nothing 777"; launch late 3 RPC_TELEMETRY_LOG=
# two differently named matches; two matches of one name
newscen twocomm; mkp "$W/twocomm/proc" 101 nothing-b 999; : > "$W/twocomm/native.log"
at twocomm 3 "telblock 100 777 run-twocomm > \"\$W/twocomm/native.log\"; rm -rf \"\$W/twocomm/proc/100\" \"\$W/twocomm/proc/101\""
launch twocomm 2
newscen twopid; mkp "$W/twopid/proc" 101 nothing 999; : > "$W/twopid/native.log"
at twopid 3 "telblock 100 777 run-twopid > \"\$W/twopid/native.log\"; rm -rf \"\$W/twopid/proc/100\" \"\$W/twopid/proc/101\""
launch twopid 2
# the process outlives the collection window
newscen alive; telblock 100 777 run-alive > "$W/alive/native.log"; fresh "$W/alive/native.log"
launch alive 1 RPC_COLLECT_SECS=2
# a pid-reuse trap: an old report of pid 100 under an older start time, claiming this very run id
newscen oldstart; { telblock 100 555 run-oldstart; telblock 100 777 run-other; } > "$W/oldstart/native.log"; fresh "$W/oldstart/native.log"
gone oldstart 3; launch oldstart 2
# an old run's report with this pid and start time under another run id, in a log that was touched since
newscen oldid; telblock 100 777 older-run > "$W/oldid/native.log"; fresh "$W/oldid/native.log"
gone oldid 3; launch oldid 2
# a report older than the run
newscen stale; telblock 100 777 run-stale > "$W/stale/native.log"; touch -d "@$((BT0 - 100))" "$W/stale/native.log"
gone stale 3; launch stale 2
# the process exits but leaves no report; a log that is not there at all
newscen noreport; printf 'noise\n' > "$W/noreport/native.log"; fresh "$W/noreport/native.log"
gone noreport 3; launch noreport 2
newscen nolog; gone nolog 3; launch nolog 2 RPC_TELEMETRY_LOG=$W/nolog/absent.log
# the report twice; a report without its frame line
newscen dup; { telblock 100 777 run-dup; telblock 100 777 run-dup; } > "$W/dup/native.log"; fresh "$W/dup/native.log"
gone dup 3; launch dup 2
newscen noframe; printf 'Telemetry run: pid=100 starttime=777 run_id=run-noframe\n' > "$W/noframe/native.log"; fresh "$W/noframe/native.log"
gone noframe 3; launch noframe 2
# a zombie is an exited process (its /proc entry stays until the parent reaps it) but is never a watched match:
# one that is already a zombie when the window opens is no process; one that becomes a zombie mid-window is a
# premature exit; one that becomes a zombie after the window has exited, with its report, and needs no wait
newscen zombstart; mkp "$W/zombstart/proc" 100 nothing 777 Z; launch zombstart 2 RPC_TELEMETRY_LOG=
newscen zombie; telblock 100 777 run-zombie > "$W/zombie/native.log"; fresh "$W/zombie/native.log"
at zombie 1 "mkp \"\$W/zombie/proc\" 100 nothing 777 Z"; launch zombie 4
newscen zombexit; telblock 100 777 run-zombexit > "$W/zombexit/native.log"; fresh "$W/zombexit/native.log"
at zombexit 3 "mkp \"\$W/zombexit/proc\" 100 nothing 777 Z"; launch zombexit 2
# sampling coverage on the integer clock: the clock advances CLK_EVERY s per sleep, so a 8 s window with 4 s steps
# has 2 samples (gap 4 > RPC_MAX_GAP 3, and under half of the 8 s); 12 s with 3 s steps has 4 samples (gap 3 is
# allowed, but 4 of 12 is sparse); 10 s with 2 s steps has 5 (the minimum, half of 10) with gaps of 2
newscen gap; launch gap 8 RPC_TELEMETRY_LOG= CLK_EVERY=4
newscen cover; launch cover 12 RPC_TELEMETRY_LOG= CLK_EVERY=3
newscen edge; launch edge 10 RPC_TELEMETRY_LOG= CLK_EVERY=2
newscen back; launch back 3 RPC_TELEMETRY_LOG= CLK_STEP=-1
# output errors: a log whose writes fail, one that swallows everything (no summary), a stdout that fails
newscen full; launch full 1 RPC_OUT=/dev/full RPC_TELEMETRY_LOG=
newscen devnull; launch devnull 1 RPC_OUT=/dev/null RPC_TELEMETRY_LOG=
newscen stdoutfull; echo "$BT0" > "$W/stdoutfull/clock.now"
env RPC_SYS=$S RPC_PROC=$W/stdoutfull/proc RPC_OUT=$W/stdoutfull/b.log RPC_EMU=nothing RPC_CLOCK=$CLK CLKF=$W/stdoutfull/clock.now \
	sh "$BND" sh "$BENCH" 1 > /dev/full 2>/dev/null; echo $? > "$W/stdoutfull/rc"
# a plain valid run without telemetry: header and identity, absent hash inputs
newscen plain; launch plain 1 RPC_TELEMETRY_LOG= RPC_RUN_ID=plain-run
newscen noinputs; launch noinputs 1 RPC_TELEMETRY_LOG= RPC_CORE= RPC_ROM=
newscen nocore; launch nocore 1 RPC_TELEMETRY_LOG= RPC_CORE=$W/absent.so
newscen noproc; launch noproc 1 RPC_TELEMETRY_LOG= RPC_EMU=zzz-absent

chk ok 0 "# window run_id=run-ok start=" "# Telemetry run: pid=100 starttime=777 run_id=run-ok" "# Frame telemetry: frames=5 intervals=4" \
	"# Audio status: input_rate=32768 output_rate=48000 accepted=1" "# valid=yes"
case "$(cat "$W/ok/out")" in *"# SUMMARY"*"# Telemetry run:"*"# valid=yes"*) ;; *) fail "ok order: $(cat "$W/ok/out")" ;; esac
case "$(cat "$W/ok/out")" in *"# run label=native-mgba emu=nothing secs=2 core=core.so core_sha256=$want_core rom=game.gba rom_sha256="*"run_id=run-ok"*) ;; *) fail "ok header: $(cat "$W/ok/out")" ;; esac
case "$(cat "$W/ok/out")" in *"# process pid=100 comm=nothing starttime=777 exe=$W/emu-bin exe_sha256=$want_exe"*) ;; *) fail "ok process: $(cat "$W/ok/out")" ;; esac
# on the virtual clock the window is exact: samples at +0 and +1, the window closes at +2, the exit lands in the
# collection wait at +3
nlines=$(grep -c '^[0-9]' "$W/ok/b.log"); sline=$(grep '^# SUMMARY  samples=' "$W/ok/b.log")
[ "$nlines" = 2 ] && [ "$sline" = "# SUMMARY  samples=2" ] || fail "ok samples: $nlines lines, '$sline'"
for want in "# run label=native-mgba" "# window run_id=run-ok" "# process pid=100 comm=nothing" "# Frame telemetry: frames=5" "# Telemetry run: pid=100 starttime=777 run_id=run-ok" "# valid=yes"; do
	grep -q -- "$want" "$W/ok/b.log" || fail "ok log lacks '$want'"
done
grep -qF "# window run_id=run-ok start=$BT0 end=$((BT0 + 2)) collect_end=$((BT0 + 3)) samples=2 max_gap=1" "$W/ok/b.log" || fail "ok window: $(grep '^# window ' "$W/ok/b.log")"
chk early 3 "valid=no reasons=" "premature-exit"
chk reused 3 "process-changed"
chk swapped 3 "process-changed"
chk extra 3 "process-changed"
chk late 3 "no-process-matched"
chk twocomm 3 "ambiguous-process"
chk twopid 3 "ambiguous-process"
chk alive 3 "process-not-exited"
chk oldstart 3 "telemetry-no-report-for-run" "telemetry-identity-conflict"; chkno oldstart "Frame telemetry: frames=5"
chk oldid 3 "telemetry-no-report-for-run"; chkno oldid "Frame telemetry: frames=5"; chkno oldid "telemetry-identity-conflict"
chk stale 3 "telemetry-stale"
chk noreport 3 "telemetry-no-report-for-run"
chk nolog 3 "telemetry-unreadable"
chk dup 3 "telemetry-ambiguous"
chk noframe 3 "telemetry-no-frame-line"
chk zombstart 3 "no-process-matched"
chk zombie 3 "premature-exit"
chk zombexit 0 "# valid=yes"; chkno zombexit "process-not-exited"
chk gap 3 "sample-gap" "samples=2 max_gap=4"
chk cover 3 "sparse-samples" "samples=4 max_gap=3"; chkno cover "sample-gap"
chk edge 0 "samples=5 max_gap=2" "# valid=yes"
chk back 3 "clock-backwards"
chk full 3 "output-error"
chk devnull 3 "summary-missing-or-mismatched" "summary-not-in-log"
# regression: a log that is a device which reads endlessly (/dev/full: NULs, no newline) must not be summarised into
# memory. 256 MiB address space, 15 s: the unbounded read dies or times out (rc 124/134/137), the fix is rc 3.
newscen fullmem
launch fullmem 1 RPC_OUT=/dev/full RPC_TELEMETRY_LOG= RPC_TEST_MEM_KB=262144 RPC_TEST_SECS=15
chk fullmem 3 "output-error"
[ "$(cat "$W/stdoutfull/rc")" = 3 ] || fail "stdout failure rc=$(cat "$W/stdoutfull/rc")"
chk plain 0 "run_id=plain-run" "# valid=yes"
chk noinputs 0 "core=none core_sha256=n/a rom=n/a rom_sha256=n/a"
chk nocore 0 "core=absent.so core_sha256=n/a "
chk noproc 3 "valid=no reasons=no-process-matched"
# a failed run never reports a pass anywhere
for n in early reused swapped extra late twocomm twopid alive oldstart oldid stale noreport nolog dup noframe \
	zombstart zombie gap cover back full devnull fullmem; do
	chkno $n "# valid=yes"
done

# bad arguments exit 2, and nothing falls back to /dev/null
newscen args
ba() { env RPC_SYS=$S RPC_PROC=$W/args/proc RPC_OUT=$W/args/b.log RPC_EMU=nothing "$@" >/dev/null 2>&1; echo $?; }
[ "$(ba sh "$BENCH" abc)" = 2 ] || fail "bench secs"
[ "$(ba RPC_OUT=$W/no-such-dir/b.log sh "$BENCH" 1)" = 2 ] || fail "unwritable bench log must exit 2"
[ "$(ba RPC_TELEMETRY_LOG=$W/x.log sh "$BENCH" 1)" = 2 ] || fail "telemetry without a run id must exit 2"
[ "$(ba RPC_RUN_ID='a b' sh "$BENCH" 1)" = 2 ] || fail "bad run id"
[ "$(ba RPC_COLLECT_SECS=x sh "$BENCH" 1)" = 2 ] || fail "bad collect secs"

# ---- join: one run id, covered windows ----
bs=$BT0; be=$((BT0 + 2))
INIT=$((bs - 1)); run 0 "power for the bench" $((be - bs + 4)) RPC_RUN_ID=run-ok; cp "$W/p.log" "$W/p-ok.log"
INIT=$((bs - 1)); run 0 "power, other id" $((be - bs + 4)) RPC_RUN_ID=run-other; cp "$W/p.log" "$W/p-other.log"
INIT=$((bs - 1)); run 0 "power, no id" $((be - bs + 4)); cp "$W/p.log" "$W/p-none.log"
INIT=$((be + 100)); run 0 "power, later window" 5 RPC_RUN_ID=run-ok; cp "$W/p.log" "$W/p-late.log"
echo abc > "$PS/battery/current_now"; INIT=$((bs - 1)); run 3 "power, invalid" $((be - bs + 4)) RPC_RUN_ID=run-ok; cp "$W/p.log" "$W/p-bad.log"
echo -500000 > "$PS/battery/current_now"; INIT=
join() { out=$(sh "$JOIN" "$1" "$2" 2>&1); rc=$?; [ $rc -eq "$3" ] || fail "join $1 $2 rc=$rc: $out"; }
join "$W/ok/b.log" "$W/p-ok.log" 0
case "$out" in *"# joined run_id=run-ok bench_window=$bs-$be power_window=$((bs - 1))-"*"valid=yes"*) ;; *) fail "join ok: $out" ;; esac
join "$W/ok/b.log" "$W/p-other.log" 3; case "$out" in *"run-id-mismatch"*) ;; *) fail "join mismatch: $out" ;; esac
join "$W/ok/b.log" "$W/p-none.log" 3; case "$out" in *"run-id-mismatch"*) ;; *) fail "join none: $out" ;; esac
join "$W/ok/b.log" "$W/p-late.log" 3; case "$out" in *"window-not-covered"*) ;; *) fail "join window: $out" ;; esac
join "$W/ok/b.log" "$W/p-bad.log" 3; case "$out" in *"power-invalid"*) ;; *) fail "join invalid power: $out" ;; esac
join "$W/early/b.log" "$W/p-ok.log" 3; case "$out" in *"bench-invalid"*) ;; *) fail "join invalid bench: $out" ;; esac
join "$W/plain/b.log" "$W/p-ok.log" 3; case "$out" in *"bench-invalid"*|*"run-id-mismatch"*) ;; *) fail "join plain: $out" ;; esac
sh "$JOIN" "$W/ok/b.log" 2>/dev/null; [ $? -eq 2 ] || fail "join usage"
sh "$JOIN" "$W/ok/b.log" "$W/absent" 2>/dev/null; [ $? -eq 2 ] || fail "join unreadable"
# the power window may begin up to RPC_JOIN_MAX_EXTRA (10) s before the bench window and end that long after it, and
# the whole of it may be 2x that longer; one second beyond any of these is not a joined run
pw() { # <name> <lead> <tail>: a power run from bs-lead to be+tail with the bench's run id
	INIT=$((bs - $2)); run 0 "power $1" $((be - bs + $2 + $3)) RPC_RUN_ID=run-ok; cp "$W/p.log" "$W/p-$1.log"
}
pw limit 10 10; join "$W/ok/b.log" "$W/p-limit.log" 0
case "$out" in *"valid=yes"*) ;; *) fail "join at the limits: $out" ;; esac
pw lead 11 1; join "$W/ok/b.log" "$W/p-lead.log" 3
case "$out" in *"power-start-too-early"*) ;; *) fail "join lead: $out" ;; esac
case "$out" in *"power-end-too-late"*|*"power-window-too-long"*) fail "join lead only: $out" ;; esac
pw tail 1 11; join "$W/ok/b.log" "$W/p-tail.log" 3
case "$out" in *"power-end-too-late"*) ;; *) fail "join tail: $out" ;; esac
case "$out" in *"power-start-too-early"*|*"power-window-too-long"*) fail "join tail only: $out" ;; esac
pw long 10 11; join "$W/ok/b.log" "$W/p-long.log" 3
case "$out" in *"power-end-too-late"*"power-window-too-long"*) ;; *) fail "join long: $out" ;; esac
out=$(RPC_JOIN_MAX_EXTRA=11 sh "$JOIN" "$W/ok/b.log" "$W/p-long.log" 2>&1); rc=$?
[ $rc -eq 0 ] || fail "join long with extra 11 rc=$rc: $out"   # lead 10 and tail 11 are within 11 each, and 21 s over 2x11
case "$out" in *"power-window-too-long"*) fail "join extra 11: $out" ;; *"valid=yes"*) ;; *) fail "join extra 11: $out" ;; esac
RPC_JOIN_MAX_EXTRA=x sh "$JOIN" "$W/ok/b.log" "$W/p-ok.log" >/dev/null 2>&1; [ $? -eq 2 ] || fail "join bad max extra"
RPC_JOIN_TOL=x sh "$JOIN" "$W/ok/b.log" "$W/p-ok.log" >/dev/null 2>&1; [ $? -eq 2 ] || fail "join bad tol"
# a power log that recorded an output failure is never accepted, whatever else it says
{ cat "$W/p-ok.log"; echo '# valid=no reasons=output-error'; } > "$W/p-oe.log"
join "$W/ok/b.log" "$W/p-oe.log" 3; case "$out" in *"power-output-error"*) ;; *) fail "join output error: $out" ;; esac
# measurement source: the power log carries it into the join, which accepts only one consistent source; the bench log
# (the emulator collector) states none and is not asked to
join "$W/ok/b.log" "$W/p-ok.log" 0; case "$out" in *"power_src=vi valid=yes"*"power_src_requested=default"*) ;; *) fail "join reports source: $out" ;; esac
INIT=$((bs - 1)); run 0 "power, vi" $((be - bs + 4)) RPC_RUN_ID=run-ok RPC_POWER_SRC=vi; cp "$W/p.log" "$W/p-vi.log"
echo 1234000 > "$PS/battery/power_now"
INIT=$((bs - 1)); run 0 "power, gauge" $((be - bs + 4)) RPC_RUN_ID=run-ok RPC_POWER_SRC=gauge; cp "$W/p.log" "$W/p-gauge.log"
rm "$PS/battery/power_now"
# a default run whose source changes mid-run (the gauge appears at +1): mixed, valid for the sampler, never joined
printf '[ "$1" -ge %d ] && echo 1234000 > %s/battery/power_now\n' $((bs + 1)) "$PS" > "$CLKF.hook"
INIT=$((bs - 1)); run 0 "power, mixed" $((be - bs + 4)) RPC_RUN_ID=run-ok; cp "$W/p.log" "$W/p-mixed.log"
rm -f "$CLKF.hook" "$PS/battery/power_now"
grep -q '^#   power_src_requested=default power_src=mixed$' "$W/p-mixed.log" || fail "mixed run summary: $(grep power_src "$W/p-mixed.log")"
join "$W/ok/b.log" "$W/p-vi.log" 0;    case "$out" in *"power_src=vi valid=yes"*"power_src_requested=vi"*) ;; *) fail "join vi: $out" ;; esac
join "$W/ok/b.log" "$W/p-gauge.log" 0; case "$out" in *"power_src=gauge valid=yes"*"power_src_requested=gauge"*) ;; *) fail "join gauge: $out" ;; esac
join "$W/ok/b.log" "$W/p-mixed.log" 3; case "$out" in *"power-src-unusable"*) ;; *) fail "join mixed: $out" ;; esac
# a log without the source record (an old log), and records that contradict each other
grep -v '^#   power_src_requested=' "$W/p-vi.log" > "$W/p-nosrc.log"
join "$W/ok/b.log" "$W/p-nosrc.log" 3; case "$out" in *"power-src-missing"*) ;; *) fail "join no source: $out" ;; esac
sed 's/^#   power_src_requested=vi power_src=vi$/#   power_src_requested=vi power_src=gauge/' "$W/p-vi.log" > "$W/p-t1.log"
join "$W/ok/b.log" "$W/p-t1.log" 3; case "$out" in *"power-src-inconsistent"*) ;; *) fail "join summary vs request: $out" ;; esac
sed 's/^#   power_src_requested=vi power_src=vi$/#   power_src_requested=gauge power_src=vi/' "$W/p-vi.log" > "$W/p-t2.log"
join "$W/ok/b.log" "$W/p-t2.log" 3; case "$out" in *"power-src-inconsistent"*) ;; *) fail "join request vs source: $out" ;; esac
sed '0,/ src=vi /s// src=gauge /' "$W/p-vi.log" > "$W/p-t3.log"
join "$W/ok/b.log" "$W/p-t3.log" 3; case "$out" in *"power-src-inconsistent"*) ;; *) fail "join sample vs summary: $out" ;; esac
sed '0,/ req=vi /s// req=gauge /' "$W/p-vi.log" > "$W/p-t4.log"
join "$W/ok/b.log" "$W/p-t4.log" 3; case "$out" in *"power-src-inconsistent"*) ;; *) fail "join sample request: $out" ;; esac
# matched comparison of two power logs: the same source only
cmp_() { out=$(sh "$JOIN" --compare-power "$1" "$2" 2>&1); rc=$?; [ $rc -eq "$3" ] || fail "compare $1 $2 rc=$rc: $out"; }
cmp_ "$W/p-vi.log" "$W/p-ok.log" 0;    case "$out" in *"power_src=vi valid=yes"*) ;; *) fail "compare vi/default-vi: $out" ;; esac
cmp_ "$W/p-gauge.log" "$W/p-gauge.log" 0; case "$out" in *"power_src=gauge valid=yes"*) ;; *) fail "compare gauge/gauge: $out" ;; esac
cmp_ "$W/p-vi.log" "$W/p-gauge.log" 3; case "$out" in *"power-src-mismatch"*) ;; *) fail "compare vi/gauge: $out" ;; esac
cmp_ "$W/p-gauge.log" "$W/p-vi.log" 3; case "$out" in *"power-src-mismatch"*) ;; *) fail "compare gauge/vi: $out" ;; esac
cmp_ "$W/p-vi.log" "$W/p-mixed.log" 3; case "$out" in *"b-power-src-unusable"*) ;; *) fail "compare mixed: $out" ;; esac
cmp_ "$W/p-vi.log" "$W/p-nosrc.log" 3; case "$out" in *"b-power-src-missing"*) ;; *) fail "compare no source: $out" ;; esac
cmp_ "$W/p-vi.log" "$W/p-bad.log" 3; case "$out" in *"b-power-invalid"*) ;; *) fail "compare invalid: $out" ;; esac
cmp_ "$W/p-vi.log" "$W/p-oe.log" 3; case "$out" in *"b-power-output-error"*) ;; *) fail "compare output error: $out" ;; esac
# Both entry points must validate the evidence, not just the verdict and source summary.
reject_evidence() {
	join "$W/ok/b.log" "$1" 3
	cmp_ "$W/p-vi.log" "$1" 3
}
printf '#   power_src_requested=vi power_src=vi\n#   valid=yes\n' > "$W/p-summary-only.log"
reject_evidence "$W/p-summary-only.log"
grep -v '^[0-9]' "$W/p-vi.log" > "$W/p-no-samples.log"
reject_evidence "$W/p-no-samples.log"
sed '0,/ src=vi /s// src=- /' "$W/p-vi.log" > "$W/p-unknown-src.log"
reject_evidence "$W/p-unknown-src.log"
sed '1s/power_src_requested=vi/power_src_requested=gauge/' "$W/p-vi.log" > "$W/p-header-mismatch.log"
reject_evidence "$W/p-header-mismatch.log"
sed '1s/$/ power_src_requested=vi/' "$W/p-vi.log" > "$W/p-header-duplicate.log"
reject_evidence "$W/p-header-duplicate.log"
sed '0,/ req=vi /s// req=vi req=vi /' "$W/p-vi.log" > "$W/p-sample-duplicate.log"
reject_evidence "$W/p-sample-duplicate.log"
sed '0,/ flags=ok /s// flags=charger /' "$W/p-vi.log" > "$W/p-sample-invalid.log"
reject_evidence "$W/p-sample-invalid.log"
sed 's/samples=6 valid_samples=6/samples=7 valid_samples=7/' "$W/p-vi.log" > "$W/p-count.log"
reject_evidence "$W/p-count.log"
sed '/^#   energy_mWh=/d' "$W/p-vi.log" > "$W/p-no-energy.log"
reject_evidence "$W/p-no-energy.log"
sed 's/^#   energy_mWh=[^ ]*/#   energy_mWh=999.99/' "$W/p-vi.log" > "$W/p-energy.log"
reject_evidence "$W/p-energy.log"
sed '/^#   run_id=.* window_start=/d' "$W/p-vi.log" > "$W/p-no-window.log"
reject_evidence "$W/p-no-window.log"
sed '/^#   run_id=.* window_start=/p' "$W/p-vi.log" > "$W/p-duplicate-window.log"
reject_evidence "$W/p-duplicate-window.log"
sed '/^#   power_src_requested=/p' "$W/p-vi.log" > "$W/p-duplicate-summary.log"
reject_evidence "$W/p-duplicate-summary.log"
sh "$JOIN" --compare-power "$W/p-vi.log" 2>/dev/null; [ $? -eq 2 ] || fail "compare usage"
sh "$JOIN" --compare-power "$W/p-vi.log" "$W/absent" 2>/dev/null; [ $? -eq 2 ] || fail "compare unreadable"
echo "rpc-power-sample: ok"
