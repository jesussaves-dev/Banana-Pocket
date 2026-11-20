# Banana Pocket Roadmap (RPC / SM4450)
Refs: `docs/ARCHITECTURE.md` (how it works, and why), `docs/RPC-TEST.md` (hardware checklist). Shipped work lives in `git log` and the
release notes, not here. Updated 2026-11-20 (rewritten around 1.0; the old *Done* list is in git history; goal grill added the rename, the
battery rule and the ship rule).

## Banana Pocket 1.0
**The promise: a stranger can install it, it never loses their progress, and sleep works like a console.** An item is
in 1.0 only if a stranger would notice it missing or broken. Sizes: **S** a day or two, **M** about a week, **L** a project.

**Audience:** non-technical Retroid Pocket Classic owners; 1.0 is RPC-only. **Trust rule:** never lose progress,
settings, saves, or battery that can be saved. Battery is a bug class, not a number: a wake, burst or drain with a
known fixable cause blocks 1.0; the hardware floor does not, and there is no mA target.

**Ship rule:** 1.0 ships when every gate is green and one sleep soak on hardware passes. A battery win found after that
goes into 1.0.1, not into a delay.

**Rule from now on:** every new user-facing string goes in through `tr()` with `es`/`de` entries in the same change
(`tools/check-lang.py` enforces keys), so the Languages phase 2 debt stops growing.

### Must-haves (in order)
1. **Sleep: the 12.7 s phantom burst.** Three wakes exactly 12.7 s apart at an unchanged battery %, then `bananad` gives up
   and wakes fully (2026-11-16 14:34). Not the gauge. Not seen again through 2026-11-19 (overnight 9.9 h, 12 mA, five
   slow phantoms that re-slept: `ipcc_0`/`glink-smem` +40-49, no power key, the battmgr ping). At the next burst, dump
   `wakeup_sources` and `/proc/interrupts` deltas per re-sleep before guessing. Stopgap: the give-up keeps the panel dark.
2. **Sleep: power-button sleeps that "did not take"** (17 in `sleep.log`, last 2026-11-18 22:01): woke 0-4 s later with
   "power key" and no kernel wakeup event. Not only the USB gadget (three at 14:40 were unplugged), and 5 sleeps since
   23:16 took, so the sample proves nothing. Next: about 20 quick sleep/wake presses, unplugged and on USB, after the
   signed flash.
3. **Signed releases: built, unshipped.** `update` verifies `sha256sums.sig` (ed25519, `tools/release-key.sh`) against
   `/etc/banana/release.pub` and refuses unsigned packages unless `/data/banana/allow-unsigned` exists. Left: back up
   `~/.config/banana/release-ed25519.pem`, commit the public key, build the image once (needs `openssl` in the
   image: `LIBOPENSSL_BIN`), and apply a signed and a tampered package on hardware. The first signed release must go
   out as a full flash.
4. **Sessions survive a hang (M):** two alternating hidden state slots on a timer, a fsynced journal in
   `/data/banana/sessions.tsv`, and a "cut off 4 minutes ago, resume?" card at boot. Quick resume today saves only on a
   clean quit (`run-retroarch:378`).
5. **Recovery screen without Android (M):** hold a button at power-on: boot the other slot, reset settings, fsck `/data`,
   transfer mode.
6. **Fresh-card welcome (S):** where ROMs go, the share and `banana.local` address with a QR code, missing BIOS files.
7. **Rename to Banana Pocket (S):** the public name. Change every user-visible string (boot splash, About screen, update
   package name, site, installer, flasher, release notes) through `tr()`; `banana-rpc` and `retroid-pocket-classic`
   stay as internal IDs. The README credits Jorre by name and his BananaLinux, which this code derives from. Check the kernel
   `-Bananix` string and `check-kver` before touching it.

### Headline features
- **`banana.local` (M):** a web page on the handheld: drop in ROMs and BIOS, download saves and a backup, read logs,
  apply updates. Replaces SMB/SSH for strangers and is the 1.0 answer to "saves off the card". QR in Settings > Network.
- **The game page (M):** art, description, facts, playtime (`playstats.tsv`), achievements. The scraper already receives
  the data and drops it.
- **Home button, game keeps waiting (L):** back to the library with the game frozen and a "running" badge; one press to
  return. Builds on the compositor's freeze and the pause menu.

### 1.0 checks (on hardware, ticked in `docs/RPC-TEST.md`)
- **Flasher 2.0.0 on real Windows:** one full flash and one mid-flash cancel (Windows paths are tested with mocks only).
- **Sleep means paused:** done, checked on hardware 2026-11-19 (patch `0015`, `SAVE_STATE_AUTO`, answer in
  `/run/banana/autosave`): sleep with a game open and Quick resume on wrote the `.auto` state in 40 ms before the freeze.
- **Sleep soak (the last gate):** 20 sleep/wake cycles with a game open plus one overnight, with no stuck wake, no lost
  audio, no `enc35 timeout`. Drain is accepted at the ~18 mA baseline (0.8.0 overnight, ~12 days standby); it is not a
  gate, but any avoidable waste the soak shows is.
- **Recovery and welcome** on a freshly flashed card, from the website's install path end to end.

## After 1.0
Not commitments. Banana owns the screen (it sees every frame, owns an overlay plane, can freeze and frame any game), has
its own daemon and A/B updates, and a CPU-drawn UI that costs nothing idle.

### Next in line
- **Languages, phase 2:** Settings sub-pages, menu chrome, system and app names, date/time formats, then more languages.
  Pipeline done (`ui/src/i18n.cpp`, `/usr/share/banana/lang/`, card override `/data/banana/lang/`); the main Settings
  list is translated (130 strings, informal tú/du).
- **Rumble for games without it (M):** per-game files map rcheevos memory conditions to vibrator patterns via `bananad`.

### New ideas
- **Living shelf (S-M):** box art wears with play: dust on the untouched, a creased spine on favourites, a sticker at 100 %
  achievements.
- **Banana Radio (M):** the menu BGM becomes a station of your own soundtrack rips (VGM/SPC/GSF on the card), with a
  now-playing ticker.
- **Ghost runs (M):** record inputs and replay your best attempt at a stretch as a translucent overlay.
- **Pass-and-play hot-seat (S):** turn timer and handover chime for turn-based games.
- **Banana Bulletin (S):** a weekly "this week you played…" card from `playstats.tsv` and `/run/banana/cheevos`.

### Compositor
- **Real-frame screenshots (S):** the exact panel frame without overlays (the `3c-dump-now` path).
- **Frame-time graph (S):** pause-menu toggle drawing fps/graph from the 3F trace, for ports too.
- **Filters for every game (M):** colour-blind modes, GBA colour correction, grayscale, in the rotate pass.
- **Guides over the game (M):** a pause-menu row showing a `.txt`/`.md`/`.png` walkthrough on the overlay.
- **Touch controls for ports (M):** translucent overlay layer injected via uinput, defined per port.
- **Handheld as a webcam (M):** UVC gadget function fed compositor frames.
- **Last-30-seconds clips (L):** frame ring buffer; hardware encode needs the `venus` driver, not in the kernel.

### Play sessions
- **Per-game memory (S):** restore brightness, volume, shader, fast-forward and slot (`pergame.conf` exists).
- **Quick-resume shelf (S):** last three games with their saved state on the home screen.
- **Session recap and play diary (S):** from `playstats.tsv` and `/run/banana/cheevos`.
- **Gentle limits (S):** "remind me after 1 h", bedtime reminder, a kid profile with a locked Settings.
- **Instant "off" (M):** power-off as a deep sleep, real shutdown only at low battery or after N days.

### Battery and sleep
- **More standby drain (M):** the queued `s2idle` comparison; the floor is ADSP firmware idle, not Linux (unloading the
  audio stack wedged the APM 2026-11-18: do not retry without an ADSP-side idea). `sleep.d/30-usb` earns nothing; candidate
  to drop.
- **Faster wake (S-M):** Wi-Fi coming back is the slow part (`sleep.d/10-wifi` always downs it); consider keeping the link.
- **Haptic cues (S)** for achievements, low battery, refused sleep.
- **Charge limit (S):** "80 % when plugged in" if the charger exposes `charge_control_end_threshold`.
- **Battery health (S):** cycle count and full-charge capacity.
- **AMOLED care (S):** drift static chrome by a pixel or two, a true-black theme, dim the bezel.
- **Headphone volume limit (S),** remembered per output.
- **Auto clocks from frame times (M):** step clocks down with headroom, up before a miss.

### Saves and sharing
- **Save sync over Wi-Fi (M+):** SMB/WebDAV to your own computer/NAS, keep both copies on conflict.
- **Achievement hunting (M):** all achievements by rarity, cached from the RetroAchievements API.
- **Two handhelds, one game (L):** RetroArch netplay via mDNS, started from the pause menu.

### Updates and install
- **Slot versions in Settings (S)** (version and build date per slot).
- **Offline update cache (S):** keep the last good package until the new slot boots once.
- **Field diagnostics bundle (S):** logs, `dmesg` tail, `settings.conf` (secrets stripped) into `/data/banana/backups`.
- **Delta updates (M):** zchunk/bsdiff against the previous release, full package as fallback.
- **macOS flasher (M)** and a **code-signed Windows `.exe`** (SmartScreen); Etcher and the manual route cover macOS today.

### Polish
- **Quick settings in the pause menu (S):** brightness, volume, Wi-Fi rows.
- **Sleep timer for BananaTube (S).**
- **What's new after an update (S):** a one-time card of headlines.
- **Per-game controls (M):** a Banana remap page replacing RetroArch's.
- **Faster to the menu (M):** show the menu while audio comes up (~1.8 s wait on cold boot; measurements in
  `git show 6288890:docs/BOOT-SOUND.md`).

### Parked
- **Bluetooth (designed 2026-11-08):** bespoke `banana-bt` over the kernel mgmt API, a small SDP client and `hidp`; no
  D-Bus or BlueZ; Classic HID only. Spec `docs/superpowers/specs/2026-11-08-bluetooth-design.md`. Kernel side ready.
  Blocked on a Classic pad (Pro 2/SN30 in D-input, or a PS4 pad); the 8BitDo Ultimate 2 here is BLE.

*Dropped by request: quick-settings/performance overlay (RetroArch's OSD covers it), screenshot gallery, collections/tags,
porting BananaTube's newer settings screen, accessibility / log viewer / battery-time (2026-11-04); 802.1X, static IP and
hotspot for Wi-Fi unless someone asks.*

## Field notes
- **Rule for device tests: never modify `/data/banana/settings.conf`;** keep any safety copy on the card, outside the tree
  under test, and verify the archive exists before deleting anything. (2026-11-03: a failed `tar -z` test reset
  `settings.conf` to the image template, turning off SSH/ADB and the theme. The OTA does not touch `/data`.)
- **Check `wake_audio_fast` in the card's `bananad.conf` before trusting a drain number** (on, it costs ~12 mA).
- **Unexplained, 2026-11-03 23:21:59 on 0.5.5:** `banana-ui-session` exited and `settings.conf` was rewritten, with no
  Restore pressed. If it repeats: `banana-ui.log.1` tail + `/run/banana/messages`.
- **`rcK` transfer mode:** the refused-handover branch (`usb-msc.mode = rw`, `/data` unmounted) is reasoned from code only;
  try it once on hardware with a real card.

## Maintenance
- **System shaders, two loose ends:** with a mask or colour active the first commits after a wake can fail `EBUSY` (at most
  5 frames, ~83 ms, not visible; cause unfound); and one `[dpu error] enc35 timeout pending` in ~60 wakes, after a failed
  `deep` suspend (`-95`, predates the shaders).
- **Agent DX:** `make agent-check` and `make agent-map`; keep the `AGENTS.md` Source Map fresh when core files move.

## Baseline
QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`. Nintendo
button layout (`A=east`). Single-screen RetroArch library only. 1240x1080 panel rotated 90°; games are Wayland clients of
`banana-ui`'s compositor (one EGL rotate pass). Mesa 26.2.6 (freedreno GL + Turnip Vulkan) on the a613, no local patch.
USB-C is an NCM gadget (DHCP `169.254.170.2`, SSH) plus adb (FunctionFS, TCP 5555 on its own switch).
