OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    ram (rwx) : ORIGIN = 0x00010000, LENGTH = 0x03FF0000
}

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > ram

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > ram

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > ram

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } > ram

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
    }
}
