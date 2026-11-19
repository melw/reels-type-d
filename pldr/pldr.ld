/* Loaded at 0x80000000 by the loader, linked for RELOC = 0x87000000 (entry.S copies it there).
 * 0x000-0x0ff: header written by make_pldr.py (entry word, NT96658 id, size, checksum; 0x80-0xa7 handoff block).
 * RAM is 128 MB; the loader body lives at 0x87f05000, stage 1 at 0x87f00000. */
ENTRY(_entry)
SECTIONS
{
    _reloc_start = 0x87000000;
    .text 0x87000100 : {
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    }
    _image_end = .;
    .bss : { *(.bss*) *(.sbss*) *(COMMON) }
    _image_size = _image_end - _reloc_start;
    _stack_top = 0x87e00000;
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
