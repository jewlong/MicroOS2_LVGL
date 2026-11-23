/* SPDX-License-Identifier: Apache-2.0 */
#include "touch_queue.h"
#include <ucos_ii.h>

#define TOUCH_Q_CAP 16u

typedef struct {
    uint16_t x_norm;
    uint16_t y_norm;
    uint8_t pressed;
    uint8_t reset;
} touch_evt_t;

volatile uint32_t usb_touch_connected = 0u;
volatile uint32_t usb_touch_reports = 0u;
volatile uint32_t usb_touch_x_max = 0u;
volatile uint32_t usb_touch_y_max = 0u;

static touch_evt_t g_q[TOUCH_Q_CAP];
static uint8_t g_head = 0u;
static uint8_t g_tail = 0u;
static uint8_t g_count = 0u;
static uint16_t g_last_x = 0u;
static uint16_t g_last_y = 0u;
static bool g_last_pressed = false;

void touch_post(uint16_t x_norm, uint16_t y_norm, bool pressed)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    usb_touch_reports++;

    /* Coalesce consecutive move events while pressed, but never drop press/release edges. */
    if (g_count > 0u) {
        uint8_t prev = (uint8_t)((g_tail + TOUCH_Q_CAP - 1u) % TOUCH_Q_CAP);
        if (!g_q[prev].reset && g_q[prev].pressed == (pressed ? 1u : 0u)) {
            if (pressed) {
                g_q[prev].x_norm = x_norm;
                g_q[prev].y_norm = y_norm;
            }
            OS_CPU_SR_Restore(sr);
            return;
        }
    } else if (!pressed && !g_last_pressed) {
        /* Ignore redundant idle release packets when already released. */
        OS_CPU_SR_Restore(sr);
        return;
    }

    if (g_count >= TOUCH_Q_CAP) {
        /* Drop oldest event to keep latest state fresh. */
        g_head = (uint8_t)((g_head + 1u) % TOUCH_Q_CAP);
        g_count--;
    }
    g_q[g_tail].x_norm = x_norm;
    g_q[g_tail].y_norm = y_norm;
    g_q[g_tail].pressed = pressed ? 1u : 0u;
    g_q[g_tail].reset = 0u;
    g_tail = (uint8_t)((g_tail + 1u) % TOUCH_Q_CAP);
    g_count++;
    OS_CPU_SR_Restore(sr);
}

void touch_reset(void)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_head = 0u;
    g_tail = 0u;
    g_count = 0u;
    usb_touch_connected = 0u;
    usb_touch_x_max = 0u;
    usb_touch_y_max = 0u;
    if (g_last_pressed) {
        /* Emit a reset event so LVGL cancels in-progress click without mis-triggering on unplug. */
        g_q[0].x_norm = g_last_x;
        g_q[0].y_norm = g_last_y;
        g_q[0].pressed = 0u;
        g_q[0].reset = 1u;
        g_tail = 1u;
        g_count = 1u;
    }
    OS_CPU_SR_Restore(sr);
}

bool touch_poll(uint16_t *out_x_norm, uint16_t *out_y_norm, bool *out_pressed, bool *out_reset)
{
    bool more = false;
    bool reset = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (g_count > 0u) {
        touch_evt_t ev = g_q[g_head];
        g_head = (uint8_t)((g_head + 1u) % TOUCH_Q_CAP);
        g_count--;
        if (ev.pressed) {
            g_last_x = ev.x_norm;
            g_last_y = ev.y_norm;
        }
        g_last_pressed = (ev.pressed != 0u);
        reset = (ev.reset != 0u);
        more = (g_count > 0u);
    }
    if (out_x_norm) *out_x_norm = g_last_x;
    if (out_y_norm) *out_y_norm = g_last_y;
    if (out_pressed) *out_pressed = g_last_pressed;
    if (out_reset) *out_reset = reset;
    OS_CPU_SR_Restore(sr);
    return more;
}
