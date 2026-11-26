/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_http_ops.h"
#include "vgp_runtime.h"
#include "vgp_platform.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "http_ops";

static EXT_RAM_BSS_ATTR char g_ota_buf[VGP_OTA_MAX_BYTES];
static EXT_RAM_BSS_ATTR char g_json_tx_buf[8192];
static volatile size_t g_ota_len = 0u;
static volatile bool g_ota_busy = false;
static volatile bool g_ota_ready = false;
static char g_ops_token[64];

static const char g_index_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>ViewGP ESP32-P4 Ops</title>"
    "<style>body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;max-width:860px;margin:24px auto;padding:0 16px}"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:8px;padding:16px;margin-bottom:16px}"
    "button{background:#0284c7;color:#fff;border:0;padding:8px 16px;border-radius:6px;cursor:pointer}"
    "pre{background:#020617;padding:12px;border-radius:6px;overflow:auto;font-size:13px}</style></head><body>"
    "<h2>ViewGP ESP32-P4 Field Ops</h2>"
    "<div class=card><h3>Hot-Reload &amp; Persist project.json</h3>"
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

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-Ops-Token");
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, g_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    size_t n = vgp_runtime_format_status_json(g_json_tx_buf, sizeof(g_json_tx_buf));
    return httpd_resp_send(req, g_json_tx_buf, (ssize_t)n);
}

static esp_err_t alarms_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    size_t n = vgp_runtime_format_alarms_json(g_json_tx_buf, sizeof(g_json_tx_buf));
    return httpd_resp_send(req, g_json_tx_buf, (ssize_t)n);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t project_post_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (g_ops_token[0] != '\0') {
        char tok[64] = {0};
        if (httpd_req_get_hdr_value_str(req, "X-Ops-Token", tok, sizeof(tok)) != ESP_OK ||
            strcmp(tok, g_ops_token) != 0) {
            httpd_resp_set_status(req, "401 Unauthorized");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid X-Ops-Token\"}");
        }
    }

    if (req->content_len == 0u || req->content_len >= VGP_OTA_MAX_BYTES) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"payload size out of range\"}");
    }

    uint32_t sr = vgp_plat_enter_critical();
    if (g_ota_busy || g_ota_ready) {
        vgp_plat_exit_critical(sr);
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"OTA reload busy\"}");
    }
    g_ota_busy = true;
    vgp_plat_exit_critical(sr);

    size_t received = 0u;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, g_ota_buf + received, req->content_len - received);
        if (r <= 0) {
            sr = vgp_plat_enter_critical();
            g_ota_busy = false;
            vgp_plat_exit_critical(sr);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"incomplete upload\"}");
        }
        received += (size_t)r;
    }
    g_ota_buf[received] = '\0';

    sr = vgp_plat_enter_critical();
    g_ota_len = received;
    g_ota_ready = true;
    vgp_plat_exit_critical(sr);

    return httpd_resp_sendstr(req, "{\"ok\":true,\"queuedBytes\":true}");
}

bool esp_http_ops_start(const char *ops_token)
{
    memset(g_ops_token, 0, sizeof(g_ops_token));
    if (ops_token && ops_token[0]) {
        strncpy(g_ops_token, ops_token, sizeof(g_ops_token) - 1u);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP ops server on port 8080");
        return false;
    }

    const httpd_uri_t u_idx  = { .uri = "/",            .method = HTTP_GET,     .handler = index_get_handler };
    const httpd_uri_t u_st   = { .uri = "/api/status",  .method = HTTP_GET,     .handler = status_get_handler };
    const httpd_uri_t u_al   = { .uri = "/api/alarms",  .method = HTTP_GET,     .handler = alarms_get_handler };
    const httpd_uri_t u_post = { .uri = "/api/project", .method = HTTP_POST,    .handler = project_post_handler };
    const httpd_uri_t u_opt  = { .uri = "/api/project", .method = HTTP_OPTIONS, .handler = options_handler };

    httpd_register_uri_handler(server, &u_idx);
    httpd_register_uri_handler(server, &u_st);
    httpd_register_uri_handler(server, &u_al);
    httpd_register_uri_handler(server, &u_post);
    httpd_register_uri_handler(server, &u_opt);

    ESP_LOGI(TAG, "Field Ops HTTP listening on port 8080");
    return true;
}

bool esp_http_ops_take_ota(const char **out_json, size_t *out_len)
{
    uint32_t sr = vgp_plat_enter_critical();
    if (!g_ota_ready) {
        vgp_plat_exit_critical(sr);
        return false;
    }
    *out_json = g_ota_buf;
    *out_len = g_ota_len;
    vgp_plat_exit_critical(sr);
    return true;
}

void esp_http_ops_ota_done(bool reload_ok)
{
    if (reload_ok && g_ota_len > 0u) {
        FILE *fp = fopen(VGP_PROJECT_STORAGE_PATH, "wb");
        if (fp) {
            fwrite(g_ota_buf, 1u, g_ota_len, fp);
            fclose(fp);
            ESP_LOGI(TAG, "Persisted new project.json (%u bytes) to Flash", (unsigned)g_ota_len);
        }
    }
    uint32_t sr = vgp_plat_enter_critical();
    g_ota_ready = false;
    g_ota_busy = false;
    vgp_plat_exit_critical(sr);
}
