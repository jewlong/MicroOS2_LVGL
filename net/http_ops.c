/* SPDX-License-Identifier: Apache-2.0 */
#include "http_ops.h"
#include "vgp_runtime.h"
#include "bootcfg.h"
#include "net_platform.h"
#include "lwip/tcp.h"
#include <ucos_ii.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP_MAX_CONNS     4u
#define HTTP_HDR_BUF_SIZE  1024u
#define HTTP_TX_BUF_SIZE   8192u

typedef enum {
    HTTP_ST_HDR = 0,
    HTTP_ST_BODY,
    HTTP_ST_SEND
} http_state_t;

typedef struct {
    bool used;
    struct tcp_pcb *pcb;
    http_state_t state;
    uint8_t idle_ticks;
    bool is_post_project;
    bool owns_ota_buf;
    char hdr_buf[HTTP_HDR_BUF_SIZE];
    size_t hdr_len;
    size_t content_length;
    size_t body_received;
    char tx_buf[HTTP_TX_BUF_SIZE];
    size_t tx_len;
    size_t tx_sent;
} http_conn_t;

static http_conn_t g_conns[HTTP_MAX_CONNS];
static char g_ota_buf[VGP_OTA_MAX_BYTES];
static volatile size_t g_ota_len = 0u;
static volatile bool g_ota_writing = false;
static volatile bool g_ota_ready = false;
static volatile bool g_ota_in_use = false;

static const char g_index_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>ViewGP Pi Zero Ops</title>"
    "<style>body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;max-width:860px;margin:24px auto;padding:0 16px}"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:8px;padding:16px;margin-bottom:16px}"
    "button{background:#0284c7;color:#fff;border:0;padding:8px 16px;border-radius:6px;cursor:pointer}"
    "pre{background:#020617;padding:12px;border-radius:6px;overflow:auto;font-size:13px}</style></head><body>"
    "<h2>ViewGP Pi Zero Field Ops</h2>"
    "<div class=card><h3>Hot-Reload project.json</h3>"
    "<input type=file id=f accept=.json> "
    "<input type=password id=tok placeholder='ops_token (optional)' style='padding:6px;margin:0 6px'> "
    "<button onclick=up()>Upload &amp; Reload</button>"
    "<span id=msg style='margin-left:12px'></span></div>"
    "<div class=card><h3>System &amp; Modbus Status <button onclick=load()>Refresh</button></h3><pre id=st>Loading...</pre></div>"
    "<div class=card><h3>Alarm History</h3><pre id=al>Loading...</pre></div>"
    "<script>async function load(){"
    "let s=await fetch('/api/status').then(r=>r.json());document.getElementById('st').textContent=JSON.stringify(s,null,2);"
    "let a=await fetch('/api/alarms').then(r=>r.json());document.getElementById('al').textContent=JSON.stringify(a,null,2);}"
    "async function up(){let f=document.getElementById('f').files[0];if(!f)return;"
    "let h={'Content-Type':'application/json'};let t=document.getElementById('tok').value.trim();if(t)h['X-Ops-Token']=t;"
    "let m=document.getElementById('msg');m.textContent='Uploading...';"
    "let r=await fetch('/api/project',{method:'POST',headers:h,body:await f.text()});"
    "m.textContent=r.status+' '+await r.text();setTimeout(load,800);}load();</script></body></html>";

static void close_conn(http_conn_t *c)
{
    if (!c) return;
    if (c->owns_ota_buf) {
        OS_CPU_SR sr = OS_CPU_SR_Save();
        g_ota_writing = false;
        OS_CPU_SR_Restore(sr);
        c->owns_ota_buf = false;
    }
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        tcp_poll(c->pcb, NULL, 0u);
        if (tcp_close(c->pcb) != ERR_OK) {
            tcp_abort(c->pcb);
        }
        c->pcb = NULL;
    }
    c->used = false;
}

static void pump_tx(http_conn_t *c)
{
    if (!c || !c->pcb || c->state != HTTP_ST_SEND) return;
    while (c->tx_sent < c->tx_len) {
        u16_t avail = tcp_sndbuf(c->pcb);
        if (avail == 0u) break;
        size_t rem = c->tx_len - c->tx_sent;
        u16_t chunk = (rem > (size_t)avail) ? avail : (u16_t)rem;
        if (chunk > 1400u) chunk = 1400u;
        err_t e = tcp_write(c->pcb, c->tx_buf + c->tx_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) break;
        c->tx_sent += chunk;
    }
    tcp_output(c->pcb);
    if (c->tx_sent >= c->tx_len) {
        close_conn(c);
    }
}

static void send_response(http_conn_t *c, int code, const char *status_txt,
                          const char *ctype, const char *body, size_t body_len)
{
    int hlen = snprintf(c->tx_buf, sizeof(c->tx_buf),
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type, Authorization, X-Ops-Token\r\n\r\n",
                        code, status_txt, ctype, (unsigned long)body_len);
    if (hlen < 0) hlen = 0;
    size_t pos = (size_t)hlen;
    if (body && body_len > 0u && pos < sizeof(c->tx_buf)) {
        size_t avail = sizeof(c->tx_buf) - pos;
        size_t n = (body_len < avail) ? body_len : avail;
        memcpy(c->tx_buf + pos, body, n);
        pos += n;
    }
    c->tx_len = pos;
    c->tx_sent = 0u;
    c->state = HTTP_ST_SEND;
    pump_tx(c);
}

static bool ascii_ieq_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static size_t parse_content_length(const char *hdr, size_t hdr_len)
{
    size_t i = 0u;
    while (i < hdr_len) {
        size_t line_start = i;
        while (i < hdr_len && hdr[i] != '\n') i++;
        size_t line_end = i;
        if (i < hdr_len) i++;
        if (line_end - line_start > 15u && ascii_ieq_prefix(hdr + line_start, "content-length:")) {
            size_t k = line_start + 15u;
            while (k < line_end && (hdr[k] == ' ' || hdr[k] == '\t')) k++;
            size_t val = 0u;
            while (k < line_end && hdr[k] >= '0' && hdr[k] <= '9') {
                val = val * 10u + (size_t)(hdr[k] - '0');
                k++;
            }
            return val;
        }
    }
    return 0u;
}

static void finish_post_project(http_conn_t *c)
{
    /* Basic sanity check: non-empty and starts with '{' (after optional whitespace/BOM) */
    size_t p = 0u;
    if (c->body_received >= 3u && (uint8_t)g_ota_buf[0] == 0xEFu &&
        (uint8_t)g_ota_buf[1] == 0xBBu && (uint8_t)g_ota_buf[2] == 0xBFu) {
        p = 3u;
    }
    while (p < c->body_received && (unsigned char)g_ota_buf[p] <= ' ') p++;
    if (p >= c->body_received || g_ota_buf[p] != '{') {
        OS_CPU_SR sr = OS_CPU_SR_Save();
        g_ota_writing = false;
        OS_CPU_SR_Restore(sr);
        c->owns_ota_buf = false;
        const char *msg = "{\"ok\":false,\"error\":\"invalid json\"}";
        send_response(c, 400, "Bad Request", "application/json", msg, strlen(msg));
        return;
    }

    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_ota_len = c->body_received;
    g_ota_writing = false;
    g_ota_ready = true;
    OS_CPU_SR_Restore(sr);
    c->owns_ota_buf = false;

    char resp[64];
    int n = snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%lu}", (unsigned long)c->body_received);
    send_response(c, 200, "OK", "application/json", resp, (size_t)(n > 0 ? n : 0));
}

static bool verify_ops_token(const char *hdr, size_t hdr_len)
{
    const wifi_settings_t *cfg = bootcfg_wifi();
    if (!cfg || !cfg->ops_token_set || !cfg->ops_token[0]) {
        return true; /* No token configured -> open on local LAN */
    }
    const char *expected = cfg->ops_token;
    size_t exp_len = strlen(expected);

    size_t i = 0u;
    while (i < hdr_len) {
        size_t ls = i;
        while (i < hdr_len && hdr[i] != '\n') i++;
        size_t le = i;
        if (i < hdr_len) i++;
        while (le > ls && (hdr[le - 1u] == '\r' || hdr[le - 1u] == ' ' || hdr[le - 1u] == '\t')) le--;

        size_t vs = 0u;
        if (le - ls > 12u && ascii_ieq_prefix(hdr + ls, "x-ops-token:")) {
            vs = ls + 12u;
        } else if (le - ls > 21u && ascii_ieq_prefix(hdr + ls, "authorization:")) {
            vs = ls + 14u;
            while (vs < le && (hdr[vs] == ' ' || hdr[vs] == '\t')) vs++;
            if (le - vs >= 7u && ascii_ieq_prefix(hdr + vs, "bearer ")) {
                vs += 7u;
            } else {
                continue;
            }
        } else {
            continue;
        }
        while (vs < le && (hdr[vs] == ' ' || hdr[vs] == '\t')) vs++;
        if (le - vs == exp_len && memcmp(hdr + vs, expected, exp_len) == 0) {
            return true;
        }
    }
    return false;
}

static void handle_headers_complete(http_conn_t *c, const char *body_part, size_t body_part_len)
{
    if (strncmp(c->hdr_buf, "OPTIONS ", 8u) == 0) {
        send_response(c, 204, "No Content", "text/plain", "", 0u);
        return;
    }
    if (strncmp(c->hdr_buf, "GET / ", 6u) == 0 || strncmp(c->hdr_buf, "GET /index.html ", 16u) == 0) {
        send_response(c, 200, "OK", "text/html; charset=utf-8", g_index_html, sizeof(g_index_html) - 1u);
        return;
    }
    if (strncmp(c->hdr_buf, "GET /api/status ", 16u) == 0) {
        static char json_tmp[4096];
        size_t n = vgp_runtime_format_status_json(json_tmp, sizeof(json_tmp));
        send_response(c, 200, "OK", "application/json", json_tmp, n);
        return;
    }
    if (strncmp(c->hdr_buf, "GET /api/alarms ", 16u) == 0) {
        static char json_tmp[6144];
        size_t n = vgp_runtime_format_alarms_json(json_tmp, sizeof(json_tmp));
        send_response(c, 200, "OK", "application/json", json_tmp, n);
        return;
    }
    if (strncmp(c->hdr_buf, "POST /api/project ", 18u) == 0) {
        if (!verify_ops_token(c->hdr_buf, c->hdr_len)) {
            const char *msg = "{\"ok\":false,\"error\":\"unauthorized (check ops_token)\"}";
            send_response(c, 401, "Unauthorized", "application/json", msg, strlen(msg));
            return;
        }
        size_t clen = parse_content_length(c->hdr_buf, c->hdr_len);
        if (clen == 0u) {
            const char *msg = "{\"ok\":false,\"error\":\"missing Content-Length\"}";
            send_response(c, 400, "Bad Request", "application/json", msg, strlen(msg));
            return;
        }
        if (clen > VGP_OTA_MAX_BYTES) {
            const char *msg = "{\"ok\":false,\"error\":\"payload exceeds 2 MiB\"}";
            send_response(c, 413, "Payload Too Large", "application/json", msg, strlen(msg));
            return;
        }
        bool busy = false;
        OS_CPU_SR sr = OS_CPU_SR_Save();
        if (g_ota_writing || g_ota_ready || g_ota_in_use) {
            busy = true;
        } else {
            g_ota_writing = true;
        }
        OS_CPU_SR_Restore(sr);
        if (busy) {
            const char *msg = "{\"ok\":false,\"error\":\"reload busy\"}";
            send_response(c, 409, "Conflict", "application/json", msg, strlen(msg));
            return;
        }
        c->owns_ota_buf = true;
        c->is_post_project = true;
        c->content_length = clen;
        c->body_received = 0u;
        c->state = HTTP_ST_BODY;
        if (body_part_len > 0u) {
            size_t copy = (body_part_len > clen) ? clen : body_part_len;
            memcpy(g_ota_buf, body_part, copy);
            c->body_received = copy;
        }
        if (c->body_received >= c->content_length) {
            finish_post_project(c);
        }
        return;
    }
    const char *nf = "{\"error\":\"not found\"}";
    send_response(c, 404, "Not Found", "application/json", nf, strlen(nf));
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)tpcb;
    (void)len;
    http_conn_t *c = (http_conn_t *)arg;
    if (c && c->used) {
        c->idle_ticks = 0u;
        pump_tx(c);
    }
    return ERR_OK;
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;
    if (!c || !c->used) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        close_conn(c);
        return ERR_OK;
    }

    c->idle_ticks = 0u;
    tcp_recved(tpcb, p->tot_len);

    if (c->state == HTTP_ST_HDR) {
        size_t avail = sizeof(c->hdr_buf) - 1u - c->hdr_len;
        size_t to_copy = (p->tot_len < avail) ? (size_t)p->tot_len : avail;
        pbuf_copy_partial(p, c->hdr_buf + c->hdr_len, (u16_t)to_copy, 0u);
        c->hdr_len += to_copy;
        c->hdr_buf[c->hdr_len] = '\0';

        char *sep = strstr(c->hdr_buf, "\r\n\r\n");
        if (sep) {
            size_t hdr_end = (size_t)(sep - c->hdr_buf) + 4u;
            size_t in_hdr_body = (c->hdr_len > hdr_end) ? (c->hdr_len - hdr_end) : 0u;
            size_t extra_in_pbuf = (p->tot_len > to_copy) ? ((size_t)p->tot_len - to_copy) : 0u;
            handle_headers_complete(c, c->hdr_buf + hdr_end, in_hdr_body);
            if (c->used && c->state == HTTP_ST_BODY && extra_in_pbuf > 0u) {
                size_t rem = c->content_length - c->body_received;
                size_t cp = (extra_in_pbuf < rem) ? extra_in_pbuf : rem;
                pbuf_copy_partial(p, g_ota_buf + c->body_received, (u16_t)cp, (u16_t)to_copy);
                c->body_received += cp;
                if (c->body_received >= c->content_length) {
                    finish_post_project(c);
                }
            }
        } else if (c->hdr_len >= sizeof(c->hdr_buf) - 1u) {
            const char *msg = "{\"error\":\"headers too large\"}";
            send_response(c, 400, "Bad Request", "application/json", msg, strlen(msg));
        }
    } else if (c->state == HTTP_ST_BODY && c->owns_ota_buf) {
        size_t rem = c->content_length - c->body_received;
        size_t cp = ((size_t)p->tot_len < rem) ? (size_t)p->tot_len : rem;
        if (cp > 0u) {
            pbuf_copy_partial(p, g_ota_buf + c->body_received, (u16_t)cp, 0u);
            c->body_received += cp;
        }
        if (c->body_received >= c->content_length) {
            finish_post_project(c);
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err)
{
    (void)err;
    http_conn_t *c = (http_conn_t *)arg;
    if (!c) return;
    c->pcb = NULL; /* Already freed by lwIP */
    close_conn(c);
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *tpcb)
{
    (void)tpcb;
    http_conn_t *c = (http_conn_t *)arg;
    if (!c || !c->used) return ERR_OK;
    if (++c->idle_ticks >= 6u) {
        close_conn(c);
    }
    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;

    http_conn_t *slot = NULL;
    for (uint32_t i = 0; i < HTTP_MAX_CONNS; ++i) {
        if (!g_conns[i].used) {
            slot = &g_conns[i];
            break;
        }
    }
    if (!slot) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->pcb = newpcb;
    slot->state = HTTP_ST_HDR;

    tcp_arg(newpcb, slot);
    tcp_recv(newpcb, http_recv_cb);
    tcp_sent(newpcb, http_sent_cb);
    tcp_err(newpcb, http_err_cb);
    tcp_poll(newpcb, http_poll_cb, 10u);
    return ERR_OK;
}

void http_ops_init(void)
{
    memset(g_conns, 0, sizeof(g_conns));
    if (bootcfg_wifi()->ops_http_disabled) {
        net_log("[HTTP] Field Ops server disabled by wifi.txt (ops_http=0)\n");
        return;
    }
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ADDR_ANY, HTTP_OPS_PORT) != ERR_OK) {
        tcp_close(pcb);
        return;
    }
    pcb = tcp_listen_with_backlog(pcb, 4);
    if (!pcb) return;
    tcp_accept(pcb, http_accept_cb);
    net_log("[HTTP] Field Ops server listening on port %u\n", (unsigned)HTTP_OPS_PORT);
}

bool http_ops_take_ota(const char **out_json, size_t *out_len)
{
    bool got = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (g_ota_ready && !g_ota_in_use) {
        g_ota_ready = false;
        g_ota_in_use = true;
        if (out_json) *out_json = g_ota_buf;
        if (out_len) *out_len = g_ota_len;
        got = true;
    }
    OS_CPU_SR_Restore(sr);
    return got;
}

void http_ops_ota_done(void)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_ota_in_use = false;
    OS_CPU_SR_Restore(sr);
}
