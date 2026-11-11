# Changelog

Build numbers are the on-screen `FW N`. Only released builds and milestones are listed. Numbers in
between were development and test builds. The top entry is the latest release. Where a `Rebuild` line
is given, that command run on the release's commit gives a `FWDV280.BIN` with exactly that SHA256 (see
[BUILDING.md](BUILDING.md#reproducibility-check)).

## FW 280
Rebuild: `.\build.ps1 -Build 280 -Date 2026-11-10`, SHA256
`be659c6552a7df370efb8b18de0590d39d95b92331d3d59f356071c9b29982ac`

- **Manual exposure.** The `Exp:` row in the menu switches between `A` (auto, stock behaviour) and
  `M` (manual). The first +/− or Left/Right press switches to manual at the current exposure. +/− then
  adjusts by ~6 % and Left/Right by one stop. One more Left/− at the minimum (20 µs), or `Reset`,
  returns to auto. The setting is saved across power cycles.
- **Reel-end auto-stop is off in manual** (it relies on the auto exposure). The menu shows
  `ASt: off (M)`. Your ASt setting comes back when you return to auto, after a ~20 s settling time.
- **The auto-exposure calculation is paused in manual.**
- **New boot splash:** a feature summary with the build number and build date.
- Builds 273–279 were shared for internal testing only.

## FW 272
- **Clearer menu state:** the `[ ]` brackets and the `Reset` row are shown only while a recording is
  running. Outside recording, `Edit in REC` at the top of the list says where editing works. Previously
  the brackets stayed visible in preview after a recording, which looked like a live menu.
- The date brackets no longer show in plain preview.
- **WB hardening:** saved WB values are used only if all three are 16–999; otherwise stock WB applies.
  This fixes a unit whose settings memory made the mod apply ~0 gains (very dark picture,
  `WB gains 000,000,000`). The first WB edit starts from 689,256,218 if the live gains are invalid, and
  edits stop at 16 instead of 0.

## FW 271
- **Build number on the boot splash:** "Type D - FW 271" instead of "Type D - WIP". The splash is shown
  by stock code before the mod runs, so it confirms which file was actually flashed. Otherwise identical
  to 270.

## FW 270
Rebuild (commit `391530a`): `.\build.ps1 -Build 270 -NoSplash`, SHA256
`7bcb6f01b51e6b1fd4ca5d5764b3c2ca17be36bd523ff0bf4fd63ceadc5ed276`

- **OSD flicker fixed.** The OSD no longer disappears when the REC dot blinks.
- **Stall thresholds corrected** back to 20/30 OnTimer ticks (~8 s warning, ~12 s stop).
- Diagnostic erase counters built in but hidden (`DIAG_ERASE`).

## FW 265
- **Hard freeze fixed** (root cause: `jr $k1` in the OnTimer trampoline).
- **Qp floor off by default.** At x4 the old floor pinned the rate control and caused stalls.
- **Bitrate menu** (`BR`): x1 / x2 / x4, default x4.
- **Menu reworked:** `[ ]` marks the value being edited. Left/Right = coarse, +/− = fine.
- `CARD STALLED` renamed **`REC STALLED`**. It detects "no new frames", which isn't always the card.
- **Build number on screen** (`FW N`, top right).

## Earlier milestones
- **Reel-end auto-stop toggle** (`ASt`), for faded or thin film that looked like the end of the film.
- **On-screen diagnostics**: `Qp` now/lowest, `Sz` frame size now/largest, `Cum` total. They stay on
  screen after a stop or freeze.
- **Reel-end auto-stop**: exposure-based blank-frame detection, then a clean stop with the file saved.
- **Clean stop on stall**: a stalled recording stops and saves instead of running the film off the reel.
- **Recording date** setting (1980–2100), with the RTC epoch moved from 2000 to 1980.
- **Selectable frame rate** 18 / 16 fps.
- **Settings menu with NVM persistence**; manual white balance, live in the preview.
- **Card-stall warning.**
- **OSD on Type D**: draws into the display pool, in the right border.
- **Picture quality**: bitrate x4, FilmGrain off, 3D noise reduction on. 1600×1200 by default, native
  1728×1296 as a build option.
- Based on **videodoctor's Type D firmware** (1600×1200, 18 fps, working auto exposure).
