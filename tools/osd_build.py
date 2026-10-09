#!/usr/bin/env python3
"""Splice the compiled mod (build/osd.elf) into the Type D base firmware and apply the patch set.

Places the ELF's .text/.rodata at their link addresses (VMA = 0x80000000 + file offset), applies the
constant patches (quality, RTC epoch, optional resolution), then installs the hooks -- each hook is only
applied if the ELF defines the matching symbol, so the source decides what gets hooked.

Inputs  (relative to the repo root; override with env vars):
    build/osd.elf                 KODAK_ELF    compiled + linked mod (produced by build.ps1)
    base/vd-FWDV280-D.rbn         KODAK_BASE   videodoctor's Type D base -- see base/README.md
Output:
    build/vd-osd.rbn              KODAK_OUT    patched, unpacked image (build.ps1 then checksums + packs it)
Options:
    KODAK_BR       x1 | x2 | x4   static bitrate, ONLY used if the bitrate hook is absent (NOHOOKS)
    KODAK_RES      std | native   std = 1600x1200 (default), native = stock 1728x1296 incl. sprocket holes
    KODAK_NOHOOKS  1              diagnostic: constant patches only, none of the mod's code runs
    KODAK_ANYBASE  1              skip the base SHA256 check (for porting to another base -- expect asserts)
    KODAK_BUILD    <N>            build number for the boot splash ("Type D - FW <N>"); set by build.ps1
    KODAK_SPLASH   0              keep the original "Type D - WIP" splash (default 1 = write the build number)

Every patch asserts the ORIGINAL instruction first, so a wrong base fails loudly at build time instead of
producing firmware that hangs at boot.
"""
import hashlib, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF  = os.environ.get("KODAK_ELF",  os.path.join(ROOT, "build", "osd.elf"))
BASE = os.environ.get("KODAK_BASE", os.path.join(ROOT, "base", "vd-FWDV280-D.rbn"))
OUT  = os.environ.get("KODAK_OUT",  os.path.join(ROOT, "build", "vd-osd.rbn"))

BASE_SHA256 = "90d2abb68b1232b4c759c774105922888945a9250183c79f1213b37b0bb199c6"
SCRATCH_BASE = 0x80345000   # runtime scratch (menu state, counters) -- injected code must end below this


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def parse(path):
    d = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<I", d, 0x20)
    shent, shnum, shstr = struct.unpack_from("<HHH", d, 0x2e)
    secs = []
    for i in range(shnum):
        nm, typ, fl, addr, off, sz, lk, inf, al, ent = struct.unpack_from("<IIIIIIIIII", d, e_shoff + i * shent)
        secs.append(dict(nm=nm, typ=typ, addr=addr, off=off, sz=sz, lk=lk, ent=ent))
    shs = secs[shstr]
    def name(s):
        e = d.find(b"\0", shs["off"] + s["nm"]); return d[shs["off"] + s["nm"]:e].decode()
    byname = {name(s): s for s in secs}
    placed = []
    for nm in (".text", ".rodata", ".data"):
        s = byname.get(nm)
        if s and s["sz"] > 0:
            placed.append((nm, s["addr"], d[s["off"]:s["off"] + s["sz"]]))
    symt = next(s for s in secs if s["typ"] == 2); strt = secs[symt["lk"]]
    syms = {}
    for i in range(symt["sz"] // symt["ent"]):
        st_name, st_val, st_sz, info, oth, shndx = struct.unpack_from("<IIIBBH", d, symt["off"] + i * symt["ent"])
        e = d.find(b"\0", strt["off"] + st_name)
        syms[d[strt["off"] + st_name:e].decode()] = st_val
    return placed, syms


def word(img, off):
    return struct.unpack_from("<I", img, off)[0]


def patch(img, off, expect, new, what):
    cur = word(img, off)
    if cur != expect:
        die(f"{what} @0x{0x80000000 + off:08x}: expected 0x{expect:08x}, found 0x{cur:08x} -- wrong base?")
    struct.pack_into("<I", img, off, new)


def jal(target): return 0x0c000000 | ((target >> 2) & 0x03FFFFFF)
def j(target):   return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


# ---- inputs ------------------------------------------------------------------------------------
if not os.path.exists(ELF):
    die(f"{ELF} not found -- run build.ps1 (it compiles first)")
if not os.path.exists(BASE):
    die(f"base firmware not found at {BASE} -- see base/README.md")
sha = hashlib.sha256(open(BASE, "rb").read()).hexdigest()
if sha != BASE_SHA256 and os.environ.get("KODAK_ANYBASE") != "1":
    die(f"base SHA256 mismatch:\n  expected {BASE_SHA256}\n  found    {sha}\n"
        "Every address in this mod is specific to that exact base -- see base/README.md.")
print(f"base: {os.path.basename(BASE)} (sha256 ok)")

placed, syms = parse(ELF)
img = bytearray(open(BASE, "rb").read())

# ---- constant patches --------------------------------------------------------------------------
# FilmGrain off + 3DNR forced on (these two are what make the picture clean at high bitrate)
patch(img, 0x2f1c60, 0x8c840000, 0x00002021, "FilmGrain")   # lw a0,0(a0) -> move a0,zero
patch(img, 0x2b9d0c, 0x1202001b, 0x1000001b, "3DNR")        # beq s0,v0   -> b (always)
print("quality: FilmGrain off, 3DNR on")

# RTC epoch 2000-01-01 -> 1980-01-01. One shared packed constant used by both the RTC write converter
# and the read-back, so moving it moves the whole epoch. 1980 is FAT's own floor anyway.
patch(img, 0x6a2c4, 0x000fa021, 0x000f7821, "RTC epoch")
print("RTC epoch: 1980-01-01")

# Resolution. std keeps the base's 1600x1200 (cropped to the image, no sprocket holes); native restores
# the sensor-native 1728x1296 of stock D (the full film gate, holes included, marginally more detail).
res = os.environ.get("KODAK_RES", "std")
if res == "native":
    patch(img, 0xddc718, 0x00000640, 0x000006c0, "width")        # 1600 -> 1728
    patch(img, 0xddc71c, 0x000004b0, 0x00000510, "height")       # 1200 -> 1296
    patch(img, 0x2bfeb0, 0x0c0cebb1, 0x24050030, "geometry")     # drop the 1600 clamp hook
print(f"resolution: {res}")

# Boot splash: "Type D - WIP" -> "Type D - FW <N>". The splash is a plain 864x480 JPEG that stock code shows
# before any of the mod runs, so it proves WHICH file was flashed even if the mod itself never starts.
# Both splash size getters (0x80125cf0/0x80125d10) return 0x5000, so the new JPEG must fit in 20480 bytes;
# the bytes after the old JPEG are free up to 0x33a08c. Needs Pillow; KODAK_SPLASH=0 keeps the original.
SPLASH_OFF, SPLASH_LEN, SPLASH_MAX = 0x333948, 20385, 0x5000
SPLASH_SHA256 = "60cdc9b2ad876beb343d7d2f84e1d2f5ad5f06cd5665945e760c99bc3e4b3af7"
if os.environ.get("KODAK_SPLASH", "1") == "1":
    build_no = os.environ.get("KODAK_BUILD")
    if not build_no:
        die("KODAK_BUILD not set (build.ps1 sets it from -Build)")
    try:
        import io
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        die("the boot splash needs Pillow: pip install pillow  (or build with -NoSplash)")
    old = bytes(img[SPLASH_OFF:SPLASH_OFF + SPLASH_LEN])
    if hashlib.sha256(old).hexdigest() != SPLASH_SHA256:
        die(f"splash @0x{0x80000000 + SPLASH_OFF:08x} is not the expected 'Type D - WIP' JPEG -- wrong base?")
    for off in (0x125cf4, 0x125d14):
        patch(img, off, 0x24025000, 0x24025000, "splash size getter")    # li v0,0x5000 (assert only)
    pic = Image.open(io.BytesIO(old)).convert("RGB")
    draw = ImageDraw.Draw(pic)
    draw.rectangle([425, 170, 659, 213], fill=(249, 185, 0))             # erase "Type D - WIP"
    font = ImageFont.load_default(size=34)                               # Pillow's bundled font: same on every machine
    draw.text((431, 172), f"Type D - FW {build_no}", font=font, fill=(255, 255, 255),
              stroke_width=1, stroke_fill=(255, 255, 255))               # stroke = bold, like the original
    jpg = None
    for q in range(95, 20, -1):                                          # best quality that fits the slot
        # subsampling=1 (4:2:2) and baseline: the same format as the original splash -- the boot-time
        # JPEG decoder has only ever been shown this one (0dan0's splash tool forces 4:2:2 too)
        buf = io.BytesIO(); pic.save(buf, format="JPEG", quality=q, optimize=True, subsampling=1)
        if buf.tell() <= SPLASH_MAX:
            jpg = buf.getvalue(); break
    if jpg is None:
        die("could not fit the splash JPEG into 20480 bytes")
    img[SPLASH_OFF:SPLASH_OFF + SPLASH_MAX] = bytes(SPLASH_MAX)           # clear the slot, then write
    img[SPLASH_OFF:SPLASH_OFF + len(jpg)] = jpg
    print(f"splash: 'Type D - FW {build_no}' ({len(jpg)} bytes, JPEG quality {q})")
else:
    print("splash: original ('Type D - WIP')")

# ---- splice the mod ----------------------------------------------------------------------------
for nm, addr, body in placed:
    off = addr & 0xFFFFFF
    if any(img[off:off + len(body)]):
        die(f"{nm} placement 0x{off:06x} is not free in the base")
    img[off:off + len(body)] = body
    print(f"placed {nm}: {len(body)} bytes @ 0x{addr:08x}")
code_end = max(addr + len(body) for nm, addr, body in placed)
if code_end > SCRATCH_BASE:
    die(f"code+rodata end 0x{code_end:08x} overran the scratch base 0x{SCRATCH_BASE:08x} -- move scratch up")
print(f"code+rodata end 0x{code_end:08x} ({SCRATCH_BASE - code_end} B headroom below scratch)")

# ---- SAFETY GATE: no $k0/$k1 in anything we splice ---------------------------------------------
# $26/$27 are reserved for the exception handler and are clobbered by ANY interrupt. A stub that holds a
# jump target in one (`lui/ori/jr $27`) jumps to a garbage address if an interrupt lands in that window:
# an instant hard hang.
def k_regs(w):
    op = w >> 26
    if op in (2, 3):                                   # j / jal: bits 0..25 are an address, not registers
        return ()
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    return (rs, rt, rd) if op == 0 else (rs, rt)
bad = [(addr + i, word(body, i)) for nm, addr, body in placed if nm == ".text"
       for i in range(0, len(body) - 3, 4) if any(r in (26, 27) for r in k_regs(word(body, i)))]
if bad:
    die("$k0/$k1 used in spliced code (an interrupt will clobber it):\n" +
        "\n".join(f"    0x{a:08x}: 0x{w:08x}" for a, w in bad[:10]))
print(f"k0/k1 audit: clean ({sum(len(b) // 4 for n, _, b in placed if n == '.text')} instructions)")

# ---- hooks -------------------------------------------------------------------------------------
if os.environ.get("KODAK_NOHOOKS") == "1":
    # Diagnostic: nothing of ours ever runs. This is how the freeze was proven to be in our code.
    syms = {}
    print("*** NO-HOOKS MODE: mod spliced but never called -- expect no OSD on the device ***")

# 1. Per-frame entry: the white-balance call inside the capture pipeline -> osd_main (draws the OSD,
#    runs the menu, applies WB, then tail-calls the original WB function).
if "osd_main" in syms:
    patch(img, 0x2b7c90, jal(0x802b7b7c), jal(syms["osd_main"]), "WB call")
    print("hook: WB call -> osd_main")

# 2. Frame rate: replace `li a3,0xd2f0; sw a3,0(a1)` with `jal fps_stub; nop` (FPS menu, 16/18).
if "fps_stub" in syms:
    patch(img, 0x1f20c8, 0x3407d2f0, jal(syms["fps_stub"]), "fps")
    struct.pack_into("<I", img, 0x1f20cc, 0)          # old store -> nop (the stub does it)
    print("hook: frame rate -> fps_stub")

# 3. Bitrate: record-init computes *(s0+0x50) = *(s1+0x18) << 3. `j bitrate_stub` makes the multiplier
#    menu-selectable (x1/x2/x4). The j's delay slot (the original store) still runs; the stub redoes it.
if "bitrate_stub" in syms:
    patch(img, 0x240588, 0x000318c0, j(syms["bitrate_stub"]), "bitrate")
    print("hook: bitrate -> bitrate_stub (menu x1/x2/x4)")
else:
    br = {"x1": 0x000318c0, "x2": 0x00031900, "x4": 0x00031940}[os.environ.get("KODAK_BR", "x4")]
    patch(img, 0x240588, 0x000318c0, br, "bitrate")
    print(f"bitrate: static 0x{br:08x}")

# 4. Rate-control minimum Qp: the per-frame RC computes min(targetQp, max(cap, 10)); the 10 is the only
#    lower bound. `j qpfloor_stub` rebuilds that clamp from the QpF menu value (off = stock 10).
if "qpfloor_stub" in syms:
    patch(img, 0x241074, 0x2a85000a, j(syms["qpfloor_stub"]), "Qp clamp")   # slti a1,s4,10
    patch(img, 0x241078, 0x2403000a, 0, "Qp clamp")                          # li v1,10 -> nop (delay slot)
    print("hook: RC Qp clamp -> qpfloor_stub")

# 5. Movie-window OnTimer: entry detour -> my_ontimer (runs the stock handler, then after_ontimer:
#    stall detector, reel-end auto-stop, warnings, and the OnTimer-side OSD repaint).
if "my_ontimer" in syms:
    patch(img, 0x11e344, 0x27bdff10, j(syms["my_ontimer"]), "OnTimer")      # addiu sp,sp,-0xf0
    patch(img, 0x11e348, 0xafbe00e8, 0, "OnTimer")                           # delay slot -> nop
    print("hook: OnTimer -> my_ontimer")

# 6. UI compose blit (the call inside OSD_Compose 0x8010bc38): run the real blit, then repaint our OSD if
#    the blit just wiped it -- this is what fixes the record-mode flicker.
if "wrap_compose" in syms:
    patch(img, 0x10bc70, jal(0x8010c164), jal(syms["wrap_compose"]), "compose blit")
    print("hook: compose blit -> wrap_compose")

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(img)
print(f"wrote {OUT}")
