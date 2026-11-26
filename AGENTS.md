
# BananaOS Engineering Rules

Target: Retroid Pocket Classic (SM4450).
Follow `~/.codex/AGENTS.md` and [engineering rules](docs/ENGINEERING-RULES.md).

## Compliance and Scope
- Use authorized editing tools only; never bypass restrictions. If unavailable, stop and report. Disclose violations.
- Explicit approval required for commits, push/merge, destructive Git, installations, other repos, temporary repos/worktrees, hardware/SSH, deployment, flashing, partition/bootloader or power operations.
- Read-only means no builds or modifications. Documentation and skills are not authorization.
- Work in `/home/tom/banana-retro-dev`; preserve other checkouts and accepted fixes.
- Preserve evidence, exit statuses and task state until independent acceptance. After compaction, recheck scope, approvals and unresolved findings.
- No scope expansion, repeated investigations without new evidence or unverified success claims.

## Source Authority
- Consult [architecture](docs/ARCHITECTURE.md), [Banana-retro architecture](docs/plans/banana-retro-architecture.md) and [roadmap](docs/plans/banana-retro-roadmap.md) as applicable.
- Use `make agent-map` when needed and authorized.
- Preserve boot sequencing. Kernel/upstream patches require evidence, explicit approval and justification over userspace/configuration fixes.

## Skills
Read matching `skill://<name>` before subsystem work; load others only for distinct questions.

| Boundary | Skill |
|---|---|
| Suspend/wake, audio, power | `banana-audio-power` |
| Buildroot, stale binaries, kernel/DT | `banana-board-integration` |
| Wayland/KMS, scanout, black frames | `banana-display-triage` |
| Native lifecycle, pause, checkpoints, saves | `banana-retro-lifecycle` |
| IPC authorization, input, updates, archives, services | `banana-trust-boundaries` |

## System Invariants
- CPU: `banana-perf`, policy0 6x A55, policy6 2x A78. GPU: Adreno a613, MSM DRM, Freedreno/Turnip.
- Display: CH13726A DSI-1 1080x1240, DT rotation 90°, logical 1240x1080. Preserve 640x557 canvas, dirty rectangles, register transpose, VBlank, buffer ownership, sync, import order and recovery. `banana-ui` owns DRM master; games primary plane, menus overlay. No Cage, secondary compositor or BFI.
- Audio: AudioReach/q6apm routes while FE PCM closed, immediately before open. Preserve PipeWire UCM reapplication; shared switches only in `SectionVerb` `EnableSequence`. Sink suspend timeout zero; periods/headroom via `monitor.alsa.rules`. Clients use `/run/banana/asound.conf` and default PipeWire PCM/control. No client mixer writes, manual FE/BE handoffs or `amixer`/`alsaucm` forks. `bananad` owns AW88166 `0-0035` FL gain 0–511, persisted in `/data/banana/volume`; readable legacy `asound.state` fallback only when volume cannot open. RetroArch gain unity.
- Input/power: rsinput buttons, FT5452 touch, EV_KEY power/volume, EV_SW headphones. No SW_LID, status LEDs or RTC wakealarm. Power-key wake on pinned kernel. Preserve wake swallowing, `/run/banana/screen-off`, `Sound::setScreenOn(false)` and battery/charging/thermal/suspend protections.
- Storage: p1 FAT BANANA-BOOT; p2/p3 read-only LZ4 squashfs root-a/b; p4 exFAT BANANA `/data`; tmpfs `/run/banana`. Verify inactive slot before switching `extlinux.conf`; retain rollback/shared data. Host tests cannot qualify exFAT durability.
- Init: BusyBox, inittab, rcS/rcK. No systemd, udev, PulseAudio, DBus, NetworkManager, fbcon or VTs.
- Banana-retro: separate runtime; preserve stock RetroArch, UI display ownership and daemon power policy. Authenticated versioned IPC, per-peer authorization, independent pause holds, Libretro safe points, paused Wayland dispatch/releases, audio worker, bounded PCM queues, isolated atomic saves/recovery. Migration requires approval. Respect software/GLES/Vulkan boundaries; qualify profiles before promotion.
- Resume: attempt count synced before start and never refunded; unsynced unlinks stay owed until a directory sync succeeds, even for absent or foreign markers, and replacements are never deleted. Launcher adopts the registered ROM spelling byte-exactly. Committed native runtime rejects all fresh mutations, replays exact requests.

## Code and Verification
- UI: C++17, `-fno-exceptions -fno-rtti`, preserve applicable ROCKNIXDS behavior. bananad: POSIX C99/C11. banana-retro: C99.
- Preserve Buildroot host/target separation, incremental tracking and legal-source handling. Prefer blocking event APIs, contract-based error handling and incremental builds.
- Inspect commands; use focused fail-before/pass-after tests and ASan/UBSan as appropriate. Create output dirs first; report LeakSanitizer restrictions.
- Authorized checks: `make -s -C system/banana-retro check`, `make -s test`, `make -s retroid-pocket-classic`.
- QEMU only by request; use `tools/qemu-boot.sh` or `tools/qemu-headless.sh`. Inspect serial/screenshots; boot alone cannot qualify presentation or atomic KMS/overlay support.
- Physical audio: [SOUND-TEST.md](docs/SOUND-TEST.md), `BANANA_HOST`.
- Assets: [icon workflow](docs/ENGINEERING-RULES.md#10-icon-generation), `tools/gen-icon.py`, `tools/fetch-ref.py`, ImageMagick; 128x128 RGBA overrides in `ui/assets/overrides/icons/`.

## Completion
- Run `git diff --check`; verify repo, branch, HEAD, changed files and staged/untracked state.
- Report commands, exit statuses, evidence, limitations, violations and remaining gates. Distinguish source inspection, host fixtures, production execution and hardware qualification.
- If authorized commit created, report full hash and parent.
- Preserve logs; stop at review/approval boundary.
