/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include "vgp_model.h"

/* Configure PL011 UART0 for Modbus RTU framing (called once if RTU device enabled). */
void modbus_rtu_configure(const vgp_device_t *dev);

/* Synchronous RTU request/response exchange on PL011 UART0. */
bool modbus_rtu_exchange(const vgp_device_t *dev,
                         const uint8_t *req_pdu, size_t req_len,
                         uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len,
                         char *err_buf, size_t err_buf_len);

#endif
