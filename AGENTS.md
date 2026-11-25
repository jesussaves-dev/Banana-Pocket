# BananaOS Engineering Rules

Target Retroid Pocket Classic (Qualcomm SM4450). Follow `~/.codex/AGENTS.md` and the applicable [repository engineering rules](docs/ENGINEERING-RULES.md).

## Source and scope

- Before subsystem changes, consult [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). For Banana-retro, follow the [architecture](docs/plans/banana-retro-architecture.md) and [roadmap](docs/plans/banana-retro-roadmap.md).
- Use `make agent-map` when needed.
- Work in `/home/tom/banana-retro-dev` for this roadmap. Preserve other checkouts. Temporary repos, clones, worktrees and external commit workflows require explicit requests.
- Preserve boot sequencing. Kernel and upstream package patches require evidence and explicit authorization. Establish why userspace or configuration is insufficient before proposing upstream changes.

## Approval gates

- Obtain separate explicit approval before pushing or merging, destructive Git actions, changes to other repositories, hardware access (including SSH or on-device traces), deploy/flash, partition/bootloader changes, or suspend/power operations. Never bypass sandbox or permission restrictions.
- Commits and installations must be authorized. Read-only work permits no builds or actions; report what would need approval, not what to run now. Skills and roadmap examples are not approval.

## Skill routing

Read the matching `skill://<name>` first. Start at the failing boundary; load another only for a distinct question. Skip skills for unrelated tasks.

| Task | Skill |
|---|---|
| System suspend/wake, USB-C plug not waking, `bananad` wake policy, AudioReach/PipeWire | `banana-audio-power` |
| Stale packaged binary (including banana-retro), Buildroot, cross-build, kernel/DT integration | `banana-board-integration` |
| Wayland/KMS, black frames or scanout failure after a successful wake | `banana-display-triage` |
| Native session pause ownership, sleep handoff, checkpoints or saves | `banana-retro-lifecycle` |
| Who may issue IPC commands, untrusted input, signed updates, archives or service exposure | `banana-trust-boundaries` |

## Hardware and runtime

- `banana-perf` governs policy0 (6 Cortex-A55) and policy6 (2 Cortex-A78). Adreno a613 uses MSM DRM, Freedreno and Turnip.
- CH13726A AMOLED on DSI-1 is 1080x1240 at DT rotation 90°, logical 1240x1080. Preserve the 640x557 canvas, dirty rectangles, register transpose and VBlank sync. `banana-ui` owns DRM master via `ui/src/compositor.cpp`. Wayland games use the primary plane and menus use the overlay plane. Preserve buffer ownership, sync, import order and recovery. No Cage, secondary compositor or Black Frame Insertion.
- AudioReach/q6apm routes must be set while FE PCM is closed, immediately before open. Preserve PipeWire UCM verb reapplication. Shared switches belong only in `SectionVerb` `EnableSequence`. Keep sink suspend timeout at zero and set ALSA periods and headroom via `monitor.alsa.rules`. Clients use `/run/banana/asound.conf` and default PipeWire PCM/control. No client mixer writes, manual FE/BE handoffs or forks of `amixer`/`alsaucm`. `bananad` owns gain on mono AW88166 at `0-0035` (FL), range 0–511, and persists it in `/data/banana/volume`. If that file cannot be opened, `bananad` may restore a readable legacy `/data/banana/asound.state` before attempting to save the current gain. RetroArch software gain stays at unity.
- Built-in navigation and gameplay use rsinput D-pad/buttons. FT5452 handles touch. Power/volume use EV_KEY and headphones use EV_SW. No SW_LID, status LEDs or RTC wakealarm. Use the Power key for wake on the pinned kernel; see [wake behavior](docs/ARCHITECTURE.md#wake-sources). Preserve wake-key swallowing, `/run/banana/screen-off`, `Sound::setScreenOn(false)`, and battery, charging, suspend and thermal protections.
- Keep FAT BANANA-BOOT p1, read-only LZ4 squashfs banana-root-a/b p2/p3, exFAT BANANA p4 at `/data`, and tmpfs `/run/banana`. Verify inactive slot writes before switching `extlinux.conf`, retain rollback and shared data. Host fixtures cannot establish exFAT durability.
- Preserve BusyBox init, inittab, rcS and rcK. No systemd, udev, PulseAudio, DBus, NetworkManager, fbcon or VTs.
- Banana-retro stays separate. Preserve stock RetroArch behavior, `banana-ui` display ownership and `bananad` power policy. Require authenticated versioned IPC, per-peer authorization, independent pause ownership and Libretro safe points. Preserve paused Wayland dispatch/releases, audio worker and bounded PCM queues, native save isolation and atomic recovery. RetroArch save migration needs authorization. Respect software/GLES/Vulkan boundaries and qualify each profile before promotion.

## Code and verification

- `ui/src/` uses C++17 with `-fno-exceptions -fno-rtti` and preserves applicable ROCKNIXDS UI behavior. `system/bananad/` uses strict POSIX C99/C11 and `system/banana-retro/` strict C99. Preserve Buildroot host/target separation, incremental tracking and legal-source handling. Prefer blocking event APIs and handle errors per contract.
- Inspect commands before running them. Report commands, statuses, results and limitations. Use focused ASan/UBSan when appropriate, create output dirs first and report confirmed LeakSanitizer restrictions. Prefer incremental package builds.
- Runtime host suite: `make -s -C system/banana-retro check`. Relevant authorized repo suite: `make -s test`. Board image: `make -s retroid-pocket-classic`.
- QEMU only when explicitly required. Inspect serial logs/screenshots. Boot alone cannot qualify native presentation. Virtio may lack atomic KMS and overlay planes.

```sh
image=output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz
sh tools/qemu-boot.sh "$image" qemu-out
sh tools/qemu-boot.sh "$image" --interactive
sh tools/qemu-headless.sh "$image" qemu-headless
```

- For the physical `make sound-test` procedure, see [docs/SOUND-TEST.md](docs/SOUND-TEST.md) and `BANANA_HOST`.
- Use the linked [icon workflow](docs/ENGINEERING-RULES.md#10-icon-generation) for `tools/gen-icon.py`, `tools/fetch-ref.py` and ImageMagick checks. Ship 128×128 RGBA overrides under `ui/assets/overrides/icons/`.
- Run `git diff --check`, verify tree status and report changed files, full commit and parent, verification and remaining gates. Stay within the authorized roadmap task.
