# PLAN: DRM/KMS plane compositor in `banana-ui` (replacing `cage`)

Branch `feat/drm-plane-compositor` · shipped through **0.5.17** · updated 2026-11-10.
Executor-oriented: do ONE task (§5) per session. Background, measurements and the old phase text live in
`docs/PLAN_DRM_COMPOSITOR_HISTORY.md` — read a section of it by `rg -n` + `sed -n` only when a task says so.

## 0. Executor rules (read first, every session)

1. Pick the **first task in §5 whose status is `TODO`** and whose `needs` are all `DONE`. Do only that task.
2. Do not re-measure anything in §2. Those are settled facts; a task that contradicts one must STOP and report.
3. Touch only the files listed in the task. Anything else needs user approval.
4. One change, one test. Two failed attempts in a row → STOP and print the 5-line report (§6). No speculative
   refactors, no "while I'm here" cleanups.
5. Never edit: `linux/`, `drivers/`, kernel patches, Buildroot `package/`, `ui/third_party/`, `ui/rnds/`,
   `tools/qemu-boot.sh` (user has local changes), `docs/PLAN_DRM_COMPOSITOR_HISTORY.md`.
6. Host gate for every code task: `make -s test 2>&1 | grep -iE "error|warning|fail" | tail -n 30` (empty = pass).
   Hardware gates need the handheld; if it is not reachable, mark the task `BLOCKED (needs unit)` and stop.
7. When a task is done: set its status to `DONE (<short sha>)` in §5, add one line to §2 if it produced a new
   fact, and commit. Do not rewrite other sections.

## 1. Current state (one paragraph)

`banana-ui` owns DRM master permanently, runs an in-process Wayland server (`/run/banana/wayland/banana-0`),
puts the game on the **primary** plane via one **EGL rotate pass** (1240×1080 → 1080×1240 panel), and draws
menu cards on the **overlay** plane. **RetroArch, ports and BananaTube** all run resident through it when `compositor-mode` is `ui` (the default);
any window size is letterboxed and rotated (T2, T4).
`compositor = "cage"` in `/data/banana/bananad.conf` restores the old path for everything.

## 2. Settled facts (do not re-test)

| # | fact | source |
|---|---|---|
| F1 | One CRTC (50), primary plane 38, overlay plane 44, `DSI-1` 1080×1240@60. DRM leases are useless (one CRTC). | history §1, §1.7 |
| F2 | Plane rotate-90 is **rejected** (`EINVAL`) for linear buffers → game frames need the EGL rotate pass. | history §1.7, §2.2 |
| F3 | Clients default to XR24 + modifier `0x0500000000000001` (compressed); `AddFB2WithModifiers` **rejects** it. Advertise **LINEAR XR24/AR24 only**. | history §2.2 |
| F4 | Overlay: AR24, zpos 255 above primary 0, alpha + premultiplied accepted; scaling/letterbox accepted. | history §1.7 |
| F5 | EGL slots must be imported into EGL **before** PRIME/`AddFB2` registration, else the GPU hangs. | history §3.3 |
| F6 | Must flush queued Wayland events before blocking in `poll()`; flips are nonblocking atomic; rotate is fenced (EGL native fence), not `glFinish`. | 3F commits |
| F7 | RetroArch Vulkan resident: 60.03 fps, 0/932 dropped (cage control 59.91, 13/793) under the balanced profile. | `7b515c7` |
| F8 | Resume with a resident game: `Display::restore` re-modeset + `Compositor::resume()`; a failed present must still fire the frame callback. | `3ed4c85` |
| F9 | Input: MENU opens the pause card after release, `EVIOCGRAB`s pad+touch only (never power/volume), writes `/run/banana/overlay-open`; 50/50 cycles clean. | `9e8e753` |
| F10 | Cards: bananad writes `/run/banana/overlay` (`kind\|title\|body\|badge\|duration_ms`, tmp+rename) only while `/run/banana/compositor` exists; otherwise `SHOW_MSG` to RetroArch. | `d55e005`, `ba695c2` |
| F12 | Any source size is letterboxed + rotated by `fitRotated` (`rotate.h`); DirectPresenter is now only the `3c-direct` escape hatch. Readback: 640×480 → viewport 75,0 930×1240, same corner orientation as 1240×1080. | T2 |
| F13 | The toplevel configure must carry `XDG_TOPLEVEL_STATE_FULLSCREEN`: without it SDL2 keeps a non-resizable window at its creation size (BananaTube 1024×768). With it BananaTube, Celeste Classic and RetroArch all render 1240×1080 with no cage. | T4 |
| F14 | Cause of F11: the rotate shader was `precision mediump` (fp16 on Adreno); under `GL_NEAREST` ~1 source column in 12 sampled its neighbour (104,986 px of a GBA frame wrong). `highp` makes the pass bit-exact (0 px). Diagnostic: `touch /run/banana/3c-dump-now` writes `/data/3c-src.ppm` + `/data/3c-dst.ppm`; dst must equal `numpy.rot90(src, 3)`. | T1/T3 |
| F11 | Slang shaders and RetroArch overlays/bezels glitch on the compositor path (reported, not characterised). | `575cddb` |

## 3. Code map

| file | what | key symbols (line) |
|---|---|---|
| `ui/src/compositor.cpp` (1303 lines) | Wayland globals, client/surface/buffer lifetimes, frame scheduling | `start` 1047, `stop` 1193, `dispatch` 1234, `flush` 1242, `vblank` 1247, `resume` 1252, `frame` 1257, `setOverlayFb` 1262, `forwardInput` 1294, `forwardTouch` 1299 |
| `ui/src/egl_presenter.cpp` (856) | EGL import + 90° rotate into linear scanout slots, fence | `start` 539, `stop` 677, `render` 721, `takeFenceFd` 802, `flipDone` 828, `forget` 840 |
| `ui/src/presenter.{h,cpp}` | `Presenter` interface; `DirectPresenter` (3B, plane-scaled, unrotated) | `presenter.h:49-58` |
| `ui/src/rotate.h` | rotate quad (tested against `turn.h`) | — |
| `ui/src/kms.cpp` | atomic modeset, `presentGame`, overlay plane, `restore` | see AGENTS source map |
| `ui/src/main.cpp` | `residentGames` 1332, `runGame` 1711 (`resident` = run-retroarch only, 1718), reap loop 1818, card file 1895-1917, resume 2112-2127 | — |
| `system/bananad/bananad.c` | `compositor_on` 346, `overlay_open` 349, card writer 357-370 | — |
| `system/rootfs/usr/libexec/banana/compositor-mode` | prints `ui` or `cage` from `bananad.conf` | — |
| launchers `run-retroarch` (454-467), `run-port`, `run-bananatube` | choose direct client (`WAYLAND_DISPLAY=banana-0`) or `cage --` | — |

Runtime switches (all off unless the file exists): `/data/banana/3c-direct` (force DirectPresenter),
`/run/banana/3c-egl`, `/run/banana/3c-trace`, `/run/banana/3f-trace` (diagnostic traces).

## 4. Hardware loop (for any task with a hardware gate)

```
H=root@169.254.170.2                                            # USB NCM link (docs/RPC-TEST.md); SSH on in Settings > Network
# build + install the image the usual way (the user's update flow); never hand-copy over a RO root
ssh $H 'tail -n 50 /data/banana/logs/banana-ui.log'             # banana-ui stderr (banana-ui-session)
ssh $H 'grep -A6 -E "plane\[(38|44)\]" /sys/kernel/debug/dri/*/state'
ssh $H 'pidof cage; pidof retroarch'                            # cage must be empty on the resident path
```
Never run DRM-master tools on the host display. Run on the charger or ≥40% battery.

## 5. Task list (do in order)

Format per task: **goal · files · steps · verify · done when · stop if**.

### T1 — Characterise the shader/overlay glitch (F11) · `DONE (2026-11-10, see F14)` · needs: —
- **goal:** a written diagnosis, **no fix yet**.
- **files:** none edited except this plan (§2 new fact) and optionally a report in `docs/reports/`.
- **steps:** on the unit, launch one PSX game resident (a) plain, (b) with one Slang shader, (c) with one overlay/bezel.
  For each, capture: client buffer size/format/modifier from the banana-ui log, number of surfaces/subsurfaces,
  any presenter switch ("direct presenter" / "size change dropped" log lines), and a photo/description of the glitch.
- **verify:** the three captures exist.
- **done when:** one sentence names the cause class: (i) buffer size ≠ 1240×1080, (ii) second surface/subsurface,
  (iii) format/modifier change, or (iv) other (with the evidence).
- **stop if:** the glitch does not reproduce in (b) or (c) → record "not reproduced on <version>" and mark DONE.

### T2 — EGL presenter: scale + rotate any source size · `DONE (872ce06, 2026-11-10)` · needs: —
- **goal:** a client buffer of any size ≤ 1920×1920 is letterboxed (aspect kept) into the 1240×1080 logical
  frame and rotated, instead of falling back to the sideways DirectPresenter.
- **files:** `ui/src/egl_presenter.cpp`, `ui/src/rotate.h`, the existing rotate test next to `turn.h`.
- **steps:**
  1. In `rotate.h`, add a pure function that returns the destination quad for `(srcW, srcH)` → letterboxed in
     1240×1080, then rotated 90° clockwise. Unit-test it for 1240×1080 (identity to today), 640×480, 1280×720, 1080×1240.
  2. In `EglPresenter::render` (721), use that quad and clear the slot to black first; remove the
     "not 1240×1080 → direct presenter" switch and the "size change dropped" path (a size change re-renders).
  3. Keep slot import order (F5) and the fence (F6) untouched.
- **verify:** `make -s test`; on the unit: `tools/dmabuf-capture/sdl2-gl-client` at 640×480 and 1280×720
  shows upright and letterboxed; RetroArch PSX still 60 fps (`statistics_show`).
- **done when:** both sizes upright; no "direct presenter" line in the log.
- **stop if:** a GPU hang or a frame >16 ms in the 3F trace → revert, report.

### T3 — Fix the shader/overlay glitch · `DONE (872ce06, 2026-11-10; awaiting a visual check with lcd3x + an overlay)` · needs: T1, T2
- **goal:** fix only the cause class T1 named. If (i), T2 should already fix it — verify and close.
  If (ii), present only the toplevel's surface and composite subsurfaces only if T1 proved RetroArch uses one.
- **files:** decided by T1; at most `compositor.cpp` and `egl_presenter.cpp`.
- **verify:** T1's (b) and (c) look identical to cage.
- **stop if:** the fix needs a new Wayland protocol or a RetroArch patch → STOP, ask the user.

### T4 — Ports and BananaTube resident · `DONE (872ce06, 2026-11-10; video+audio playback not yet checked by hand)` · needs: T2
- **goal:** all three launchers run through the compositor when `compositor-mode` prints `ui`.
- **files:** `ui/src/main.cpp` (`runGame` line ~1718: drop the `run-retroarch`-only condition),
  `system/rootfs/usr/libexec/banana/run-port`, `run-bananatube` (mirror run-retroarch 454-467: set
  `XDG_RUNTIME_DIR=/run/banana/wayland`, `WAYLAND_DISPLAY=banana-0`, no `cage --`, no `cage.pid`; keep the
  `cage` branch for `compositor = "cage"`).
- **verify:** on the unit: BananaTube plays a video upright with audio, exits to the menu; one PortMaster port
  (install one if none) runs upright; `pidof cage` empty in both; the card (volume) shows over both.
- **stop if:** an app needs `wl_subcompositor`, `xdg_decoration` or another missing global → log its name, STOP.

### T5 — Wake path on the shipped image · `DONE (2026-11-10, 20 real cycles by the user)` · needs: —
- **goal:** confirm F8 on a **real** power-button wake (3ed4c85 was triggered manually; the daemon change ships in 0.5.17).
- **files:** none (only this plan).
- **verify:** on 0.5.17+: 20 sleep/wake cycles with a resident RetroArch game; each: game visible, audio on,
  no `panel restore failed` in the log.
- **stop if:** any cycle fails → capture `dmesg | tail -n 40` and the banana-ui log tail, STOP.

### T6 — Acceptance gate (trimmed 2026-11-10) · `IN PROGRESS` · needs: T3, T4, T5
Scoped to what the 4.2 changes (872ce06) can break. Record results in `docs/reports/compositor-acceptance-<date>.txt`:
1. **Pacing:** 10 min PSX (Vulkan) and Dreamcast (glcore) resident: ≥ 59.9 fps at the compositor (cage
   baseline 59.91, F7), no `drm`/`dpu` errors in `dmesg`, `banana-ui` + RetroArch RSS recorded. Runner:
   a device script that launches via `/run/banana/launch` and diffs the `egl: frame N` counter.
2. **BananaTube by hand:** one video plays upright with sound and exits to the menu, no cage.
3. **Escape hatch:** `compositor = "cage"` in `bananad.conf` → a RetroArch game, a port and BananaTube each run
   under cage (`pidof cage` non-empty) and return to the menu; remove the line afterwards.
- **Item 1 measured 2026-11-10** (dev build of 29fdbf9 + the pause menu, on the charger): Dreamcast (Flycast,
  glcore) **59.999 fps**, RSS banana-ui 72 MB / RetroArch 176 MB; PSX (SwanStation, Vulkan, Crash Bandicoot)
  **51.999 fps** ← FAIL, RSS 86 / 386 MB; 0 `drm`/`dpu` dmesg lines in both. Dreamcast at 60 on the same build
  points at SwanStation/the game, not the compositor: next step is the same 10 min under `compositor = "cage"`
  (if cage also shows ~52, it is not a T6 failure). Runner: `/data/t6-pace.sh` (`SECS=600`), output
  `/data/banana/logs/t6-pace.txt`.
- **stop if:** any item fails → record it, STOP; do not start T7.
- **Dropped, with reasons:** `sleep-test.sh` and real sleep cycles (bananad unchanged since 4.1b; T5 did 20
  real cycles); 100 pause-card cycles (input/grab code unchanged; 3E passed 50/50); `make sound-test` (no audio
  path change); burn-in guard (only the user's separate uncommitted edit touches it); A/B update / Previous
  version (untouched; exercised by the next release install); `make -s test` + `make bench` (passed 2026-11-10).

### T7 — Remove cage (next release after T6) · `TODO` · needs: T6 **and** one released image with T4
- **files:** `buildroot/configs/banana_retroid_pocket_classic_defconfig` (`BR2_PACKAGE_CAGE` and its comments
  106-113), the `cage` branches in the three launchers, `compositor-mode`, `/run/banana/cage.pid` users in
  bananad, the QEMU boot expectation (**ask the user first**: `tools/qemu-boot.sh` has their local changes).
- **verify:** `make -s`; image file list has no `cage`/`libwlroots`; QEMU boot test passes; T6 items 1, 3, 9 again.
- **stop if:** anything else links `wlroots` → report the package, STOP.

### T8 — Docs · `TODO` · needs: T7
`docs/ARCHITECTURE.md` §3 (`sed -n '16,23p'`) and §4 (`25,35p`): the handover is gone, banana-ui owns DRM;
README "What runs the show": drop cage; `docs/ROADMAP.md`: mark done with T6's numbers;
`system/rootfs/etc/banana/retroarch.cfg`: note the socket is banana-ui's (`banana-0`). Then set this plan's
header to "Done".

### T9 — Banana pause menu (replaces RetroArch's menu on MODE) · `DONE (fe28b3c, 2026-11-10)`
- `MODE` over a resident game opens a menu on the overlay plane and freezes the game (`signalTree(SIGSTOP)` on the
  launcher's whole process tree, so ports/BananaTube freeze too). RetroArch: Resume, Save state, Load state,
  Restart game, RetroArch menu, Quit game (via `/run/banana/retroarch.cmd`); others: Resume, Quit (SIGTERM to the
  launcher). Load/Restart/Quit ask "Press A again". B or MODE resumes. MODE + another key is a RetroArch hotkey
  combo and opens nothing.
- RetroArch's own menu moves to MODE + X on the compositor path only (`run-retroarch` appends
  `input_menu_toggle_btn = "3"` and restores the card's value after the game); cage keeps plain MODE.
- Fixed with it: a card changed while a flip was in flight never reached a frozen client's screen (the
  compositor now re-commits the overlay on the next vblank); a game ending under the menu releases the grab; a
  wake re-freezes the game under an open menu.
- **Two-column version (661d0c1, 2026-11-10):** header with battery/clock/time played; rows Resume, Save
  state / Load state with `< Slot N >` (0-9) and the slot's thumbnail on the right, Change disc `< Disc k/n >`
  (`.m3u` games only), Fast-forward On/Off (stays open), Shader (toggle), Achievements `n/total` (when
  `/run/banana/ra-progress` exists; the panel lists this session's unlocks and the newest badge), Restart,
  RetroArch menu, Quit. Integer scaling is **not** there: RetroArch has no command for it.
- **RetroArch stdin facts (1.22.2):** a query (`GET_STATUS`, `GET_CONFIG_PARAM`) on stdin **segfaults RetroArch**:
  never send one; the menu tracks slot/disc/fast-forward itself. RetroArch keeps one flag per command name per
  poll, so a repeated command in one frame runs once: banana-ui queues lines and sends one per 100 ms after the
  thaw. `retroarch.cfg`'s saved `state_slot` is not the slot RetroArch starts on, so `run-retroarch` pins
  `state_slot = "0"` per launch.
- Verified on the unit with a uinput pad: slot 3 saved to `.state3`, slot 1 loaded `.state1`, thumbnail shown,
  fast-forward/shader lines sent (shader on → off seen in the frame dump), achievements panel with test data.
  **Not tested:** Change disc (no `.m3u` game on the card).
- Verified on the unit with a uinput pad: every RetroArch row (state saved/loaded, `[Core] Reset.`, RetroArch's
  Quick Menu opened, quit status 0, config restored) and a port (frozen, Quit). **Not yet checked by hand:** a
  real MODE press no longer opening RetroArch's menu, MODE + X opening it, MODE + Start still quitting.

### Deferred (do not start without the user)
- Compressed/UBWC import via EGL (bandwidth win; F3 says direct import fails). Test a 1280-wide buffer first.
- Removing the 3C/3F diagnostic gates and traces.

## 6. STOP report format (≤5 lines)

```
TASK: T<n> attempt <1|2>  RESULT: <fail|blocked>
EXPECTED: <one line>
OBSERVED: <one line: log line / dmesg / plane state / test output>
DIFF: <what changed between attempts, files touched>
ASK: <the single decision needed from the user>
```
