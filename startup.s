/* SPDX-License-Identifier: Apache-2.0
 * Cold SD boot only, MMU / D-cache off at firmware handoff.
 */
.syntax unified
.cpu arm1176jzf-s
.arm
.section .text.boot,"ax",%progbits
.balign 4
.global _start
.type _start,%function
_start:
    cpsid aif
    cps #0x13
    setend le
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #(1 << 0)
    bic r0, r0, #(1 << 1)
    bic r0, r0, #(1 << 2)
    bic r0, r0, #(1 << 11)
    bic r0, r0, #(1 << 12)
    bic r0, r0, #(1 << 13)
    bic r0, r0, #(1 << 25)
    orr r0, r0, #(1 << 22) /* ARMv6 U: packed network headers may be unaligned. */
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c12, c0, 0
    mcr p15, 0, r0, c7, c10, 4
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c8, c7, 0
    mcr p15, 0, r0, c7, c5, 4

    /* Dedicated exception stacks. All C entry stack tops are 8-byte aligned. */
    cps #0x12
    ldr sp, =__irq_stack_top
    cps #0x17
    ldr sp, =__abt_stack_top
    cps #0x1b
    ldr sp, =__und_stack_top
    cps #0x11
    ldr sp, =__fiq_stack_top
    cps #0x13
    ldr sp, =__svc_stack_top

    ldr r0, =vector_template
    mov r1, #0
    ldr r2, =vector_template_end
1:
    cmp r0, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 1b
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c7, c5, 4

    ldr r0, =__data_load
    ldr r1, =__data_start
    ldr r2, =__data_end
    cmp r0, r1
    beq 3f
2:
    cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 2b
3:
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
4:
    cmp r0, r1
    strlo r2, [r0], #4
    blo 4b
    bl main
    b .
.ltorg
.size _start,.-_start

.balign 4
.global vector_template,vector_template_end
vector_template:
    .rept 8
    ldr pc, [pc, #24]
    .endr
    .word _start, fatal_undefined, port_svc_handler, fatal_prefetch
    .word fatal_data, fatal_reserved, port_irq_handler, fatal_fiq
vector_template_end:

/* Fatal-only paths: preserve the exception LR/SPSR for UART diagnosis.
 * No exception return, no kernel services and no mutex after a fatal fault.
 */
.macro fatal_handler name,number
.type \name,%function
\name:
    cpsid aif
    mov r1, lr
    mrs r2, spsr
    mov r0, #\number
    b port_fatal_exception
.size \name,.-\name
.endm
fatal_handler fatal_undefined,1
fatal_handler fatal_prefetch,3
fatal_handler fatal_data,4
fatal_handler fatal_reserved,5
fatal_handler fatal_fiq,7
.section .note.GNU-stack,"",%progbits
