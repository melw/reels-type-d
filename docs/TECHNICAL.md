# Technical notes

Everything here refers to videodoctor's Type D base, unpacked `vd-FWDV280-D.rbn` (SHA256
`90d2abb6…b199c6`). The addresses are specific to that image.

## Platform basics

- Novatek SoC, **MIPS32 little-endian**. The unpacked image is loaded at `0x80000000`, so
  **address = `0x80000000` + file offset**.
- `0xA…` addresses are the uncached view of the same RAM, used for hardware-shared structs such as the
  button state.
- The mod is one C file, linked at **`0x8034116c`** into free space in the base. It must end below the
  **scratch area at `0x80345000`**, which holds runtime state and is reset at every boot. The build
  checks both.
- Firmware functions are called through fixed addresses (`orig` = stock WB `0x802b7b7c`,
  `resolve` = display-pool getter `0x80006500`, `set_exp`/`set_gain` = sensor driver
  `0x801df99c`/`0x801df88c`, `ae_calc` = per-frame AE calculation `0x801e4854`, passed as `--defsym`
  at link time).
- **The unit scans at ~2 frames per second.** The file's 16/18 fps is just its playback rate.
  "Per frame" timings below are per *scanned* frame.

## Hooks

`tools/osd_build.py` installs a hook only when the ELF defines the matching symbol, and asserts the
original instruction first.

| # | site | original | becomes | runs | purpose |
|---|---|---|---|---|---|
| 1 | `0x802b7c90` | `jal 0x802b7b7c` (WB) | `jal osd_main` | every captured frame (~30/s, also in preview) | draw OSD, poll buttons, run the menu, apply WB/date/FPS, track Qp/size; tail-calls the stock WB |
| 2 | `0x801f20c8` | `li a3,0xd2f0` + `sw a3,0(a1)` | `jal fps_stub` + `nop` | record setup | frame-rate tag = fps × 3000 (54000 = 18 fps, 48000 = 16) |
| 3 | `0x80240588` | `sll v1,v1,3` | `j bitrate_stub` | record setup | `*(s0+0x50) = raw<<3 × m`, m ∈ {1,2,4}; the delay slot's original store runs, then the stub redoes it |
| 4 | `0x80241074/78` | `slti a1,s4,10` + `li v1,10` | `j qpfloor_stub` + `nop` | every encoded frame | rebuilds the rate control's `max(cap, 10)` with the menu floor (10 when off, i.e. stock) |
| 5 | `0x8011e344/48` | `addiu sp,-0xf0` + `sw s8,0xe8(sp)` | `j my_ontimer` + `nop` | movie-window timer, **~2–3 Hz** (measured) | runs the stock OnTimer, then `after_ontimer`: stall detector, reel-end detector, warnings, clean stop, OSD repaint |
| 6 | `0x8010bc70` | `jal 0x8010c164` (compose blit) | `jal wrap_compose` | every UI compose | runs the real blit, then repaints the OSD if the blit wiped it |
| 7 | `0x802b6950/6c50/6f10/70d4`, `0x802b697c/6c7c/6f20/70e4` | `jal 0x801df99c` (sensor exposure), `jal 0x801df88c` (sensor gain) | `jal ae_exp_hook`, `jal ae_gain_hook` | every AE update (every 3rd captured frame) | manual exposure: substitute the menu values, else pass through and remember the AE's own |
| 8 | `0x802b6bdc` | `jal 0x801e4854` (AE calculation, its only caller) | `jal ae_calc_hook` | every AE update | manual exposure: skip the calculation, so the AE result (and the sensor mode) stays frozen |

**Manual exposure (hook 7).** The stock AE is two 3A callbacks: init `0x802b683c` and per-frame
`0x802b6a08`, which does its work on every 3rd frame. It hands its result to the sensor driver as
exposure in µs and gain ×100. The wrappers swap in the manual values, then jump straight to the
original, using no stack.

The per-frame AE only *writes* the sensor when its own result changed. It ORs "changed" bits into
`0x80F8245C` (bit 0 exposure, bit 1 gain), and the apply functions `0x802b620c`/`0x802b61b4` check them.
So in manual the wrappers set those bits themselves. Back in Auto, they set them once more, so the AE's
own value is restored.

**Frozen calculation (hook 8).** The calculation `0x801e4854` fills the result struct `0x80F82580`
(+4 exposure µs, +8 gain ×100, +0xc sensor mode 1..4). In manual it would run open loop: the picture
no longer follows its output, so it keeps pushing in one direction and eventually changes the mode
field. A mode change goes through `0x802b60fc` → `0x80248124`, which pauses an engine, switches the
mode and resumes it, mid-recording. In the field that stalled recordings after a few minutes (FW 274).
`ae_calc_hook` returns without calling it while in manual, so the struct keeps the values from the
moment manual was entered. Back in auto it runs normally again.

Stock AE bookkeeping, for anyone extending this:

| address | meaning |
|---|---|
| `0x80F82580` | current result: +4 exposure µs, +8 gain ×100, +0xC sensor mode 1..4 |
| `0x80F82570` | the previous result (same layout), compared to decide what changed |
| `0x80F82590` | the copy the getters read; `0x80F82594` (its +4) is the exposure the OSD and reel-end detector use |
| `0x80F8245C` | "changed" bits: 0 exposure (applied by `0x802b620c`), 1 gain (`0x802b61b4`), 2 mode (`0x802b60fc`, which maps the mode through the table at `0x80DBC520` = {2,3,4,5} and calls `0x80248124`: engine state 5 = paused, switch, state 2 = running) |
| `0x80F82464` | frame divider: the per-frame callback does its work on every 3rd frame |
| `0x80F8246C` | set by the init callback |

Why it exists: above 1/120 s, the stock AE only allows whole 1/120 s steps (8333, 16666 µs: 60 Hz
anti-flicker). On dense film whose right exposure falls between two steps, it flips between them every
few seconds. Manual exposure bypasses the steps because it substitutes the value at the driver call.
Not found yet: where the AE snaps to those steps (the `1e6` constants at `0x801e69c8/7efc/811c` are
frame-rate maths, not it). Removing the snapping would allow a smooth auto mode. The other route is
0dan0's own histogram AE from A/B/C: his hook point is still in the base (`calc_histogram` at
`0x8033B700`, called via `0x8033B7F4`), but videodoctor's base stubs it to a counter.

Constant patches (no code):

| site | change | why |
|---|---|---|
| `0x802f1c60` | `lw a0,0(a0)` → `move a0,zero` | FilmGrain off |
| `0x802b9d0c` | `beq s0,v0` → `b` | 3D noise reduction always on |
| `0x8006a2c4` | packed epoch constant 2000-01-01 → 1980-01-01 | the RTC can hold dates back to 1980 (FAT's own floor) |
| `0x80ddc718/1c`, `0x802bfeb0` | *(only `-Resolution native`)* 1600×1200 → 1728×1296, drop the geometry clamp | full film gate |
| `0x80333948` | boot splash JPEG replaced (not with `-NoSplash`) | build number, date and features at power-on |

**Boot splash.** Stock code shows a plain 864×480 JPEG at `0x80333948` (originally 20,385 bytes,
"Type D - WIP") before any of the mod runs. Both size getters (`0x80125cf0`/`0x80125d10`) return
`0x5000`, so a replacement must fit in 20,480 bytes; the build asserts that, and that the original is
in place. The bytes after it are free up to `0x8033A08C`. `osd_build.py` erases the old text, draws
the info block with Courier Prime (`tools/fonts/`), and saves at the best JPEG quality that fits, as
**baseline 4:2:2**: the format of the original, and the only one the boot-time decoder has been
shown to handle (0dan0's splash tool forces 4:2:2 too). Progressive or 4:2:0 output is untested here.

## Rules for new hooks

1. **Never use `$k0`/`$k1` (`$26`/`$27`)** in hand-written asm. The exception handler clobbers them on
   any interrupt, at any instruction. Jump back with a direct `j 0x…`, which needs no register. For a
   computed target, use `$t9` or another `t` register. `a0–a3` hold the hooked function's arguments,
   so they aren't free. The build decodes every spliced instruction and **fails** if either register
   appears. This rule exists because a lui/ori/jr $27 trampoline caused an intermittent hard freeze
   (screen frozen, motor running, no file) until FW 265.
2. **A `j`/`jal` hook's delay slot still executes.** Either NOP it in the image and redo the displaced
   instruction in the stub, or, where it is harmless, let it run and redo its effect.
3. **Stay off the boot and motor-init path.** Hooks at the entry of timing-critical functions freeze the
   unit at boot. Every hook here is in capture, encode or UI code that only runs once the unit is up.
4. **Every hook must earn its place.** The per-frame paths (1, 4, 6, 7) are hot. Keep them tiny, and
   prefer reading state over writing hardware registers.
5. Functions differ between Type C and D by a **non-constant** offset. To find a known C function in
   D, search for a byte fingerprint of its body, then check the match with `mipsel-linux-gnu-objdump`.
   Shared data constants are often one-word patch points (as with the RTC epoch).

## RAM map (what the mod reads and writes)

| address | meaning |
|---|---|
| `0xA0E8B7D0` | button bitmask: Up 0x1, Down 0x2, Left 0x4, Right 0x8, Back 0x20, − 0x100, + 0x200, OK 0x800 |
| `0xA0E8B7DC` | button hold counter (ramps to 0x40, 0 on release) |
| `0x80F9075C/60/64` | live WB gains R/G/B (+ a shadow copy at `…68/6C/70`) |
| `0x80F82594` | auto-exposure time, µs. Bare backlight ~118, grey leader ~204, footage ~340–1000 |
| `0x80F82598` | sensor gain |
| `0x80E974DC` | → encoder context `ctx` (valid pointer while a recording is set up) |
| `ctx+0xD4` | encoded frame counter (advances only while really recording) |
| `ctx+0xD8` | size of the last encoded frame, bytes |
| `ctx+0x94` | current Qp, as computed by the rate control |
| `ctx+0x91..93` | per-frame-type Qp caps (upper bounds, not seeds) |
| `0x80E0A624` | movie state byte: 10 = recording, 8 = preview |
| `0x80E08DF0` | handle of display pool O1. `resolve()` gives the buffer the OSD draws into |
| `0x80DDC718` | encode mode table (idx 0 = 1600×1200) |
| `0x80069EA0` | `setdate(y, m, d, 0)` |
| `0x801203A0` | clean recording stop: motor stop + file finalise (used by both auto-stops) |

**NVM settings** struct at `0x80E0ADA4`. Stock uses words 0–4. The mod uses free words that read 0 on
a fresh unit. So every value has a safe "unset" meaning, and each one is range-checked on load.

| word | address | setting |
|---|---|---|
| 5, 6, 7 | `0x80E0ADB8/BC/C0` | WB R, G, B (0,0,0 = not set → stock AWB) |
| 8, 9, 10 | `0x80E0ADC4/C8/CC` | date Y, M, D |
| 11 | `0x80E0ADD0` | fps (16/18) |
| 12 | `0x80E0ADD4` | *retired* (old Qp floor, values 16–26). Deliberately not reused |
| 13 | `0x80E0ADD8` | reel-end auto-stop **disable** flag (0 = ON, so a fresh unit defaults ON) |
| 14 | `0x80E0ADDC` | bitrate multiplier. Strict allow-list 1/2/4, so a stale 3 can never load |
| 15 | `0x80E0ADE0` | Qp floor (0 = off, 15–22) |
| 16 | `0x80E0ADE4` | manual exposure, µs (20–33000; 0 or out of range = Auto) |
| 17 | `0x80E0ADE8` | gain held in manual, ×100 (100–1600, else 100) |

**Scratch** at `0x80345000`: `ST[]` (stall state), `MENU[]` at `+0x20` (cursor, live values,
loaded-once flags). Single words: the fps value at `+0x100`, reel-end detector at `+0x300`, Qp/size
trackers at `+0x310…+0x334`, Qp floor `+0x338`, bitrate `+0x33C`, OSD sentinel `+0x340`, erase
diagnostics `+0x344…+0x354`. The exact layout is commented in `osdmenu.c`.

## Encoder and rate control

- Per encoded frame, the rate control computes **`Qp = min(targetQp, max(cap, 10))`**. `targetQp` is a
  hard-coded 40 (array at `0x80DF5C6C`). The caps are `ctx+0x91..93`. The `10` is the only lower
  bound, and that is what hook 4 replaces.
- **Bitrate** is set once at record start: `*(s0+0x50) = *(s1+0x18) << 3`. The stock `sll 3` is x1.
  Shifting by 4 or 5 gives x2 and x4. The stub computes `base × m` with adds only.
- At x4 the rate control naturally runs at about Qp 15. **A floor above ~16 pins it there for the whole
  recording.** It then can never reach its target, the encoder saturates, and recording stalls for
  seconds at a time. That is why the floor is off by default.
- **Very large frames at x4 in manual exposure.** One field report (FW 279, floor off, gain 1×): a single
  extremely grainy, bright shot stalls the recording at the same spot on the film every time, while long
  reels go through fine. Tested on that spot:

  | bitrate | exposure | result |
  |---|---|---|
  | x2 | manual | OK |
  | x4 | auto | OK |
  | x4 | manual (466 and 932 µs) | stalls, 3 out of 3 |

  At the stall the OSD shows Qp at 10 (current and lowest) and the largest frame at 496–746 KB, where
  about 250 KB is normal at x4 / 16 fps. At x4 the budget lets the rate control go all the way down to
  the hard floor of 10. On this content the frames at Qp 10 get too big for the encoder. At x2 Qp stays
  higher, and in auto the AE darkens the bright shot, so the frames stay smaller. The card is not the
  limit: at ~2 scanned frames per second even 746 KB frames are only ~1.5 MB/s.
  Workaround: x2, or auto exposure, for that reel. Open: whether QpF 15 avoids it at x4 (untested since
  the FW 256 and FW 279 fixes), and a proper fix that caps frame size without pinning Qp.

## OSD drawing

- The OSD is drawn into the display pool that `resolve(*(0x80E08DF0))` returns, at pool offset 0. The
  buffer is **column-major**, 480 rows high. A pixel (x, y) with x in 0…863 is at byte
  `(863 − x) × 480 + y`. One byte = one palette index.
- Text uses the 7×12 font in `font_data.h`. Everything sits in the right-hand black border
  (x ≈ 764…864), so nothing covers the picture.
- **Flicker fix.** The stock UI periodically repaints the layer we draw on, and that wipes the OSD.
  There are two separate sources, and both must be handled:
  1. the **compose blit** (`0x8010c164`, ~every 2 s, the blinking REC dot). Hook 6 runs the blit, then
     repaints straight away;
  2. a wipe **inside the stock OnTimer** (~once a minute). `after_ontimer` repaints straight after it.

- The menu is a **passive button reader**. The mod can't stop the stock UI reacting to buttons, so it
  only acts while recording, when the stock UI ignores the direction and +/− buttons. The same
  approach as 0dan0's A/B/C builds.

## Stall guard and reel end

Both run in `after_ontimer`, at ~2–3 Hz. That rate was measured: ~1 tick per encoded frame, and the
worst normal gap is 2 ticks.

- **Stall:** if the frame counter hasn't moved for 20 ticks (~8 s), show `REC STALLED`. At 30 ticks
  (~12 s), call the clean stop `0x801203A0`, which saves the file. A soft stall means the card is slow
  but alive, so the finalise succeeds once no more frames are fed in.
- **Reel end:** armed only after real footage (exposure ≥ 320 µs), so the start leader can't trigger
  it. Then 20 consecutive encoded frames with exposure < 150 µs (bare backlight) → clean stop.
  White footage dips to ~158 µs at most briefly.
- **Reel end in manual exposure:** off. The detector reads the AE result, which in manual no longer
  follows the picture: hook 8 keeps it frozen at the value from when manual was entered. The blank run is held at 0, the menu shows
  `ASt: off (M)`, and after switching back to Auto detection waits 40 encoded frames (~20 s) for the AE
  to settle. A manual-mode detector would need a picture-brightness signal (the AE's scene metric at
  `0x80F823AC` is a candidate) and measurements on real footage.
