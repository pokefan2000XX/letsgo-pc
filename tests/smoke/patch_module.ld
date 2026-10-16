/* Patch module for decompiled code: placed after main's .bss (main spans
   0x7100000000-0x7101EE0000), within BL range of every original function. */
ENTRY(patch_entry)
nnMain = 0x7100004110;

SECTIONS {
    . = 0x7102000000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.* .data.rel.ro .data.rel.ro.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
