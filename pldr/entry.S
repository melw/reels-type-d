/* Type probe / patch-loader entry. The loader reads FW96658T.BIN (raw) to 0x80000000, writes its handoff block at
 * 0x80000080, and stage 1 jumps to the word at 0x80000000 (= 0x80000100, this code). Linked for RELOC (0x87000000):
 * copy the whole image (with the handoff block) there, then flush the caches with our own loops (the same
 * index operations and range as the Type D stage 1) so that nothing in the bootloader is called before
 * pldr_main has checked that it is the Type D bootloader. */
    .set noreorder
    .set mips32r2
    .section .text.entry, "ax"
    .globl _entry
_entry:
    lui   $t0, 0x8000            /* src = load address */
    lui   $t1, %hi(_reloc_start) /* dst = link address */
    addiu $t1, $t1, %lo(_reloc_start)
    lui   $t2, %hi(_image_size)
    addiu $t2, $t2, %lo(_image_size)
    addu  $t2, $t2, $t0          /* src end */
1:  lw    $t3, 0($t0)
    sw    $t3, 0($t1)
    addiu $t0, $t0, 4
    bne   $t0, $t2, 1b
    addiu $t1, $t1, 4
    lui   $sp, %hi(_stack_top)
    addiu $sp, $sp, %lo(_stack_top)
    lui   $t0, 0x8000            /* D-cache: index write-back invalidate, 16 KB in 32-byte lines */
    ori   $t1, $t0, 0x4000
2:  cache 1, 0($t0)
    addiu $t0, $t0, 0x20
    bne   $t0, $t1, 2b
    nop
    sync
    lui   $t0, 0x8000            /* I-cache: index invalidate, same range */
    ori   $t1, $t0, 0x4000
3:  cache 0, 0($t0)
    addiu $t0, $t0, 0x20
    bne   $t0, $t1, 3b
    nop
    sync
    lui   $t9, %hi(pldr_main)    /* continue at the relocated copy, with an instruction hazard barrier */
    addiu $t9, $t9, %lo(pldr_main)
    jr.hb $t9
    nop
