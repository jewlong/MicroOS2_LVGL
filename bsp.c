/* SPDX-License-Identifier: Apache-2.0 */
#include <ucos_ii.h>
#include "platform.h"
#include "uart.h"
#include "bsp.h"
#include "net_platform.h"
#include "port.h"

#define IRQ_BASE       (PERIPHERAL_BASE + 0xb200u)
#define IRQ_PENDING1   (IRQ_BASE + 0x04u)
#define IRQ_FIQ_CTRL   (IRQ_BASE + 0x0cu)
#define IRQ_ENABLE1    (IRQ_BASE + 0x10u)
#define IRQ_DISABLE1   (IRQ_BASE + 0x1cu)
#define IRQ_DISABLE2   (IRQ_BASE + 0x20u)
#define IRQ_DISABLE_BASIC (IRQ_BASE + 0x24u)
#define ST_CS          (PERIPHERAL_BASE + 0x3000u)
#define ST_C1          (PERIPHERAL_BASE + 0x3010u)
#define ST_MATCH1      (1u << 1)
#define TICK_US        (1000000u / OS_TICKS_PER_SEC)

volatile uint32_t bsp_irq_count;
volatile uint32_t bsp_late_ticks;
static uint32_t next_tick;
static OS_EVENT *log_mutex;

_Static_assert(OS_TICKS_PER_SEC == 100u, "stage 2 uses 100 Hz ticks");

void bsp_interrupts_init(void)
{
    mmio_write(IRQ_DISABLE1, UINT32_MAX);
    mmio_write(IRQ_DISABLE2, UINT32_MAX);
    mmio_write(IRQ_DISABLE_BASIC, UINT32_MAX);
    mmio_write(IRQ_FIQ_CTRL, 0u);
    mmio_write(ST_CS, ST_MATCH1); /* Never touch GPU-owned channels 0/2. */
}

void bsp_tick_start(void)
{
    next_tick = timer_us() + TICK_US;
    mmio_write(ST_CS, ST_MATCH1);
    mmio_write(ST_C1, next_tick);
    mmio_write(IRQ_ENABLE1, ST_MATCH1);
}

void bsp_irq_dispatch(void)
{
    uint32_t elapsed_ticks = 0u;
    uint32_t now;
    if ((mmio_read(IRQ_PENDING1) & ST_MATCH1) == 0u) {
        bsp_panic("unexpected IRQ source");
    }
    if ((OSRunning != OS_TRUE) || (OSIntNesting != 0u)) {
        bsp_panic("IRQ outside running task / nested IRQ");
    }
    ++bsp_irq_count;
    OSIntEnter();

    /* Absolute deadlines avoid cumulative ISR drift. If a deadline was missed,
     * account for elapsed ticks and re-arm in the future, not after a 71m wrap.
     * Signed delta is valid for latencies below 2^31 microseconds.
     */
    do {
        mmio_write(ST_CS, ST_MATCH1);
        now = timer_us();
        while ((int32_t)(now - next_tick) >= 0) {
            next_tick += TICK_US;
            ++elapsed_ticks;
            if (elapsed_ticks > 100u) {
                bsp_panic("IRQ delayed more than one second");
            }
        }
        mmio_write(ST_C1, next_tick);
    } while ((int32_t)(timer_us() - next_tick) >= 0);

    if (elapsed_ticks == 0u) {
        bsp_panic("timer IRQ before deadline");
    }
    bsp_late_ticks += elapsed_ticks - 1u;
    for (uint32_t i = 0; i < elapsed_ticks; ++i) {
        OSTimeTick();
        wifi_sdio_poll_irq();
    }
    OSIntExit(); /* May call OSIntCtxSw and never return to this IRQ stack. */
}

void bsp_log_init(void)
{
    INT8U err;
    log_mutex = OSMutexCreate(APP_LOG_CEILING, &err);
    if ((err != OS_ERR_NONE) || (log_mutex == (OS_EVENT *)0)) {
        bsp_panic("OSMutexCreate");
    }
}

void bsp_log_lock(void)
{
    INT8U err;
    OSMutexPend(log_mutex, 0u, &err);
    if (err != OS_ERR_NONE) { bsp_panic("OSMutexPend"); }
}

void bsp_log_unlock(void)
{
    if (OSMutexPost(log_mutex) != OS_ERR_NONE) { bsp_panic("OSMutexPost"); }
}

_Noreturn void bsp_panic(const char *message)
{
    __asm__ volatile ("cpsid aif" : : : "memory");
    uart_puts("\nPANIC: ");
    uart_puts(message);
    uart_puts("\n");
    uart_flush();
    for (;;) { __asm__ volatile ("nop"); }
}

void port_fatal_exception(uint32_t kind, uint32_t address, uint32_t saved_cpsr)
{
    __asm__ volatile ("cpsid aif" : : : "memory");
    uart_puts("\nFATAL exception="); uart_puthex32(kind);
    uart_puts(" LR="); uart_puthex32(address);
    uart_puts(" SPSR="); uart_puthex32(saved_cpsr);
    bsp_panic("exception");
}
