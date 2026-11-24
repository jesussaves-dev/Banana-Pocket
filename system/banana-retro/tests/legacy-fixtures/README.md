# Profile-produced raw save fixtures

These content ROMs are small, generated homebrew test inputs. RetroArch ran each
one under the matching packaged core in the generated BananaOS AArch64 image,
inside QEMU, with `video_driver = "null"`, `audio_driver = "null"`, and
`--max-frames=20`. The raw files below are RetroArch's output in the sorted
per-core layout. They contain no commercial game content.

| Profile | Reported core name | Content | SRAM | RTC | Core SHA-256 |
| --- | --- | ---: | ---: | ---: | --- |
| mgba | mGBA | `mgba.gba` (32 KiB) | 131072 bytes | none | `1a3ab69ab871cdca7abc6e838bcd57a5601d9a309c46ed64c74b44f638c96242` |
| gambatte | Gambatte | `gambatte.gb` (32 KiB) | 32768 bytes | 8 bytes | `0ea14782f81a1124c09776e7abe41ac4d6f52f8fb2845b070c814b790162b2d8` |
| fceumm | FCEUmm | `fceumm.nes` (24592 bytes) | 8192 bytes | none | `c92d81870053800371cd0fa44bc217d3f9c51995331eb19de9a83b63d342a286` |

Core hashes identify the exact packaged shared objects used to generate these
fixtures. `check-saves` imports these files through the production save-store
code using their observed sizes and library-name directories, then compares the
imported bytes and confirms the source files remain unchanged.

The Game Boy fixture has an MBC3 RAM+RTC header. It exercises the separate RTC
file emitted by Gambatte. The mGBA test content emits SRAM but no separate RTC
file. This is QEMU target-software evidence; it does not qualify physical
hardware behavior or exFAT power-loss durability.
