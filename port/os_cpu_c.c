/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <ucos_ii.h>
#include "bsp.h"
#include "port.h"

volatile uint32_t port_irq_switches;
volatile uint32_t port_svc_switches;
volatile uint32_t port_task_switches;

_Static_assert(sizeof(OS_STK) == 4, "port requires 32-bit stack words");
_Static_assert(sizeof(void *) == 4, "port requires 32-bit pointers");
_Static_assert(offsetof(OS_TCB, OSTCBStkPtr) == 0, "assembly TCB offset mismatch");

OS_STK *OSTaskStkInit(void (*task)(void *), void *arg, OS_STK *ptos, INT16U opt)
{
    /* ptos points to the highest usable WORD, not one past the allocation. */
    uintptr_t top = ((uintptr_t)(ptos + 1)) & ~(uintptr_t)7u;
    OS_STK *frame = (OS_STK *)(top - 16u * sizeof(OS_STK));
    (void)opt;
    for (uint32_t i = 0; i < 13u; ++i) {
        frame[i] = UINT32_C(0x10101010) * i;
    }
    frame[0] = (OS_STK)(uintptr_t)arg;
    frame[13] = (OS_STK)(uintptr_t)port_task_return;
    frame[14] = (OS_STK)(uintptr_t)task;
    frame[15] = 0x15fu; /* SYS, ARM, little endian; A/F masked, IRQ enabled. */
    return frame;
}

void port_select_next(void)
{
    OSTaskSwHook();
    OSPrioCur = OSPrioHighRdy;
    OSTCBCur = OSTCBHighRdy;
}

void port_task_return(void)
{
    bsp_panic("task returned");
}

void OSInitHookBegin(void) { }
void OSInitHookEnd(void) { }
void OSTaskCreateHook(OS_TCB *tcb) { (void)tcb; }
void OSTaskDelHook(OS_TCB *tcb) { (void)tcb; }
void OSTaskIdleHook(void) { __asm__ volatile ("nop"); }
void OSTaskReturnHook(OS_TCB *tcb) { (void)tcb; bsp_panic("task returned"); }
void OSTaskStatHook(void) { }
void OSTCBInitHook(OS_TCB *tcb) { (void)tcb; }
void OSTaskSwHook(void) { ++port_task_switches; }
