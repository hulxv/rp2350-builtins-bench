// Bare-metal startup for an RP2350 (Cortex-M33, Secure) image that runs
// entirely from SRAM. No pico-sdk: the SDK swaps in its own float routines
// for __aeabi_*, which would defeat the point of benchmarking compiler-rt.

    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .vectors, "ax"
    .p2align 7
    .global __vectors
__vectors:
    .word __stack_top
    .word reset_handler
    .word fault_handler     // NMI
    .word fault_handler     // HardFault
    .word fault_handler     // MemManage
    .word fault_handler     // BusFault
    .word fault_handler     // UsageFault
    .word fault_handler     // SecureFault
    .word 0, 0, 0
    .word fault_handler     // SVCall
    .word fault_handler     // DebugMonitor
    .word 0
    .word fault_handler     // PendSV
    .word fault_handler     // SysTick

// IMAGE_DEF block (see pico-sdk embedded_start_block.inc.S). The bootrom
// looks for it in the first 4K of the image; for a RAM image it takes SP/PC
// from the VECTOR_TABLE item.
    .section .embedded_block, "a"
    .p2align 2
embedded_block:
    .word 0xffffded3                // PICOBIN_BLOCK_MARKER_START
    .byte 0x42, 0x01                // IMAGE_TYPE, 1 word
    .hword 0x1021                   // EXE | SECURITY_S | CPU_ARM | CHIP_RP2350
    .byte 0x03, 0x02                // VECTOR_TABLE, 2 words
    .hword 0
    .word __vectors
    .byte 0xff                      // LAST
    .hword (embedded_block_end - embedded_block - 16) / 4
    .byte 0
    .word 0                         // single-block loop
    .word 0xab123579                // PICOBIN_BLOCK_MARKER_END
embedded_block_end:

    .text
    .thumb_func
    .global reset_handler
reset_handler:
    cpsid i
    ldr r0, =0xe000ed08             // VTOR
    ldr r1, =__vectors
    str r1, [r0]
    ldr r0, =__stack_top
    msr msp, r0
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    movs r2, #0
1:  cmp r0, r1
    bhs 2f
    str r2, [r0], #4
    b 1b
2:  bl main
3:  b 3b

    .thumb_func
    .global fault_handler
fault_handler:
    mrs r0, ipsr
    mov r1, lr
    ldr r2, =__stack_top
    msr msp, r2
    bl bench_fault
4:  b 4b

// Loop-overhead reference: returns its argument(s) untouched, so it has the
// same call/return shape as every builtin signature the harness uses.
    .thumb_func
    .global bench_nop
bench_nop:
    bx lr
