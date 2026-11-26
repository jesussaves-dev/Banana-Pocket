#!/bin/sh
# Battery power drawn by the handheld during a run, one line a sample, from /sys/class/power_supply only, so the
# same RetroArch and native banana-retro runs can be compared in energy as well as pacing. Run it from the host
# over ssh/adb or on the unit:
#
#   ssh root@<handheld> sh /data/rpc-power-sample.sh 300 > power-native.txt
#
# Units follow the kernel power_supply ABI (Documentation/ABI/testing/sysfs-class-power): voltage_now in microvolts,
# current_now in microamps, power_now in microwatts, charge_counter in microamp-hours, capacity in percent. The
# current's sign is driver specific, so the magnitude is used and `status` decides what the sample means.
#
# A sample is only evidence of discharge power while status=Discharging, no other supply reports online=1, voltage and
# at least one of current_now/power_now are readable integers, and the power is positive and plausible. Anything else
# is flagged (missing-v, missing-i, bad-reading, zero-power, range-v, range-i, range-p, not-discharging, charger), the
# summary says valid=no and the script exits 3 -- whatever RPC_OUT is, or whether it is set. The run is also invalid
# when it covered less than the requested seconds, the clock went backwards, or two samples were far apart.
#
# What this does NOT establish: the units are never detected, only checked for plausibility. A reading is flagged when
# it falls outside the Li-ion range (voltage 2.5-5.0 V, current under 10 A, power under 20 W); a current reported in
# the wrong unit but still inside that range (say milliamps where microamps are expected, reading as 1/1000 of the
# draw) is undetectable here. Cross-check avg_mW against a known load once per device. Over a USB gadget ssh link the
# usb supply shows online=1, which makes the run invalid on purpose: use a network link, not the cable.
#
# energy_mWh integrates the valid samples' power over the time to the next sample (the last one to the end of the
# run). Battery SoC percent is coarse: use avg_mW/energy_mWh, not the capacity delta.
#
#   RPC_SYS=<dir>          a mock tree (the host test)
#   RPC_BATTERY=<name>     the supply to read (default: the first with type=Battery)
#   RPC_INTERVAL=<secs>    whole seconds between samples (default 1)
#   RPC_OUT=<file>         also write the log and summary there. It must be a regular file that can be written (a
#                          device such as /dev/null or /dev/full keeps no evidence: exit 2). A write that fails on the
#                          log or on stdout during the run is remembered and ends the run with "valid=no
#                          reasons=output-error" (best effort) and exit 3, never valid=yes.
#   RPC_RUN_ID=<id>        1-64 of [A-Za-z0-9._-]: the identifier shared with rpc-emu-bench.sh (and the native
#                          BANANA_RETRO_RUN_ID) so tools/rpc-run-join.sh can pair this log with that run. Recorded
#                          in the log with the measurement window (start, end, epoch seconds); "none" if unset.
#   RPC_CLOCK=<script>     the host test's deterministic clock: `sh script` prints the epoch, `sh script sleep N`
#                          advances it. Never set on the unit.
SYS=${RPC_SYS:-/sys}
secs=${1:-300}
step=${RPC_INTERVAL:-1}
PS=$SYS/class/power_supply
RUN_ID=${RPC_RUN_ID:-none}
case "$RUN_ID" in *[!A-Za-z0-9._-]*|'') echo "rpc-power-sample: RPC_RUN_ID must be 1-64 of A-Za-z0-9._-, got '$RUN_ID'" >&2; exit 2 ;; esac
[ ${#RUN_ID} -le 64 ] || { echo "rpc-power-sample: RPC_RUN_ID is longer than 64" >&2; exit 2; }
clock() { if [ -n "$RPC_CLOCK" ]; then sh "$RPC_CLOCK"; else date +%s; fi; }
nap() { if [ -n "$RPC_CLOCK" ]; then sh "$RPC_CLOCK" sleep "$1"; else sleep "$1"; fi; }

case "$secs" in ''|*[!0-9]*|0) echo "rpc-power-sample: seconds must be a positive integer, got '$secs'" >&2; exit 2 ;; esac
case "$step" in ''|*[!0-9]*|0) echo "rpc-power-sample: RPC_INTERVAL must be a positive integer, got '$step'" >&2; exit 2 ;; esac

BAT=
if [ -n "$RPC_BATTERY" ]; then BAT=$PS/$RPC_BATTERY
else
	for d in "$PS"/*; do
		[ -r "$d/type" ] || continue
		read -r t < "$d/type" 2>/dev/null || continue
		[ "$t" = Battery ] && { BAT=$d; break; }
	done
fi
if [ -z "$BAT" ] || [ ! -d "$BAT" ]; then
	echo "rpc-power-sample: no battery under $PS" >&2
	exit 2
fi

# The verdict is computed from the samples held here, never from the output file, so it cannot depend on where (or
# whether) the log is written.
OUT=
if [ -n "$RPC_OUT" ]; then
	( : > "$RPC_OUT" ) 2>/dev/null || { echo "rpc-power-sample: cannot write $RPC_OUT" >&2; exit 2; }
	# a device (/dev/null, /dev/full) accepts or refuses writes without keeping any evidence
	[ -f "$RPC_OUT" ] || { echo "rpc-power-sample: $RPC_OUT is not a regular file" >&2; exit 2; }
	OUT=$RPC_OUT
fi
# every line goes to stdout and the log; a failed write on either is remembered (stickily) and fails the run
outfail=0
emit() {
	{ printf '%s\n' "$1"; } 2>/dev/null || outfail=1
	[ -z "$OUT" ] || { printf '%s\n' "$1" >> "$OUT"; } 2>/dev/null || outfail=1
	[ "$outfail" = 0 ]
}

rd() { v=; [ -r "$1" ] && read -r v < "$1" 2>/dev/null || v=; printf '%s' "$v"; }

# other supplies with online=1 (a charger or USB host feeding the unit)
online_sources() {
	for d in "$PS"/*; do
		[ "$d" = "$BAT" ] && continue
		[ -r "$d/online" ] || continue
		[ "$(rd "$d/online")" = 1 ] && printf '%s,' "${d##*/}"
	done
}

emit "# run id=$RUN_ID secs=$secs interval=$step"
emit '# epoch status=<state> cap=<%> v_mV=<mV> i_mA=<mA magnitude|-> p_mW=<mW|-> counter_uAh=<uAh> online=<supplies|none> flags=<...>'
samples=
start=$(clock)
end=$((start + secs))
while :; do
	now=$(clock)
	[ "$now" -ge "$end" ] && break
	st=$(rd "$BAT/status"); cap=$(rd "$BAT/capacity"); uv=$(rd "$BAT/voltage_now")
	ua=$(rd "$BAT/current_now"); uw=$(rd "$BAT/power_now"); cc=$(rd "$BAT/charge_counter")
	on=$(online_sources); on=${on%,}
	line=$(awk -v now="$now" -v st="${st:-unknown}" -v cap="${cap:--}" -v uv="$uv" -v ua="$ua" \
		-v uw="$uw" -v cc="${cc:--}" -v on="${on:-none}" 'BEGIN {
		isint = "^-?[0-9]+$"
		flags = ""
		hv = (uv ~ isint); hi = (ua ~ isint); hp = (uw ~ isint)
		if (!hv) flags = flags (uv == "" ? "missing-v," : "bad-reading,")
		if (ua == "" && uw == "") flags = flags "missing-i,"
		else if ((ua != "" && !hi) || (uw != "" && !hp)) flags = flags "bad-reading,"
		if (hv) { uv += 0; if (uv < 2500000 || uv > 5000000) flags = flags "range-v," }
		if (hi) { ua += 0; if (ua < 0) ua = -ua; if (ua > 10000000) flags = flags "range-i," }
		if (hp) { uw += 0; if (uw < 0) uw = -uw }
		# power: the gauge power when it reads positive, else voltage x current
		have = 0; p = 0
		if (hp && uw > 0) { p = uw / 1000; have = 1 }
		else if (hv && hi) { p = (uv / 1000) * (ua / 1000) / 1000; have = 1 }
		if (have && p <= 0) flags = flags "zero-power,"
		if (have && p > 20000) flags = flags "range-p,"
		if (!have && (hi || hp)) flags = flags "zero-power,"
		if (st != "Discharging") flags = flags "not-discharging,"
		if (on != "none") flags = flags "charger,"
		if (flags == "") flags = "ok"; else sub(/,$/, "", flags)
		printf "%s status=%s cap=%s v_mV=%s i_mA=%s p_mW=%s counter_uAh=%s online=%s flags=%s", \
			now, st, cap, (hv ? sprintf("%.0f", uv / 1000) : "-"), (hi ? sprintf("%.0f", ua / 1000) : "-"), \
			(have ? sprintf("%.0f", p) : "-"), cc, on, flags }')
	emit "$line"
	samples="$samples$line
"
	nap "$step"
done
stop=$(clock)

summary=$(printf '%s' "$samples" | awk -v start="$start" -v stop="$stop" -v secs="$secs" -v step="$step" -v run_id="$RUN_ID" '
$1 ~ /^[0-9]+$/ {
	n++; t[n] = $1 + 0
	split("", f)
	for (i = 2; i <= NF; i++) { split($i, a, "="); f[a[1]] = a[2] }
	isok[n] = (f["flags"] == "ok"); p[n] = f["p_mW"] + 0; v[n] = f["v_mV"] + 0
	if (f["counter_uAh"] ~ /^[0-9]+$/) { if (c0 == "") c0 = f["counter_uAh"]; c1 = f["counter_uAh"] }
	cap1 = f["cap"]; if (cap0 == "") cap0 = f["cap"]
}
END {
	for (i = 1; i <= n; i++) {
		dt = (i < n ? t[i + 1] : stop) - t[i]
		if (dt < 0) { backwards = 1; dt = 0 }
		if (dt > maxdt) maxdt = dt
		if (isok[i]) { ok++; ps += p[i]; if (p[i] > pk) pk = p[i]; vs += v[i]; ws += p[i] * dt; wt += dt }
		else bad++
	}
	elapsed = stop - start
	reasons = ""
	if (n == 0) reasons = reasons "no-samples,"
	if (bad > 0) reasons = reasons "invalid-samples,"
	if (elapsed < secs) reasons = reasons "short-run,"
	if (backwards) reasons = reasons "clock-backwards,"
	if (maxdt > 2 * step + 2) reasons = reasons "sample-gap,"
	if (reasons == "") reasons = "none"; else sub(/,$/, "", reasons)
	printf "# SUMMARY  samples=%d valid_samples=%d invalid_samples=%d elapsed_s=%d requested_s=%d\n", n, ok, bad, elapsed, secs
	printf "#   avg_mW=%.0f peak_mW=%.0f avg_mV=%.0f time_weighted_avg_mW=%.0f\n", (ok ? ps / ok : 0), pk, (ok ? vs / ok : 0), (wt ? ws / wt : 0)
	printf "#   energy_mWh=%.2f over %d s of valid samples\n", ws / 3600, wt
	if (c0 != "") printf "#   charge_counter_delta_uAh=%d\n", c1 - c0
	printf "#   capacity_start=%s capacity_end=%s\n", cap0, cap1
	printf "#   run_id=%s window_start=%d window_end=%d\n", run_id, start, stop
	printf "#   invalid_reasons=%s\n", reasons
	printf "#   valid=%s\n", (reasons == "none" ? "yes" : "no")
}')
# After any output failure the summary must not claim a valid run, on stdout or in the log: it is rewritten before it is
# written, and the log is written first so a failure there still downgrades what stdout shows.
downgrade() {
	printf '%s\n' "$1" | sed -e 's/^#   valid=yes$/#   valid=no/' -e 's/^#   invalid_reasons=none$/#   invalid_reasons=output-error/'
}
final=$summary
[ "$outfail" = 0 ] || final=$(downgrade "$summary")
if [ -n "$OUT" ]; then
	{ printf '%s\n' "$final" >> "$OUT"; } 2>/dev/null || { outfail=1; final=$(downgrade "$summary"); }
fi
{ printf '%s\n' "$final"; } 2>/dev/null || outfail=1
if [ "$outfail" != 0 ]; then
	# best effort: say so in both places; the log's reader (rpc-run-join.sh) rejects a log with this line
	if [ -n "$OUT" ]; then { printf '%s\n' "# valid=no reasons=output-error" >> "$OUT"; } 2>/dev/null || :; fi
	{ printf '%s\n' "# valid=no reasons=output-error"; } 2>/dev/null || :
	echo "rpc-power-sample: output error: the log is incomplete, the run is not valid" >&2
	exit 3
fi
case "$summary" in *"valid=yes"*) ;; *) exit 3 ;; esac
