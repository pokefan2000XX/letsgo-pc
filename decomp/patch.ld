/* The decomp patch module: decompiled code at a fixed guest address after main's .bss (main
   ends at 0x7101EF0000), within BL range of every original function in main. Linked statically:
   no relocations at run time. decomp/tools/build.py adds the symbols for original code. */
SECTIONS {
    . = 0x7102000000;
    .text : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data : { *(.data .data.* .data.rel.ro .data.rel.ro.*) }
    .bss : { *(.bss .bss.* COMMON) }
    .init_array : { *(.init_array .init_array.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
