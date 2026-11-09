# Plan: a native DRM/KMS plane compositor (replacing `cage`)

Status: draft for `feat/drm-plane-compositor`. No code, defconfig or launcher is changed by this plan.

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
3. Based on 1 and 2: zero-copy, one EGL rotate pass, or keep cage?

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

- **Enumeration** (works while `banana-ui` runs): kernel and DRM driver, connectors and modes, `panel orientation`,
  every CRTC with its active mode and `GAMMA_LUT_SIZE`/`OUT_FENCE_PTR`, and every plane with its type,
  `possible_crtcs`, formats, `IN_FORMATS` modifiers, linear support for AR24/XR24, `zpos` range/value/immutability,
  `alpha` range, `pixel blend mode` values, `rotation` values, and fence properties.
- **Overlay choice:** the topmost (highest `zpos`) overlay that takes linear AR24; the target summary prints both
  planes' `zpos` and warns when the chosen overlay does not stack above the primary (fixed `zpos` is common).
- **Atomic matrix** (only with DRM master): the six TEST_ONLY cases in 1.6, each printing `[ok]` or `[reject]` with
  `errno` and a plain-English cause. It also prints a `VERDICT:` line from what was measured.
- Scaling has no DRM property to read (the DPU does not advertise a scaler limit): rows 5–6 are the measurement.
- It **never** calls a commit without `DRM_MODE_ATOMIC_TEST_ONLY`; no page flip, no event, no modeset. It does not
  call `drmSetMaster` unless asked with `--take-master`, and drops it on exit.
- Exit: `0` matrix ran; `2` enumeration only (the usual case while the menu is up); `1` fatal (no card, no resources).

**Why `--take-master` exists.** `DRM_IOCTL_MODE_ATOMIC` is registered with `DRM_MASTER`
(`drivers/gpu/drm/drm_ioctl.c:714`), so the kernel refuses it with `EACCES` even for `TEST_ONLY`. There is no
"harmless peek" while another process is master. The probe therefore reports the capability dump without master and
refuses to guess the matrix.

### 1.4 Running the atomic matrix (exclusive window)

The menu must not hold master, and init respawns it. On a dev unit, with a card you can reboot:

```
# a stand-in for the menu that init may respawn
printf '#!/bin/sh\nexec sleep 86400\n' > /tmp/no-menu.sh && chmod +x /tmp/no-menu.sh
mount --bind /tmp/no-menu.sh /usr/bin/banana-ui-session
kill $(pidof banana-ui); sleep 1
pidof banana-ui && { echo "menu still up: stop here"; exit 1; }

scp -O /tmp/drm-plane-probe root@<handheld>:/data/
ssh root@<handheld> '/data/drm-plane-probe --take-master | tee /data/banana/logs/drm-probe-$(date +%Y%m%d-%H%M).txt'

umount /usr/bin/banana-ui-session
kill $(pgrep -f /tmp/no-menu.sh) 2>/dev/null      # init respawns the real session
```

- The panel keeps the last frame while the menu is down; the TEST_ONLY commits change nothing.
- Do not run it with a game, port or BananaTube open: they own the display.
- A reboot always restores the normal session.
- If `drmSetMaster` fails with `EBUSY`, something still holds the display; the probe says so and skips.

### 1.5 Example of the report format

Synthetic and trimmed: it shows the shape of the report, not any device's result.

```
banana drm-plane-probe
kernel : Linux ... aarch64
driver : msm 1.0.0 (0) "msm"
card   : /dev/dri/card0
objects: 1 connector(s), 1 encoder(s), 1 CRTC(s), 0 framebuffer(s)

connectors
  DSI-1 (id 45) connected, encoder 44, 1 mode(s)   <- target
    mode 0: 1080x1240 @ 60 Hz "1080x1240" preferred
    panel orientation: 3 (right-side-up) immutable
crtcs
  CRTC 41 (index 0) active   <- target
    mode: 1080x1240 @ 60 Hz "1080x1240"
planes (advertised; the atomic tests below decide what is actually accepted)
  plane 50 PRIMARY  possible CRTCs: 41 ... formats: XR24 AR24 ... rotation: rotate-0 rotate-90
  plane 51 OVERLAY  possible CRTCs: 41 ... zpos: 0..255  alpha: 0..65535  rotation: rotate-0 rotate-90
target
  overlay plane: found (AR24 advertised)
  primary rotation advertised: rotate-90
atomic tests (TEST_ONLY)
  [ok]     modeset: primary at the panel's mode
  [ok]     primary + overlay (zpos top, alpha 50%, premultiplied)
  [ok]     primary rotated 90 (the game buffer: the zero-copy gate)
  ...
VERDICT: GO (zero-copy) -- ...
```

### 1.6 The atomic matrix

Each row is a separate TEST_ONLY request, so a rejection names one combination:

| # | request | proves |
|---|---|---|
| 1 | connector `CRTC_ID`, CRTC `MODE_ID`/`ACTIVE`, primary `FB_ID`, panel-sized XR24 | the base modeset is valid |
| 2 | + overlay `FB_ID`, `SRC`/`CRT`, `zpos` top, `alpha` 50%, premultiplied | the menu layer can sit over the game |
| 3 | overlay rotated 90 | informational (the menu can pre-rotate; not a gate) |
| 4 | primary `FB_ID` landscape (panel H×W), `SRC` landscape, `CRT` portrait, `rotation=ROTATE_90` | **the zero-copy gate**: the game frame can be rotated by the plane |
| 5 | primary scaled 640×480 → panel | the DPU scaler can upscale, if a core outputs low |
| 6 | overlay scaled + alpha | the toast band can be scaled, if ever needed |

Reading a failure: `EINVAL` means a property, format, modifier or the combination was refused; `ENOSPC` means no
plane/bandwidth; `EACCES` means the probe is not master (fix 1.4, not the hardware). The probe prints all of these
next to the failing row.

**Two levels of "supported" are reported:** *advertised* (the property/format list) and *validated* (row `[ok]`).
The verdict uses only validated rows. A format listed but rejected in the test is exactly the "advertised but not
supported" case the brief asks about, and it is printed as such.

### 1.7 The hardware report to fill in

Capture the probe output on the unit and record:

| field | value |
|---|---|
| device, board, kernel | |
| DRM driver + version | |
| target connector, mode, `panel orientation` | |
| CRTC count, target CRTC, `GAMMA_LUT_SIZE` | |
| planes for that CRTC (types, ids) | |
| primary formats + linear AR24/XR24 | |
| overlay formats + linear AR24/XR24 | |
| primary/overlay `rotation` values | |
| `zpos` range/immutable, `alpha` range, blend modes | |
| rows 1–6 | ok / reject + errno |
| verdict | |

Nothing in this table is filled in until the probe has run on the real device.

**Status of the measurements in this document: none.** The probe has been compiled for the host and with the image's
`aarch64` toolchain, and its enumeration has been exercised on host DRM devices only (no display result is quoted
here); the host enumeration also runs clean under AddressSanitizer/UndefinedBehaviorSanitizer. The atomic matrix has **not** been run: there is no target device in this session, and taking DRM master on
the development host's active display would disrupt it. No result is copied from a host run, and none is invented.

**GO/NO-GO for Phase 2 right now: HOLD.** This is not a hardware NO-GO — it means the matrix is unmeasured. The GO
is issued only by a filled-in report from the target device with rows 2 and 4 `[ok]` (zero-copy) or row 2 `[ok]` and
row 4 rejected/absent (EGL rotate). Any other result is NO-GO, retain cage.

### 1.8 Buffer synchronisation contract (documented now, implemented in Phase 3)

If the matrix says GO, the compositor will import client buffers and hand them to the DPU. That needs, before any
code:

- **Import:** `drmModeAddFB2WithModifiers` with the client's fourcc, modifier, stride and offset. Reject a modifier
  the plane does not list.
- **Produce → compose:** the client must not hand a buffer that is still being written. Use the plane's
  `IN_FENCE_FD` (acquire fence) per plane where advertised; otherwise the client must complete its write before the
  commit, or the compositor waits on a shared `sync_file`/dma-buf reservation first.
- **Compose → reuse:** a buffer may be reused only after the commit that last scanned it has completed. Use the
  commit's `DRM_MODE_PAGE_FLIP_EVENT` (and/or the CRTC `OUT_FENCE_PTR`) and keep the FB alive until then; only then
  release the buffer to the client (Wayland `wl_buffer.release`).
- **Lifetime:** hold a reference for every buffer in flight; on commit failure (`EBUSY`/`EINVAL`) keep the previous
  frame and do not release the new buffer until it is no longer referenced.
- **Never** assume implicit sync between GPU and DPU; the whole point of the probe's fence lines is to know what is
  available on this driver.
- Two or three buffers per client, never one; never write a buffer the DPU may still be reading, including across
  suspend.

### 1.9 Decision criteria (GO/NO-GO)

| measured result | decision | next |
|---|---|---|
| overlay row 2 `[ok]` **and** primary rotation row 4 `[ok]` | **GO — zero-copy** | Phase 2 with the plane assignment as planned |
| overlay row 2 `[ok]`, row 4 not advertised or rejected | **GO — EGL rotate** | Phase 2, but budget one GPU pass per frame; "zero copy" is off the table |
| no overlay plane for the CRTC, or row 2 rejected | **NO-GO** | retain cage; Phase 1B (atomic menu) can still ship |
| overlay has no AR24 / no alpha | partial | opaque cards only, or an ARGB primary and a second plane; record it |

The verdict line the probe prints is a convenience, not the decision: the decision is this table filled in from the
device's report, reviewed before Phase 2 starts. No GO from a host run, from another board, or from property lists
alone.

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
  one EGL/GBM rotate pass into a GBM scanout buffer (the same work cage does today, so no regression, but "zero GPU
  copy" must then be dropped from the goal).
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
| msm planes cannot rotate 90° | The panel is portrait (1080×1240) while every client draws 1240×1080; cage does the turn today | Probe `rotation` on each plane; if absent, budget one EGL pass and drop "zero GPU copy" |
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
