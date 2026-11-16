# Kernel task board

Claim a task by filling in **Agent** and **Branch** and setting **Status** to `claimed` before starting.
Never pick up a claimed task. Plan and rules: `docs/PLAN_CUSTOM_KERNEL.md` (Versioning; Working with two agents).

Status: `open` · `claimed` · `in review` · `done`

| ID | Milestone | Task | Owned files | Depends on | Agent | Branch | Status |
|---|---|---|---|---|---|---|---|
| K0.1 | 0.1 | Test-boot fallback: `update stage` + OpenABL Boot entries one-shot pick | `libexec/banana/update`, `tools/rpc-push-update.sh`, plan §K0 | — | Claude | kernel-k0-boot | done |
| K0.2 | 0.1 | `tools/kernel-report.sh`: Image size, modules, dmesg time-to-init, rcS marks, lsmod, config.gz, probed drivers | `tools/kernel-report.sh` | — | DeepSeek | kernel-k0-report | done |
| K0.3 | 0.1 | Baseline report from the 0.8.0 kernel (owner runs on device) | `docs/reports/kernel-baseline-*.txt` | K0.2 | owner (run by Claude) | kernel | done |
| K8.1 | 0.2 | `KERNEL_VERSION` file + `CONFIG_LOCALVERSION` wired from it | `board/.../KERNEL_VERSION`, `linux-banana.fragment` | — | DeepSeek | kernel-k8-version | in review |
| K8.2 | 0.2 | Image carries only the running kernel's modules: post-build removes every `/lib/modules/*` not named after `kernel.release` (an incremental build after a version bump leaves the old dir) | `board/.../post-build.sh` | K8.1 | | | open |
| K1.1 | 0.2 | Driver inventory: what probes + what rcS modprobes, from the baseline | `docs/KERNEL-CONFIG.md` | K0.3 | Claude | kernel-k1-inventory | done |
| K1.2 | 0.2 | `linux-banana.config` from `allnoconfig`, grouped and explained | `board/.../linux-banana.config`, defconfig | K1.1 | | | open |
| K1.2a | 0.2 | Draft config from `allnoconfig`: deps resolved, builds | `board/.../linux-banana.config` | K1.1 | DeepSeek | kernel-k1-draft | in review |
