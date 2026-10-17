/* Real-time battle engine for Pokemon Liquid Crystal (BPRE 1.0 binary hack): code and data at 0x09000000,
   RAM in the 4 KB cut from the top of gHeap (InitHeap is hooked to leave it out). */
ENTRY(RtLc_Entry)
SECTIONS
{
    . = 0x09000000;
    .text : { KEEP(*(.rtlc_header)) *(.text*) *(.glue_7) *(.glue_7t) }
    .rodata : ALIGN(4) { *(.rodata*) }
    .data : ALIGN(4) { *(.data*) }
    __rom_end = .;
    . = 0x0201B000;
    .ram (NOLOAD) : ALIGN(4) { __ram_start = .; *(ewram_data) *(.bss*) *(.sbss*) *(COMMON) __ram_end = .; }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) }
}
ASSERT(__ram_end <= 0x0201C000, "RT RAM does not fit in the 4 KB block")
ASSERT(SIZEOF(.data) == 0, "initialized RAM data is not supported")
INCLUDE syms.ld
