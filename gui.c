/* SPDX-License-Identifier: Apache-2.0 */
#include <ucos_ii.h>
#include "lvgl.h"
#include "gui.h"
#include "keyboard.h"
#include "mouse_queue.h"
#include "touch_queue.h"
#include "net_platform.h"
#include "framebuffer.h"
#include "bootcfg.h"
#include "vgp_runtime.h"
#include "http_ops.h"
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

_Alignas(8) static uint32_t draw_pixels[FB_MAX_WIDTH * GUI_DRAW_ROWS];
static void *queue_storage[GUI_QUEUE_LEN];
static OS_EVENT *gui_queue;
/* Immutable static messages: queue pointers remain valid until consumed,
 * even when the same command is posted repeatedly. No borrowed stack data. */
static char command_plus = '+', command_minus = '-', command_reset = 'r';
static lv_obj_t *wifi_label, *keyboard_label;
static lv_group_t *keyboard_group;
volatile uint32_t gui_usb_events, gui_usb_resets, gui_usb_focus, gui_usb_down;
volatile uint32_t gui_touch_events, gui_touch_resets, gui_touch_down;
char gui_usb_text[64];
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


static void keyboard_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last_key=LV_KEY_ENTER;
    static uint8_t active_usage;
    static bool down;
    key_event_t event;
    owner_check();
    data->key=last_key;data->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; gui_usb_down=down?1u:0u;
    data->continue_reading=false;
    if(!keyboard_pop(&event))return;
    data->continue_reading=true;++gui_usb_events;
    if(event.reset){
        down=false;active_usage=0;++gui_usb_resets;
        lv_indev_reset(indev,NULL);
        lv_indev_wait_release(indev); /* cancel Enter: unplug is not a click */
        lv_obj_t *focused=lv_group_get_focused(keyboard_group);
        if(focused)lv_obj_remove_state(focused,LV_STATE_PRESSED);
    }else if(event.pressed){
        uint32_t key=event.key;
        /* Text is a bounded demonstration line, not a terminal/IME. */
        if(!(event.modifiers&0xddu)){
            size_t n=lv_strlen(gui_usb_text);
            if(key>=32 && key<127){
                if(n==sizeof(gui_usb_text)-1){lv_memmove(gui_usb_text,gui_usb_text+1,n-1);--n;}
                gui_usb_text[n++]=(char)key;gui_usb_text[n]=0;
            }else if(key==KEY_BACKSPACE && n)gui_usb_text[n-1]=0;
            if(keyboard_label)lv_label_set_text_fmt(keyboard_label,"USB text: %s",gui_usb_text);
            lv_obj_t *target=key=='+'?plus_button:key=='-'?minus_button:
                (key=='r' || key=='R')?reset_button:NULL;
            if(target){lv_obj_send_event(target,LV_EVENT_CLICKED,NULL);++gui_commands;}
            if(vgp_runtime_is_active())vgp_runtime_handle_key(key);
        }
        if(key==KEY_RIGHT || key==KEY_DOWN)key=LV_KEY_NEXT;
        if(key==KEY_LEFT || key==KEY_UP)key=LV_KEY_PREV;
        if(key==' ' && vgp_runtime_is_active())key=LV_KEY_ENTER;
        if(!down && (key==LV_KEY_NEXT || key==LV_KEY_PREV || key==LV_KEY_ENTER || key==LV_KEY_ESC)){
            last_key=key;active_usage=event.usage;down=true;
        }
    }else if(event.usage==active_usage){down=false;active_usage=0;}
    data->key=last_key;data->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; gui_usb_down=down?1u:0u;
}

static lv_obj_t *mouse_cursor;
static int32_t mouse_x = 160, mouse_y = 120;

static void mouse_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    owner_check();
    int32_t dx = 0, dy = 0;
    bool left = false;
    bool moved = mouse_poll(&dx, &dy, &left);
    if (moved || (usb_mouse_connected && !mouse_cursor)) {
        const framebuffer_t *fb = framebuffer_info();
        mouse_x += dx;
        mouse_y += dy;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int32_t)fb->width) mouse_x = (int32_t)fb->width - 1;
        if (mouse_y >= (int32_t)fb->height) mouse_y = (int32_t)fb->height - 1;
        if (!mouse_cursor) {
            mouse_cursor = lv_obj_create(lv_layer_sys());
            lv_obj_remove_style_all(mouse_cursor);
            lv_obj_set_size(mouse_cursor, 12, 12);
            lv_obj_set_style_bg_color(mouse_cursor, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(mouse_cursor, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(mouse_cursor, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(mouse_cursor, 2, 0);
            lv_obj_set_style_radius(mouse_cursor, LV_RADIUS_CIRCLE, 0);
            lv_obj_remove_flag(mouse_cursor, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(mouse_cursor, LV_OBJ_FLAG_SCROLLABLE);
            lv_indev_set_cursor(indev, mouse_cursor);
        }
    }
    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = left ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool prev_pressed = false;
    static uint16_t prev_nx = 0u, prev_ny = 0u;
    owner_check();
    uint16_t nx = 0u, ny = 0u;
    bool pressed = false, reset = false;
    bool more = touch_poll(&nx, &ny, &pressed, &reset);

    if (reset) {
        ++gui_touch_resets;
        ++gui_touch_events;
        prev_pressed = false;
        lv_indev_reset(indev, NULL);
        lv_indev_wait_release(indev);
    } else if (pressed != prev_pressed || (pressed && (nx != prev_nx || ny != prev_ny))) {
        ++gui_touch_events;
        prev_pressed = pressed;
        prev_nx = nx;
        prev_ny = ny;
    }
    gui_touch_down = pressed ? 1u : 0u;

    if (bootcfg_status() == BOOTCFG_OK) {
        const wifi_settings_t *cfg = bootcfg_wifi();
        if (cfg->touch_swap_xy) {
            uint16_t tmp = nx;
            nx = ny;
            ny = tmp;
        }
        if (cfg->touch_invert_x) nx = (uint16_t)(65535u - nx);
        if (cfg->touch_invert_y) ny = (uint16_t)(65535u - ny);
    }

    const framebuffer_t *fb = framebuffer_info();
    uint32_t max_x = (fb->width > 1u) ? (fb->width - 1u) : 0u;
    uint32_t max_y = (fb->height > 1u) ? (fb->height - 1u) : 0u;
    data->point.x = (int32_t)(((uint32_t)nx * max_x + 32767u) / 65535u);
    data->point.y = (int32_t)(((uint32_t)ny * max_y + 32767u) / 65535u);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = more;
}
static void create_screen(uint32_t screen_w, uint32_t screen_h)
{
    lv_obj_t *root = lv_screen_active();
    box_style(root, BG, 0);
    label(root, 24, 18, "PI ZERO W  /  REAL-TIME GUI", &lv_font_montserrat_14, TEAL);
    label(root, 24, 43, "LVGL + uC/OS-II", &lv_font_montserrat_28, WHITE);
    wifi_label = label(root, 24, 80, "Wi-Fi: starting", &lv_font_montserrat_14, MUTED);

    /* Layout adapts to the queried resolution (minimum 640x480). */
    const int32_t content_w = (int32_t)screen_w - 48;
    const int32_t card_w = (content_w - 24) / 3;
    lv_obj_t *p = panel(root, 24, 112, card_w, 82);
    label(p, 14, 12, "SCHEDULER", &lv_font_montserrat_14, MUTED);
    label(p, 14, 38, "PREEMPT: PASS", &lv_font_montserrat_16, TEAL);
    p = panel(root, 24 + card_w + 12, 112, card_w, 82);
    label(p, 14, 12, "OS TICKS / 100 Hz", &lv_font_montserrat_14, MUTED);
    tick_label = label(p, 14, 36, "0", &lv_font_montserrat_28, WHITE);
    p = panel(root, 24 + 2 * (card_w + 12), 112, content_w - 2 * (card_w + 12), 82);
    label(p, 14, 12, "WORKER CYCLES", &lv_font_montserrat_14, MUTED);
    work_label = label(p, 14, 36, "0", &lv_font_montserrat_28, WHITE);

    p = panel(root, 24, 210, content_w, 216);
    label(p, 18, 14, "QUEUED BUTTON CONTROL", &lv_font_montserrat_14, MUTED);
    label(p, 18, 42, "USB / UART > queue > GUI task", &lv_font_montserrat_16, WHITE);
    count_label = label(p, content_w - 99, 35, "50", &lv_font_montserrat_28, ORANGE);
    load_bar = lv_bar_create(p);
    box_style(load_bar, BG, 6);
    lv_obj_set_pos(load_bar, 18, 87);
    lv_obj_set_size(load_bar, content_w - 36, 18);
    lv_obj_set_style_bg_color(load_bar, lv_color_hex(ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(load_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(load_bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(load_bar, 0, 100);
    keyboard_label = label(p, 18, 118, "USB text: (Tab / arrows / Enter)", &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(keyboard_label,content_w - 36);
    lv_label_set_long_mode(keyboard_label,LV_LABEL_LONG_MODE_CLIP);
    minus_button = button(p, 18, 146, "[-]  Decrease", TEAL);
    plus_button = button(p, 176, 146, "[+]  Increase", ORANGE);
    reset_button = button(p, 334, 240, "[R]  Reset to 50", 0xb4c8e3u);
    status_label = label(root, 24, (int32_t)screen_h - 39, "Starting GUI...", &lv_font_montserrat_14, MUTED);
    if (keyboard_group) {
        lv_group_add_obj(keyboard_group,minus_button);
        lv_group_add_obj(keyboard_group,plus_button);
        lv_group_add_obj(keyboard_group,reset_button);
    }
    lv_obj_t *buttons[]={minus_button,plus_button,reset_button};
    for(unsigned i=0;i<3;i++){
        lv_obj_set_style_border_width(buttons[i],2,LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(buttons[i],lv_color_hex(WHITE),LV_STATE_FOCUSED);
    }
    update_value();
}

static void update_status(void)
{
    owner_check();
    if (!vgp_runtime_is_active()) {
        char wifi[128]; network_snapshot(wifi,sizeof(wifi));
        const bootcfg_status_t cfg = bootcfg_status();
        const char *cfg_short = (cfg == BOOTCFG_OK) ? "wifi.txt OK" : bootcfg_status_text(cfg);
        lv_label_set_text_fmt(wifi_label, "[CFG: %s]  %s  |  RX:%lu TX:%lu",
            cfg_short, wifi, (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
        lv_obj_t *focus=lv_group_get_focused(keyboard_group);
        gui_usb_focus=focus==minus_button?0:focus==plus_button?1:2;
        lv_label_set_text_fmt(tick_label, "%lu", (unsigned long)OSTimeGet());
        lv_label_set_text_fmt(work_label, "%lu", (unsigned long)app_low_completed);
        lv_label_set_text_fmt(status_label, "Frames %lu | USB st:%lu keys:%lu | Events %lu | Drops %lu",
            (unsigned long)app_display_frames, (unsigned long)usb_state, (unsigned long)usb_key_presses,
            (unsigned long)gui_commands, (unsigned long)gui_dropped);
    } else {
        vgp_runtime_update();
    }
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
    const framebuffer_t *fb = framebuffer_info();
    lv_display_t *display = lv_display_create((int32_t)fb->width, (int32_t)fb->height);
    if (display == NULL) { gui_fault(); }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_pixels, NULL,
                           fb->width * GUI_DRAW_ROWS * 4u, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_pixels);
    keyboard_group = lv_group_create();
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(input, keyboard_read);
    lv_indev_set_group(input, keyboard_group);

    lv_indev_t *mouse_in = lv_indev_create();
    lv_indev_set_type(mouse_in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(mouse_in, mouse_read);

    lv_indev_t *touch_in = lv_indev_create();
    lv_indev_set_type(touch_in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_in, touch_read);

    size_t proj_len = 0;
    const char *proj_json = bootcfg_project_json(&proj_len);
    bool vgp_loaded = false;
    if (proj_json && proj_len > 0) {
        vgp_loaded = vgp_runtime_init(proj_json, proj_len, fb->width, fb->height, keyboard_group);
    }
    if (!vgp_loaded) {
        create_screen(fb->width, fb->height);
        if (proj_json && proj_len > 0u) {
            const char *perr = vgp_runtime_last_error();
            lv_obj_t *err_bar = lv_label_create(lv_screen_active());
            lv_obj_set_pos(err_bar, 24, (int32_t)fb->height - 62);
            lv_obj_set_style_text_font(err_bar, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(err_bar, lv_color_hex(0xf87171u), 0);
            lv_label_set_text_fmt(err_bar, "project.json load error: %s",
                                  (perr && perr[0]) ? perr : "invalid JSON / no screens");
        }
    }
    gui_ready = 1u;
    bsp_log_lock();
    if (vgp_loaded) {
        uart_puts("[GUI] ViewGP Project running: ");
        uart_puts(vgp_runtime_project_name());
        uart_puts("\n");
    } else {
        uart_puts("[GUI] LVGL 9.4.0 ready; dedicated GUI task, keys + - r.\n");
    }
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
            if (target != NULL) {
                (void)lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
            }
            ++gui_commands;
        }
        const uint32_t now = OSTimeGet();
        const char *ota_json = NULL;
        size_t ota_len = 0u;
        if (http_ops_take_ota(&ota_json, &ota_len)) {
            if (vgp_runtime_is_active()) {
                (void)vgp_runtime_reload(ota_json, ota_len);
            } else {
                (void)vgp_runtime_init(ota_json, ota_len, fb->width, fb->height, keyboard_group);
            }
            http_ops_ota_done();
        }
        if ((uint32_t)(now - last_status) >= 25u) {
            last_status = now;
            update_status();
        }
        uint32_t wait_ms = lv_timer_handler();
        /* Yield at least one OS tick; cap latency for queued UART commands.
         * Frame rendering time is additional, not a guaranteed frame rate. */
        if (wait_ms > 10u) { wait_ms = 10u; }
        const INT32U delay = (wait_ms + 9u) / 10u;
        OSTimeDly(delay == 0u ? 1u : delay);
    }
}
