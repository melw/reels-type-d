# pldr/: programs that run from the SD card (no flashing)

The Type D bootloader has a second card file besides `FWDV280.BIN`: **`FW96658T.BIN`**. When that file is in
the root of the card, the bootloader loads it into RAM and runs it. **It never writes it to flash.** Remove
the card (or the file) and the unit boots its installed firmware again. Field reports show that Type C units
and the newer A3026 units run it too.

Two programs are built from the same source (`pldr.c`, `entry.S`) for that slot:

- **the patch-loader** (`build.ps1 -Card`): runs a build on top of the installed firmware, without flashing.
- **the type probe** (`python tools/make_probe.py`): shows which firmware is installed and which type it is
  for, and changes nothing. It names the firmware, not the hardware: a unit flashed with the wrong type shows
  that wrong type.

They can't both be on one card, because both use the name `FW96658T.BIN`.

## What both do at every power-on

1. **Check the bootloader** (read only). Are the bootloader routines they need exactly the Type D ones? The
   check covers 38 words from [sig.h](sig.h): the start of every routine used, plus the bootloader's id string.
2. **Read and unpack the installed firmware** from flash:
   - Type D bootloader: with its own read and decompress routines.
   - Any other bootloader: with their own SPI flash read (only the READ command `0x03`) and their own
     unpacker. The result must then pass the bootloader's image checksum, or they stop.
3. **Recognise it** by its unpacked size and a 32-bit word sum, as one of the firmware versions in
   [bases.txt](bases.txt):
   - patch-loader, known base: patch it in RAM into the build it was made from (the `-Build` number). The build
     itself (already flashed) starts as it is.
   - type probe, known firmware: put a "Type probe / TYPE X / <firmware name>" screen in place of its
     start-up picture.
   - both, unknown firmware: put "Unknown firmware" in place of its start-up picture, and **don't patch
     it**. If no start-up picture can be found, pause for about 15 s instead.
4. **Start it** exactly like a normal boot from flash.

Every screen also shows a **code**, `SSSSSSSS-ZZZZZZ`: the word sum, then the low 24 bits of the size, in hex.
For an unknown firmware, the code is what to report. The screen is assembled at boot from precomputed JPEG
pieces (`tools/splashgen.py`, `tools/jpegbits.py`). It's written only where the old picture was, and only if
it fits.

Unknown firmware is never patched. The mod calls into the firmware at fixed addresses, so it must never be
applied to a firmware whose layout we haven't checked.

The patch-loader contains only our code and the target build's words at the places that differ, no complete
Kodak firmware. For the current base list it's about 175 KB. The probe is about 140 KB.

## Building

```powershell
.\build.ps1 -Build <N> -Card                 # patch-loader -> out\build-<N>\FW96658T.BIN (next to FWDV280.BIN)
.\build.ps1 -Build <N> -Card -Drive E        # ...and copy FW96658T.BIN to the SD card in E:
.\build.ps1 -Build <N> -Card -Dump <file>    # ...and test it in an emulator first (see below)

python tools\make_probe.py --out out\probe\FW96658T.BIN [--dump <file>]   # type probe
```

Both need Python with Pillow and numpy (`pip install pillow numpy`) for the screens.

- `tools/pldr_common.py`: the shared steps. It loads and verifies the bases, picks the highest JPEG quality
  at which every screen fits its picture, compiles with the WSL toolchain, runs the `.bss` and `$k0/$k1`
  audits, and writes the raw image (header, size word, bootloader checksum).
- `tools/make_pldr.py`: the patch-loader. It finds the words in which each base differs from the target,
  stores each target word once in a shared pool, writes `build\pldr\bases.h` and compiles with `-DPATCH`.
- `tools/make_probe.py`: the probe. It writes one screen per known firmware, plus the unknown one, into
  `build\probe\`. Its device-test options are `--exclude NAME` (treat a known firmware as unknown) and
  `--badsig` (act as if the bootloader weren't Type D).

## The bases

[bases.txt](bases.txt) lists every firmware the programs recognise. Each line has its type, the file offset of
its start-up picture, its use, and the SHA256 of the packed file. Use `patch` means the patch-loader patches it
and the probe shows it. Use `probe` means only the probe shows it. The files themselves aren't committed. Put
them where `bases.txt` says:

| name | file | where to get it |
|---|---|---|
| `stockD` | `pldr\bases\stockD.BIN` | Kodak's stock Type D `FWDV280.BIN` |
| `vdD` | `base\FWDV280.BIN` | videodoctor's build: the build base, see [../base/README.md](../base/README.md) |
| `fw270`, `fw280`, `fw292` | `pldr\bases\fw<N>.BIN` | the release posts in the forum thread, or rebuild from the release commit with the command in [CHANGELOG.md](../docs/CHANGELOG.md) |
| `fw271`, `fw272` | `pldr\bases\fw<N>.BIN` | the release posts in the forum thread |
| `0dan0A` … `0dan0D` | `pldr\bases\0dan0<X>.BIN` | 0dan0's latest Type A/B/C/D builds ([github.com/0dan0/4reels](https://github.com/0dan0/4reels)) |

A missing file is skipped with a warning. For the patch-loader, that means units with that firmware boot
unpatched. A file with the wrong SHA256 stops the build. Keep the order of the lines, because the probe's
screen numbers follow it.

**Adding a patch base**: add a line with use `patch`, and the file. The new base must have the same unpacked
size as the target and a fingerprint of its own. The build checks both, and checks that applying its patches
gives exactly the target. For a firmware with a different code layout, a full-image patch is still correct,
because every differing word is replaced. But check that the unit's settings memory means the same thing to
both firmwares.

## The emulator test

`tools/emu_pldr.py` runs either program in a MIPS emulator (unicorn, `pip install unicorn`). It uses the real
bootloader taken from a flash dump of a Type D unit, so the bootloader check runs against real code and the
real decompressor unpacks the firmware. The SPI controller model accepts only READ (`0x03`). The timer counts
only while enabled, as on the hardware. The tests:

- **Each known firmware** in the dump's firmware area: RAM must end up byte-identical to the expected image.
  For the patch-loader that's the target; for the probe, the firmware with its screen.
- **An unknown (altered) firmware**: its code on the "Unknown firmware" screen, not patched, no pause.
  Without a start-up picture: a pause, then it starts unchanged.
- **Another bootloader**: their own flash read and unpack, no bootloader call. With corrupted flash data:
  they stop and don't start the firmware.
- **Every test**: no flash write is ever attempted.

A flash dump is a 4 MB read of the unit's SPI flash chip (U14, a Macronix MX25L3233F-class chip), made with
a SOIC-8 clip and a 3.3 V SPI reader. The dump contains Kodak's bootloader and firmware, so it isn't
committed. Without a dump, the test is skipped, and the patch-loader's `manifest.txt` says so.

## Safety

- Only the card is written. Neither program writes flash. They read it only with the bootloader's read
  routine, or with the READ command; the emulator test fails on anything else.
- If anything doesn't check out, they stop with a black screen. Remove the card and the unit boots its
  installed firmware.
- `FWDV280.BIN` on the same card wins: that boot flashes it, and the card file runs from the next boot on.
- **Never put `LDDV280.BIN` on a card.** That name makes the bootloader rewrite itself. `deploy.ps1` refuses
  a card that has one.

How it works in detail (bootloader addresses, handoff block, SPI controller): [../docs/TECHNICAL.md](../docs/TECHNICAL.md#bootloader-and-the-patch-loader).
