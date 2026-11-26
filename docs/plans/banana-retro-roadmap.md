# Banana-retro implementation roadmap

**Status** — Proposed task queue. [Architecture and source evidence](banana-retro-architecture.md).

Each task is a separate Banana work item with the stated allowlist. Dependency IDs refer to verified changes available in the task's approved base. Authorize tasks individually. Run sanctioned local checks only under the applicable repository workflow. Hardware qualification, deployment and configuration promotion each require their own authorization. Verification changes outside a task's allowlist require a separate work item.

## 01. Build a host-only Libretro lifecycle fixture

- **Scope and files** — `system/banana-retro/Makefile`, `system/banana-retro/include/libretro.h`, `system/banana-retro/src/main.c`, `system/banana-retro/src/core.[ch]`, `system/banana-retro/tests/fixture_core.c`, `system/banana-retro/tests/check-core.sh`.
- **Dependencies and interface** — None. Pin and credit the MIT Libretro header. Define a CLI taking an explicit fixture core and content path with `--frames 2`. Define the internal load, step and release functions.
- **Sequence and completion** — Add required export resolution and API version validation, callback registration, init, system info, path or byte content loading, AV validation, frame execution and ordered cleanup. A synthetic fixture records the order and returns deterministic video, audio and input observations. Test two ticks, missing exports, wrong API version, rejected load, oversized/invalid content, early cleanup and a normal unload. Verify strict C99 `-Wall -Wextra -Werror` and an authorized `make -s -C system/banana-retro check`.
- **Complexity and rollback** — Small. ABI and callback lifetime are the main risks. Keep the fixture opt-in and local to its directory.

## 02. Establish native Wayland software presentation

- **Scope and files** — `system/banana-retro/protocol/banana-retro-v1.xml`, `system/banana-retro/Makefile`, `system/banana-retro/src/main.c`, `system/banana-retro/src/video_wayland.[ch]`, `system/banana-retro/tests/check-video.c`, `ui/Makefile`, `ui/src/{compositor.cpp,client_buffer.h,presenter.h,egl_presenter.cpp,egl_presenter.h}`.
- **Dependencies and interface** — 01. Fullscreen xdg-shell, XRGB8888 SHM, bounded reusable slots, presenter capability, geometry per commit, and session-scoped first-presentation event.
- **Sequence and completion** — Implement checked conversion and configure sequencing first. Add actual SHM capability and the native-only per-surface metadata. Confirm readiness after a real flip. Exercise RGB565, XRGB8888, 0RGB1555, padded pitch, null duplicates, resize/rotation/aspect, busy slots, unsupported SHM, failed KMS commit, release order, overlay and a lost surface. Verify local fixtures and authorized compositor build, then QEMU with the runtime as an explicit experimental client when a test package exists. Carry that QEMU gate into 06 if packaging is pending.
- **Complexity and rollback** — Large. Keep existing clients on their current protocol and renderer. If native presentation fails, disable the native capability and preserve RetroArch launch.

## 03. Add digital pad input and core-paced timing

- **Scope and files** — `system/banana-retro/src/{main.c,core.c,input_evdev.c,input_evdev.h}`, `system/banana-retro/tests/check-input.c`, `system/banana-retro/Makefile`.
- **Dependencies and interface** — 01. Digital RetroPad snapshot, evdev/inotify, per-profile device mapping, monotonic clock with bounded overrun behavior.
- **Sequence and completion** — Read the initial device state and handle discovery, hotplug, `SYN_DROPPED` and neutralization. Register pad and optional bitmask capabilities. Build a deadline loop with injectable clock/event inputs. Verify 50/59.94/60 Hz fixture cadence, pad unplug, held key release, idle blocking and independent presenter pressure when integrated with 02.
- **Complexity and rollback** — Medium. Fixture mapping and clock selection stay local to the host until native launch is wired.

## 04. Add BananaOS PCM output

- **Scope and files** — `system/banana-retro/src/{main.c,core.c,audio_alsa.c,audio_alsa.h}`, `system/banana-retro/tests/check-audio.c`, `system/banana-retro/Makefile`.
- **Dependencies and interface** — 01 and 03. ALSA `default`, bounded sample/batch queue, one worker, status and pause/rebase methods.
- **Sequence and completion** — Add a fake PCM interface for fixture verification, then real polling, bounded recovery and resampling. Start with the comparable latency request from the source baseline. Verify no core-loop blocking, bounded queue occupancy under injected clock drift, partial writes, XRUN recovery, server disconnect/reopen, rate change, and safe pause/quit. Separately qualify on RPC with authorized `make sound-test` and native audio playback before pilot promotion.
- **Complexity and rollback** — Medium. Preserve existing PipeWire/UCM and amp policy. Disable native profiles without working audio.

## 05. Persist native SRAM, RTC and manual states

- **Scope and files** — `system/banana-retro/src/{main.c,core.c,saves.c,saves.h}`, `system/banana-retro/tests/check-saves.c`, `system/banana-retro/Makefile`.
- **Dependencies and interface** — 01. Safe-point memory import/snapshot, explicit state slot, exact compatibility metadata and atomic generation commit.
- **Sequence and completion** — Establish identity and checked save API, then add atomic writes and restore validation. Verify round-trip, zero memory size, corrupt data, incompatible build/options, basename collision, truncated writes, interrupted generation and full-disk preservation of the last good generation. Test both SIGTERM and normal exit. Gate actual exFAT claims on separately authorized device or image-backed fault testing.
- **Complexity and rollback** — Medium. Keep native storage isolated. Failed serialization or I/O must leave committed generations usable.

## 06. Stage the opt-in Buildroot package

- **Scope and files** — `buildroot/package/banana-retro/{Config.in,banana-retro.mk}`, `buildroot/Config.in`, `Makefile`, `system/banana-retro/Makefile`, `system/banana-retro/config/defaults.conf`.
- **Dependencies and interface** — 02–05. Target C99, threads/dynamic loader, Wayland client and scanner, alsa-lib, and current core install paths. Keep the stock board selection unchanged.
- **Sequence and completion** — Add opt-in package, scanner generation and source rebuild tracking. Build enabled in an isolated configuration, inspect ARM64 ELF and expected files, check library closure, legal inputs, and unchanged stock package settings. Verify 02's QEMU presentation path once available. Rebuild after a local source edit to exercise the tracking.
- **Complexity and rollback** — Small to medium. Remove the opt-in selection to return to the stock image.

## 07. Add authenticated native game controls

- **Scope and files** — `system/bananad/include/banana-retro-control.h`, `system/banana-retro/src/{main.c,control.c,control.h,options.c,options.h,core.c}`, `system/banana-retro/tests/check-control.c`, `system/banana-retro/Makefile`.
- **Dependencies and interface** — 01, 03–05. Versioned seqpacket control, status, owner-specific holds, memory/state operations, options/reset and graceful quit.
- **Sequence and completion** — Validate packet bounds and identities, then execute one mutation at a safe point. Verify duplicate/stale request behavior, independent pause holds, options v0/v1/v2 and restart cases, save while paused, malformed packets, disconnect/reconnect, timeout reconciliation and clean exit. Confirm Wayland dispatch and buffer release continue during pause.
- **Terminal commit** — After QUIT or SHUTDOWN commits, every fresh mutation (reset, set_option, pause/resume, sleep prepare/wake/abort, save, flush, checkpoint, load) must fail with a non-OK status while an exact replay of an answered request still returns its cached reply. Verified by `check-control` and `check-saves`.
- **Complexity and rollback** — Medium. Keep native control unavailable on a failed handshake. Keep existing session negotiation available to later UI and power adapters.

## 08. Integrate an explicit experimental launch and pause path

- **Scope and files** — `system/rootfs/usr/libexec/banana/{run-libretro,run-retroarch,libretro-core.sh}`, `ui/src/{main.cpp,library.cpp,library.h,pause_menu.cpp,pause_menu.h}`, `ui/Makefile`, `tools/check-systems.py`, `system/tests/run-libretro-test.sh`.
- **Dependencies and interface** — 02, 06 and 07. Explicit runtime kind, per-session metadata, subprocess readiness and typed startup errors, native pause/status/quit adapter.
- **Sequence and completion** — Extract shared installed-core resolution while preserving the existing RetroArch launcher interface. Resolve the effective core once, classify runtime eligibility, launch the child and preserve PID/path publication and reaping. Wire a native pause handshake and input ownership handoff, existing overlay controls, native state/options/quit where supported, and fallback before readiness only. Verify custom systems commands, unchanged baked defaults, failed startup, timestamp/PID reuse rejection, rapid exit, mid-session crash, and menu restoration. Preserve standalone/app and RetroArch paths. Native end-to-end QEMU qualification was not performed and is not passed. The original native QEMU acceptance requirement is explicitly replaced by mandatory end-to-end physical RPC qualification in Task 11. Existing QEMU boot and protocol checks remain software evidence only. A specialized VKMS/Mesa/virtio-sound QEMU environment is not required under this amended roadmap.
- **Launcher identity** — A typed boot that times out must have observably spawned and reaped its child (`run-libretro-test.sh`). A tracked launch adopts the registered `game.path` spelling of an equivalent ROM path, byte-exact, before the runtime starts.
- **Complexity and rollback** — Large. Disable the experimental runtime rule to route back through RetroArch. Verify no switch if native data has already been committed in the current attempt.

## 09. Connect bananad power and shutdown to native sessions

- **Scope and files** — `system/bananad/{bananad.c,Makefile,include/banana-retro-control.h}`, `system/rootfs/etc/init.d/rcK`, `system/banana-retro/src/{main.c,control.c}`, `system/bananad/tests/retro-lifecycle-test.sh`.
- **Dependencies and interface** — 07 and 08. Power epochs, prepare/abort/wake, bounded checkpoint/PCM and exit reporting, tracked-child shutdown and resume marker.
- **Sequence and completion** — Keep existing platform calls and introduce backend-specific requests at the current boundaries. Verify suspend request during overlay, aborted suspend, late ack, renderer/service loss, stopped and stale child, valid/failed checkpoint, direct shutdown and crash reaping via mocked power operations. Hardware suspend, battery interlocks and audio restore are a separate approval and qualification gate.
- **Resume durability** — Directory-sync debt (UI and daemon) is paid before any further removal, including for absent or foreign markers, and a replacement is never deleted. Attempt counts are written and synced before the start. A newly queued launch after the menu animation wakes the poll loop. Verified on host fixtures only (`check-checkpoint`, `resume-test`); exFAT durability and hardware remain unqualified.
- **Complexity and rollback** — Large. Disable native power integration and native profiles together on a failed gate. Preserve generic process-group and RetroArch paths and existing init sequence.

## 10. Add explicit legacy data import and quick resume

- **Scope and files** — `system/banana-retro/{Makefile,src/{main.c,core.c,core.h,saves.c,saves.h},tests/{check-saves.c,fixture_core.c,legacy-fixtures/}}`, `ui/src/{settings.cpp,settings.h}`, `ui/tests/{run.sh,legacy-import-test.sh}`. The core files expose safe-point memory-size, checkpoint capture, and restore operations; the runtime fixture supplies deterministic serialization and rejected-restore behavior; the build and UI test files execute the scoped regression paths.
- **Dependencies and interface** — 05 and 09. Previewable SRAM/RTC import, native checkpoint metadata and boot resume record.
- **Sequence and completion** — Validate produced legacy raw save fixtures per core. Test old root/per-core layouts, name collisions, corrupt files, mismatched size, read-only originals, already-imported data, diverging native and RetroArch files, and cold-boot resume. Keep state import disabled until a separate task has verified actual legacy containers and exact-build unserialize. Enable automatic resume only for a verified native checkpoint and profile.
- **Cold-boot contract** — `bananad` publishes the versioned native boot record only after an authenticated checkpoint acknowledgement. The UI and launchers carry it to the matching enabled core; the runtime accepts only the exact verified checkpoint for `CURRENT`. Path-only records remain supported. A rejected native resume returns to the menu without switching to RetroArch or overwriting battery data. This handoff also covers `system/bananad/{bananad.c,include/banana-retro-control.h,tests/check-checkpoint.c}`, the runtime control and persistence fixtures, `ui/src/{app.h,main.cpp}`, `run-retroarch`, `run-libretro`, and their focused launcher fixture.
- **Complexity and rollback** — Medium. Disable importer or quick resume at the profile level. Preserve both save namespaces.

## 11. Qualify three software pilot profiles on hardware

- **Scope and planned files** — `system/banana-retro/config/defaults.conf`. Add `check-profiles.sh` under [runtime tests](../../system/banana-retro/tests/). The pilot is not yet qualified; record the authorized hardware results in a new `banana-retro-pilot-results.md` alongside this roadmap. Generate scratch artifacts outside the checkout.
- **Dependencies and interface** — 06 and 08–10. One externally authorized profile at a time for mGBA/GBA, Gambatte/GB+GBC, and FCEUmm/NES cartridges. A pilot requires matching core build, content, options and input policy.
- **Sequence and completion** — Capture the comparable RetroArch baseline first. For each externally authorized pilot profile, explicitly select the experimental native profile on physical RPC and verify runtime startup/readiness, successful first-frame presentation, audio readiness and output, pause/resume, input ownership, crash recovery, and menu restoration. These transferred checks replace the original native end-to-end QEMU acceptance requirement, which was not performed and is not passed. Task 11 cannot receive approval until every transferred check passes on physical hardware. A specialized native QEMU environment is not required, and QEMU smoke or protocol evidence does not satisfy this physical gate. Then verify the exact packaged core, fullpath/memory behavior, options, frame/audio timing, save import and state, overlay, input, exit, suspend/wake, and both automatic and manual fallback cases. Compare candidate latency, frame pacing, CPU/memory and audio under sustained runs. Record failures and disable the affected profile. Report actual commands, inputs, board and measurements. Preserve the existing hardware qualification requirements and keep profiles opt-in and stock defaults unchanged.
- **Complexity and rollback** — Medium implementation and substantial qualification. Keep opt-in profiles and stock defaults. Roll back per profile.

## 12. Qualify additional software or accessory capability

- **Scope and files** — Select one core or accessory in each new task. Candidate paths are `system/banana-retro/config/defaults.conf`, `system/banana-retro/src/{core.c,input_evdev.c,input_evdev.h,control.c,media.c,media.h}`, `system/banana-retro/tests/` with specific files named in the task, and `ui/src/pause_menu.cpp` when the UI action is in scope.
- **Dependencies and interface** — 11. Separate bounded items for each new software profile, capped fast-forward, archive staging, multi-disc, external analog/remap, evdev rumble, or keyboard. Define the ABI calls and content/input policy for the selected item.
- **Sequence and completion** — Validate the selected feature against pinned source and fixtures, then target behavior. For disc switching verify absolute indices, failed eject/select/insert and playlist state preservation. For fast-forward verify cap, normal clock rebasing, and queue recovery. For devices verify hotplug and neutral state on loss.
- **Complexity and rollback** — Small to medium per item. Keep the affected capability disabled and route dependent profiles to RetroArch if it fails.

## 13. Add one GL or GLES hardware profile

- **Scope and planned files** — `system/banana-retro/src/{core.c,main.c,video_wayland.c,video_gl.c,video_gl.h}`, `system/banana-retro/Makefile`, `buildroot/package/banana-retro/{Config.in,banana-retro.mk}`, `system/banana-retro/config/defaults.conf`. Add `check-gl.c` under [runtime tests](../../system/banana-retro/tests/).
- **Dependencies and interface** — 02, 06, 07, and 11. Versioned HW negotiation, proc lookup/FBO, Mesa EGL/Wayland and supported DMA-BUF output. Choose a verified GLES core or fixture first.
- **Sequence and completion** — Exercise a fixture context reset/destroy and loss before enabling one packaged GLES profile. Verify direct and transformed import, producer fencing, nonblocking control, buffer release, overlays, suspend/wake, and display and audio pacing on RPC.
- **Complexity and rollback** — Large. Reject unsupported APIs and keep per-profile RetroArch fallback. Treat desktop GL requirements as a separate profile qualification.

## 14. Add one Vulkan hardware profile

- **Scope and planned files** — `system/banana-retro/src/{core.c,main.c,video_wayland.c,video_vulkan.c,video_vulkan.h}`, `system/banana-retro/Makefile`, `buildroot/package/banana-retro/{Config.in,banana-retro.mk}`, `system/banana-retro/config/defaults.conf`. Add `check-vulkan.c` under [runtime tests](../../system/banana-retro/tests/).
- **Dependencies and interface** — 13's hardware lifecycle and 11's pilot infrastructure. Negotiation/render versions, queue and image ownership, WSI swapchain and device loss handling. Choose one of Flycast or SwanStation after tracing its packaged requests.
- **Sequence and completion** — Validate the fixture contract, then the selected core with pinned options and measured WSI/rotation paths. Confirm imported format/modifier, completion before scanout, old-buffer lifetime, graceful context loss and suspend recovery. Measure a613/Turnip settings before enabling a tuning profile.
- **Complexity and rollback** — Large. Gate each driver/core pair explicitly. Keep software and GLES profiles independent of Vulkan availability.

## 15. Decouple core packaging from RetroArch

- **Scope and planned files** — `buildroot/Config.in`, `buildroot/package/retroarch/{Config.in,cores.in}`, `buildroot/configs/banana_retroid_pocket_classic_defconfig`, and `tools/check-systems.py`. Add `libretro-cores/Config.in` under [Buildroot packages](../../buildroot/package/).
- **Dependencies and interface** — 06 and 11. Core choice and installation survive independently of the RetroArch frontend symbol. Keep the configured stock frontend and core inventory available.
- **Sequence and completion** — Compare core selection, target dependencies, license/source manifests and baseline image behavior with and without RetroArch in scratch configurations. Confirm the optional frontend-free image builds and keeps the selected cores.
- **Complexity and rollback** — Medium. Restore the previous Kconfig ownership and stock image if core selection or dependencies drift.

## 16. Promote one qualified system default

- **Scope and files** — `system/rootfs/etc/banana/systems.conf` and `system/banana-retro/config/defaults.conf`. Choose one system when authorizing the task.
- **Dependencies and interface** — 11 and 15, plus the specific 12–14 items needed for the selected system. The runtime-selection policy must preserve effective custom commands, fallback and save provenance.
- **Sequence and completion** — Apply the architecture's release gates to enabled core/content profiles. Switch only the selected default, exercise user override and runtime and image rollback, and record any remaining profile that uses RetroArch. Removing the frontend and its data, patches or assets requires a subsequent scoped task after the remaining supported profiles and A/B rollback are addressed.
- **Complexity and rollback** — Medium implementation with a high release impact. Restore the previous default or image and retain both save namespaces.
