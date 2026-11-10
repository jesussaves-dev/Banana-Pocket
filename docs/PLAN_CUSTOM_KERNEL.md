# PLAN: Banana's own kernel (RPC / SM4450)

Written 2026-11-10 · status: **proposal, nothing started** · owner decision needed per phase (kernel work needs explicit
approval under `AGENTS.md` §4, and this plan is that request, phase by phase).

## 0. What "our own kernel" can honestly mean

**Whose code counts as ours.** Banana Pocket (Tom) and Jorre's project (JorreFog) are separate repositories with the
same soul: Tom fixes bugs in Jorre's repo, each maintains his own, and **code by either of them is "ours"** and free to
reuse here. The long-term goal is a completely custom distro: patch out, over time, everything that is neither
Banana's nor Jorre's — except the generic Linux core and the firmware, which no one can reasonably own (below).

Banana already does **not** run someone else's kernel tree: it builds **mainline Linux 7.2** straight from kernel.org
and adds a patch queue, a config and a device tree on top. So the realistic goal is not "write a kernel"; it is to
**own every layer that is specific to this handheld** — the configuration, the device tree, the device's own drivers
and the patch queue — and to depend on others only for the generic core (scheduler, memory, filesystems, USB, the
Qualcomm subsystem drivers) that thousands of people maintain.

Three facts frame every recommendation below:

1. **License.** Anything that is part of the Linux kernel is GPL-2.0. "Our own" means *Banana wrote it and maintains
   it*, not that it is closed or exclusively ours. Device trees can stay BSD-3-Clause (they already are).
2. **Firmware is not replaceable.** The ADSP (audio), WPSS (Wi-Fi), the GPU's `a630_sqe.fw`, the AW88166 amp's ACF and
   the AudioReach topology are Qualcomm/vendor binaries loaded *by* the kernel; they are signed or undocumented. No
   kernel choice removes them. (They are 30 MB of `board/.../firmware`.)
3. **The kernel is not the slow part of the boot.** It hands over to init at **0.49 s**. The 5–6 s before sound is the
   ADSP firmware starting (`docs/BOOT-SOUND.md`), which no kernel change moves.

## 1. Where it stands (2026-11-10)

| Layer | Today | Whose |
|---|---|---|
| Kernel source | `linux-7.2.tar.xz` from kernel.org (`BR2_LINUX_KERNEL_CUSTOM_TARBALL`) | mainline |
| Config | `linux-rocknix.config`: **8,565 lines, ~2,011 `=y`, 267 `=m`** — ROCKNIX's whole distro config | ROCKNIX |
| Config fragments | `linux-banana.fragment` (47 lines), `linux-qemu.fragment` (17) | **Banana** |
| Patches | `linux-patches/`: **45**, ~4,700 added lines | **34 named outside authors** (23 by one SM4450 bring-up author, sunshineinabox; the 2,152-line interconnect driver is Qualcomm's), **2 Banana** (0062, 0063), **9 with no author line** (0011, 0018, 0030, 0046–0049, 0051, 0052), added to this repo by Tom — **to confirm**: if Tom (or Jorre) wrote them they are ours; none of the 45 is by Jorre |
| Device tree | `dts/qcom/sm4450-retroid-pocket-classic*.dts[i]`: 3,328 lines, BSD-3-Clause | adapted, in-tree here |
| Kernel image | `Image` ~**30.7 MB** (slot status), plus modules | — |
| Bootloader | OpenABL (`arm-efi`, `extlinux.conf`) | OpenABL project |

The patches by area (what each phase would touch):

| Area | Patches | Lines (approx.) | Notes |
|---|---|---|---|
| SoC enablement (clocks, regulators, power domains, interconnect, IOMMU, PHYs, remoteproc) | 0002–0008, 0011–0014, 0017–0018, 0022, 0040–0043, 0060 | ~2,600 | generic SM4450 support; belongs upstream |
| Display + GPU (DPU, a613, panel CH13726A) | 0001, 0009, 0010, 0015, 0019 | ~500 | panel timings are this device's |
| Input (rsinput pad, touch power) | 0016, 0059, 0061 | ~810 | the pad driver is the device's identity |
| Audio (I2S clocks, AW88166, WCD937x, SoundWire, q6apm) | 0030–0034, 0039, 0056, 0062, 0063 | ~480 | two are Banana's |
| Wi-Fi/Bluetooth, USB-C, battery | 0024, 0045–0049, 0051–0054 | ~290 | |
| Scheduler hints (cpu capacity, energy model in DT) | 0057 | 17 | |

## 2. Recommendations, ranked

| # | Phase | Effort | Risk | Payoff | Recommend |
|---|---|---|---|---|---|
| K0 | Safe test loop + baseline numbers | **S** (1–2 days) | low | everything else depends on it | **yes, first** |
| K1 | Banana's own config from scratch | **M** (1–2 weeks) | medium | the single biggest "it's ours" win: every option chosen and documented; smaller, faster to build | **yes** |
| K2 | Own the device tree | **S–M** (3–5 days) | low | the hardware description becomes Banana's, documented line by line | **yes** |
| K3 | A kernel repo with a rebase workflow + CI | **S** (2–3 days) + ongoing | low | stops "45 loose patches"; makes 7.3, 7.4… routine | **yes** |
| K4 | Banana-written device drivers (pad, board/platform driver) | **M** per driver (1–2 weeks) | medium | the parts that make it a Banana Pocket are Banana code | **yes, after K1–K3** |
| K5 | Rewrite or retire each community patch (patch-by-patch audit) | **M–L** (weeks, incremental) | medium | fewer foreign patches; each one understood | **yes, incremental** |
| K6 | Upstream the SoC/board support to mainline | **L** (months, review cycles) | low (to the device) | the real way to carry *no* patches: they become mainline | **yes, long game** |
| K7 | Boot and runtime specialisation (built-ins, async probe, EAS, I/O) | **S–M** | low–medium | small measurable gains | optional |
| K8 | Identity (version string, `uname`, panic/oops handling, logo) | **S** | low | the visible "Banana kernel" | yes, with K1 |
| ✗ | Write a kernel from scratch / a Rust OS | **XL** (years) | — | none for a handheld: no GPU, DSP or Wi-Fi drivers | **no** |
| ✗ | Rewrite mainline subsystems (msm DRM, ASoC q6, interconnect framework) | **XL** | high | fork maintenance forever | **no** |
| ✗ | Replace firmware blobs | impossible | — | — | **no** |
| ~ | Own bootloader instead of OpenABL | **L** | **high** | a fully Banana boot chain | later, only after K0–K4 |

**Recommended order:** K0 → K1 (+K8) → K2 → K3 → K4 (pad driver first) → K5 continuously → K6 in the background.
Roughly **6–8 weeks** of focused work gets a kernel whose config, device tree, workflow and device drivers are all
Banana's, with the generic SoC support still community code on its way upstream.

## 3. Phases (executor format: goal · files · steps · verify · stop if)

### K0 — Safe test loop and baseline · S
- **Goal:** test a kernel without risking the working system, and have numbers to compare against.
- **Why safe:** the whole OS lives on the **microSD card**, and each slot has its own kernel (`/boot/a`, `/boot/b`).
  A kernel that does not boot is fixed by re-writing the card on a PC — the handheld's internal Android is untouched.
- **Steps:**
  1. Find out whether OpenABL's `extlinux.conf` handling supports a boot menu, `TIMEOUT` or a one-shot entry. If yes,
     add a `LABEL test` entry that boots `/boot/test/Image` once and falls back. If not, test kernels go to the
     inactive slot with `update apply` (slot A keeps the release; **Previous version** cannot help if the new kernel
     never reaches the menu, so keep a known-good card image at hand).
  2. A `tools/kernel-report.sh` that records: `Image` size, module count/size, `dmesg` time to init, rcS marks,
     `lsmod`, `/proc/config.gz` (needs `CONFIG_IKCONFIG_PROC=y`), and which drivers actually probed
     (`/sys/bus/*/drivers/*/`).
- **Verify:** a report for the shipped 0.6.0 kernel in `docs/reports/kernel-baseline-<date>.txt`.
- **Stop if:** OpenABL has no fallback and you are not comfortable re-flashing the card — then every later phase uses
  the slot method only.

### K1 — Banana's own kernel config · M
- **Goal:** replace `linux-rocknix.config` (8,565 lines, written for many devices) with `banana_rpc_defconfig`, built
  up from `allnoconfig`, where every enabled option is there for a reason this device has.
- **Files:** new `board/retroid-pocket-classic/linux-banana.config` (+ a short `docs/KERNEL-CONFIG.md` that groups and
  explains the options); `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE` in the defconfig; keep `linux-qemu.fragment` for the
  QEMU boot test.
- **Steps:**
  1. From K0's report, list what probes on the device and what rcS `modprobe`s (audio, Wi-Fi, fan, battery, GPU).
  2. Start from `make ARCH=arm64 allnoconfig`, add: arm64 + SM4450 platform, the drivers from step 1, squashfs/LZ4,
     exFAT, ext4 for nothing (drop it if unused), evdev/uinput, DRM msm, ASoC q6/AudioReach, ath10k_snoc, USB gadget
     (NCM, FunctionFS, later UVC), ksmbd, cgroups only if needed (no systemd), `IKCONFIG_PROC`.
  3. Decide built-in vs module deliberately: built in what the first frame needs (display, input, storage); modules for
     what rcS loads in order (audio, Wi-Fi) — rcS's order is a hardware dependency (see its comments), keep it.
  4. Iterate: boot → K0 report → hardware checklist (`docs/RPC-TEST.md`) → `make sound-test` → QEMU boot test.
- **Verify:** everything in `RPC-TEST.md` passes; `sound-test` 14/14; sleep/wake 20 cycles; Wi-Fi, USB NCM, adb, SMB;
  the QEMU boot test. Record `Image` size and boot marks against the baseline (expect the image well under half of
  today's ~30 MB, and a faster kernel build; boot time changes little — see §0).
- **Stop if:** a missing option silently breaks one feature twice in a row (2-strike rule): diff the probed-driver
  list against the baseline instead of guessing.

### K2 — Own the device tree · S–M
- **Goal:** the `.dts`/`.dtsi` files read as Banana's description of this handheld, every node justified.
- **Steps:** delete nodes for hardware the RPC does not have or Banana does not use (HDMI/DP, cameras, unused
  regulators); split the 2,857-line `common.dtsi` by subsystem (display, audio, input, power, connectivity) with a
  header comment per block; name Banana as the maintainer in the file header; keep BSD-3-Clause.
- **Verify:** `dtc` builds with no new warnings; the K0 report's probed-driver list is unchanged; RPC-TEST passes.

### K3 — A Banana kernel repository and a rebase workflow · S + ongoing
- **Goal:** turn `linux-patches/` into a real git branch: `banana-linux` (`v7.2` + Banana commits), from which the
  patch directory is generated (`git format-patch v7.2..`) so Buildroot keeps working unchanged.
- **Steps:** import the 45 patches as commits with their original authors (keep authorship — it is their work and
  the GPL expects it), so `git log --author` shows at a glance what is ours (Tom, Banana, Jorre) and what is left to
  replace; tag `banana-7.2-1`; document the rebase recipe (`git rebase --onto v7.3 v7.2`), conflict notes per
  patch; a CI job that builds the kernel + modules and runs the QEMU boot test on every push.
- **Verify:** the generated patch dir is byte-identical to today's; CI green.

### K4 — Banana-written drivers for what makes it a Banana Pocket · M each
Start with the ones that are the device's identity, smallest first:
1. **The gamepad (`rsinput` → `banana-pad`)**, 700 + 21 lines today (patches 0016, 0059, by Philippe Simons and Jacob
   Cook). A clean-room rewrite from the MCU's serial protocol (documented by reading the existing driver's behaviour,
   not copying it): button map, digital triggers, a sysfs knob for the d-pad/analog mode (today a RetroArch command),
   and rumble if the MCU has it. **M, medium risk** (input is everything on a handheld; keep the old driver selectable
   until the new one has passed a week of play).
2. **A board platform driver (`banana-pocket`)**: one place for device-specific controls that today live in scattered
   sysfs paths or userspace — fan curve, vibrator, charge limit (`charge_control_end_threshold`, if the battery
   firmware supports it), a stable "board info" sysfs. **M, low risk.**
3. **The panel (CH13726A)**: today a vendor-timings patch to a community driver (0010). Owning it means a
   `panel-banana-rpc` driver with the init sequence and timings documented. **M, medium risk** (a wrong init sequence is
   a black screen — K0's fallback matters here).
4. **AW88166 changes** (0031–0034, 0062): consolidate into one Banana-maintained patch with the clock-lock fix, then
   upstream (K6). **S–M.**

### K5 — Patch-by-patch audit · incremental
First, settle authorship: the 9 patches with no `From:` line get one — Tom/Jorre if they wrote them (then they are
ours and stay), or the original author if they came from ROCKNIX or a mailing list (then they join the audit below).
For each remaining outside patch, decide one of: **upstreamed already** (drop on rebase), **Banana rewrite** (K4),
**send upstream** (K6), or **keep as community code** (credited, in the K3 branch). Record the decision in the patch's
commit message. Start with the small ones (1–20 lines: 0002, 0008, 0011–0014, 0017–0018, 0033, 0041, 0046–0049, 0052,
0054, 0056) — they are quick to understand and often already fixed upstream.

### K6 — Upstream it · L (months)
The paradox of "less of other people's code": the way to carry *no* patches is to get SM4450 + the Retroid Pocket
Classic into mainline, where the community maintains it with you. Order: the device tree and small fixes first (easy
reviews), then the interconnect and clock pieces (coordinate with whoever already posted them — 0006 is Qualcomm's own
series), the panel and pad drivers last. Expect several review rounds per series on the `linux-arm-msm` list.

### K7 — Specialisation · S–M, optional
Async probing for slow drivers, the energy model and cpu capacities (0057) checked against measured A55/A78 power,
`CONFIG_HZ`/preemption chosen for latency, `zram` if memory ever gets tight, I/O scheduler for SD (`mq-deadline` vs
`none`). Each one measured with K0's report, one at a time.

### K8 — Identity · S
`CONFIG_LOCALVERSION="-banana"` and a Banana build string (so `uname -r` reads e.g. `7.2.0-banana1`), panic → reboot
after N s with the last `dmesg` saved to `/data` on the next boot (pstore/ramoops), and a Banana line in the kernel's
early console for the serial log.

## 4. What I would not do (and why)

- **A kernel from scratch or a Rust OS.** Years of work before a single game runs: the GPU (freedreno/Turnip), the
  AudioReach DSP, Wi-Fi and the display pipeline only exist as Linux drivers, and the firmware blobs speak to those.
- **Forking mainline subsystems** (msm DRM, ASoC q6, interconnect). Every kernel release would become a merge war; the
  benefit is nil because those drivers are not device-specific.
- **Replacing firmware.** Signed and undocumented; not possible.
- **A Banana bootloader (now).** OpenABL is what makes the SD card bootable on this device; replacing it risks the
  boot chain itself. Revisit only after K0–K4, and only with a recovery path that does not depend on it.

## 5. Risks and how each phase limits them

- **A kernel that does not boot:** K0's fallback entry or the inactive slot; the OS is on a removable card.
- **A silent feature loss** (an option dropped in K1): the probed-driver diff against the baseline, RPC-TEST, sound-test
  and the QEMU boot test before any release.
- **Input regressions** (K4 pad driver): keep the old driver selectable for a release; a week of real play first.
- **Maintenance load:** K3's branch + CI makes each mainline update a rebase, not an archaeology dig; K6 shrinks the
  load over time.
