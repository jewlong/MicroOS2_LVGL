/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_PLATFORM_H
#define VGP_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Memory section attribute for large HMI static buffers (~1.3 MiB total).
 * - On ESP-IDF (ESP32-P4 / ESP32-S3): placed in external PSRAM (.ext_ram.bss).
 * - On Pi Zero bare-metal: standard .bss in SDRAM. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifndef VGP_BSS_ATTR
#define VGP_BSS_ATTR EXT_RAM_BSS_ATTR
#endif
#else
#ifndef VGP_BSS_ATTR
#define VGP_BSS_ATTR
#endif
#endif

/* Cross-platform logical key codes passed to vgp_runtime_handle_key().
 * Numeric values match Pi Zero USB keyboard.h for drop-in compatibility. */
enum {
    VGP_KEY_BACKSPACE = 8,
    VGP_KEY_ESC       = 27,
    VGP_KEY_F12       = 140
};

/* Snapshot of platform telemetry for F12 diagnostic overlay & /api/status JSON. */
typedef struct {
    const char *platform_short;    /* e.g. "ZeroW", "Zero2W", "ESP32-P4" */
    const char *platform_full;     /* e.g. "Pi Zero W", "Pi Zero 2 W", "ESP32-P4" */
    uint32_t uptime_sec;
    uint32_t lvgl_heap_free;

    /* Input / peripheral telemetry */
    uint32_t input_state;
    uint32_t key_presses;
    bool mouse_connected;
    uint32_t mouse_reports;
    bool touch_connected;
    uint32_t touch_reports;
    char input_detail[160];        /* Platform-specific input diagnostic line */
} vgp_plat_status_t;

/* Monotonic millisecond and second clocks since boot. */
uint32_t vgp_plat_now_ms(void);
uint32_t vgp_plat_uptime_sec(void);

/* Short critical-section lock/unlock for shared timestamps (safe on single & SMP cores). */
uint32_t vgp_plat_enter_critical(void);
void vgp_plat_exit_critical(uint32_t state);

/* Diagnostic log line output (e.g. UART on Pi Zero, ESP_LOGI on ESP32-P4). */
void vgp_plat_log(const char *msg);

/* Populate platform telemetry snapshot.
 * Set include_detail=false when called from non-GUI threads (e.g. HTTP /api/status). */
void vgp_plat_get_status(vgp_plat_status_t *out, bool include_detail);

/* Format bottom status bar text and return desired RGB24 text color. */
void vgp_plat_format_status_bar(const char *modbus_summary, bool ntp_synced,
                                char *out, size_t max_out, uint32_t *out_color_rgb);

#endif
