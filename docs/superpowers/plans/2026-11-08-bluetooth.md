# Bluetooth Classic Gamepads Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pair a Bluetooth Classic HID gamepad on the handheld, keep it paired across reboots, and have it appear to RetroArch as a normal input device — with no D-Bus, no BlueZ and no udev.

**Architecture:** A new `banana-bt` daemon talks to the kernel's Bluetooth management socket (power, discovery, pairing, link keys), runs a small SDP client to fetch the HID report descriptor, and listens on the HID L2CAP PSMs to add the pad through `HIDPCONNADD`. A Settings > Bluetooth page drives it over files in `/run/banana`.

**Tech Stack:** POSIX C99 (kernel mgmt/HIDP/L2CAP ABI declared locally), Buildroot local package, banana-ui (C++17) Settings page, shell test harness.

**Spec:** `docs/superpowers/specs/2026-11-08-bluetooth-design.md`

## Global Constraints

- **No D-Bus, BlueZ (library or daemon), systemd, udev, NetworkManager, PulseAudio.** Kernel ABI only; no new package dependencies.
- C99, `-Wall -Wextra -Werror`; every wait goes through `poll()`; no busy loops or sleep-polling; handle `EINTR`/`EAGAIN`; bounded buffers.
- `bluetooth = off` is the default and the radio is powered down around a sleep. Never power it on at boot unless the setting says so.
- Exact files:
  - `/run/banana/bt-state` — `off`, `on`, `scanning`, `pairing <addr>`, `connected <n>`, `error <why>`
  - `/run/banana/bt-list` — `addr<TAB>name<TAB>flags`, flags `p` (paired), `c` (HIDP session up)
  - `/run/banana/bt-cmd` — `on`, `off`, `scan`, `scan stop`, `pair <addr>`, `connect <addr>`, `forget <addr>`
  - `/data/banana/bt-paired` — `addr<TAB>name`
  - `/data/banana/bt-keys` — `addr<TAB>32 hex<TAB>type`
  - `/data/banana/logs/banana-bt.log`
- Every settings key is in `ui/src/settings_defaults.cpp`'s `KEYS` or `tools/check-systems.py` fails (the 0.5.12 lesson).
- Every user-visible string is a `tr()` key present in `ui/assets/lang/{en,es,de}.json`; `tools/check-lang.py` fails otherwise.
- `make -s test` passes after every task; host tests need no root, no radio and no `/run`.

## Review Focus

- A pad that only accepts host-initiated connections: pressing A must still connect it (checklist, Task 9).
- No adapter at all (no `hci0`): the daemon must not crash or spin; `bt-state` reports `error no adapter` and toggling Off/On tries again (checklist, Task 9).
- A malformed or continued SDP response: no `HIDPCONNADD` with a bad descriptor, no crash (`sdp: reject truncated`, Task 3; continuation, Task 3).
- A corrupt `/data/banana/bt-keys`: skip the bad line, keep the rest, still allow re-pairing (`keys: skip garbage line`, Task 4).
- Radio state after Off or a sleep: powered down and not connectable, back only if it was on (`power: off stays off after resume`, Task 8).

---

### Task 1: The setting, its schema entry and the On/Off row

**Files:**
- Modify: `system/rootfs/etc/banana/settings.conf`
- Modify: `ui/src/settings_defaults.cpp` (`KEYS`)
- Modify: `ui/src/settings.cpp` (Settings rows)
- Modify: `ui/assets/lang/en.json`, `ui/assets/lang/es.json`, `ui/assets/lang/de.json`

**Interfaces:**
- Consumes: nothing.
- Produces: settings key `bluetooth` (`on`/`off`, default `off`); the UI strings `Bluetooth` (section and row) and its help text (`Devices` arrives with its page in Task 7).

- [ ] **Step 1: Add the key to the template and the schema**

In `settings.conf`, after `adb = off`:

```sh
# bluetooth: the radio, for Bluetooth Classic gamepads (Settings > Bluetooth). Off keeps it powered down; the frames
# and the pairing are managed by banana-bt.
bluetooth = off
```

In `KEYS` (after `bezels`): `{ "bluetooth", "off", ENUM, ON_OFF, 0, 0 },`

- [ ] **Step 2: Run the guard and watch it fail on the missing UI strings**

Run: `python3 tools/check-systems.py && python3 tools/check-lang.py`
Expected: `check-systems` passes; `check-lang` still passes at this point (the row does not exist yet). This step is the baseline.

- [ ] **Step 3: Add the row and the strings**

In `settings.cpp`, new group after `System`'s last row:

```cpp
const char* B = "Bluetooth";
add(B, "phone", "Bluetooth", "The radio for Bluetooth Classic gamepads (8BitDo Pro, SN30, PS4 pads). Off keeps it powered down; pairing lives on the next row. Bluetooth Low Energy pads (8BitDo Ultimate) are not supported.", menu, "bluetooth", { "On", "Off" }, { "on", "off" });
```

Add the catalogue keys (`Bluetooth` and the row's help text) to all three JSON files, same style as the adjacent rows.

- [ ] **Step 4: Run the checks**

Run: `python3 tools/check-systems.py && python3 tools/check-lang.py`
Expected: both print their success lines.

- [ ] **Step 5: Commit**

```bash
git add system/rootfs/etc/banana/settings.conf ui/src/settings_defaults.cpp ui/src/settings.cpp ui/assets/lang/en.json ui/assets/lang/es.json ui/assets/lang/de.json
git commit -m "settings: add the bluetooth key and its rows"
```

---

### Task 2: The daemon skeleton, the host test harness, and boot wiring

**Files:**
- Create: `system/bananad/banana-bt.c`
- Create: `system/bananad/bt-abi.h`
- Create: `system/tests/banana-bt-test.sh`
- Modify: `system/bananad/Makefile`
- Modify: `system/rootfs/etc/init.d/rcS`
- Modify: `Makefile` (`test` target)

**Interfaces:**
- Consumes: nothing.
- Produces: binary `banana-bt` in `/usr/bin`; `banana-bt --selftest <topic>` where topics are `keys`, `cmd`, `list`, `sdp`, `mgmt`; `bt-abi.h` with the mgmt/HIDP/L2CAP structs and constants used by later tasks.

- [ ] **Step 1: Write the failing host test**

`system/tests/banana-bt-test.sh` (the `hud-test.sh` pattern):

```sh
#!/bin/sh
# banana-bt's pure parts, tested without the handheld or a radio: the key store, the command parser, the list
# formatter and the SDP parser, through the --selftest hook.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
cc -O2 -Wall -Wextra -Werror -o "$W/banana-bt" "$HERE/bananad/banana-bt.c" "$HERE/bananad/bt-sdp.c"
out=$("$W/banana-bt" --selftest all)
printf '%s\n' "$out"
fail=0
want() { printf '%s\n' "$out" | grep -qxF "$1" || { echo "FAIL: wanted \"$1\"" >&2; fail=1; }; }
want 'bt: selftest ready'
[ "$fail" = 0 ] || exit 1
echo "banana-bt: all checks passed"
```

Add `system/tests/banana-bt-test.sh` to `make test` (next to `run-retroarch-test.sh`).

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `cc: .../banana-bt.c: No such file or directory`

- [ ] **Step 3: Implement the skeleton**

`bt-abi.h`: the ABI declarations only — `struct sockaddr_hci`, `HCI_DEV_NONE`, `HCI_CHANNEL_CONTROL`, the mgmt opcodes/event codes used later (`SET_POWERED`, `SET_CONNECTABLE`, `SET_DISCOVERABLE`, `SET_PAIRABLE`, `START_DISCOVERY`, `STOP_DISCOVERY`, `PAIR_DEVICE`, `CANCEL_PAIR_DEVICE`, `UNPAIR_DEVICE`, `SET_DEVICE_FLAGS`, `LOAD_LINK_KEYS`, `READ_INDEX_LIST`, `DEVICE_FOUND`, `DEVICE_CONNECTED`, `DEVICE_DISCONNECTED`, `DEVICE_PAIRED`, `NEW_LINK_KEY`, `INDEX_ADDED`, `INDEX_REMOVED`) and the fields of their payloads, `struct hidp_connadd_req` + `HIDPCONNADD`/`HIDPCONNDEL`, L2CAP PSM sockets. Comment each block with the uapi header it mirrors.

`banana-bt.c`: `main()` accepts only `--selftest <topic>` for now and prints `bt: selftest ready`; without it, print usage and exit 1. Include `bt-sdp.h` (a stub file created here returning 0 and printing nothing, replaced in Task 3).

`system/bananad/Makefile`: add `banana-bt: banana-bt.c bt-sdp.c bt-abi.h` (same flags), and install it.

`rcS`, after the pwm-fan/battery modprobes:

```sh
# Bluetooth: the WCN3950's HCI over serdev (the firmware is in /lib/firmware/qca); banana-bt owns the radio after this
modprobe -a bluetooth hci_uart btqca hidp 2>/dev/null
[ -x /usr/bin/banana-bt ] && banana-bt &
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/banana-bt.c system/bananad/bt-abi.h system/bananad/bt-sdp.c system/bananad/bt-sdp.h system/bananad/Makefile system/tests/banana-bt-test.sh system/rootfs/etc/init.d/rcS Makefile
git commit -m "banana-bt: daemon skeleton, selftest hook and boot wiring"
```

---

### Task 3: The SDP client (report descriptor and PnP ids)

**Files:**
- Modify: `system/bananad/bt-sdp.c`, `system/bananad/bt-sdp.h`
- Modify: `system/bananad/banana-bt.c` (`--selftest sdp`)
- Modify: `system/tests/banana-bt-test.sh`

**Interfaces:**
- Consumes: `bt-abi.h` (nothing from it here).
- Produces:
  - `size_t bt_sdp_search(uint8_t *buf, size_t cap, uint16_t uuid, const uint16_t *attrs, size_t n_attrs)` — a ServiceSearchAttributeRequest PDU, 0 on overflow.
  - `int bt_sdp_parse(const uint8_t *pdu, size_t len, uint8_t *rd, size_t rd_cap, size_t *rd_len, uint16_t ids[4], uint8_t *cont, size_t *cont_len)` — 0 ok, -1 malformed; `ids` = vendorIdSource, vendorId, productId, version (0 when absent); `*cont_len` = continuation bytes to re-request (0 = complete).

- [ ] **Step 1: Write the failing selftest assertions**

Add to `banana-bt-test.sh`:

```sh
want 'sdp: search 1124 len 19'
want 'sdp: report 64 bytes'
want 'sdp: pnp 0x054c 0x05c4 0x0100'
want 'sdp: reject truncated'
want 'sdp: continuation 2'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: FAIL on the first `sdp:` line.

- [ ] **Step 3: Implement build and parse**

ServiceSearchAttributeRequest (PDU id 0x06, transaction 1) with the UUID-16 pattern `0x1124` (HID) / `0x1200` (PnP Information), attribute ids as 16-bit ids, maximum attribute byte count 0xFFFF, empty continuation. Parse the response's data-element sequence (0x35 sequence), attribute lists (0x35) and values; walk the HID descriptor list for the report descriptor (descriptor type `0x22`); read the PnP ids (`0x0202`–`0x0204`). Reject lengths that run past the PDU. Fixtures live in the selftest as `static const uint8_t` arrays with comments naming what produced them.

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/bt-sdp.c system/bananad/bt-sdp.h system/bananad/banana-bt.c system/tests/banana-bt-test.sh
git commit -m "banana-bt: SDP client for the HID report descriptor and PnP ids"
```

---

### Task 4: mgmt core — adapter, power, connectable, commands and the link-key store

**Files:**
- Modify: `system/bananad/banana-bt.c`
- Modify: `system/tests/banana-bt-test.sh`

**Interfaces:**
- Consumes: `bt-abi.h`; the settings file (`bluetooth`).
- Produces: `bt-state`/`bt-cmd` handling, `mgmt_open()`, `mgmt_cmd(fd, op, index, param, len, reply, cap)`, `keys_load(path)`, `keys_save(path, addr, key, type)`, and the `keys`/`cmd`/`list` selftest topics.

- [ ] **Step 1: Write the failing selftest assertions**

```sh
want 'keys: roundtrip 00:1A:7D:DA:71:13 0123456789ABCDEF0123456789ABCDEF 0'
want 'keys: reject short'
want 'keys: skip garbage line'
want 'cmd: scan stop'
want 'cmd: reject forget nonsense'
want 'list: 1 paired 0 connected'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: FAIL on the first `keys:` line.

- [ ] **Step 3: Implement**

- `mgmt_open()`: `socket(AF_BLUETOOTH, SOCK_RAW|SOCK_CLOEXEC, BTPROTO_HCI)`, bind `{HCI_DEV_NONE, HCI_CHANNEL_CONTROL}`.
- Event loop: `poll()` on the mgmt fd, the `/run/banana` inotify fd and the HIDP sockets (later tasks); `READ_INDEX_LIST` at start; `MGMT_EV_INDEX_ADDED` → if the setting is `on`, `SET_POWERED true` then `SET_CONNECTABLE true`; `INDEX_REMOVED` → `error no adapter`.
- Commands: on `IN_CLOSE_WRITE` for `bt-cmd`, read one line, dispatch `on`/`off` (`SET_CONNECTABLE false` before `SET_POWERED false`), unlink the file, rewrite `bt-state`.
- Keys: strict `addr<TAB>32 hex<TAB>1 digit` parsing; skip bad lines; write via temp + fsync + rename; `LOAD_LINK_KEYS` after power-on.
- State machine: `off` → `on` → `scanning` → `connected <n>`; `error <why>` cleared by the next command or successful event.

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/banana-bt.c system/tests/banana-bt-test.sh
git commit -m "banana-bt: mgmt power, connectability, commands and the link-key store"
```

---

### Task 5: Discovery, pairing, trust and the paired store

**Files:**
- Modify: `system/bananad/banana-bt.c`
- Modify: `system/tests/banana-bt-test.sh`

**Interfaces:**
- Consumes: Task 4's mgmt core and stores.
- Produces: `scan`/`scan stop`/`pair <addr>`/`forget <addr>` handling; `bt-list` updates; `bt-paired` persistence; the `DEVICE_FOUND`/`DEVICE_PAIRED`/`NEW_LINK_KEY` handling used by Task 6.

- [ ] **Step 1: Write the failing selftest assertions**

```sh
want 'mgmt: device found 00:1A:7D:DA:71:13 "Wireless Controller"'
want 'list: 00:1A:7D:DA:71:13 Wireless Controller p'
want 'paired: skip line without tab'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: FAIL on the first `mgmt:` line.

- [ ] **Step 3: Implement**

- `scan` → `START_DISCOVERY` (BR/EDR); `scan stop` → `STOP_DISCOVERY`.
- `MGMT_EV_DEVICE_FOUND`: decode address, name (empty is fine) and flags; upsert `bt-list` and rewrite the file.
- `pair <addr>` → `PAIR_DEVICE` (io capability NoInputNoOutput, no PIN); on `DEVICE_PAIRED` save the key from `NEW_LINK_KEY`, `SET_DEVICE_FLAGS` trusted + `bt-paired`, then signal Task 6 to connect.
- `forget <addr>` → `HIDPCONNDEL`, `UNPAIR_DEVICE`, remove from both stores.
- `bt-state`: `pairing <addr>` while a pair is in flight; timeout after 30 s → `error pairing timed out`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/banana-bt.c system/tests/banana-bt-test.sh
git commit -m "banana-bt: discovery, pairing and paired-device storage"
```

---

### Task 6: HIDP — listen, accept, connect (and the dial fallback)

**Files:**
- Modify: `system/bananad/banana-bt.c`
- Modify: `system/tests/banana-bt-test.sh`

**Interfaces:**
- Consumes: Task 3's SDP client, Task 4/5's mgmt state and paired devices.
- Produces: `hidp_add(bdaddr_t addr, int ctrl, int intr)`, `hidp_del(bdaddr_t addr)`, the PSM-17/19 listeners, and the `connect <addr>` command.

- [ ] **Step 1: Write the failing selftest assertions**

```sh
want 'hidp: req gba 0000 0000 24'
want 'hidp: req pnp 054c 05c4 0300'
want 'hidp: reject empty descriptor'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: FAIL on the first `hidp:` line.

- [ ] **Step 3: Implement**

- On power-on: bind and listen on L2CAP PSM 17 (control) and 19 (interrupt), `SOCK_SEQPACKET`; accept pairs; for a paired address run SDP (`bt_sdp_search`/`bt_sdp_parse` with the UUID `0x1124` then `0x1200`, following continuation up to 4 rounds), then `ioctl(fd, HIDPCONNADD, &req)` with the accepted sockets, descriptor and ids; on failure log and close the sockets.
- `HIDPCONNDEL` on disconnect, forget and controller power-off.
- `connect <addr>`: dial PSM 17/19 to the pad and use the same path (the fallback for pads that do not page the host).
- `bt-list` flag `c` is set only after `HIDPCONNADD` succeeds; `bt-state` `connected <n>` counts those.

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/banana-bt.c system/tests/banana-bt-test.sh
git commit -m "banana-bt: HIDP listener, SDP-driven connadd and the dial fallback"
```

---

### Task 7: The Devices page

**Files:**
- Modify: `ui/src/settings.h` (`Page` enum, method declarations)
- Modify: `ui/src/settings.cpp` (`pressBluetooth`, `renderBluetooth`, dispatch, `BANANA_RUN`)
- Modify: `ui/assets/lang/{en,es,de}.json`
- Modify: `ui/tests/run.sh`

**Interfaces:**
- Consumes: `bt-state`, `bt-list`, `bt-cmd` (Global Constraints), `BANANA_RUN` (default `/run/banana`).
- Produces: the `Devices` row (`btdevices`, `Item::PAGE`) and its page: header from `bt-state`, rows from `bt-list`, A writes `pair <addr>` (or `connect <addr>` when it is already paired), X writes `forget <addr>`, opening writes `scan`, leaving writes `scan stop`.

- [ ] **Step 1: Write the failing UI test**

In `ui/tests/run.sh`, in the settings block, after the existing settings checks:

```sh
mkdir -p "$FX/run/banana"
printf 'scanning\n' > "$FX/run/banana/bt-state"
printf '00:1A:7D:DA:71:13\tWireless Controller\tp\n' > "$FX/run/banana/bt-list"
BANANA_RUN="$FX/run/banana" "$HERE/banana-ui" --headless "$OUT/$size-bt" --single --size 640x557 --assets "$HERE/assets/rnds" \
    --systems "$HERE/tests/systems.conf" --roms "$FX/roms" --data "$FX/data" \
    --script "press start; press a; press r; press r; press r; press r; press r; press down; press a; press a; wait 1" 2> "$OUT/$size-bt.log" ||
    { echo "FAIL $size: banana-ui (bluetooth page) exited $?"; fail=1; }
[ -s "$OUT/$size-bt-top.png" ] || { echo "FAIL $size: no bluetooth shot"; fail=1; }
grep -qx 'pair 00:1A:7D:DA:71:13' "$FX/run/banana/bt-cmd" || { echo "FAIL $size: A didn't ask to pair"; fail=1; }
rm -rf "$FX/run"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make -s test`
Expected: FAIL `no bluetooth shot` (the page does not exist).

- [ ] **Step 3: Implement the page**

- Add the `Devices` row under the Bluetooth row (Task 1's group) and its strings (`Devices`, the help text) to the three catalogues.
- `Page` gains `BLUETOOTH`; `pressBluetooth`/`renderBluetooth` follow `pressNetworks`/`renderNetworks`.
- Read `bt-state` and `bt-list` from `getenv("BANANA_RUN") ?: "/run/banana"`; parse `addr<TAB>name<TAB>flags`; draw name and a paired/connected marker; empty list draws "No devices yet".
- On open: write `scan`; on B: write `scan stop` and return to `LIST`.
- A on a row writes `pair <addr>` (unpaired) or `connect <addr>` (paired); X writes `forget <addr>` with the existing confirm-twice pattern.
- Catalogue keys for every new literal; the row's count/status text is `bt-state`'s first token, capitalised.

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -s test`
Expected: `banana-ui: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add ui/src/settings.h ui/src/settings.cpp ui/assets/lang/en.json ui/assets/lang/es.json ui/assets/lang/de.json ui/tests/run.sh
git commit -m "settings: the Bluetooth devices page"
```

---

### Task 8: Power — sleep hook and Off semantics

**Files:**
- Create: `system/rootfs/etc/banana/sleep.d/15-bt`
- Modify: `system/bananad/banana-bt.c`
- Modify: `system/tests/banana-bt-test.sh`

**Interfaces:**
- Consumes: Task 4's power path.
- Produces: `SIGUSR1` = radio down for a sleep (remember whether it was on), `SIGUSR2` = restore it.
- `sleep.d/15-bt` (executable):

```sh
#!/bin/sh
# The Bluetooth radio goes down for a sleep: a powered controller costs current and can wake the SoC. banana-bt
# remembers whether it was on and brings it back on the way up; pairing and link keys are untouched.
case $1 in
pre)  pid=$(pidof banana-bt) && kill -USR1 "$pid" ;;
post) pid=$(pidof banana-bt) && kill -USR2 "$pid" ;;
esac
exit 0
```

- [ ] **Step 1: Write the failing selftest assertions**

```sh
want 'power: suspend remembered on'
want 'power: resume restores on'
want 'power: off stays off after resume'
```

- [ ] **Step 2: Run it to verify it fails**

Run: `sh system/tests/banana-bt-test.sh`
Expected: FAIL on the first `power:` line.

- [ ] **Step 3: Implement**

`SIGUSR1`: `SET_CONNECTABLE false` + `SET_POWERED false`, remember the previous on/off; `SIGUSR2`: power on + connectable + `LOAD_LINK_KEYS` only if it was on. A `off` command clears the remembered state. `sh -n` the hook.

- [ ] **Step 4: Run the test to verify it passes**

Run: `sh system/tests/banana-bt-test.sh && sh -n system/rootfs/etc/banana/sleep.d/15-bt`
Expected: `banana-bt: all checks passed`

- [ ] **Step 5: Commit**

```bash
git add system/bananad/banana-bt.c system/tests/banana-bt-test.sh system/rootfs/etc/banana/sleep.d/15-bt
git commit -m "banana-bt: radio down for a sleep, back only if it was on"
```

---

### Task 9: Docs, roadmap and the on-device checklist

**Files:**
- Modify: `docs/RPC-TEST.md`
- Modify: `docs/ROADMAP.md`
- Modify: `docs/EMULATORS.md` (one line under Settings)

**Interfaces:**
- Consumes: everything above.
- Produces: the hardware checklist the next session with a Classic pad runs.

- [ ] **Step 1: Add the hardware checklist to `docs/RPC-TEST.md`**

```
Bluetooth (banana-bt)
- Settings > Bluetooth On; `banana-bt.log` says the controller was powered (`hci0`), no error.
- Pad in D-input mode → Devices → it appears; A pairs; `bt-keys` has the key; pad plays in RetroArch.
- Off/On the pad; `bt-list` drops `c` and comes back; buttons work again.
- Reboot with the pad off; switch it on; it reconnects without touching Settings.
- Forget it; it does not come back; pairing again works.
- Bluetooth Off: `bt-state` is `off`, no pad in `/proc/bus/input/devices`; idle current noted.
- Sleep and wake with Bluetooth on: radio off during the sleep, back after; pad reconnects.
```

- [ ] **Step 2: Update `docs/ROADMAP.md`**

Replace the Bluetooth *In progress* bullet's last line with the implemented state and keep "no Classic pad to test with" open; add the done entry only once the checklist above has run.

- [ ] **Step 3: Update `docs/EMULATORS.md`**

One line under Settings: Bluetooth is Settings' own (not RetroArch's), a HIDP pad shows up as a normal input device.

- [ ] **Step 4: Run the suite**

Run: `make -s test`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add docs/RPC-TEST.md docs/ROADMAP.md docs/EMULATORS.md
git commit -m "docs: Bluetooth on-device checklist and status"
```
