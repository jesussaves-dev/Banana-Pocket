# Pad MCU serial protocol (for `banana-pad`)

Status: draft 1, task K4.1. Written for the clean-room `banana-pad` driver
(`docs/PLAN_CUSTOM_KERNEL.md` §K4). Source: observed behaviour of the existing
`rsinput` driver (patches 0015 and 0041, by Philippe Simons and Jacob Cook), read for what it
sends, accepts and reports. `banana-pad` is written from this document, not from that code.

Items marked **UNVERIFIED** are read off the old driver but never confirmed against the MCU. The
first job on hardware is a raw capture (§9) to settle them.

## 1. Hardware

- The pad MCU is a serial client on SoC `uart3` (GENI UART, serdev). Link: **115200 baud, 8N1 assumed,
  no flow control.**
- Supply: `vdd-supply` (`mcu_vdd` regulator). Must be on before the UART is used.
- Three optional GPIOs, board dependent:

  | Name | RPC (`.dts`) | RPC revq | Role |
  |---|---|---|---|
  | `reset` | tlmm 1, active-low | tlmm 9, active-high | MCU reset line |
  | `enable` | absent | tlmm 7, active-high | MCU enable |
  | `boot` | absent | tlmm 6, active-high | bootloader select; held inactive in normal operation |

- Identity reported to userspace comes from the DT node: `gamepad-name`, `gamepad-bus/vid/pid/rev`
  (RPC: "Retroid Pocket Classic Gamepad", bus 3, 2f24:0001, rev 1). Keep these exactly, or
  controller mappings (SDL, RetroArch autoconfig) break.

## 2. Frame format

All multi-byte values are little-endian unless stated. Same format both directions.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `A5 D3 5A 3D` |
| 4 | 1 | Sequence number (host: starts at 0, +1 per frame sent, wraps at 256) |
| 5 | 1 | Command |
| 6 | 2 | Payload length N |
| 8 | N | Payload |
| 8+N | 1 | Checksum |

- Total frame length = 9 + N. Shortest frame (N=0) is 9 bytes.
- **Checksum** = XOR of bytes 4 .. 7+N (sequence through last payload byte). The magic is not covered.
- The MCU's own sequence numbers are not checked by the old driver. Do not depend on them.

Commands:

| Cmd | Name | Direction |
|---|---|---|
| `01` | COMMOD (control: version, parameters) | host to MCU, MCU replies with same cmd |
| `02` | STATUS (input state) | MCU to host, streamed |

First payload byte of COMMOD is a sub-command: `02` version, `05` set parameters.

## 3. Host to MCU

### 3.1 Version request
Cmd `01`, payload `02`. Reply: cmd `01`, payload starts `02`, then the version string (ASCII,
unterminated). The reply is also the **liveness probe** (§6): any valid frame counts as alive.

### 3.2 Set parameters
Cmd `01`, payload `05 01 00 00 00 28 00 00 00 07` (10 bytes). The MCU acks with cmd `01`, payload
starting `05`. Meaning of the fields is **UNVERIFIED**: the shape reads as `05, 01, u32 0x28, u32
0x07` (big-endian words), probably report rate or filter settings. Send the bytes verbatim.

No other host commands are known. **No rumble, LED or mode command is known**; whether the MCU has
a vibration channel is unanswered (§9).

## 4. MCU to host: STATUS (cmd `02`)

Payload offsets (relative to payload start):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Button bitmask (§4.1) |
| 2 | 2 | Left trigger, raw u16 |
| 4 | 2 | Right trigger, raw u16 |
| 6 | 2 | Left stick X, raw s16 |
| 8 | 2 | Left stick Y, raw s16 |
| 10 | 2 | Right stick X, raw s16 |
| 12 | 2 | Right stick Y, raw s16 |

The old driver accepts a status frame when N >= 6 but then reads 14 payload bytes: reading beyond a
short frame is a bug. `banana-pad` requires **N >= 14** and drops shorter frames. (Real frames
likely carry exactly 14; **UNVERIFIED**.)

### 4.1 Button bits

Bit 0 is the least significant bit of the first payload byte.

| Bit | Event code | Bit | Event code |
|---|---|---|---|
| 0 | BTN_DPAD_UP | 8 | BTN_TL |
| 1 | BTN_DPAD_DOWN | 9 | BTN_TR |
| 2 | BTN_DPAD_LEFT | 10 | BTN_SELECT |
| 3 | BTN_DPAD_RIGHT | 11 | BTN_START |
| 4 | BTN_NORTH | 12 | BTN_THUMBL |
| 5 | BTN_WEST | 13 | BTN_THUMBR |
| 6 | BTN_EAST | 14 | BTN_MODE |
| 7 | BTN_SOUTH | 15 | BTN_BACK |

Report only bits that changed since the previous frame; keep that "previous state" **per device**
(the old driver held it in a function-local static shared across instances).

### 4.2 Axes

- **Sticks:** the wire value is the *negation* of the logical value. Logical = `-wire + center`.
  Then: inside the deadzone (`|v| < deadzone`) report 0, else `sign(v) * (|v| - antideadzone)`.
  Optional per-axis inversion (DT `invert-x/-y/-rx/-ry`) negates after that.
- **Range:** default +/-0x580 (1408) per stick, trigger range 0..0x610 (1552). DT `axis-range = <n>`
  sets all four sticks to +/-n. Reported range shrinks by the antideadzone.
- **Triggers, analog mode:** logical = `max - wire`, with deadzone and antideadzone as for sticks.
  Reported as ABS_Z / ABS_RZ, range 0..(max - antideadzone), flat 30.
- **Triggers, digital mode** (DT `digital-triggers`, set on both RPC boards): no ABS_Z/RZ; BTN_TL2 /
  BTN_TR2 pressed when the raw trigger value is non-zero. Note the old driver treats analog as
  `max - wire` but digital as `wire != 0`: these disagree about the resting value. Which is right
  is **UNVERIFIED** and is the first thing to capture (§9).
- Stick axes are ABS_X, ABS_Y (left) and ABS_RX, ABS_RY (right). One `input_sync` per status frame.

### 4.3 Tuning knobs

The old driver exposes 25+ module parameters (`/sys/module/rsinput/parameters/`: per-axis min,
center, max, deadzone, antideadzone, per-trigger max/deadzone/antideadzone, `update_params`).
`banana-pad` replaces them with per-device sysfs attributes under the input device, and keeps
`update_params` semantics (re-apply ranges) implicit: writing a range attribute updates the absinfo.
The d-pad/analog mode knob from the plan is a separate attribute, to be added once the MCU mode
(if any) is found.

## 5. Power-up sequence

1. Enable `vdd`. Request `boot` (if present) and `enable` (if present) and `reset` as outputs.
2. Open UART, 115200, no flow control. Install the receive callback **before** sending.
3. Set `boot` to logical 0 and `reset` to logical 0; wait 100 ms.
4. Set `enable` to logical 1 (if present), then `reset` to logical 1; wait 100 ms.
5. Send version request; wait 100 ms; send set-parameters.
6. Start the watchdog (§6), first check 1 s later.

Values above are *logical* (gpiod) levels; the DT polarity flags in §1 map them to electrical levels.
The old driver requests all three lines at logical 1, then runs steps 3-4, so **`reset` finishes at
logical 1** on both boards, which is an odd end state for a reset line (on the RPC, active-low,
that is electrically low). It works on shipping hardware, so keep the sequence exactly, but the
reset polarity and whether the MCU is released by that final transition are **UNVERIFIED**:
confirm with a scope or the schematic before changing anything here.

Remove: stop the watchdog, close UART, deassert `enable` and `reset`, disable `vdd`.

## 6. Link watchdog

The MCU sometimes goes silent. The old driver's recovery, to be kept:

- Every 5 s (first check after 1 s) it records the count of valid frames and sends a version
  request. If the count advanced since the last check, the link is alive.
- Silent: attempts 1-2, close and re-open the UART (re-set baud and flow control). From attempt 3,
  also power-cycle: `enable` off, `vdd` off, 100 ms, `vdd` on, then rerun §5 steps 3-5; next
  check after 3 s (MCU boot time). After attempt 5, back off to 30 s checks (assume a dead MCU).
- Log once when a recovery succeeds.

## 7. Receive handling (differences from the old driver)

The old driver treats each serial callback chunk as exactly one frame: it checksums the whole
chunk, discards it on mismatch, and so loses frames when two arrive in one chunk or when one is
split across chunks. `banana-pad` does proper stream framing:

- Keep a byte accumulator (fixed buffer, no allocation in the callback).
- Hunt for the magic, read N, wait until 9+N bytes are present, verify the checksum.
- On a bad checksum, drop **one byte** and hunt again (do not skip the whole claimed frame length).
- Cap N (reject N > 64, a status frame never needs more) so a corrupt length cannot stall the parser.
- The callback returns the number of bytes consumed, as serdev expects.

## 8. Device-tree contract

`banana-pad` keeps the existing binding so DTs need no change while both drivers exist:
`compatible = "gamepad,rsinput"` is claimed by whichever driver is built in. During the "old driver
stays selectable" period the new driver must use a separate compatible (proposed
`banana,pad`) and the DT picks one. Properties: `gamepad-name`, `gamepad-bus/vid/pid/rev`,
`digital-triggers`, `invert-x/-y/-rx/-ry`, `axis-range`, `trigger-left-deadzone`,
`trigger-right-deadzone`, `vdd-supply`, `reset-gpios`, `enable-gpios`, `boot-gpios`.

## 9. Hardware findings and open questions

Verified on a Retroid Pocket Classic (kernel 7.2.0-Bananix0.4, `banana,pad` bound to serdev0-0, 3756
frames, 0 bad bytes, dmesg clean). Input device Bus/Vendor/Product, KEY and ABS=1b match the old
`rsinput` baseline; only Phys differs (empty).

**Verified**

- Button bits 0-11 work (d-pad, face, L1/R1, Select = 0x400, Start = 0x800). Bits 12-15 were never
  seen, so THUMBL/THUMBR/MODE/BACK are probably not wired on this device.
- L2/R2 are digital: rest 0, pressed 1. This settles the digital-mode question in 4.2 (`wire != 0`).
- The stick words are constant 0x05dc (1500): the device has no sticks. The driver reports -1500 as
  the old driver did (centre defaults to 0); no change needed.

**Still open**

1. What do `28` and `07` in set-parameters do? Does changing them change the report rate?
2. Does the MCU accept a vibration or mode command? (Needed for the plan's rumble and d-pad/analog
   knob. Without MCU documentation this needs probing, so go carefully: unknown commands to a
   microcontroller can wedge it. Never touch the `boot` line.)
3. Any extra payload (battery, rumble state, mode bit) the old driver ignores: none confirmed.

Capture method: the read-only debugfs file `/sys/kernel/debug/banana-pad` in the driver.
