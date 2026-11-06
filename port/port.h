/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PORT_H
#define PORT_H
#include <stdint.h>

extern volatile uint32_t port_irq_switches;
extern volatile uint32_t port_svc_switches;
extern volatile uint32_t port_task_switches;

void port_select_next(void);
void port_task_return(void);
void port_svc_handler(void);
void port_irq_handler(void);
void port_fatal_exception(uint32_t kind, uint32_t return_address, uint32_t saved_cpsr);

#endif
