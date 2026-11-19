/* SPDX-License-Identifier: Apache-2.0 */
#include "modbus_tcp.h"
#include "vgp_model.h"
#include "net_platform.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>

#define MB_TCP_MAX_FRAME 260u

typedef struct mb_tcp_job mb_tcp_job_t;

typedef struct {
    struct tcp_pcb *pcb;
    bool connected;
    bool connecting;
    uint16_t tx_id;
    uint8_t rx_buf[MB_TCP_MAX_FRAME];
    size_t rx_len;
    mb_tcp_job_t *active_job;
} mb_tcp_conn_t;

struct mb_tcp_job {
    uint16_t dev_idx;
    const char *host;
    uint16_t port;
    uint8_t unit_id;
    const uint8_t *req_pdu;
    size_t req_len;
    uint8_t *resp_pdu;
    size_t max_resp;
    size_t resp_len;
    uint16_t expected_tx_id;
    bool done;
    bool ok;
    char err[48];
};

static mb_tcp_conn_t g_conns[VGP_MAX_DEVICES];
static OS_EVENT *g_done_sem = NULL;
static bool g_inited = false;

static void set_job_err(mb_tcp_job_t *job, const char *msg)
{
    if (!job || !msg) return;
    strncpy(job->err, msg, sizeof(job->err) - 1u);
    job->err[sizeof(job->err) - 1u] = '\0';
}

static void finish_job(mb_tcp_conn_t *c, bool ok, const char *err_msg)
{
    mb_tcp_job_t *job = c->active_job;
    c->active_job = NULL;
    if (job && !job->done) {
        job->ok = ok;
        job->done = true;
        if (!ok && err_msg) set_job_err(job, err_msg);
        OSSemPost(g_done_sem);
    }
}

static void drop_conn_locked(mb_tcp_conn_t *c, const char *err_msg)
{
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        if (tcp_close(c->pcb) != ERR_OK) {
            tcp_abort(c->pcb);
        }
        c->pcb = NULL;
    }
    c->connected = false;
    c->connecting = false;
    c->rx_len = 0u;
    finish_job(c, false, err_msg);
}

static void send_active_request(mb_tcp_conn_t *c)
{
    mb_tcp_job_t *job = c->active_job;
    if (!job || !c->pcb || !c->connected) return;

    uint8_t frame[MB_TCP_MAX_FRAME];
    if (job->req_len == 0u || job->req_len + 7u > sizeof(frame)) {
        drop_conn_locked(c, "PDU too large");
        return;
    }

    c->tx_id++;
    if (c->tx_id == 0u) c->tx_id = 1u;
    job->expected_tx_id = c->tx_id;

    uint16_t mbap_len = (uint16_t)(job->req_len + 1u);
    frame[0] = (uint8_t)(c->tx_id >> 8);
    frame[1] = (uint8_t)(c->tx_id & 0xFFu);
    frame[2] = 0x00u;
    frame[3] = 0x00u;
    frame[4] = (uint8_t)(mbap_len >> 8);
    frame[5] = (uint8_t)(mbap_len & 0xFFu);
    frame[6] = job->unit_id;
    memcpy(frame + 7u, job->req_pdu, job->req_len);

    c->rx_len = 0u;
    err_t e = tcp_write(c->pcb, frame, (u16_t)(job->req_len + 7u), TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) {
        drop_conn_locked(c, "TCP write error");
        return;
    }
    tcp_output(c->pcb);
}

static err_t mb_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    mb_tcp_conn_t *c = (mb_tcp_conn_t *)arg;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        drop_conn_locked(c, "TCP closed");
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    if (!c->active_job) {
        pbuf_free(p);
        return ERR_OK;
    }

    size_t copy_len = p->tot_len;
    if (c->rx_len + copy_len > sizeof(c->rx_buf)) {
        pbuf_free(p);
        drop_conn_locked(c, "MBAP overflow");
        return ERR_OK;
    }
    pbuf_copy_partial(p, c->rx_buf + c->rx_len, (u16_t)copy_len, 0u);
    c->rx_len += copy_len;
    pbuf_free(p);

    if (c->rx_len < 7u) {
        return ERR_OK;
    }

    uint16_t rx_tx_id = (uint16_t)(((uint16_t)c->rx_buf[0] << 8) | c->rx_buf[1]);
    uint16_t rx_proto = (uint16_t)(((uint16_t)c->rx_buf[2] << 8) | c->rx_buf[3]);
    uint16_t rx_mbap_len = (uint16_t)(((uint16_t)c->rx_buf[4] << 8) | c->rx_buf[5]);
    uint8_t rx_unit = c->rx_buf[6];

    if (rx_proto != 0u || rx_mbap_len < 2u || rx_mbap_len > 254u) {
        drop_conn_locked(c, "Invalid MBAP");
        return ERR_OK;
    }

    size_t total_expected = 6u + (size_t)rx_mbap_len;
    if (c->rx_len < total_expected) {
        return ERR_OK;
    }

    mb_tcp_job_t *job = c->active_job;
    if (rx_tx_id != job->expected_tx_id || rx_unit != job->unit_id) {
        drop_conn_locked(c, "MBAP tx/unit mismatch");
        return ERR_OK;
    }

    size_t pdu_len = (size_t)rx_mbap_len - 1u;
    if (pdu_len > job->max_resp) {
        drop_conn_locked(c, "PDU buffer too small");
        return ERR_OK;
    }
    memcpy(job->resp_pdu, c->rx_buf + 7u, pdu_len);
    job->resp_len = pdu_len;
    c->rx_len = 0u;
    finish_job(c, true, NULL);
    return ERR_OK;
}

static void mb_err_cb(void *arg, err_t err)
{
    (void)err;
    mb_tcp_conn_t *c = (mb_tcp_conn_t *)arg;
    if (!c) return;
    /* lwIP already freed pcb before invoking err callback */
    c->pcb = NULL;
    c->connected = false;
    c->connecting = false;
    c->rx_len = 0u;
    finish_job(c, false, "TCP connection reset");
}

static err_t mb_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)tpcb;
    mb_tcp_conn_t *c = (mb_tcp_conn_t *)arg;
    if (!c) return ERR_OK;
    c->connecting = false;
    if (err != ERR_OK) {
        drop_conn_locked(c, "TCP connect failed");
        return ERR_OK;
    }
    c->connected = true;
    send_active_request(c);
    return ERR_OK;
}

static void start_tcp_connect(mb_tcp_conn_t *c, const ip_addr_t *addr)
{
    mb_tcp_job_t *job = c->active_job;
    if (!job) return;
    c->pcb = tcp_new();
    if (!c->pcb) {
        finish_job(c, false, "No TCP PCB");
        return;
    }
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, mb_recv_cb);
    tcp_err(c->pcb, mb_err_cb);
    c->connecting = true;
    err_t e = tcp_connect(c->pcb, addr, job->port, mb_connected_cb);
    if (e != ERR_OK) {
        drop_conn_locked(c, "tcp_connect err");
    }
}

static void mb_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    mb_tcp_conn_t *c = (mb_tcp_conn_t *)arg;
    if (!c || !c->active_job) return;
    if (!ipaddr) {
        finish_job(c, false, "DNS resolve failed");
        return;
    }
    start_tcp_connect(c, ipaddr);
}

static void mb_tcp_start_cb(void *arg)
{
    mb_tcp_job_t *job = (mb_tcp_job_t *)arg;
    if (job->dev_idx >= VGP_MAX_DEVICES) {
        job->ok = false;
        job->done = true;
        set_job_err(job, "Bad device idx");
        OSSemPost(g_done_sem);
        return;
    }
    mb_tcp_conn_t *c = &g_conns[job->dev_idx];
    c->active_job = job;

    if (c->pcb && c->connected) {
        send_active_request(c);
        return;
    }

    if (c->pcb) {
        drop_conn_locked(c, NULL);
        c->active_job = job;
    }

    ip_addr_t addr;
    if (ip4addr_aton(job->host, ip_2_ip4(&addr))) {
        start_tcp_connect(c, &addr);
        return;
    }

    err_t e = dns_gethostbyname(job->host, &addr, mb_dns_cb, c);
    if (e == ERR_OK) {
        start_tcp_connect(c, &addr);
    } else if (e != ERR_INPROGRESS) {
        finish_job(c, false, "DNS error");
    }
}

static void mb_tcp_abort_cb(void *arg)
{
    mb_tcp_job_t *job = (mb_tcp_job_t *)arg;
    if (job->dev_idx < VGP_MAX_DEVICES) {
        mb_tcp_conn_t *c = &g_conns[job->dev_idx];
        if (c->active_job == job) {
            drop_conn_locked(c, "Timeout");
            return;
        }
    }
    if (!job->done) {
        job->ok = false;
        job->done = true;
        set_job_err(job, "Timeout");
        OSSemPost(g_done_sem);
    }
}

static void mb_tcp_close_one_cb(void *arg)
{
    uint32_t idx = (uint32_t)(uintptr_t)arg;
    if (idx < VGP_MAX_DEVICES) {
        drop_conn_locked(&g_conns[idx], "Closed");
    }
}

static void mb_tcp_close_all_cb(void *arg)
{
    (void)arg;
    for (uint16_t i = 0; i < VGP_MAX_DEVICES; ++i) {
        drop_conn_locked(&g_conns[i], "Link down");
    }
}

void modbus_tcp_init(void)
{
    if (g_inited) return;
    memset(g_conns, 0, sizeof(g_conns));
    g_done_sem = OSSemCreate(0u);
    g_inited = (g_done_sem != NULL);
}

bool modbus_tcp_exchange(uint16_t dev_idx, const char *host, uint16_t port, uint8_t unit_id,
                         const uint8_t *req_pdu, size_t req_len,
                         uint8_t *resp_pdu, size_t max_resp, size_t *out_resp_len,
                         uint32_t timeout_ms, char *err_buf, size_t err_buf_len)
{
    if (out_resp_len) *out_resp_len = 0u;
    if (!g_inited || !host || !host[0] || !req_pdu || !resp_pdu || dev_idx >= VGP_MAX_DEVICES) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "Invalid args");
        return false;
    }

    /* Drain any stale semaphore count */
    while (OSSemAccept(g_done_sem) != 0u) {}

    mb_tcp_job_t job;
    memset(&job, 0, sizeof(job));
    job.dev_idx = dev_idx;
    job.host = host;
    job.port = port ? port : 502u;
    job.unit_id = unit_id;
    job.req_pdu = req_pdu;
    job.req_len = req_len;
    job.resp_pdu = resp_pdu;
    job.max_resp = max_resp;

    if (tcpip_callback(mb_tcp_start_cb, &job) != ERR_OK) {
        if (err_buf && err_buf_len) snprintf(err_buf, err_buf_len, "tcpip queue full");
        return false;
    }

    if (!net_sem_wait(g_done_sem, timeout_ms)) {
        /* Timeout: request tcpip_thread to abort and wait for acknowledgment so stack job stays valid */
        (void)tcpip_callback(mb_tcp_abort_cb, &job);
        (void)net_sem_wait(g_done_sem, 2000u);
    }

    if (!job.ok) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "%s", job.err[0] ? job.err : "TCP error");
        }
        return false;
    }

    if (out_resp_len) *out_resp_len = job.resp_len;
    return true;
}

void modbus_tcp_close_device(uint16_t dev_idx)
{
    if (!g_inited || dev_idx >= VGP_MAX_DEVICES) return;
    (void)tcpip_callback(mb_tcp_close_one_cb, (void *)(uintptr_t)dev_idx);
}

void modbus_tcp_close_all(void)
{
    if (!g_inited) return;
    (void)tcpip_callback(mb_tcp_close_all_cb, NULL);
}
