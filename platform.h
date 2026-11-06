#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* BCM2835 ARM physical addresses, NOT 0x7e... GPU bus addresses. */
#define PERIPHERAL_BASE UINT32_C(0x20000000)
#define GPIO_BASE       (PERIPHERAL_BASE + UINT32_C(0x00200000))
#define UART0_BASE      (PERIPHERAL_BASE + UINT32_C(0x00201000))
#define TIMER_CLO       (PERIPHERAL_BASE + UINT32_C(0x00003004))

/* ARM1176 supports CP15 barriers. ARMv7 'dmb sy'/'dsb sy' are not used. */
static inline void arm_dmb(void)
{
    const uint32_t zero = 0u;
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 5" : : "r" (zero) : "memory");
}

static inline void mmio_write(uint32_t address, uint32_t value)
{
    arm_dmb();
    *(volatile uint32_t *)(uintptr_t)address = value;
    arm_dmb();
}

static inline uint32_t mmio_read(uint32_t address)
{
    uint32_t value;
    arm_dmb();
    value = *(volatile uint32_t *)(uintptr_t)address;
    arm_dmb();
    return value;
}

/* Free-running 1 MHz time source. Stage 2 also uses compare channel 1 for IRQ. */
static inline uint32_t timer_us(void)
{
    return mmio_read(TIMER_CLO);
}

static inline void delay_us(uint32_t delay)
{
    const uint32_t start = timer_us();
    while ((uint32_t)(timer_us() - start) < delay) {
        /* Unsigned subtraction handles the 32-bit timer wrap. */
    }
}

#endif
