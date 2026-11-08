# Bluetooth Classic gamepads, without D-Bus

## Intent

The handheld has a Bluetooth radio whose kernel side is already in the image, but no userspace at all: no BlueZ, no
D-Bus, no `hciattach`. This adds just enough userspace to pair a Bluetooth Classic gamepad once, have it come back on
its own, and play — and nothing else. The stack's rule (no D-Bus) is what shapes it, not a preference for writing a
Bluetooth stack.

## Scope

- **Bluetooth Classic (BR/EDR) HID only.** Gamepads are the target; a classic keyboard or mouse attached the same way
  also just works, but is not a UI goal.
- **One Settings page:** a `Bluetooth` row (On/Off) and a `Devices` page (scan, pair/connect, forget) — the Network page
  is the model.
- **Persistent:** a paired pad is remembered (link key on the card) and reconnects after a reboot without re-pairing.
- **Off by default**, and the radio is powered down around a sleep.
- **No D-Bus, no BlueZ, no udev:** a small C daemon, `banana-bt`, over the kernel's management (`mgmt`) API and `hidp`.

## Non-goals

BLE HID (8BitDo Ultimate 2 in Bluetooth mode, Xbox Series pads, Stadia), Bluetooth audio/A2DP, OBEX/file transfer,
Bluetooth networking (PAN), pairing with phones, multi-host. The 8BitDo Ultimate 2 speaks BLE, so it will **not** be
seen by this; adding it later means either BlueZ or a HOGP client, and is a separate design.

## What the image already has (checked on the tree and the unit)

- **Device tree:** `bluetooth { compatible = "qcom,wcn3950-bt" }` under the WCN3990 combo node — the serdev attach
  exists.
- **Kernel:** `CONFIG_BT`, `BT_BREDR`, `BT_LE`, `BT_HIDP`, `BT_QCA`, `BT_HCIUART` (serdev + H4 + LL + QCA) — all
  modules present in `/lib/modules`. `CONFIG_BT_RFCOMM`/`BNEP` are off (not needed).
- **Firmware:** `/lib/firmware/qca/cmbtfw13.tlv` + `.ver` (and the `cmnv13*` files) ship.
- **HID drivers, built in:** `hid-nintendo`, `hid-playstation`, `hid-sony`, `hid-microsoft`, `hid-generic`. So a
  connected pad becomes an ordinary `/dev/input/eventN`; RetroArch's udev driver uses it, and `banana-ui` rescans input.
- **Nothing in userspace:** `BR2_PACKAGE_BLUEZ5_UTILS`, `BLUEZ_ALSA` and `DBUS` are all off; `rcS` loads modules
  explicitly because there is no udev.

## Approaches

1. **BlueZ + D-Bus** — the supported stack (pairing, HID input plugin, A2DP through PipeWire's bluez5). Rejected: it
   needs D-Bus (forbidden) and a long-running bus + daemon at boot. It is the path to take if BLE or audio is ever
   wanted.
2. **A bespoke `banana-bt` over the kernel mgmt API + `hidp`** — chosen. The kernel already owns the radio, firmware and
   HID parsing; userspace only has to power it, pair, store link keys and open a HIDP connection. No new daemon
   framework, no bus, ~700 lines of C and a Settings page.
3. **Shell out to BlueZ's `btmgmt`/`hcitool`** — rejected: those binaries come with BlueZ, and driving pairing and
   reconnection from shell is fragile and untestable.

## Design

### Process and IPC

`system/bananad/banana-bt.c`, built and installed by the existing `bananad` package (`Makefile` gets a second target),
started by `rcS` after the modules. Strict POSIX C, one `poll()` loop, no threads.

The card's file-IPC convention carries the UI (as `launch`, `screen.req` and `update-status` do):

- `/run/banana/bt-state` — one line for the page header: `off`, `on`, `scanning`, `pairing <addr>`, `connected <n>`,
  `error <why>`. `connected` counts devices with a **HIDP session** (an input device exists), not just a baseband link;
  `error` is cleared by the next command or successful event.
- `/run/banana/bt-list` — `addr<TAB>name<TAB>flags` per line, flags `p` paired, `c` connected; written after every
  mgmt event that changes it.
- `/run/banana/bt-cmd` — the UI writes one command, the daemon reads it on `IN_CLOSE_WRITE` and unlinks it:
  `on`, `off`, `scan`, `pair <addr>`, `connect <addr>`, `forget <addr>`.
- `/data/banana/bt-paired` — `addr<TAB>name`, for the offline list.
- `/data/banana/bt-keys` — `addr<TAB>link-key-hex<TAB>type`, the mgmt link keys; written on
  `MGMT_EV_NEW_LINK_KEY`, loaded with `MGMT_OP_LOAD_LINK_KEYS` at start.
- `/data/banana/logs/banana-bt.log` — timestamped lines, for RPC-TEST-style diagnoses.

### Management API and pairing

A private `btmgmt.h` declares only what is used (opcodes, event codes and their structs), written from the kernel's
mgmt API documentation rather than copied from a GPL header. The socket is
`socket(AF_BLUETOOTH, SOCK_RAW|SOCK_CLOEXEC, BTPROTO_HCI)` bound to `{HCI_DEV_NONE, HCI_CHANNEL_CONTROL}`.

- Start: `bluetooth = on` is *intent*, not a fact at boot. The daemon reads the index list; if it is empty it waits
  for `MGMT_EV_INDEX_ADDED` (the WCN3990 firmware download registers `hci0` seconds after `rcS` modprobes), so a slow
  adapter is not an error and there is no retry loop. On the index it powers the controller on, loads the link keys and
  marks each paired device trusted (`SET_DEVICE_FLAGS`, trusted).
- **Connectable:** `SET_CONNECTABLE true` whenever the radio is on — without page scan a paired pad can never page the
  host, which is the whole "comes back on its own" behaviour. While the Devices page is open it also sets
  `SET_DISCOVERABLE`/`SET_PAIRABLE` (pairable timeout 0: until stopped) and `START_DISCOVERY` (BR/EDR inquiry);
  leaving the page stops all three. Off and sleep set connectable off before powering down.
- Scan: devices come from `MGMT_EV_DEVICE_FOUND` (address always, name when the controller supplied it).
- Pair: `PAIR_DEVICE` with `NoInputNoOutput` (Just Works) and a 30 s timeout, then on `MGMT_EV_DEVICE_PAIRED` store
  the link key from `MGMT_EV_NEW_LINK_KEY`, trust it, add to `bt-paired`, and open the HIDP connection.
- Reconnect: the **pad** pages the host (that is what BlueZ's own input plugin relies on), so the daemon listens on
  the HID PSMs and accepts; `MGMT_EV_DEVICE_CONNECTED` for a paired address is the signal to have the listener ready.
  Pressing A on a paired device also dials out as a fallback for pads that only accept incoming connections.
  `DEVICE_DISCONNECTED` tears the HIDP session down.
- Forget: `HIDPCONNDEL`, `UNPAIR_DEVICE`, drop it from both stores.
- A second pad is allowed; the daemon keeps one HIDP session per address.

### SDP: the report descriptor has to come from somewhere

The kernel's HIDP driver only builds a real HID device when `hidp_connadd_req` carries a **report descriptor**; with
`rd_size = 0` it falls back to `hidp_setup_input`, which is boot-protocol keyboard/mouse only (`BTN_LEFT`, no axes, no
gamepad). BlueZ gets the descriptor from SDP, so the daemon needs a small SDP client — and it cannot use
`libbluetooth`, because Buildroot's `bluez5_utils` selects **D-Bus** (the one thing this design exists to avoid);
`bluez5_utils-headers` alone gives headers, not the `sdp_*` API.

The client is L2CAP to PSM 1 (SDP's fixed channel, no discovery needed) and two `ServiceSearchAttributeRequest`s:
UUID `0x1124` (HID) for `HIDDescriptorList` (`0x0206`) — parse the data-element sequence and take the report descriptor
(type `0x22`) — and UUID `0x1200` (PnP Information) for vendor/product/version, so the kernel's `hid-*` driver matches
(`hid-nintendo`, `hid-playstation`, …) and RetroArch's udev autoconfig knows the pad. If the PnP record is missing the
pad still works through `hid-generic` with vendor/product 0.

### HIDP

`hidp` is the kernel side of Bluetooth HID. The daemon **listens** on the HID PSMs (17 control, 19 interrupt), accepts
the pad's incoming pair of connections, and calls `ioctl(HIDPCONNADD)` with `struct hidp_connadd_req` (sockets, the
descriptor and PnP ids from SDP); the kernel then registers the input device through the matching `hid-*` driver. The
dial-out path (A on a paired device) opens the same two PSMs and uses the same request. The small
`bluetooth`/`hidp`/`l2cap` ABI structs and ioctl constants are declared locally (same reason as `btmgmt.h`). `HIDPCONNDEL`
on forget, disconnect or controller power-off.

### Settings and UI

- `bluetooth = off` added to `system/rootfs/etc/banana/settings.conf`, to `settings_defaults.cpp`'s `KEYS` (the bezels
  bug: a key missing from the schema is re-appended from the template at every boot, and `check-systems.py` now fails on
  it), with two rows in a new **Bluetooth** group:
  - **Bluetooth** — On/Off (`bluetooth`).
  - **Devices** — a `PAGE` like Network: the header draws `bt-state`; the list is `bt-list`, A pairs/connects, X forgets,
    B returns. The page starts a scan on open and stops it on close.
- Strings go through `tr()` with keys in `ui/assets/lang/{en,es,de}.json`; `tools/check-lang.py` fails if one is
  missing.
- With the radio off the Devices row shows "Off" and does not open.

### Power

Default off, and the radio is powered down for a sleep: `etc/banana/sleep.d/15-bt` signals the daemon (`SIGUSR1` before
the sleep, `SIGUSR2` after) — it powers the controller off and remembers that it was on, then restores it; nothing is
forgotten, so the pad reconnects on wake. `bluetooth = off` keeps `SET_POWERED false` all the time (zero cost, and the
module can stay idle).

### Module loading

`rcS` gets `modprobe -a bluetooth hci_uart btqca hidp` (the image has no udev/modprobe helper, so every module is
listed there); the firmware is already in `/lib/firmware/qca`. `banana-bt` is started after that and reports
`error no adapter` if the controller list is empty, without retry loops.

### Failure handling

One line in `bt-state` and the log, no popups: no adapter, pairing cancelled/timed out, HIDP add failure (device out of
range), a key file that cannot be read (rebuild from the controller's own pairing state? no — report and let the user
pair again). Every socket and key buffer is bounds-checked; the log is flushed on critical paths only.

## Testing

- **Host, in `make test`:** `system/tests/banana-bt-test.sh` compiles `banana-bt` on this machine and runs
  `banana-bt --selftest` (the `bananad --selftest` pattern): link-key encode/decode (hex round-trip, rejection of
  garbage and wrong lengths), the `bt-cmd` parser, the `bt-list` formatter, and the SDP response parser against fixture
  byte arrays (a HID descriptor list with a report descriptor, a PnP record, a continuation response, and a malformed
  one). No radio needed.
- **Headless UI:** render the Devices page against a fabricated `bt-list` and check the rows/status text (the
  `ui/tests/run.sh` single-screen pattern).
- **On the unit (checklist in `docs/RPC-TEST.md`):** turn Bluetooth on; put a Classic pad in D-input mode and pair it;
  confirm the pad appears in RetroArch; quit and reboot with the pad off, then switch the pad on — it reconnects and
  plays; forget it and confirm it does not return; turn Bluetooth off and confirm the radio is down
  (`MGMT_OP_SET_POWERED false`, no input device); sleep and wake with Bluetooth on and confirm the radio is off during
  the sleep and the pad is back after; measure idle current with Bluetooth on as a data point.

## Risks

- **No BLE:** the user's 8BitDo Ultimate 2 is a BLE pad and will not pair with this. Testing needs a Classic controller
  (8BitDo Pro 2/SN30 in D-input, a PS4 pad, a Wii-era pad); the Ultimate 2 needs the later BlueZ/HOGP design.
- **Switch-mode pads** enumerate as a Nintendo Pro Controller via `hid-nintendo`; A/B are swapped and want a remap
  (RetroArch controller remaps, or a per-pad note in the page).
- **Hand-written ABI:** the mgmt/HIDP structs are stable kernel/user ABI but are declared by hand here; a kernel bump
  should re-check them (`docs/RPC-TEST.md` gets the check). Same for the SDP data-element parser, which only has the
  fixtures above as a guard.
- **Who connects:** most pads page the host, so the listener is the main path; a pad that only accepts host dials needs
  A (the fallback). If the test pad behaves the other way, the plan swaps the priority, not the code.
- **Power:** a powered radio costs current even idle; keeping it off by default and off in sleep is deliberate. An
  always-on mode is not offered.
- **A new daemon at boot:** kept separate from `bananad` so a Bluetooth fault cannot take power, volume or the audio
  hooks with it; `rcS` starts it after the modules.
