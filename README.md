# Kodak Reels — Type D firmware mod

Custom firmware for the **Type D** revision of the Kodak Reels 8mm / Super 8 film scanner. It adds an
on-screen display, a settings menu that survives power cycles, higher-quality encoding, and guards that
stop a recording when the reel ends or the card stalls.

It builds on the work of **videodoctor** (the Type D base firmware: 1600×1200, 18 fps, working auto
exposure) and **0dan0** (the original Type A/B/C mods, the menu approach and the font). This repo
contains only our source, the build scripts and the documentation. **It contains no firmware binaries.**
You supply videodoctor's base yourself, and the build checks it by SHA256.

> **Not affiliated with Kodak.** Flashing custom firmware is at your own risk. Test on a throwaway reel
> before trusting it with film you can't replace.

## What it does

| | |
|---|---|
| **Picture** | Bitrate ×4 by default (≈32 Mb/s, menu: ×1/×2/×4). FilmGrain off, 3D noise reduction on. 1600×1200 cropped to the image (or native 1728×1296 incl. sprocket holes, as a build option) |
| **Frame rate** | 18 or 16 fps (file metadata; pick the one that matches your film) |
| **White balance** | Manual R/G/B gains, live in the preview, saved across power cycles |
| **Date** | Sets the recording date written into files (1980–2100), so a reel can carry the year it was shot |
| **OSD** | Frame counter, WB, settings, live Qp / frame size / total size, gain and exposure, build number |
| **Reel end** | Detects when the film runs out, stops the motor and closes the file cleanly (toggle: `ASt`) |
| **Stall guard** | If no frame is encoded for ~8 s: `REC STALLED` warning; ~12 s: stops the recording cleanly and saves the file |

Using it: **[docs/USER-GUIDE.md](docs/USER-GUIDE.md)**. Building it:
**[docs/BUILDING.md](docs/BUILDING.md)**.

## Quick build

Windows 10/11 with WSL (Ubuntu) and Python 3:

```powershell
# one-time: toolchain in WSL
wsl sudo apt install gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu

# one-time: base\FWDV280.BIN (videodoctor's release) and bin\*.exe (packing tools)
#           -- see base\README.md and bin\README.md

.\build.ps1 -Build 271              # -> out\build-271\FWDV280.BIN
.\build.ps1 -Build 271 -Drive E     # ...and copy it to the FAT32 SD card in E:
```

Build 270 from this source is **byte-identical** to the published FW 270. That is the reproducibility
check if you want one.

## Repo layout

```
src/osdmenu.c        the whole mod (C + a few small asm stubs)
src/font_data.h      7x12 bitmap font (from 0dan0's project)
tools/osd_build.py   splices the mod into the base and applies every patch (each one asserts the original instruction first)
build.ps1            compile -> splice -> pack -> stage
deploy.ps1           copy a build to an SD card
base/                videodoctor's base firmware goes here (not committed)
bin/                 the two Novatek packing tools go here (not committed)
docs/                user guide, building, technical notes, changelog
```

## Documentation

- [USER-GUIDE.md](docs/USER-GUIDE.md): flashing, the menu, what the screen shows, troubleshooting
- [BUILDING.md](docs/BUILDING.md): toolchain, first build, flashing, recovery
- [TECHNICAL.md](docs/TECHNICAL.md): hooks, addresses, memory map, rate control, OSD drawing, how to extend it
- [CHANGELOG.md](docs/CHANGELOG.md)

## Status

FW 270 is the current release. Contributions and ports are welcome. Read the hook-safety rules in
TECHNICAL.md first.

## Credits and licence

- **videodoctor**: the Type D base firmware and the original Type D reverse engineering
- **0dan0**: the Kodak Reels mod project for Types A/B/C, the passive-button menu approach, the font
- Everyone on the tinkerdifferent.com Kodak Reels thread who tested builds and sent freeze reports

MIT licence, see [LICENSE](LICENSE). The base firmware is not part of this repo and is not covered by it.
