/* RAM_BASE and RAM_SIZE come from the Makefile (--defsym). The vector table
 * (the stack pointer and the reset address) is the first thing in the image. */
MEMORY { RAM (rwx) : ORIGIN = RAM_BASE, LENGTH = RAM_SIZE }
SECTIONS
{
  .vectors : { KEEP(*(.vectors)) } > RAM
  .text : { *(.text*) *(.rodata*) } > RAM
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
