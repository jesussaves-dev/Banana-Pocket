#!/bin/sh
# What the handheld is doing under an emulator, one line a second, so a game can be compared across builds, settings
# or operating systems on the same SoC (Banana Pocket/Linux and the stock Android). It logs only things both expose --
# each thermal zone, the fan, every CPU policy's clock, every devfreq node's clock, the load, memory and the
# emulator's own CPU share -- from /sys and /proc, so it runs unchanged in a Linux shell, over ssh, or from
# `adb shell` on Android. A drawn FPS counter is not logged, so read it alongside the log.
#
#   scp tools/rpc-emu-bench.sh root@<handheld>:/data/          # or: adb push tools/rpc-emu-bench.sh /data/local/tmp/
#   ssh root@<handheld> RPC_EMU=<name> sh /data/rpc-emu-bench.sh 300 > bench-linux.txt
#   adb shell RPC_EMU=<name> sh /data/local/tmp/rpc-emu-bench.sh 300 > bench-android.txt
#
# Collection protocol (docs/RPC-TEST.md "Matched benchmark runs"). The emulator is ALREADY RUNNING when this starts:
#   1. choose one run id, RPC_RUN_ID=<1-64 of A-Za-z0-9._->, and give the same one to the power sampler and to the
#      native process (BANANA_RETRO_RUN_ID, with BANANA_RETRO_TELEMETRY=1 and its stderr in the telemetry log);
#   2. the measurement window is `secs`. The matched process, pid and start time, is locked from the first sample and
#      must be the same and alone in every sample and at the window's end: no process at the start, an exit, a
#      replacement or an extra match inside the window all reject the run;
#   3. with RPC_TELEMETRY_LOG, after the window the script waits up to RPC_COLLECT_SECS (default 10) for that exact
#      process to exit by itself. The native process writes "Telemetry run: pid= starttime= run_id=" and its
#      statistics as playback ends, so stop the game after the window closes, not before. The report must carry the
#      locked pid, start time and run id, appear exactly once, and be newer than this script's start.
# Rejected (valid=no, exit 3): no process at the start, premature exit, a replaced or extra matching process,
# differently named matches, a process still alive after the collection window, a missing, stale, unbound,
# duplicated or foreign report, any output error, a summary that is missing or disagrees with the samples. Bad
# arguments or an unwritable log exit 2. A secs of 0 runs until interrupted and prints no verdict.
# Also rejected: a clock that steps backwards, a window shorter than `secs`, a gap between samples (or from the window
# edges to the nearest sample) over RPC_MAX_GAP seconds, and fewer than one sample per two seconds of window. All times
# are whole epoch seconds from `date +%s`, which truncates: each boundary is up to 1 s off, so durations carry +-1 s
# and the gap limit is deliberately looser than the nominal 1 s cadence.
#
# The log (RPC_OUT, else the first writable temp spot; never a silent /dev/null) holds everything printed: the run
# header with the run id, each matched process (pid, comm, start time, exe, exe sha256), the per-second lines, the
# summary, the measurement window (start, end, collection end, epoch seconds), the telemetry lines and the verdict.
# tools/rpc-run-join.sh pairs it with the power log of the same run id.
#
#   RPC_RUN_ID=<id>  RPC_COLLECT_SECS=<n>  RPC_TELEMETRY_LOG=<file>  RPC_LABEL=<text>  RPC_CORE=<.so>  RPC_ROM=<file>
#   RPC_SYS=<dir> RPC_PROC=<dir> sh ...    a mock tree (the host test); RPC_OUT names the log file
#   RPC_MAX_GAP=<n>        longest allowed gap in whole seconds between samples (and window edges), default 3
#   RPC_CLOCK=<script>     the host test's deterministic clock: `sh script` prints the epoch, `sh script sleep N`
#                          advances it. Never set on the unit.
SYS=${RPC_SYS:-/sys}
PROC=${RPC_PROC:-/proc}
EMU=${RPC_EMU:-retroarch}
secs=${1:-300}
case "$secs" in ''|*[!0-9]*) echo "rpc-emu-bench: seconds must be a non-negative integer, got '$secs'" >&2; exit 2 ;; esac
collect=${RPC_COLLECT_SECS:-10}
case "$collect" in ''|*[!0-9]*) echo "rpc-emu-bench: RPC_COLLECT_SECS must be a non-negative integer, got '$collect'" >&2; exit 2 ;; esac
RUN_ID=${RPC_RUN_ID:-none}
case "$RUN_ID" in *[!A-Za-z0-9._-]*|'') echo "rpc-emu-bench: RPC_RUN_ID must be 1-64 of A-Za-z0-9._-, got '$RUN_ID'" >&2; exit 2 ;; esac
[ ${#RUN_ID} -le 64 ] || { echo "rpc-emu-bench: RPC_RUN_ID is longer than 64" >&2; exit 2; }
if [ -n "$RPC_TELEMETRY_LOG" ] && [ "$RUN_ID" = none ]; then
	echo "rpc-emu-bench: RPC_TELEMETRY_LOG needs a unique RPC_RUN_ID shared with the native process" >&2; exit 2
fi
maxgap_limit=${RPC_MAX_GAP:-3}
case "$maxgap_limit" in ''|*[!0-9]*|0) echo "rpc-emu-bench: RPC_MAX_GAP must be a positive integer, got '$maxgap_limit'" >&2; exit 2 ;; esac
clock() { if [ -n "$RPC_CLOCK" ]; then sh "$RPC_CLOCK"; else date +%s; fi; }
nap() { if [ -n "$RPC_CLOCK" ]; then sh "$RPC_CLOCK" sleep "$1"; else sleep "$1"; fi; }

hz=$(getconf CLK_TCK 2>/dev/null); [ -n "$hz" ] || hz=100
# the log file: RPC_OUT if set, else the first writable of the usual spots (Linux /tmp; Android has no /tmp but keeps
# /data/local/tmp). A log that cannot be written is an error: a verdict without its evidence is not a verdict.
if [ -n "$RPC_OUT" ]; then OUT=$RPC_OUT
else
	OUT=
	for d in "$TMPDIR" /data/local/tmp /tmp; do
		[ -n "$d" ] && [ -d "$d" ] && [ -w "$d" ] && { OUT=$d/rpc-emu-bench.$$; break; }
	done
	[ -n "$OUT" ] || { echo "rpc-emu-bench: no writable place for the log (set RPC_OUT)" >&2; exit 2; }
fi
( : > "$OUT" ) 2>/dev/null || { echo "rpc-emu-bench: cannot write $OUT" >&2; exit 2; }
# every line goes to stdout and the log; a failed write on either is remembered (stickily) and fails the run
outfail=0 nsamp=0
emit() {
	printf '%s\n' "$1" || outfail=1
	{ printf '%s\n' "$1" >> "$OUT"; } 2>/dev/null || outfail=1
	[ "$outfail" = 0 ]
}

# the watched processes: their command name carries $EMU (the kernel truncates it, so match a part), but skip the
# launcher shells -- a `run-<name>` wrapper forks the real binary, and the script idles at 0%.
# Everything here is shell builtins (comm is one line; no awk/cut per pid), because Android has ~600 processes and
# forking per pid made a sample take ~17s instead of one second.
emu_pids() {
	for c in "$PROC"/[0-9]*/comm; do
		[ -r "$c" ] || continue
		read -r n < "$c" 2>/dev/null || continue
		case "$n" in *"$EMU"*) ;; *) continue ;; esac
		case "$n" in run-*|sh|bash|dash) continue ;; esac
		p=${c#"$PROC"/}; p=${p%/comm}
		# an exited but unreaped (zombie) process is not running: it is not part of the watched set
		if read -r l < "$PROC/$p/stat" 2>/dev/null; then
			l=${l##*)}; set -- $l
			case "$1" in Z|X) continue ;; esac
		fi
		echo "$p"
	done
}
# their combined CPU time in ticks (utime + stime); the comm can hold spaces, so drop everything through the last ')'
emu_ticks() {
	t=0
	for p in $1; do
		[ -r "$PROC/$p/stat" ] || continue
		read -r line < "$PROC/$p/stat" 2>/dev/null || continue
		line=${line##*)}
		set -- $line
		t=$((t + ${12:-0} + ${13:-0}))
	done
	printf '%d' "$t"
}
# pid:starttime of each, which tells a reused pid or a relaunch from the process first seen (stat field 22)
emu_ident() {
	for p in $1; do
		[ -r "$PROC/$p/stat" ] || continue
		read -r line < "$PROC/$p/stat" 2>/dev/null || continue
		line=${line##*)}
		set -- $line
		printf '%s:%s,' "$p" "${20:-?}"
	done
}

# Run identity for matched comparisons (same ROM, same scene, same duration; only the runtime differs). All optional:
#   RPC_LABEL=<free text, e.g. retroarch-mgba or native-mgba>  RPC_CORE=<core .so>  RPC_ROM=<rom file>
#   RPC_TELEMETRY_LOG=<banana-retro stderr log run with BANANA_RETRO_TELEMETRY=1>
# The telemetry log is used only under the collection protocol in the header: a report of the locked process's pid,
# start time and RPC_RUN_ID, newer than this script. RetroArch writes no such report, so its pacing comes from the
# telemetry-free measures plus a capture; a drawn FPS counter is not pacing evidence.
sha_of() {
	[ -n "$1" ] && [ -r "$1" ] || { printf 'n/a'; return; }
	h=$(sha256sum "$1" 2>/dev/null) || h=
	h=${h%% *}
	printf '%s' "${h:-n/a}"
}
base_or() { b=${1##*/}; printf '%s' "${b:-$2}"; }
start=$(clock)
emit "# run label=${RPC_LABEL:-none} emu=$EMU secs=$secs core=$(base_or "$RPC_CORE" none) core_sha256=$(sha_of "$RPC_CORE") rom=$(base_or "$RPC_ROM" n/a) rom_sha256=$(sha_of "$RPC_ROM") kernel=$(uname -r 2>/dev/null) started=$start run_id=$RUN_ID"
emit '# epoch <zone>=<C> ... fan=<pwm>/<rpm> <policy>=<GHz> ... <devfreq>=<MHz> ... load1=<L> mem=<avail>/<total>MB emu=<pid>:<cpu%>'

locked= lock_pids= comms= ncomm=0 changed=0 missing=0 first=1
# the first matched set is the run's process: record exactly what it is
record_identity() {
	for p in $1; do
		read -r c < "$PROC/$p/comm" 2>/dev/null || c=n/a
		exe=$(readlink "$PROC/$p/exe" 2>/dev/null)
		read -r l < "$PROC/$p/stat" 2>/dev/null || l=
		l=${l##*)}; set -- $l
		emit "# process pid=$p comm=$c starttime=${20:-n/a} exe=${exe:-n/a} exe_sha256=$(sha_of "$PROC/$p/exe")"
		case " $comms " in *" $c "*) ;; *) comms="$comms $c"; ncomm=$((ncomm + 1)) ;; esac
	done
}


# is a locked pid:starttime still that very process (a reused pid has another start time)
locked_alive() {
	for e in $(echo "$locked" | tr ',' ' '); do
		lp=${e%%:*}; ls=${e#*:}
		read -r l < "$PROC/$lp/stat" 2>/dev/null || continue
		l=${l##*)}; set -- $l
		# a zombie (Z) or dead (X) process has exited, whatever its start time still says
		case "$1" in Z|X) continue ;; esac
		[ "${20:-?}" = "$ls" ] && return 0
	done
	return 1
}

end=0 maxgap=0 clockback=0 last_t=$start
[ "$secs" -gt 0 ] && end=$((start + secs))
prev_ticks=
# the identity check, once per sample and once more when the window closes
check_identity() {
	ids=$(emu_ident "$1")
	if [ -n "$1" ]; then
		if [ -n "$locked" ]; then [ "$ids" = "$locked" ] || changed=1
		elif [ -n "$first" ]; then locked=$ids; lock_pids=$(echo $1); record_identity "$1"
		fi
	elif [ -n "$locked" ]; then
		missing=1
	fi
}
while :; do
	now=$(clock)
	[ "$end" -gt 0 ] && [ "$now" -ge "$end" ] && break
	pids=$(emu_pids); pid=$(printf '%s\n' "$pids" | head -n1); t=$(emu_ticks "$pids")
	check_identity "$pids"
	first=
	cpu=""
	dt=1; [ -n "$prev_now" ] && dt=$((now - prev_now)); [ "$dt" -gt 0 ] || dt=1
	[ -n "$prev_ticks" ] && [ -n "$t" ] && cpu=$(( (t - prev_ticks) * 100 / (hz * dt) ))
	line="$now"
	for z in "$SYS"/class/thermal/thermal_zone*; do
		read -r v < "$z/temp" 2>/dev/null || continue
		[ -n "$v" ] || continue
		read -r ty < "$z/type" 2>/dev/null
		case "$v" in -*) s=-; v=${v#-} ;; *) s= ;; esac
		line="$line $ty=${s}$((v/1000)).$(( (v%1000)/100 ))"
	done
	line="$line fan=$(cat "$SYS"/class/hwmon/hwmon*/pwm1 2>/dev/null | head -n1)/$(cat "$SYS"/class/hwmon/hwmon*/fan1_input 2>/dev/null | head -n1)"
	for p in "$SYS"/devices/system/cpu/cpufreq/policy*; do
		[ -r "$p/scaling_cur_freq" ] || continue
		line="$line ${p##*/}=$(awk -v v="$(cat "$p/scaling_cur_freq")" 'BEGIN{printf "%.2f", v/1000000}')"
	done
	for d in "$SYS"/class/devfreq/*; do
		[ -r "$d/cur_freq" ] || continue
		line="$line ${d##*/}=$(awk -v v="$(cat "$d/cur_freq")" 'BEGIN{printf "%.0f", v/1000000}')"
	done
	line="$line load1=$(cut -d' ' -f1 "$PROC/loadavg" 2>/dev/null) mem=$(awk '/MemAvailable/{printf "%d",$2/1024}' "$PROC/meminfo" 2>/dev/null)/$(awk '/MemTotal/{printf "%d",$2/1024}' "$PROC/meminfo" 2>/dev/null) emu=${pid:-none}:${cpu:-0}"
	emit "$line" && nsamp=$((nsamp + 1))
	prev_ticks=$t; prev_now=$now
	gap=$((now - last_t)); [ "$gap" -ge 0 ] || clockback=1
	[ "$gap" -le "$maxgap" ] || maxgap=$gap
	last_t=$now
	nap 1
done
win_end=$(clock)
gap=$((win_end - last_t)); [ "$gap" -ge 0 ] || clockback=1
[ "$gap" -le "$maxgap" ] || maxgap=$gap
check_identity "$(emu_pids)"

# the bounded collection wait: the exact locked process must exit by itself and leave its report
reasons=
if [ -n "$RPC_TELEMETRY_LOG" ] && [ -n "$locked" ]; then
	waited=0
	while locked_alive && [ "$waited" -lt "$collect" ]; do nap 1; waited=$((waited + 1)); done
	if locked_alive; then reasons="$reasons process-not-exited"; fi
fi
collect_end=$(clock)
emit "# window run_id=$RUN_ID start=$start end=$win_end collect_end=$collect_end samples=$nsamp max_gap=$maxgap" || :

# Only a regular file is summarised, and at most 16 MiB of it: RPC_OUT can name a device (/dev/full reads as an endless
# stream of NULs with no newline, one record that awk would buffer until the host is out of memory). Anything else
# gives an empty summary, which the checks below reject.
summary=$(if [ -f "$OUT" ]; then head -c 16777216 "$OUT" 2>/dev/null; fi | awk '
/^#/ { next }
{
	n++
	for (i = 2; i <= NF; i++) {
		split($i, a, "="); k0 = a[1]; v = a[2]
		if (k0 == "fan") { split(v, b, "/"); if (b[1] + 0 > fp) fp = b[1] + 0; continue }
		if (k0 == "load1") { l += v; nl++; continue }
		if (k0 == "mem") continue
		if (k0 == "emu") { split(v, b, ":"); c = b[2] + 0; cu += c; nc++; if (c > cp) cp = c; continue }
		if (v ~ /^[0-9.]+$/) { s[k0] += v; k[k0]++; if (v + 0 > pk[k0]) pk[k0] = v + 0 }
	}
}
END {
	printf "# SUMMARY  samples=%d\n", n
	for (k0 in s) printf "#   %-20s avg=%.1f peak=%.1f\n", k0, s[k0]/k[k0], pk[k0]
	printf "#   %-20s peak=%d\n", "fan pwm", fp
	printf "#   %-20s avg=%.2f\n", "load1", (nl ? l/nl : 0)
	printf "#   %-20s avg=%.0f%% peak=%.0f%%\n", "emulator cpu", (nc ? cu/nc : 0), cp
}' 2>/dev/null)
emit "$summary" || :
printf '# log: %s\n' "$OUT" || outfail=1

[ -n "$locked" ] || reasons="$reasons no-process-matched"
[ "$ncomm" -le 1 ] || reasons="$reasons ambiguous-process"
[ "$changed" = 0 ] || reasons="$reasons process-changed"
[ "$missing" = 0 ] || reasons="$reasons premature-exit"
case "$locked" in *:\?,*) reasons="$reasons identity-unreadable" ;; esac
[ "$nsamp" -gt 0 ] || reasons="$reasons no-samples"
# Sample coverage. The clock is whole seconds (`date +%s` truncates), so a boundary can read up to 1 s off either way:
# a measured gap d means a true gap in (d-1, d+1) s, and the window is at least secs-1 s of real time. A nominal 1 s
# cadence shows as gaps of 1-2 s (3 s when the shell is slow); RPC_MAX_GAP (default 3, i.e. a true gap under 4 s)
# bounds the longest one, from the window start to the first sample, between samples, and from the last sample to the
# window end. At least one sample per two seconds of window is required (nsamp*2 >= secs), so a loaded or stalled
# sampler gives no verdict. Both limits are in whole seconds, not statistics about CPU load.
[ "$clockback" = 0 ] || reasons="$reasons clock-backwards"
[ "$maxgap" -le "$maxgap_limit" ] || reasons="$reasons sample-gap"
[ $((nsamp * 2)) -ge "$secs" ] || reasons="$reasons sparse-samples"
[ $((win_end - start)) -ge "$secs" ] || reasons="$reasons short-window"
# the summary must exist and count exactly the samples that were taken, on stdout and in the log
sumn=$(printf '%s\n' "$summary" | sed -n 's/^# SUMMARY  samples=\([0-9][0-9]*\)$/\1/p')
[ "$sumn" = "$nsamp" ] || reasons="$reasons summary-missing-or-mismatched"
[ -f "$OUT" ] && head -c 16777216 "$OUT" 2>/dev/null | grep -q "^# SUMMARY  samples=$nsamp\$" || reasons="$reasons summary-not-in-log"
if [ -n "$RPC_TELEMETRY_LOG" ]; then
	emit "# telemetry_log=$RPC_TELEMETRY_LOG sha256=$(sha_of "$RPC_TELEMETRY_LOG")" || :
	set -- $lock_pids
	[ -z "$locked" ] || [ $# -eq 1 ] || reasons="$reasons ambiguous-process"
	if [ ! -r "$RPC_TELEMETRY_LOG" ]; then
		reasons="$reasons telemetry-unreadable"
	elif [ -n "$locked" ] && [ $# -eq 1 ]; then
		mtime=$(stat -c %Y "$RPC_TELEMETRY_LOG" 2>/dev/null)
		case "$mtime" in ''|*[!0-9]*) reasons="$reasons telemetry-mtime-unknown" ;;
		*) [ "$mtime" -ge "$start" ] || reasons="$reasons telemetry-stale" ;; esac
		lock_start=${locked#*:}; lock_start=${lock_start%,}
		# line 1: run lines of exactly this pid+starttime+run id, frame lines inside them, and other-identity run
		# lines that claim this run id; then the matching run's lines
		block=$(awk -v pid="$1" -v st="$lock_start" -v id="$RUN_ID" '
			BEGIN { want = "Telemetry run: pid=" pid " starttime=" st " run_id=" id }
			/^Telemetry run: / {
				cur = ($0 == want)
				if (cur) { nm++; out = out $0 "\n" }
				else { split($0, f, " run_id="); if (f[2] == id) foreign++ }
				next }
			cur && /^Frame telemetry:/ { nf++; out = out $0 "\n"; next }
			cur && /^Audio status:/ { out = out $0 "\n" }
			END { printf "%d %d %d\n%s", nm, nf, foreign, out }' "$RPC_TELEMETRY_LOG" 2>/dev/null)
		counts=${block%%
*}
		set -- $counts
		nm=${1:-0}; nf=${2:-0}; foreign=${3:-0}
		[ "$nm" -ge 1 ] || reasons="$reasons telemetry-no-report-for-run"
		[ "$nm" -le 1 ] || reasons="$reasons telemetry-ambiguous"
		[ "$foreign" -eq 0 ] || reasons="$reasons telemetry-identity-conflict"
		if [ "$nm" -eq 1 ]; then
			[ "$nf" -ge 1 ] || reasons="$reasons telemetry-no-frame-line"
			[ "$nf" -le 1 ] || reasons="$reasons telemetry-ambiguous"
			if [ "$nf" -eq 1 ] && [ "$foreign" -eq 0 ]; then
				while IFS= read -r l; do emit "# $l" || :; done <<EOF
${block#*
}
EOF
			fi
		fi
	fi
fi
[ "$outfail" = 0 ] || reasons="$reasons output-error"
if [ -n "$reasons" ]; then
	emit "# valid=no reasons=${reasons# }" || :
	exit 3
fi
[ "$secs" -eq 0 ] && exit 0
emit "# valid=yes" || exit 3
