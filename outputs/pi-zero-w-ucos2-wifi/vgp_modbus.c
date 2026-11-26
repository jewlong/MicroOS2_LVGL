/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_modbus.h"
#include "modbus_codec.h"
#include "modbus_tcp.h"
#include "modbus_rtu.h"
#include "net_platform.h"
#include <ucos_ii.h>
#include <string.h>
#include <stdio.h>

#define VGP_MB_WRITE_Q_LEN 16u

typedef struct {
    uint16_t var_idx;
    double num_val;
    bool bool_val;
} vgp_mb_write_req_t;

extern volatile uint32_t app_preempt_pass;

static vgp_project_t * volatile g_proj = NULL;
static volatile bool g_started = false;
static volatile bool g_busy = false;

static uint16_t g_dev_sorted_vars[VGP_MAX_DEVICES][VGP_MAX_VARIABLES];
static vgp_mb_batch_t g_dev_batches[VGP_MAX_DEVICES][MODBUS_MAX_BATCHES];
static uint16_t g_dev_num_batches[VGP_MAX_DEVICES];
static uint32_t g_dev_last_poll_ms[VGP_MAX_DEVICES];

static vgp_mb_write_req_t g_write_q[VGP_MB_WRITE_Q_LEN];
static volatile uint8_t g_wq_head = 0u;
static volatile uint8_t g_wq_tail = 0u;

static char g_write_warn[64];

static uint32_t now_ms_tick(void)
{
    return (uint32_t)OSTimeGet() * (1000u / (uint32_t)OS_TICKS_PER_SEC);
}

static void set_write_warn(const char *msg)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (msg && msg[0]) {
        strncpy(g_write_warn, msg, sizeof(g_write_warn) - 1u);
        g_write_warn[sizeof(g_write_warn) - 1u] = '\0';
    } else {
        g_write_warn[0] = '\0';
    }
    OS_CPU_SR_Restore(sr);
}

static void mark_device_tags_bad(vgp_project_t *proj, uint16_t dev_idx, const char *err_msg)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    vgp_device_t *dev = &proj->devices[dev_idx];
    dev->state = VGP_DEV_STATE_ERROR;
    dev->error_count++;
    if (err_msg) {
        strncpy(dev->last_error, err_msg, sizeof(dev->last_error) - 1u);
        dev->last_error[sizeof(dev->last_error) - 1u] = '\0';
    }
    for (uint16_t i = 0; i < proj->num_variables; ++i) {
        if (proj->variables[i].modbus.enabled &&
            proj->variables[i].modbus.device_idx == (int16_t)dev_idx) {
            proj->variables[i].quality = VGP_QUAL_BAD;
        }
    }
    OS_CPU_SR_Restore(sr);
}

static bool exchange_device(const vgp_device_t *dev, uint16_t dev_idx,
                            const uint8_t *req_pdu, size_t req_len,
                            uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len,
                            char *err_buf, size_t err_buf_len)
{
    if (dev->transport == VGP_MB_TRANSPORT_TCP) {
        if (!network_is_online()) {
            if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "Wi-Fi offline");
            return false;
        }
        return modbus_tcp_exchange(dev_idx, dev->host, dev->port, dev->unit_id,
                                   req_pdu, req_len, resp_pdu, max_resp, out_resp_len,
                                   dev->timeout_ms, err_buf, err_buf_len);
    }
    return modbus_rtu_exchange(dev, req_pdu, req_len, resp_pdu, max_resp, out_resp_len,
                               err_buf, err_buf_len);
}

static bool pop_write_req(vgp_mb_write_req_t *out)
{
    bool got = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (g_wq_head != g_wq_tail) {
        *out = g_write_q[g_wq_tail];
        g_wq_tail = (uint8_t)((g_wq_tail + 1u) % VGP_MB_WRITE_Q_LEN);
        got = true;
    }
    OS_CPU_SR_Restore(sr);
    return got;
}

static void process_write_req(vgp_project_t *proj, const vgp_mb_write_req_t *req)
{
    if (req->var_idx >= proj->num_variables) return;
    vgp_variable_t *v = &proj->variables[req->var_idx];
    if (!v->modbus.enabled || !v->modbus.writable || v->modbus.device_idx < 0) return;

    uint16_t dev_idx = (uint16_t)v->modbus.device_idx;
    if (dev_idx >= proj->num_devices || !proj->devices[dev_idx].enabled) return;
    const vgp_device_t *dev = &proj->devices[dev_idx];

    uint8_t req_pdu[MODBUS_MAX_PDU_LEN];
    uint8_t resp_pdu[MODBUS_MAX_PDU_LEN];
    size_t req_len = modbus_encode_write_req(&v->modbus, req->num_val, req->bool_val,
                                             (v->type == VGP_VAR_BOOL), req_pdu, sizeof(req_pdu));
    if (req_len == 0u) {
        char warn[64];
        snprintf(warn, sizeof(warn), "Write range err: %s", v->name[0] ? v->name : v->id);
        set_write_warn(warn);
        OS_CPU_SR sr = OS_CPU_SR_Save();
        v->write_failed = true;
        OS_CPU_SR_Restore(sr);
        return;
    }

    size_t resp_len = 0u;
    char err_buf[48] = {0};
    uint8_t exc = 0u;
    bool ok = exchange_device(dev, dev_idx, req_pdu, req_len,
                              resp_pdu, sizeof(resp_pdu), &resp_len,
                              err_buf, sizeof(err_buf));
    if (ok) {
        ok = modbus_validate_write_resp(req_pdu, req_len, resp_pdu, resp_len, &exc);
    }

    if (!ok) {
        /* Per spec: do NOT auto-retry failed writes; alert operator */
        char warn[64];
        snprintf(warn, sizeof(warn), "Write unconfirmed: %s", v->name[0] ? v->name : v->id);
        set_write_warn(warn);
        OS_CPU_SR sr = OS_CPU_SR_Save();
        v->write_failed = true;
        OS_CPU_SR_Restore(sr);
        return;
    }

    /* Mandatory Read-Back confirmation */
    uint16_t qty = (v->modbus.area == VGP_MB_AREA_COIL) ? 1u : modbus_datatype_words(v->modbus.data_type);
    size_t rb_req_len = modbus_encode_read_req(v->modbus.area, v->modbus.address, qty,
                                               req_pdu, sizeof(req_pdu));
    size_t rb_resp_len = 0u;
    bool rb_ok = (rb_req_len > 0u) &&
                 exchange_device(dev, dev_idx, req_pdu, rb_req_len,
                                 resp_pdu, sizeof(resp_pdu), &rb_resp_len,
                                 err_buf, sizeof(err_buf));
    double rb_num = 0.0;
    bool rb_bool = false;
    if (rb_ok) {
        rb_ok = modbus_decode_read_value(&v->modbus, v->modbus.address,
                                         resp_pdu, rb_resp_len, &rb_num, &rb_bool);
    }

    if (!rb_ok) {
        char warn[64];
        snprintf(warn, sizeof(warn), "Readback failed: %s", v->name[0] ? v->name : v->id);
        set_write_warn(warn);
        OS_CPU_SR sr = OS_CPU_SR_Save();
        v->write_failed = true;
        OS_CPU_SR_Restore(sr);
        return;
    }

    uint32_t now_ms = now_ms_tick();
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (v->type == VGP_VAR_BOOL) v->val_bool = rb_bool;
    else if (v->type == VGP_VAR_NUM) v->val_num = rb_num;
    v->quality = VGP_QUAL_GOOD;
    v->updated_ms = now_ms;
    v->write_failed = false;
    OS_CPU_SR_Restore(sr);
    set_write_warn("");
}

static void poll_device(vgp_project_t *proj, uint16_t dev_idx)
{
    vgp_device_t *dev = &proj->devices[dev_idx];
    uint16_t nb = g_dev_num_batches[dev_idx];
    if (!dev->enabled || nb == 0u) return;

    uint8_t req_pdu[MODBUS_MAX_PDU_LEN];
    uint8_t resp_pdu[MODBUS_MAX_PDU_LEN];
    char err_buf[48] = {0};

    for (uint16_t b = 0; b < nb; ++b) {
        const vgp_mb_batch_t *batch = &g_dev_batches[dev_idx][b];
        size_t req_len = modbus_encode_read_req(batch->area, batch->start_addr, batch->quantity,
                                                req_pdu, sizeof(req_pdu));
        if (req_len == 0u) continue;

        size_t resp_len = 0u;
        bool ok = exchange_device(dev, dev_idx, req_pdu, req_len,
                                  resp_pdu, sizeof(resp_pdu), &resp_len,
                                  err_buf, sizeof(err_buf));
        if (!ok) {
            mark_device_tags_bad(proj, dev_idx, err_buf);
            return;
        }
        if (resp_len >= 2u && (resp_pdu[0] & 0x80u) != 0u) {
            snprintf(err_buf, sizeof(err_buf), "Modbus exc 0x%02x", (unsigned)resp_pdu[1]);
            mark_device_tags_bad(proj, dev_idx, err_buf);
            return;
        }

        uint32_t now_ms = now_ms_tick();
        for (uint16_t k = 0; k < batch->var_count; ++k) {
            uint16_t vi = g_dev_sorted_vars[dev_idx][batch->var_start + k];
            vgp_variable_t *v = &proj->variables[vi];
            double dnum = 0.0;
            bool dbool = false;
            bool dec_ok = modbus_decode_read_value(&v->modbus, batch->start_addr,
                                                   resp_pdu, resp_len, &dnum, &dbool);
            OS_CPU_SR sr = OS_CPU_SR_Save();
            if (dec_ok) {
                if (v->type == VGP_VAR_BOOL) v->val_bool = dbool;
                else if (v->type == VGP_VAR_NUM) v->val_num = dnum;
                v->quality = VGP_QUAL_GOOD;
                v->updated_ms = now_ms;
            } else {
                v->quality = VGP_QUAL_BAD;
            }
            OS_CPU_SR_Restore(sr);
        }
    }

    uint32_t now_ms = now_ms_tick();
    OS_CPU_SR sr = OS_CPU_SR_Save();
    dev->state = VGP_DEV_STATE_CONNECTED;
    dev->last_success_ms = now_ms;
    dev->last_error[0] = '\0';
    OS_CPU_SR_Restore(sr);
}

void vgp_modbus_start(vgp_project_t *proj)
{
    if (!proj) return;
    memset(g_dev_num_batches, 0, sizeof(g_dev_num_batches));
    memset(g_dev_last_poll_ms, 0, sizeof(g_dev_last_poll_ms));

    for (uint16_t d = 0; d < proj->num_devices; ++d) {
        if (proj->devices[d].enabled) {
            modbus_build_device_batches(proj, d,
                                        g_dev_sorted_vars[d], VGP_MAX_VARIABLES,
                                        g_dev_batches[d], MODBUS_MAX_BATCHES,
                                        &g_dev_num_batches[d]);
        }
    }

    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_proj = proj;
    g_wq_head = 0u;
    g_wq_tail = 0u;
    g_write_warn[0] = '\0';
    g_started = true;
    OS_CPU_SR_Restore(sr);
}

void vgp_modbus_stop(void)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_started = false;
    g_wq_head = 0u;
    g_wq_tail = 0u;
    OS_CPU_SR_Restore(sr);
    for (uint32_t wait = 0u; wait < 50u && g_busy; ++wait) {
        OSTimeDly(1u);
    }
    modbus_tcp_close_all();
    OS_CPU_SR sr2 = OS_CPU_SR_Save();
    g_proj = NULL;
    OS_CPU_SR_Restore(sr2);
}

bool vgp_modbus_enqueue_write(uint16_t var_idx, double num_val, bool bool_val)
{
    vgp_project_t *proj = g_proj;
    if (!proj || !g_started || var_idx >= proj->num_variables) return false;

    bool queued = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    const vgp_variable_t *v = &proj->variables[var_idx];
    if (v->modbus.enabled && v->modbus.writable && v->quality == VGP_QUAL_GOOD) {
        uint8_t next = (uint8_t)((g_wq_head + 1u) % VGP_MB_WRITE_Q_LEN);
        if (next != g_wq_tail) {
            g_write_q[g_wq_head].var_idx = var_idx;
            g_write_q[g_wq_head].num_val = num_val;
            g_write_q[g_wq_head].bool_val = bool_val;
            g_wq_head = next;
            queued = true;
        }
    }
    OS_CPU_SR_Restore(sr);
    return queued;
}

void vgp_modbus_status_summary(char *out, size_t max_out)
{
    if (!out || max_out == 0u) return;
    out[0] = '\0';
    vgp_project_t *proj = g_proj;
    if (!proj || proj->num_devices == 0u) return;

    uint16_t enabled_cnt = 0u, connected_cnt = 0u, err_cnt = 0u;
    char warn[64];
    OS_CPU_SR sr = OS_CPU_SR_Save();
    for (uint16_t d = 0; d < proj->num_devices; ++d) {
        if (!proj->devices[d].enabled) continue;
        enabled_cnt++;
        if (proj->devices[d].state == VGP_DEV_STATE_CONNECTED) connected_cnt++;
        else if (proj->devices[d].state == VGP_DEV_STATE_ERROR) err_cnt++;
    }
    strncpy(warn, g_write_warn, sizeof(warn) - 1u);
    warn[sizeof(warn) - 1u] = '\0';
    OS_CPU_SR_Restore(sr);

    if (enabled_cnt == 0u) return;
    if (warn[0]) {
        snprintf(out, max_out, "MB:%u/%u %s", (unsigned)connected_cnt, (unsigned)enabled_cnt, warn);
    } else if (err_cnt > 0u) {
        snprintf(out, max_out, "MB:%u/%u ERR", (unsigned)connected_cnt, (unsigned)enabled_cnt);
    } else {
        snprintf(out, max_out, "MB:%u/%u OK", (unsigned)connected_cnt, (unsigned)enabled_cnt);
    }
}

void vgp_modbus_task(void *arg)
{
    (void)arg;
    while (app_preempt_pass == 0u) {
        OSTimeDly(1u);
    }
    modbus_tcp_init();

    bool was_online = false;

    for (;;) {
        vgp_project_t *proj = g_proj;
        if (!g_started || !proj || proj->num_devices == 0u) {
            OSTimeDly(10u);
            continue;
        }

        bool online = network_is_online();
        if (was_online && !online) {
            modbus_tcp_close_all();
        }
        was_online = online;

        g_busy = true;
        /* 1. Prioritize operator writes */
        vgp_mb_write_req_t req;
        while (g_started && pop_write_req(&req)) {
            process_write_req(proj, &req);
        }

        /* 2. Poll due devices */
        uint32_t now_ms = now_ms_tick();
        for (uint16_t d = 0; g_started && d < proj->num_devices; ++d) {
            vgp_device_t *dev = &proj->devices[d];
            if (!dev->enabled || g_dev_num_batches[d] == 0u) continue;
            if (dev->transport == VGP_MB_TRANSPORT_TCP && !online) {
                if (dev->state == VGP_DEV_STATE_CONNECTED) {
                    mark_device_tags_bad(proj, d, "Wi-Fi offline");
                }
                continue;
            }
            if (g_dev_last_poll_ms[d] == 0u ||
                (uint32_t)(now_ms - g_dev_last_poll_ms[d]) >= dev->poll_interval_ms) {
                poll_device(proj, d);
                g_dev_last_poll_ms[d] = now_ms_tick();
                /* Service any urgent write queued during poll */
                while (g_started && pop_write_req(&req)) {
                    process_write_req(proj, &req);
                }
            }
        }
        g_busy = false;

        OSTimeDly(2u); /* 20ms scheduler yield */
    }
}
