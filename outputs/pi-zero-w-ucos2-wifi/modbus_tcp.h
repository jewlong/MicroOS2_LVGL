/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MODBUS_TCP_H
#define MODBUS_TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void modbus_tcp_init(void);

/* Synchronous wrapper called from modbus_task; all lwIP Raw API calls execute in tcpip_thread. */
bool modbus_tcp_exchange(uint16_t dev_idx, const char *host, uint16_t port, uint8_t unit_id,
                         const uint8_t *req_pdu, size_t req_len,
                         uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len,
                         uint32_t timeout_ms, char *err_buf, size_t err_buf_len);

void modbus_tcp_close_device(uint16_t dev_idx);
void modbus_tcp_close_all(void);

#endif
