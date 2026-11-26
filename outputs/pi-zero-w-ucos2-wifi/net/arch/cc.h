/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LWIP_PORT_CC_H
#define LWIP_PORT_CC_H
#include <stdint.h>
#include <stddef.h>
#include "net_platform.h"
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { net_log x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { bsp_panic(x); } while (0)
#include "bsp.h"
#define LWIP_RAND() port_lwip_rand()
uint32_t port_lwip_rand(void);
#define LWIP_NO_UNISTD_H 1
#endif
