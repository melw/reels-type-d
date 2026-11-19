"""jpegbits.py - minimal baseline JPEG encoder (YCbCr 4:2:2, 864x480) that exposes the entropy-coded bits of every
MCU, so a splash can be assembled at boot from precomputed pieces (static runs + per-digit glyph rows).

Layout rules that make pieces position-independent: a "slot" (one character cell, one MCU row high) must start
right after a pure-background MCU and end with a pure-background MCU. Then the DC predictors entering and leaving
every slot are the background's, so a digit's bits are the same in every slot.
"""
import io
import numpy as np

W, H = 864, 480
MW, MH = 16, 8                       # MCU: 16x8 (Y 2x1, Cb 1x1, Cr 1x1)
NX, NY = W // MW, H // MH            # 54 x 60

ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
      21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60,
      61, 54, 47, 55, 62, 63]
QL = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,
      51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121,
      120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99,
      99, 99, 99, 99] + [99] * 32
# Annex K standard Huffman tables: (bits[1..16], values)
DC_L = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_C = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_L = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], bytes.fromhex(
    '01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738'
    '393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5'
    'a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa'))
AC_C = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], bytes.fromhex(
    '000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738'
    '393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4'
    'a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa'))


def huff_codes(spec):
    bits, vals = spec
    codes, code, k = {}, 0, 0
    for ln in range(1, 17):
        for _ in range(bits[ln - 1]):
            codes[vals[k]] = (code, ln)
            code += 1
            k += 1
        code <<= 1
    return codes


HC = {'dcl': huff_codes(DC_L), 'dcc': huff_codes(DC_C), 'acl': huff_codes(AC_L), 'acc': huff_codes(AC_C)}


def qtab(base, quality):
    s = 5000 // quality if quality < 50 else 200 - 2 * quality
    return [min(255, max(1, (b * s + 50) // 100)) for b in base]


_C = np.array([[np.sqrt((1 if k == 0 else 2) / 8) * np.cos((2 * n + 1) * k * np.pi / 16) for n in range(8)] for k in range(8)])


def dct8(b):
    return _C @ b @ _C.T


def cat(v):
    v = abs(int(v))
    return 0 if v == 0 else v.bit_length()


def vbits(v):
    c = cat(v)
    return (v if v >= 0 else v + (1 << c) - 1), c


class Bits:
    def __init__(self):
        self.b = []                      # list of (value, nbits)

    def put(self, v, n):
        if n:
            self.b.append((v, n))

    def as_bitstring(self):
        return ''.join(format(v, '0%db' % n) for v, n in self.b)


def block_coeffs(px, q):
    c = dct8(px.astype(float) - 128.0)
    return [int(round(c.flat[ZZ[i]] / q[ZZ[i]])) for i in range(64)]


def encode_block(out, coeffs, pred, dc_t, ac_t):
    dc = coeffs[0]
    d = dc - pred
    v, c = vbits(d)
    code, ln = HC[dc_t][c]
    out.put(code, ln); out.put(v, c)
    run = 0
    last = max([i for i in range(1, 64) if coeffs[i] != 0], default=0)
    for i in range(1, last + 1):
        a = coeffs[i]
        if a == 0:
            run += 1
            continue
        while run > 15:
            code, ln = HC[ac_t][0xf0]; out.put(code, ln); run -= 16
        v, c = vbits(a)
        code, ln = HC[ac_t][(run << 4) | c]
        out.put(code, ln); out.put(v, c)
        run = 0
    if last < 63:
        code, ln = HC[ac_t][0x00]; out.put(code, ln)
    return dc


class Image422:
    """Holds quantised coefficients of every MCU; encodes any MCU range given incoming DC predictors."""

    def __init__(self, rgb, quality):
        a = np.asarray(rgb.convert('RGB'), dtype=float)
        r, g, b = a[..., 0], a[..., 1], a[..., 2]
        y = 0.299 * r + 0.587 * g + 0.114 * b
        cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128
        cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128
        cb2 = (cb[:, 0::2] + cb[:, 1::2]) / 2          # 4:2:2: halve horizontally
        cr2 = (cr[:, 0::2] + cr[:, 1::2]) / 2
        self.ql, self.qc = qtab(QL, quality), qtab(QC, quality)
        self.mcu = {}
        for my in range(NY):
            for mx in range(NX):
                y0, x0 = my * 8, mx * 16
                yb0 = block_coeffs(np.clip(y[y0:y0 + 8, x0:x0 + 8], 0, 255), self.ql)
                yb1 = block_coeffs(np.clip(y[y0:y0 + 8, x0 + 8:x0 + 16], 0, 255), self.ql)
                cbb = block_coeffs(np.clip(cb2[y0:y0 + 8, mx * 8:mx * 8 + 8], 0, 255), self.qc)
                crb = block_coeffs(np.clip(cr2[y0:y0 + 8, mx * 8:mx * 8 + 8], 0, 255), self.qc)
                self.mcu[(mx, my)] = (yb0, yb1, cbb, crb)

    def dcs(self, mx, my):
        m = self.mcu[(mx, my)]
        return m[1][0], m[2][0], m[3][0]               # predictors after this MCU: Y (last Y block), Cb, Cr

    def is_flat(self, mx, my):
        m = self.mcu[(mx, my)]
        return all(all(c == 0 for c in blk[1:]) for blk in m) and m[0][0] == m[1][0]

    def encode(self, cells, pred):
        """Bits of the given MCU list, starting from pred = (Y, Cb, Cr); returns (Bits, pred_after)."""
        out = Bits()
        py, pb, pr = pred
        for cell in cells:
            yb0, yb1, cbb, crb = self.mcu[cell]
            py = encode_block(out, yb0, py, 'dcl', 'acl')
            py = encode_block(out, yb1, py, 'dcl', 'acl')
            pb = encode_block(out, cbb, pb, 'dcc', 'acc')
            pr = encode_block(out, crb, pr, 'dcc', 'acc')
        return out, (py, pb, pr)


def header(quality):
    ql, qc = qtab(QL, quality), qtab(QC, quality)
    h = bytearray(b'\xff\xd8')
    h += b'\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00'
    for tid, q in ((0, ql), (1, qc)):
        h += b'\xff\xdb\x00\x43' + bytes([tid]) + bytes(q[ZZ[i]] for i in range(64))
    h += b'\xff\xc0\x00\x11\x08' + H.to_bytes(2, 'big') + W.to_bytes(2, 'big') + b'\x03\x01\x21\x00\x02\x11\x01\x03\x11\x01'
    for cls_id, (bits, vals) in ((0x00, DC_L), (0x10, AC_L), (0x01, DC_C), (0x11, AC_C)):
        body = bytes([cls_id]) + bytes(bits) + bytes(vals)
        h += b'\xff\xc4' + (len(body) + 2).to_bytes(2, 'big') + body
    h += b'\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00'
    return bytes(h)


def finish(bitstring):
    """Pad with 1-bits, byte-stuff, append EOI (what the probe does at boot)."""
    bitstring += '1' * (-len(bitstring) % 8)
    out = bytearray()
    for i in range(0, len(bitstring), 8):
        v = int(bitstring[i:i + 8], 2)
        out.append(v)
        if v == 0xff:
            out.append(0)
    return bytes(out) + b'\xff\xd9'


def optimal_table(freq):
    """libjpeg's jpeg_gen_optimal_table: Huffman code lengths limited to 16 bits -> (bits[16], values)."""
    freq = dict(freq)
    f = [0] * 257
    for s, c in freq.items():
        f[s] = c
    f[256] = 1                                          # reserved code point: no all-ones code
    codesize = [0] * 257
    others = [-1] * 257
    while True:
        c1 = -1; v = 1 << 60
        for i in range(257):
            if f[i] and f[i] <= v:
                v = f[i]; c1 = i
        c2 = -1; v = 1 << 60
        for i in range(257):
            if f[i] and f[i] <= v and i != c1:
                v = f[i]; c2 = i
        if c2 < 0:
            break
        f[c1] += f[c2]; f[c2] = 0
        codesize[c1] += 1
        while others[c1] >= 0:
            c1 = others[c1]; codesize[c1] += 1
        others[c1] = c2
        codesize[c2] += 1
        while others[c2] >= 0:
            c2 = others[c2]; codesize[c2] += 1
    bits = [0] * 33
    for i in range(257):
        if codesize[i]:
            bits[codesize[i]] += 1
    for i in range(32, 16, -1):
        while bits[i] > 0:
            j = i - 2
            while bits[j] == 0:
                j -= 1
            bits[i] -= 2; bits[i - 1] += 1; bits[j + 1] += 2; bits[j] -= 1
    i = 16
    while bits[i] == 0:
        i -= 1
    bits[i] -= 1                                        # remove the reserved code point
    vals = [s for ln in range(1, 33) for s in range(256) if codesize[s] == ln]
    return bits[1:17], vals


def collect(im, cells, pred, stats):
    """Symbol statistics of encoding cells (same walk as encode)."""
    py, pb, pr = pred
    for cell in cells:
        for k, blk in enumerate(im.mcu[cell]):
            dct, act = ('dcl', 'acl') if k < 2 else ('dcc', 'acc')
            p = py if k < 2 else (pb if k == 2 else pr)
            stats[dct][cat(blk[0] - p)] = stats[dct].get(cat(blk[0] - p), 0) + 1
            if k < 2:
                py = blk[0]
            elif k == 2:
                pb = blk[0]
            else:
                pr = blk[0]
            last = max([i for i in range(1, 64) if blk[i] != 0], default=0)
            run = 0
            for i in range(1, last + 1):
                if blk[i] == 0:
                    run += 1; continue
                while run > 15:
                    stats[act][0xf0] = stats[act].get(0xf0, 0) + 1; run -= 16
                s = (run << 4) | cat(blk[i])
                stats[act][s] = stats[act].get(s, 0) + 1; run = 0
            if last < 63:
                stats[act][0] = stats[act].get(0, 0) + 1
    return (py, pb, pr)


def use_tables(tables):
    """Switch the encoder (and header) to the given {'dcl','dcc','acl','acc'} -> (bits, values) tables."""
    global DC_L, DC_C, AC_L, AC_C
    DC_L, DC_C, AC_L, AC_C = tables['dcl'], tables['dcc'], tables['acl'], tables['acc']
    for k in tables:
        HC[k] = huff_codes(tables[k])


def ac_bits(out, coeffs, ac_t):
    """AC part of one block (everything after the DC code)."""
    run = 0
    last = max([i for i in range(1, 64) if coeffs[i] != 0], default=0)
    for i in range(1, last + 1):
        a = coeffs[i]
        if a == 0:
            run += 1
            continue
        while run > 15:
            code, ln = HC[ac_t][0xf0]; out.put(code, ln); run -= 16
        v, c = vbits(a)
        code, ln = HC[ac_t][(run << 4) | c]
        out.put(code, ln); out.put(v, c)
        run = 0
    if last < 63:
        code, ln = HC[ac_t][0x00]; out.put(code, ln)


def item(im, cells):
    """A run of MCUs whose bits do not depend on what comes before it: the first MCU's three DC codes are left
    out (the probe codes them at boot from the actual predictors). Returns
    (dcY, dcCb, dcCr, s1, s2, s3, exit_pred): s1 = Y0 AC + Y1; s2 = Cb AC; s3 = Cr AC + the remaining MCUs."""
    yb0, yb1, cbb, crb = im.mcu[cells[0]]
    s1, s2, s3 = Bits(), Bits(), Bits()
    ac_bits(s1, yb0, 'acl')
    encode_block(s1, yb1, yb0[0], 'dcl', 'acl')
    ac_bits(s2, cbb, 'acc')
    ac_bits(s3, crb, 'acc')
    rest, exit_pred = im.encode(cells[1:], (yb1[0], cbb[0], crb[0]))
    s3.b += rest.b
    return (yb0[0], cbb[0], crb[0], s1.as_bitstring(), s2.as_bitstring(), s3.as_bitstring(), exit_pred)


def item_bits(it, pred):
    """Python reference of the probe's runtime: the item's bits after a block with DC predictors pred."""
    dy, dcb, dcr, s1, s2, s3, ex = it

    def dc(diff, t):
        v, c = vbits(diff)
        code, ln = HC[t][c]
        return format(code, '0%db' % ln) + (format(v, '0%db' % c) if c else '')
    return dc(dy - pred[0], 'dcl') + s1 + dc(dcb - pred[1], 'dcc') + s2 + dc(dcr - pred[2], 'dcc') + s3, ex
