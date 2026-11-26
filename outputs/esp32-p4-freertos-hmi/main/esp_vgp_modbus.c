/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_modbus.h"
#include "modbus_codec.h"
#include "vgp_platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <string.h>

#define VGP_MB_WRITE_Q_LEN 16u

typedef struct {
    uint16_t var_idx;
    double num_val;
    bool bool_val;
} vgp_mb_write_req_t;

static vgp_project_t * volatile g_proj = NULL;
static volatile bool g_started = false;

static EXT_RAM_BSS_ATTR uint16_t g_dev_sorted_vars[VGP_MAX_DEVICES][VGP_MAX_VARIABLES];
static EXT_RAM_BSS_ATTR vgp_mb_batch_t g_dev_batches[VGP_MAX_DEVICES][MODBUS_MAX_BATCHES];
static uint16_t g_dev_num_batches[VGP_MAX_DEVICES];
static uint32_t g_dev_last_poll_ms[VGP_MAX_DEVICES];
static int g_dev_socks[VGP_MAX_DEVICES];
static uint16_t g_tid = 1u;

static vgp_mb_write_req_t g_write_q[VGP_MB_WRITE_Q_LEN];
static volatile uint8_t g_wq_head = 0u;
static volatile uint8_t g_wq_tail = 0u;

static void close_all_sockets(void)
{
    for (uint16_t i = 0u; i < VGP_MAX_DEVICES; ++i) {
        if (g_dev_socks[i] >= 0) {
            lwip_close(g_dev_socks[i]);
            g_dev_socks[i] = -1;
        }
    }
}

static bool mb_tcp_transact(uint16_t dev_idx, const vgp_device_t *dev,
                            const uint8_t *req_pdu, size_t req_len,
                            uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len)
{
    if (g_dev_socks[dev_idx] < 0) {
        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(dev->port ? dev->port : 502u);
        if (inet_pton(AF_INET, dev->host, &dest.sin_addr) != 1) {
            return false;
        }
        int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (s < 0) return false;
        struct timeval tv = {
            .tv_sec = (int)(dev->timeout_ms / 1000u),
            .tv_usec = (int)((dev->timeout_ms % 1000u) * 1000u)
        };
        lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (lwip_connect(s, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            lwip_close(s);
            return false;
        }
        g_dev_socks[dev_idx] = s;
    }

    int s = g_dev_socks[dev_idx];
    uint16_t tid = g_tid++;
    uint8_t adu[260];
    uint16_t mbap_len = (uint16_t)(1u + req_len);
    adu[0] = (uint8_t)(tid >> 8);
    adu[1] = (uint8_t)(tid & 0xFFu);
    adu[2] = 0u;
    adu[3] = 0u;
    adu[4] = (uint8_t)(mbap_len >> 8);
    adu[5] = (uint8_t)(mbap_len & 0xFFu);
    adu[6] = dev->unit_id;
    memcpy(adu + 7u, req_pdu, req_len);

    if (lwip_send(s, adu, 7u + req_len, 0) != (int)(7u + req_len)) {
        lwip_close(s);
        g_dev_socks[dev_idx] = -1;
        return false;
    }

    uint8_t hdr[7];
    int got = lwip_recv(s, hdr, 7, MSG_WAITALL);
    if (got != 7) {
        lwip_close(s);
        g_dev_socks[dev_idx] = -1;
        return false;
    }
    uint16_t rx_tid = (uint16_t)(((uint16_t)hdr[0] << 8) | hdr[1]);
    uint16_t rx_len = (uint16_t)(((uint16_t)hdr[4] << 8) | hdr[5]);
    if (rx_tid != tid || rx_len < 2u || (size_t)(rx_len - 1u) > max_resp) {
        lwip_close(s);
        g_dev_socks[dev_idx] = -1;
        return false;
    }
    size_t pdu_len = (size_t)(rx_len - 1u);
    if (lwip_recv(s, resp_pdu, pdu_len, MSG_WAITALL) != (int)pdu_len) {
        lwip_close(s);
        g_dev_socks[dev_idx] = -1;
        return false;
    }
    *out_resp_len = pdu_len;
    return true;
}

static void mark_device_bad(vgp_project_t *proj, uint16_t dev_idx, const char *err)
{
    uint32_t sr = vgp_plat_enter_critical();
    vgp_device_t *dev = &proj->devices[dev_idx];
    dev->state = VGP_DEV_STATE_ERROR;
    dev->error_count++;
    if (err) {
        strncpy(dev->last_error, err, sizeof(dev->last_error) - 1u);
        dev->last_error[sizeof(dev->last_error) - 1u] = '\0';
    }
    for (uint16_t i = 0u; i < proj->num_variables; ++i) {
        if (proj->variables[i].modbus.enabled &&
            proj->variables[i].modbus.device_idx == (int16_t)dev_idx) {
            proj->variables[i].quality = VGP_QUAL_BAD;
        }
    }
    vgp_plat_exit_critical(sr);
}

void vgp_modbus_start(vgp_project_t *proj)
{
    vgp_modbus_stop();
    if (!proj) return;

    for (uint16_t d = 0u; d < proj->num_devices; ++d) {
        g_dev_last_poll_ms[d] = 0u;
        modbus_build_device_batches(proj, d,
                                    g_dev_sorted_vars[d], VGP_MAX_VARIABLES,
                                    g_dev_batches[d], MODBUS_MAX_BATCHES,
                                    &g_dev_num_batches[d]);
    }

    uint32_t sr = vgp_plat_enter_critical();
    g_wq_head = 0u;
    g_wq_tail = 0u;
    g_proj = proj;
    g_started = true;
    vgp_plat_exit_critical(sr);
}

void vgp_modbus_stop(void)
{
    uint32_t sr = vgp_plat_enter_critical();
    g_started = false;
    g_proj = NULL;
    g_wq_head = 0u;
    g_wq_tail = 0u;
    vgp_plat_exit_critical(sr);
    close_all_sockets();
}

bool vgp_modbus_enqueue_write(uint16_t var_idx, double num_val, bool bool_val)
{
    uint32_t sr = vgp_plat_enter_critical();
    vgp_project_t *proj = g_proj;
    if (!g_started || !proj || var_idx >= proj->num_variables) {
        vgp_plat_exit_critical(sr);
        return false;
    }
    const vgp_variable_t *v = &proj->variables[var_idx];
    if (!v->modbus.enabled || !v->modbus.writable || v->quality != VGP_QUAL_GOOD) {
        vgp_plat_exit_critical(sr);
        return false;
    }
    uint8_t next = (uint8_t)((g_wq_head + 1u) % VGP_MB_WRITE_Q_LEN);
    if (next == g_wq_tail) {
        vgp_plat_exit_critical(sr);
        return false;
    }
    g_write_q[g_wq_head].var_idx = var_idx;
    g_write_q[g_wq_head].num_val = num_val;
    g_write_q[g_wq_head].bool_val = bool_val;
    g_wq_head = next;
    vgp_plat_exit_critical(sr);
    return true;
}

void vgp_modbus_status_summary(char *out, size_t max_out)
{
    if (!out || max_out == 0u) return;
    out[0] = '\0';
    vgp_project_t *proj = g_proj;
    if (!g_started || !proj || proj->num_devices == 0u) return;

    uint16_t ok = 0u, err = 0u;
    for (uint16_t d = 0u; d < proj->num_devices; ++d) {
        if (proj->devices[d].state == VGP_DEV_STATE_CONNECTED) ok++;
        else if (proj->devices[d].state == VGP_DEV_STATE_ERROR) err++;
    }
    snprintf(out, max_out, "MB:%u/%u err:%u", (unsigned)ok, (unsigned)proj->num_devices, (unsigned)err);
}

void vgp_modbus_task(void *arg)
{
    (void)arg;
    for (uint16_t i = 0u; i < VGP_MAX_DEVICES; ++i) g_dev_socks[i] = -1;

    uint8_t req_pdu[MODBUS_MAX_PDU_LEN];
    uint8_t resp_pdu[MODBUS_MAX_PDU_LEN];

    for (;;) {
        vgp_project_t *proj = g_proj;
        if (!g_started || !proj) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 1. Drain write queue first */
        while (g_wq_tail != g_wq_head) {
            uint32_t sr = vgp_plat_enter_critical();
            vgp_mb_write_req_t req = g_write_q[g_wq_tail];
            g_wq_tail = (uint8_t)((g_wq_tail + 1u) % VGP_MB_WRITE_Q_LEN);
            vgp_plat_exit_critical(sr);

            vgp_variable_t *v = &proj->variables[req.var_idx];
            int16_t didx = v->modbus.device_idx;
            if (didx < 0 || didx >= (int16_t)proj->num_devices) continue;
            const vgp_device_t *dev = &proj->devices[didx];
            size_t rlen = modbus_encode_write_req(&v->modbus, req.num_val, req.bool_val,
                                                  v->type == VGP_VAR_BOOL, req_pdu, sizeof(req_pdu));
            size_t resp_len = 0u;
            uint8_t exc = 0u;
            if (rlen > 0u && dev->transport == VGP_MB_TRANSPORT_TCP &&
                mb_tcp_transact((uint16_t)didx, dev, req_pdu, rlen, resp_pdu, sizeof(resp_pdu), &resp_len) &&
                modbus_validate_write_resp(req_pdu, rlen, resp_pdu, resp_len, &exc)) {
                g_dev_last_poll_ms[didx] = 0u; /* Force immediate readback */
            } else {
                v->write_failed = true;
            }
        }

        /* 2. Scheduled batch polling */
        uint32_t now = vgp_plat_now_ms();
        for (uint16_t d = 0u; d < proj->num_devices && g_started; ++d) {
            vgp_device_t *dev = &proj->devices[d];
            if (!dev->enabled || g_dev_num_batches[d] == 0u) continue;
            if (g_dev_last_poll_ms[d] != 0u &&
                (uint32_t)(now - g_dev_last_poll_ms[d]) < dev->poll_interval_ms) {
                continue;
            }
            g_dev_last_poll_ms[d] = now ? now : 1u;

            if (dev->transport != VGP_MB_TRANSPORT_TCP) {
                mark_device_bad(proj, d, "RTU UART not bound");
                continue;
            }

            bool all_ok = true;
            for (uint16_t b = 0u; b < g_dev_num_batches[d]; ++b) {
                const vgp_mb_batch_t *batch = &g_dev_batches[d][b];
                size_t rlen = modbus_encode_read_req(batch->area, batch->start_addr, batch->quantity,
                                                     req_pdu, sizeof(req_pdu));
                size_t resp_len = 0u;
                if (!mb_tcp_transact(d, dev, req_pdu, rlen, resp_pdu, sizeof(resp_pdu), &resp_len)) {
                    mark_device_bad(proj, d, "TCP timeout/disconnect");
                    all_ok = false;
                    break;
                }
                for (uint16_t k = 0u; k < batch->var_count; ++k) {
                    uint16_t vi = g_dev_sorted_vars[d][batch->var_start + k];
                    vgp_variable_t *v = &proj->variables[vi];
                    double num = 0.0;
                    bool bval = false;
                    if (modbus_decode_read_value(&v->modbus, batch->start_addr, resp_pdu, resp_len, &num, &bval)) {
                        v->val_num = num;
                        v->val_bool = bval;
                        v->quality = VGP_QUAL_GOOD;
                        v->updated_ms = now;
                    } else {
                        v->quality = VGP_QUAL_BAD;
                        all_ok = false;
                    }
                }
            }
            if (all_ok) {
                dev->state = VGP_DEV_STATE_CONNECTED;
                dev->last_success_ms = now;
                dev->last_error[0] = '\0';
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
