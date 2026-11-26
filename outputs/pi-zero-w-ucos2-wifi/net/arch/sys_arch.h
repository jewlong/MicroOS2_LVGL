/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LWIP_PORT_SYS_H
#define LWIP_PORT_SYS_H
#include <ucos_ii.h>
typedef OS_EVENT *sys_sem_t;
typedef OS_EVENT *sys_mutex_t;
typedef struct port_mbox *sys_mbox_t;
typedef INT8U sys_thread_t;
typedef OS_CPU_SR sys_prot_t;
#define SYS_MBOX_NULL ((sys_mbox_t)0)
#define SYS_SEM_NULL ((sys_sem_t)0)
#define sys_sem_valid(x) (*(x) != 0)
#define sys_sem_set_invalid(x) (*(x) = 0)
#define sys_mutex_valid(x) (*(x) != 0)
#define sys_mutex_set_invalid(x) (*(x) = 0)
#define sys_mbox_valid(x) (*(x) != 0)
#define sys_mbox_set_invalid(x) (*(x) = 0)
#endif
