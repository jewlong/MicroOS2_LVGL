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
uint32_t soc_peripheral_base = UINT32_C(0x20000000);
uint32_t soc_dma_bus_alias   = UINT32_C(0x40000000);
uint8_t  soc_is_bcm2837      = 0u;
_Alignas(8) static uint32_t mmu_l1_table_raw[8192];
static uint32_t next_tick;
static OS_EVENT *log_mutex;

_Static_assert(OS_TICKS_PER_SEC == 100u, "stage 2 uses 100 Hz ticks");

void soc_detect_init(void)
{
    uint32_t midr = 0u;
    __asm__ volatile ("mrc p15, 0, %0, c0, c0, 0" : "=r" (midr));
    if (((midr >> 4) & 0xfffu) != 0xb76u) {
        /* Raspberry Pi Zero 2 W (BCM2837 / RP3A0 Cortex-A53) */
        soc_peripheral_base = UINT32_C(0x3f000000);
        soc_dma_bus_alias   = UINT32_C(0xc0000000);
        soc_is_bcm2837      = 1u;

        /* 16 KB aligned L1 section page table */
        uint32_t *const mmu_l1_table = (uint32_t *)(
            ((uintptr_t)mmu_l1_table_raw + 0x3fffu) & ~(uintptr_t)0x3fffu);

        /* On ARMv8 Cortex-A53, MMU-off forces all RAM to Device-nGnRnE which faults
         * on any unaligned access. Set up a flat 1:1 identity-mapped L1 section table
         * with RAM (0x000..0x3EF MB) as Normal Non-Cacheable (TEX=001, C=0, B=0) and
         * Peripherals (0x3F0..0xFFF MB) as Device/Strongly-Ordered, keeping D-cache OFF
         * so USB/Mailbox/Framebuffer DMA stays 100% coherent without cache maintenance. */
        for (uint32_t i = 0u; i < 4096u; ++i) {
            if (i < 0x3f0u) {
                mmu_l1_table[i] = (i << 20) | (1u << 12) | (3u << 10) | 2u;
            } else {
                mmu_l1_table[i] = (i << 20) | (3u << 10) | (1u << 4) | 2u;
            }
        }
        const uint32_t zero = 0u;
        const uint32_t dacr = 0x55555555u;
        const uint32_t ttbr = (uint32_t)(uintptr_t)mmu_l1_table;
        __asm__ volatile (
            "mcr p15, 0, %0, c2, c0, 2\n\t" /* TTBCR = 0 (32-bit short-descriptor) */
            "mcr p15, 0, %1, c3, c0, 0\n\t" /* DACR = Client for all domains */
            "mcr p15, 0, %2, c2, c0, 0\n\t" /* TTBR0 */
            "mcr p15, 0, %0, c8, c7, 0\n\t" /* TLBIALL */
            "mcr p15, 0, %0, c7, c10, 4\n\t" /* DSB */
            "mcr p15, 0, %0, c7, c5, 4\n\t" /* ISB */
            : : "r" (zero), "r" (dacr), "r" (ttbr) : "memory"
        );
        uint32_t sctlr = 0u;
        __asm__ volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (sctlr));
        sctlr |= (1u << 0) | (1u << 5); /* Enable MMU (1:1 Normal Uncached), CP15BEN */
        sctlr &= ~((1u << 1) | (1u << 2)); /* Keep A=0 (unaligned OK) and C=0 (D-cache OFF) */
        __asm__ volatile (
            "mcr p15, 0, %0, c1, c0, 0\n\t"
            "mcr p15, 0, %1, c7, c10, 4\n\t"
            "mcr p15, 0, %1, c7, c5, 4\n\t"
            : : "r" (sctlr), "r" (zero) : "memory"
        );
    } else {
        /* Raspberry Pi Zero W (BCM2835 / ARM1176JZF-S) */
        soc_peripheral_base = UINT32_C(0x20000000);
        soc_dma_bus_alias   = UINT32_C(0x40000000);
        soc_is_bcm2837      = 0u;
    }
}

void bsp_interrupts_init(void)
{
    if (soc_is_bcm2837 != 0u) {
        /* Route GPU interrupts to Core 0 IRQ on BCM2837 ARM local controller */
        mmio_write(UINT32_C(0x4000000c), 0u);
    }
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
