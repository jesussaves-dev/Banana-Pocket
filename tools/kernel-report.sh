#!/bin/sh
# Record what the handheld is actually running, so two kernels can be compared with diff. Runs on this computer and
# collects everything over the USB link (ssh + cat; the handheld's dropbear has no scp). Read-only on the device: no
# module loads, no sysfs writes.
#
#   tools/kernel-report.sh [out-file]
#
# The default out-file is docs/reports/kernel-report-<version>-<slot>-<YYYY-MM-DD>.txt. KEEP_CONFIG=1 also saves the
# full kernel config next to it as <out-file>.config (off by default: each tag rebuilds it from linux-banana.fragment,
# so a committed dump is ~7k lines of bulk; the counts below are always reported). Sections: identity, image, modules, boot marks, config counts, probed drivers.
#
# Host: $BANANA_HOST, else root@169.254.170.2 (the USB gadget, same as tools/rpc-push-update.sh). $SSH_OPTS overrides
# the ssh options. Over Wi-Fi instead: BANANA_HOST=root@<the handheld's address> tools/kernel-report.sh
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
HOST=${BANANA_HOST:-root@169.254.170.2}
SSH_OPTS=${SSH_OPTS:-"-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15"}

run() { ssh $SSH_OPTS "$HOST" "$@"; }

if ! run true 2>/dev/null; then
    echo "no link to $HOST. Plug the handheld into this computer with a data cable (not charge-only) and turn ssh on" >&2
    echo "(Settings > Network). Over Wi-Fi: BANANA_HOST=root@<address> $0" >&2
    exit 1
fi

VER=$(run 'cat /etc/banana/version 2>/dev/null || echo unknown')
SLOT=$(run 'cat /run/banana/slot 2>/dev/null || echo unknown')
KREL=$(run 'uname -r')
OUT=${1:-$HERE/docs/reports/kernel-report-$VER-$SLOT-$(date +%F).txt}
if [ "${KEEP_CONFIG:-0}" = 1 ]; then
    CONF=$OUT.config
else
    CONF=$(mktemp) || exit 1
    trap 'rm -f "$CONF"' EXIT
fi
mkdir -p "$(dirname "$OUT")"

{
    echo "# Banana kernel report"
    echo "# host: $HOST   collected: $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo
    echo "== identity"
    echo "version: $VER"
    echo "slot: $SLOT"
    echo "uname: $KREL"
    echo "cmdline: $(run 'cat /proc/cmdline')"
    echo
    echo "== image"
    # The kernel and its dtb of the running slot; the loader on p1 picks /boot/<slot> for that label.
    run "cd /boot/$SLOT 2>/dev/null && ls -l Image *.dtb 2>/dev/null | awk '{printf \"%s %s bytes\\n\", \$9, \$5}'" ||
        echo "(no /boot/$SLOT)"
    echo
    echo "== modules"
    # *.ko* is every module file (compressed or not); du -b is the apparent size, the same number ls -l prints.
    run "find /lib/modules/$KREL -name '*.ko*' -type f -exec du -cb {} + |
         awk '\$2 != \"total\" { n++; s += \$1 } END { printf \"%d files, %d bytes\\n\", n, s }'"
    echo "lsmod:"
    run 'lsmod' || echo "(lsmod failed)"
    echo
    echo "== boot"
    echo "rcS marks (/run/banana/boot-times):"
    run 'cat /run/banana/boot-times 2>/dev/null' || echo "(no boot-times)"
    # dmesg-saved is rcS's copy of the boot log; the live ring buffer may have wrapped since.
    echo "init: $(run 'grep -m1 "Run /sbin/init" /run/banana/dmesg-saved 2>/dev/null ||
                      dmesg 2>/dev/null | grep -m1 "Run /sbin/init" || echo "(not found)"')"
    echo
    echo "== config"
    if run 'test -s /proc/config.gz'; then
        run 'zcat /proc/config.gz' > "$CONF"
        [ "${KEEP_CONFIG:-0}" = 1 ] && echo "file: ${CONF##*/}"
        # Anchored to a bare value so a string ("...=y") cannot match; symbols may be mixed case (CONFIG_ARM_CCI5xx_PMU).
        echo "=y: $(grep -c '^CONFIG_[A-Za-z0-9_]*=y$' "$CONF")"
        echo "=m: $(grep -c '^CONFIG_[A-Za-z0-9_]*=m$' "$CONF")"
    else
        echo "(no /proc/config.gz: CONFIG_IKCONFIG_PROC is off, or the kernel booted without it)"
    fi
    echo
    echo "== probed drivers"
    # Only drivers with at least one bound device: the symlinks next to bind/unbind/uevent are the devices. Built-in
    # driver dirs still carry a "module" symlink, so skip that one or every built-in counts as probed.
    run 'for d in /sys/bus/*/drivers/*/; do
             [ -d "$d" ] || continue
             n=$(ls -l "$d" | grep "^l" | grep -vc " module -> ")
             [ "$n" -gt 0 ] || continue
             bus=${d#/sys/bus/}; bus=${bus%%/*}
             drv=${d%/}; drv=${drv##*/}
             printf "%s/%s: %s\n" "$bus" "$drv" "$n"
         done | sort'
} > "$OUT"

echo "report: $OUT"
