/* RAM-only image: picotool loads it into SRAM and the bootrom boots it. */
MEMORY
{
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
  SCRATCH (rwx) : ORIGIN = 0x20080000, LENGTH = 8K
}

ENTRY(reset_handler)

SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    KEEP(*(.embedded_block))
    *(.text*)
    *(.rodata*)
  } > RAM

  .data : { *(.data*) } > RAM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }

  __stack_top = ORIGIN(SCRATCH) + LENGTH(SCRATCH);
}
