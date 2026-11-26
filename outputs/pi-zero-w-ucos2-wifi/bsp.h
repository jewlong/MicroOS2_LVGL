/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BSP_H
#define BSP_H
#include <stdint.h>

extern volatile uint32_t bsp_irq_count;
extern volatile uint32_t bsp_late_ticks;

void bsp_interrupts_init(void);
void bsp_tick_start(void);
void bsp_irq_dispatch(void);
_Noreturn void bsp_panic(const char *message);
void bsp_log_lock(void);
void bsp_log_unlock(void);
void bsp_log_init(void);

#endif
