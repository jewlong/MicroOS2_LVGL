/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_runtime.h"
#include "vgp_parser.h"
#include "net_platform.h"
#include "bootcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "vgp_font_cjk.h"
#include "platform.h"

extern char gui_usb_text[64];

static double parse_double(const char *s)
{
    if (!s) return 0.0;
    while (*s == ' ' || *s == '\t') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') { s++; }
    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    return sign * val;
}
static vgp_project_t g_proj;
static bool g_active = false;
static uint32_t g_screen_w = 640, g_screen_h = 480;
static int32_t g_current_screen_idx = 0;
static int32_t g_history[16];
static int32_t g_history_depth = 0;
static int32_t g_active_popup_idx = -1;
static lv_obj_t *g_popup_layer = NULL;
static lv_obj_t *g_status_bar = NULL;
static lv_obj_t *g_status_label = NULL;
static lv_group_t *g_kbd_group = NULL;
static uint32_t g_sec_counter = 0;

static vgp_variable_t *find_var(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (uint16_t i = 0; i < g_proj.num_variables; ++i) {
        if (strcmp(g_proj.variables[i].id, id) == 0) {
            return &g_proj.variables[i];
        }
    }
    return NULL;
}

static int32_t find_screen(const char *id)
{
    if (!id || !id[0]) return -1;
    for (uint16_t i = 0; i < g_proj.num_screens; ++i) {
        if (strcmp(g_proj.screens[i].id, id) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void calc_scaling(int32_t proj_w, int32_t proj_h, int32_t *scale_out, int32_t *off_x_out, int32_t *off_y_out)
{
    if (proj_w <= 0) proj_w = 960;
    if (proj_h <= 0) proj_h = 600;
    int32_t sx = ((int32_t)g_screen_w * 1000) / proj_w;
    int32_t sy = ((int32_t)g_screen_h * 1000) / proj_h;
    int32_t scale = (sx < sy) ? sx : sy;
    if (scale <= 0) scale = 1000;
    *scale_out = scale;
    *off_x_out = ((int32_t)g_screen_w - (proj_w * scale) / 1000) / 2;
    *off_y_out = ((int32_t)g_screen_h - (proj_h * scale) / 1000) / 2;
}

static const lv_font_t *select_font(const char *text, int32_t font_size, int32_t scale)
{
    (void)text;
    int32_t scaled_size = (font_size * scale) / 1000;
    if (scaled_size >= 19) return &vgp_font_zh_22;
    return &vgp_font_zh_16;
}

static void format_num_val(char *buf, size_t max_buf, double val, const vgp_element_t *e)
{
    char num_str[32];
    uint8_t dec = e->decimals;
    if (dec > 4) dec = 4;
    if (dec == 0) {
        long iv = (long)(val >= 0.0 ? val + 0.5 : val - 0.5);
        snprintf(num_str, sizeof(num_str), "%ld", iv);
    } else {
        long mult = 1;
        for (uint8_t i = 0; i < dec; ++i) mult *= 10;
        bool neg = (val < 0.0);
        double abs_v = neg ? -val : val;
        long scaled = (long)(abs_v * (double)mult + 0.5);
        long int_part = scaled / mult;
        long frac_part = scaled % mult;
        snprintf(num_str, sizeof(num_str), "%s%ld.%0*ld", neg ? "-" : "", int_part, (int)dec, frac_part);
    }
    if (e->unit[0]) {
        snprintf(buf, max_buf, "%s %s", num_str, e->unit);
    } else {
        snprintf(buf, max_buf, "%s", num_str);
    }
}

static void apply_box_style(lv_obj_t *obj, uint32_t bg_color, uint32_t border_color, uint16_t border_w, uint16_t radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_width(obj, border_w, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void apply_focus_style(lv_obj_t *obj)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00FACC15u), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, lv_color_hex(0x0038BDF8u), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, LV_STATE_PRESSED);
}

static void add_screen_to_kbd_group(int32_t s_idx)
{
    if (!g_kbd_group || s_idx < 0 || s_idx >= g_proj.num_screens) return;
    vgp_screen_t *sc = &g_proj.screens[s_idx];
    for (uint16_t i = 0; i < sc->num_elements; ++i) {
        vgp_element_t *el = &g_proj.elements[sc->element_start + i];
        if (!el->lv_obj) continue;
        if (el->kind == VGP_KIND_BUTTON ||
            el->kind == VGP_KIND_SWITCH ||
            el->kind == VGP_KIND_INPUT ||
            el->kind == VGP_KIND_MULTISTATE) {
            lv_group_add_obj(g_kbd_group, el->lv_obj);
        }
    }
}

static void rebuild_kbd_group(void)
{
    if (!g_kbd_group) return;
    lv_group_remove_all_objs(g_kbd_group);

    if (g_active_popup_idx >= 0 && g_active_popup_idx < g_proj.num_screens) {
        add_screen_to_kbd_group(g_active_popup_idx);
        return;
    }

    if (g_current_screen_idx >= 0 && g_current_screen_idx < g_proj.num_screens) {
        /* Add current screen widgets first so initial focus is on the main content */
        add_screen_to_kbd_group(g_current_screen_idx);
        vgp_screen_t *sc = &g_proj.screens[g_current_screen_idx];
        if (sc->parent_screen_id[0]) {
            int32_t parent_idx = find_screen(sc->parent_screen_id);
            if (parent_idx >= 0 && parent_idx != g_current_screen_idx) {
                add_screen_to_kbd_group(parent_idx);
            }
        }
    }
}

static void switch_screen_to(int32_t idx);
static void open_popup_screen(int32_t idx);
static void close_active_popup(void);

static void execute_action(vgp_element_t *el)
{
    if (!el) return;
    vgp_variable_t *v = find_var(el->variable_id);

    switch (el->action) {
        case VGP_ACT_TOGGLE:
            if (v && v->type == VGP_VAR_BOOL) {
                v->val_bool = !v->val_bool;
            }
            break;
        case VGP_ACT_SET:
            if (v) {
                if (v->type == VGP_VAR_NUM) v->val_num = parse_double(el->action_value);
                else if (v->type == VGP_VAR_BOOL) v->val_bool = (strcmp(el->action_value, "true") == 0 || strcmp(el->action_value, "1") == 0);
                else strncpy(v->val_str, el->action_value, sizeof(v->val_str) - 1);
            }
            break;
        case VGP_ACT_INCREMENT:
            if (v && v->type == VGP_VAR_NUM) {
                v->val_num += parse_double(el->action_value);
            }
            break;
        case VGP_ACT_RESET:
            if (v) {
                v->val_num = v->initial_num;
                v->val_bool = v->initial_bool;
                strncpy(v->val_str, v->initial_str, sizeof(v->val_str) - 1);
            }
            break;
        case VGP_ACT_SCREEN:
            if (el->target_screen_id[0]) {
                int32_t target_idx = find_screen(el->target_screen_id);
                if (target_idx >= 0) {
                    if (g_history_depth < 16) g_history[g_history_depth++] = g_current_screen_idx;
                    close_active_popup();
                    switch_screen_to(target_idx);
                }
            }
            break;
        case VGP_ACT_PREV_SCREEN:
            close_active_popup();
            if (g_history_depth > 0) {
                int32_t prev_idx = g_history[--g_history_depth];
                switch_screen_to(prev_idx);
            }
            break;
        case VGP_ACT_OPEN_DIALOG:
        case VGP_ACT_POPUP:
            if (el->target_screen_id[0]) {
                int32_t pop_idx = find_screen(el->target_screen_id);
                if (pop_idx >= 0) open_popup_screen(pop_idx);
            }
            break;
        case VGP_ACT_CLOSE_DIALOG:
        case VGP_ACT_CLOSE_POPUP:
            close_active_popup();
            break;
        default:
            break;
    }
    vgp_runtime_update();
}

static void button_click_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    execute_action(el);
}

static void switch_value_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el) return;
    vgp_variable_t *v = find_var(el->variable_id);
    if (v && v->type == VGP_VAR_BOOL) {
        v->val_bool = lv_obj_has_state(el->lv_obj, LV_STATE_CHECKED);
    }
    vgp_runtime_update();
}

static void create_screen_widgets(int32_t s_idx, lv_obj_t *parent_obj, int32_t scale, int32_t off_x, int32_t off_y)
{
    vgp_screen_t *sc = &g_proj.screens[s_idx];

    /* Handle inheritance first: render parent's elements if inherited */
    if (sc->parent_screen_id[0]) {
        int32_t parent_idx = find_screen(sc->parent_screen_id);
        if (parent_idx >= 0 && parent_idx != s_idx) {
            create_screen_widgets(parent_idx, parent_obj, scale, off_x, off_y);
        }
    }

    for (uint16_t i = 0; i < sc->num_elements; ++i) {
        vgp_element_t *el = &g_proj.elements[sc->element_start + i];
        int32_t ex = off_x + (el->x * scale) / 1000;
        int32_t ey = off_y + (el->y * scale) / 1000;
        int32_t ew = (el->width * scale) / 1000;
        int32_t eh = (el->height * scale) / 1000;
        if (ew < 2) ew = 2;
        if (eh < 2) eh = 2;

        const lv_font_t *font = select_font(el->text[0] ? el->text : el->name, el->font_size, scale);

        switch (el->kind) {
            case VGP_KIND_RECTANGLE:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width, el->border_radius);
                break;

            case VGP_KIND_CIRCLE:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width, LV_RADIUS_CIRCLE);
                break;

            case VGP_KIND_LINE:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh > 4 ? eh : 2);
                apply_box_style(el->lv_obj, el->color ? el->color : el->background, 0, 0, 0);
                break;

            case VGP_KIND_TEXT:
                el->lv_obj = lv_label_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_set_style_text_font(el->lv_obj, font, 0);
                lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->color), 0);
                lv_label_set_text(el->lv_obj, el->text);
                break;

            case VGP_KIND_NUMBER:
                el->lv_obj = lv_label_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_set_style_text_font(el->lv_obj, font, 0);
                lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->color), 0);
                lv_label_set_text(el->lv_obj, "0");
                break;

            case VGP_KIND_BUTTON:
                el->lv_obj = lv_button_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background ? el->background : 0x005268EEu, el->border_color, el->border_width, el->border_radius ? el->border_radius : 8);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, button_click_cb, LV_EVENT_CLICKED, el);

                el->lv_extra = lv_label_create(el->lv_obj);
                lv_obj_set_style_text_font(el->lv_extra, font, 0);
                lv_obj_set_style_text_color(el->lv_extra, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                lv_label_set_text(el->lv_extra, el->text[0] ? el->text : el->name);
                lv_obj_center(el->lv_extra);
                break;

            case VGP_KIND_INDICATOR:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, 0x0042516Au, el->border_color, el->border_width, LV_RADIUS_CIRCLE);
                break;

            case VGP_KIND_SWITCH:
#if LV_USE_SWITCH
                el->lv_obj = lv_switch_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, switch_value_cb, LV_EVENT_VALUE_CHANGED, el);
#endif
                break;

            case VGP_KIND_BAR:
#if LV_USE_BAR
                el->lv_obj = lv_bar_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_bar_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                apply_box_style(el->lv_obj, el->track_color ? el->track_color : 0x00334155u, 0, 0, 6);
                lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->fill_color ? el->fill_color : 0x0038BDF8u), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(el->lv_obj, LV_OPA_COVER, LV_PART_INDICATOR);
#endif
                break;

            case VGP_KIND_GAUGE:
#if LV_USE_ARC
                el->lv_obj = lv_arc_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_arc_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                lv_arc_set_angles(el->lv_obj, 135, 45);
#endif
                break;

            case VGP_KIND_TREND:
#if LV_USE_CHART
                el->lv_obj = lv_chart_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_chart_set_type(el->lv_obj, LV_CHART_TYPE_LINE);
                apply_box_style(el->lv_obj, el->background, el->border_color, 1, 6);
                el->lv_extra = (lv_obj_t *)lv_chart_add_series(el->lv_obj, lv_color_hex(el->color ? el->color : 0x0050DFB6u), LV_CHART_AXIS_PRIMARY_Y);
#endif
                break;

            case VGP_KIND_ALARM:
            case VGP_KIND_TABLE:
#if LV_USE_TABLE
                el->lv_obj = lv_table_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_set_style_text_font(el->lv_obj, &vgp_font_zh_16, 0);
                lv_obj_set_style_text_font(el->lv_obj, &vgp_font_zh_16, LV_PART_ITEMS);
                lv_table_set_column_count(el->lv_obj, el->table_cols ? el->table_cols : 4);
                lv_table_set_row_count(el->lv_obj, el->table_rows ? el->table_rows : 4);
                lv_table_set_cell_value(el->lv_obj, 0, 0, el->kind == VGP_KIND_ALARM ? "時間" : "A");
                lv_table_set_cell_value(el->lv_obj, 0, 1, el->kind == VGP_KIND_ALARM ? "警報訊息" : "B");
                lv_table_set_cell_value(el->lv_obj, 0, 2, el->kind == VGP_KIND_ALARM ? "等級" : "C");
                lv_table_set_cell_value(el->lv_obj, 0, 3, el->kind == VGP_KIND_ALARM ? "狀態" : "D");
#endif
                break;

            case VGP_KIND_DATETIME:
                el->lv_obj = lv_label_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_set_style_text_font(el->lv_obj, font, 0);
                lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->color), 0);
                lv_label_set_text(el->lv_obj, "2026/11/04 12:00:00");
                break;

            case VGP_KIND_INPUT:
            case VGP_KIND_MULTISTATE:
                el->lv_obj = lv_button_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width, el->border_radius ? el->border_radius : 8);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, button_click_cb, LV_EVENT_CLICKED, el);

                el->lv_extra = lv_label_create(el->lv_obj);
                lv_obj_set_style_text_font(el->lv_extra, font, 0);
                lv_obj_set_style_text_color(el->lv_extra, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                lv_label_set_text(el->lv_extra, el->text[0] ? el->text : el->name);
                lv_obj_center(el->lv_extra);
                break;

            case VGP_KIND_SCALE:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, 0x001E293Bu, el->color ? el->color : 0x0094A3B8u, 1, 0);
                break;

            default:
                break;
        }
    }
}

static void switch_screen_to(int32_t idx)
{
    if (idx < 0 || idx >= g_proj.num_screens) return;
    g_current_screen_idx = idx;
    vgp_screen_t *sc = &g_proj.screens[idx];

    int32_t scale, off_x, off_y;
    calc_scaling(sc->width, sc->height, &scale, &off_x, &off_y);
    if (!sc->lv_screen) {
        sc->lv_screen = lv_obj_create(NULL);
        apply_box_style(sc->lv_screen, sc->background, 0, 0, 0);
    } else {
        lv_obj_clean(sc->lv_screen);
    }
    create_screen_widgets(idx, sc->lv_screen, scale, off_x, off_y);
    rebuild_kbd_group();
    lv_screen_load(sc->lv_screen);
}

static void open_popup_screen(int32_t idx)
{
    if (idx < 0 || idx >= g_proj.num_screens) return;
    close_active_popup();
    g_active_popup_idx = idx;

    vgp_screen_t *sc = &g_proj.screens[idx];

    /* Create semi-transparent overlay covering the screen */
    g_popup_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(g_popup_layer, 0, 0);
    lv_obj_set_size(g_popup_layer, (int32_t)g_screen_w, (int32_t)g_screen_h);
    lv_obj_set_style_bg_color(g_popup_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_popup_layer, LV_OPA_50, 0);
    lv_obj_remove_flag(g_popup_layer, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scale, off_x, off_y;
    calc_scaling(sc->width, sc->height, &scale, &off_x, &off_y);

    int32_t pop_w = (sc->width * scale) / 1000;
    int32_t pop_h = (sc->height * scale) / 1000;
    int32_t pop_x = ((int32_t)g_screen_w - pop_w) / 2;
    int32_t pop_y = ((int32_t)g_screen_h - pop_h) / 2;

    lv_obj_t *panel = lv_obj_create(g_popup_layer);
    lv_obj_set_pos(panel, pop_x, pop_y);
    lv_obj_set_size(panel, pop_w, pop_h);
    apply_box_style(panel, sc->background, 0x00475569u, 2, 12);

    create_screen_widgets(idx, panel, scale, 0, 0);
    rebuild_kbd_group();
    if (g_status_bar) lv_obj_move_to_index(g_status_bar, -1);
}

static void close_active_popup(void)
{
    if (g_popup_layer) {
        lv_obj_delete(g_popup_layer);
        g_popup_layer = NULL;
    }
    g_active_popup_idx = -1;
    rebuild_kbd_group();
}

bool vgp_runtime_init(const char *json, size_t json_len, uint32_t screen_w, uint32_t screen_h, lv_group_t *kbd_group)
{
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    g_kbd_group = kbd_group;

    if (!json || json_len == 0) return false;
    if (!vgp_parse_project(json, json_len, &g_proj)) return false;
    if (g_proj.num_screens == 0) return false;

    int32_t start_idx = find_screen(g_proj.start_screen_id);
    if (start_idx < 0) {
        /* Find first non-popup screen */
        for (uint16_t i = 0; i < g_proj.num_screens; ++i) {
            if (!g_proj.screens[i].is_popup) {
                start_idx = (int32_t)i;
                break;
            }
        }
        if (start_idx < 0) start_idx = 0;
    }

    switch_screen_to(start_idx);

    /* Create top-layer status bar for live Wi-Fi & config diagnostics */
    g_status_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_status_bar);
    lv_obj_set_pos(g_status_bar, 8, (int32_t)g_screen_h - 28);
    lv_obj_set_size(g_status_bar, (int32_t)g_screen_w - 16, 24);
    lv_obj_set_style_bg_color(g_status_bar, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(g_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_status_bar, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(g_status_bar, 1, 0);
    lv_obj_set_style_radius(g_status_bar, 0, 0);
    lv_obj_remove_flag(g_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_status_bar, LV_OBJ_FLAG_CLICKABLE);

    g_status_label = lv_label_create(g_status_bar);
    lv_obj_set_pos(g_status_label, 10, 3);
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x38bdf8), 0);
    lv_label_set_text(g_status_label, "Wi-Fi: starting...");

    g_active = true;
    vgp_runtime_update();
    return true;
}

static void set_label_if_changed(lv_obj_t *lbl, const char *txt)
{
    if (!lbl || !txt) return;
    const char *cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

void vgp_runtime_update(void)
{
    if (!g_active) return;
    g_sec_counter++;

    if (g_status_label) {
        char wifi_str[128];
        network_snapshot(wifi_str, sizeof(wifi_str));
        const bootcfg_status_t cfg = bootcfg_status();
        const char *cfg_short = (cfg == BOOTCFG_OK) ? "wifi.txt OK" : bootcfg_status_text(cfg);

        char usb_str[64];
        if (usb_state == 4u) {
            snprintf(usb_str, sizeof(usb_str), "USB:OK(%04lx:%04lx k:%lu \"%s\")",
                (unsigned long)(usb_vid_pid >> 16), (unsigned long)(usb_vid_pid & 0xffffu),
                (unsigned long)usb_key_presses, gui_usb_text);
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

        char full_status[256];
        snprintf(full_status, sizeof(full_status), "[%s] [%s] [CFG: %s] %s | RX:%lu TX:%lu",
            soc_is_bcm2837 ? "Zero2W" : "ZeroW",
            usb_str, cfg_short, wifi_str, (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
        set_label_if_changed(g_status_label, full_status);

        static uint32_t last_status_color = 0;
        uint32_t color = (net_stage == 6u) ? 0x4ade80u :
                         (net_stage == 0xffu || net_error != 0u) ? 0xf87171u : 0x38bdf8u;
        if (color != last_status_color) {
            last_status_color = color;
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(color), 0);
        }
    }

    /* Update widgets for active screen, its parent master screen, and popup if open */
    int32_t parent_idx = -1;
    if (g_current_screen_idx >= 0 && g_current_screen_idx < g_proj.num_screens) {
        const char *pid = g_proj.screens[g_current_screen_idx].parent_screen_id;
        if (pid[0]) parent_idx = find_screen(pid);
    }
    int32_t screens_to_update[3] = { parent_idx, g_current_screen_idx, g_active_popup_idx };
    int count = 3;

    for (int si = 0; si < count; ++si) {
        int32_t s_idx = screens_to_update[si];
        if (s_idx < 0 || s_idx >= g_proj.num_screens) continue;
        vgp_screen_t *sc = &g_proj.screens[s_idx];

        for (uint16_t i = 0; i < sc->num_elements; ++i) {
            vgp_element_t *el = &g_proj.elements[sc->element_start + i];
            if (!el->lv_obj) continue;

            /* Check conditionId */
            if (el->condition_id[0]) {
                vgp_variable_t *cv = find_var(el->condition_id);
                if (cv && cv->type == VGP_VAR_BOOL) {
                    if (cv->val_bool) lv_obj_remove_flag(el->lv_obj, LV_OBJ_FLAG_HIDDEN);
                    else lv_obj_add_flag(el->lv_obj, LV_OBJ_FLAG_HIDDEN);
                }
            }

            /* Check enabledId */
            if (el->enabled_id[0]) {
                vgp_variable_t *ev = find_var(el->enabled_id);
                if (ev && ev->type == VGP_VAR_BOOL) {
                    if (ev->val_bool) lv_obj_remove_state(el->lv_obj, LV_STATE_DISABLED);
                    else lv_obj_add_state(el->lv_obj, LV_STATE_DISABLED);
                }
            }

            vgp_variable_t *v = find_var(el->variable_id);

            switch (el->kind) {
                case VGP_KIND_NUMBER:
                    if (v && v->type == VGP_VAR_NUM) {
                        char buf[64];
                        format_num_val(buf, sizeof(buf), v->val_num, el);
                        set_label_if_changed(el->lv_obj, buf);
                    }
                    break;

                case VGP_KIND_INDICATOR:
                    if (v && v->type == VGP_VAR_BOOL) {
                        uint32_t color = v->val_bool ? 0x0058D3ACu : 0x0042516Au;
                        if (el->color != color) {
                            el->color = color;
                            lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(color), 0);
                        }
                    }
                    break;

                case VGP_KIND_SWITCH:
#if LV_USE_SWITCH
                    if (v && v->type == VGP_VAR_BOOL) {
                        bool cur = lv_obj_has_state(el->lv_obj, LV_STATE_CHECKED);
                        if (cur != v->val_bool) {
                            if (v->val_bool) lv_obj_add_state(el->lv_obj, LV_STATE_CHECKED);
                            else lv_obj_remove_state(el->lv_obj, LV_STATE_CHECKED);
                        }
                    }
#endif
                    break;

                case VGP_KIND_BAR:
#if LV_USE_BAR
                    if (v && v->type == VGP_VAR_NUM) {
                        if (lv_bar_get_value(el->lv_obj) != (int32_t)v->val_num) {
                            lv_bar_set_value(el->lv_obj, (int32_t)v->val_num, LV_ANIM_OFF);
                        }
                    }
#endif
                    break;

                case VGP_KIND_GAUGE:
#if LV_USE_ARC
                    if (v && v->type == VGP_VAR_NUM) {
                        if (lv_arc_get_value(el->lv_obj) != (int32_t)v->val_num) {
                            lv_arc_set_value(el->lv_obj, (int32_t)v->val_num);
                        }
                    }
#endif
                    break;

                case VGP_KIND_TREND:
#if LV_USE_CHART
                    if (v && v->type == VGP_VAR_NUM && el->lv_extra && (g_sec_counter % 4u == 0u)) {
                        lv_chart_series_t *ser = (lv_chart_series_t *)el->lv_extra;
                        lv_chart_set_next_value(el->lv_obj, ser, (int32_t)v->val_num);
                    }
#endif
                    break;

                case VGP_KIND_MULTISTATE:
                    if (v && el->lv_extra) {
                        double cur_val = (v->type == VGP_VAR_NUM) ? v->val_num : (v->val_bool ? 1.0 : 0.0);
                        const vgp_multistate_item_t *matched = NULL;
                        for (uint8_t m = 0; m < el->num_multistate_items; ++m) {
                            if ((int32_t)el->multistate_items[m].value == (int32_t)cur_val) {
                                matched = &el->multistate_items[m];
                                break;
                            }
                        }
                        if (matched) {
                            const char *cur_lbl = lv_label_get_text(el->lv_extra);
                            if (!cur_lbl || strcmp(cur_lbl, matched->label) != 0) {
                                lv_label_set_text(el->lv_extra, matched->label);
                                lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(matched->color), 0);
                                lv_obj_set_style_text_color(el->lv_extra, lv_color_hex(matched->text_color), 0);
                            }
                        }
                    }
                    break;

                case VGP_KIND_INPUT:
                    if (v && el->lv_extra) {
                        char buf[64];
                        if (v->type == VGP_VAR_NUM) format_num_val(buf, sizeof(buf), v->val_num, el);
                        else if (v->type == VGP_VAR_BOOL) snprintf(buf, sizeof(buf), "%s", v->val_bool ? "ON" : "OFF");
                        else snprintf(buf, sizeof(buf), "%s", v->val_str);
                        set_label_if_changed(el->lv_extra, buf);
                    }
                    break;

                case VGP_KIND_DATETIME: {
                    char dt[32];
                    uint32_t sec = g_sec_counter / 4;
                    uint32_t m = (sec / 60) % 60;
                    uint32_t h = (sec / 3600) % 24;
                    uint32_t s = sec % 60;
                    snprintf(dt, sizeof(dt), "2026/11/04 %02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
                    set_label_if_changed(el->lv_obj, dt);
                    break;
                }

                default:
                    break;
            }
        }
    }
}

void vgp_runtime_handle_key(uint32_t key)
{
    if (!g_active) return;

    if (key == KEY_ESC && g_active_popup_idx >= 0) {
        close_active_popup();
        vgp_runtime_update();
        return;
    }

    if (!g_kbd_group) return;
    lv_obj_t *focused = lv_group_get_focused(g_kbd_group);
    if (!focused) return;

    int32_t active_s = (g_active_popup_idx >= 0) ? g_active_popup_idx : g_current_screen_idx;
    if (active_s < 0 || active_s >= g_proj.num_screens) return;
    vgp_screen_t *sc = &g_proj.screens[active_s];

    for (uint16_t i = 0; i < sc->num_elements; ++i) {
        vgp_element_t *el = &g_proj.elements[sc->element_start + i];
        if (el->lv_obj == focused && el->kind == VGP_KIND_INPUT) {
            vgp_variable_t *v = find_var(el->variable_id);
            if (!v) return;
            if (v->type == VGP_VAR_NUM) {
                if (key >= '0' && key <= '9') {
                    double d = (double)(key - '0');
                    if (v->val_num >= 0.0) v->val_num = (long)v->val_num * 10.0 + d;
                    else v->val_num = (long)v->val_num * 10.0 - d;
                    vgp_runtime_update();
                } else if (key == KEY_BACKSPACE) {
                    v->val_num = (double)((long)v->val_num / 10);
                    vgp_runtime_update();
                } else if (key == '+') {
                    v->val_num += 1.0;
                    vgp_runtime_update();
                } else if (key == '-') {
                    v->val_num -= 1.0;
                    vgp_runtime_update();
                }
            } else if (v->type == VGP_VAR_STR) {
                size_t len = strlen(v->val_str);
                if (key >= 32 && key < 127 && len + 1 < sizeof(v->val_str)) {
                    v->val_str[len] = (char)key;
                    v->val_str[len + 1] = '\0';
                    vgp_runtime_update();
                } else if (key == KEY_BACKSPACE && len > 0) {
                    v->val_str[len - 1] = '\0';
                    vgp_runtime_update();
                }
            }
            return;
        }
    }
}

bool vgp_runtime_is_active(void)
{
    return g_active;
}

const char *vgp_runtime_project_name(void)
{
    return g_proj.name[0] ? g_proj.name : "ViewGP Project";
}

