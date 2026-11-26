OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x8000, LENGTH = 0xff8000 }
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS
{
    . = ORIGIN(RAM);
    __image_start = .;
    .text : ALIGN(4) { KEEP(*(.text.boot)) *(.text .text.*) } > RAM :code
    .rodata : ALIGN(4) { *(.rodata .rodata.*) . = ALIGN(4); } > RAM :code
    .data : ALIGN(8)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM :data
    __data_load = LOADADDR(.data);
    __image_end = .;
    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM :data
    .stacks (NOLOAD) : ALIGN(8)
    {
        __svc_stack_bottom = .; . += 0x2000; __svc_stack_top = .;
        __irq_stack_bottom = .; . += 0x2000; __irq_stack_top = .;
        __abt_stack_bottom = .; . += 0x0800; __abt_stack_top = .;
        __und_stack_bottom = .; . += 0x0800; __und_stack_top = .;
        __fiq_stack_bottom = .; . += 0x0800; __fiq_stack_top = .;
    } > RAM :data
    __end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx* .ARM.extab*) *(.eh_frame*) }
    ASSERT(_start == 0x8000, "entry must match config.txt")
    ASSERT((__svc_stack_top & 7) == 0, "SVC stack alignment")
    ASSERT((__irq_stack_top & 7) == 0, "IRQ stack alignment")
    ASSERT(((__bss_end - __bss_start) & 3) == 0, "BSS word alignment")
    ASSERT(__end <= ORIGIN(RAM) + LENGTH(RAM), "RAM overflow")
}
