/* SPDX-License-Identifier: Apache-2.0 */
#include "modbus_rtu.h"
#include "modbus_codec.h"
#include "uart.h"
#include "platform.h"
#include <ucos_ii.h>
#include <string.h>
#include <stdio.h>

#define MB_RTU_MAX_FRAME 256u

static uint32_t g_last_baud = 0u;
static uint8_t g_last_parity = 0xFFu;
static uint8_t g_last_stop = 0u;

void modbus_rtu_configure(const vgp_device_t *dev)
{
    if (!dev) return;
    uint32_t baud = dev->baud_rate ? dev->baud_rate : 9600u;
    uint8_t parity = (uint8_t)dev->parity;
    uint8_t stop = (dev->stop_bits >= 2u) ? 2u : 1u;
    if (!uart_rtu_active() || baud != g_last_baud || parity != g_last_parity || stop != g_last_stop) {
        uart_configure_rtu(baud, parity, stop);
        g_last_baud = baud;
        g_last_parity = parity;
        g_last_stop = stop;
    }
}

bool modbus_rtu_exchange(const vgp_device_t *dev,
                         const uint8_t *req_pdu, size_t req_len,
                         uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len,
                         char *err_buf, size_t err_buf_len)
{
    if (out_resp_len) *out_resp_len = 0u;
    if (!dev || !req_pdu || req_len == 0u || req_len + 3u > MB_RTU_MAX_FRAME || !resp_pdu) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU bad args");
        return false;
    }
    if (dev->unit_id == 0u || dev->unit_id > 247u) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU unitId 1..247");
        return false;
    }

    modbus_rtu_configure(dev);

    uint8_t tx[MB_RTU_MAX_FRAME];
    tx[0] = dev->unit_id;
    memcpy(tx + 1u, req_pdu, req_len);
    uint16_t crc = modbus_crc16(tx, req_len + 1u);
    tx[req_len + 1u] = (uint8_t)(crc & 0xFFu);
    tx[req_len + 2u] = (uint8_t)(crc >> 8);

    uart_rx_drain();
    uart_tx_bytes(tx, req_len + 3u);

    uint32_t baud = dev->baud_rate ? dev->baud_rate : 9600u;
    uint32_t t35_us = (baud > 19200u) ? 1750u : (38500000u / baud);
    if (t35_us < 1750u) t35_us = 1750u;
    uint32_t timeout_us = (dev->timeout_ms ? dev->timeout_ms : 1000u) * 1000u;

    uint8_t rx[MB_RTU_MAX_FRAME];
    size_t rx_len = 0u;
    uint32_t start_us = timer_us();
    uint32_t last_rx_us = start_us;

    for (;;) {
        uint8_t b = 0u;
        bool got_any = false;
        while (uart_rx_byte_raw(&b)) {
            got_any = true;
            last_rx_us = timer_us();
            if (rx_len < sizeof(rx)) {
                rx[rx_len++] = b;
            }
        }
        uint32_t now_us = timer_us();
        if (rx_len > 0u) {
            if ((uint32_t)(now_us - last_rx_us) >= t35_us) {
                break; /* Inter-frame silence reached */
            }
        } else {
            if ((uint32_t)(now_us - start_us) >= timeout_us) {
                if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU timeout");
                return false;
            }
            OSTimeDly(1u); /* Yield 10ms while waiting for first response byte */
        }
        if (!got_any && rx_len > 0u && (uint32_t)(now_us - start_us) >= timeout_us) {
            break;
        }
    }

    if (rx_len < 4u) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU short frame");
        return false;
    }

    uint16_t rx_crc = (uint16_t)((uint16_t)rx[rx_len - 2u] | ((uint16_t)rx[rx_len - 1u] << 8));
    uint16_t calc_crc = modbus_crc16(rx, rx_len - 2u);
    if (rx_crc != calc_crc) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU CRC mismatch");
        return false;
    }

    if (rx[0] != dev->unit_id) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU unit mismatch");
        return false;
    }

    size_t pdu_len = rx_len - 3u;
    if (pdu_len > max_resp) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "RTU PDU overflow");
        return false;
    }
    memcpy(resp_pdu, rx + 1u, pdu_len);
    if (out_resp_len) *out_resp_len = pdu_len;
    return true;
}
