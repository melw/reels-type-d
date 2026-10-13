# User guide

## Flashing

You need a **FAT32** microSD card of **32 GB or less**. The bootloader can't read exFAT, which is what
Windows gives larger cards by default.

1. Copy `FWDV280.BIN` to the card root.
2. Create an **empty folder** named `NVTDELFW` on the card root. It tells the bootloader to delete the
   file after flashing. Without it, the unit reflashes on every boot.
3. **Safe-eject** the card. Pulling it without ejecting can leave the file incomplete, and the
   bootloader then silently skips it.
4. With the Reels **powered off**, insert the card and power on.
5. The power LED lights and the screen stays black for about 30 s while it flashes. **Do not cut the
   power.** It then boots into the new firmware.
6. Check the build number: from FW 271 on, the start-up screen shows it ("Type D - FW 271"), and it is
   always in the top right of the screen, e.g. `FW 271`. If the start-up screen still shows an older
   number, or "Type D - WIP", the new file wasn't flashed.

If the old build still boots, look at the card. If `FWDV280.BIN` is still there, the flash never ran.
See [BUILDING.md: It didn't flash](BUILDING.md#it-didnt-flash).

**Going back:** flash an older build, or videodoctor's original `FWDV280.BIN`, the same way. The mod
never touches the boot path, so a bad build can always be reflashed from the card.

Your settings normally survive reflashing, because they live in the unit's settings memory, not in
the firmware.

## The screen

Everything is drawn in the black border to the right of the picture, so nothing covers the film. Colour
only groups the rows. Every value has a text label.

**In preview:**
```
Edit in REC      reminder: settings can only be changed while recording
WB gains:
 689,256,204     white balance gains (red, green, blue)
Res:1600x1200    capture resolution
Gain:...         sensor gain
Exp:...us        exposure time
Date:
 1984-09-01      date that will be written into recordings
```

**While recording**, the same list becomes the settings menu:
```
Frm:1234         frames encoded so far
WB gains:
[689],256,204    [ ] = the value you are editing
FPS: 18          frame rate written into the file
BR: x4           bitrate multiplier
ASt: ON          reel-end auto-stop
Qp:22/16 f-      encoder Qp now / lowest this recording, then the Qp floor setting (- = off)
Sz:180/420K      encoded frame size now / largest this recording
Cum:312M         total encoded so far
Gain:...
Exp:...us A
Date:
 1984-09-01
Reset
```

`Qp`, `Sz` and `Cum` stay on screen after a recording ends. The `[ ]` brackets and the `Reset` row
disappear, and `Edit in REC` returns at the top. If anything ever goes wrong, a photo of the screen
with the `FW` number is the most useful thing you can post.

## The menu

The menu **only works while recording.** In preview the stock firmware owns the buttons, so the mod
can't use them there. The easy way: start a short throwaway recording, set everything up, stop, then
capture for real. **Settings are saved** and survive a power cycle.

| button | action |
|---|---|
| **Up / Down** | move between settings (wraps around) |
| **Left / Right** | big step: white balance in 16s, year in 10s |
| **Minus / Plus** | fine step, one at a time |
| OK, Back | unchanged: stop recording / back |

The `[ ]` brackets mark the value being edited.

| setting | values | default | notes |
|---|---|---|---|
| **WB** R, G, B | 16–999 each | stock auto WB | Applied live, so you see the result straight away. The first press takes the current gains as its starting point, so nothing jumps (or 689,256,218 if the current gains look invalid) |
| **FPS** | 18 / 16 | 18 | Playback rate written into the file. Takes effect on the **next** recording |
| **BR** | x1 / x2 / x4 | x4 | x1 ≈ 8 Mb/s (stock), x2 ≈ 16, x4 ≈ 32. Takes effect on the next recording. |
| **ASt** | ON / OFF | ON | Reel-end auto-stop. Turn it OFF if very faded or thin film gets stopped early |
| **QpF** (the `f` value on the Qp row) | - (off), 15–22 | off | Advanced: leave it off. See below |
| **Date** | 1980-01-01 … 2100-12-31 | 2026-01-01 | Date stamp for new recordings. |
| **Reset** | press any adjust button | | Everything back to the defaults above, and WB back to stock auto |

**Qp floor (QpF).** Experimental. This sets a minimum for the encoder's quantiser. Off means stock
behaviour. At x4 the encoder wants to go down to about Qp 15. Can cause stalls (`REC STALLED`) on x4 BR.
It is safer at x1/x2.

## Automatic stops

Both stops halt the film motor and **close the file properly**, so the footage up to that point is
saved.

**Reel end** (`ASt: ON`). Once real footage has been seen, the mod watches the auto exposure. When it
sees the bare white backlight for 20 frames in a row (about 10 s), i.e. the film has run out, it
shows:
```
BLANK FRAMES        (with a live count)
STOPPING REC
```
then stops, and shows `REEL ENDED / AUTO-STOPPED / FILE SAVED`. The grey leader at the start of a
reel can't trigger it.

**Recording stall** (always on). If no new frame gets encoded for about 8 s:
```
! REC STALLED
NO NEW FRAMES
STOP REC NOW
```
After about 12 s it stops by itself and shows `REC STALLED / AUTO-STOPPED / FILE SAVED`. Common causes:
a slow or failing card, or a Qp floor set too high at x4.

## Tips

- **SD card:** use a good-quality FAT32 card (V10/U1 or better).
- **Long reels:** FAT32 can't store files over 4 GB. At x4 that is roughly 18,000 frames, about
  17 minutes of playback at 18 fps. Runs past ~3.8 GB haven't been tested. For longer reels,
  consider x2 or split the recording in half.
- **Sharpness** is optical: clean the lens and the film gate. The firmware can't sharpen a blurry
  image.
- **Resolution:** the standard build is 1600×1200, cropped to the picture. A `native` build
  (1728×1296) shows the whole film gate, sprocket holes included, with slightly more grain and detail.
  See [BUILDING.md](BUILDING.md).

## Troubleshooting

| symptom | what to do |
|---|---|
| Flashed, but the old build still boots | See [It didn't flash](BUILDING.md#it-didnt-flash) |
| Buttons do nothing in preview | Expected. The menu only works while recording (`Edit in REC` at the top of the list) |
| Very dark picture, `WB gains` near 000,000,000 | Start a recording and use `Reset` (FW 272+ ignores invalid saved WB values) |
| Reels of faded or thin film stop early with `REEL ENDED` | Set `ASt: OFF` |
| `REC STALLED` | Try another card. Check `QpF` is off. |
