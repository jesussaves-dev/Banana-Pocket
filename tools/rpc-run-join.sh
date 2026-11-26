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
# The power log must also say which measurement source its figure came from (vi or gauge: power_src in the summary,
# consistent with the requested mode and with every sample's src); a log with no source, a mixed or no-source run, or
# an inconsistent record is power-src-missing / power-src-unusable / power-src-inconsistent. The bench log (the
# emulator collector) measures no power and states no source. To compare two joined runs (say RetroArch against
# native) in energy, check that their power logs used the same source first:
#
#   sh tools/rpc-run-join.sh --compare-power power-a.log power-b.log
#
# valid=yes only when both logs are valid, carry a consistent single source, and it is the same one (otherwise
# power-src-mismatch, exit 3). The two figures are never comparable across sources.
# Reject malformed power evidence before either join mode trusts its summary.
power_evidence() {
	awk -v expected="$2" '
	function num(x) { return x ~ /^[0-9]+([.][0-9]+)?$/ }
	function intg(x) { return x ~ /^[0-9]+$/ }
	/^# run id=/ { h++; for(i=3;i<=NF;i++){if(split($i,a,"=")!=2 || a[1] in head)bad=1;head[a[1]]=a[2]} hid=head["id"]; req=head["power_src_requested"] }
	/^# epoch status=/ { eh++ }
	/^[0-9]/ {
		n++; delete f
		for(i=2;i<=NF;i++){if(split($i,a,"=")!=2 || a[1] in f)bad=1;f[a[1]]=a[2]}
		if(!intg($1)||f["status"]!="Discharging"||f["flags"]!="ok"||!intg(f["v_mV"])||!intg(f["p_mW"])||f["p_mW"]<1||f["p_mW"]>20000||f["v_mV"]<2500||f["v_mV"]>5000||f["online"]!="none")bad=1
		if(!("cap" in f)||!("i_mA" in f)||!("counter_uAh" in f)||!("gauge_mW" in f)||!("warn" in f))bad=1
		if(f["src"]=="vi"&&!intg(f["i_mA"]))bad=1
		if(f["src"]=="gauge"&&!intg(f["gauge_mW"]))bad=1
		if(f["src"]!="vi"&&f["src"]!="gauge")bad=1
		if(f["req"]!=req)bad=1
		t[n]=$1;p[n]=f["p_mW"];sum+=p[n];if(n==1)first=t[n];else if(t[n]<t[n-1] || t[n]-t[n-1]>2*head["interval"]+2)bad=1;last=t[n]
		if(f["src"]=="vi")vi++;else gauge++
	}
	/^# SUMMARY/ {s++;for(i=3;i<=NF;i++){if(split($i,a,"=")!=2 || a[1] in sm)bad=1;sm[a[1]]=a[2]}}
	/^#   avg_mW=/ {avg++; delete avf; for(i=2;i<=NF;i++){if(split($i,a,"=")!=2 || a[1] in avf)bad=1;avf[a[1]]=a[2]} av=avf["avg_mW"]; if(!num(avf["peak_mW"])||!num(avf["avg_mV"])||!num(avf["time_weighted_avg_mW"]))bad=1}
	/^#   energy_mWh=/ {energy++;split($2,a,"=");en=a[2]; if(NF!=8||$3!="over"||!intg($4)||$5!="s"||$6!="of"||$7!="valid"||$8!="samples")bad=1; energy_time=$4}
	/^#   power_src_requested=/ {src++;if(NF!=3||$2!~/^power_src_requested=/||$3!~/^power_src=/)bad=1;split($2,a,"=");mreq=a[2];split($3,a,"=");msrc=a[2]}
	/^#   run_id=/ {win++;if(NF!=4||$2!~/^run_id=/||$3!~/^window_start=/||$4!~/^window_end=/)bad=1;split($2,a,"=");wid=a[2];split($3,a,"=");ws=a[2];split($4,a,"=");we=a[2]}
	/^#   valid=yes$/ {valid++}
	/^#   valid=/ && $0 != "#   valid=yes" { bad=1 }
	/^# valid=no reasons=output-error$/ {out=1}
	END{
		if(h!=1||eh!=1||s!=1||src!=1||win!=1||avg!=1||energy!=1||valid!=1||out||n<2)bad=1
		if(hid==""||hid=="none"||wid!=hid||req!=mreq||!intg(ws)||!intg(we)||ws>=we||first<ws||last>=we||!intg(head["secs"])||!intg(head["interval"])||head["secs"]<1||head["interval"]<1||we-last>2*head["interval"]+2)bad=1
		if((msrc!="vi"&&msrc!="gauge")||(req!="default"&&req!=msrc)||(expected!=""&&expected!=msrc))bad=1
		if((msrc=="vi"&&gauge)||(msrc=="gauge"&&vi))bad=1
		if(!intg(sm["samples"])||!intg(sm["valid_samples"])||!intg(sm["invalid_samples"])||!intg(sm["elapsed_s"])||!intg(sm["requested_s"])||sm["samples"]!=n||sm["valid_samples"]!=n||sm["invalid_samples"]!=0||sm["elapsed_s"]!=we-ws||sm["requested_s"]!=head["secs"]||we-ws<head["secs"])bad=1
		if(!num(av)||av+0<sum/n-1||av+0>sum/n+1)bad=1
		actual=0;for(i=1;i<=n;i++)actual+=p[i]*((i<n?t[i+1]:we)-t[i])/3600
		if(!num(en)||energy_time!=we-first||en+0<actual-.02||en+0>actual+.02)bad=1
		if(bad)exit 1;print msrc
	}' "$1"
}
source_reason() {
	awk '
	/^#   power_src_requested=/ { for(i=2;i<=NF;i++){split($i,a,"=");if(a[1]=="power_src_requested")req=a[2];if(a[1]=="power_src")src=a[2]} }
	/^[0-9]/ { for(i=2;i<=NF;i++){split($i,a,"=");if(a[1]=="req")r=a[2];if(a[1]=="src")s=a[2]} if(r!="" && req!="" && r!=req)bad=1; if(s!="" && src!="" && s!=src)bad=1; rs[NR]=r; ss[NR]=s }
	END {
		if(req=="" || src=="")print "power-src-missing"
		else if(src!="vi" && src!="gauge")print "power-src-unusable"
		else { if(req!="default" && req!=src)bad=1; for(i in rs)if(rs[i]!=req || ss[i]!=src)bad=1; if(bad)print "power-src-inconsistent"; else print "power-src-unusable" }
	}' "$1"
}
if [ "${1:-}" = --compare-power ]; then
	[ $# -eq 3 ] || { echo "usage: rpc-run-join.sh --compare-power power-a.log power-b.log" >&2; exit 2; }
	[ -r "$2" ] && [ -r "$3" ] || { echo "rpc-run-join: cannot read $2 or $3" >&2; exit 2; }
	reasons=
	for pair in "a-:$2" "b-:$3"; do
		px=${pair%%:*} pf=${pair#*:}
		grep -q '^#   valid=yes$' "$pf" || reasons="$reasons ${px}power-invalid"
		grep -q '^# valid=no reasons=output-error$' "$pf" && reasons="$reasons ${px}power-output-error"
		psrc=$(power_evidence "$pf" 2>/dev/null) || { reasons="$reasons ${px}$(source_reason "$pf")"; psrc=; }
		eval "src_${px%-}=\$psrc"
	done
	[ -z "$src_a" ] || [ -z "$src_b" ] || [ "$src_a" = "$src_b" ] || reasons="$reasons power-src-mismatch"
	if [ -n "$reasons" ]; then echo "# power-compare valid=no reasons=${reasons# }"; exit 3; fi
	echo "# power-compare power_src=$src_a valid=yes"
	exit 0
fi
[ $# -eq 2 ] || { echo "usage: rpc-run-join.sh bench.log power.log | --compare-power power-a.log power-b.log" >&2; exit 2; }
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
# power evidence includes the single metadata window and all samples.
	grep -q '^#   valid=yes$' "$power" || reasons="$reasons power-invalid"
	grep -q '^# valid=no reasons=output-error$' "$power" && reasons="$reasons power-output-error"
psrc=$(power_evidence "$power" 2>/dev/null) || reasons="$reasons $(source_reason "$power")"
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
echo "# joined run_id=$bid bench_window=$bs-$be power_window=$ps_-$pe_ power_src=$psrc valid=yes"
grep -E '^#   (avg_mW|energy_mWh|elapsed_s|power_src_requested|gauge_disagreement_samples|counter_energy_mWh)' "$power" 2>/dev/null || :
