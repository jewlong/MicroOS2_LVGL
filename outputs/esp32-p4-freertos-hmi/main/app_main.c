/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_runtime.h"
#include "vgp_modbus.h"
#include "vgp_platform.h"
#include "sys_clock.h"
#include "esp_http_ops.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "p4_hmi";

/* Default display geometry (e.g. 7-inch / 10.1-inch MIPI-DSI panel on ESP32-P4).
 * Replace flush_cb & touch_read_cb below with your board's esp_lcd panel handle. */
#define DISP_HOR_RES    1024u
#define DISP_VER_RES    600u
#define DISP_DRAW_ROWS  40u

extern volatile uint32_t g_p4_lvgl_heap_free;

static EXT_RAM_BSS_ATTR uint16_t s_draw_buf[DISP_HOR_RES * DISP_DRAW_ROWS];
static EXT_RAM_BSS_ATTR char s_boot_json_buf[VGP_OTA_MAX_BYTES];

/* Fallback minimal project when /storage/project.json has not been uploaded yet */
static const char s_fallback_json[] =
    "{\"schemaVersion\":1,\"id\":\"p4-init\",\"name\":\"ESP32-P4 Ready\","
    "\"startScreenId\":\"main\",\"variables\":[],\"screens\":[{\"id\":\"main\","
    "\"name\":\"Main\",\"width\":1024,\"height\":600,\"background\":\"#0f172a\","
    "\"elements\":[{\"id\":\"t1\",\"kind\":\"text\",\"x\":180,\"y\":240,\"width\":664,\"height\":60,"
    "\"text\":\"ViewGP ESP32-P4 Ready — 請透過 Port 8080 上傳 project.json\","
    "\"fontSize\":24,\"color\":\"#38bdf8\",\"textAlign\":\"center\"}]}]}";

static void sntp_sync_cb(struct timeval *tv)
{
    if (tv && tv->tv_sec > 0) {
        sys_clock_set_utc((uint32_t)tv->tv_sec);
        ESP_LOGI(TAG, "SNTP time synchronized: %lu", (unsigned long)tv->tv_sec);
    }
}

static void init_storage_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount warning: %s", esp_err_to_name(ret));
    }
}

static const char *load_initial_project_json(size_t *out_len)
{
    FILE *fp = fopen(VGP_PROJECT_STORAGE_PATH, "rb");
    if (fp) {
        size_t n = fread(s_boot_json_buf, 1u, sizeof(s_boot_json_buf) - 1u, fp);
        fclose(fp);
        if (n > 16u) {
            s_boot_json_buf[n] = '\0';
            *out_len = n;
            ESP_LOGI(TAG, "Loaded %u bytes from %s", (unsigned)n, VGP_PROJECT_STORAGE_PATH);
            return s_boot_json_buf;
        }
    }
    *out_len = strlen(s_fallback_json);
    return s_fallback_json;
}

static void display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* Board BSP hook: call esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
     * area->x2 + 1, area->y2 + 1, px_map) for your MIPI-DSI or RGB panel here. */
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* Board BSP hook: read GT911 / FT5x06 via esp_lcd_touch_read_data() here. */
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void gui_task(void *arg)
{
    (void)arg;
    lv_init();
    lv_tick_set_cb(vgp_plat_now_ms);

    lv_display_t *disp = lv_display_create((int32_t)DISP_HOR_RES, (int32_t)DISP_VER_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof(s_draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, display_flush_cb);

    lv_group_t *kbd_group = lv_group_create();
    lv_indev_t *touch_in = lv_indev_create();
    lv_indev_set_type(touch_in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_in, touch_read_cb);

    size_t json_len = 0u;
    const char *json = load_initial_project_json(&json_len);
    if (!vgp_runtime_init(json, json_len, DISP_HOR_RES, DISP_VER_RES, kbd_group)) {
        ESP_LOGE(TAG, "project.json parse failed: %s", vgp_runtime_last_error());
    }

    uint32_t last_ui_sync_ms = 0u;
    for (;;) {
        const char *ota_json = NULL;
        size_t ota_len = 0u;
        if (esp_http_ops_take_ota(&ota_json, &ota_len)) {
            bool ok = vgp_runtime_is_active()
                ? vgp_runtime_reload(ota_json, ota_len)
                : vgp_runtime_init(ota_json, ota_len, DISP_HOR_RES, DISP_VER_RES, kbd_group);
            esp_http_ops_ota_done(ok);
        }

        uint32_t now = vgp_plat_now_ms();
        if ((uint32_t)(now - last_ui_sync_ms) >= 250u) {
            last_ui_sync_ms = now;
            vgp_runtime_update();
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            g_p4_lvgl_heap_free = (uint32_t)mon.free_size;
        }

        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 5u) wait_ms = 5u;
        if (wait_ms > 15u) wait_ms = 15u;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    sys_clock_init(8); /* Default UTC+8 (Taiwan) */
    init_storage_spiffs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Configure SNTP once Ethernet (RMII) or ESP-Hosted C6 Wi-Fi obtains an IP */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb = sntp_sync_cb;
    esp_netif_sntp_init(&sntp_cfg);

    esp_http_ops_start(NULL);

    /* Pin Modbus/IO polling to Core 0 and dedicated LVGL ViewGP GUI rendering to Core 1 */
    xTaskCreatePinnedToCore(vgp_modbus_task, "vgp_modbus", 8192, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(gui_task, "vgp_gui", 16384, NULL, 5, NULL, 1);
}
