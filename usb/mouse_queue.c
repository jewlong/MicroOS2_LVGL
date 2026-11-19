/* SPDX-License-Identifier: Apache-2.0 */
#include "mouse_queue.h"
#include <ucos_ii.h>

volatile uint32_t usb_mouse_connected = 0u;
volatile uint32_t usb_mouse_reports = 0u;

static volatile int32_t g_acc_dx = 0;
static volatile int32_t g_acc_dy = 0;
static volatile bool g_left_btn = false;
static volatile bool g_dirty = false;

void mouse_post(int8_t dx, int8_t dy, uint8_t buttons)
{
    OS_CPU_SR sr = OS_CPU_SR_Save();
    g_acc_dx += (int32_t)dx;
    g_acc_dy += (int32_t)dy;
    g_left_btn = (buttons & 1u) != 0u;
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
    g_dirty = false;
    usb_mouse_connected = 0u;
    OS_CPU_SR_Restore(sr);
}

bool mouse_poll(int32_t *out_dx, int32_t *out_dy, bool *out_left)
{
    bool had_event = false;
    OS_CPU_SR sr = OS_CPU_SR_Save();
    if (out_dx) *out_dx = g_acc_dx;
    if (out_dy) *out_dy = g_acc_dy;
    if (out_left) *out_left = g_left_btn;
    had_event = g_dirty;
    g_acc_dx = 0;
    g_acc_dy = 0;
    g_dirty = false;
    OS_CPU_SR_Restore(sr);
    return had_event;
}
