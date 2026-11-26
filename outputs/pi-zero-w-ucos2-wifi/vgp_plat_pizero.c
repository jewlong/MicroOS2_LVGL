/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_platform.h"
#include <ucos_ii.h>
#include "platform.h"
#include "uart.h"
#include "net_platform.h"
#include "bootcfg.h"
#include "keyboard.h"
#include "mouse_queue.h"
#include "touch_queue.h"
#include <stdio.h>
#include <string.h>

extern char gui_usb_text[64];
extern volatile uint32_t gui_heap_free;

uint32_t vgp_plat_now_ms(void)
{
    return (uint32_t)OSTimeGet() * (1000u / (uint32_t)OS_TICKS_PER_SEC);
}

uint32_t vgp_plat_uptime_sec(void)
{
    return (uint32_t)OSTimeGet() / (uint32_t)OS_TICKS_PER_SEC;
}

uint32_t vgp_plat_enter_critical(void)
{
    return (uint32_t)OS_CPU_SR_Save();
}

void vgp_plat_exit_critical(uint32_t state)
{
    OS_CPU_SR_Restore((OS_CPU_SR)state);
}

void vgp_plat_log(const char *msg)
{
    if (!msg) return;
    uart_puts("[VGP] ");
    uart_puts(msg);
    uart_puts("\n");
}

void vgp_plat_get_status(vgp_plat_status_t *out, bool include_detail)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->platform_short = soc_is_bcm2837 ? "Zero2W" : "ZeroW";
    out->platform_full  = soc_is_bcm2837 ? "Pi Zero 2 W" : "Pi Zero W";
    out->uptime_sec     = vgp_plat_uptime_sec();
    out->lvgl_heap_free = gui_heap_free;
    out->input_state    = usb_state;
    out->key_presses    = usb_key_presses;
    out->mouse_connected = (usb_mouse_connected != 0u);
    out->mouse_reports  = usb_mouse_reports;
    out->touch_connected = (usb_touch_connected != 0u);
    out->touch_reports  = usb_touch_reports;
    if (include_detail) {
        usb_diag_line(out->input_detail, sizeof(out->input_detail));
    }
}

void vgp_plat_format_status_bar(const char *modbus_summary, bool ntp_synced,
                                char *out, size_t max_out, uint32_t *out_color_rgb)
{
    if (!out || max_out == 0u) return;

    char wifi_str[128];
    network_snapshot(wifi_str, sizeof(wifi_str));
    const bootcfg_status_t cfg = bootcfg_status();
    const char *cfg_short = (cfg == BOOTCFG_OK) ? "wifi.txt OK" : bootcfg_status_text(cfg);
    const char *ntp_str = ntp_synced ? " [NTP]" : "";
    const char *plat = soc_is_bcm2837 ? "Zero2W" : "ZeroW";

    char usb_str[80];
    if (usb_state == 4u) {
        if (usb_mouse_connected) {
            snprintf(usb_str, sizeof(usb_str), "USB:OK(%04lx:%04lx M:%lu k:%lu)",
                (unsigned long)(usb_vid_pid >> 16), (unsigned long)(usb_vid_pid & 0xffffu),
                (unsigned long)usb_mouse_reports, (unsigned long)usb_key_presses);
        } else {
            snprintf(usb_str, sizeof(usb_str), "USB:OK(%04lx:%04lx k:%lu \"%s\")",
                (unsigned long)(usb_vid_pid >> 16), (unsigned long)(usb_vid_pid & 0xffffu),
                (unsigned long)usb_key_presses, gui_usb_text);
        }
    } else if (usb_state == 2u) {
        snprintf(usb_str, sizeof(usb_str), "USB:wait(HPRT:%04lx)",
            (unsigned long)(usb_diag_hprt() & 0xffffu));
    } else if (usb_state == 3u) {
        snprintf(usb_str, sizeof(usb_str), "USB:enum(st:%lu)", (unsigned long)usb_step);
    } else if (usb_state == 254u) {
        snprintf(usb_str, sizeof(usb_str), "USB:err(%lu,st:%lu,v:%lx,id:%04lx:%04lx)",
            (unsigned long)usb_error, (unsigned long)usb_step, (unsigned long)usb_init_val,
            (unsigned long)(usb_vid_pid >> 16), (unsigned long)(usb_vid_pid & 0xffffu));
    } else {
        snprintf(usb_str, sizeof(usb_str), "USB:off(e:%lu,v:%08lx)",
            (unsigned long)usb_init_err, (unsigned long)usb_init_val);
    }

    if (modbus_summary && modbus_summary[0]) {
        snprintf(out, max_out, "[%s]%s [%s] [%s] [CFG: %s] %s | RX:%lu TX:%lu",
            plat, ntp_str, modbus_summary,
            usb_str, cfg_short, wifi_str,
            (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
    } else {
        snprintf(out, max_out, "[%s]%s [%s] [CFG: %s] %s | RX:%lu TX:%lu",
            plat, ntp_str,
            usb_str, cfg_short, wifi_str,
            (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
    }

    if (out_color_rgb) {
        *out_color_rgb = (net_stage == 6u) ? 0x4ade80u :
                         (net_stage == 0xffu || net_error != 0u) ? 0xf87171u : 0x38bdf8u;
    }
}
