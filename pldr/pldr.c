/* Card programs for the Kodak Reels (NT96658 SoC): the patch-loader (-DPATCH) and the type probe (default).
 * Both run as FW96658T.BIN from the SD card: the bootloader runs that file from RAM and never writes it to flash.
 * Neither contains Kodak code. Shared core:
 *   1. bootloader check (reads only): is it the Type D bootloader? (sig.h: first words of every routine used + id)
 *   2. read + unpack the installed firmware from flash (0x4000):
 *        Type D bootloader: its own routines (open, READ, close, decompress)
 *        any other:         our own SPI READ (0x03 only, DMA) + our own unpacker, then the size word and the
 *                           bootloader's image checksum must match; any failure: stop (black screen)
 *   3. fingerprint it (size + 32-bit word sum) against the known firmware (bases.h, from pldr/bases.txt)
 *        known, patch-loader: apply that base's patch runs (target words from a shared pool) -> the target build
 *        known, probe:        "TYPE X" + its name on the start-up screen, at its known splash address
 *        unknown, both:       "Unknown firmware" on the first referenced 864x480 JPEG we can find; boot it unpatched
 *      Every screen shows "Code: <sum>-<size>", assembled at boot from precomputed JPEG bit pieces (splashgen.h)
 *      and written only if it fits the old picture; unknown and nowhere to write: a ~PAUSE_S s pause instead
 *   4. hand over as a normal flash boot (flags 4 -> 8), flush the caches, start the firmware
 * Bootloader routines (resident, from the 2026-11-15 flash dump analysis):
 *   0x87f08840 flash open, 0x87f082c8 flash READ (never 0x87f07ad4 = write), 0x87f078d4 flash close,
 *   0x8ff0415c BCL LZ decompress, 0x8ff04110 D-cache write-back, 0x8ff040b4 I-cache invalidate.
 */
typedef unsigned int u32;
typedef unsigned char u8;
#include "sig.h"
#include "bases.h"
#include "splashgen.h"

#define FW        ((volatile u32 *)0x80000000)
#define BUF       ((u8 *)0x86000000)            /* compressed firmware (<= 4 MB) */
#define BUFU      ((u8 *)0xa6000000)            /* the same, uncached: our own path reads the DMA'd bytes here */
#define SGBUF     ((u8 *)0x86800000)            /* the assembled screen */
#define HANDOFF   ((volatile u32 *)0x87000080)  /* our copy of the handoff block the bootloader wrote */
#define FW_BLOCK  4                             /* 4 KB units: flash 0x4000 */
#define REG(a)    (*(volatile u32 *)(a))
#define SPI(o)    REG(0xb0230000 + (o))
#define TIMER_US  REG(0xb0040108)               /* 1 MHz counter (firmware delay: ms * 1000) */
#define TIMER_EN  REG(0xb0040100)               /* stage 1 writes 1; the bootloader writes 0 before the handoff */
#define SPIN      0x10000000u                   /* bound for every wait loop */

#define fl_open   ((int (*)(void))0x87f08840)
#define fl_read   ((int (*)(u32 blk, u32 len, void *dst, u32 mode))0x87f082c8)   /* never 0x87f07ad4 = write */
#define fl_close  ((void (*)(void))0x87f078d4)
#define unlz      ((void (*)(const void *src, void *dst, u32 len))0x8ff0415c)
#define dflush    ((void (*)(void))0x8ff04110)
#define iinval    ((void (*)(void))0x8ff040b4)

static u32 sg_acc __attribute__((section(".data"))), sg_n __attribute__((section(".data"))),
           sg_len __attribute__((section(".data")));    /* in the image: there is no .bss */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

__attribute__((noinline, noreturn)) static void halt(void)   /* own function: the emulator watches it */
{
    for (;;)
        ;
}

static void cache_all(void)                    /* as stage 1: D write-back+invalidate, I invalidate, 16 KB */
{
    u32 a;
    for (a = 0x80000000; a < 0x80004000; a += 32)
        __asm__ volatile("cache 1, 0(%0)" : : "r"(a));
    __asm__ volatile("sync");
    for (a = 0x80000000; a < 0x80004000; a += 32)
        __asm__ volatile("cache 0, 0(%0)" : : "r"(a));
    __asm__ volatile("sync; ehb");
}

/* ---- our own flash path: the SoC SPI controller as the Type D bootloader drives it ------------------------ */

static int wait_st(u32 bit, u32 us)
{
    u32 n, t0 = TIMER_US;
    for (n = 0; n < SPIN; n++) {
        if (SPI(4) & bit)
            return 0;
        if (TIMER_US - t0 > us)
            return -1;
    }
    return -1;
}

static void spi_on(void)                       /* as the bootloader's flash open: clock + pins (+ our timer) */
{
    TIMER_EN = 1;
    REG(0xb0020064) |= 0x40;
    REG(0xb0010004) |= 0x2002;
}

static void spi_off(void)                      /* as its flash close */
{
    REG(0xb0020064) &= ~0x40u;
    REG(0xb0010004) &= ~0x2002u;
    TIMER_EN = 0;                              /* hand over the state the bootloader left */
}

static int own_read(u32 addr, u32 len)         /* plain READ (0x03), 1 lane, data by DMA to BUF */
{
    int r = 0;
    SPI(0) &= ~0x8000u;                        /* chip select */
    SPI(0) &= ~0x100u;                         /* bootloader: and -0x101 = ~0x100 (bit 0 = enable stays set!) */
    SPI(0x18) = (SPI(0x18) & ~0x30000u) | 3;   /* single lane, 32-bit words */
    if (wait_st(1, 50000) < 0)
        r = -1;
    else {
        SPI(0x10) = 0x03 | ((addr >> 16) & 0xff) << 8 | ((addr >> 8) & 0xff) << 16 | (addr & 0xff) << 24;
        if (wait_st(2, 50000) < 0)
            r = -1;
        else {
            (void)SPI(0xc);
            SPI(4) = 0x1ff;
            SPI(0x20) = (len + 15) & ~15u;
            SPI(0x24) = (u32)BUF;              /* the cached address, as the bootloader (device-proven) */
            SPI(0) &= ~0x200000u;              /* direction: flash -> RAM */
            SPI(0x1c) = 1;
            if (wait_st(0x40, 10000000) < 0)
                r = -1;
            SPI(4) = 0x1ff;
        }
    }
    SPI(0) |= 0x8000u;                         /* release chip select */
    SPI(0x18) = (SPI(0x18) & ~0x70000u) | 0x40000u;   /* as the bootloader leaves it */
    return r;
}

static u32 own_unlz(const u8 *src, u32 in_size, u8 *dst, u32 out_size)   /* BCL LZ77 (algorithm 9) */
{
    u32 i = 1, o = 0, len, off, k;
    u8 marker = src[0], c;
    while (i < in_size && o < out_size) {
        c = src[i++];
        if (c != marker) {
            dst[o++] = c;
        } else if (src[i] == 0) {
            dst[o++] = marker;
            i++;
        } else {
            len = 0;
            do { c = src[i++]; len = (len << 7) | (c & 0x7f); } while (c & 0x80);
            off = 0;
            do { c = src[i++]; off = (off << 7) | (c & 0x7f); } while (c & 0x80);
            if (off == 0 || off > o)
                return 0;                      /* corrupt stream */
            for (k = 0; k < len && o < out_size; k++, o++)
                dst[o] = dst[o - off];
        }
    }
    return o;
}

static int image_ok(u32 size)                  /* the bootloader's image check (0x87f05228) */
{
    u32 n = size >> 1, i, sum = 0;
    const volatile unsigned short *h = (const volatile unsigned short *)FW;
    if ((size & 1) || FW[0x68 >> 2] != size)
        return 0;
    for (i = 0; i < n; i++)
        sum += h[i];
    sum += (n - 1) * n / 2;
    return (sum & 0xffff) == 0;
}

static int own_load(u32 *usz_out)              /* 0 = loaded and verified */
{
    u32 usz, csz;
    spi_on();
    cache_all();                               /* no dirty line may be written back over the DMA data */
    if (own_read(FW_BLOCK << 12, 0x1000) < 0 || be32(BUFU) != 0x42434c31) {
        spi_off();
        return -1;
    }
    usz = be32(BUFU + 8);
    csz = be32(BUFU + 12);
    if (usz < 0x1000 || usz > 0x05000000 || csz > 0x003fc000 - 16) {
        spi_off();
        return -1;
    }
    cache_all();
    if (own_read(FW_BLOCK << 12, csz + 16) < 0) {
        spi_off();
        return -1;
    }
    spi_off();
    if (own_unlz(BUFU + 16, csz, (u8 *)FW, usz) != usz || !image_ok(usz))
        return -1;
    *usz_out = usz;
    return 0;
}

/* ---- the start-up screen ------------------------------------------------------------------------------------ */

static void sg_byte(u32 v)
{
    SGBUF[sg_len++] = (u8)v;
    if (v == 0xff)
        SGBUF[sg_len++] = 0;                   /* JPEG byte stuffing */
}

static void sg_put(u32 v, u32 n)              /* append the low n bits of v, MSB first */
{
    while (n--) {
        sg_acc = (sg_acc << 1) | ((v >> n) & 1);
        if (++sg_n == 8) {
            sg_byte(sg_acc);
            sg_acc = sg_n = 0;
        }
    }
}

static void sg_bits(u32 off, u32 n)            /* append n bits of sg_data[] starting at bit offset off */
{
    u32 i;
    for (i = off; i < off + n; i++)
        sg_put((sg_data[i >> 3] >> (7 - (i & 7))) & 1, 1);
}

static void sg_dc(const u32 (*tab)[2], int diff)   /* one DC code: category + magnitude bits */
{
    u32 a = diff < 0 ? -diff : diff, c = 0;
    while (a >> c)
        c++;
    sg_put(tab[c][0], tab[c][1]);
    if (c)
        sg_put(diff < 0 ? (u32)(diff + (1 << c) - 1) : (u32)diff, c);
}

static u32 sg_compose(u32 scr, const u8 *code) /* code = 14 hex digit values; returns the JPEG length */
{
    u32 i, it;
    int py = 0, pb = 0, pr = 0;
    const int *x;
    for (i = 0; i < SG_HDR; i++)
        SGBUF[i] = sg_hdr[i];
    sg_len = SG_HDR;
    sg_acc = sg_n = 0;
    for (i = sg_scr[scr][0]; i < sg_scr[scr][0] + sg_scr[scr][1]; i++) {
        it = sg_prog[i];
        x = (it & 0x8000) ? sg_item[SG_NITEM + code[(it >> 4) & 0xff] * SG_ROWS + (it & 0xf)] : sg_item[it];
        sg_dc(sg_dcl, x[0] - py);
        sg_bits(x[6], x[7]);
        sg_dc(sg_dcc, x[1] - pb);
        sg_bits(x[8], x[9]);
        sg_dc(sg_dcc, x[2] - pr);
        sg_bits(x[10], x[11]);
        py = x[3]; pb = x[4]; pr = x[5];
    }
    if (sg_n) {                                /* pad the last byte with 1-bits */
        while (sg_n < 8) {
            sg_acc = (sg_acc << 1) | 1;
            sg_n++;
        }
        sg_byte(sg_acc);
    }
    SGBUF[sg_len++] = 0xff;
    SGBUF[sg_len++] = 0xd9;
    return sg_len;
}

static int code_ref(u32 va, u32 usz)           /* does the code load this address (lui + addiu)? */
{
    u32 hi = (va + 0x8000) >> 16, lo = va & 0xffff, i, j, w, rt, end = (usz < 0x400000 ? usz : 0x400000) >> 2;
    for (i = 0; i + 6 < end; i++) {
        w = FW[i];
        if ((w >> 26) != 0x0f || (w & 0xffff) != hi)
            continue;
        rt = (w >> 16) & 31;
        for (j = 1; j <= 6; j++) {
            w = FW[i + j];
            if ((w >> 26) == 0x09 && ((w >> 21) & 31) == rt && (w & 0xffff) == lo)
                return 1;
        }
    }
    return 0;
}

static u32 jpeg_len(u32 o, u32 usz)            /* 864x480 baseline JPEG at o: its length, else 0 */
{
    const volatile u8 *b = (const volatile u8 *)FW;
    u32 s, e;
    if (o + 0x400 >= usz || b[o] != 0xff || b[o + 1] != 0xd8 || b[o + 2] != 0xff)
        return 0;
    for (s = o + 2; s < o + 0x400; s++)        /* SOF0 with 480 x 864 */
        if (b[s] == 0xff && b[s + 1] == 0xc0)
            break;
    if (s >= o + 0x400 || b[s + 5] != 0x01 || b[s + 6] != 0xe0 || b[s + 7] != 0x03 || b[s + 8] != 0x60)
        return 0;
    for (e = s; e < o + 0x10000 && e + 1 < usz; e++)
        if (b[e] == 0xff && b[e + 1] == 0xd9)
            return e + 2 - o;
    return 0;
}

static u32 find_splash(u32 usz, u32 *len)      /* first referenced 864x480 JPEG in 0x300000..0x380000 */
{
    u32 o, n, end = usz < 0x380000 ? usz : 0x380000;
    for (o = 0x300000; o + 0x400 < end; o++) {
        n = jpeg_len(o, usz);
        if (n && code_ref(0x80000000 + o, usz)) {
            *len = n;
            return o;
        }
    }
    return 0;
}

static int show(u32 scr, u32 off, u32 len, u32 sum, u32 usz)   /* 1 = the screen is in place */
{
    u8 code[14];
    u32 i, n;
    volatile u8 *b = (volatile u8 *)FW;
    if (!off || !len)
        return 0;
    for (i = 0; i < 8; i++)
        code[i] = (sum >> (28 - 4 * i)) & 15;
    for (i = 0; i < 6; i++)
        code[8 + i] = (usz >> (20 - 4 * i)) & 15;
    n = sg_compose(scr, code);
    if (n > len)
        return 0;
    for (i = 0; i < len; i++)                  /* the new picture, then zeros up to the old picture's end */
        b[off + i] = i < n ? SGBUF[i] : 0;
    return 1;
}

void pldr_main(void)
{
    u32 usz = 0, csz, sum, i, k, hit, t0, ldr, off, len;
#ifdef PATCH
    u32 j;
#endif

    ldr = 1;                                   /* 1. Type D bootloader? (reads only) */
    for (i = 0; i < NSIG; i++)
#ifdef BADSIG
        if (*(volatile u32 *)sig_addr[i] != (sig_val[i] ^ (i == 3)))   /* device test: as if another bootloader */
#else
        if (*(volatile u32 *)sig_addr[i] != sig_val[i])
#endif
            ldr = 0;

    if (ldr) {                                 /* 2. with the Type D bootloader's routines */
        if (fl_open() < 0)
            halt();
        if (fl_read(FW_BLOCK, 0x1000, BUF, 2) < 0)
            halt();
        if (be32(BUF) != 0x42434c31)           /* "BCL1" */
            halt();
        usz = be32(BUF + 8);
        csz = be32(BUF + 12);
        if (usz < 0x1000 || usz > 0x05000000 || csz > 0x003fc000 - 16)
            halt();
        if (fl_read(FW_BLOCK, csz + 16, BUF, 2) < 0)
            halt();
        fl_close();
        unlz(BUF + 16, (void *)FW, csz);
    } else if (own_load(&usz) < 0)             /*    or on our own, verified */
        halt();

    sum = 0;                                   /* 3. which firmware? */
    for (i = 0; i < (usz >> 2); i++)
        sum += FW[i];
    hit = NBASE;
    if (FW[0x68 >> 2] == usz)
        for (i = 0; i < NBASE; i++)
            if (usz == base_size[i] && sum == base_sum[i])
                hit = i;
    if (hit < NBASE) {
#ifdef PATCH
        for (i = base_first[hit]; i < base_first[hit] + base_nrun[hit]; i++)   /* target words from the pool */
            for (j = 0, k = run_pool[i]; j < run_len[i]; j++, k++)
                FW[(run_off[i] >> 2) + j] = pool[k];
#else
        off = base_off[hit];
        len = jpeg_len(off, usz) == base_len[hit] ? base_len[hit] : 0;   /* the picture we know must be there */
        k = show(base_scr[hit], off, len, sum, usz);
#endif
    } else {
        len = 0;
        off = find_splash(usz, &len);
        k = show(0, off, len, sum, usz);
        if (!k) {                              /* nowhere to show the code: the pause is the signal */
            TIMER_EN = 1;
            t0 = TIMER_US;
            for (i = 0; i < 0x30000000u && TIMER_US - t0 < PAUSE_S * 1000000u; i++)
                ;
            TIMER_EN = 0;
        }
    }

    for (i = 0; i < 10; i++)                   /* 4. handoff block as a normal flash boot */
        FW[0x20 + i] = HANDOFF[i];
    FW[0x94 >> 2] = (HANDOFF[(0x94 - 0x80) >> 2] & ~4u) | 8u;
    FW[0x98 >> 2] = FW[0x68 >> 2];
    if (ldr) {
        dflush();
        iinval();
    } else
        cache_all();
    ((void (*)(void))FW[0])();
    halt();
}
