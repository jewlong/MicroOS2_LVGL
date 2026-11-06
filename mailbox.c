/* SPDX-License-Identifier: Apache-2.0 */
#include "mailbox.h"
#include "platform.h"

#define MBOX_BASE    (PERIPHERAL_BASE + 0x0000b880u)
#define MBOX0_READ   (MBOX_BASE + 0x00u)
#define MBOX0_STATUS (MBOX_BASE + 0x18u)
#define MBOX1_WRITE  (MBOX_BASE + 0x20u)
#define MBOX1_STATUS (MBOX_BASE + 0x38u)
#define MBOX_FULL    (1u << 31)
#define MBOX_EMPTY   (1u << 30)
#define PROPERTY_CHANNEL 8u

static void arm_dsb(void)
{
    const uint32_t zero = 0;
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (zero) : "memory");
}

mailbox_result_t mailbox_property_call(volatile uint32_t *message,
                                      size_t bytes, uint32_t timeout_us)
{
    uint32_t control;
    const uintptr_t address = (uintptr_t)message;
    const uint32_t start = timer_us();
    __asm__ volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (control));
    if ((message == (void *)0) || ((address & 15u) != 0u) ||
        (address >= 0x20000000u) || (bytes > 0x20000000u-address) || (bytes < 12u) ||
        (bytes > 4096u) || ((bytes & 3u) != 0u) ||
        (timeout_us == 0u) || (timeout_us > 1000000u) ||
        ((control & 5u) != 0u)) {
        return MAILBOX_BAD_ARGUMENT;
    }
    /* Property channel 8 takes the ARM PHYSICAL buffer address, not a VC alias. */
    const uint32_t token = (uint32_t)address | PROPERTY_CHANNEL;
    arm_dsb();
    while ((mmio_read(MBOX1_STATUS) & MBOX_FULL) != 0u) {
        if ((uint32_t)(timer_us() - start) >= timeout_us) {
            return MAILBOX_WRITE_TIMEOUT;
        }
    }
    mmio_write(MBOX1_WRITE, token);
    for (;;) {
        if ((uint32_t)(timer_us() - start) >= timeout_us) {
            return MAILBOX_REPLY_TIMEOUT;
        }
        if ((mmio_read(MBOX0_STATUS) & MBOX_EMPTY) == 0u) {
            const uint32_t reply = mmio_read(MBOX0_READ);
            if (reply == token) {
                arm_dsb(); /* GPU writes to the shared message must be visible. */
                return MAILBOX_OK;
            }
            /* No other mailbox clients exist during this boot transaction. */
        }
    }
}
