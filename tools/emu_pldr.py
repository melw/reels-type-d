"""emu_pldr.py - test a card program (patch-loader or type probe) in a MIPS emulator against a Type D flash dump.

  python tools/emu_pldr.py <FW96658T.BIN> <flash_dump.bin> halt=<addr> sg=<splashgen.pkl>
                           unknown=<packed.BIN>=<splash offset> own=<name,...> NAME=<packed.BIN>=<expected.rbn> ...

The emulated RAM holds the real bootloader body (unpacked from the dump to 0x87f05000) and the real stage-1 helpers
(0x8ff04000), so the bootloader check runs against real code and the firmware is unpacked by the bootloader's real
decompressor. Flash open/read/close are emulated from the dump. The SPI controller model accepts only READ (0x03)
and fails if the controller enable bit is cleared; the 1 MHz timer counts only while enabled (as the hardware).
Tests (each fails on a flash-write call, a halt, a timeout or any difference in RAM):
  NAME cases      that packed file in the firmware area -> RAM == expected (+ handoff block, normal-boot flags),
                  timer untouched
  unknown         the unknown= file, altered -> its code on the "Unknown firmware" screen at that offset, no pause
  unknown, no splash          -> timer on/off, a pause, then started unchanged
  own= cases      as if another bootloader: own READ (0x03 only, 2 DMA), own unpack + check, no bootloader call
  own, corrupted flash data   -> stopped, firmware NOT started
"""
import os, pickle, struct, sys
sys.dont_write_bytecode = True   # no __pycache__ in the repo
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UcError
from unicorn.mips_const import UC_CPU_MIPS32_74KF, UC_MIPS_REG_CP0_STATUS, UC_MIPS_REG_PC, UC_MIPS_REG_RA, \
    UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3, UC_MIPS_REG_V0, UC_MIPS_REG_SP
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bcl_lz import lz_decompress   # noqa: E402
import pldr_common as C           # noqa: E402

FW_OFF, FW_ENTRY = 0x4000, 0x80000400
WRITE = 0x87f07ad4                 # bootloader flash write: must never be called
HANDOFF = bytes(range(0x80, 0xa8))  # recognisable fake handoff block
T_FLAGS = 0x5                      # card boot (4) + bit 0, to check that only bit 4 is cleared
MUTATE_AT = 0x200000               # "unknown" firmware = a known one with this byte altered


def p(va):
    return va & 0x1fffffff         # kseg0/kseg1 -> physical


def with_fw(dump, packed):
    """The flash dump with its firmware area replaced by a packed FWDV280.BIN (as if that were installed)."""
    assert packed[:4] == b'BCL1' and FW_OFF + len(packed) <= len(dump)
    return dump[:FW_OFF] + packed + bytes([0xff]) * (len(dump) - FW_OFF - len(packed))


def run(stub, dump, body, mutate=None, corrupt_loader=False, limit=3_000_000_000, halt_addr=None, kill_splash=False):
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
    uc.ctl_set_cpu_model(UC_CPU_MIPS32_74KF)            # 74K with DSP ASE: the bootloader's decompressor uses lbux
    uc.reg_write(UC_MIPS_REG_CP0_STATUS, uc.reg_read(UC_MIPS_REG_CP0_STATUS) | (1 << 24))   # Status.MX (DSP on)
    uc.mem_map(0, 0x10000000)                           # 256 MB covers RAM (128 MB) and the 0x8ff04000 helpers
    uc.mem_map(0x10040000, 0x1000)                      # timer block (0xb0040108)
    uc.mem_map(0x10230000, 0x1000)                      # SPI flash controller (0xb0230000)
    uc.mem_map(0x10020000, 0x1000)                      # clock enables (0xb0020064)
    uc.mem_map(0x10010000, 0x1000)                      # pin mux (0xb0010004)
    uc.mem_write(p(0x80000000), stub)
    ho = bytearray(HANDOFF)
    struct.pack_into('<I', ho, 0x94 - 0x80, T_FLAGS)
    uc.mem_write(p(0x80000080), bytes(ho))
    uc.mem_write(p(0x8ff04000), dump[0x6cc:0x92c])     # real stage-1 helpers, copied like stage 1 does
    b = bytearray(body)
    if corrupt_loader:
        struct.pack_into('<I', b, 0x87f082c8 - 0x87f05000, 0x12345678)
    uc.mem_write(p(0x87f05000), bytes(b))               # real bootloader body
    st = {'done': False, 'open': False, 'calls': [], 'timer': 0, 'treads': 0, 'en': 0, 'en_writes': [],
          'cmds': [], 'dma': [], 'addr': None, 'halted': False, 'pause': mutate is not None}

    def ret(v=0):
        uc.reg_write(UC_MIPS_REG_V0, v & 0xffffffff)
        uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))

    def code(uc, addr, size, _):
        try:
            a0, a1, a2, a3 = (uc.reg_read(r) for r in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3))
            if addr == 0x87f08840:
                st['open'] = True; st['calls'].append('open'); ret(0)
            elif addr == 0x87f082c8:
                assert st['open'] and a3 == 2
                uc.mem_write(p(a2), dump[a0 * 4096:a0 * 4096 + a1]); st['calls'].append('read'); ret(0)
            elif addr == 0x87f078d4:
                st['open'] = False; st['calls'].append('close'); ret(0)
            elif addr == 0x8ff0415c and mutate:          # python decompressor + simulated other firmware
                out = bytearray(lz_decompress(bytes(uc.mem_read(p(a0), a2)), mutate))
                out[MUTATE_AT] ^= 0xff
                if kill_splash:                            # no 864x480 JPEG anywhere near the usual place
                    for o in range(0x300000, 0x380000):
                        if out[o:o + 3] == b'\xff\xd8\xff':
                            out[o + 1] = 0
                uc.mem_write(p(a1), bytes(out)); st['calls'].append('unlz-mut'); ret(0)
            elif addr == 0x8ff0415c:
                st['calls'].append('unlz')                 # the real code runs
            elif addr in (0x8ff04110, 0x8ff040b4):
                st['calls'].append('cache'); ret(0)
            elif addr == WRITE:
                st['err'] = 'FLASH WRITE CALLED'; uc.emu_stop()
            elif addr == FW_ENTRY:
                st['done'] = True; uc.emu_stop()
            elif addr == halt_addr:
                st['halted'] = True; uc.emu_stop()
        except BaseException as e:                         # never let an exception escape into unicorn
            st['err'] = repr(e); uc.emu_stop()

    def timer(uc, access, addr, size, value, _):
        if st['en']:                                       # like the hardware: counts only while enabled
            st['timer'] += 3_000_000 if st['pause'] else 10   # pause: 3 s per read; own path: 10 us per read
        st['treads'] += 1
        uc.mem_write(addr & ~3, struct.pack('<I', st['timer'] & 0xffffffff))

    def spi_w(uc, access, addr, size, value, _):        # SPI controller model: only READ (0x03) is allowed
        try:
            o = addr - 0x10230000
            if o == 0 and not (value & 1):
                st['err'] = 'SPI controller disabled (bit 0 of +0x00 cleared)'; uc.emu_stop(); return
            if o == 0x10:
                cmd = value & 0xff
                st['cmds'].append(cmd)
                if cmd != 0x03:
                    st['err'] = 'SPI command %#x sent (only 0x03 READ allowed)' % cmd; uc.emu_stop(); return
                st['addr'] = ((value >> 8) & 0xff) << 16 | ((value >> 16) & 0xff) << 8 | ((value >> 24) & 0xff)
            elif o == 0x1c and value == 1:
                n = struct.unpack('<I', uc.mem_read(0x10230020, 4))[0]
                dst = struct.unpack('<I', uc.mem_read(0x10230024, 4))[0]
                if st['addr'] is None:
                    st['err'] = 'DMA without a READ command'; uc.emu_stop(); return
                uc.mem_write(p(dst) if dst >= 0x80000000 else dst, dump[st['addr']:st['addr'] + n])
                st['dma'].append((st['addr'], n)); st['addr'] = None
        except BaseException as e:
            st['err'] = repr(e); uc.emu_stop()

    def spi_r(uc, access, addr, size, value, _):
        uc.mem_write(0x10230004, struct.pack('<I', 0x43))   # tx ready, rx full, DMA done
    uc.mem_write(0x10230000, struct.pack('<I', 0x8001))  # as the bootloader leaves it: enabled, chip select released
    uc.hook_add(UC_HOOK_MEM_WRITE, spi_w, begin=0x10230000, end=0x102300ff)
    uc.hook_add(UC_HOOK_MEM_READ, spi_r, begin=0x10230004, end=0x10230007)

    for a in (0x87f08840, 0x87f082c8, 0x87f078d4, 0x8ff0415c, 0x8ff04110, 0x8ff040b4, WRITE, FW_ENTRY) + ((halt_addr,) if halt_addr else ()):
        uc.hook_add(UC_HOOK_CODE, code, begin=a, end=a)
    uc.hook_add(UC_HOOK_MEM_READ, timer, begin=0x10040108, end=0x1004010b)

    def timer_en(uc, access, addr, size, value, _):
        st['en'] = value & 1
        st['en_writes'].append(value)
    uc.hook_add(UC_HOOK_MEM_WRITE, timer_en, begin=0x10040100, end=0x10040103)
    uc.reg_write(UC_MIPS_REG_SP, 0x87f08000)
    uc.reg_write(UC_MIPS_REG_RA, 0x87f0066c)
    try:
        uc.emu_start(struct.unpack_from('<I', stub, 0)[0], FW_ENTRY, count=limit)
    except UcError as e:
        raise SystemExit('FAIL: emulator %s at %#x calls=%s' % (e, uc.reg_read(UC_MIPS_REG_PC), st['calls']))
    if st.get('err'):
        raise SystemExit('FAIL: %s calls=%s' % (st['err'], st['calls']))
    st['done'] = st['done'] or uc.reg_read(UC_MIPS_REG_PC) == FW_ENTRY
    return st, uc


def expect(img):
    """What RAM must hold at the firmware entry: img + the handoff block of a normal flash boot."""
    e = bytearray(img)
    e[0x80:0xa8] = HANDOFF
    struct.pack_into('<I', e, 0x94, (T_FLAGS & ~4) | 8)
    struct.pack_into('<I', e, 0x98, struct.unpack_from('<I', e, 0x68)[0])
    return bytes(e)


def same(label, uc, want):
    got = bytes(uc.mem_read(p(0x80000000), len(want)))
    if got != want:
        d = [i for i in range(0, len(got), 4) if got[i:i + 4] != want[i:i + 4]]
        raise SystemExit('FAIL %s: %d words differ, first %s' % (label, len(d), [hex(x) for x in d[:6]]))


def main():
    stub = open(sys.argv[1], 'rb').read()
    dump = open(sys.argv[2], 'rb').read()
    opt = dict(x.split('=', 1) for x in sys.argv[3:] if x.split('=', 1)[0] in ('halt', 'sg', 'unknown', 'own'))
    cases = [x.split('=') for x in sys.argv[3:] if x.split('=', 1)[0] not in opt]
    halt_addr = int(opt['halt'], 16)
    sg = pickle.load(open(opt['sg'], 'rb'))
    usz, csz = struct.unpack('>II', dump[0x92c + 8:0x92c + 16])
    body = lz_decompress(dump[0x92c + 16:0x92c + 16 + csz], usz)
    n = 0

    for name, packed_path, exp_path in cases:            # known firmware
        st, uc = run(stub, with_fw(dump, open(packed_path, 'rb').read()), body)
        if not st['done']:
            raise SystemExit('FAIL %s: did not start the firmware, calls=%s' % (name, st['calls']))
        same(name, uc, expect(open(exp_path, 'rb').read()))
        if st['treads'] or st['en_writes']:
            raise SystemExit('FAIL %s: paused or touched the timer although known' % name)
        print('emu OK  %-7s -> expected image, firmware started' % name); n += 1

    upath, uoff = opt['unknown'].rsplit('=', 1)           # unknown firmware -> its code on its own splash, no pause
    packed = open(upath, 'rb').read()
    base = bytearray(C.unpack(packed)); base[MUTATE_AT] ^= 0xff
    st, uc = run(stub, with_fw(dump, packed), body, mutate=len(base))
    if not st['done'] or st['treads']:
        raise SystemExit('FAIL unknown: done=%s timer reads=%d' % (st['done'], st['treads']))
    want = C.expected_screen(sg, bytes(base), 0, int(uoff, 16))
    same('unknown', uc, expect(want))
    print('emu OK  unknown -> code %s on its splash, not patched, no pause, firmware started' % C.code_of(bytes(base))); n += 1

    st, uc = run(stub, with_fw(dump, packed), body, mutate=len(base), kill_splash=True)   # ...and no splash: the pause
    if not st['done'] or st['treads'] < 2 or st['en_writes'] != [1, 0]:
        raise SystemExit('FAIL unknown/no splash: done=%s timer reads=%d enable writes=%s' % (st['done'], st['treads'], st['en_writes']))
    print('emu OK  unknown without a splash -> timer on/off, paused (%d timer reads), firmware started' % st['treads']); n += 1

    own = [c for c in cases if c[0] in opt['own'].split(',')]   # another bootloader -> own SPI read + unpack + check
    if not own:
        raise SystemExit('FAIL: no own-path case among %s' % [c[0] for c in cases])
    for name, packed_path, exp_path in own:
        st, uc = run(stub, with_fw(dump, open(packed_path, 'rb').read()), body, corrupt_loader=True, halt_addr=halt_addr)
        if not st['done'] or st['calls']:
            raise SystemExit('FAIL own %s: done=%s halted=%s bootloader calls=%s' % (name, st['done'], st['halted'], st['calls']))
        if set(st['cmds']) != {3} or len(st['dma']) != 2:
            raise SystemExit('FAIL own %s: commands %s dma %s' % (name, st['cmds'], st['dma']))
        same('own ' + name, uc, expect(open(exp_path, 'rb').read()))
        print('emu OK  other bootloader, %-7s -> own READ (0x03 only, %d DMA), own unpack + check, started' % (name, len(st['dma']))); n += 1

    bad = bytearray(open(own[0][1], 'rb').read())        # another bootloader + bad flash data -> must stop
    bad[len(bad) // 2] ^= 0x01
    st, uc = run(stub, with_fw(dump, bytes(bad)), body, corrupt_loader=True, halt_addr=halt_addr)
    if st['done'] or not st['halted']:
        raise SystemExit('FAIL own corrupt: done=%s halted=%s' % (st['done'], st['halted']))
    print('emu OK  other bootloader, corrupted flash data -> stopped, firmware NOT started'); n += 1
    print('emu: all %d tests passed (no flash write)' % n)


if __name__ == '__main__':
    main()
