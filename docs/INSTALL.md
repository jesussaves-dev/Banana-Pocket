# Installing BananaOS on the Retroid Pocket Classic

Two things stand between a stock Retroid Pocket Classic and BananaOS: a Linux bootloader (**OpenABL**, flashed once —
the stock bootloader boots **Android only**), and the Banana image on a microSD card. Both have a tool now:

| Step | Tool | Runs on |
|---|---|---|
| 1. Bootloader | [**Web installer**](https://jesussaves-dev.github.io/Banana-Pocket/install.html) | Chrome or Edge (WebUSB), any desktop OS |
| 2. Card | [**BananaLinux Flasher**](https://github.com/jesussaves-dev/banana-linux-flasher/releases/latest) | Windows `.exe`, Linux AppImage |

No `fastboot`, `dd` or Android root needed. Android and its data stay where they are.
The manual routes are further down, under [Doing it by hand](#doing-it-by-hand).

> [!CAUTION]
> <span style="color:red">**Retroid Pocket Classic only** (Qualcomm Snapdragon G1 Gen 2 / **SM4450**, board "Rev Q").</span>
> A bootloader for the wrong chipset is the classic way to brick a handheld, and flashing one is the only step here
> that can. The web installer checks the device and the file's sha256 and refuses anything else — if you go by hand,
> do the same checks yourself.
>
> **I am not responsible if you accidentally brick your device.**

## What you need

- A **Retroid Pocket Classic** (SM4450, board "Rev Q").
- A **USB‑C data cable** (not a charge‑only one) and a computer with **Chrome or Edge** — Firefox and Safari have no
  WebUSB.
- A **microSD card** and a card reader. The image is ~0.7 GB and the rest of the card becomes the games partition, so
  32 GB or more for a library. (exFAT is made on first boot; you don't format it.)

## Step 1 — Flash the bootloader (web installer)

Open **<https://jesussaves-dev.github.io/Banana-Pocket/install.html>** and follow its three cards:

1. **Bootloader file** — download `abl_signed-SM4450.elf` from the
   [OpenABL v1.2 release](https://github.com/OpenHandheldCollective/OpenABL/releases) (the page links it) and pick it.
   The page checks its sha256 before it will flash it.
2. **Connect** — power the handheld off, hold **Volume Down** and press **Power** (fastboot), plug it in, press
   **Connect** and pick it. The page checks it really is a Retroid Pocket Classic.
   A stock handheld's own bootloader can't flash, so the page then asks you to **Restart into fastbootd**: press it,
   wait for the fastbootd screen (~20 s), and press **Connect** again.
3. **Flash OpenABL** — writes both bootloader slots (`abl_a`, `abl_b`) and restarts the handheld into the OpenABL menu.
   Don't unplug it until the page says done.

This only ever needs doing once; later Banana updates don't touch the bootloader.

**The handheld doesn't show up?**

- **Windows:** install the [Google USB driver](https://developer.android.com/studio/run/win-usb), replug, try again.
- **Linux:** let your user open fastboot devices, then replug:

  ```sh
  echo 'SUBSYSTEM=="usb", ENV{ID_USB_INTERFACES}=="*:ff4203:*", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/51-fastboot.rules
  sudo udevadm control --reload-rules
  ```

- **Anywhere:** close anything else holding the device — a running `fastboot`, or an adb server (`adb kill-server`).
- Try another cable or port; charge‑only cables are common.

## Step 2 — Write the card (BananaLinux Flasher)

Download **BananaLinux Flasher** from
<https://github.com/jesussaves-dev/banana-linux-flasher/releases/latest> — `BananaLinuxFlasher.exe` on Windows, the
`.AppImage` on Linux (`chmod +x` it first).

1. Pick **Retroid Pocket Classic**.
2. Insert the card, press **Refresh**, pick the card.
3. Press **Download & Flash**. It fetches the newest Banana release, checks its sha256 and writes it. Approve the UAC
   prompt (Windows) or type your `sudo` password in its dialog (Linux).

**Everything on the card is erased.** The flasher only lists removable drives and re‑checks the one you picked before
it writes.

- Windows may show **SmartScreen** for this unsigned build: *More info → Run anyway*. Some antivirus engines
  false‑flag PyInstaller apps; each release ships `BananaLinuxFlasher.exe.sha256` to check your download against.
- **macOS:** no prebuilt flasher — run it from source, or use the [manual card routes](#2-the-card-by-hand).

## Step 3 — First boot

Put the card in the handheld, hold **Volume Down** and press **Power** for the OpenABL menu:

- Set **Boot mode → Linux** and **Boot source → Auto**, then *Start*. OpenABL remembers these, so after this an
  ordinary power‑on boots Banana (**Volume Up** at power‑on boots Android).
- The boot animation plays; the very first boot takes a few seconds longer while it makes the games partition from
  the rest of the card.
- The **first‑time setup** opens: **Wi‑Fi**, **time zone**, **RetroAchievements**, **Box art**, **Finish**. **B** skips
  it (Settings → System → First‑time setup brings it back).
- The menu appears on the shelf of systems. There is no Nintendo DS page: this handheld has one screen and ships the
  RetroArch systems.

If the picture is turned or upside down, add `banana.rotate=0`, `90`, `180` or `270` to the `APPEND` lines in
`extlinux/extlinux.conf` on the card's **BANANA-BOOT** partition and reboot — no rebuild needed.

## Step 4 — Games

Power off, put the card in a computer: a **BANANA** drive with `roms/`, `bios/`, `banana/`.

- Games go in **`roms/<system>`** — `roms/gba`, `roms/snes`, `roms/psx`, and so on (the folder names are in
  `banana/README.txt`, and each shelf says where its games go when it is empty).
- **BIOS** files go in **`banana/bios`** for the systems that need one: Dreamcast (`dc_boot.bin`, `dc_flash.bin`),
  Sega CD (`bios_CD_U.bin` / `_E` / `_J`), PC Engine CD (`syscard3.pce`), Atari Lynx (`lynxboot.img`).
- **Box art** is fetched by the menu itself over Wi-Fi (Settings → System → Box art).

## Updating later

A release's **`banana-retroid-pocket-classic-<version>.update.tar`** goes in **`banana/updates/`** on the card (from a
computer, over File sharing, or over SSH), then **Settings → System → Update** lists it and **A** twice writes it to
the slot not in use and checks it hash by hash. **Previous version** (Settings → System) restarts into the build
before. Nothing on the data partition — games, saves, settings — is touched.

Re‑running the flasher also works, but it **erases the whole card**, games and saves included.

## Booting Android again, or uninstalling

In the OpenABL menu set **Boot mode → Android**. Installing OpenABL does **not** remove Android or its data — the
bootloader is already unlocked and nothing is wiped to flash it. To go fully back to the factory bootloader you need a
backup of the stock ABL partitions (see [below](#back-up-the-stock-bootloader)).

## When something is wrong

`banana/logs/` on the card (put it in a computer) has `boot.txt` (the milestones in seconds), `dmesg.txt`, the menu's
`banana-ui.log`, `sleep.log`, and the last game's `retroarch.log`. **[`FIRST-BOOT.md`](FIRST-BOOT.md)** has the common
cases: a black panel with no `BANANA` drive (the kernel or init — a serial console on `ttyMSM0`, `banana.console=1`),
a black panel with the drive there (the menu could not open the panel), a black panel a second after a game starts
(the probes in [`RPC-TEST.md`](RPC-TEST.md)), and "can't read the card's games" (the data partition did not mount).

---

## Doing it by hand

For when the tools don't fit — no Chromium browser, macOS, a custom recovery already installed, or you just prefer a
terminal. The result is the same.

### 1. The bootloader by hand

Banana needs **OpenABL** — <https://github.com/OpenHandheldCollective/OpenABL> — a patched ABL with a graphical menu
that offers a Linux boot and reads `extlinux/extlinux.conf` from the card. It is the only bootloader Banana supports.
Its releases carry `abl_signed-SM4450.elf` (+ `.sha256`) and `openabl-recovery-flashable-sm4450.zip`. It must be the
**SM4450** build, and check its `.sha256` before flashing.

#### OpenABL with `fastboot`

The bootloader is **already unlocked** (`fastboot getvar unlocked` says `yes`), so nothing is wiped. But the **stock
bootloader's fastboot has no `flash` command** — it answers `unknown command` — so the flash goes through
**fastbootd**, Android recovery's fastboot, which does. (This is exactly what the web installer does.)

1. Download `abl_signed-SM4450.elf` and its `.sha256` and check it (`sha256sum -c`).
2. Power off; hold **Volume Down** and press **Power** → `fastboot devices` lists the handheld.
3. Check it is the right device: `fastboot getvar product` says `parrot`, `getvar variant` says `SGP EMMC`.
4. Restart into fastbootd and flash **both** A/B slots:

   ```sh
   fastboot reboot fastboot        # about 20 s; `fastboot getvar is-userspace` then says yes
   fastboot flash abl_a abl_signed-SM4450.elf
   fastboot flash abl_b abl_signed-SM4450.elf
   fastboot reboot bootloader
   ```

5. The OpenABL menu opens (`getvar product` now says `OpenABL`). OpenABL's own fastboot flashes directly, so upgrading
   it later needs no fastbootd.

#### OpenABL from a custom recovery

Flash `openabl-recovery-flashable-sm4450.zip` in a custom recovery such as TWRP: it writes ABL to both slots and adds
an `addon.d` script that re‑flashes the other slot after an Android OTA.

#### Back up the stock bootloader

Before flashing any ABL by hand, from a root shell on Android:

```sh
dd if=/dev/block/by-name/abl_a of=abl_a.img
dd if=/dev/block/by-name/abl_b of=abl_b.img
```

Keep those two files off the device; they are your way back to the factory bootloader. A bad ABL is also recoverable
through Qualcomm **EDL (9008)** with the device's own firmware.

### 2. The card by hand

Download **`banana-retroid-pocket-classic.img.gz`** from
[the Releases page](https://github.com/jesussaves-dev/Banana-Pocket/releases). **Everything on the card is erased.**

- **balenaEtcher** (also macOS) takes the `.gz` as it is — pick the file, pick the card, flash.
- The repository's checker confirms the card is removable, big enough and unmounted, then writes it and reads it back
  sector for sector — a card that lies about writes is the usual cause of a half‑boot:

  ```sh
  sudo tools/write-sd.sh banana-retroid-pocket-classic.img.gz /dev/sdX
  ```

- Or plain `dd`:

  ```sh
  zcat banana-retroid-pocket-classic.img.gz | sudo dd of=/dev/mmcblk0 bs=4M status=progress
  ```

---

Source for the bootloader steps: the [OpenABL project](https://github.com/OpenHandheldCollective/OpenABL). The card's
own layout, the A/B slots and the update flow are in [`ARCHITECTURE.md`](ARCHITECTURE.md).
