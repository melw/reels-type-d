"""pldr_common.py - shared build steps of the two card programs (FW96658T.BIN): the patch-loader (make_pldr.py)
and the type probe (make_probe.py). Both compile pldr/pldr.c + pldr/entry.S, see pldr/README.md.

  bases (pldr/bases.txt: verify SHA256, unpack, fingerprint) -> start-up screens (splashgen: the highest JPEG
  quality at which every screen fits its slot) -> compile + link (WSL mipsel gcc) -> .bss / k0-k1 audit ->
  raw image (entry word, NT96658 id, size word, bootloader checksum)
"""
import hashlib, os, pickle, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True   # no __pycache__ in the repo
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from bcl_lz import lz_decompress   # noqa: E402

LOAD, RELOC = 0x80000000, 0x87000000
UNKNOWN_MAX = 14000          # the unknown screen must fit stock-sized splash slots (stock D: 14,240 bytes)
SPARE = 64                   # bytes kept free in every slot for JPEG byte stuffing


def die(msg):
    sys.exit('ERROR: ' + msg)


def run(cmd, cwd=ROOT):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        die('%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def sha(b):
    return hashlib.sha256(b).hexdigest()


def unpack(packed):
    if packed[:4] != b'BCL1':
        die('not a packed (BCL1) firmware')
    usz, csz = struct.unpack('>II', packed[8:16])
    return lz_decompress(packed[16:16 + csz], usz)


def fingerprint(img):
    """(size, 32-bit word sum) of an unpacked image: what the card programs compare at boot."""
    return len(img), sum(struct.unpack('<%dI' % (len(img) // 4), img[:len(img) // 4 * 4])) & 0xffffffff


def code_of(img):
    """The code on the screen: word sum, then the low 24 bits of the size (hex)."""
    size, sm = fingerprint(img)
    return '%08X%06X' % (sm, size & 0xffffff)


def jpeg_len(img, off):
    if img[off:off + 3] != b'\xff\xd8\xff':
        die('no JPEG at %#x' % off)
    return img.find(b'\xff\xd9', off) + 2 - off


def ld_checksum(d):
    """The bootloader's image check (0x87f05228): sum of LE halfwords + n(n-1)/2 must be 0 mod 65536."""
    n = len(d) // 2
    return (sum(struct.unpack('<%dH' % n, d[:n * 2])) + ((n - 1) * n // 2)) & 0xffff


def fix_checksum(d):
    struct.pack_into('<H', d, 0x6c, 0xaa55)
    struct.pack_into('<H', d, 0x6e, 0)
    struct.pack_into('<H', d, 0x6e, (-ld_checksum(d)) & 0xffff)
    assert ld_checksum(d) == 0


def load_bases(bld, use=None):
    """pldr/bases.txt rows (all, or only those with this use) as dicts; unpacked images cached in bld."""
    bases = []
    for line in open(os.path.join(ROOT, 'pldr', 'bases.txt'), encoding='utf-8'):
        if not line.strip() or line.startswith('#'):
            continue
        p = line.split()
        name, typ, splash, u, rel, size, digest, label = p[0], p[1], int(p[2], 16), p[3], p[4], int(p[5]), p[6], ' '.join(p[7:])
        if use and u != use:
            continue
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print('WARNING: base %s skipped: %s not found (see pldr/README.md)' % (name, rel))
            continue
        packed = open(path, 'rb').read()
        if len(packed) != size or sha(packed) != digest:
            die('base %s: %s is not the expected file (size %d, sha256 %s)' % (name, rel, len(packed), sha(packed)))
        cache = os.path.join(bld, 'base-%s-%s.rbn' % (name, digest[:12]))
        if os.path.exists(cache):
            img = open(cache, 'rb').read()
        else:
            img = unpack(packed)
            open(cache, 'wb').write(img)
        bases.append({'name': name, 'type': typ, 'splash': splash, 'label': label, 'path': path, 'img': img})
    if not bases:
        die('no base firmware found (see pldr/README.md)')
    if len({fingerprint(b['img']) for b in bases}) != len(bases):
        die('two bases share a fingerprint')
    return bases


def make_screens(bld, screens, slots, heading='Type probe', note=''):
    """The highest JPEG quality at which every screen fits its slot. Writes splashgen.h (+ .pkl for the emulator)."""
    import splashgen
    lo, hi, best = 10, 95, None
    while lo <= hi:
        q = (lo + hi) // 2
        sg_q = splashgen.build(screens, q, heading, note)
        sizes_q = splashgen.check(sg_q, screens, bld)
        if all(z <= s - SPARE for z, s in zip(sizes_q, slots)):
            best, sg, sizes = q, sg_q, sizes_q
            lo = q + 1
        else:
            hi = q - 1
    if best is None:
        die('no JPEG quality fits')
    splashgen.check(sg, screens, bld)                  # leaves screenNN.jpg of the chosen quality in bld
    splashgen.write_c(os.path.join(bld, 'splashgen.h'), sg)
    pickle.dump(sg, open(os.path.join(bld, 'splashgen.pkl'), 'wb'))
    print('screens: %d at JPEG quality %d; worst sizes %s (slots %s)' % (len(screens), best, sizes, slots))
    return sg, sizes


def expected_screen(sg, img, scr, off):
    """The unpacked image with screen scr (and img's code) in the picture at off, as the card program leaves it."""
    import splashgen
    old = jpeg_len(img, off)
    jpg = splashgen.compose(sg, scr, code_of(img))
    if len(jpg) > old:
        die('screen %d does not fit (%d > %d)' % (scr, len(jpg), old))
    out = bytearray(img)
    out[off:off + old] = jpg + bytes(old - len(jpg))
    return bytes(out)


def compile_card(bld, defines=()):
    """Compile pldr/ with the generated headers in bld. Returns (text, symbols)."""
    w = lambda p: run(['wsl.exe', 'wslpath', '-a', p.replace('\\', '/')]).strip()
    src, b = w(os.path.join(ROOT, 'pldr')), w(bld)
    cc = 'mipsel-linux-gnu-gcc -march=mips32r2 -EL -fno-pic -mno-abicalls -G0'
    run(['wsl.exe', 'bash', '-c',
         "cd '%s' && %s %s -ffreestanding -nostdlib -Os -fno-builtin -Wall -I. -I'%s' -c '%s/pldr.c' -o pldr.o && "
         "%s -c '%s/entry.S' -o entry.o && "
         "mipsel-linux-gnu-ld -EL -T '%s/pldr.ld' --no-dynamic-linker -o pldr.elf entry.o pldr.o && "
         "mipsel-linux-gnu-objcopy -O binary -j .text pldr.elf pldr.text"
         % (b, cc, ' '.join('-D' + d for d in defines), src, src, cc, src, src)])
    sym = {}
    for l in run(['wsl.exe', 'bash', '-c', "mipsel-linux-gnu-nm '%s/pldr.elf'" % b]).splitlines():
        p = l.split()
        if len(p) == 3:
            sym[p[2]] = int(p[0], 16)
    for l in run(['wsl.exe', 'bash', '-c', "mipsel-linux-gnu-objdump -h '%s/pldr.elf'" % b]).splitlines():
        p = l.split()
        if len(p) > 2 and p[1] == '.bss' and int(p[2], 16):
            die('.bss is not empty (it would not be in the image)')
    if sym.get('_entry') != RELOC + 0x100:
        die('_entry not at %#x' % (RELOC + 0x100))
    # k0/k1 audit (the exception handler clobbers them): code only, the tables follow it
    data = [v for k, v in sym.items() if k.startswith(('sg_', 'base_', 'sig_', 'run_', 'pool')) and v >= RELOC]
    dis = run(['wsl.exe', 'bash', '-c', "mipsel-linux-gnu-objdump -d --stop-address=0x%x '%s/pldr.elf'" % (min(data), b)])
    insns = [l.split('\t', 2)[-1] for l in dis.splitlines() if l.count('\t') >= 2]
    if not insns or any(('k0' in s or 'k1' in s) for s in insns):
        die('k0/k1 used in the card program code')
    return open(os.path.join(bld, 'pldr.text'), 'rb').read(), sym


def write_image(text, sym, out):
    """The raw FW96658T.BIN: header page + code. Returns the bytes."""
    img = bytearray(0x100) + text
    img += bytes(-len(img) % 4)
    if len(img) != sym['_image_size']:
        die('image size %d != linker _image_size %d' % (len(img), sym['_image_size']))
    struct.pack_into('<I', img, 0, LOAD + 0x100)                # entry word (stage 1 jumps to *(0x80000000))
    img[0x50:0x68] = b'NT96658 ' + b'10000000' + b'20100701'     # same id layout as the Kodak header
    struct.pack_into('<I', img, 0x68, len(img))                  # raw image: the bootloader checks size == file size
    fix_checksum(img)
    open(out, 'wb').write(img)
    return bytes(img)
