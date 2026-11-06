/* SPDX-License-Identifier: Apache-2.0 */
#include <ucos_ii.h>
#include "lvgl.h"
#include "gui.h"
#include "net_platform.h"
#include "framebuffer.h"
#include "bsp.h"
#include "uart.h"
#include "port.h"

_Static_assert(OS_TICKS_PER_SEC == 100u, "GUI time conversion requires 100 Hz");
_Static_assert(LV_USE_OS == LV_OS_NONE && LV_DRAW_SW_DRAW_UNIT_CNT == 1, "GUI must own LVGL");
_Static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 4 && LVGL_VERSION_PATCH == 0, "Pinned LVGL 9.4.0 required");

#define GUI_QUEUE_LEN 16u
#define GUI_DRAW_ROWS 32u
#define BG 0x101a2bu
#define PANEL 0x1c2b40u
#define WHITE 0xf1f5f9u
#define MUTED 0x9caec5u
#define TEAL 0x50dfb6u
#define ORANGE 0xffb86bu

_Alignas(8) static uint32_t draw_pixels[FB_WIDTH * GUI_DRAW_ROWS];
static void *queue_storage[GUI_QUEUE_LEN];
static OS_EVENT *gui_queue;
/* Immutable static messages: queue pointers remain valid until consumed,
 * even when the same command is posted repeatedly. No borrowed stack data. */
static char command_plus = '+', command_minus = '-', command_reset = 'r';
static lv_obj_t *wifi_label;
static lv_obj_t *count_label, *status_label, *tick_label, *work_label, *load_bar;
static lv_obj_t *plus_button, *minus_button, *reset_button;
volatile uint32_t app_display_frames, gui_ready, gui_commands, gui_dropped;
volatile uint32_t gui_value = 50u;
volatile uint32_t gui_button_events, gui_heap_free, gui_stack_free, gui_owner_checks;
extern volatile uint32_t app_preempt_pass, app_high_runs, app_low_completed;

static void owner_check(void)
{
    if (OSPrioCur != APP_GUI_PRIO) { bsp_panic("LVGL called outside GUI task"); }
    ++gui_owner_checks;
}

_Noreturn void gui_fault(void)
{
    gui_ready = 0u;
    bsp_log_lock();
    uart_puts("[GUI] LVGL assertion failed; GUI stopped, UART/RTOS continue.\n");
    bsp_log_unlock();
    /* Do not unwind into partially updated LVGL state after an assertion. */
    for (;;) { (void)OSTaskSuspend(OS_PRIO_SELF); }
}

void gui_queue_init(void)
{
    gui_queue = OSQCreate(queue_storage, GUI_QUEUE_LEN);
    if (gui_queue == (OS_EVENT *)0) { bsp_panic("GUI queue allocation"); }
}

bool gui_post_char(char ch)
{
    char *message;
    if (ch == '+') { message = &command_plus; }
    else if (ch == '-') { message = &command_minus; }
    else if (ch == 'r' || ch == 'R') { message = &command_reset; }
    else { return false; }
    /* Called only by UART task. This function makes NO LVGL calls. */
    if (OSQPost(gui_queue, message) != OS_ERR_NONE) { ++gui_dropped; }
    return true;
}

static uint32_t gui_milliseconds(void)
{
    return (uint32_t)OSTimeGet() * (1000u / OS_TICKS_PER_SEC);
}

static void flush_pixels(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    owner_check();
    const framebuffer_t *fb = framebuffer_info();
    /* LVGL reshapes partial buffers for each dirty rectangle. Read its
     * actual source stride, independently from the firmware's output pitch. */
    const uint32_t stride = lv_display_get_buf_active(display)->header.stride;
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    if (area->x1 < 0 || area->y1 < 0 || width <= 0 || height <= 0 ||
        area->x2 >= (int32_t)fb->width || area->y2 >= (int32_t)fb->height ||
        stride < (uint32_t)width * 4u ||
        stride * (uint32_t)height > sizeof(draw_pixels)) {
        gui_fault();
    }
    for (int32_t y = 0; y < height; ++y) {
        const uint32_t *src = (const uint32_t *)(const void *)(pixels + (uint32_t)y * stride);
        volatile uint32_t *dst = (volatile uint32_t *)((uintptr_t)fb->pixels +
            ((uint32_t)area->y1 + (uint32_t)y) * fb->pitch) + area->x1;
        for (int32_t x = 0; x < width; ++x) {
            const uint32_t rgb = src[x] & 0x00ffffffu; /* XRGB8888 */
            dst[x] = 0xff000000u | ((fb->pixel_order == 0u) ? rgb :
                ((rgb & 0xffu) << 16) | (rgb & 0xff00u) | ((rgb >> 16) & 0xffu));
        }
    }
    fb_present(); /* Synchronous CPU copy, no outstanding DMA. */
    if (lv_display_flush_is_last(display)) { ++app_display_frames; }
    lv_display_flush_ready(display);
}

static lv_obj_t *label(lv_obj_t *parent, int32_t x, int32_t y,
                       const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static void box_style(lv_obj_t *obj, uint32_t color, int32_t radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *root, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(root);
    box_style(obj, PANEL, 12);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

static void update_value(void)
{
    lv_label_set_text_fmt(count_label, "%lu", (unsigned long)gui_value);
    lv_bar_set_value(load_bar, (int32_t)gui_value, LV_ANIM_OFF);
}

static void button_event(lv_event_t *event)
{
    owner_check();
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == plus_button && gui_value < 100u) { ++gui_value; }
    else if (target == minus_button && gui_value > 0u) { --gui_value; }
    else if (target == reset_button) { gui_value = 50u; }
    ++gui_button_events;
    update_value();
}

static lv_obj_t *button(lv_obj_t *parent, int32_t x, int32_t w,
                        const char *text, uint32_t color)
{
    lv_obj_t *obj = lv_button_create(parent);
    box_style(obj, color, 8);
    lv_obj_set_pos(obj, x, 154);
    lv_obj_set_size(obj, w, 44);
    lv_obj_t *caption = label(obj, 0, 0, text, &lv_font_montserrat_16, BG);
    lv_obj_center(caption);
    lv_obj_add_event_cb(obj, button_event, LV_EVENT_CLICKED, NULL);
    return obj;
}

static void create_screen(void)
{
    lv_obj_t *root = lv_screen_active();
    box_style(root, BG, 0);
    label(root, 24, 18, "PI ZERO W  /  REAL-TIME GUI", &lv_font_montserrat_14, TEAL);
    label(root, 24, 43, "LVGL + uC/OS-II", &lv_font_montserrat_28, WHITE);
    wifi_label = label(root, 24, 80, "Wi-Fi: starting", &lv_font_montserrat_14, MUTED);

    lv_obj_t *p = panel(root, 24, 112, 186, 82);
    label(p, 14, 12, "SCHEDULER", &lv_font_montserrat_14, MUTED);
    label(p, 14, 38, "PREEMPT: PASS", &lv_font_montserrat_16, TEAL);
    p = panel(root, 222, 112, 186, 82);
    label(p, 14, 12, "OS TICKS / 100 Hz", &lv_font_montserrat_14, MUTED);
    tick_label = label(p, 14, 36, "0", &lv_font_montserrat_28, WHITE);
    p = panel(root, 420, 112, 196, 82);
    label(p, 14, 12, "WORKER CYCLES", &lv_font_montserrat_14, MUTED);
    work_label = label(p, 14, 36, "0", &lv_font_montserrat_28, WHITE);

    p = panel(root, 24, 210, 592, 216);
    label(p, 18, 14, "QUEUED BUTTON CONTROL", &lv_font_montserrat_14, MUTED);
    label(p, 18, 42, "UART task > queue > GUI task", &lv_font_montserrat_16, WHITE);
    count_label = label(p, 493, 35, "50", &lv_font_montserrat_28, ORANGE);
    load_bar = lv_bar_create(p);
    box_style(load_bar, BG, 6);
    lv_obj_set_pos(load_bar, 18, 87);
    lv_obj_set_size(load_bar, 556, 18);
    lv_obj_set_style_bg_color(load_bar, lv_color_hex(ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(load_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(load_bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(load_bar, 0, 100);
    label(p, 18, 118, "Send a key in the serial terminal:", &lv_font_montserrat_14, MUTED);
    minus_button = button(p, 18, 146, "[-]  Decrease", TEAL);
    plus_button = button(p, 176, 146, "[+]  Increase", ORANGE);
    reset_button = button(p, 334, 240, "[R]  Reset to 50", 0xb4c8e3u);
    status_label = label(root, 24, 441, "Starting GUI...", &lv_font_montserrat_14, MUTED);
    update_value();
}

static void update_status(void)
{
    char wifi[80]; network_snapshot(wifi,sizeof(wifi)); lv_label_set_text(wifi_label,wifi);
    owner_check();
    lv_label_set_text_fmt(tick_label, "%lu", (unsigned long)OSTimeGet());
    lv_label_set_text_fmt(work_label, "%lu", (unsigned long)app_low_completed);
    lv_label_set_text_fmt(status_label, "GUI frames %lu   |   Events %lu   |   Queue drops %lu",
        (unsigned long)app_display_frames, (unsigned long)gui_commands, (unsigned long)gui_dropped);
    if (lv_mem_test() != LV_RESULT_OK) { gui_fault(); }
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    gui_heap_free = (uint32_t)mem.free_size;
    OS_STK_DATA stack;
    if (OSTaskStkChk(APP_GUI_PRIO, &stack) == OS_ERR_NONE) {
        gui_stack_free = (uint32_t)stack.OSFree * sizeof(OS_STK);
    }
}

void gui_task(void *arg)
{
    (void)arg;
    while (app_preempt_pass == 0u) { OSTimeDly(1u); }
    if (!framebuffer_ready()) {
        bsp_log_lock();
        uart_puts("[GUI] disabled: HDMI framebuffer unavailable.\n");
        bsp_log_unlock();
        for (;;) {
            INT8U err;
            (void)OSQPend(gui_queue, 0u, &err); /* Drain commands without touching LVGL. */
        }
    }
    owner_check();
    lv_init();
    lv_tick_set_cb(gui_milliseconds); /* The only LVGL time source; no lv_tick_inc. */
    lv_display_t *display = lv_display_create(FB_WIDTH, FB_HEIGHT);
    if (display == NULL) { gui_fault(); }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_pixels, NULL, sizeof(draw_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_pixels);
    create_screen();
    gui_ready = 1u;
    bsp_log_lock();
    uart_puts("[GUI] LVGL 9.4.0 ready; dedicated GUI task, keys + - r.\n");
    bsp_log_unlock();
    uint32_t last_status = OSTimeGet() - 25u;
    for (;;) {
        owner_check();
        /* Bounded work: a continuously busy sender cannot starve rendering. */
        for (uint32_t i = 0; i < GUI_QUEUE_LEN; ++i) {
            INT8U err;
            const char *message = (const char *)OSQAccept(gui_queue, &err);
            if (err == OS_ERR_Q_EMPTY) { break; }
            if (err != OS_ERR_NONE || message == NULL) { gui_fault(); }
            lv_obj_t *target = (*message == '+') ? plus_button :
                               (*message == '-') ? minus_button : reset_button;
            (void)lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
            ++gui_commands;
        }
        const uint32_t now = OSTimeGet();
        if ((uint32_t)(now - last_status) >= 25u) {
            last_status = now;
            update_status();
        }
        uint32_t wait_ms = lv_timer_handler();
        /* Yield at least one OS tick; cap latency for queued UART commands.
         * Frame rendering time is additional, not a guaranteed frame rate. */
        if (wait_ms > 20u) { wait_ms = 20u; }
        const INT32U delay = (wait_ms + 9u) / 10u;
        OSTimeDly(delay == 0u ? 1u : delay);
    }
}
