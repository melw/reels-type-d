# Building

## What you need

| | |
|---|---|
| Windows 10/11 | the packing tools are Windows executables |
| WSL with Ubuntu | for the MIPS cross compiler. Tested: Ubuntu 24.04, `mipsel-linux-gnu-gcc` 12.4 |
| Python 3 on Windows | as `python` on the PATH. Standard library only |
| `base\FWDV280.BIN` | videodoctor's Type D release, see [../base/README.md](../base/README.md) |
| `bin\ntkcalcVS.exe`, `bin\bfc4ntkVS.exe` | Novatek packing tools, see [../bin/README.md](../bin/README.md) |

Toolchain, once:

```powershell
wsl --install -d Ubuntu              # if you don't have WSL yet
wsl sudo apt update
wsl sudo apt install gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu
```

## Build

```powershell
.\build.ps1 -Build 271
```

If PowerShell refuses to run scripts:
`powershell -ExecutionPolicy Bypass -File .\build.ps1 -Build 271`.

What it does:

0. The first time only, it unpacks `base\FWDV280.BIN` to `base\vd-FWDV280-D.rbn`.
1. Compiles and links `src\osdmenu.c` in WSL, at `0x8034116c`, into free space in the base.
2. Runs `tools\osd_build.py`. This checks the base's SHA256, applies the constant patches, places the
   code and runs the `$k0/$k1` safety audit. It then installs the hooks. **Every patch first checks the
   original instruction is there**, so a wrong base fails here, not on the device.
3. Checksums, compresses and checksums again → `build\vd-osd.bcl`.
4. Stages `out\build-271\FWDV280.BIN` and a `manifest.txt` (source and firmware hashes).

A good run ends with `BUILD OK: FW 271` and lists all six hooks:

```
hook: WB call -> osd_main
hook: frame rate -> fps_stub
hook: bitrate -> bitrate_stub (menu x1/x2/x4)
hook: RC Qp clamp -> qpfloor_stub
hook: OnTimer -> my_ontimer
hook: compose blit -> wrap_compose
```

### Options

| option | effect |
|---|---|
| `-Build N` | **required**. The number shown on screen as `FW N`. Use a new number for every build you flash |
| `-Resolution native` | 1728×1296, the whole film gate including sprocket holes. Default `std` = 1600×1200 cropped to the picture |
| `-Drive E` | also copy the result to the FAT32 SD card in `E:` (runs `deploy.ps1`) |
| `-NoHooks` | diagnostic: constant patches only, none of the mod's code ever runs. No OSD. |
| `-Bitrate x1\|x2\|x4` | only with `-NoHooks`: static bitrate, because there's no menu |

### Reproducibility check

`.\build.ps1 -Build 270` from this source gives a `FWDV280.BIN` of 3,945,280 bytes, SHA256
`7bcb6f01b51e6b1fd4ca5d5764b3c2ca17be36bd523ff0bf4fd63ceadc5ed276`. That is byte-identical to the
released FW 270. If you get the same hash, your toolchain matches. With a different GCC version the
code can come out differently and still be fine, but check the full hook list and test on a throwaway
reel.

## Flash

```powershell
.\deploy.ps1 -Bin out\build-271\FWDV280.BIN -Drive E
```

This refuses any card that isn't FAT32. It copies the file, creates the empty `NVTDELFW` folder and
flushes Windows' write cache to the card. Then:

1. Safe-eject the card.
2. Insert it into the **powered-off** Reels.
3. Power on. You'll get about 30 s of black screen with the LED on. Don't cut the power.
4. Check the `FW` number on screen.

Full user-side steps are in [USER-GUIDE.md](USER-GUIDE.md#flashing).

## It didn't flash

The old `FW` number boots. First look at the card:

- **`FWDV280.BIN` is still on the card** → the bootloader didn't accept it. Usual causes:
  - The card is exFAT or NTFS, or larger than 32 GB. The bootloader only reads FAT32.
  - The unit wasn't really off (standby is not off). It needs a cold start.
  - The file was incomplete because the card was pulled without a safe-eject.
  - The `NVTDELFW` folder is missing or misspelled. The flash then runs, but repeats on every boot.
- **The file is gone, but the old FW number shows** → the flash ran with a different file than you
  think. Check which build you copied (`manifest.txt` has its hash).

## Recovery

The mod never hooks the boot path: every hook is in capture, encode or UI code that only runs once
the unit is up. So a bad build still boots far enough to reflash from the card. Flash a known-good
build, or videodoctor's original `FWDV280.BIN`, the normal way.

The risky changes are ones that run **during boot**: a hook at the entry of a function on the
motor-init path, or anything that crashes before the card is read. Keep new hooks out of that path.
See [TECHNICAL.md](TECHNICAL.md#rules-for-new-hooks).

## Debugging on the device

- **The screen is the debugger.** Put what you need to see on the OSD.
- **`-NoHooks` bisect.** Rebuild with -NoHooks. If the problem persists, it's in the
  base firmware; if it goes away, it's in our code. Then switch features off one at a
  time: `#define` switches at the top of `osdmenu.c`
  (`WB_OVERRIDE`, `OSD_DRAW_WHILE_REC`, `QPFLOOR_CLAMP`, `DIAG_ERASE`).
- UART console at 115200 baud is available on the board, for memory dumps and crash output.
