"""splashgen.py - the card programs' start-up screens as JPEG bit pieces, assembled at boot with the firmware's code.

Every screen: heading ("Type probe") / title / firmware name (or a note) / "Code:" / SSSSSSSS-ZZZZZZ (S = 32-bit word sum of the unpacked
firmware, Z = its size, hex). One Huffman table set for all screens (optimised over all of them and all digits), so
the JPEG header and the digit pieces are shared; each screen adds only its static items.
An item is a run of MCUs coded without its first MCU's three DC codes: the probe codes those at boot from the
actual predictors (jpegbits.item). So digits can sit in plain 2-MCU (32 px) cells, next to each other.
"""
import io, os, sys
sys.dont_write_bytecode = True
import jpegbits as J
from PIL import Image, ImageDraw, ImageFont

FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fonts', 'CourierPrime-Regular.ttf')
BG, INK = (249, 185, 0), (30, 20, 0)
Q = 18
SLOT_MX = [4, 6, 8, 10, 12, 14, 16, 18, 22, 24, 26, 28, 30, 32]    # 8 + 6 hex digits; MCU column of each 2-MCU cell
DASH_MX = 20
SLOT_MY = 40                                                        # first MCU row (y = 320)
ROWS = 6
CELL = 2
HEX = '0123456789ABCDEF'


def font(s):
    return ImageFont.truetype(FONT, s)


def screen_image(title, label, heading='Type probe', note=''):
    pic = Image.new('RGB', (J.W, J.H), BG)
    d = ImageDraw.Draw(pic)
    d.text((60, 40), heading, font=font(34), fill=INK)
    if label:                                          # known firmware: big type + firmware name
        d.text((60, 82), title, font=font(110), fill=INK)
        d.text((60, 208), label, font=font(30), fill=INK)
    else:                                              # unknown firmware (+ an optional note under it)
        d.text((60, 120), title, font=font(52), fill=INK)
        if note:
            d.text((60, 208), note, font=font(30), fill=INK)
    d.text((60, 262), 'Code:', font=font(30), fill=INK)
    d.text((DASH_MX * 16 + 4, SLOT_MY * 8 + 4), '-', font=font(40), fill=INK)
    return pic


def put_digit(pic, mx, ch):
    ImageDraw.Draw(pic).text((mx * 16 + 4, SLOT_MY * 8 + 4), ch, font=font(40), fill=INK)


def cells_of(mx, r):
    return [(mx + k, SLOT_MY + r) for k in range(CELL)]


def build(screens, q=None, heading='Type probe', note=''):
    """screens: list of (title, label); screen 0 is the unknown one (label ''), the only one that shows the note.
    Returns sg = (hdr, progs, items, pieces, dc_codes, huffman_tables)."""
    q = q or Q
    imgs = [J.Image422(screen_image(t, l, heading, note), q) for t, l in screens]
    digit_imgs = []
    for ch in HEX:                                     # digits rendered in slot 0 of screen 0
        p = screen_image(*screens[0], heading=heading, note=note); put_digit(p, SLOT_MX[0], ch)
        digit_imgs.append(J.Image422(p, q))
    slot_at = {(mx, SLOT_MY + r): (si, r) for si, mx in enumerate(SLOT_MX) for r in range(ROWS)}
    mine = {c for mx in SLOT_MX[:1] for r in range(ROWS) for c in cells_of(mx, r)}
    for di in digit_imgs:                              # a digit changes nothing outside its own cell
        for c, m in di.mcu.items():
            if c not in mine:
                assert m == imgs[0].mcu[c], 'digit spills out of its cell at %s' % (c,)
    for im in imgs:                                    # no screen text inside the code cells
        for mx in SLOT_MX:
            for r in range(ROWS):
                for c in cells_of(mx, r):
                    assert im.is_flat(*c), 'screen text inside a code cell at %s' % (c,)

    order = [(mx, my) for my in range(J.NY) for mx in range(J.NX)]
    stats = {'dcl': {}, 'dcc': {}, 'acl': {}, 'acc': {}}
    for im in imgs:
        J.collect(im, order, (0, 0, 0), stats)
    for di in digit_imgs:
        for r in range(ROWS):
            J.collect(di, cells_of(SLOT_MX[0], r), (0, 0, 0), stats)
    for t in ('dcl', 'dcc'):                           # every DC category may occur at boot
        for c in range(12):
            stats[t][c] = stats[t].get(c, 0) + 1
    tables = {k: J.optimal_table(v) for k, v in stats.items()}
    J.use_tables(tables)

    items, progs = [], []
    for im in imgs:
        prog, run, i = [], [], 0
        while i < len(order):
            cell = order[i]
            if cell in slot_at:
                if run:
                    items.append(J.item(im, run)); prog.append(('S', len(items) - 1)); run = []
                si, r = slot_at[cell]
                prog.append(('D', si, r))
                i += CELL
            else:
                run.append(cell); i += 1
        if run:
            items.append(J.item(im, run)); prog.append(('S', len(items) - 1))
        progs.append(prog)
    pieces = [[J.item(di, cells_of(SLOT_MX[0], r)) for r in range(ROWS)] for di in digit_imgs]
    dct = {t: [J.HC[t][c] for c in range(12)] for t in ('dcl', 'dcc')}
    return J.header(q), progs, items, pieces, dct, tables


def compose(sg, screen, code):
    """Python reference of the probe's runtime assembly (code = 14 hex chars)."""
    hdr, progs, items, pieces, dct, tables = sg
    J.use_tables(tables)                               # the tables of this screen set, not whatever ran last
    s, pred = '', (0, 0, 0)
    for it in progs[screen]:
        x = items[it[1]] if it[0] == 'S' else pieces[HEX.index(code[it[1]])][it[2]]
        b, pred = J.item_bits(x, pred)
        s += b
    return hdr + J.finish(s)


def write_c(path, sg):
    hdr, progs, items, pieces, dct, tables = sg
    nl = chr(10)
    blob = []                                          # all segment bits, concatenated (bit offsets)

    def seg(bits):
        off = len(blob)
        blob.extend(bits)
        return off, len(bits)
    rows = []
    for x in items + [p for d in pieces for p in d]:   # items first, then pieces[d][r] in order
        dy, dcb, dcr, s1, s2, s3, ex = x
        rows.append((dy, dcb, dcr) + ex + seg(s1) + seg(s2) + seg(s3))
    bits = ''.join(blob) + '0' * (-len(blob) % 8)
    data = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
    prog, scr = [], []
    for p in progs:
        scr.append((len(prog), len(p)))
        for it in p:
            prog.append(it[1] if it[0] == 'S' else 0x8000 | (it[1] << 4) | it[2])
    with open(path, 'w') as f:
        f.write('/* generated by splashgen.py: start-up screens as JPEG bit pieces */' + nl)
        f.write('#define SG_HDR %d%s#define SG_ROWS %d%s#define SG_NSCR %d%s#define SG_NITEM %d%s'
                % (len(hdr), nl, ROWS, nl, len(progs), nl, len(items), nl))
        f.write('static const u8 sg_hdr[SG_HDR] = {%s};' % ','.join(str(x) for x in hdr) + nl)
        f.write('static const u8 sg_data[%d] = {%s};' % (len(data), ','.join(str(x) for x in data)) + nl)
        f.write('/* dcY dcCb dcCr exitY exitCb exitCr s1off s1len s2off s2len s3off s3len */' + nl)
        f.write('static const int sg_item[%d][12] = {%s};' % (len(rows), ','.join('{%s}' % ','.join(map(str, r)) for r in rows)) + nl)
        f.write('static const unsigned short sg_prog[%d] = {%s};' % (len(prog), ','.join(map(str, prog))) + nl)
        f.write('static const unsigned short sg_scr[SG_NSCR][2] = {%s};' % ','.join('{%d,%d}' % x for x in scr) + nl)
        for t in ('dcl', 'dcc'):
            f.write('static const u32 sg_%s[12][2] = {%s};' % (t, ','.join('{%d,%d}' % x for x in dct[t])) + nl)


def check(sg, screens, outdir):
    """Decode every screen with a sample code; save them; return the worst-case sizes."""
    for i in range(len(screens)):
        jpg = compose(sg, i, 'CD836910E08DAC')
        im = Image.open(io.BytesIO(jpg)); im.load()
        assert im.size == (J.W, J.H)
        open(os.path.join(outdir, 'screen%02d.jpg' % i), 'wb').write(jpg)
    worst = []
    for i in range(len(screens)):                      # widest digit everywhere is a safe upper bound in practice
        worst.append(max(len(compose(sg, i, c * 14)) for c in HEX))
    return worst
