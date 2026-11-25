---
name: banana-retro-lifecycle
description: Use for native banana-retro launch/exit, overlay and sleep hold behavior, checkpoints, saves, or quick resume. Not for Buildroot packaging, system wake, scanout, or authorization-only review.
---

# Banana-retro lifecycle

1. **Scope.** Read the matching [roadmap](../../../docs/plans/banana-retro-roadmap.md), [runtime architecture](../../../docs/plans/banana-retro-architecture.md), [AGENTS.md](../../../AGENTS.md), and [engineering rules](../../../docs/ENGINEERING-RULES.md). Trace the affected launcher, UI, runtime, and daemon. Roadmap scope is not approval.
2. **Control.** Read the [wire contract](../../../system/bananad/include/banana-retro-control.h), [control](../../../system/banana-retro/src/control.c), and [loop](../../../system/banana-retro/src/main.c). Preserve version/session checks, authenticated roles, independent overlay/sleep holds, and core safe points. Check stale epochs, abort, timeout, and exit. Keep paused Wayland dispatch/releases and bounded worker PCM. UI owns display; bananad owns power. Preserve stock RetroArch. Automatic fallback only before readiness and before any native save or state commit.
3. **Saves.** Trace [saves.c](../../../system/banana-retro/src/saves.c). Check identity, compatibility, interrupted writes, and last-good generation recovery. Resume only from a validated checkpoint. Preview and confirm legacy SRAM/RTC import; reject stale previews, preserve originals, and do not import legacy state slots.
4. **Checks.** Inspect the [Makefile](../../../system/banana-retro/Makefile). For authorized host runs, use `check-control`, `check-saves`, or the relevant video/audio fixture with pre-created scratch `OUT`. Check affected [launcher](../../../system/tests/run-libretro-test.sh), [power](../../../system/bananad/tests/retro-lifecycle-test.sh), or [UI import](../../../ui/tests/legacy-import-test.sh) callers. Use focused ASan/UBSan as needed. Report commands, statuses, and limits, including any confirmed LeakSanitizer restriction.
5. **Gates.** Host checks do not prove exFAT durability or device behavior. QEMU needs explicit approval and cannot satisfy the physical RPC pilot gate. Get separate approval for hardware, suspend/power, deployment, save migration, and profile promotion. Do not advance the roadmap without approval.
