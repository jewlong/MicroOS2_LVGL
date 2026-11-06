/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NET_PLATFORM_H
#define NET_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ucos_ii.h>
void net_log(const char *fmt, ...);
void net_heap_init(void);
void net_platform_init(void);
uint32_t net_now_ms(void);
INT32U net_ms_ticks(uint32_t ms);
bool net_sem_wait(OS_EVENT *sem, uint32_t ms); /* UINT32_MAX=forever, 0=try */
void wifi_sdio_poll_irq(void);
bool wifi_sdio_init(void);
void network_task(void *arg);
void network_snapshot(char *out, size_t size);
extern volatile uint32_t net_stage, net_error, net_rx_frames, net_tx_frames;
extern volatile uint32_t net_reconnects, net_udp_echoes, net_tcp_bytes;
extern volatile uint32_t net_sdio_commands, net_sdio_errors;
#endif
