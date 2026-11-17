# Banana kernel config — driver inventory (K1.1)

Input for K1.2 (`linux-banana.config`; the result is §5). Everything here is read from the 0.8.0 baseline,
`docs/reports/kernel-report-0.8.0-b-2026-11-16.txt` (+ `.config`), and from the scripts that load modules. A row
is in this file because the handheld **uses** it; K1.2 adds each one deliberately and nothing else.

Legend: **y** built in · **m** module · ★ needed for the menu's first frame (display, input, storage) → must stay
built in.

## 1. Who loads modules

There is no udev. A module is loaded only by one of these, in this order:

| When | Who | Modules | Notes |
|---|---|---|---|
| rcS, 0.84 s | `etc/init.d/rcS:78` | `qcom_q6v5_pas qcom_q6v5 qcom_sysmon qcom_common` | ADSP remoteproc; rcS then waits for `running` |
| rcS, 1.08 s | `rcS:88` | `snd-q6dsp-common snd-q6apm q6apm-dai q6apm-lpass-dais q6prm q6prm-clocks` | DSP services first: q6apm's 5 s GPR wait is on the critical path |
| rcS, 1.10 s | `rcS:90` | `snd-soc-lpass-macro-common snd-soc-lpass-{rx,tx,va}-macro soundwire-qcom` | SoundWire takes its iface clock from the RX macro |
| rcS, 1.13 s | `rcS:92` | `snd-soc-wcd-common snd-soc-wcd-mbhc snd-soc-wcd-classh snd-soc-wcd937x snd-soc-aw88261` | see §4.2: aw88261 binds nothing |
| rcS, 1.17 s | `rcS:94-95` | `snd-soc-qcom-common`, `snd-soc-sc8280xp` | machine driver; card appears at 5.44 s |
| rcS, 5.47 s | `rcS:105` | `pwm-fan qcom_battmgr` | see §4.1: pwm-fan binds nothing |
| background | `rcS:126` | `ath10k_snoc` | Wi-Fi, WCN3950 (`qcom,wcn3990-wifi`) |
| bananad start | `system/bananad/bananad.c:1897` | `pm8xxx_vibrator` | FF input for the rumble pulse |
| USB hotplug | `bananad.c:2025` (`load_driver`) | whatever a `usb:` MODALIAS names (xpad, USB serial…) | external pads/adapters; K1.2 keeps a USB-HID/pad module set |
| Settings > File sharing | `usr/libexec/banana/share:10` | `ksmbd` | unloaded again when sharing goes off |

**rcS's order is a hardware dependency** (its comments at `rcS:70-77`): K1.2 keeps the whole audio stack and Wi-Fi as
modules so this order still holds.

Modules pulled in only as dependencies (visible in `lsmod`, no `modprobe` of their own): `qcom_pil_info`,
`snd_soc_qcom_sdw`, `snd_soc_wcd937x_sdw`, `regmap_sdw`, `soundwire_bus`, `slimbus` (symbol dependency of
`soundwire_qcom`; no SLIMbus device on this board), `smb_compress`, `nls_ucs2_utils`.

## 2. Modules (=m, 17 options used)

| Subsystem | Config | Driver(s) bound |
|---|---|---|
| ADSP remoteproc | `QCOM_Q6V5_PAS`, `QCOM_Q6V5_COMMON`, `QCOM_SYSMON`, `QCOM_PIL_INFO`, `QCOM_RPROC_COMMON` | `qcom_q6v5_pas` ×2 |
| AudioReach DSP | `SND_SOC_QDSP6`, `SND_SOC_QDSP6_APM`, `SND_SOC_QDSP6_APM_DAI`, `SND_SOC_QDSP6_APM_LPASS_DAI`, `SND_SOC_QDSP6_PRM`, `SND_SOC_QDSP6_PRM_LPASS_CLOCKS` | `aprbus/qcom-apm`, `aprbus/qcom-prm`, `q6apm-dai`, `q6apm-lpass-dais`, `q6prm-lpass-clock` |
| LPASS macros | `SND_SOC_LPASS_MACRO_COMMON`, `SND_SOC_LPASS_{RX,TX,VA}_MACRO` | `rx_macro`, `tx_macro`, `va_macro` |
| SoundWire | `SOUNDWIRE`, `SOUNDWIRE_QCOM`, `REGMAP_SOUNDWIRE`, `SLIMBUS` | `qcom-soundwire` ×2 |
| Headset codec | `SND_SOC_WCD937X`, `SND_SOC_WCD937X_SDW`, `SND_SOC_WCD_MBHC`, `SND_SOC_WCD_CLASSH` | `wcd937x_codec`, `soundwire/wcd9370-codec` ×2 |
| Machine | `SND_SOC_SC8280XP`, `SND_SOC_QCOM_COMMON`, `SND_SOC_QCOM_SDW` | `snd-sc8280xp` |
| Wi-Fi | `ATH10K_SNOC` (`ATH10K=y`) | `ath10k_snoc` |
| Battery | `BATTERY_QCOM_BATTMGR` | `auxiliary/qcom_battmgr.pmic_glink_power_supply` |
| Rumble | `INPUT_PM8XXX_VIBRATOR` | `pm8xxx-vib` |
| File sharing | `SMB_SERVER` | — (no device) |
| Fan | `SENSORS_PWM_FAN` | **none** — §4.1 |
| Speaker amp (unused) | `SND_SOC_AW88261` | **none** — §4.2 |

## 3. Built in (=y) — every driver that probed

### Display ★
| Config | Bound |
|---|---|
| `DRM_MSM` (DPU + DSI + GPU in one) | `msm-mdss`, `msm_dpu`, `msm_dsi`, `msm_dsi_phy`, `adreno` |
| `DRM_PANEL_CHIPWEALTH_CH13726A` | `mipi-dsi/panel-ch13726a-amoled` |
| `SM_DISPCC_4450`, `SM_GPUCC_4450` | `dispcc-sm4450`, `gpucc-sm4450` |
| `ARM_SMMU` | `arm-smmu` ×2 |
| `DRM_AUX_BRIDGE`, `DRM_AUX_HPD_BRIDGE` | `aux_bridge`, `aux_hpd_bridge` (USB-C DP altmode chain; no DP out is used — K1.2 candidate, check `pmic_glink_altmode` needs them) |

### Input ★
| Config | Bound |
|---|---|
| `JOYSTICK_RSINPUT` (ROCKNIX patch) | `serial/rsinput` — the pad MCU on a GENI UART |
| `JOYSTICK_BANANA_PAD` | `serial/banana-pad`: clean-room pad MCU driver, compatible `banana,pad`; both board DTS use it since K4.1 (#21); rsinput is still built in |
| `BANANA_POCKET` | `platform/banana-pocket`: board identity sysfs (`model`, `board`, `board_id`, `kernel_version`) under `/sys/devices/platform/banana-pocket/`; DT node `banana,pocket` in both board DTS (K4.2) |
| `TOUCHSCREEN_EDT_FT5X06` | `i2c/edt_ft5x06` |
| `KEYBOARD_GPIO` | `gpio-keys` (volume) |
| `INPUT_PM8941_PWRKEY` | `pm8941-pwrkey` ×2 (power, resin) |
| `INPUT_EVDEV`, `INPUT_UINPUT`, `INPUT_JOYDEV` | — (interfaces; joydev: check K1.2 whether any emulator opens `/dev/input/js*`) |

### Storage ★
| Config | Bound |
|---|---|
| `MMC_SDHCI_MSM` | `sdhci_msm`, `mmc/mmcblk` (microSD: whole OS) |
| `SQUASHFS`, `SQUASHFS_LZ4` | root (`rootfstype=squashfs`) |
| `VFAT_FS` + `NLS_CODEPAGE_437`, `NLS_ISO8859_1` | `/boot` (p1, `rcS:67`); the NLS pair are vfat's default codepage and iocharset, which a mount without `codepage=`/`iocharset=` needs (the K1.2 config missed them and `/boot` stopped mounting) |
| `EXFAT_FS` | `/data` (p4, `rcS:53`); `rcS:59` falls back to "whatever the kernel can mount" (FAT32) |
| `DEVTMPFS(_MOUNT)`, `TMPFS`, `PROC_FS`, `SYSFS`, `DEVPTS` | `rcS:5-17` |
| `EXT4_FS`, `F2FS_FS` | **nothing mounts them** — drop in K1.2 |

### SoC core (clocks, power, buses)
| Config | Bound |
|---|---|
| `SM_GCC_4450`, `COMMON_CLK_QCOM`, `QCOM_CLK_RPMH` | `gcc-sm4450`, `clk-rpmh` |
| `PINCTRL_SM4450`, `PINCTRL_SM8450_LPASS_LPI`, `PINCTRL_QCOM_SPMI_PMIC` | `sm4450-tlmm`, `qcom-sm8450-lpass-lpi-pinctrl`, `qcom-spmi-gpio` ×3 |
| `INTERCONNECT_QCOM_SM4450`, `INTERCONNECT_QCOM_BCM_VOTER` | `qnoc-sm4450` ×9, `bcm_voter` |
| `QCOM_RPMH`, `QCOM_COMMAND_DB`, `QCOM_RPMHPD`, `REGULATOR_QCOM_RPMH`, `REGULATOR_FIXED_VOLTAGE` | `rpmh`, `cmd-db`, `qcom-rpmhpd`, `qcom-rpmh-regulator` ×2, `reg-fixed-voltage` ×8 |
| `QCOM_SCM`, `QCOM_SMEM`, `HWSPINLOCK_QCOM`, `QCOM_SMP2P`, `QCOM_IPCC`, `QCOM_AOSS_QMP`, `QCOM_PDC` | `qcom_scm`, `qcom-smem`, `qcom_hwspinlock`, `qcom_smp2p` ×2, `qcom-ipcc`, `qcom_aoss_qmp`, `qcom_pdc` |
| `QCOM_SOCINFO`, `QCOM_STATS`, `NVMEM_QCOM_QFPROM`, `QCOM_LLCC` | `qcom-socinfo`, `qcom_stats`, `qcom,qfprom` (LLCC: nothing probed — check) |
| `QCOM_GENI_SE`, `I2C_QCOM_GENI`, `SERIAL_QCOM_GENI(_CONSOLE)`, `QCOM_GPI_DMA` | `geni_se_qup` ×2, `geni_i2c` ×2, `qcom_geni_serial` ×3, `gpi` |
| `ARM_QCOM_CPUFREQ_HW`, `ARM_PSCI_CPUIDLE_DOMAIN` | `qcom-cpufreq-hw`, `psci-cpuidle-domain`, `genpd_provider` ×25 |
| `ARM_ARCH_TIMER`, `OF` (simple-pm-bus), arm64 PMU | `arch-timer-mmio`, `simple-pm-bus`, `armv8-pmu` ×2 |
| `QCOM_WDT` | — (nothing bound in the list; check whether the DT has the node) |

### PMIC, battery, USB-C
| Config | Bound |
|---|---|
| `SPMI_MSM_PMIC_ARB`, `MFD_SPMI_PMIC`, `MFD_QCOM_PM8008` | `spmi_pmic_arb`, `spmi/pmic-spmi` ×4 (PM8008: nothing bound — check) |
| `QCOM_SPMI_ADC5`, `QCOM_TSENS`, `QCOM_SPMI_TEMP_ALARM`, `NVMEM_SPMI_SDAM` | `qcom-spmi-adc5` ×2, `qcom-tsens` ×2, `spmi-temp-alarm`, `qcom,spmi-sdam` ×10 |
| `RTC_DRV_PM8XXX`, `POWER_RESET_QCOM_PON` | `rtc-pm8xxx`, `qcom-pon` |
| `QCOM_PMIC_GLINK` (incl. altmode), `UCSI_PMIC_GLINK`, `TYPEC`, `TYPEC_UCSI` | `qcom_pmic_glink`, `qcom_pmic_glink_rpmsg`, `pmic_glink_altmode`, `pmic_glink_ucsi` |
| `QCOM_PD_MAPPER`, `QRTR`, `QRTR_SMD`, `RPMSG_CTRL`, `QCOM_APR` | `qcom-pdm-mapper` ×2, `qcom_smd_qrtr` ×2, `rpmsg_ctrl` ×2, `rpmsg/qcom,apr` |

### USB (gadget: NCM, adb via FunctionFS; host: external pads)
| Config | Bound |
|---|---|
| `USB_DWC3`, `USB_DWC3_QCOM`, `USB_DWC3_DUAL_ROLE` | `dwc3-qcom` |
| `PHY_QCOM_QMP_COMBO`, `PHY_QCOM_USB_SNPS_FEMTO_V2` | `qcom-qmp-combo-phy`, `qcom-snps-hs-femto-v2-phy` |
| `USB_GADGET`, `USB_CONFIGFS`, `USB_CONFIGFS_NCM`, `USB_CONFIGFS_F_FS` | `gadget/configfs-gadget.banana` |
| `USB_CONFIGFS_ECM`, `USB_CONFIGFS_RNDIS` | — (the gadget uses NCM; drop unless a script picks ECM/RNDIS) |
| `USB_CONFIGFS_F_UVC` | not set — plan's "later UVC" |

### Audio, built-in part
| Config | Bound |
|---|---|
| `SND_SOC_AW88166` (ROCKNIX patch) | `i2c/aw88166` ×2 — the speaker amps |

### Wi-Fi, built-in part
| Config | Bound |
|---|---|
| `POWER_SEQUENCING_QCOM_WCN` | `pwrseq-qcom_wcn` |

### Debug / identity
| Config | Bound |
|---|---|
| `PSTORE_RAM` | `ramoops` (keep: K8 pstore) |
| `IKCONFIG_PROC` | `/proc/config.gz` (K0 report reads it) |
| `SERIAL_8250`, `SERIAL_8250_CONSOLE` | `serial8250` — the generic platform stub; no 8250 UART on this SoC → drop |

## 4. Findings for K1.2

1. **The fan never binds in 0.8.0.** rcS loads `pwm_fan`, but no `pwm-fan` driver appears in the probed list. The DT
   fan takes `pwms = <&pm6450_pwm …>` (`sm4450-retroid-pocket-classic-revq.dts:32`), whose node is
   `compatible = "qcom,pm6450-pwm"` (`…-common.dtsi:2242`); `LEDS_QCOM_LPG` is not set, and none of the 45 patches in
   `linux-patches/` mention `pm6450-pwm`. So the PWM provider has no driver and pwm-fan defers forever. Needs owner
   confirmation on the device (`ls /sys/class/pwm`, `cat /sys/kernel/debug/devices_deferred` — no debugfs on 0.8.0,
   so: `dmesg | grep -i fan`), then a separate task (kernel config + maybe a compatible patch — needs the owner's OK
   under the patch rule).
2. **`snd_soc_aw88261` binds nothing.** The amps are `awinic,aw88166` (`…-common.dtsi:1745,1755`), bound by the
   built-in `SND_SOC_AW88166`. Drop `SND_SOC_AW88261` and its `modprobe` in rcS:92 (rcS change = a separate task, it is
   not a K1 file).
3. **Dead weight already identified:** `EXT4_FS`, `F2FS_FS`, `SERIAL_8250`, `USB_CONFIGFS_ECM/RNDIS`; to check:
   `QCOM_LLCC`, `MFD_QCOM_PM8008`, `QCOM_WDT`, `DRM_AUX_(HPD_)BRIDGE`, `INPUT_JOYDEV` (enabled, nothing bound; the
   board DT in `dts/qcom/` has no LLCC, PM8008 or watchdog node — check mainline's `sm4450.dtsi` before dropping).
4. **Not in the probed list but required:** s2idle suspend (`SUSPEND`, `PM_SLEEP`), cpufreq governors, `CRYPTO_*`
   and `NLS_UTF8` that `SMB_SERVER` selects, the firmware loader (`FW_LOADER`, for ADSP/wpss/tplg), `RFKILL`/`CFG80211`/
   `MAC80211` for ath10k, `PACKET`/`UNIX`/`INET` for Wi-Fi + NCM + ssh, `SND_TIMER`/`SND_PCM` for PipeWire, USB host HID
   + the pad modules bananad loads by MODALIAS (list them from the 258 module files before trimming).
5. **Counts to beat:** 1963 =y / 258 =m, Image 30.7 MB, 258 module files / 12.8 MB (from the baseline).

## 5. The config (K1.2)

`buildroot/board/retroid-pocket-classic/linux-banana.config` is the kernel's whole configuration, and the Buildroot
defconfig's `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE` points at it. `linux-rocknix.config` stays in the tree as the 0.8.0
reference only.

### How Buildroot reads it

Buildroot doesn't run `allnoconfig`. `pkg-kconfig.mk:67` runs `merge_config.sh -m` with this file and
`linux-banana.fragment` and `linux-qemu.fragment`, in that order, then `make olddefconfig`. So any symbol the file
doesn't name takes its **Kconfig default**. The file is a defconfig: it lists the changes from the defaults.
`=y`/`=m` turns on something this board needs. `# ... is not set` turns off a default the handheld doesn't use.

The K1.2a draft was built and checked against `allnoconfig`. Its savedefconfig check counted only `=` lines and
skipped about 100 `is not set` lines, so under Buildroot it would have resolved to 1196 =y and not the 1083 it
reported. Most of that difference is defaults that are worth keeping (below).

### Decisions

| Choice | Result | Why |
|---|---|---|
| Kconfig defaults that allnoconfig turned off: Cortex-A55/A78 errata workarounds, `ARM64_PTR_AUTH`, `ARM64_BTI`, `ARM64_SVE`, `SECCOMP`, `STACKPROTECTOR_STRONG`, `RELR`, `SCHED_MC`/`_CLUSTER` | **kept** (not listed: they are defaults) | The errata are correctness fixes for the SoC's own cores. The rest is cheap hardening and the stock arm64 ABI. |
| Defaults the handheld has no use for | **off**, the last group in the file | `INPUT_MOUSE` (PS/2), `LEGACY_PTYS`, `LEGACY_TIOCSTI`, `DEVMEM`, `SWAP`, `IOSCHED_BFQ`, `MQ_IOSCHED_KYBER`, `HW_RANDOM`, `COMPAT_BRK`. `git grep` over `system/`, `buildroot/board/`, `ui/src/` finds no user of `/dev/mem`, swap, loop or hwrng. |
| `COMPAT` (32-bit userspace), `RANDOMIZE_BASE`, `CGROUPS`, `PSI` | off (the default) | The rootfs is aarch64-only. Nothing in the image uses cgroups or PSI. KASLR can come back later as a separate task if wanted. |
| `EXPERT=y` | on | `VT` has a prompt only under EXPERT, so without it the fragment's `# CONFIG_VT is not set` has no effect. It also opens the prompt for `SLUB_DEBUG`, which the fragment now turns off. `NAMESPACES` is `default !EXPERT`, so the file turns it on explicitly. |
| HID pad drivers (`HID_SONY`, `_PLAYSTATION`, `_NINTENDO`, `_LOGITECH`, `_MICROSOFT`) | **=y** (the draft had them as =m) | Their aliases are `hid:…`. bananad's `load_driver` (`system/bananad/bananad.c:2021`) only modprobes `usb:` aliases, so as modules nothing would ever load them. |
| `JOYSTICK_XPAD`, `USB_STORAGE`, `USB_SERIAL`, `USB_ACM` | =m | Their aliases are `usb:…`, so bananad loads them by MODALIAS. |
| `QCOM_LLCC`, `MFD_QCOM_PM8008`, `QCOM_WDT` (+`WATCHDOG`), `INPUT_JOYDEV` | **dropped** | Neither mainline `sm4450.dtsi` nor the board dts has an llcc, pm8008 or watchdog node. Nothing opens `/dev/input/js*` (banana-ui reads evdev, RetroArch uses udev). |
| `DRM_AUX_BRIDGE`, `DRM_AUX_HPD_BRIDGE` | =y via select | `PHY_QCOM_QMP_COMBO` selects them: the USB-C DP altmode chain. |
| `SENSORS_PWM_FAN` | =m, kept | rcS:105 modprobes it. It binds nothing until the PWM provider has a driver (§4 finding 1). |
| `linux-qemu.fragment` | now carries `ARM_AMBA`, `PCI`, `PCI_HOST_GENERIC`, `PCI_MSI`, `VIRTIO_MENU`, `VIRTIO_PCI`, `VIRTIO_CONSOLE`, `VIRTIO_NET` | It used to rely on linux-rocknix.config for these. Without them its virtio-gpu, PL011 and SDHCI-PCI lines resolve to off, and `tools/qemu-boot.sh` gets no console or root disk. |

### Check

1. Merge the file and both fragments into an empty `O=` dir with `merge_config.sh -m`, as Buildroot does.
2. Run `olddefconfig`.
3. Confirm every `=` line and every `is not set` line across the three files held.
4. Run `savedefconfig`. Every line it prints must already be in one of the three files.

Kernel tree: linux-7.2 plus the board's 49 patches, with Buildroot's `aarch64-linux-` toolchain.

| | 0.8.0 baseline | K1.2a draft (allnoconfig, no fragment) | K1.2 (Buildroot flow) |
|---|---|---|---|
| Lines checked: held / not held | | | 273 / 0 |
| savedefconfig lines not in the files | | (skipped `is not set`) | 0 |
| =y / =m | 1963 / 258 | 1083 / 65 | 1249 / 61 |
| Image | 30.7 MB | 19.0 MB | 17.9 MB |
| `.ko` files / bytes | 258 / 12.8 MB | 65 / 3.4 MB | 60 / 3.3 MB |
| dtb | 62,981 B | 62,981 B | 62,981 B |

The Image + modules + revq dtb build passes with no warnings. Nothing has been built in Buildroot, booted in QEMU or
flashed yet. RPC-TEST, sound, sleep and `tools/qemu-boot.sh` are the next checks.

### K1.2c: timers, preemption, thermal, GPU devfreq (found on the device)

A default dropping out is only caught by a test that exercises it, and nothing in QEMU did. On the handheld the menu's
sound came out slow and crunchy, the CPUs had no cooling device, and the GPU had no devfreq. Each one is now named in
`linux-banana.config`, matching the 0.8.0 kernel:

| Lines | Default without them | What broke |
|---|---|---|
| `PREEMPT`, `HIGH_RES_TIMERS`, `NO_HZ_IDLE` | `PREEMPT_LAZY`, 4 ms timer resolution, `HZ_PERIODIC` | The menu's 15 ms PCM buffer, fed through PipeWire's ALSA plugin, underran: slow, crunchy menu sound. Games were fine (bigger buffers). Runtime `preempt=full` alone did not fix it, so the timers were the cause. |
| `CPU_THERMAL`, `CPU_FREQ_THERMAL` | off | The board dts's cooling-maps for cpu0-7 bound nothing. The only cooling device was `ath10k_thermal`, so a hot SoC was never throttled. |
| `PM_DEVFREQ`, `DEVFREQ_GOV_{SIMPLE_ONDEMAND,PERFORMANCE,POWERSAVE}`, `DEVFREQ_THERMAL` | off | "Couldn't initialize GPU devfreq": no `/sys/class/devfreq`, so `banana-perf`'s GPU governor writes went nowhere |

With NO_HZ back, the cpuidle governor default also goes back from `ladder` to `menu`. Result on the device: timer
resolution 1 ns, preempt `full`, cooling devices `cpufreq-cpu0`, `cpufreq-cpu6`, `devfreq-3d00000.gpu`, the GPU on
`powersave` in the menu, sound-test 14/14, and menu and game sound reported good. Image 17.97 MB, 1262 =y / 61 =m,
60 `.ko` files (`docs/reports/kernel-report-0.8.0-k12c-2026-11-16.txt`). The one savedefconfig line not in the files,
`# CONFIG_GCC_PLUGINS is not set`, comes from Buildroot's `linux.mk` fixup and was already there in K1.2.
`QCOM_LMH` stays off: its driver matches `qcom,sdm845-lmh` and neither dts has such a node. On this SoC,
qcom-cpufreq-hw handles the dcvsh (LMh) interrupts itself.
