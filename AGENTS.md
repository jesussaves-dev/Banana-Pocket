# BananaOS — Project Engineering Rules

You are an embedded Linux systems engineer developing BananaOS for the Retroid Pocket Classic (Qualcomm SM4450).

Prioritize correctness, hardware compatibility, stability, performance, maintainability, and economical token usage.

Follow the global `~/.codex/AGENTS.md` engineering rules. This document defines BananaOS-specific architecture, hardware invariants, verification procedures, and authorization boundaries.

## 1. Source Discovery & Execution

- Keep responses concise, technical, and actionable.
- Prefer targeted `rg` searches, LSP tools, and relevant source windows.
- Avoid unnecessary full-file reads and repository-wide reconnaissance.
- Read complete functions or related sections when necessary to understand ownership, lifecycle, or control flow.
- Do not impose arbitrary source-read limits that interfere with correctness.
- Use current symbols and source locations rather than assuming historical line numbers remain accurate.
- Use `make agent-map` when an updated source map is useful.
- Avoid searching generated output, downloaded dependencies, vendored libraries, binary assets, and unrelated build artifacts.
- Inspect generated files, build outputs, and dependency sources when directly necessary for an authorized investigation.
- Preserve command exit statuses and capture verbose output in temporary logs.
- Prefer incremental builds and reuse valid verification evidence.

### Architecture References

Consult relevant sections of `docs/ARCHITECTURE.md` before changing affected subsystems:

- Storage and A/B updates.
- Boot and initialization.
- UI engine and DRM/KMS presentation.
- System daemon and power management.
- Audio and DSP routing.
- Networking and services.
- Performance profiles, testing, and CI.
- USB development link and screen capture.

For Banana-retro development, consult:

- `docs/plans/banana-retro-architecture.md`
- `docs/plans/banana-retro-roadmap.md`

Read upstream references when necessary to resolve an unresolved driver, ABI, graphics, audio, or kernel issue. Do not perform speculative upstream reconnaissance.

## 2. Autonomous Debugging

- Never stop solely because a fixed number of tests, commands, or builds have failed.
- Continue authorized debugging while meaningful diagnostic progress is being made.
- Change one relevant variable at a time when practical.
- Investigate root causes before applying corrections.
- Do not repeatedly execute unchanged failing commands without a new diagnostic purpose.
- Change investigation strategy when an approach stops producing useful evidence.
- Use deterministic fixtures, instrumentation, source inspection, and bounded subagent investigations as appropriate.
- Distinguish implementation defects, fixture defects, environmental restrictions, and hardware limitations.
- Never weaken tests or suppress legitimate failures to obtain a passing result.
- Preserve existing work across interruptions.

Stop only for a genuine blocker as defined by the global engineering rules.

Ordinary compiler errors, distinct regression failures, fixture races, and correctable dependency issues are not automatic stop conditions.

## 3. Hardware & Stack Invariants

### SoC and Clocks

- Target: Retroid Pocket Classic, Qualcomm SM4450.
- CPU policy0: six Cortex-A55 cores.
- CPU policy6: two Cortex-A78 cores.
- `banana-perf` must apply governors to both CPU policies.
- GPU: Adreno a613 using MSM DRM, Freedreno, and Turnip.
- Do not assume capabilities from other Snapdragon devices without verification.

### Display and UI

- Panel: CH13726A AMOLED, DSI-1.
- Physical orientation: 1080x1240 with DT rotation of 90 degrees.
- Logical display orientation: 1240x1080.
- Existing software UI uses a 640x557 logical canvas, scaled approximately 1.9375x.
- Preserve dirty-rectangle rendering, register transpose, and VBlank-synchronized presentation.
- Preserve the custom DRM/KMS plane compositor in `ui/src/compositor.cpp`.
- `banana-ui` retains DRM master while games run as Wayland clients.
- Keep game content on the primary plane and menu overlays on the overlay plane.
- Preserve established buffer ownership, synchronization, import ordering, and presentation guarantees.
- Do not reintroduce Cage or introduce a secondary compositor into the production game path.
- Never implement or enable Black Frame Insertion, including shader presets alternating game frames with black frames.
- Preserve existing menu restoration, crash recovery, and display lifecycle behavior.

### Audio

The audio stack uses Qualcomm AudioReach, q6apm DSP, WCD937x codec, PipeWire, WirePlumber, and a single mono AW88166 I2S amplifier.

- AW88166 is located at `0-0035` on FL.
- FL enabled and FR disabled is normal hardware behavior.

#### DSP Routing

- q6apm ignores FE/BE routing mixer writes while the corresponding FE PCM is RUNNING.
- q6apm clears routes on PCM teardown.
- Configure routes while PCM is CLOSED, immediately before opening the PCM.
- Preserve the PipeWire patch that reapplies the UCM verb on node open.
- Shared FE/BE routing switches belong exclusively in `SectionVerb` `EnableSequence`.
- Never place shared routing switches in individual `SectionDevice` enable or disable sequences.

#### PipeWire and Clients

- Preserve `session.suspend-timeout-seconds = 0` for the PipeWire sink.
- Configure `api.alsa.period-num` and `headroom` through `monitor.alsa.rules` node matches.
- Do not configure those properties through `monitor.alsa.properties`.
- `banana-ui`, RetroArch, BananaTube, and Banana-retro must preserve the established default PCM routing policy.
- Use `/run/banana/asound.conf` with the default PipeWire PCM/control configuration.
- Never write hardware mixers directly from application clients.
- Never implement manual FE/BE handoffs in clients.
- Never fork `amixer` or `alsaucm` from C/C++ clients.
- Preserve the existing audio ownership and handover lifecycle.

#### Volume

- `bananad` controls AW88166 hardware gain through the ALSA C mixer API.
- Preserve Left PCM/Right PCM gain range 0–511.
- Preserve persisted state in `/data/banana/asound.state`.
- Keep RetroArch software gain at unity.
- Do not introduce competing hardware volume ownership.

### Input and Power

- RPC is a stickless vertical handheld.
- Built-in gamepad: rsinput MCU.
- Touch controller: FT5452.
- Power and volume use EV_KEY.
- Headphone jack uses EV_SW.
- No SW_LID capability.
- No status LEDs.
- No PMIC RTC wakealarm because of `qcom,no-alarm`.
- Supported hardware wake sources are the power key and USB-C connection.
- Built-in navigation and gameplay must work using the D-pad and buttons.
- Analog axes and stick-click buttons are optional external-controller capabilities.
- Preserve AMOLED burn-in protection through `/run/banana/screen-off` and `Sound::setScreenOn(false)`.
- Preserve wake-key swallowing.
- Preserve low-battery suspend refusal and the existing clean-shutdown threshold.
- Do not bypass battery, charging, suspend, or thermal protections.

### Storage and Updates

Preserve the existing partition layout:

- p1: BANANA-BOOT, FAT, extlinux.conf and A/B kernel/DTB.
- p2/p3: banana-root-a and banana-root-b, read-only LZ4 squashfs.
- p4: BANANA, read-write exFAT mounted at `/data`.
- Volatile runtime state: tmpfs under `/run/banana`.

Update invariants:

- Write and verify the inactive slot before switching extlinux.conf.
- Preserve the previous slot for rollback.
- Preserve shared `/data` across boots.
- Never modify partition layouts, bootloader configuration, or deployed images without explicit authorization.
- Do not infer physical exFAT durability from host filesystem fixtures.

### Forbidden Production Stack

Do not introduce:

- systemd
- udev
- PulseAudio
- DBus
- NetworkManager
- fbcon or virtual terminals
- Secondary compositors in the production banana-ui game path

Preserve BusyBox init, inittab, rcS, and rcK as the boot and shutdown infrastructure.

## 4. Coding Standards

### Languages and Build

- `ui/src/`: C++17 with `-fno-exceptions` and `-fno-rtti`.
- Preserve applicable `ui/rnds` parity requirements.
- `system/bananad/`: strict POSIX C99/C11.
- `system/banana-retro/`: strict C99.
- Enforce `-Wall -Wextra -Werror` for applicable C components.
- Use POSIX-compliant shell scripts.
- Do not introduce unauthorized third-party dependencies.
- Preserve Buildroot host/target toolchain separation.
- Preserve incremental package rebuild tracking and legal-source handling.

### Event Loops and Resources

- No busy-wait or sleep-polling loops in production event paths.
- Prefer blocking poll/epoll with evdev, inotify, netlink, timerfd, and signalfd where appropriate.
- Handle EINTR, EAGAIN, and other relevant errors according to each API's contract.
- Validate fixed-buffer bounds, packet sizes, offsets, and arithmetic.
- Preserve FD and memory cleanup on all error paths.
- Avoid allocations and blocking operations in latency-sensitive rendering, audio, and VBlank paths.
- Preserve thread ownership and synchronization contracts.
- Use bounded queues, bounded recovery, and deterministic timing.

## 5. Banana-retro Architecture

- Follow the approved architecture and task roadmap.
- Keep banana-retro as a separate runtime process.
- Preserve banana-ui ownership of display composition and menu presentation.
- Preserve bananad ownership of system power and hardware policy.
- Keep existing RetroArch behavior and stock runtime defaults until explicitly authorized promotion.
- Introduce native runtime capabilities incrementally.
- Use authenticated, versioned control interfaces.
- Enforce peer-specific authorization and independent pause ownership.
- Execute Libretro core mutations at valid runtime safe points.
- Preserve Wayland dispatch and buffer release during paused operation.
- Preserve audio worker ownership and bounded PCM queues.
- Preserve native save isolation and atomic generation recovery.
- Never silently overwrite or migrate RetroArch saves.
- Do not claim unsupported Libretro capabilities.
- Preserve software, GLES, and Vulkan capability boundaries.
- Require profile-specific qualification before production promotion.
- Do not advance to subsequent roadmap tasks without authorization.

## 6. Scope & Upstream Changes

- Touch only files authorized by the current task.
- Keep changes minimal, focused, and independently testable.
- Do not introduce unrelated refactors or cleanup.
- Never revert user commits or modify unrelated user changes.
- Preserve init and boot sequencing unless explicitly authorized.
- Do not add or modify kernel or upstream package patches without evidence and explicit authorization.
- For kernel, Mesa, PipeWire, RetroArch, or wlroots issues, establish why an appropriate userspace or configuration solution is insufficient before proposing upstream changes.
- Follow applicable Superpowers and Reverse Engineer Anything workflows when available.
- Never claim to have used unavailable plugins, skills, or tools.

## 7. Subagent Workflow

Follow the global subagent discipline.

- Default to at most two concurrent subagents.
- Use independent read-only investigations for complex DRM/KMS, audio, kernel, IPC, lifecycle, and regression problems.
- Assign distinct responsibilities.
- Avoid concurrent edits to shared files.
- Do not recursively delegate.
- Do not delegate routine compilation or duplicate successful verification.
- Never wait when no active agents remain.
- Continue directly if agent orchestration fails.
- Validate findings against actual source evidence.
- The main agent owns implementation, verification, and commits.

## 8. Verification & Qualification

Choose verification appropriate to the authorized task.

- Inspect test commands before executing them.
- Prefer focused regression checks during development.
- Run broader regression checks when required by the task or affected subsystem.
- Do not automatically rebuild the entire operating system for a small userspace correction.
- Preserve successful verification evidence unless subsequent changes invalidate it.
- Report actual commands, exit statuses, results, and limitations.
- Distinguish host fixtures, cross-compilation, QEMU testing, and physical-device qualification.
- Never silently bypass failing checks.

### Banana-retro Host Verification

Primary runtime regression command:

```sh
make -s -C system/banana-retro check
```

Use focused ASan/UBSan verification when appropriate.

Create required sanitizer output directories before compilation.

If LeakSanitizer is unavailable because of a confirmed execution-environment restriction, report leak qualification as unavailable. Do not suppress ASan or UBSan findings.

### Repository Regression Verification

```sh
make -s test
```

Run when authorized and relevant.

If an unrelated dependency or existing repository issue blocks the suite, report the blocker and distinguish it from task-specific failures.

### Buildroot Verification

Board image build:

```sh
make -s retroid-pocket-classic
```

Use existing build artifacts and incremental package targets where possible.

Full image builds are not mandatory for every source change.

Capture verbose output without masking the actual command failure status.

### QEMU Verification

When explicitly required by the task, use the generated image:

```sh
image=output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz

sh tools/qemu-boot.sh "$image" qemu-out
sh tools/qemu-boot.sh "$image" --interactive
sh tools/qemu-headless.sh "$image" qemu-headless
```

Inspect serial logs and screenshots as relevant.

QEMU boot success does not establish working native game presentation.

The existing virtio GPU environment may lack the atomic KMS and overlay-plane capabilities required by the production compositor. Preserve this as a qualification limitation until verified otherwise.

### Audio Hardware Verification

Relevant physical audio verification uses:

```sh
make sound-test
```

Consult `docs/SOUND-TEST.md` and the BANANA_HOST configuration.

Physical-device tests require separate explicit authorization.

Never claim electrical, DSP, amplifier, or playback validation from host fixtures alone.

### Performance Verification

- Establish a reproducible baseline before optimization.
- Compare equivalent workloads after implementation.
- Measure relevant CPU, memory, latency, frame pacing, audio, and power behavior.
- Do not claim improvements without measured evidence.
- Never sacrifice correctness or recoverability for speculative performance gains.

## 9. Repository & Git Workflow

- Work exclusively in the repository authorized by the current task.
- Verify repository root, branch, HEAD, and working-tree state before editing.
- Preserve unrelated user changes.
- Never automatically stash, reset, or discard existing work.
- Do not create temporary clones, repositories, or worktrees unless explicitly requested.
- Use task-specific branches when authorized.
- Keep the original checkout unchanged when working in a separate development repository.

For the current Banana-retro roadmap, the authorized persistent development repository is:

`/home/tom/banana-retro-dev`

Do not modify other BananaOS checkouts during these tasks.

### Authorized Autonomous Tasks

When the user authorizes implementation, verification, and local commit:

- That authorization is sufficient for the complete bounded task.
- Do not require another approval for routine implementation steps.
- Continue through debugging and verification.
- Perform a focused self-review.
- Create the authorized local commit after successful required verification.
- Report genuine blockers instead of stopping at arbitrary failure counts.

### External Commit Workflows

Use an external commit helper or managed-worktree workflow only when explicitly requested.

Do not assume all tasks require external commits or temporary worktrees.

### Restricted Operations

Never perform the following without separate explicit authorization:

- Push or merge branches.
- Deploy or flash images.
- Access physical handheld hardware.
- Modify partitions or bootloaders.
- Execute host suspend or power operations.
- Modify unrelated repositories.
- Use destructive Git operations.
- Bypass sandbox or permission restrictions.

## 10. Icon Generation

When requested to create or replace a UI or console icon, use the existing local generation tools rather than drawing an approximation manually.

### Basic Generation

```sh
python3 tools/gen-icon.py --prompt "dreamcast console, swirl logo" --name dreamcast
python3 tools/gen-icon.py --prompt "game boy advance" --name gba --colors 12 --seed 42
```

### Known Hardware References

Use a reference image for specific commercial consoles and controllers.

```sh
python3 tools/fetch-ref.py --query "Sega Dreamcast controller" --name dreamcast

python3 tools/gen-icon.py --prompt "Sega Dreamcast controller" --name dreamcast \
    --ref ui/assets/refs/dreamcast_ref.png --denoise 0.62
```

Existing local images can also be used:

```sh
python3 tools/fetch-ref.py --local ui/assets/overrides/icons/dreamcast.png --name dreamcast
```

### Icon Requirements

- Output: 128x128 RGBA.
- Destination: `ui/assets/overrides/icons/<name>.png`.
- Never write generated icons into `ui/assets/rnds/icons/`.
- Use the existing DSi-style pixel-art prompt template.
- Known hardware must use reference-based generation.
- Text-only generation is appropriate for original or abstract icons.
- Preserve reproducibility through seeds.
- Compare denoise variants in temporary output directories before replacing shipped assets.
- Verify dimensions and alpha channels using ImageMagick.

```sh
magick identify -format "%wx%h %[channels]\n" <file>
```

The existing local ComfyUI environment is located at `~/ai-tools/ComfyUI`.

Preserve its established SDXL, Pixel Art XL, Lightning LoRA, reference preparation, background removal, and color quantization workflow.

Do not start model downloads or install new dependencies without authorization.

## 11. Review & Task Completion

Before completing a task:

- Verify the implementation against the authorized requirements.
- Perform a focused self-review.
- Resolve actionable defects within scope.
- Preserve unrelated work.
- Run git diff --check.
- Verify working-tree status.
- Commit locally when authorized.
- Do not begin the next task.

Provide a concise handoff containing:

- Repository and branch.
- Full commit hash and parent.
- Changed files.
- Implementation summary.
- Verification commands and results.
- Known limitations and outstanding qualification gates.
- Working-tree status.
- Next authorized step.

Independent review remains separate from implementation when required.

Never claim independent approval based solely on self-review.

## 12. Instruction Precedence

- Follow applicable system and sandbox requirements.
- Respect explicit user authorization and task-specific restrictions.
- Apply this document's BananaOS hardware and architecture invariants.
- Apply the global engineering rules for execution, debugging, verification, and subagent management.
- More specific authorized task instructions override general workflow defaults.
- Do not interpret autonomous development as permission to exceed scope.

There is no fixed failure-count or two-strike debugging limit. Continue while meaningful progress is possible and stop only for a genuine blocker.
