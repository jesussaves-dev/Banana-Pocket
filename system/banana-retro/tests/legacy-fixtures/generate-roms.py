#!/usr/bin/env python3
"""Generate the original, non-commercial Banana-retro save-import ROM inputs."""

from pathlib import Path
import sys


def emit(directory: Path, name: str, size: int, patches: dict[int, bytes]) -> None:
    data = bytearray(size)
    for offset, payload in patches.items():
        data[offset:offset + len(payload)] = payload
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT_DIR", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])

    # ARM branch-to-self at the GBA entry point. The mGBA auto-detection
    # markers select SRAM; no game or third-party code is present.
    emit(root / "mgba", "mgba.gba", 32 * 1024, {
        0: bytes.fromhex("fe ff ff ea"),
        512: b"SRAM_V SIIRTC_V",
    })

    # Blank Game Boy homebrew image with MBC3+RTC and 32 KiB RAM cartridge
    # header bytes. Gambatte supplies the raw SRAM and RTC files.
    emit(root / "gambatte", "gambatte.gb", 32 * 1024, {
        0x147: b"\x10",
        0x149: b"\x03",
    })

    # iNES mapper-0 battery-backed test cartridge. The PRG begins with JMP
    # $8000; its NMI vector targets that entry. No commercial content is used.
    emit(root / "fceumm", "fceumm.nes", 16 + 16 * 1024 + 8 * 1024, {
        0: b"NES\x1a\x01\x01\x02" + bytes(9),
        16: bytes.fromhex("4c 00 80"),
        16 + 0x3ffa: bytes.fromhex("00 80"),
    })
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
