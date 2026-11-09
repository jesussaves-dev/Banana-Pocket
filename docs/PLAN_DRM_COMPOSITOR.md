# Plan: a native DRM/KMS plane compositor (replacing `cage`)

Status: draft for `feat/drm-plane-compositor`. No compositor code, defconfig or launcher is changed by this plan.

## Terminology (read before the decision)

- **Direct plane presentation** — the client's frame reaches a KMS plane without a CPU/GPU render or copy into a
  compositor buffer. This is what "zero-copy" in older notes meant, and what a **GO (direct plane)** buys.
- **Zero composition cost** — *not* claimed and not what the probe measures. The DPU still composites: z-order,
  alpha/blending, and rotation (where the plane supports it) consume display-controller resources, and scanout
  bandwidth is still spent. "Zero-copy" never means "free".
- **GO (EGL rotate)** — the overlay works, but the plane cannot rotate the game frame: one GPU pass per frame is
  needed, so the copy is gone but a render pass returns.
- **Real-client DMA-BUF compatibility** is a **separate** requirement: the probe's frames are linear dumb buffers;
  RetroArch/SDL2 hand over tiled/compressed buffers with their own modifiers, and that import must be validated
  with a real client (Phase 2 spike), not inferred from a GO here.

## 0. What the code does today (verified in this tree)

- `banana-ui` opens `/dev/dri/card*` read/write, takes DRM master, makes two 1080×1240 XRGB8888 dumb buffers, and
  modesets one CRTC for the `DSI-1` connector with the legacy API (`ui/src/kms.cpp:114` `openSingle`, `:196`
  `restore`). Each frame it copies the damage rectangle and flips with `drmModePageFlip` (`:251` `present`,
  `:282`). Atomic KMS, universal planes, leases and DMA-BUF are not used anywhere in `ui/src`.
- On a game launch the menu writes `/run/banana/game.pid`/`game.path`, hands the **whole panel** over by closing its
  DRM fd (`input.close(); disp.close();` in `runGame`, `ui/src/main.cpp:1550`), forks, and blocks in `waitpid`
  (`:1612`). When the child exits it reopens the panel in a retry loop (`:1623`).
- `cage` is spawned by all three launchers — `run-retroarch:457`, `run-port:79`, `run-bananatube:35` — so today there
  is exactly one compositor, and while a game runs `banana-ui` has no display at all.
- Input is read by every process at once: `bananad` reads evdev "alongside banana-ui and the games … and grabs
  nothing" (`system/bananad/bananad.c:19`), the menu reads it directly (`ui/src/input.cpp`), and RetroArch is forced
  to `input_driver = udev` (`run-retroarch`: `set_cfg input_driver udev`). Nothing uses `EVIOCGRAB`.
- In-game notifications do not come from the menu: `bananad` writes `SHOW_MSG` lines into RetroArch's stdin FIFO and
  `buildroot/patches/retroarch/0014-banana-osd.patch` draws them; achievements are RetroArch popups restyled by the
  same patch. The menu's toasts only run after the game, from `/run/banana/cheevos`. Ports and BananaTube get none of
  this.

### Premises in the brief that do not match the tree

| Brief says | Tree says |
|---|---|
| "158/158 byte-identical screenshot tests" | No such test exists. `ui/tests/run.sh` compares 32×24 greyscale goldens with an RMSE threshold of 0.04, at two sizes, plus functional checks. |
| "0.54 ms scroll frame times" | `docs/PERFORMANCE.md`: scrolling ≈0.97 ms average at 640×480 on the host (≈2.8 ms at 1024×768); the device's 640×557 number is not in the repo. |
| "hands off the DRM master lease" | It closes the fd and drops master; there is no lease (`docs/ARCHITECTURE.md:18`: "goes to `cage` by being closed (no DRM lease)"). |
| `/run/banana/toast` | Does not exist. The paths are `/run/banana/{retroarch.cmd,hud-live,cheevos,osd-style}`; `/run/banana/toast` is only a test-harness comment (`ui/src/main.cpp:693`). |
| `/userdata/bgm.wav` | `/data/bgm.wav` or `/data/music/bgm.wav` (`ui/src/sound.cpp:143`). |
| "Adreno A12" | The kernel tree calls it `a613` (`buildroot/board/retroid-pocket-classic/linux-patches/0001-…-a613-catalog-entry.patch`); the firmware in use is `a630_sqe.fw`. This plan uses the tree's names. |

## 1. Architecture decision (read this before Phase 2)

**The goal "game on `PRIMARY`, menu/toasts on `OVERLAY`, `banana-ui` resident" is a compositor problem, not a DRM
lease problem.** Three facts decide it:

1. **One panel, one usable CRTC.** The DPU drives one DSI output. A lease hands objects to a lessee and removes them
   from the lessor's view; leasing the CRTC (the only one that can scan out to `DSI-1`) leaves `banana-ui` unable to
   show anything, overlay included. Leasing a plane without its CRTC is not usable: an atomic commit needs the CRTC.
2. **No client can consume a lease.** `drmModeCreateLease` exists in the image's libdrm (sysroot `xf86drmMode.h:480`),
   but RetroArch's KMS context, SDL2's KMSDRM and Mesa's Wayland WSI all expect either full master or a Wayland
   compositor. None accepts a lease fd.
3. **A DMA-BUF socket inside `banana-ui` needs the emulator to send frames.** RetroArch, SDL2 ports and BananaTube's
   mpv have no such output path. Adding one means patching every client (or injecting a library that speaks a
   protocol — which is a compositor again).

**Recommendation: Option D1 — `banana-ui` becomes a single-client Wayland compositor that owns DRM master and
assigns planes.** It keeps the contract the clients already speak and are tested against (`BR2_PACKAGE_SDL2_WAYLAND`,
RetroArch `--enable-wayland`), drops `cage` and `wlroots`, and lets `banana-ui` place the client's buffer on
`PRIMARY` and its own UI on an `OVERLAY` plane. Wayland stays; `cage` and `wlroots` go. If the requirement is
literally "no Wayland protocol at all", that is option D2 (patch RetroArch + SDL2 + mpv for a custom DMA-BUF
protocol) and should be rejected for v1 on cost and risk.

| Option | What it is | Verdict |
|---|---|---|
| A. Lease | `banana-ui` leases the CRTC/planes to the game | **Rejected for the overlay goal.** Single CRTC; the menu loses the display. Only reproduces today's full handover. The lease test belongs to the Phase 2 spike (2.1), as evidence, not to Phase 1. |
| B. DMA-BUF socket | Game sends frames to `banana-ui` over a socket | **Rejected for v1.** No client can produce it without patches to every frontend. |
| B′. Custom DRM compositor, clients speak Wayland | `banana-ui` owns DRM + the Wayland socket; `cage`/`wlroots` removed | **Recommended (D1).** Biggest piece of work, but the only one that meets the goal with the current clients. |
| C. Keep the handover | Game owns DRM; OSD drawn in-process (RetroArch patch) | **Fallback.** Today's behaviour. Works for RetroArch; ports/BananaTube get no OS overlay. |

**Fallback rules (decided by the Phase 1 probe, not by hope):** if no overlay plane exists that can scan out
1080×1240 ARGB8888 with `zpos` and alpha, or if plane rotation is absent and an EGL rotate costs more than 1 ms per
frame, or if the clients' DMA-BUF import fails, stop at option C and ship only Phase 1B (atomic menu) plus the
in-process OSD. Do not half-build a compositor.

---

## Phase 1 — plane discovery and the atomic test matrix

**This phase ships a probe and a hardware report.** It does not change `banana-ui`, the defconfig or the launchers,
and it does not replace `cage`. Moving `banana-ui` to atomic KMS is **1B**, and starts only after the report is in.

### 1.0 Which device this applies to

This tree is the Retroid Pocket Classic (`SM4450`, one DSI panel, native 1080×1240, menu 1240×1080). The brief names
a Retroid Pocket 6; the probe is connector-agnostic (it takes `--connector`), but a report is only valid for the
device and boot it was captured on. Never carry a result from one board to another.

### 1.1 The three questions, in order

1. Which planes can feed the `DSI-*` CRTC, with which formats and modifiers?
2. Does the driver *accept* — not advertise — game on `PRIMARY` plus menu on `OVERLAY`, with the rotation the
   portrait panel needs?
3. Based on 1 and 2: direct plane, one EGL rotate pass, or keep cage?

Only TEST_ONLY commits answer (2). Property lists answer (1) and are labelled "advertised" everywhere, because a
driver can advertise a format or a rotation it still rejects in a combination.

### 1.2 Read-only diagnostics (safe with the menu running)

```
cat /sys/class/drm/card*-DSI-*/{status,modes,enabled}
cat /sys/kernel/debug/dri/0/name
cat /sys/kernel/debug/dri/0/state        # debugfs is mounted by bananad
dmesg | grep -iE "msm|dpu|dsi|adreno"
```

`modetest` is not in the image; the probe below replaces it.

### 1.3 `tools/drm-plane-probe.c`

A standalone C11 tool (`libdrm` only), read-only by default:

```
drm-plane-probe [--card /dev/dri/cardN] [--connector DSI-1] [--take-master] [--no-atomic]
```

- **Enumeration** (works while `banana-ui` runs): kernel and DRM driver, DRM capabilities, connectors and modes,
  `panel orientation`, every CRTC with its active mode and `GAMMA_LUT_SIZE`/`OUT_FENCE_PTR`, and every plane with
  its type, `possible_crtcs`, formats, the **full `IN_FORMATS` set** (each modifier with its formats), linear
  AR24/XR24, `zpos` range/value/immutability, `alpha` range, `pixel blend mode`, `rotation`, fences, and the
  property ids the requests use.
- **Overlay choice:** the topmost (highest `zpos`) overlay that takes linear AR24; the summary prints both planes'
  `zpos` and whether the overlay can stack above the primary.
- **Atomic matrix** (only with DRM master and a valid mode blob): the six rows with sub-cases in 1.6. Every line is
  `[ok]`, `[reject] <errno> + cause`, or `[skip] <why>`; tested, rejected and skipped are counted separately.
- Framebuffers are created with `drmModeAddFB2WithModifiers(…, DRM_FORMAT_MOD_LINEAR)` (falling back to `AddFB2`
  only if the driver refuses the modifier form, and saying so).
- Row 2 fails when the overlay cannot stack above the primary, even if the kernel accepts the commit; unknown
  `zpos` makes the verdict HOLD, not NO-GO.
- Scaling has no DRM property to read (the DPU does not advertise a scaler limit): rows 5a/5b are the measurement.
- It **never** calls a commit without `DRM_MODE_ATOMIC_TEST_ONLY`; no page flip, no event, no modeset. It does not
  call `drmSetMaster` unless asked with `--take-master`, and drops it on exit.
- Exit: `0` = a decision (GO or NO-GO); `2` = HOLD/incomplete (skipped rows, no master, no mode); `1` = fatal.

**Why `--take-master` exists.** `DRM_IOCTL_MODE_ATOMIC` is registered with `DRM_MASTER`
(`drivers/gpu/drm/drm_ioctl.c:714`), so the kernel refuses it with `EACCES` even for `TEST_ONLY`. There is no
"harmless peek" while another process is master. The probe therefore reports the capability dump without master and
refuses to guess the matrix.

### 1.4 Running the atomic matrix (exclusive window)

The menu holds DRM master and init respawns it, so the matrix runs in a short, scripted window. Use
`tools/drm-probe-window.sh` on the handheld — not a hand-typed kill/mount sequence:

```
# on the host: build the probe for the handheld and copy both files
export SYS=output/retroid-pocket-classic/host/aarch64-buildroot-linux-gnu/sysroot
output/retroid-pocket-classic/host/bin/aarch64-linux-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    --sysroot=$SYS -I$SYS/usr/include/libdrm tools/drm-plane-probe.c -L$SYS/usr/lib -ldrm \
    -o /tmp/drm-plane-probe
scp -O /tmp/drm-plane-probe tools/drm-probe-window.sh root@<handheld>:/data/
ssh root@<handheld> 'chmod +x /data/drm-plane-probe /data/drm-probe-window.sh'

# on the handheld: it detaches, prints its log path, stops the menu, runs the probe, restores the menu
ssh root@<handheld> "/data/drm-probe-window.sh --connector DSI-1 --commit $(git rev-parse --short HEAD)"
# then watch the printed log and copy the report off:
ssh root@<handheld> 'tail -f /data/banana/logs/drm-probe-window-*.log'
scp -O 'root@<handheld>:/data/banana/logs/drm-probe-*.txt' .
```

What the wrapper guarantees:

- it stops the menu only after the stand-in bind mount is visible in `/proc/mounts`;
- the stand-in is a uniquely named script with a **recorded pid** (not `exec sleep`, whose argv would hide it);
- `autosleep_minutes`/`dim_minutes` are set to 0 for the window and restored afterwards; `bananad.conf` is put back
  only if nobody changed it during the window (otherwise only this script's block is removed, never the other
  change);
- the probe runs under `timeout 180`; `EXIT`/`INT`/`TERM`/`HUP` all restore;
- restore unmounts first (so init respawns the real launcher), kills the stand-in, HUPs bananad, and verifies
  `banana-ui` is running again;
- it detaches with `setsid`, so a dropped SSH session does not kill the restore path.

Preconditions it enforces: root, the probe and launcher present, no game running, `/data` not handed to a computer,
and either the charger connected or the battery at least 40%. Do not run it with a port or BananaTube open.

**Ultimate recovery.** If the wrapper is `SIGKILL`ed or the handheld loses power, the stand-in can remain in the
respawn slot and the panel stays on its last frame. **A reboot always restores the normal system** — the bind mount
is per-boot and cage is untouched. Hold Power to force it off if the menu does not come back.

Never run `--take-master` on the development host's active display. The matrix is for the target device, in this
window, on the charger.

### 1.5 What the output looks like

A **format template, not a measured result** (placeholders, no invented values):

```
banana drm-plane-probe 2
kernel  : <uname -s -r -m>
driver  : <drm driver name and version>
card    : /dev/dri/card0
compatible: <soc>,<board> (from /proc/device-tree/compatible; absent off-device)
caps    : dumb=<0|1> prime=<n> addfb2_modifiers=<0|1> async_flip=<0|1> timestamp_monotonic=<0|1>
connectors
  <DSI-N> (id <n>) connected, encoder <n>, <n> mode(s)   <- target
    mode 0: <W>x<H> @ <Hz> "<name>" preferred
    panel orientation: <n> (<name>) immutable
crtcs
  CRTC <id> (index <n>) active   <- target
planes (advertised; the atomic tests below decide what is actually accepted)
  plane <id> PRIMARY ... formats: ... rotation: ...
    IN_FORMATS: <n> modifier(s)
      0x<modifier> <name>: <formats>
  plane <id> OVERLAY ... zpos: <min>..<max> (now <n>) ...
target
  overlay plane: found (AR24 advertised) | NONE
  stacking: primary zpos <n>, overlay zpos <n> -> overlay above | OVERLAY NOT ABOVE
atomic tests (TEST_ONLY)
  [ok]     modeset: 1 ...
  [ok]     overlay band: 2a ...
  [skip]   4b primary rotated 90 (AR24): AR24 not advertised
  ...
  matrix: <n> ok, <n> rejected, <n> skipped
VERDICT: GO (direct plane) | GO (EGL rotate) | NO-GO | HOLD -- <why>
```

### 1.6 The atomic matrix

Six numbered rows, some with sub-cases (each sub-case is its own TEST_ONLY request, so a rejection names the exact
combination):

| # | sub-case | request | proves |
|---|---|---|---|
| 1 | — | connector `CRTC_ID`, CRTC `MODE_ID`/`ACTIVE`, primary `FB_ID`, SRC/CRTC, panel-sized | the base modeset is valid |
| 2a | band overlay | + overlay `FB_ID`, SRC/CRTC, zpos, alpha, premultiplied, 600×200 AR24 | a toast-sized layer stacks over the game |
| 2b | full-panel overlay | the same at the panel size | the planned full-panel menu layer is accepted |
| 3 | — | overlay rotated 90 | informational (the menu pre-rotates its own buffer) |
| 4a | game buffer, XR24 | primary 1240×1080 landscape, SRC landscape, CRTC portrait, `ROTATE_90` | **the direct-plane gate** |
| 4b | game buffer, AR24 | the same in AR24, if the primary lists it | the client's alpha format also rotates |
| 5a | scale | primary 640×480 → the panel | the scaler is available |
| 5b | letterbox | primary panel-width × (H−240) at destination offset (0,120) | non-zero destination offsets work |
| 6 | — | overlay scaled 600×200 → 300×400 | informational |

Each line is one of:

- `[ok]` — the driver accepted that exact combination (validated, not merely advertised).
- `[reject] <errno>` — the driver refused it; the line carries the plain cause (`EINVAL` property/format/modifier,
  `ENOSPC` bandwidth/plane, `EACCES` not master, `EBUSY` busy).
- `[skip] <why>` — the row could not run (no buffer, no feature). **A skip is never a rejection**: the verdict
  below distinguishes them.

Row 2 also fails if the overlay cannot stack above the primary (its `zpos` is fixed below the primary's), even
when the kernel accepts the commit — the kernel checks a valid state, not the layer you wanted. If `zpos` is not
exposed on both planes, stacking is unverifiable and the verdict is HOLD.

**Two levels of "supported":** *advertised* (property/format/modifier lists) and *validated* (a row `[ok]`). Only
validated rows feed the verdict. A format listed but rejected in a row is exactly the "advertised but not
supported" case.

**Real-client buffers.** These rows use linear dumb buffers via `AddFB2WithModifiers(LINEAR)`. RetroArch (Vulkan/GL)
and SDL2 hand over tiled/compressed DMA-BUFs with their own modifiers; the report lists the plane's full
`(format, modifier)` set, but importing a real client buffer is **separate validation** (Phase 2 spike) and cannot
be inferred from a GO here.

Exit codes: `0` = a decision (GO or NO-GO); `2` = HOLD/incomplete; `1` = fatal.

### 1.7 The hardware report

The wrapper writes `/data/banana/logs/drm-probe-<board>-<UTC>.txt` containing, before the matrix:

| field | source |
|---|---|
| board | `/etc/banana/board` (written at build: `retroid-pocket-classic`) |
| SoC + board compatible strings | the probe, from `/proc/device-tree/compatible` (e.g. `qcom,sm4450`, `retroid,pocket-classic`) |
| panel, connector, mode, plane capabilities | the probe |
| kernel (release, machine) | `uname -a` |
| DRM driver + version | the probe (`drmGetVersion`) |
| UTC date/time | the wrapper |
| probe commit | `--commit` from the host (`git rev-parse --short HEAD`) |
| probe binary SHA-256 | the wrapper (`sha256sum`) |
| the full six-row matrix and the verdict | the probe |

Copy the report off the card before any reboot, then fill in the review table:

| field | value |
|---|---|
| rows 2a/2b | ok / reject |
| stacking | overlay above / not above / unknown |
| rows 4a/4b | ok / reject / not advertised |
| rows 3, 5a, 5b, 6 | ok / reject / skip |
| exit code and verdict | |
| reviewed by / date | |

**Status of the measurements in this document: none.** The probe and wrapper are implemented; the probe has been
compiled for the host and with the image's `aarch64` toolchain, its enumeration ran on host DRM devices only (no
target values are quoted), and it is clean under AddressSanitizer/UndefinedBehaviorSanitizer. **The atomic matrix
has not been run on the target.** No result is copied from a host run, and none is invented.

**GO/NO-GO for Phase 2 right now: HOLD.** This is not a hardware NO-GO; the matrix is unmeasured. The GO is issued
only from a reviewed target report.

### 1.8 Buffer synchronisation contract (documented now, implemented in Phase 3)

If the matrix says GO, the compositor will import client buffers and hand them to the DPU. That needs, before any
code:

- **Import:** `drmModeAddFB2WithModifiers` with the client's fourcc, modifier, stride and offset. Reject a modifier
  the plane does not list (and test it with a real client buffer; see 1.6).
- **Produce → compose:** the client must not hand a buffer that is still being written. Use the plane's
  `IN_FENCE_FD` (acquire fence) per plane where advertised; otherwise the client must complete its write before the
  commit, or the compositor waits on a shared `sync_file`/dma-buf reservation first.
- **Compose → reuse:** a buffer may be reused only after the commit that last scanned it has completed. Use the
  commit's `DRM_MODE_PAGE_FLIP_EVENT` (and/or the CRTC `OUT_FENCE_PTR`) and keep the FB alive until then; only then
  release the buffer to the client (Wayland `wl_buffer.release`).
- **Lifetime:** hold a reference for every buffer in flight; on commit failure (`EBUSY`/`EINVAL`) keep the previous
  frame and do not release the new buffer until it is no longer referenced.
- **Never** assume implicit sync between GPU and DPU; the probe's fence lines say what is available.
- Two or three buffers per client, never one; never write a buffer the DPU may still be reading, including across
  suspend.

### 1.9 Decision criteria (GO / NO-GO / HOLD)

| measured result | verdict | next |
|---|---|---|
| rows 2a/2b `[ok]`, stacking verified, rows 4a/4b `[ok]` | **GO (direct plane)** | Phase 2 with the plane assignment as planned |
| rows 2a/2b `[ok]`, stacking verified, rotation not advertised or rejected | **GO (EGL rotate)** | Phase 2, but budget one GPU pass per frame |
| no overlay plane, or rows 2a/2b rejected, or stacking impossible | **NO-GO** | retain cage; Phase 1B can still ship |
| any required row skipped, no master, no mode, z-order unknown | **HOLD** | do not proceed; rerun when it can be measured |
| overlay has no AR24 / no alpha | partial | record it; opaque cards only, or a different plane |

The probe's `VERDICT:` line is a convenience; the decision is this table filled in from a reviewed target report.
No GO from a host run, from property lists alone, or from another board.

### 1.10 Phase 1B — atomic KMS in `banana-ui` (after the report)

Unchanged in intent, out of scope for this change: set `DRM_CLIENT_CAP_ATOMIC`/`UNIVERSAL_PLANES` in
`Display::openSingle` (`ui/src/kms.cpp:114`), find the primary plane for the CRTC, do the initial modeset atomically,
flip with `drmModeAtomicCommit(… PAGE_FLIP_EVENT | NONBLOCK)` in `present()` (`:251`), restore atomically (`:196`),
keep `drmModeCrtcSetGamma` until the night-light path is compared, and keep a `banana.kms=legacy` switch. The
software rotation (`turn()`) and both dumb buffers stay exactly as they are.

**Non-regression:** `--headless` never opens DRM, so the golden suite cannot catch a KMS regression; 1B needs the
hardware smoke mode (render N frames, flip, restore) and a before/after `make bench`.

### 1.11 Rollback

The probe is a tool on `/data`: delete it. 1B is behind `banana.kms=legacy`. Nothing in this phase changes the
image, the launchers or the fallback (cage).

## Phase 2 — The compositor: leases out, a single-client Wayland compositor in

**Goal:** `banana-ui` keeps DRM master while a game runs and owns the plane assignment.

### 2.1 Prove the lease answer (a day, not a phase)

Run `drm-plane-probe --lease-test` in an exclusive window (see 1.4). Expected, and the basis for option A being
rejected: the master can no longer touch the leased CRTC, so a menu overlay cannot be shown; the lessee can. Record
the exact `errno`s in the probe output.

### 2.2 Scope of the compositor (D1)

`banana-ui` creates `/run/banana/wayland/wayland-0` itself (the same path cage used, so the launchers only lose the
`cage --` prefix and `SEATD_VTBOUND`). Objects, in order of need:

- `wl_display`, `wl_compositor`, `wl_region`, `wl_subcompositor` (not required at first).
- `xdg_wm_base`: **one** toplevel, always fullscreen at the panel size; a second client is refused (kiosk rule).
- `wl_output`: mode 1080×1240, `transform` = the panel's orientation, scale 1.
- `wl_seat`: keyboard, pointer and touch. Gamepads are **not** forwarded — clients read evdev directly (see Phase 4).
- `wl_shm` and `zwp_linux_dmabuf_v1` (import): shm for software clients (mpv/SDL software), dmabuf for GL/Vulkan
  clients. Accept the modifiers the probe listed; reject unknown ones with a clear error.
- Renderer: import the client buffer, and if the probe showed hardware plane rotation, scan it out as-is; otherwise
  one EGL/GBM rotate pass into a GBM scanout buffer (the same work cage does today, so no regression, but the
  direct-plane saving is lost).
- Plane policy: game buffer on `PRIMARY`; banana-ui's own content on an `OVERLAY` (`zpos` top, `pixel blend mode` =
  premultiplied) when the overlay is visible; when no game runs, today's dumb-buffer path on `PRIMARY` is unchanged.
- Loop: the existing `poll()` gains the Wayland socket fds, a `signalfd` for `SIGCHLD`, and the existing timers. The
  menu must **not** block in `waitpid` any more; the child's exit is an event.
- Handover: keep `Sound::release()` and the game clock profile; **do not** `disp.close()`. The burn-in guard pauses
  while a client is focused. `game.pid`/`game.path` keep their meaning so bananad's sleep path is untouched.
- Sleep/wake: bananad still `SIGSTOP`s the game's process group; `banana-ui` stays master and, on `SIGUSR1`, re-commits
  the atomic state instead of reopening the panel; `/run/banana/cage.pid` becomes `/run/banana/compositor.pid` (or is
  dropped, since it is already running) and bananad's wake path is updated in the same change.
- The RetroArch patch stays: the in-process OSD is the fallback when the compositor path is off.

### 2.3 Fallback rules

- Probe shows no usable overlay plane, or no plane rotation and an EGL rotate > 1 ms/frame: **stop**, keep cage, ship
  Phase 1B only.
- Any client that cannot import or present through the new compositor: that launcher keeps `cage --` behind a
  per-system switch, and the release notes say which.
- No lease-based hybrid. No second CRTC.

**Acceptance:** RetroArch (Vulkan and glcore), one PortMaster port and BananaTube each run fullscreen with no cage
process (`pidof cage` empty); panel 60 fps with no tearing artefact on a checkerboard; suspend/resume with a game
running (20 cycles); audio alive on wake; the menu's return path has no "panel reopen" retries in the log.

---

## Phase 3 — Quick-menu and OS toast layer on the hardware overlay plane

**Goal:** volume/brightness, achievements, low battery, unplug and errors render above any client, without waking the
GPU.

1. **Buffer.** One full-panel 1080×1240 ARGB8888 dumb buffer is enough for v1 (≈5.2 MB; double-buffering only if the
   probe shows the plane needs it). Draw with the existing `soft.cpp` canvas and `Display::turn()` so the overlay is
   rotated exactly like the menu.
2. **Visibility.** Hidden is an atomic commit with the overlay plane `FB_ID = 0` (and `CRTC_ID = 0`); the DPU stops
   fetching it, so the cost is zero, not a transparent frame. Visible is `FB_ID` + `CRTC_ID` + `zpos` + `alpha =
   0xffff` + `pixel blend mode = premultiplied`. Verify both with the probe's plane state dump.
3. **Content source.** A new `/run/banana/overlay` file, written atomically (tmp + rename) by `bananad`:
   `kind|title|body|badge|duration_ms` (`volume`, `brightness`, `achievement`, `battery`, `unplug`, `error`).
   `banana-ui` already watches `/run/banana` with inotify; it reads the file and renders. When the compositor path is
   off, `bananad` keeps sending `SHOW_MSG` to RetroArch exactly as today — one flag decides, set at startup
   (`/run/banana/compositor`).
4. **Achievements.** `bananad` knows the badge path from `/run/banana/cheevos`; pass it through the overlay file and
   load the PNG with the existing `stb_image` cache. Keep `hud = 0` as the one switch that turns all of it off.
5. **Coverage.** Because this is compositor-level, ports and BananaTube get the same cards with no per-app work.
6. **Burn-in.** Cards auto-hide on their existing timers; a card that stays up (an error) follows the existing guard
   rules. Nothing static is left on the overlay plane while idle.

**Acceptance:** the cards are pixel-identical to the menu's at 1×; the volume card appears within one frame of the
key press; with the overlay hidden the plane's `FB_ID` is 0 in the probe dump and the idle power draw is unchanged
(compare 10-minute `charge_counter` deltas, overlay hidden vs. a transparent full-panel buffer); an achievement badge
from RetroArch and one from a port both show.

---

## Phase 4 — Input interception and `EVIOCGRAB` multiplexing

**Goal:** while the quick menu is up, the game gets no controller input; power and volume keep working.

1. **Ownership split.** `/dev/input` on the RPC has three kinds of devices: the `rsinput` MCU gamepad, the `FT5452`
   touch, and gpio-keys (power, volume). `bananad` owns power/volume and the F-combos; the menu owns the overlay.
2. **Grab only the pad and the touch**, only while the overlay is visible: `ioctl(fd, EVIOCGRAB, 1)` on the gamepad
   and touchscreen fds, never on gpio-keys. Volume and power therefore keep working during the overlay, and
   `bananad`'s sleep/shutdown path is untouched.
3. **Opening the overlay** from `BTN_MODE` (the menu key): grab **after** the opening press is released, so no key is
   down at grab time and RetroArch cannot be left with a stuck direction. Close on `B`/`BTN_EAST`, ungrab, and only
   then return input to the game.
4. **Coordination with `bananad`.** A `/run/banana/overlay-open` marker tells bananad its gamepad chords (F+B analog
   toggle, F+volume backlight) are suspended while the overlay is up; power/volume single keys are not affected.
5. **Seat forwarding.** Keyboard/pointer/touch go to the Wayland client over `wl_seat` when the overlay is closed, so
   ports and mpv keep their keyboard/mouse; gamepads stay direct evdev for every client, as today.
6. **Failure paths.** If `EVIOCGRAB` returns `EBUSY` (another holder), open the overlay anyway, log it, and pause the
   game through the existing FIFO (`PAUSE_TOGGLE`) so a press cannot leak into gameplay. Hotplug: the existing
   inotify watch re-opens and re-grabs.

**Acceptance:** 100 open/close cycles with no stuck input (hold a direction, open, release, close: the game must not
keep running); volume and brightness keys work during the overlay; RetroArch, a port and BananaTube all keep their
own controls; sleep while the overlay is open resumes with the overlay consistent.

---

## Phase 5 — Buildroot cleanup and the merge gate

### 5.1 Packages

- `BR2_PACKAGE_CAGE=y` (`buildroot/configs/banana_retroid_pocket_classic_defconfig:109`) goes. `wlroots` is only
  pulled by cage and disappears with it — verify in `output/…/build/*/` and in the installed file list.
- Keep: `wayland`, `wayland-protocols`, `libxkbcommon` (SDL2 Wayland and the new compositor), `libdrm`, `mesa3d`
  (+ freedreno/Turnip), `SDL2_WAYLAND`, `SDL2_KMSDRM`.
- Add only if actually used: `BR2_PACKAGE_LIBINPUT` (the recommendation is to reuse `ui/src/input.cpp` and skip
  libinput entirely), `BR2_PACKAGE_LIBSEAT` (not needed: no VT, no seat manager).
- `banana-ui`'s link line gains `wayland-server`; there is no new dependency for the DRM side (`libdrm` is there).
- Launchers: drop `cage --`, `SEATD_VTBOUND`, and the `cage.pid` writes in `run-retroarch`, `run-port`,
  `run-bananatube`; `run-retroarch`'s `video_context_driver` stays `vk_wayland` (the socket is the same).
- Keep one release of escape hatch: `banana.compositor=cage` on the kernel command line (or a `bananad.conf` key)
  that restores the old launchers and the cage package, so a field failure is a reboot away, not a reflash.

### 5.2 Acceptance on the handheld (all required before merging)

1. **Frame pacing:** 60.00 fps in RetroArch (`statistics_show`) over 10 minutes, dropped frames within 1% of the
   cage baseline, on PSX (Vulkan) and Dreamcast (glcore).
2. **No tearing/under-run:** a full-screen scrolling test pattern from `tools/` for 5 minutes; no `drm` errors in
   `dmesg`.
3. **Sleep/wake:** `system/bananad/tests/sleep-test.sh` 50/50; then 20 real press-to-sleep/wake cycles with a game
   running: audio present on the first frame (no `AUDIO_REINIT` wait), no modeset flicker, no panel left dark.
4. **Sound:** `make sound-test` on the unit, plus the in-game card and a port's audio.
5. **Burn-in guard:** with the menu idle, dim at 90 s and off at 180 s; with a game paused and the overlay open, the
   guard's rule is explicit and tested (recommended: the guard is suspended while a client is focused).
6. **Memory:** menu RSS at boot and with a game running; total of `banana-ui + cage + wlroots` before vs. after; the
   plan's target is a net reduction (cage and wlroots gone), and the overlay buffers must not exceed +12 MB.
7. **Input:** Phase 4's checks, plus the analog toggle and volume keys.
8. **Everything else:** `make -s test` (goldens included), `make bench` within noise, QEMU boot test still passes
   (cage is exercised there too — update the expectation), thermals unchanged, the update path untouched.
9. **Rollback:** `banana.compositor=cage` boots the old path; the A/B update and **Previous version** still work.

### 5.3 Documentation and merge

`docs/ARCHITECTURE.md` §3/§4 (the handover is gone; the compositor owns DRM), README's "What runs the show" (cage
leaves the list), `docs/ROADMAP.md` (this plan becomes Done with the measured numbers), and a note in
`system/rootfs/etc/banana/retroarch.cfg` that the Wayland socket is `banana-ui`'s.

---

## Risks, in the order they can kill the project

| Risk | Evidence it is real | Mitigation |
|---|---|---|
| msm planes cannot rotate 90° | The panel is portrait (1080×1240) while every client draws 1240×1080; cage does the turn today | Probe `rotation` on each plane; if absent, budget one EGL pass and the direct-plane saving is lost |
| DMA-BUF import fails for Vulkan/GL clients | Turnip + Vulkan WSI allocate from the driver; modifiers must match | Probe formats/modifiers; test with RetroArch first (Vulkan and glcore), then SDL2 |
| A single CRTC makes the overlay impossible | One DSI output; planes are tied to a CRTC | This is why leases are out; the compositor owns the one CRTC and composites everything |
| Being resident changes sleep | The menu currently leaves the panel to the game; bananad's SIGHUP/SIGUSR1 logic assumes cage | Explicit task in Phase 2 (`compositor.pid`, resume re-commit), tested in 5.2.3 |
| Input regressions | Three readers today, none grabbing | Grab only the pad/touch, only while the overlay is up, and only after the press is released |
| Atomic KMS regression in the menu | The renderer is hand-tuned and the tests do not open DRM | Legacy kill switch, `--kms-selftest`, before/after bench, 30-minute soak |
| Scope | A correct single-client compositor is thousands of lines, not 500 | Phase 1A ships regardless; Phase 2 stops at the fallback gate |

## Suggested sequencing

1. Phase 1A probe + report (this change); 1B atomic menu after the GO.
2. Phase 2 lease test (one day) → decision gate.
3. Phase 2 compositor spike on a branch: RetroArch only, no overlay, no ports.
4. Phase 3 overlay plane + bananad's `/run/banana/overlay`.
5. Phase 4 input grab.
6. Phase 5 ports/BananaTube, package removal, acceptance.
