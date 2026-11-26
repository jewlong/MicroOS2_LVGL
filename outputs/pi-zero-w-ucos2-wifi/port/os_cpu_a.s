/* SPDX-License-Identifier: Apache-2.0
 * ARMv6 frame at OSTCBStkPtr (64 bytes, descending System-mode task stack):
 *   +00 r0 ... +48 r12, +52 lr_sys, +56 pc, +60 cpsr.
 * SRS stores LR_exception/SPSR_exception, RFE restores PC/CPSR atomically.
 * IRQ LR is corrected by 4; SVC LR already points past the SVC instruction.
 */
.syntax unified
.cpu arm1176jzf-s
.arm
.section .text.port,"ax",%progbits
.balign 4

.global OS_CPU_SR_Save
.type OS_CPU_SR_Save,%function
OS_CPU_SR_Save:
    mrs r0, cpsr
    cpsid if
    bx lr
.size OS_CPU_SR_Save,.-OS_CPU_SR_Save

.global OS_CPU_SR_Restore
.type OS_CPU_SR_Restore,%function
OS_CPU_SR_Restore:
    msr cpsr_c, r0
    bx lr
.size OS_CPU_SR_Restore,.-OS_CPU_SR_Restore

.global OSCtxSw
.type OSCtxSw,%function
OSCtxSw:
    svc #0
    bx lr
.size OSCtxSw,.-OSCtxSw

.global port_svc_handler
.type port_svc_handler,%function
port_svc_handler:
    cpsid if
    srsdb sp!, #0x1f           /* Save exception return pair on SYS stack. */
    cps #0x1f
    push {r0-r12,lr}
    ldr r0, =OSTCBCur
    ldr r0, [r0]
    str sp, [r0]              /* OS_TCB's FIRST field must be OSTCBStkPtr. */
    cps #0x13                 /* C calls use the dedicated aligned SVC stack. */
    ldr r0, =port_svc_switches
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    bl port_select_next
    b port_restore_task
.size port_svc_handler,.-port_svc_handler

.global port_irq_handler
.type port_irq_handler,%function
port_irq_handler:
    sub lr, lr, #4
    srsdb sp!, #0x1f
    cps #0x1f
    push {r0-r12,lr}
    ldr r0, =OSTCBCur
    ldr r0, [r0]
    str sp, [r0]
    cps #0x12                 /* IRQ nesting is never enabled. */
    bl bsp_irq_dispatch
    b port_restore_task       /* No switch: resume saved current task. */
.size port_irq_handler,.-port_irq_handler

.global OSIntCtxSw
.type OSIntCtxSw,%function
OSIntCtxSw:
    /* OSIntExit called this in IRQ mode. Discard its C call chain. */
    ldr r0, =port_irq_switches
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    bl port_select_next
    ldr sp, =__irq_stack_top
    b port_restore_task
.size OSIntCtxSw,.-OSIntCtxSw

.global OSStartHighRdy
.type OSStartHighRdy,%function
OSStartHighRdy:
    cpsid if
    ldr sp, =__svc_stack_top   /* Abandon the boot main/OSStart call chain. */
    bl OSTaskSwHook
    ldr r0, =OSRunning
    mov r1, #1
    strb r1, [r0]
    b port_restore_task
.size OSStartHighRdy,.-OSStartHighRdy

.global port_restore_task
.type port_restore_task,%function
port_restore_task:
    /* This path has no C calls and needs no scratch registers after the pop. */
    cps #0x1f
    ldr r0, =OSTCBCur
    ldr r0, [r0]
    ldr sp, [r0]
    pop {r0-r12,lr}
    rfeia sp!
.size port_restore_task,.-port_restore_task
.ltorg
.section .note.GNU-stack,"",%progbits
