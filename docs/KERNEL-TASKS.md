# Kernel task board

Claim a task by filling in **Agent** and **Branch** and setting **Status** to `claimed` before starting.
Never pick up a claimed task. Plan and rules: `docs/PLAN_CUSTOM_KERNEL.md` (Versioning; Working with two agents).

Status: `open` · `claimed` · `in review` · `done`

| ID | Milestone | Task | Owned files | Depends on | Agent | Branch | Status |
|---|---|---|---|---|---|---|---|
| K0.1 | 0.1 | Test-boot fallback: `update stage` + OpenABL Boot entries one-shot pick | `libexec/banana/update`, `tools/rpc-push-update.sh`, plan §K0 | — | Claude | kernel-k0-boot | done |
| K0.2 | 0.1 | `tools/kernel-report.sh`: Image size, modules, dmesg time-to-init, rcS marks, lsmod, config.gz, probed drivers | `tools/kernel-report.sh` | — | DeepSeek | kernel-k0-report | done |
| K0.3 | 0.1 | Baseline report from the 0.8.0 kernel (owner runs on device) | `docs/reports/kernel-baseline-*.txt` | K0.2 | owner (run by Claude) | kernel | done |
| K8.1 | 0.2 | `KERNEL_VERSION` file + `CONFIG_LOCALVERSION` wired from it | `board/.../KERNEL_VERSION`, `linux-banana.fragment` | — | DeepSeek | kernel-k8-version | done (#11) |
| K8.2 | 0.2 | Image carries only the running kernel's modules: post-build removes every `/lib/modules/*` not named after `kernel.release` (an incremental build after a version bump leaves the old dir) | `board/.../post-build.sh` | K8.1 | DeepSeek | kernel-k8-modprune | done (#12) |
| K1.1 | 0.2 | Driver inventory: what probes + what rcS modprobes, from the baseline | `docs/KERNEL-CONFIG.md` | K0.3 | Claude | kernel-k1-inventory | done |
| K1.2 | 0.2 | `linux-banana.config` as a defconfig (Kconfig defaults + this board), grouped and explained; defconfig points at it | `board/.../linux-banana.config`, fragments, defconfig, `docs/KERNEL-CONFIG.md` §5 | K1.2a | Claude | kernel-k1-draft | done (#13, #15) |
| K1.2a | 0.2 | Draft config from `allnoconfig`: deps resolved, builds | `board/.../linux-banana.config` | K1.1 | DeepSeek | kernel-k1-draft | done (#13) |
| K1.2b | 0.2 | NLS codepage 437 + iso8859-1 for vfat `/boot` (the K1.2 config dropped them; updates broke in the QEMU test); rcS marks a failed `/boot` mount | `linux-banana.config`, `rcS`, `docs/KERNEL-CONFIG.md` | K1.2 | Claude | kernel-k1-nls | done (#15) |
| K1.2c | 0.2 | Defaults K1.2 lost, found on the device: PREEMPT + HIGH_RES_TIMERS + NO_HZ_IDLE (menu sound slow/crunchy), CPU_THERMAL/CPU_FREQ_THERMAL (no CPU cooling), PM_DEVFREQ + governors + DEVFREQ_THERMAL (no GPU devfreq) | `linux-banana.config`, `docs/KERNEL-CONFIG.md`, kernel report | K1.2b | Claude | kernel-k12-timers | done |
| K2 | 0.3 | Own the device tree: drop nodes for hardware the RPC lacks or Banana doesn't use (HDMI/DP, cameras, unused regulators); split `common.dtsi` by subsystem with header comments; Banana named maintainer, BSD-3-Clause kept | `board/.../dts/qcom/*` | K1.2c | Claude | kernel-k2-dts | done |
