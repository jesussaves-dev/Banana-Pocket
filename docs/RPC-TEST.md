# Retroid Pocket Classic: what to check on the hardware

Write the card (from the host, with the card in a reader):

    sudo tools/write-sd.sh output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz /dev/sdX

Then put it in the handheld and power on: OpenABL (already in `abl_a`/`abl_b`) reads
`extlinux/extlinux.conf` from the card's FAT partition, so the card boots whether or not the
internal Android is there.

## First boot

- [ ] The panel lights up and the Banana Pocket splash plays (white screen, BANANA flying in, sparkle, the banana bunch)
- [ ] The menu comes up **upright** and fills the panel, not sideways or upside down
      (if it is turned: add `banana.rotate=0`, `90`, `180` or `270` to the `APPEND` lines in
      `/boot/extlinux/extlinux.conf` -- no rebuild needed)
- [ ] The first-time setup runs: Wi-Fi lists networks, the time zone changes the clock
- [ ] The menu's text and art are sharp (the UI draws at 1240x1080, 1.9375x the 640-wide layout)

## Panel and power

- [ ] Below-minimum brightness: at the lowest backlight F+volume-down three more times dims the picture (card reads 1%),
      F+volume-up undoes it before the backlight rises, the Settings slider resets it, and it survives a reboot
      (`/data/banana/bl-scale`). Watch dark gradients for banding.
- [ ] Brightness (Settings > Display & sound) changes the backlight over its whole range
- [ ] Sleep (Settings > Sleep & power, or the power button) turns the panel off, and waking it
      shows the menu again -- this is the ROCKNIX backlight bug, watch for a black screen
- [ ] Charging shows in the status bar, and the percentage rises

## Sound

- [ ] Menu sounds play (Settings > sounds on)
- [ ] A game's music and effects come out of the speaker (one mono AW88166 amp)
- [ ] Headphones: plugging in mutes the speaker

## Buttons and sticks

- [ ] D-pad, A/B/X/Y, L/R, Start/Select move through the menu as the hints say
- [ ] Every button registers in Settings (the gamepad is the MCU's `rsinput` device)
- [ ] The 6-button variant's extra buttons (if this is one) work

## Settings

- [ ] The pages are Display & sound, Sleep & power, RetroArch, Network, System, Frames, Shaders
- [ ] Sleep when idle is off by default here (a slate has no lid to close): the screen dims after two idle minutes and
      stays awake; the power button still sleeps it, and Settings > Sleep & power or `autosleep_minutes` in
      `banana/bananad.conf` asks for the timer back

## System shaders

Settings > Shaders. `tools/drm-probe-window.sh --color` (with `tools/drm-plane-probe`) shows on the panel what the
hardware can do, without the menu.

- [ ] Each Screen mask draws at once in the menu; Mask strength moves on each step
- [ ] Each Screen colour looks different, and black stays black
- [ ] The mask and colour survive sleep/wake, in the menu and with a game running
- [ ] In a game with a console frame, the lines match the game's pixels and stay off the frame
- [ ] MODE + Select takes the mask off and puts it back; after quitting that game the saved mask is back
- [ ] The volume card and pause menu draw over the mask, with and without a frame
- [ ] With the mask off and no frame, `plane[44]` in `/sys/kernel/debug/dri/1/state` shows `crtc=(null)`

## Fan and heat

- [ ] The fan spins up under a game and settles when idle (`/sys/class/hwmon/*/pwm1`)
- [ ] Nothing gets too hot after 10 minutes of a N64/PSX game

Profile it rather than guess. Copy `tools/rpc-thermals.sh` to the card and run it while the game plays, then send the
log back (a line a second: every thermal zone, the fan's duty and tachometer, the CPU clusters' clock and the GPU's):

    scp tools/rpc-thermals.sh root@169.254.170.2:/data/
    ssh root@169.254.170.2 sh /data/rpc-thermals.sh 600 > thermals.txt

The curve itself is in the rev Q DTS (`.../sm4450-retroid-pocket-classic-revq.dts`): off below 60 C, then 60/120/180/255
of the fan's range at 60/70/80/88 C, `step_wise`. Tune the trips there and rebuild.

## Matched benchmark runs

To compare a game under RetroArch and under the native `banana-retro` runtime (or a build, a setting, the stock
Android) on this SoC, collect pacing, heat and battery power for the *same* run. Three tools, one run id:

- `tools/rpc-emu-bench.sh <secs>`: thermals, fan, clocks, load, memory and the emulator's CPU share, a line a second.
- `tools/rpc-power-sample.sh <secs>`: battery voltage/current/power from `/sys/class/power_supply`, and the energy.
- `tools/rpc-run-join.sh bench.log power.log`: accepts the pair only when it is one valid run.

Choose a run id (1-64 of `A-Za-z0-9._-`) and give it to all of them. Unplug the USB gadget cable and use a network link
for ssh: a cable makes the usb supply `online=1`, and the power run is rejected on purpose (`charger`).

    ID=mgba-native-1
    # on the unit, one shell each; start the sampler first, then the game, then the bench once the game runs
    RPC_RUN_ID=$ID RPC_OUT=/data/power.log sh /data/rpc-power-sample.sh 120
    BANANA_RETRO_TELEMETRY=1 BANANA_RETRO_RUN_ID=$ID <the native launch> 2> /data/telemetry.log
    RPC_RUN_ID=$ID RPC_EMU=banana-retro RPC_TELEMETRY_LOG=/data/telemetry.log RPC_OUT=/data/bench.log \
        sh /data/rpc-emu-bench.sh 60
    # after the bench's window closes, quit the game; then, on the computer
    sh tools/rpc-run-join.sh bench.log power.log

- [ ] The bench's last line is `# valid=yes`. Rejected runs exit 3 and name why: no process at the start, an exit,
      a replacement or an extra match inside the window, a process still alive after `RPC_COLLECT_SECS`, a missing,
      stale, duplicated or foreign telemetry report, an output error. Stop the game *after* the window: the native
      process writes its `Telemetry run:` line and frame statistics only as playback ends
- [ ] The power log says `valid=yes`: status `Discharging`, plausible readings, the whole window covered. Its units
      are checked for plausibility only (a current in milliamps where microamps are expected is not detectable), so
      cross-check `avg_mW` against a known load once per unit. Use `avg_mW`/`energy_mWh`, not the coarse capacity
- [ ] The join says `valid=yes`: same real run id, one window each, and the power window covers the bench's. The
      energy is for the *power* window, so start it just before and stop it just after the bench for a per-run figure
- [ ] A RetroArch run for the baseline uses the same ids and the same bench; only the telemetry log is native-only

How the pair is judged, and what the numbers mean:

- Every instant is an integer second (`date +%s`, truncated), so each window boundary is known to within 1 s. The join
  accepts a power window that covers the bench's within `RPC_JOIN_TOL` s (default 2: `window-not-covered`), that begins
  at most `RPC_JOIN_MAX_EXTRA` s (default 10) before the bench's (`power-start-too-early`), ends at most that long after
  it (`power-end-too-late`) and is at most twice that longer overall (`power-window-too-long`). The energy is for the
  power window, so its error against the bench window is up to the extra time on both sides plus the truncation; keep
  the margins small and compare runs with the same margins. These limits are policy, not a measured accuracy
- The bench needs sampling coverage: no gap between samples above `RPC_MAX_GAP` s (default 3: `sample-gap`), at least
  half as many samples as seconds (`sparse-samples`), a window of at least the requested length (`short-window`) and a
  clock that does not go backwards (`clock-backwards`). Its window line ends with `samples=` and `max_gap=`
- Native frame statistics cover the native *process lifetime*, from the start of paced playback to its end, not the
  bench window: the process prints them once as it exits. A longer game than the window therefore has more frames than
  the bench saw. Compare frame statistics only between native runs of equal play time; RetroArch prints no such line
- A zombie (state `Z`/`X`) counts as an exited process, never as a match; identity (start time) is still checked
- `Audio status:` is unchanged without telemetry (`accepted= overflow= underrun= xrun= reopens= queue= delay=`). With
  `BANANA_RETRO_TELEMETRY=1` the line is printed once, with `input_rate=` and `output_rate=` first, after the frame line
- Known limits, accepted at review (host-qualified only; none is a measured accuracy):
  - **Power source (`RPC_POWER_SRC`).** The unset default is the legacy rule: `power_now` when positive, else voltage
    x current. `RPC_POWER_SRC=vi` always uses voltage x current (a derived estimate); `RPC_POWER_SRC=gauge` always
    uses `power_now` and never falls back (`missing-p`, `zero-power`, `range-p`, `bad-reading` reject the sample).
    Any other value, including empty, exits 2. Every sample records `req=` and `src=`, and the header and summary
    record `power_src_requested=` and `power_src=` (`gauge`, `vi`, `mixed` or `none`). The default can mix sources
    within a run; use an explicit mode for a comparison. All validity thresholds are unchanged in every mode
  - **Why.** In the first G2 run (`~/task11-preserve/g2/power-g2-ra-mgba-1.out`, invalid) the SM4450 `power_now` stayed
    at 126433 mW (~127 W, `range-p`) while voltage x current was about 1.1-1.7 W (4247 mV x 399 mA). Treat the gauge
    as unreliable on this unit until shown otherwise, and use `vi` for such a run
  - `vi` is an estimate, not a measurement: the units are never detected and its accuracy is unqualified. Cross-check
    `avg_mW` against a known load (an external meter) once per unit before trusting absolute figures
  - `warn=gauge-disagree` (and `gauge_disagreement_samples=` in the summary) flags a sample whose readable gauge,
    including zero, differs from voltage x current by more than 20% of voltage x current. It is a diagnostic only,
    never a validity reason. When either reading is unavailable the sample is not comparable (`warn=n/a`)
  - `counter_energy_mWh` / `counter_vs_energy_pct` compare `|charge_counter delta| x average mV` with the integrated
    energy. Signed 32-bit counter readings are accepted; malformed or out-of-range readings are ignored. This is
    supporting evidence only, without a threshold: the counter is coarse (uAh steps, slow update)
  - Both join modes validate the measurement samples, window, counts, energy and consistent source metadata; missing,
    duplicate, malformed or contradictory evidence is rejected. The bench log states no power source. Never compare
    energy across sources: `sh tools/rpc-run-join.sh --compare-power a.log b.log` exits 3 unless both logs are valid
    and used the same source. Logs written before source metadata was added are unsupported and rejected
  - The power sampler does not check the gap from its start to its first sample; the join's `RPC_JOIN_TOL` and
    `RPC_JOIN_MAX_EXTRA` bound that window edge. The bench does check it
  - The power sampler accepts `RPC_INTERVAL` larger than `<secs>` (one sample held for the whole interval, with
    `elapsed_s` showing the real length). Use an interval well under `<secs>`; only a paired join bounds the length
  - If the sampler's stdout fails after its log summary was written, the log relies on a trailing
    `# valid=no reasons=output-error` line; if that append also fails (full disk) the log still reads `valid=yes`
    while the exit status is 3. A qualifying run needs both exit statuses below
  - The sampler test's cleanup (`pkill -P`) misses `timeout` children inside a pipeline subshell; they are bounded by
    their own timeout, so this is test hygiene only
- Hardware qualification requires exit status 0 from `rpc-power-sample.sh` and from `rpc-emu-bench.sh` (record both,
  e.g. `echo $?`), as well as `valid=yes` in their logs: a nonzero status (3 = rejected or output error) voids the run
  whatever the log says

Nothing here is a hardware result until a person runs it on the unit: the host tests (`make test`,
`system/tests/rpc-power-sample-test.sh`) use a mock `/sys` and clock and qualify the scripts' bookkeeping, not any
reading. The log is a regular file: `RPC_OUT` pointing at a device (`/dev/full`, `/dev/null`) is reported as an
error rather than read back.

## Card and data

- [ ] Games on the card's `banana-data` partition show in the library
- [ ] A save survives a reboot

## Updates

- [ ] Settings > System > Update lists a package put in `banana/updates` on the card, and
      applying it writes the other slot and restarts into it

## USB link (development)

The handheld's own port is a USB **device**, and it runs an Ethernet (NCM) gadget with a DHCP server, so plugging it
into a computer gives that computer a network interface **and an address** with nothing to configure -- no Wi-Fi.
`ssh` has to be on for a shell (Settings > Network), and its key is the same one the Wi-Fi link uses.

- [ ] Plugged into a computer, the handheld answers at `169.254.170.2` (the interface is named from the USB path:
      `enp0s20f0u1` on a systemd host, `usb0`/`enx…` elsewhere), around 4 ms and 7 MB/s over `ssh`
- [ ] If nothing enumerates, re-seat the cable: the port attaches when the cable is in a computer, and the gadget is
      bound at boot whether or not one is there. Run `sh tools/rpc-usb-trace.sh 40` (pushed to the card) across a
      re-plug: a single `configured` and no `adbd exited` lines is a good plug
- [ ] Reverse-orientation hotplug (charges, no interface): the port reports `orientation=reverse` and `bananad`
      re-binds the gadget within ~3 s, so the interface appears without a reboot (`usb: re-bound the gadget ...` and
      `usb: typec ...` lines in `/run/banana/messages`). If it does not, replug; `tools/rpc-usb-trace.sh` records it
- [ ] Settings > System > **Mass storage**: Off shows no disk; Read-only gives the computer a read-only exFAT volume
      (`roms/`, `saves/`) while the menu keeps running; Read-write stops the menu and hands the card over (copy a file
      on from the computer), and ejecting -- or `usb-msc off`, or a short press of the power button (the menu is
      stopped in this mode) -- mounts `/data` and restarts the menu with the file present. The cable re-enumerates
      once per toggle, not in a loop
- [ ] A charge-only cable, or a charger, gives you this instead: the handheld charges, `/sys/class/udc/*/state` stays
      `not attached` and no interface appears on the computer
- [ ] With the menu showing, `tools/rpc-screen.sh` fetches a frame of it (`-w` streams them for a viewer); a game
      running gives no frame -- that screen is RetroArch's, and its `SCREENSHOT` command is the way (on the command
      FIFO bananad already writes `TOGGLE_ANALOG_DPAD` to)



The panel going black a second after a game starts has two very different causes, and the handheld looks the same for
both. Tell them apart before touching the kernel:

    sh /data/rpc-hang-watch.sh start 1200        # the kernel's buffer onto the card, synced every second
    /data/rpc-gpu-probe offscreen                # the GPU on its own: prints "freedreno ... FD613" and 120 frames
    killall banana-ui; /data/rpc-gpu-probe dumb   # the panel on its own: a modeset and 64 flips, no GPU
    killall banana-ui; /data/rpc-gpu-probe kms    # both together: the GPU's buffers scanned out

Build the probe on the computer with `tools/rpc-probe-build.sh` (it uses the image's own cross toolchain) and copy
both tools to the card's `/data` with scp; they are not part of the image — they are for a machine that can be read
afterwards.

- All three pass and the game is still black: it is RetroArch's own setup, not the hardware. Its log is
  `/data/banana/logs/retroarch.log` and the keys that must be right (`video_driver` above all — `"null"` there is a
  black panel with the game running under it) are forced by `run-retroarch` every start.
- A probe hangs (the box stops answering, `sh /data/rpc-hang-watch.sh show` after the power button ends the story at
  the step the probe printed last): that is kernel territory — the DPU splash handover, the GPU's power domains. Send
  that file.
- `offscreen` alone hangs: the GPU, its firmware (`/lib/firmware/qcom/sm4450/`) or its clocks.

## What to send back

- Photos of the screen at the menu and in a game
- `logs/boot.txt`, `logs/dmesg.txt`, `logs/messages.txt` and, for a game that would not show anything,
  `logs/retroarch.log` and `logs/hang-watch.*.txt` from the card's `banana/logs`
- Anything on the panel, sound or buttons that is wrong, with the setting or game
