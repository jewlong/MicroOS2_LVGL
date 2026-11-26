/* SPDX-License-Identifier: Apache-2.0 */
#include "mouse_queue.h"
#include <ucos_ii.h>

volatile uint32_t usb_mouse_connected = 0u;
volatile uint32_t usb_mouse_reports = 0u;

static volatile int32_t g_acc_dx = 0;
static volatile int32_t g_acc_dy = 0;
static volatile bool g_left_btn = false;
static volatile bool g_latched_press = false;
static volatile bool g_last_polled_left = false;
static volatile bool g_dirty = false;

void mouse_post(int16_t dx, int16_t dy, uint8_t buttons)
{
    bool left = (buttons & 1u) != 0u;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_acc_dx += (int32_t)dx;
    g_acc_dy += (int32_t)dy;
    if (left) g_latched_press = true;
    g_left_btn = left;
    g_dirty = true;
    usb_mouse_reports++;
    OS_CPU_SR_Restore(sr);
}

void mouse_reset(void)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_acc_dx = 0;
    g_acc_dy = 0;
    g_left_btn = false;
    g_latched_press = false;
    g_last_polled_left = false;
    g_dirty = false;
    usb_mouse_connected = 0u;
    OS_CPU_SR_Restore(sr);
}

bool mouse_poll(int32_t *out_dx, int32_t *out_dy, bool *out_left)
{
    bool had_event = false;
    bool report_left = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (out_dx) *out_dx = g_acc_dx;
    if (out_dy) *out_dy = g_acc_dy;
    if (!g_last_polled_left && g_latched_press) {
        report_left = true;
        g_latched_press = false;
    } else {
        report_left = g_left_btn;
        g_latched_press = false;
    }
    g_last_polled_left = report_left;
    if (out_left) *out_left = report_left;
    had_event = g_dirty;
    g_acc_dx = 0;
    g_acc_dy = 0;
    g_dirty = (report_left && !g_left_btn);
    OS_CPU_SR_Restore(sr);
    return had_event;
}
