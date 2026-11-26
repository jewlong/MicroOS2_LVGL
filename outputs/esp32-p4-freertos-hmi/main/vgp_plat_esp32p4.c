/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_platform.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "vgp_p4";
static portMUX_TYPE s_vgp_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Updated by the touch read callback in app_main.c */
volatile bool g_p4_touch_connected = false;
volatile uint32_t g_p4_touch_reports = 0u;
volatile uint32_t g_p4_lvgl_heap_free = 0u;

uint32_t vgp_plat_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t vgp_plat_uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

uint32_t vgp_plat_enter_critical(void)
{
    portENTER_CRITICAL(&s_vgp_spinlock);
    return 0u;
}

void vgp_plat_exit_critical(uint32_t state)
{
    (void)state;
    portEXIT_CRITICAL(&s_vgp_spinlock);
}

void vgp_plat_log(const char *msg)
{
    if (msg) {
        ESP_LOGI(TAG, "%s", msg);
    }
}

void vgp_plat_get_status(vgp_plat_status_t *out, bool include_detail)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->platform_short  = "ESP32-P4";
    out->platform_full   = "ESP32-P4 (FreeRTOS SMP)";
    out->uptime_sec      = vgp_plat_uptime_sec();
    out->lvgl_heap_free  = g_p4_lvgl_heap_free;
    out->input_state     = g_p4_touch_connected ? 4u : 2u;
    out->touch_connected = g_p4_touch_connected;
    out->touch_reports   = g_p4_touch_reports;
    if (include_detail) {
        snprintf(out->input_detail, sizeof(out->input_detail),
                 "MIPI-DSI / RGB + I2C Capacitive Touch (%s)",
                 g_p4_touch_connected ? "READY" : "WAITING");
    }
}

void vgp_plat_format_status_bar(const char *modbus_summary, bool ntp_synced,
                                char *out, size_t max_out, uint32_t *out_color_rgb)
{
    if (!out || max_out == 0u) return;

    char ip_str[48] = "No IP";
    bool has_ip = false;
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0u) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            has_ip = true;
        }
    }

    if (modbus_summary && modbus_summary[0]) {
        snprintf(out, max_out, "[ESP32-P4]%s [%s] [Touch:%s] IP: %s",
                 ntp_synced ? " [NTP]" : "",
                 modbus_summary,
                 g_p4_touch_connected ? "OK" : "--",
                 ip_str);
    } else {
        snprintf(out, max_out, "[ESP32-P4]%s [Touch:%s] IP: %s",
                 ntp_synced ? " [NTP]" : "",
                 g_p4_touch_connected ? "OK" : "--",
                 ip_str);
    }

    if (out_color_rgb) {
        *out_color_rgb = has_ip ? 0x4ade80u : 0x38bdf8u;
    }
}
