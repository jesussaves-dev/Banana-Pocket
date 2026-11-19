# BananaOS Roadmap (RPC / SM4450)
Refs: `docs/RPC-PORT.md` (port log), `docs/RPC-TEST.md` (hardware checklist). Shipped work is one line each under *Done*;
the detail lives in `git log` and the docs named there. Updated 2026-11-18.

## Recommended next (in order)
1. **The 12.7 s phantom burst.** A fast wake source that gives up after three re-sleeps and leaves the handheld awake.
   Dump `wakeup_sources` and `/proc/interrupts` deltas at the next burst before guessing.
2. **Power-button sleeps that "did not take"** (2026-11-18): woke 1-3 s later with "power key" and no kernel wakeup event,
   with the unit on the USB gadget. Retest unplugged before blaming code. Possible follow-up: hibernate after a long sleep.
3. **A real CRT personality (S)** (see *Ideas*). Below-minimum brightness is built; check it on hardware (RPC-TEST.md).
4. **The flasher on real Windows:** one flash and a mid-flash cancel; 2.0.0's Windows paths are tested with mocks only.
5. **Languages, phase 2:** Settings sub-pages and menu chrome.
6. **Bluetooth:** designed, not built; blocked on a Classic (BR/EDR) controller to test with.

## In progress

- **Sleep: cut the 26-42 mA standby drain** (overnight baseline 2026-11-17: ~18 mA, 99 to 95 % over 13 h on 0.8.0) (~140 mW at 37-42 mA; Android-class standby is 5-20 mW). Measured with the
  gauge's `charge_counter` (the awake line logs it).
  - *Slow phantom wakes = the battery gauge* (confirmed 2026-11-16): one wake per 1 % drop, 73-91 min apart, each re-slept
    dark (the 0.7.1 fix holds). They cost the wakes only, not the drain.
  - *Open, fast burst (2026-11-16 14:34):* wake at 85 s, then three more exactly 12.7 s apart at an unchanged 99 %, then
    `bananad` gave up and woke fully. A fixed period suggests a timer or firmware ping. Decide whether the give-up
    should leave the panel dark.
  - *Test 1, sound server (win):* `sleep.d/20-sound` stops PipeWire/WirePlumber for the sleep: 25-29 mA vs 37-42 mA (the
    AudioReach graph pinned by `session.suspend-timeout-seconds = 0` kept the ADSP up). `make sound-test` passes 14/14 after.
  - *Test 2, USB gadget (no win):* `sleep.d/30-usb` earns nothing (26-29 mA either way); candidate to drop.
  - *2026-11-11:* with `qcom,rpmh-stats`, a 76 min `deep` sleep drew ~32 mA and `aosd`/`cxsd`/`ddr` stayed at 0. APSS and
    WPSS slept; the ADSP never did, with LPASS clock votes held (`LPASS_HW_DCODEC` 6, `TX_CORE_MCLK` 3). The ADSP cannot be
    stopped to test (it runs `qcom_battmgr` and `ucsi_glink`). Next: unload the audio drivers.
  - *Audio drivers unloaded: no win, and unsafe (2026-11-18).* With the menu released and the server down, `rmmod` of the
    sound stack leaves `snd_soc_lpass_va_macro`, `macro_common` and `q6prm_clocks` stuck (`EAGAIN`; sysfs `unbind` is
    denied), so `TX_CORE_MCLK`/`TX_CORE_NPL_MCLK` (and `LPASS_HW_DCODEC` 2) stay voted. Pulling `q6apm` out also wedged
    the ADSP's APM (`CMD timeout` x3), and the card never came back by `modprobe`; only a reboot restored it (sound-test
    14/14 after). Do not retry without an ADSP-side idea. The remaining lever is the ADSP firmware's own idle, not Linux.
  - *Queued:* an `s2idle` sleep for comparison. If nothing moves it, the floor is firmware S2R depth and the rest is on the
    wake side. (Unbinding `ath10k_snoc`/WPSS was tried: 69 mA, no win.)
  - *Wake side:* 57-60 ms from resume to a drawn menu, kernel resume ~1.05 s; Wi-Fi coming back is the slow part
    (`sleep.d/10-wifi` always downs it). Consider keeping the link across suspend.
  - *A game's audio after a wake (fixed 2026-11-09):* `bananad` waits for the server and sends `AUDIO_REINIT` to the game
    (~1.7 s). **Fast wake audio** (Settings > Sleep & power, default off) keeps the server up instead, at ~12 mA more.

- **Bluetooth (designed 2026-11-08).** Bespoke `banana-bt` over the kernel mgmt API, a small SDP client and `hidp`; no
  D-Bus or BlueZ; Classic HID only. Spec `docs/superpowers/specs/2026-11-08-bluetooth-design.md`, plan (9 tasks) in
  `docs/superpowers/plans/` (local notes). The kernel side is ready (`qcom,wcn3950-bt` node, `bluetooth`/`hci_uart`/`btqca`/
  `hidp` modules, `qca/cmbtfw13.*` firmware). Blocked: the pad here (8BitDo Ultimate 2) is BLE; needs a Pro 2/SN30 in
  D-input or a PS4 pad. BLE and audio would be the BlueZ design, deliberately not taken.

- **Languages: Spanish and German.** Pipeline done (`tr()` in `ui/src/i18n.cpp`, JSON catalogs in
  `/usr/share/banana/lang/`, card override in `/data/banana/lang/`, a Language row). The main Settings list is translated
  (130 strings, informal tú/du); `tools/check-lang.py` runs in `make test`. Next: sub-pages, menu chrome, system/app names,
  date/time formats, then more languages.

## Ideas
Not commitments. Banana owns the screen (it sees every frame, owns an overlay plane, can freeze and frame any game), has
its own daemon and A/B updates, and a CPU-drawn UI that costs nothing idle. Sizes: **S** a day or two, **M** about a
week, **L** a project.

### Picked to build
- **Below-minimum brightness (S):** built, untested on hardware. At the 2% floor a further F+volume-down steps the picture
  scale 100/70/50/35% (`bananad` -> `/run/banana/pic-scale` -> `colorLutFill`); up undoes it first. Menu or compositor only; a bare-RetroArch game cannot change the LUT (it keeps the menu's scale until the menu returns).
- **A real CRT personality (S):** with a CRT mask on, collapse to a line and dot on sleep (animate the plane size), degauss
  thump via the vibrator and a ~20 s colour warm-up on wake.
- **Rumble for games without it (M):** per-game files map rcheevos memory conditions to vibrator patterns via `bananad`.

### Compositor
- **Real-frame screenshots (S):** save the exact panel frame without overlays (the `3c-dump-now` path). *Gallery dropped 2026-11-04.*
- **Frame-time graph (S):** pause-menu toggle drawing fps/graph from the 3F trace, for ports too.
- **Filters for every game (M):** colour-blind modes, GBA colour correction, grayscale, in the rotate pass.
- **Guides over the game (M):** a pause-menu row showing a `.txt`/`.md`/`.png` walkthrough on the overlay.
- **Touch controls for ports (M):** translucent overlay layer injected via uinput, defined per port.
- **Handheld as a webcam (M):** UVC gadget function fed compositor frames.
- **Home button, game keeps waiting (L):** return to the library with the game still frozen and a "running" badge.
- **Last-30-seconds clips (L):** frame ring buffer; hardware encode needs the `venus` driver, not in the kernel.

### Play sessions
- **Per-game memory (S):** restore brightness, volume, shader, fast-forward and slot (`pergame.conf` exists).
- **Quick-resume shelf (S):** last three games with their saved state on the home screen.
- **Sleep means paused (S): built, untested on the unit (2026-11-18).** `bananad` asks RetroArch for the quit-time `.auto`
  state (patch `0015`, `SAVE_STATE_AUTO`, answer in `/run/banana/autosave`) before it freezes the game; needs Quick resume
  on, skips a game the pause menu froze, gives up after 1.5 s. Needs an image build (`make`) and a sleep with a game open.
- **Session recap and play diary (S):** from `playstats.tsv` and `/run/banana/cheevos`.
- **Gentle limits (S):** "remind me after 1 h", bedtime reminder, a kid profile with a locked Settings.
- **The game page (M):** art, description, facts, playtime, achievements; the scraper already receives the data and drops it.
- **Sessions that survive a hang / autosave on crash (M):** two alternating hidden state slots on a timer, a fsynced journal
  in `/data/banana/sessions.tsv`, and a "cut off 4 minutes ago, resume?" card at boot.
- **Instant "off" (M):** power-off as a deep sleep, real shutdown only at low battery or after N days.

### Battery and hardware
- **Haptic cues (S)** for achievements, low battery, refused sleep.
- **Charge limit (S):** "80 % when plugged in" if the charger exposes `charge_control_end_threshold`.
- **Battery health (S):** cycle count and full-charge capacity. *Battery-time estimate dropped 2026-11-04.*
- **AMOLED care (S):** drift static chrome by a pixel or two, a true-black theme, dim the bezel.
- **Headphone volume limit (S),** remembered per output.
- **Auto clocks from frame times (M):** step clocks down with headroom, up before a miss.

### Saves and sharing
- **Save sync over Wi-Fi (M):** SMB/WebDAV to your own computer/NAS, keep both copies on conflict.
- **A web page on the handheld (M):** `http://banana.local` for uploads, saves, logs, settings, updates; QR code in Settings > Network.
- **Achievement hunting (M):** all achievements by rarity, cached from the RetroAchievements API.
- **Two handhelds, one game (L):** RetroArch netplay via mDNS, started from the pause menu.

### Updates, recovery, trust
- **Slot versions in Settings (S)** (version and build date per slot).
- **Offline update cache (S):** keep the last good package until the new slot boots once.
- **Field diagnostics bundle (S):** logs, `dmesg` tail, `settings.conf` (secrets stripped) into `/data/banana/backups`.
- **Delta updates (M):** zchunk/bsdiff against the previous release, full package as fallback.
- **Recovery screen without Android (M):** hold a button at power-on: other slot, reset settings, fsck, transfer mode.
- **Signed releases (M):** verify a signature before writing a slot.

### Polish
- **Quick settings in the pause menu (S):** brightness, volume, Wi-Fi rows.
- **Sleep timer for BananaTube (S).**
- **Fresh-card welcome (S):** where ROMs go, share address and QR, missing BIOS files.
- **What's new after an update (S):** a one-time card of headlines.
- **Per-game controls (M):** a Banana remap page replacing RetroArch's.
- **Faster to the menu (M):** show the menu while audio comes up (~1.8 s wait on cold boot).

## Field notes
- **Rule for device tests: never modify `/data/banana/settings.conf`;** keep any safety copy on the card, outside the tree
  under test, and verify the archive exists before deleting anything. (2026-11-03: a failed `tar -z` test reset
  `settings.conf` to the image template, turning off SSH/ADB and the theme. The OTA does not touch `/data`.)
- **Unexplained, 2026-11-03 23:21:59 on 0.5.5:** `banana-ui-session` exited and `settings.conf` was rewritten, with no
  Restore pressed. If it repeats: `banana-ui.log.1` tail + `/run/banana/messages`.
- **`rcK` transfer mode:** the refused-handover branch (`usb-msc.mode = rw`, `/data` unmounted) is reasoned from code only;
  try it once on hardware with a real card.
- **Update packages piling up:** fixed after 0.6.0 (`update prune`, run 2 min into a boot).

## Maintenance
- **System shaders, two loose ends:** with a mask or colour active the first commits after a wake can fail `EBUSY` (at most
  5 frames, ~83 ms, not visible; cause unfound); and one `[dpu error] enc35 timeout pending` in ~60 wakes, after a failed
  `deep` suspend (`-95`, predates the shaders).
- **Agent DX:** `make agent-check` and `make agent-map`; keep the `AGENTS.md` Source Map fresh when core files move.

*Dropped by request: quick-settings/performance overlay (RetroArch's OSD covers it), screenshot gallery, collections/tags,
porting BananaTube's newer settings screen, accessibility / log viewer / battery-time (2026-11-04); 802.1X, static IP and
hotspot for Wi-Fi unless someone asks.*

## Baseline
QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`. Nintendo
button layout (`A=east`). Single-screen RetroArch library only. 1240x1080 panel rotated 90°; games are Wayland clients of
`banana-ui`'s compositor (one EGL rotate pass). Mesa 26.2.6 (freedreno GL + Turnip Vulkan) on the a613, no local patch.
USB-C is an NCM gadget (DHCP `169.254.170.2`, SSH) plus adb (FunctionFS, TCP 5555 on its own switch).
Hardware validation (`docs/RPC-TEST.md`) is in progress.

## Done
- [x] **Landing page, web installer, flasher 2.0.0 (2026-11-16).** `site/` on GitHub Pages; `install.html` flashes OpenABL over
  WebUSB via fastbootd; flasher 2.0.0 with the Blast theme and drive-safety fixes. See `docs/INSTALL.md`.
- [x] **0.8.0 Genesis style, Banana Blast (2026-11-16).** Third Style option; art baked by `tools/gen-blast.py` to Mega Drive
  rules, FM cues and funk loop (`tools/gen-blast-sounds.py`), three palettes, the Blue Banana secret. Street extras: toasts,
  dithered box art, closed-shop folder, wake mosaic.
- [x] **0.7.0 SNES style, Banana Street (2026-11-11).** Default on a new card; shops on a street, art from `tools/gen-street.py`,
  five neighborhoods, workshop Settings, ticket pause menu. Since 0.7.1: ripening-banana battery, ledger pages, pill and
  notices, touch in the workshop, its own sounds and `"bgm"` theme key. Device checks passed 2026-11-16.
- [x] **System shaders (2026-11-11).** CRT/aperture/LCD masks on the overlay plane, Trinitron/PVM/handheld colour via the CTM,
  night light on `GAMMA_LUT`; 0 drops over 43 wakes.
- [x] **Compositor bezels.** Frames on the overlay plane for RetroArch and ports, per system and per game, sized to the hole.
- [x] **0.6.1 / 0.6.0: one screen, one owner (2026-11-10).** `banana-ui` is the in-process Wayland compositor on atomic KMS;
  `cage` and `wlroots` removed. Bit-exact EGL rotate, Banana's pause menu (frozen game, slots, discs, shaders), pill and
  notices, boot click waits for sound. 60 fps measured on Vulkan, PSX and Dreamcast. `docs/PLAN_DRM_COMPOSITOR.md`.
- [x] **Wi-Fi management (0.5.12):** WPA2, WEP, saved-network priority, on-screen keyboard.
- [x] **32x32 icon set (0.5.12)** and **screen bezels for GB/GBC/GBA (0.5.12).**
- [x] **Banana Bridge and in-game pill (0.5.11):** volume, brightness, low battery, headphones, achievements through
  RetroArch's message line; the ALSA C API for the volume keys.
- [x] **Hardening sweep (0.5.10, 2026-11-05):** daemon, BananaTube, UI and i18n fixes, sleep hooks under test, dead weight removed.
- [x] **BananaTube:** Pocket shell and near-free idle (0.5.9); tabs, recents, quality cap; Local media; captions, Subs,
  Offline, play-next queue; picture restored under the new compositor.
- [x] **Resume last game at boot (0.5.8).** Menu ready 7.80 -> 6.41 s with a pending resume.
- [x] **Theme validation, WEP, saved-network priority, per-core/per-game remaps (RetroArch's own), back up & restore** (0.5.x).
- [x] **Audio:** PipeWire + WirePlumber + UCM2 (0.5.0), menu audio after updates/sleep/games, sound test (`make sound-test`,
  0.5.3), follow-ups (0.5.4), pre-PipeWire code removed.
- [x] **The 03:40 hang instrumented (0.5.2);** the sound-handover race closed.
- [x] **USB mass storage and gadget reliability (0.5.x);** **adb** (0.4.57-0.4.59) with its own switch; loopback brought up.
- [x] **OTA updates over Wi-Fi (0.5.x);** **library UX** (multi-disc `.m3u`, jump-to-letter); **audit fixes and pruning (0.5.4).**
- [x] **Dropped or closed:** the on-device self-test (the pieces exist, 2026-11-05); Wii/GameCube removed and Mesa 26.2.6
  (Turnip a613 topped out at 40-45 fps); thermals needed no tuning (peak 55 °C, fan never engaged); Sleep/Wake closed in 0.4.54
  (spurious-wake guard; no RTC wake alarm on this board, so a low battery refuses sleep).
