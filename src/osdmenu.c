/* osdmenu.c -- Kodak Reels Type D firmware mod (on-screen display, settings menu, recording guards).
 *
 * Linked at 0x8034116c and spliced into videodoctor's Type D base by tools/osd_build.py, which also
 * installs the six hooks that call into this file (see docs/TECHNICAL.md for the full map):
 *   osd_main      per captured frame (via the stock WB call): draw OSD, run the menu, apply WB/FPS/date
 *   fps_stub      frame-rate setup: 16 or 18 fps from the menu
 *   bitrate_stub  record-init: bitrate multiplier x1/x2/x4 from the menu
 *   qpfloor_stub  rate-control minimum Qp (menu "QpF", OFF = stock behaviour)
 *   my_ontimer    movie-window timer (~2-3 Hz): stall detector, REC STALLED warning, reel-end auto-stop
 *   wrap_compose  UI compose blit: repaint the OSD when the blit wipes it (fixes the record-mode flicker)
 *
 * Buttons: D button struct @ 0xA0E8b7d0 (uncached):
 *   button[0]@0xA0E8b7d0 = code bitmask: UP 0x1 DOWN 0x2 LEFT 0x4 RIGHT 0x8 BACK 0x20
 *                          NEG/- 0x100 PLUS/+ 0x200 OK 0x800
 *   button[3]@0xA0E8b7dc = hold counter (ramps to 0x40 while held; 0 on release)
 *
 * DESIGN: we are a PASSIVE reader of the button struct -- we cannot suppress the stock UI's own
 * handling. So the menu only acts DURING RECORDING, where the stock UI leaves the direction/+/- buttons
 * free (0dan0's A/B/C approach). OK (Stop) and BACK are left untouched.
 *
 * Menu: Up/Down = move between rows (the edited value is shown in [brackets]); +/- = fine adjust;
 * Left/Right = coarse adjust (WB +/-16, year +/-10). Settings persist in free NVM words (see NVM_*).
 *
 * Many comments below keep the history of how a value was found or why an approach was dropped. Build
 * numbers refer to the development log in docs/CHANGELOG.md.
 */
#include <stdint.h>
#include "font_data.h"

extern int orig(void *a0, void *a1, void *a2, void *a3);   /* 0x802b7b7c WB */
extern uint32_t resolve(uint32_t h);                       /* 0x80006500 */

#define YEL 1
#define CYN 4
#define QPC 5
#define WHT 255
#define RED 3
#define SH  480

/* D button struct (uncached) */
#define BTN_CODE 0xA0E8b7d0u
#define BTN_HOLD 0xA0E8b7dcu

/* WB gain registers (+ shadow copy) */
#define WB_R 0x80F9075Cu
#define WB_G 0x80F90760u
#define WB_B 0x80F90764u
#define WB_R2 0x80F90768u
#define WB_G2 0x80F9076Cu
#define WB_B2 0x80F90770u

/* NVM persistent settings struct 0x80E0ADA4: stock uses words 0(EV)/1(Sharp)/2(Tint)/4(flag);
 * words 5,6,7 read 0 + never change with any stock setting -> FREE. Store WB R/G/B there.
 * (0,0,0) = "not set" -> don't auto-apply (valid gains are always > 0). */
#define NVM_R 0x80E0ADB8u   /* word 5 */
#define NVM_G 0x80E0ADBCu   /* word 6 */
#define NVM_B 0x80E0ADC0u   /* word 7 */
#define NVM_YEAR 0x80E0ADC4u /* word 8 */
#define NVM_MON  0x80E0ADC8u /* word 9 */
#define NVM_DAY  0x80E0ADCCu /* word 10 */
#define NVM_FPSN 0x80E0ADD0u /* word 11 = fps number (16 or 18) */
#define NVM_QPF  0x80E0ADE0u /* word 15 = Qp floor, NEW semantics: 0 = OFF, else 15..22. */
#define NVM_BR   0x80E0ADDCu /* word 14 = bitrate multiplier, ONLY 1/2/4 (0/unset/anything else -> BR_DEF). */
#define NVM_AST  0x80E0ADD8u /* word 13 = reel-end auto-stop. Stored as a DISABLE flag so a fresh unit
                              * (NVM reads 0) defaults to ON: 0/unset = ON, 1 = user turned it OFF. */

/* scratch state (reset every power-cycle):
 *   ST   @0x80345000 [0]=prev frm [1]=stall ticks [3]=warn-shown [4]=motor-fired [5]=clean-exit marker
 *   MENU @0x80345020 [0]=cursor(0=R,1=G,2=B,3=RST,4=YEAR,5=MON,6=DAY) [1]=wbactive [2]=btn-handled
 *                    [3..5]=RGB gains [6]=loaded-from-NVM-this-boot [7]=last frm (advance detect)
 *                    [8]=last adjust delta (sel-line) [9]=delta-show countdown
 *                    [10]=record latch (decays when frm stops advancing -> buttons poll every call)
 *                    [11]=date-loaded-this-boot [12]=year [13]=month [14]=day
 *                    [17]=fps number (16/18) [19]=fps-loaded-this-boot
 *                    [20]=Qp floor (16..26) [21]=qpfloor-loaded-this-boot
 *                    [22]=reel-end auto-stop (1=on,0=off) [23]=autostop-loaded-this-boot
 *                    [24]=bitrate multiplier (1/2/4) [25]=bitrate-loaded-this-boot
 *                    [26]=was-recording latch (rising edge resets the Qp/Sz/Cum trackers) */
#define ST   ((volatile uint32_t *)0x80345000u)   /* relocated (was 0x342700; code was creeping up) */
#define MENU ((volatile uint32_t *)0x80345020u)
#define STALL_THRESH 20         /* OnTimer ticks without a new frame before the REC STALLED warning.
                                 * OnTimer MEASURED at ~2-3 Hz (build 267: ~1 tick per encoded frame, worst
                                 * harmless hiccup = 2 ticks) */
#define MOTOR_STOP_THRESH 30    /* ticks before the clean auto-stop, ~12 s. */
#define MOTOR_STOP_ADDR 0x80ddc670u  /* 0dan0's CORRECTED motor-stop (page 14; 0x80ddb670 was a typo) */
#define DELTA_SHOW 12           /* osd_main calls the sel-line adjust value stays (~0.4s @ ~30 Hz) */
#define REC_LATCH 4             /* osd_main calls the "recording" gate survives w/o a frm advance;
                                 * covers the capture-vs-encode cadence gap (~30 vs ~18 fps) so
                                 * buttons poll EVERY call in recording, yet decays fast in preview */
/* ---- RC min-Qp clamp: off by default ----------------
 * The clamp existed to stop the rate control reaching Qp<=16, believed to crash the encoder. That
 * diagnosis was WRONG: the hard freeze was `jr $k1` in our OnTimer trampoline. Evidence it is
 * unnecessary on Type D:
 *   - the NO-HOOKS build has no clamp at all (RC free to dive to 15-16) -> low Qp does not cause freezes
 *   - build 245 froze with the clamp provably holding at 18 -> freeze without low Qp;
 *   - at x4 the clamp PINS the RC at the floor all capture (Qp 18/18), saturating it so it can never
 *     reach target -> spurious multi-second stalls at ~120-140 frames. Dropping to 16 unpinned it
 *     (Qp 18/16) and the same reel ran past 2000 frames;
 *   - it costs quality by stopping the encoder spending its budget;
 * With QPFLOOR_CLAMP 0 the stub is not compiled, so osd_build.py finds no symbol and skips the hook.
 * The Qp cur/low READOUT stays (read-only, needs no hook) -- still the most useful single number on a
 * frozen screen, and worth watching in case Qp ever dips somewhere unexpected. */
#define QPFLOOR_CLAMP 1
#define QP_FLOOR 20      /* legacy, unused */
/* Qp floor is OFF by default (2026-09-23). Kept as a tunable so a future freeze or a user with odd
 * footage can be investigated WITHOUT shipping new firmware. */
#define QPF_OFF  0       /* menu "-" : write the stock 10 -> clamp inert */
#define QPF_MIN  15
#define QPF_MAX  22
#define QPF_DEF  QPF_OFF
#define QPF_STOCK 10     /* the constant the untouched rate control uses */
#define STOP_THRESH  200        /* stall ticks before AUTO-STOP (inject OK) — tune via on-screen t: */
#define STOP_INJECT_MAX 280     /* stop injecting past this (safe fallback: warn stays, no auto-stop) */
#define WARN_HOLD 150           /* keep the warning on screen ~this long AFTER an auto-stop */
#define WB_STEP 16      /* coarse (Left/Right) */
#define WB_FINE 1       /* fine (+/-) */
#define WB_MAX  999     /* cap so 3-digit display always fits */
/* A WB gain outside WB_OK_MIN..WB_MAX is not a real setting: zeros or stray values in the NVM words, or
 * live registers the stock firmware never wrote. Seen in the field: a unit whose NVM made us apply ~0
 * gains (very dark picture, "WB gains 000,000,000"). Such values are treated as "not set". */
#define WB_OK_MIN 16
#define WB_OK(v) ((v) >= WB_OK_MIN && (v) <= WB_MAX)
#define WB_DEF_R 689    /* stock D preset as read on a working unit -- the start point for a first edit */
#define WB_DEF_G 256    /* when the live gains are not valid */
#define WB_DEF_B 218
/* Menu cursor. REWORKED 2026-09-25: the order below MUST match the on-screen row order top-to-bottom,
 * because Up/Down now walks the rows and the selected VALUE is wrapped in [ ]. */
#define CUR_R    0
#define CUR_G    1
#define CUR_B    2
#define CUR_FPS  3
#define CUR_BR   4       /* bitrate multiplier x1..x4 */
#define CUR_AST  5       /* reel-end auto-stop ON/OFF toggle */
#define CUR_QP   6       /* Qp floor: "-" (off) or 15..22 */
#define CUR_YEAR 7
#define CUR_MON  8
#define CUR_DAY  9
#define CUR_RST  10      /* "Reset" -- own row, last in the cycle */
#define CUR_MAX  10
/* Set to 0 to draw ONLY in preview -> a ZERO-DRAWING window during recording.
 * Set back to 1 to restore the normal OSD. */
#define OSD_DRAW_WHILE_REC 1
/* Override stock WB controls. */
#define WB_OVERRIDE 1
#define BR_VAL_ADDR  0x8034533cu  /* scratch: LIVE bitrate multiplier, read by bitrate_stub at record-init
                                   * (hooked at 0x80240588). Fed from MENU[24] by osd_main -> the BR menu
                                   * item picks the bitrate without a reflash. */
/* Bitrate multiplier, menu-selectable: x1 (~8Mbps, stock) / x2 (~16) / x4 (~32). */
#define BR_DEF 4
#define BR_OK(v) ((v) == 1u || (v) == 2u || (v) == 4u)
#define QPFLOOR_ADDR 0x80345338u  /* scratch: LIVE RC min-Qp floor. */
#define FPS_VAL_ADDR 0x80345100u  /* scratch: N=fps*3000 (48000/54000); fps_stub reads it @record-init */
#define YEAR_COARSE 10  /* Left/Right step on the year field (+/- = fine ±1) */
#define YEAR_MIN 1980   /* FAT 1980 floor */
#define YEAR_MAX 2100

static uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }
static void wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* D setdate() @0x80069ea0 (fingerprinted from A/B/C's 0x693a0, real offset +0xB00; writes RTC HW regs
 * 0xb0060010/04; sig (year,month,day,0), returns 0 on success). Verified on device build 170: stamps
 * the recorded file's date. We store y/m/d in NVM (words 8/9/10) + MENU[12..14], apply at boot + on
 * every edit. RTC not battery-backed -> "set-and-sticks" (reapply each boot). */
#define SETDATE_ADDR 0x80069ea0u
typedef int (*setdate_fn)(int y, int m, int d, int a4);
static void apply_date(void) {
    wr(NVM_YEAR, MENU[12]); wr(NVM_MON, MENU[13]); wr(NVM_DAY, MENU[14]);   /* persist */
    ((setdate_fn)SETDATE_ADDR)((int)MENU[12], (int)MENU[13], (int)MENU[14], 0);
}

/* framerate stub: hooked at 0x1f20c8 (replaces `li a3,N; sw a3,0(a1)`). Reads N (=fps*3000) from
 * scratch FPS_VAL_ADDR (set each frame by osd_main from the menu setting), defaults to 54000 (18fps)
 * if unset, writes it to *(a1) (the record-rate field). Touches only a3 -> preserves the caller's
 * v0/etc used after the call. (a1=$5, a3=$7, ra=$31.) */
__asm__(
    ".set noreorder\n"
    ".globl fps_stub\n"
    "fps_stub:\n"
    "  lui   $7, 0x8034\n"
    "  lw    $7, 0x5100($7)\n"     /* a3 = *(0x80345100) = N */
    "  bnez  $7, 1f\n"
    "  nop\n"
    "  ori   $7, $0, 0xd2f0\n"     /* default 54000 (18fps) if scratch unset */
    "1:\n"
    "  sw    $7, 0($5)\n"          /* *(a1) = N */
    "  jr    $31\n"
    "  nop\n"
    ".set reorder\n"
);
/* --- bitrate stub (hooked at 0x80240588) ----------------------------------------------------
 * Record-init computes the encoder target bitrate as  *(s0+0x50) = *(s1+0x18) << 3  -- a SHIFT, so the
 * stock lever only reaches powers of two (shift3=x1, 4=x2, 5=x4...). We replace the `sll v1,v1,3` with
 * `j bitrate_stub` so we can multiply by an ARBITRARY factor (x3!) taken from scratch, i.e. from the BR
 * menu item -- no reflash to change bitrate. NOTE the j's delay slot is the original `sw v1,0x50(s0)`,
 * which still runs and stores the UNSHIFTED value; we simply redo the store with the right one here.
 * This site runs ONCE per recording (record-init), not per frame, so it is far lower risk than a
 * per-frame hook. $at is used as the temp (nothing live in it); v1=$3 raw value, s0=$16 config struct. */
__asm__(
    ".set noreorder\n"
    ".set noat\n"
    ".globl bitrate_stub\n"
    "bitrate_stub:\n"
    "  lui   $1, 0x8034\n"
    "  lw    $1, 0x533c($1)\n"   /* at = live bitrate multiplier (1/2/4) */
    "  sltiu $4, $1, 5\n"   /* a0 = (m<5)?   a0 is DEAD here (0x240590 reloads it) */
    "  beqz  $4, 9f\n"   /* m >= 5 -> default */
    "  nop\n"
    "  bnez  $1, 8f\n"   /* m != 0 -> use it */
    "  nop\n"
    "9:\n"
    "  ori   $1, $0, 4\n"   /* 0 / unset / out of range -> x4 */
    "8:\n"
    "  sll   $4, $3, 3\n"   /* a0 = base = raw<<3  (== the stock x1 bitrate) */
    "  move  $3, $0\n"   /* v1 = 0 */
    "3:\n"
    "  addu  $3, $3, $4\n"   /* v1 += base */
    "  addiu $1, $1, -1\n"
    "  bnez  $1, 3b\n"   /* v1 = base*m, shifts+adds only; m=4 gives raw<<5 == the original x4 patch */
    "  nop\n"
    "  sw    $3, 0x50($16)\n"   /* redo the store with the correct bitrate */
    "  j     0x80240590\n"
    "  nop\n"
    ".set at\n"
    ".set reorder\n"
);

/* --- RC min-Qp floor stub (hooked at 0x80241074) --------------------------------------------
 * The per-frame rate control (0x80240f94, called every frame from EncFrameStart 0x8023f814) computes
 *     effective Qp = min( targetQp , max( cap , 10 ) )
 * at 0x80241070-0x80241084 and stores it via its SOLE writer 0x80241114 `sw v0,40(s2)` (s2 = ctx+0x6c,
 * so the stored field is ctx+0x94).
 * osd_build.py replaces `slti a1,s4,10` + `li v1,10` with `j qpfloor_stub` + nop; we rebuild those SAME
 * two values from a LIVE floor (QPFLOOR_ADDR, fed by the QpF menu) and return to 0x8024107c
 * (`movz v1,s4,a1` = max(cap,floor)). Clobbers ONLY v1($3)/a1($5) -- exactly what the two replaced
 * instructions clobbered; v0 (targetQp) and s4 (cap, $20) untouched, no stack use.
 * Guard: a floor outside 10..40 (uninitialised/garbage scratch) falls back to 10. */
#if QPFLOOR_CLAMP
__asm__(
    ".set noreorder\n"
    ".globl qpfloor_stub\n"
    "qpfloor_stub:\n"
    "  lui   $3, 0x8034\n"
    "  lw    $3, 0x5338($3)\n"   /* v1 = live floor (QPFLOOR_ADDR); 10 = OFF/stock */
    "  sltiu $5, $3, 10\n"   /* a1 = (floor < 10) */
    "  bnez  $5, 9f\n"   /* below stock -> use stock */
    "  nop\n"
    "  sltiu $5, $3, 41\n"   /* a1 = (floor < 41) */
    "  bnez  $5, 8f\n"   /* 10..40 -> use it */
    "  nop\n"
    "9:\n"
    "  ori   $3, $0, 10\n"   /* unset / out of range -> the stock constant, clamp inert */
    "8:\n"
    "  slt   $5, $20, $3\n"   /* a1 = (cap < floor)  [s4 = $20] */
    "  j     0x8024107c\n"   /* back into the RC at `movz v1,s4,a1` */
    "  nop\n"
    ".set reorder\n"
);
#endif
/* load persisted FPS choice once per boot (default 18) */
static void load_fps_once(void) {
    if (MENU[19]) return;
    MENU[19] = 1;
    MENU[17] = (rd(NVM_FPSN) == 16) ? 16 : 18;
}
/* load the persisted Qp floor once per boot. 0 = OFF; anything outside 15..22 also falls back to OFF. */
static void load_qpf_once(void) {
    if (MENU[21]) return;
    MENU[21] = 1;
    uint32_t v = rd(NVM_QPF);
    MENU[20] = (v >= QPF_MIN && v <= QPF_MAX) ? v : QPF_OFF;
}
/* load the persisted bitrate multiplier once per boot (default BR_DEF; clamp into range) */
static void load_br_once(void) {
    if (MENU[25]) return;
    MENU[25] = 1;
    uint32_t v = rd(NVM_BR);
    MENU[24] = BR_OK(v) ? v : BR_DEF;   /* allow-list: a stale 3 from the test builds falls back to x4 */
}
/* load persisted reel-end auto-stop choice once per boot. NVM holds a DISABLE flag, so a fresh unit
 * (0) defaults to ON; only an explicit 1 (user turned it off) disables it. */
static void load_ast_once(void) {
    if (MENU[23]) return;
    MENU[23] = 1;
    MENU[22] = (rd(NVM_AST) == 1u) ? 0u : 1u;
}

/* --- pool+0 (column-major, X-flipped) draw helpers --- */
static void cclr(volatile uint8_t *p0, int X0, int Y0, int w, int h, uint8_t v) {
    int xx, yy; for (xx = 0; xx < w; xx++) { volatile uint8_t *col = p0 + (863 - (X0 + xx)) * SH + Y0; for (yy = 0; yy < h; yy++) col[yy] = v; }
}
static void cchar(volatile uint8_t *p0, int X, int Y, unsigned char c, uint8_t col) {
    const unsigned char *g = &g_font[c * 12]; int r, b;
    for (r = 0; r < 12; r++) { unsigned char bits = g[r]; for (b = 0; b < 8; b++) if (bits & (0x80 >> b)) p0[(863 - (X + b)) * SH + (Y + r)] = col; }
}
static int cstr(volatile uint8_t *p0, int X, int Y, const char *s, uint8_t col) {
    while (*s) { cchar(p0, X, Y, (unsigned char)*s, col); X += 7; s++; } return X;
}
static int ndig(int v) { int n = (v <= 0) ? 1 : 0; if (v < 0) n++; while (v) { n++; v /= 10; } return n; }
static int cnum(volatile uint8_t *p0, int X, int Y, int v, uint8_t col) {
    char t[12]; int n = 0;
    if (v < 0) { cchar(p0, X, Y, '-', col); X += 7; v = -v; }
    if (v == 0) { cchar(p0, X, Y, '0', col); return X + 7; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) { cchar(p0, X, Y, t[--n], col); X += 7; }
    return X;
}
/* [ ] around the value currently being edited. Drawn only for the selected item, so exactly one value
 * on screen is ever bracketed. */
static int cbr(volatile uint8_t *p0, int X, int Y, int on, const char *br, uint8_t col) {
    return on ? cstr(p0, X, Y, br, col) : X;
}
static int cfix(volatile uint8_t *p0, int X, int Y, int v, int nd, uint8_t col) {
    int i, pw = 1; if (v < 0) v = 0;
    for (i = 1; i < nd; i++) pw *= 10;
    for (i = 0; i < nd; i++) { cchar(p0, X, Y, (char)('0' + (v / pw) % 10), col); X += 7; pw /= 10; }
    return X;
}

/* --- menu input: edge-detected, recording-only --- */
static void handle_buttons(int rec) {
    uint32_t code = rd(BTN_CODE), hold = rd(BTN_HOLD);
    if (!rec) { MENU[2] = 0; return; }        /* menu only during recording */
    if (hold == 0) { MENU[2] = 0; return; }   /* released -> re-arm */
    if (hold < 2) return;                     /* debounce */
    if (MENU[2]) return;                      /* already acted on this press */

    if (code == 0x001) { MENU[0] = (MENU[0] == 0) ? CUR_MAX : MENU[0] - 1; MENU[2] = 1; MENU[8] = 0; MENU[9] = 0; return; }  /* Up: also clear sel delta */
    if (code == 0x002) { MENU[0] = (MENU[0] + 1) % (CUR_MAX + 1); MENU[2] = 1; MENU[8] = 0; MENU[9] = 0; return; }           /* Down */

    int dir, coarse;
    if (code == 0x008) { dir = 1; coarse = 1; }        /* Right: coarse + */
    else if (code == 0x004) { dir = -1; coarse = 1; }  /* Left:  coarse - */
    else if (code == 0x200) { dir = 1; coarse = 0; }   /* Plus:  fine + */
    else if (code == 0x100) { dir = -1; coarse = 0; }  /* Minus: fine - */
    else return;                                       /* other buttons ignored */

    MENU[2] = 1;
    if (MENU[0] == CUR_FPS) {                    /* FPS: toggle 16 <-> 18 */
        MENU[17] = (MENU[17] == 16) ? 18 : 16;
        wr(NVM_FPSN, MENU[17]);                  /* persist */
        MENU[8] = 0;                             /* no sel-delta for a toggle */
        return;
    }
    if (MENU[0] == CUR_QP) {                     /* Qp floor: OFF <-> 15..22 */
        int v = (int)MENU[20];
        if (dir > 0) v = (v == QPF_OFF) ? QPF_MIN : ((v >= QPF_MAX) ? QPF_MAX : v + 1);
        else         v = (v <= QPF_MIN) ? QPF_OFF : v - 1;
        MENU[20] = (uint32_t)v; wr(NVM_QPF, MENU[20]);   /* persist */
        MENU[8] = (uint32_t)dir; MENU[9] = DELTA_SHOW;
        return;
    }
    if (MENU[0] == CUR_BR) {                     /* bitrate: x1..x4, applied at the NEXT record start */
        uint32_t v = MENU[24];                           /* wraps x1 -> x2 -> x4 -> x1, skipping broken x3 */
        if (dir > 0) v = (v == 1u) ? 2u : ((v == 2u) ? 4u : 1u);
        else         v = (v == 4u) ? 2u : ((v == 2u) ? 1u : 4u);
        MENU[24] = v; wr(NVM_BR, v);                     /* persist */
        MENU[8] = (uint32_t)dir; MENU[9] = DELTA_SHOW;
        return;
    }
    if (MENU[0] == CUR_AST) {                    /* auto-stop: any adjust toggles ON <-> OFF */
        MENU[22] = MENU[22] ? 0u : 1u;
        wr(NVM_AST, MENU[22] ? 0u : 1u);         /* persist DISABLE flag (0=on, 1=off) */
        MENU[8] = 0;                             /* no sel-delta for a toggle */
        return;
    }
    if (MENU[0] == CUR_RST) {                    /* RST: reset EVERYTHING - WB back to stock + date + FPS + Qp floor */
        wr(NVM_R, 0); wr(NVM_G, 0); wr(NVM_B, 0);
        MENU[1] = 0;                             /* stop overriding -> back to stock WB preset */
        MENU[12] = 2026; MENU[13] = 1; MENU[14] = 1;   /* date back to 2026-01-01 */
        apply_date();                            /* persist + apply the reset date */
        MENU[17] = 18; wr(NVM_FPSN, 18);         /* FPS back to default 18 */
        MENU[22] = 1; wr(NVM_AST, 0);            /* reel-end auto-stop back to default ON */
        MENU[20] = QPF_DEF; wr(NVM_QPF, QPF_DEF); /* Qp floor back to OFF */
        MENU[24] = BR_DEF; wr(NVM_BR, BR_DEF);   /* bitrate back to default x4 */
        MENU[8] = 0;                             /* no delta shown for RST */
        return;
    }
    if (MENU[0] >= CUR_YEAR && MENU[0] <= CUR_DAY) {  /* date fields: YEAR / MON / DAY */
        int f = (int)MENU[0] - CUR_YEAR;          /* 0=year, 1=month, 2=day */
        int slot = 12 + f;
        int val = (int)MENU[slot];
        int step = 1;
        if (f == 0) { step = coarse ? YEAR_COARSE : 1; val += dir * step;
                      if (val < YEAR_MIN) val = YEAR_MIN; if (val > YEAR_MAX) val = YEAR_MAX; }
        else if (f == 1) { val += dir;                                   /* month wraps 1..12 */
                      if (val > 12) val = 1; if (val < 1) val = 12; }
        else { val += dir; if (val < 1) val = 31; if (val > 31) val = 1; } /* day wraps 1..31 */
        MENU[slot] = (uint32_t)val;
        MENU[8] = (uint32_t)(dir * step); MENU[9] = DELTA_SHOW;
        apply_date();   /* persist + LIVE apply via setdate: the RTC tracks each edit, so on Stop the
                         * CURRENT video (file date = RTC at close) gets the set date. The floor-NOP
                         * means every year applies cleanly, so no intermediate-value stickiness. */
        return;
    }
    /* WB fields (R/G/B) */
    int delta = dir * (coarse ? WB_STEP : WB_FINE);
    if (!MENU[1]) {                              /* first edit: start from the live gains... */
        uint32_t r = rd(WB_R), g = rd(WB_G), b = rd(WB_B);
        if (!(WB_OK(r) && WB_OK(g) && WB_OK(b))) { r = WB_DEF_R; g = WB_DEF_G; b = WB_DEF_B; }   /* ...if sane */
        MENU[3] = r; MENU[4] = g; MENU[5] = b; MENU[1] = 1;
    }
    int idx = 3 + ((int)MENU[0] - CUR_R);        /* R/G/B are cursor 0/1/2 -> MENU[3/4/5] */
    int v = (int)MENU[idx] + delta;
    if (v < WB_OK_MIN) v = WB_OK_MIN; if (v > WB_MAX) v = WB_MAX;   /* stay in the range load_wb_once accepts */
    MENU[idx] = (uint32_t)v;
    MENU[8] = (uint32_t)delta; MENU[9] = DELTA_SHOW;   /* show last adjust briefly, then auto-clear */
    wr(NVM_R, MENU[3]); wr(NVM_G, MENU[4]); wr(NVM_B, MENU[5]);  /* persist */
}
/* load persisted WB once per boot (scratch resets on power-cycle) */
static void load_wb_once(void) {
    if (MENU[6]) return;
    uint32_t r = rd(NVM_R), g = rd(NVM_G), b = rd(NVM_B);
    if (WB_OK(r) && WB_OK(g) && WB_OK(b)) { MENU[3] = r; MENU[4] = g; MENU[5] = b; MENU[1] = 1; }   /* else: stock WB */
    MENU[6] = 1;
}
/* restore persisted date once per boot + apply via setdate (scratch resets on power-cycle). Called
 * from osd_main (camera pipeline up -> RTC driver ready). Defaults to 2026-01-01 if NVM is unset. */
static void load_date_once(void) {
    if (MENU[11]) return;
    MENU[11] = 1;
    uint32_t y = rd(NVM_YEAR), mo = rd(NVM_MON), d = rd(NVM_DAY);
    if (y >= YEAR_MIN && y <= YEAR_MAX && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
        MENU[12] = y; MENU[13] = mo; MENU[14] = d;
    } else { MENU[12] = 2026; MENU[13] = 1; MENU[14] = 1; }   /* sensible default (better than stock 2022) */
    ((setdate_fn)SETDATE_ADDR)((int)MENU[12], (int)MENU[13], (int)MENU[14], 0);
}
/* Apply our WB AFTER orig() (stock writes the fixed preset each frame; we override it). Applied
 * whenever WB is active (saved/edited) regardless of preview vs recording -> WYSIWYG in preview,
 * and it no longer snaps back to stock when a recording stalls. Menu INPUT stays recording-only
 * (handle_buttons), so preview can't be edited; it only reflects the already-set value. */
static void apply_wb(void) {
    if (!MENU[1]) return;
    wr(WB_R, MENU[3]); wr(WB_G, MENU[4]); wr(WB_B, MENU[5]);
    wr(WB_R2, MENU[3]); wr(WB_G2, MENU[4]); wr(WB_B2, MENU[5]);
}

/* --- blank-frame (reel-end) detection via auto-exposure (0x80f82594, microseconds) ---
 * Measured on device: no reel (bare white backlight) ~118us; gray leader (reel start/end) ~204us;
 * real footage ~340us (lightest) .. ~1000us (darkest). Low Exp = blank. ARM on real footage first so
 * the START leader can't trigger, then N consecutive blank frames at the END = reel ran out. */
#define EXP_ADDR   0x80f82594u
#define BLANK_EXP  150u    /* Exp below this = NO FILM (bare white backlight ~118us). NOT the gray leader
                            * (~204us): bright white FOOTAGE overlaps it (~203us, and briefly dips to ~158us
                            * on very white frames) so exposure can't separate them. Anchor on the distinct
                            * white RUN-OFF (~118us, AE at min) that footage never sustains. 150 = ~32us above
                            * the run-off, just under the briefest footage dip (158) -> the 20-frame run does
                            * the rest (a momentary white dip never lasts 20 consecutive frames). */
#define ARM_EXP    320u    /* Exp above this = real footage -> arm the detector */
#define BLANK_WARN 2u      /* show the reel-end warning after this many consecutive blank frames */
#define BLANK_STOP 20u     /* auto-stop after this many consecutive blank frames (~10s @ ~2 enc-fps); the
                            * margin also guards against a long bright footage shot false-triggering */
#define BLK_ARMED  0x80345300u  /* scratch: 1 once real footage was seen this recording */
#define BLK_COUNT  0x80345304u  /* scratch: consecutive blank-frame run length */
#define BLK_DONE   0x80345308u  /* scratch: 1 after a reel-end auto-stop -> persist the "REEL ENDED"
                                 * message in preview (so an away user sees what happened); cleared when
                                 * the next recording starts */
#define QPMIN_ADDR 0x80345310u  /* scratch: lowest raw Qp seen this recording (diagnostic; a frozen screen
                                 * then shows how low Qp went before a freeze). Reset to 99 when not recording. */
#define SZMAX_ADDR 0x80345328u  /* scratch: max per-frame encoded size (bytes) this recording. The size is
                                 * *(ctx+0xd8) (device-confirmed: updates per frame, ~170-300KB at x4). A huge
                                 * max just before a freeze = encoder output-buffer overrun. Reset when idle. */
#define CUMB_ADDR  0x8034532cu  /* scratch: cumulative encoded bytes this recording (+= *(ctx+0xd8) each new
                                 * frame). Shown in MiB. Reveals if a freeze clusters at a byte boundary (file
                                 * 2/4GiB, or an SD allocation-unit / FAT boundary). 32-bit OK (<4GiB cap). */
#define QPRAW_ADDR 0x80345330u  /* scratch: raw PRE-clamp current Qp -- the value the rate-control actually
                                 * chose this frame, before our floor clamp overwrites ctx+0x94. Shown as the
                                 * Qp "cur" so a freeze photo reveals the TRUE Qp on the frozen frame (the old
                                 * "cur" read ctx+0x94 AFTER the clamp -> cosmetically pinned to the floor). */
#define SEED_ADDR  0x80345334u  /* scratch: the 3 per-frame-type Qp SEED bytes ctx+0x91/0x92/0x93 packed
                                 * (b0|b1<<8|b2<<16), captured BEFORE we raise them. RC-init copies a seed ->
                                 * ctx+0x94 each frame, so the seeds are the real min-Qp lever (RE build 241).
                                 * Diagnostic: if these read 20-30 while driven Qp shows 15-16 -> a hidden
                                 * ctx+0x94 writer exists (seeds aren't the whole story). */

/* ---- ERASE CATCHER (diagnostic build 266) --------------------------------------------------
 * Residual known since build 144: in RECORDING the stock UI occasionally wipes our OSD for ~1 frame --
 * the user sees it exactly when the red REC dot is HIDDEN (~every 2s). We keep a SENTINEL = the address
 * of one lit pixel of the "F" in "Frm:". If it is no longer our colour, the OSD was wiped. OnTimer
 * (after_ontimer, which runs right after the stock OnTimer that blinks the icon) checks it and repaints
 * immediately; osd_main checks it too, counting wipes that slipped past OnTimer.
 *   T = wipes caught + repainted in OnTimer      E = wipes that reached osd_main (missed by OnTimer)
 * Readout on the tag line: "FW nnn Tx Ey". T rising, E ~0 => fixed. E rising => erase is elsewhere. */
#define DIAG_ERASE 0   /* 1 = show the T/E/C/G diagnostic counters (build 266-268); the FIX itself is not gated */
#define SENT_ADDR  0x80345340u  /* scratch: sentinel pixel offset + 1 (0 = invalid / not recording) */
#define CNT_T_ADDR 0x80345344u  /* scratch: wipes caught + repainted in OnTimer */
#define CNT_E_ADDR 0x80345348u  /* scratch: wipes first seen by osd_main (missed by OnTimer) */
/* STALL-MARGIN measurement (build 267). The stall thresholds are in OnTimer TICKS, and the tick rate has
 * never been measured -- "20 ticks ~= 1s" was an estimate. So measure, per recording:
 *   A = average ticks per encoded frame (the NORMAL gap)       G = largest gap that RECOVERED, in ticks
 * MOTOR_STOP_THRESH / A = how many normal frame-gaps we tolerate; G / A = worst hiccup that was harmless.
 * Shown on a second line under the build tag while DIAG_ERASE is on. */
#define CNT_C_ADDR 0x80345354u  /* scratch: wipes caught + repainted right after the compose blit */
#define GAPMAX_ADDR 0x8034534cu /* scratch: largest recovered inter-frame gap this recording (ticks) */
#define TICKS_ADDR  0x80345350u /* scratch: OnTimer ticks elapsed this recording */
static uint32_t glyph_px_off(int X, int Y, unsigned char c) {   /* offset+1 of first lit pixel of c at (X,Y) */
    const unsigned char *g = &g_font[c * 12]; int r, b;
    for (r = 0; r < 12; r++) for (b = 0; b < 8; b++)
        if (g[r] & (0x80 >> b)) return (uint32_t)((863 - (X + b)) * SH + (Y + r)) + 1u;
    return 0u;
}
static void draw_version(volatile uint8_t *p0);   /* defined with the warning-area helpers below */
static void rend_p0(volatile uint8_t *p0, int clear) {
    int SX = 764, X, Y;
    int wrv = rd(WB_R), wgv = rd(WB_G), wbv = rd(WB_B);
    int ex = rd(0x80f82594u), gn = rd(0x80f82598u);
    int resw = rd(0x80ddc718u), resh = rd(0x80ddc71cu);
    uint32_t ctx = rd(0x80e974dcu);
    int rec = (ctx >= 0x80000000u && ctx < 0xb0000000u);
    int qp = 0, frm = 0;
    if (rec) {
        int v = (int)rd(QPRAW_ADDR); if (v >= 0 && v <= 99) qp = v;   /* raw pre-clamp Qp (set by osd_main) */
        frm = *(volatile int *)(ctx + 0xd4u);
    }
    /* live = a recording is actually running (movie state 10, the same flag the stall guard uses). `rec`
     * (ctx valid) LINGERS in preview after a stop, which keeps the diagnostics on screen on purpose -- but
     * the menu only takes input while recording, so the [ ] cursor and the Reset row are drawn only when
     * live. Otherwise a hint row says where editing works (users read the stale brackets as a live menu). */
    int live = rec && (*(volatile uint8_t *)0x80e0a624u == 10);
    int wbsel = live && MENU[0] <= CUR_B;       /* any WB channel selected -> drop the indent for [ ] */
    int dtsel = live && MENU[0] >= CUR_YEAR && MENU[0] <= CUR_DAY;
    int nrows = rec ? 14 : 8;   /* rec layout: Reset row (live) or hint row (stopped); preview: + hint row */
    Y = 444 - (nrows - 1) * 13;
    if (clear) cclr(p0, SX - 2, Y - 2, 100, nrows * 13 + 4, 0);   /* 0 = repaint over an already-wiped area */
    if (!live) { cstr(p0, SX, Y, "Edit in REC", WHT); Y += 13; }   /* 11 chars; 14 ("Edit: REC only") clipped the last letter on the LCD */
    if (rec) { wr(SENT_ADDR, glyph_px_off(SX, Y, 'F'));   /* erase sentinel = a lit pixel of this "F" */
               cstr(p0, SX, Y, "Frm:", CYN); cnum(p0, SX + 63 - ndig(frm) * 7, Y, frm, CYN); Y += 13; }
    else wr(SENT_ADDR, 0u);
    /* Single WB row: the LIVE gain registers, which ARE what the menu edits -- apply_wb pushes the
     * setpoint into them earlier in the same osd_main pass, so edits show immediately and there is no
     * need for a separate "WB set" row. */
    cstr(p0, SX, Y, "WB gains:", YEL); Y += 13;
    X = wbsel ? SX : SX + 4;                 /* no indent when bracketed, to buy back the [ ] width */
    X = cbr(p0, X, Y, live && MENU[0] == CUR_R, "[", YEL);
    X = cfix(p0, X, Y, wrv, 3, YEL);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_R, "]", YEL); X = cstr(p0, X, Y, ",", YEL);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_G, "[", YEL);
    X = cfix(p0, X, Y, wgv, 3, YEL);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_G, "]", YEL); X = cstr(p0, X, Y, ",", YEL);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_B, "[", YEL);
    X = cfix(p0, X, Y, wbv, 3, YEL);
    cbr(p0, X, Y, live && MENU[0] == CUR_B, "]", YEL); Y += 13;
    if (rec) {
        X = cstr(p0, SX, Y, "FPS: ", CYN);
        X = cbr(p0, X, Y, live && MENU[0] == CUR_FPS, "[", CYN);
        X = cnum(p0, X, Y, (int)MENU[17], CYN);
        cbr(p0, X, Y, live && MENU[0] == CUR_FPS, "]", CYN); Y += 13;
        X = cstr(p0, SX, Y, "BR: ", CYN);
        X = cbr(p0, X, Y, live && MENU[0] == CUR_BR, "[", CYN);
        X = cstr(p0, X, Y, "x", CYN); X = cnum(p0, X, Y, (int)MENU[24], CYN);
        cbr(p0, X, Y, live && MENU[0] == CUR_BR, "]", CYN); Y += 13;
        X = cstr(p0, SX, Y, "ASt: ", CYN);
        X = cbr(p0, X, Y, live && MENU[0] == CUR_AST, "[", CYN);
        X = cstr(p0, X, Y, MENU[22] ? "ON" : "OFF", CYN);
        cbr(p0, X, Y, live && MENU[0] == CUR_AST, "]", CYN); Y += 13;
        X = cstr(p0, SX, Y, "Qp:", QPC); X = cnum(p0, X, Y, qp, QPC);   /* current(driven) / lowest-this-recording */
        X = cstr(p0, X, Y, "/", QPC);
        { uint32_t m = rd(QPMIN_ADDR);   /* "-" until the frm>15 warmup lets tracking start */
          X = m ? cnum(p0, X, Y, (int)m, QPC) : cstr(p0, X, Y, "-", QPC); }
        X = cstr(p0, X, Y, " f", QPC);       /* floor setting: "-" when off */
        X = cbr(p0, X, Y, live && MENU[0] == CUR_QP, "[", QPC);
        if (MENU[20] >= QPF_MIN && MENU[20] <= QPF_MAX) X = cnum(p0, X, Y, (int)MENU[20], QPC);
        else X = cstr(p0, X, Y, "-", QPC);
        cbr(p0, X, Y, live && MENU[0] == CUR_QP, "]", QPC); Y += 13;
        /* per-frame encoded size (KB): current / max-this-recording. *(ctx+0xd8) = frame bytes. A max
         * far above normal just before a freeze = encoder output-buffer overrun. */
        X = cstr(p0, SX, Y, "Sz:", QPC);
        X = cnum(p0, X, Y, (int)(*(volatile uint32_t *)(ctx + 0xd8u) >> 10), QPC);
        X = cstr(p0, X, Y, "/", QPC); X = cnum(p0, X, Y, (int)(rd(SZMAX_ADDR) >> 10), QPC);
        cstr(p0, X, Y, "K", QPC); Y += 13;
        /* cumulative encoded bytes this recording, in MiB -- does a freeze cluster at a byte boundary? */
        X = cstr(p0, SX, Y, "Cum:", QPC); X = cnum(p0, X, Y, (int)(rd(CUMB_ADDR) >> 20), QPC);
        cstr(p0, X, Y, "M", QPC); Y += 13;
        X = cstr(p0, SX, Y, "Gain:", CYN); cnum(p0, X, Y, gn, CYN); Y += 13;
        X = cstr(p0, SX, Y, "Exp:", CYN); X = cnum(p0, X, Y, ex, CYN); cstr(p0, X, Y, "us A", CYN); Y += 13;
    } else {
        X = cstr(p0, SX, Y, "Res:", CYN); X = cnum(p0, X, Y, resw, CYN); X = cstr(p0, X, Y, "x", CYN); cnum(p0, X, Y, resh, CYN); Y += 13;
        X = cstr(p0, SX, Y, "Gain:", CYN); cnum(p0, X, Y, gn, CYN); Y += 13;
        X = cstr(p0, SX, Y, "Exp:", CYN); X = cnum(p0, X, Y, ex, CYN); cstr(p0, X, Y, "us", CYN); Y += 13;
    }
    /* Date LAST in the list (persisted; applied via setdate). Shown in preview + recording. */
    cstr(p0, SX, Y, "Date:", CYN); Y += 13;
    X = dtsel ? SX : SX + 4;                 /* no indent when bracketed */
    X = cbr(p0, X, Y, live && MENU[0] == CUR_YEAR, "[", CYN);
    X = cfix(p0, X, Y, (int)MENU[12], 4, CYN);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_YEAR, "]", CYN); X = cstr(p0, X, Y, "-", CYN);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_MON, "[", CYN);
    X = cfix(p0, X, Y, (int)MENU[13], 2, CYN);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_MON, "]", CYN); X = cstr(p0, X, Y, "-", CYN);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_DAY, "[", CYN);
    X = cfix(p0, X, Y, (int)MENU[14], 2, CYN);
    X = cbr(p0, X, Y, live && MENU[0] == CUR_DAY, "]", CYN); Y += 13;
    if (live) {                                  /* Reset LAST, below Date (only while it can be used) */
        X = cbr(p0, SX, Y, MENU[0] == CUR_RST, "[", WHT);
        X = cstr(p0, X, Y, "Reset", WHT);
        cbr(p0, X, Y, MENU[0] == CUR_RST, "]", WHT);
    }
}
int osd_main(void *a0, void *a1, void *a2, void *a3) {
    uint32_t ctx = rd(0x80e974dcu);
    int ctxok = (ctx >= 0x80000000u && ctx < 0xb0000000u);
    uint32_t frm = ctxok ? *(volatile uint32_t *)(ctx + 0xd4u) : 0;
    /* The encoded-frame counter only advances during real recording (ctx alone lingers valid in
     * preview). But osd_main fires per CAPTURED frame (~30 Hz) while frm advances per ENCODED frame
     * (~18 Hz), so gating input on "frm advanced THIS call" discarded ~1/3 of polls -> sluggish.
     * Instead: on each advance refill a short latch; poll buttons on EVERY call while it's alive.
     * The latch decays within a few calls once frm stops advancing -> still inert in preview. */
    int advanced = ctxok && (frm != MENU[7]);
    MENU[7] = frm;
    if (advanced) MENU[10] = REC_LATCH;
    else if (MENU[10]) MENU[10]--;
    int recording = ctxok && (MENU[10] != 0);
    if (recording) {
        if (!MENU[26]) {   /* rising edge = a NEW recording starts -> reset the trackers HERE, so that after
                            * a stop/auto-stop/freeze the last recording's values stay readable on screen */
            MENU[26] = 1;
            wr(QPMIN_ADDR, 0u); wr(SZMAX_ADDR, 0u); wr(CUMB_ADDR, 0u); wr(QPRAW_ADDR, 0u);
            wr(CNT_T_ADDR, 0u); wr(CNT_E_ADDR, 0u); wr(CNT_C_ADDR, 0u); wr(GAPMAX_ADDR, 0u); wr(TICKS_ADDR, 0u);
        }
        volatile uint32_t *cqp = (volatile uint32_t *)(ctx + 0x94u);
        uint32_t q = *cqp;
        wr(QPRAW_ADDR, q);   /* raw driven Qp for the OSD "cur" (freeze photo -> true frozen-frame Qp) */
        /* track lowest raw Qp (pre-clamp) AFTER the startup keyframe warmup (frm>15) -- the first I-frame
         * always dips to ~10 and would mask mid-recording dips. Steady-state min = real anomaly signal. */
        { uint32_t m = rd(QPMIN_ADDR);   /* m==0 = not tracking yet (startup keyframe window) */
          if (frm > 15u && q > 0 && q <= 99u && (m == 0u || q < m)) wr(QPMIN_ADDR, q); }
        uint32_t fsz = *(volatile uint32_t *)(ctx + 0xd8u);           /* per-frame encoded size (bytes) */
        if (frm > 3u && fsz > rd(SZMAX_ADDR)) wr(SZMAX_ADDR, fsz);   /* track max (skip startup keyframe only) */
        if (advanced) wr(CUMB_ADDR, rd(CUMB_ADDR) + fsz);           /* cumulative encoded bytes (once/new frame) */
        /* NO runtime Qp write any more. The real min-Qp floor is now a BUILD-TIME patch of the RC's own
         * clamp constant (0x80241074/78, `max(cap,10)` -> `max(cap,<floor>)`), which bounds the Qp the RC
         * itself computes and stores. Device-proven dead ends: writing ctx+0x94 (the RC overwrites it each
         * frame) and raising the seed bytes ctx+0x91/92/93 (they are the `cap`, an UPPER bound via min()). */
    } else {
        MENU[26] = 0;          /* armed for the next recording's rising edge */
        /* NOTE: deliberately NOT resetting QPMIN/SZMAX/CUMB/QPRAW here any more -- they persist through
         * preview so a post-mortem (auto-stop, stall, freeze) can still be read off the screen.
         * Previously this wiped the evidence the instant we dropped into preview. */
#if 0
        wr(QPMIN_ADDR, 0u);    /* not recording -> reset the min tracker to the UNSET sentinel 0
                                * (0 is never a valid Qp; the OSD shows "-" until tracking starts,
                                * which is clearer than a number that looks like a real reading). */
        wr(SZMAX_ADDR, 0u);    /* reset max frame size for the next recording */
        wr(CUMB_ADDR, 0u);     /* reset cumulative bytes for the next recording */
        wr(QPRAW_ADDR, 0u);    /* reset raw-Qp readout */
        wr(SEED_ADDR, 0u);     /* reset seed readout */
#endif
    }
    load_wb_once();                      /* restore persisted WB at first frame after boot */
    load_date_once();                    /* restore + apply persisted date via D setdate */
    load_fps_once();                     /* restore persisted FPS choice */
    load_ast_once();                     /* restore persisted reel-end auto-stop choice */
    load_qpf_once();                     /* restore persisted Qp floor (default OFF) */
    load_br_once();                      /* restore persisted bitrate multiplier */
    { uint32_t f = MENU[20]; if (f < QPF_MIN || f > QPF_MAX) f = QPF_STOCK; wr(QPFLOOR_ADDR, f); }
                                         /* OFF -> write the stock 10 so the stub is inert */
    { uint32_t b = MENU[24]; if (!BR_OK(b)) b = BR_DEF; wr(BR_VAL_ADDR, b); }
                                         /* feed the bitrate stub (read at the next record-init) */
                                         /* feed the RC min-Qp stub (live QpF menu value) */
    wr(FPS_VAL_ADDR, (MENU[17] == 16) ? 48000u : 54000u);   /* feed the framerate stub (N=fps*3000) */
    handle_buttons(recording);           /* read input before stock WB (recording-only) */
    int r = orig(a0, a1, a2, a3);        /* stock WB writes fixed gains */
    if (WB_OVERRIDE) apply_wb();         /* BISECT: gated -- see WB_OVERRIDE above */
    uint32_t ho = rd(0x80e08df0u);
    if (OSD_DRAW_WHILE_REC || !recording) {   /* FREEZE A/B: no painting at all while recording */
        if (ho) { uint32_t pool = resolve(ho);
                  if (pool >= 0x80000000u && pool < 0xb0000000u) {
#if DIAG_ERASE
                      { uint32_t so = rd(SENT_ADDR);   /* wiped since our last draw, and nobody caught it? */
                        if (recording && so && ((volatile uint8_t *)pool)[so - 1u] != CYN)
                            wr(CNT_E_ADDR, rd(CNT_E_ADDR) + 1u); }
#endif
                      rend_p0((volatile uint8_t *)pool, 1);
                      draw_version((volatile uint8_t *)pool);   /* same pass -> same buffer as the rest
                                                                 * of the OSD, so it cannot flicker out
                                                                 * of phase with it */
                  } }
    }
    return r;
}

/* --- stall warning (top-right border) --- */
#ifndef FW_BUILD
#define FW_BUILD 0       /* set by build.ps1 from -Build, so it can never go stale */
#endif
#define FW_Y 14          /* build tag sits TWO rows ABOVE the warning box (WY 40), so warnings never
                          * overwrite it and it stays readable in every photo, warning or not. */
#define WX 766
#define WY 40
static void draw_warn(volatile uint8_t *p0) {
    cclr(p0, WX - 2, WY - 2, 98, 43, 0);
    cstr(p0, WX, WY, "! REC STALLED", RED);
    cstr(p0, WX, WY + 13, "NO NEW FRAMES", RED);
    cstr(p0, WX, WY + 26, "STOP REC NOW", RED);   /* advice: abort the recording manually */
}
static void clear_warn(volatile uint8_t *p0) { cclr(p0, WX - 2, WY - 2, 98, 43, 0); }
/* Build tag in the top-right corner -- the same box the warnings use, which is otherwise dead space.
 * Costs no rows in the main OSD list and IS visible while recording, which is the point: a HARD FREEZE
 * paints no warning, so the tag stays on screen exactly when someone photographs a frozen device. Any
 * real warning simply overwrites it (warnings matter more, and they are self-identifying anyway).
 * Colour-independent by design -- the "FW" label carries the meaning, not the colour. */
static void draw_version(volatile uint8_t *p0) {
    int X;
    cclr(p0, WX - 2, FW_Y - 2, 98, DIAG_ERASE ? 28 : 15, 0);   /* 2 lines while diagnosing */
    X = cstr(p0, WX, FW_Y, "FW ", YEL);   /* YEL = informative (same as "WB gains"); RED is reserved
                                           * for warnings, so the tag must not read as an alert. */
    X = cnum(p0, X, FW_Y, FW_BUILD, YEL);
#if DIAG_ERASE
    X = cstr(p0, X, FW_Y, " G", YEL); cnum(p0, X, FW_Y, (int)(rd(GAPMAX_ADDR) % 100u), YEL);
    X = cstr(p0, WX, FW_Y + 13, "T", YEL);  X = cnum(p0, X, FW_Y + 13, (int)(rd(CNT_T_ADDR) % 100u), YEL);
    X = cstr(p0, X, FW_Y + 13, " E", YEL);  X = cnum(p0, X, FW_Y + 13, (int)(rd(CNT_E_ADDR) % 100u), YEL);
    X = cstr(p0, X, FW_Y + 13, " C", YEL);  cnum(p0, X, FW_Y + 13, (int)(rd(CNT_C_ADDR) % 100u), YEL);
#endif
}
/* reel-end (blank-frame) warning -- shown before the auto-stop fires */
static void draw_blank_warn(volatile uint8_t *p0, uint32_t n) {
    int X;
    cclr(p0, WX - 2, WY - 2, 98, 43, 0);
    cstr(p0, WX, WY, "BLANK FRAMES", RED);
    X = cstr(p0, WX, WY + 13, "BLK ", RED);      /* live blank-run count: how close to the auto-stop */
    X = cnum(p0, X, WY + 13, (int)n, RED);
    cstr(p0, X, WY + 13, "/20", RED);
    cstr(p0, WX, WY + 26, "STOPPING REC", RED);
}
/* Painted IMMEDIATELY BEFORE FULL_STOP. FULL_STOP's encoder-finalise can BLOCK the OnTimer/UI task --
 * builds 217/218 hung the device exactly that way on the card-stall path (hence the build-220 revert to
 * the non-blocking motor lever); the reel-end path still calls it. A successful stop overwrites this with
 * the "AUTO-STOPPED / FILE SAVED" message, so **"STOP CALLED!" left frozen on screen == hung inside
 * FULL_STOP**, whereas a frozen "BLK n/20" means we never reached the call (ordinary freeze). */
static void draw_stop_called(volatile uint8_t *p0, uint32_t reason) {
    cclr(p0, WX - 2, WY - 2, 98, 43, 0);
    cstr(p0, WX, WY, (reason == 2u) ? "REC STALLED" : "BLANK FRAMES", RED);
    cstr(p0, WX, WY + 13, "STOP CALLED!", RED);
    cstr(p0, WX, WY + 26, "FINALISING..", RED);
}
/* persistent post-stop message (preview) -- what an away user sees on return. reason: 1=reel end, 2=stall */
static void draw_reelend_done(volatile uint8_t *p0, uint32_t reason) {
    cclr(p0, WX - 2, WY - 2, 98, 43, 0);
    cstr(p0, WX, WY, (reason == 2u) ? "REC STALLED" : "REEL ENDED", RED);
    cstr(p0, WX, WY + 13, "AUTO-STOPPED", RED);
    cstr(p0, WX, WY + 26, "FILE SAVED", RED);
}
/* THE clean full-stop (build 216, confirmed): stops encoder + saves file + stops motor (0x114a88(26))
 * + St=8 (preview) + UI teardown -- exactly what a real OK/Back does, landing back in preview. */
#define FULL_STOP() (((void (*)(void))0x801203a0u)())

/* Card-stall warning. Fires when the encoded-frame counter freezes while the encoder ctx is valid.
 * TWO ways to reach a frozen counter: (1) a real card stall mid-recording (motor running, NO button
 * pressed), or (2) the user STOPPED it (OK/Back) and it finalized into preview. We discriminate on
 * the button, not on a record-active flag: OK(0x800)/Back(0x20) in recording ALWAYS mean "stop"
 * (our WB menu only uses arrows/±), so seeing one marks the following freeze as a clean exit and we
 * suppress the warning. The marker clears when the next recording advances frames. This kills the
 * old "REC STALLED bleeds into preview" false positive without any RE of the record-active flag. */
/* COMPOSE-BLIT REPAINT (build 268). Build 267 showed the REC-dot wipe does NOT happen inside the stock
 * OnTimer (E >> T). Next candidate: the UI's canvas->O1 blit, OSD_Compose 0x8010bc38 -> blit 0x8010c164,
 * called from exactly one jal @0x8010bc70 (fires ~12x/s while recording). We redirect that jal to
 * wrap_compose, which runs the real blit and then this. Tried in July (build 134) while we were drawing to
 * the WRONG buffer (pool+0x65400); never tested against the steady buffer we use now. Repaint only when the
 * sentinel shows a wipe, so the extra drawing is rare. C counts catches here. */
__asm__(
    ".set noreorder\n"
    ".globl wrap_compose\n"
    "wrap_compose:\n"
    "  addiu $sp,$sp,-16\n"
    "  sw    $ra,0($sp)\n"
    "  lui   $t9,0x8010\n"
    "  ori   $t9,$t9,0xc164\n"
    "  jalr  $t9\n"   /* the real canvas->O1 blit (a0-a3 untouched) */
    "  nop\n"
    "  sw    $v0,4($sp)\n"   /* keep the blit's return value -- July's version lost it */
    "  jal   osd_after\n"
    "  nop\n"
    "  lw    $v0,4($sp)\n"
    "  lw    $ra,0($sp)\n"
    "  addiu $sp,$sp,16\n"
    "  jr    $ra\n"
    "  nop\n"
    ".set reorder\n"
);
void osd_after(void) {
    if (*(volatile uint8_t *)0x80e0a624u != 10) return;          /* recording only */
    uint32_t ho = rd(0x80e08df0u); if (!ho) return;
    uint32_t pool = resolve(ho); if (pool < 0x80000000u || pool >= 0xb0000000u) return;
    volatile uint8_t *p0 = (volatile uint8_t *)pool;
    uint32_t so = rd(SENT_ADDR);
    if (so && p0[so - 1u] != CYN) {
#if DIAG_ERASE
        wr(CNT_C_ADDR, rd(CNT_C_ADDR) + 1u);
#endif
        rend_p0(p0, 0); draw_version(p0);   /* THE FIX: repaint straight after the blit that wiped us */
    }
}
void after_ontimer(void) {
    volatile uint8_t *p0 = 0;
    uint32_t ho = rd(0x80e08df0u);
    if (ho) { uint32_t pool = resolve(ho); if (pool >= 0x80000000u && pool < 0xb0000000u) p0 = (volatile uint8_t *)pool; }
    /* record-state flag: movie state 0x80e0a624 == 10 = actively recording, 8 = preview. This is the
     * true record flag (replaces the old ctx-lingers + button clean-exit hack) -> warning only in rec. */
    int recording = (*(volatile uint8_t *)0x80e0a624u == 10);
    uint32_t ctx = rd(0x80e974dcu);
    int ctxok = (ctx >= 0x80000000u && ctx < 0xb0000000u);
    int show_stall = 0, show_blank = 0;
    if (recording && ctxok) {
        wr(BLK_DONE, 0);   /* active recording -> clear any stale "REEL ENDED" message */
        uint32_t frm = *(volatile uint32_t *)(ctx + 0xd4u);
#if DIAG_ERASE
        wr(TICKS_ADDR, rd(TICKS_ADDR) + 1u);                     /* margin diag: ticks this recording */
#endif
        if (frm != ST[0]) {   /* NEW encoded frame: update reel-end detector + reset stall counter */
#if DIAG_ERASE
            if (frm > 10u && ST[1] > rd(GAPMAX_ADDR)) wr(GAPMAX_ADDR, ST[1]);   /* biggest recovered gap */
#endif
            ST[0] = frm; ST[1] = 0; ST[4] = 0;
            uint32_t exp = rd(EXP_ADDR);
            if (exp >= ARM_EXP) { wr(BLK_ARMED, 1); wr(BLK_COUNT, 0); }              /* real footage */
            else if (exp < BLANK_EXP) { if (rd(BLK_ARMED)) wr(BLK_COUNT, rd(BLK_COUNT) + 1); }  /* blank */
            else wr(BLK_COUNT, 0);                                                   /* mid-range: reset run */
        } else {              /* CARD-STALL: counter frozen. Warn (CARD SLOW) at STALL_THRESH, then clean
                               * FULL_STOP at MOTOR_STOP_THRESH. A soft stall = card ALIVE but slow, so once
                               * we stop feeding frames the write queue drains and the finalize succeeds ->
                               * clean saved file + usable device (vs the old motor lever = power-cycle). The
                               * separate HARD freeze is an encoder crash, not this path (that one has NO
                               * warning + kills OnTimer, so it never gets here). BLK_DONE=2 = stall message. */
            ST[1]++;
            if (ST[1] >= STALL_THRESH) show_stall = 1;
            if (ST[1] >= MOTOR_STOP_THRESH && !ST[4]) {
                ST[4] = 1; wr(BLK_DONE, 2);
                if (p0) draw_stop_called(p0, 2u);   /* mark BEFORE the call (it can block) */
                FULL_STOP(); return;
            }
        }
        /* reel-end warning + auto-stop (persistent count, checked every tick so the warning is steady).
         * Gated on the auto-stop menu toggle (MENU[22]): OFF = no reel-end warning + no auto-stop, for
         * faded/thin film whose bright, low-exposure frames look like "no film" to the detector. The
         * CARD-STALL path above stays ALWAYS on (it keys off the write buffer, not exposure, so faded
         * film never trips it, and it's a genuine crash/overrun safety stop). */
        if (MENU[22] && rd(BLK_ARMED) && rd(BLK_COUNT) >= BLANK_WARN) show_blank = 1;
        if (MENU[22] && rd(BLK_ARMED) && rd(BLK_COUNT) >= BLANK_STOP && !ST[6]) {
            ST[6] = 1; ST[7]++; wr(BLK_DONE, 1);
            if (p0) draw_stop_called(p0, 1u);   /* mark BEFORE the call (it can block) */
            FULL_STOP(); return;                /* BLK_DONE persists the result message in preview */
        }
    } else {
        ST[0] = 0xFFFFFFFFu; ST[1] = 0; ST[4] = 0; ST[6] = 0;
        wr(BLK_ARMED, 0); wr(BLK_COUNT, 0);   /* fresh detector each recording (disarmed until footage) */
    }
    /* FREEZE A/B: zero drawing while recording. The auto-stop LOGIC above still runs -- reel-end and
     * card-stall still fire FULL_STOP and save the file -- only the on-screen warning is suppressed.
     * The post-stop "REEL ENDED / CARD STALLED" message still paints in preview, where it matters. */
    if (!OSD_DRAW_WHILE_REC && recording) return;
    if (show_blank) { if (p0) draw_blank_warn(p0, rd(BLK_COUNT)); ST[3] = 1; }
    else if (show_stall) { if (p0) draw_warn(p0); ST[3] = 1; }
    else if (!recording && rd(BLK_DONE)) { if (p0) draw_reelend_done(p0, rd(BLK_DONE)); ST[3] = 1; }   /* persist post-stop */
    else if (ST[3]) { if (p0) clear_warn(p0); ST[3] = 0; }
    /* ERASE CATCHER: we run straight after the stock OnTimer (which blinks the REC icon). If it just
     * wiped our OSD, repaint NOW instead of waiting up to a frame for osd_main. No clear -- the area is
     * already blank, and clearing could cut into a concurrent osd_main draw. */
    /* SECOND wipe source -- keep this repaint in the release. Build 268 counted ~1 wipe/min landing INSIDE
     * the stock OnTimer (T13 vs E1 over ~1500 frames: OnTimer at ~2.5 Hz beat osd_main at 30 Hz nearly every
     * time, which is only possible if the wipe happens synchronously in the handler we run straight after).
     * The compose-blit repaint never sees these. Without this they return as a ~once-a-minute blink. */
    if (recording && p0) {
        uint32_t so = rd(SENT_ADDR);
        if (so && p0[so - 1u] != CYN) {
#if DIAG_ERASE
            wr(CNT_T_ADDR, rd(CNT_T_ADDR) + 1u);
#endif
            rend_p0(p0, 0); draw_version(p0);
        }
    }
}

__asm__(
    ".set noreorder\n"
    ".globl my_ontimer\n"
    "my_ontimer:\n"
    "  addiu $sp,$sp,-8\n"
    "  sw    $ra,4($sp)\n"
    "  jal   ontimer_tramp\n"
    "  nop\n"
    "  sw    $v0,0($sp)\n"
    "  jal   after_ontimer\n"
    "  nop\n"
    "  lw    $v0,0($sp)\n"
    "  lw    $ra,4($sp)\n"
    "  addiu $sp,$sp,8\n"
    "  jr    $ra\n"
    "  nop\n"
    "ontimer_tramp:\n"
    "  addiu $sp,$sp,-0xF0\n"
    "  sw    $30,0xe8($sp)\n"
    "  j     0x8011e34c\n" /* direct j: never jump via $k0/$k1 */
    "  nop\n"
    ".set reorder\n"
);
