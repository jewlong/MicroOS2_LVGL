/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_runtime.h"
#include "vgp_parser.h"
#include "vgp_alarm.h"
#include "vgp_modbus.h"
#include "vgp_image.h"
#include "vgp_trend.h"
#include "sys_clock.h"
#include "net_platform.h"
#include "bootcfg.h"
#include <ucos_ii.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "mouse_queue.h"
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
static lv_obj_t *g_ticker_bar = NULL;
static lv_obj_t *g_ticker_label = NULL;
static uint32_t g_ticker_last_ms = 0u;
static uint16_t g_ticker_idx = 0u;
static uint32_t g_last_alarm_gen = 0u;
static int32_t g_momentary_var_idx = -1;
static vgp_element_t *g_edit_el = NULL;
static char g_edit_buf[32];
static lv_group_t *g_kbd_group = NULL;
static uint32_t g_sec_counter = 0;

static int32_t find_var_idx(const char *id)
{
    if (!id || !id[0]) return -1;
    for (uint16_t i = 0; i < g_proj.num_variables; ++i) {
        if (strcmp(g_proj.variables[i].id, id) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static vgp_variable_t *find_var(const char *id)
{
    int32_t idx = find_var_idx(id);
    return (idx >= 0) ? &g_proj.variables[idx] : NULL;
}

static void write_var_bool(int32_t vi, bool val)
{
    if (vi < 0 || vi >= g_proj.num_variables) return;
    vgp_variable_t *v = &g_proj.variables[vi];
    if (v->modbus.enabled) {
        (void)vgp_modbus_enqueue_write((uint16_t)vi, val ? 1.0 : 0.0, val);
    } else {
        v->val_bool = val;
    }
}

static void write_var_num(int32_t vi, double val)
{
    if (vi < 0 || vi >= g_proj.num_variables) return;
    vgp_variable_t *v = &g_proj.variables[vi];
    if (v->modbus.enabled) {
        (void)vgp_modbus_enqueue_write((uint16_t)vi, val, val != 0.0);
    } else {
        v->val_num = val;
    }
}

static double clamp_input_val(const vgp_element_t *el, double val)
{
    if (!el) return val;
    if (el->has_input_min && val < el->input_min) val = el->input_min;
    if (el->has_input_max && val > el->input_max) val = el->input_max;
    return val;
}

static void release_momentary(void)
{
    if (g_momentary_var_idx >= 0) {
        int32_t vi = g_momentary_var_idx;
        g_momentary_var_idx = -1;
        write_var_bool(vi, false);
    }
}

static void cancel_input_edit(void)
{
    g_edit_el = NULL;
    g_edit_buf[0] = '\0';
}

static void commit_input_edit(void)
{
    if (!g_edit_el) return;
    vgp_element_t *el = g_edit_el;
    g_edit_el = NULL;
    int32_t vi = find_var_idx(el->variable_id);
    if (vi >= 0) {
        vgp_variable_t *v = &g_proj.variables[vi];
        if (v->type == VGP_VAR_NUM && g_edit_buf[0]) {
            double val = clamp_input_val(el, parse_double(g_edit_buf));
            write_var_num(vi, val);
        } else if (v->type == VGP_VAR_STR) {
            strncpy(v->val_str, g_edit_buf, sizeof(v->val_str) - 1u);
            v->val_str[sizeof(v->val_str) - 1u] = '\0';
        }
    }
    g_edit_buf[0] = '\0';
}

static uint32_t threshold_fill_color(const vgp_element_t *el, double val)
{
    if (el->has_critical_threshold && val >= el->critical_threshold) return 0x00EF4444u;
    if (el->has_warning_threshold && val >= el->warning_threshold) return 0x00F59E0Bu;
    return el->fill_color ? el->fill_color : 0x0038BDF8u;
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
    if (scaled_size >= 40) return &vgp_font_num_48;
    if (scaled_size >= 29) return &vgp_font_num_36;
    if (scaled_size >= 19) return &vgp_font_zh_22;
    return &vgp_font_zh_16;
}

static void format_int_grouped(char *out, size_t max_out, uint64_t val, bool grouping, char grp_ch)
{
    char rev[32];
    size_t n = 0u;
    do {
        if (grouping && n > 0u && (n % 4u) == 3u && n + 1u < sizeof(rev)) {
            rev[n++] = grp_ch;
        }
        if (n + 1u < sizeof(rev)) {
            rev[n++] = (char)('0' + (val % 10u));
        }
        val /= 10u;
    } while (val != 0u);
    size_t pos = 0u;
    while (n > 0u && pos + 1u < max_out) {
        out[pos++] = rev[--n];
    }
    out[pos] = '\0';
}

static void format_num_val(char *buf, size_t max_buf, double val, const vgp_element_t *e)
{
    if (!buf || max_buf == 0u || !e) return;
    char dec_ch = (e->number_locale == VGP_LOCALE_DE_DE) ? ',' : '.';
    char grp_ch = (e->number_locale == VGP_LOCALE_DE_DE) ? '.' : ',';
    uint8_t dec = e->decimals;
    if (dec > 6u) dec = 6u;
    if (e->number_format == VGP_NUMFMT_INTEGER) dec = 0u;

    double v = val;
    if (e->number_format == VGP_NUMFMT_PERCENT) {
        v *= 100.0;
    }

    char core[48];
    core[0] = '\0';

    if (e->number_format == VGP_NUMFMT_SCIENTIFIC) {
        bool neg = (v < 0.0);
        double av = neg ? -v : v;
        int exp10 = 0;
        if (av > 0.0) {
            while (av >= 10.0 && exp10 < 300) { av *= 0.1; exp10++; }
            while (av < 1.0 && exp10 > -300) { av *= 10.0; exp10--; }
        }
        uint64_t mult = 1u;
        for (uint8_t i = 0; i < dec; ++i) mult *= 10u;
        uint64_t scaled = (uint64_t)(av * (double)mult + 0.5);
        if (scaled >= 10u * mult) { scaled /= 10u; exp10++; }
        uint64_t ip = scaled / mult;
        uint64_t fp = scaled % mult;
        int abs_e = (exp10 < 0) ? -exp10 : exp10;
        if (dec == 0u) {
            snprintf(core, sizeof(core), "%s%lu%c%c%02d",
                     neg ? "-" : "", (unsigned long)ip, 'e', exp10 < 0 ? '-' : '+', abs_e);
        } else {
            snprintf(core, sizeof(core), "%s%lu%c%0*lu%c%c%02d",
                     neg ? "-" : "", (unsigned long)ip, dec_ch, (int)dec, (unsigned long)fp,
                     'e', exp10 < 0 ? '-' : '+', abs_e);
        }
    } else {
        bool neg = (v < 0.0);
        double av = neg ? -v : v;
        uint64_t mult = 1u;
        for (uint8_t i = 0; i < dec; ++i) mult *= 10u;
        uint64_t scaled = (uint64_t)(av * (double)mult + 0.5);
        uint64_t ip = scaled / mult;
        uint64_t fp = scaled % mult;
        if (ip == 0u && fp == 0u) neg = false;
        char int_buf[32];
        format_int_grouped(int_buf, sizeof(int_buf), ip, e->use_grouping, grp_ch);
        if (dec == 0u) {
            snprintf(core, sizeof(core), "%s%s", neg ? "-" : "", int_buf);
        } else {
            snprintf(core, sizeof(core), "%s%s%c%0*lu",
                     neg ? "-" : "", int_buf, dec_ch, (int)dec, (unsigned long)fp);
        }
    }

    const char *pfx = e->number_prefix;
    const char *pct = (e->number_format == VGP_NUMFMT_PERCENT) ? "%" : "";
    if (e->unit[0]) {
        snprintf(buf, max_buf, "%s%s%s %s", pfx, core, pct, e->unit);
    } else {
        snprintf(buf, max_buf, "%s%s%s", pfx, core, pct);
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

static void apply_box_style_ex(lv_obj_t *obj, const vgp_element_t *el, uint16_t radius)
{
    uint16_t bw = (el->border_style == VGP_BORDER_NONE) ? 0u : el->border_width;
    apply_box_style(obj, el->background, el->border_color, bw, radius);
    if (!el->shape_filled) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    }
    if (el->border_style == VGP_BORDER_DASHED || el->border_style == VGP_BORDER_DOTTED) {
        lv_obj_set_style_border_opa(obj, LV_OPA_60, 0);
    }
}

static void apply_label_style(lv_obj_t *obj, const vgp_element_t *el, const lv_font_t *font, int32_t eh)
{
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(el->color), 0);
    lv_text_align_t align = (el->text_align == VGP_ALIGN_CENTER) ? LV_TEXT_ALIGN_CENTER :
                            (el->text_align == VGP_ALIGN_RIGHT) ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(obj, align, 0);
    lv_label_set_long_mode(obj, el->word_wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_CLIP);
    if (el->underline) {
        lv_obj_set_style_text_decor(obj, LV_TEXT_DECOR_UNDERLINE, 0);
    }
    if (el->line_height_x100 > 100u && font) {
        int32_t ls = ((int32_t)(el->line_height_x100 - 100u) * font->line_height) / 100;
        lv_obj_set_style_text_line_space(obj, ls, 0);
    }
    if (el->text_background) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(el->background), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    if (font && el->vertical_align != VGP_VALIGN_TOP) {
        int32_t pad = 0;
        if (el->vertical_align == VGP_VALIGN_MIDDLE) pad = (eh - font->line_height) / 2;
        else if (el->vertical_align == VGP_VALIGN_BOTTOM) pad = eh - font->line_height;
        if (pad > 0) lv_obj_set_style_pad_top(obj, pad, 0);
    }
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
            el->kind == VGP_KIND_MULTISTATE ||
            el->kind == VGP_KIND_ALARM ||
            el->kind == VGP_KIND_SLIDER ||
            el->kind == VGP_KIND_ARC ||
            el->kind == VGP_KIND_SPINBOX ||
            el->kind == VGP_KIND_DROPDOWN) {
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
    int32_t vi = find_var_idx(el->variable_id);
    vgp_variable_t *v = (vi >= 0) ? &g_proj.variables[vi] : NULL;

    switch (el->action) {
        case VGP_ACT_TOGGLE:
            if (v && v->type == VGP_VAR_BOOL) {
                write_var_bool(vi, !v->val_bool);
            }
            break;
        case VGP_ACT_SET:
            if (v) {
                if (v->type == VGP_VAR_NUM) write_var_num(vi, parse_double(el->action_value));
                else if (v->type == VGP_VAR_BOOL) write_var_bool(vi, (strcmp(el->action_value, "true") == 0 || strcmp(el->action_value, "1") == 0));
                else strncpy(v->val_str, el->action_value, sizeof(v->val_str) - 1);
            }
            break;
        case VGP_ACT_INCREMENT:
            if (v && v->type == VGP_VAR_NUM) {
                write_var_num(vi, v->val_num + parse_double(el->action_value));
            }
            break;
        case VGP_ACT_RESET:
            if (v) {
                if (v->type == VGP_VAR_NUM) write_var_num(vi, v->initial_num);
                else if (v->type == VGP_VAR_BOOL) write_var_bool(vi, v->initial_bool);
                else strncpy(v->val_str, v->initial_str, sizeof(v->val_str) - 1);
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
    int32_t vi = find_var_idx(el->variable_id);
    if (vi >= 0 && g_proj.variables[vi].type == VGP_VAR_BOOL) {
        bool want = lv_obj_has_state(el->lv_obj, LV_STATE_CHECKED);
        write_var_bool(vi, want);
    }
    vgp_runtime_update();
}

static void alarm_ack_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el) return;
    uint16_t el_idx = (uint16_t)(el - g_proj.elements);
    vgp_alarm_ack_for_element(el, el_idx);
    vgp_runtime_update();
}

static void momentary_press_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el) return;
    int32_t vi = find_var_idx(el->variable_id);
    if (vi >= 0 && g_proj.variables[vi].type == VGP_VAR_BOOL) {
        g_momentary_var_idx = vi;
        write_var_bool(vi, true);
        vgp_runtime_update();
    }
}

static void momentary_release_cb(lv_event_t *event)
{
    (void)event;
    release_momentary();
    vgp_runtime_update();
}

static void input_click_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (g_edit_el && g_edit_el == el) {
        commit_input_edit();
        vgp_runtime_update();
    }
}

static void input_defocus_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (g_edit_el && g_edit_el == el) {
        commit_input_edit();
        vgp_runtime_update();
    }
}

#if LV_USE_SLIDER
static void slider_release_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el || !el->lv_obj) return;
    int32_t vi = find_var_idx(el->variable_id);
    if (vi >= 0 && g_proj.variables[vi].type == VGP_VAR_NUM) {
        write_var_num(vi, (double)lv_slider_get_value(el->lv_obj));
        vgp_runtime_update();
    }
}
#endif

#if LV_USE_ARC
static void arc_release_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el || !el->lv_obj) return;
    int32_t vi = find_var_idx(el->variable_id);
    if (vi >= 0 && g_proj.variables[vi].type == VGP_VAR_NUM) {
        write_var_num(vi, (double)lv_arc_get_value(el->lv_obj));
        vgp_runtime_update();
    }
}
#endif

static void spinbox_step(vgp_element_t *el, double dir)
{
    if (!el) return;
    int32_t vi = find_var_idx(el->variable_id);
    if (vi < 0 || g_proj.variables[vi].type != VGP_VAR_NUM) return;
    double step = (el->input_step > 0.0) ? el->input_step : 1.0;
    double next = clamp_input_val(el, g_proj.variables[vi].val_num + dir * step);
    write_var_num(vi, next);
    vgp_runtime_update();
}

static void spinbox_dec_cb(lv_event_t *event)
{
    spinbox_step((vgp_element_t *)lv_event_get_user_data(event), -1.0);
}

static void spinbox_inc_cb(lv_event_t *event)
{
    spinbox_step((vgp_element_t *)lv_event_get_user_data(event), 1.0);
}

#if LV_USE_DROPDOWN
static void dropdown_change_cb(lv_event_t *event)
{
    vgp_element_t *el = (vgp_element_t *)lv_event_get_user_data(event);
    if (!el || !el->lv_obj) return;
    uint32_t sel = lv_dropdown_get_selected(el->lv_obj);
    if (sel < el->num_multistate_items) {
        int32_t vi = find_var_idx(el->variable_id);
        if (vi >= 0) {
            double val = el->multistate_items[sel].value;
            if (g_proj.variables[vi].type == VGP_VAR_BOOL) write_var_bool(vi, val != 0.0);
            else if (g_proj.variables[vi].type == VGP_VAR_NUM) write_var_num(vi, val);
            vgp_runtime_update();
        }
    }
}
#endif

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
                apply_box_style_ex(el->lv_obj, el, el->border_radius);
                break;

            case VGP_KIND_CIRCLE:
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style_ex(el->lv_obj, el, LV_RADIUS_CIRCLE);
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
                apply_label_style(el->lv_obj, el, font, eh);
                lv_label_set_text(el->lv_obj, el->text);
                break;

            case VGP_KIND_NUMBER:
                el->lv_obj = lv_label_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_label_style(el->lv_obj, el, font, eh);
                lv_label_set_text(el->lv_obj, "0");
                break;

            case VGP_KIND_BUTTON:
                el->lv_obj = lv_button_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background ? el->background : 0x005268EEu, el->border_color,
                                (el->border_style == VGP_BORDER_NONE) ? 0u : el->border_width,
                                el->border_radius ? el->border_radius : 8);
                apply_focus_style(el->lv_obj);
                lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                if (el->has_btn_pressed_bg) {
                    lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->btn_pressed_bg), LV_STATE_PRESSED);
                    lv_obj_set_style_bg_opa(el->lv_obj, LV_OPA_COVER, LV_STATE_PRESSED);
                }
                if (el->has_btn_pressed_color) {
                    lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->btn_pressed_color), LV_STATE_PRESSED);
                }
                if (el->has_btn_disabled_bg) {
                    lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->btn_disabled_bg), LV_STATE_DISABLED);
                    lv_obj_set_style_bg_opa(el->lv_obj, LV_OPA_COVER, LV_STATE_DISABLED);
                }
                if (el->has_btn_disabled_color) {
                    lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->btn_disabled_color), LV_STATE_DISABLED);
                }
                if (el->action == VGP_ACT_MOMENTARY) {
                    lv_obj_add_event_cb(el->lv_obj, momentary_press_cb, LV_EVENT_PRESSED, el);
                    lv_obj_add_event_cb(el->lv_obj, momentary_release_cb, LV_EVENT_RELEASED, el);
                    lv_obj_add_event_cb(el->lv_obj, momentary_release_cb, LV_EVENT_PRESS_LOST, el);
                    lv_obj_add_event_cb(el->lv_obj, momentary_release_cb, LV_EVENT_DEFOCUSED, el);
                } else {
                    lv_obj_add_event_cb(el->lv_obj, button_click_cb, LV_EVENT_CLICKED, el);
                }

                el->lv_extra = lv_label_create(el->lv_obj);
                lv_obj_set_style_text_font(el->lv_extra, font, 0);
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
                if (el->show_value) {
                    el->lv_aux = lv_label_create(el->lv_obj);
                    lv_obj_set_style_text_font(el->lv_aux, &vgp_font_zh_16, 0);
                    lv_obj_set_style_text_color(el->lv_aux, lv_color_hex(0x00FFFFFFu), 0);
                    lv_label_set_text(el->lv_aux, "0");
                    lv_obj_center(el->lv_aux);
                }
#endif
                break;

            case VGP_KIND_GAUGE:
#if LV_USE_ARC
                el->lv_obj = lv_arc_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_arc_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                lv_arc_set_angles(el->lv_obj, 135, 45);
                lv_obj_remove_flag(el->lv_obj, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_opa(el->lv_obj, LV_OPA_TRANSP, LV_PART_KNOB);
                lv_obj_set_style_arc_color(el->lv_obj, lv_color_hex(el->track_color ? el->track_color : 0x00334155u), LV_PART_MAIN);
                lv_obj_set_style_arc_color(el->lv_obj, lv_color_hex(el->fill_color ? el->fill_color : 0x0038BDF8u), LV_PART_INDICATOR);
                if (el->show_value) {
                    el->lv_aux = lv_label_create(el->lv_obj);
                    lv_obj_set_style_text_font(el->lv_aux, font, 0);
                    lv_obj_set_style_text_color(el->lv_aux, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                    lv_label_set_text(el->lv_aux, "0");
                    lv_obj_center(el->lv_aux);
                }
#endif
                break;

            case VGP_KIND_ARC:
#if LV_USE_ARC
                el->lv_obj = lv_arc_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_arc_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                lv_arc_set_angles(el->lv_obj, 135, 45);
                lv_obj_set_style_arc_color(el->lv_obj, lv_color_hex(el->track_color ? el->track_color : 0x00334155u), LV_PART_MAIN);
                lv_obj_set_style_arc_color(el->lv_obj, lv_color_hex(el->fill_color ? el->fill_color : 0x0038BDF8u), LV_PART_INDICATOR);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, arc_release_cb, LV_EVENT_RELEASED, el);
                if (el->show_value) {
                    el->lv_aux = lv_label_create(el->lv_obj);
                    lv_obj_set_style_text_font(el->lv_aux, font, 0);
                    lv_obj_set_style_text_color(el->lv_aux, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                    lv_label_set_text(el->lv_aux, "0");
                    lv_obj_center(el->lv_aux);
                }
#endif
                break;

            case VGP_KIND_SLIDER:
#if LV_USE_SLIDER
                el->lv_obj = lv_slider_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_slider_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->track_color ? el->track_color : 0x00334155u), LV_PART_MAIN);
                lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->fill_color ? el->fill_color : 0x0038BDF8u), LV_PART_INDICATOR);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, slider_release_cb, LV_EVENT_RELEASED, el);
                if (el->show_value) {
                    el->lv_aux = lv_label_create(parent_obj);
                    lv_obj_set_pos(el->lv_aux, ex + ew + 6, ey);
                    lv_obj_set_style_text_font(el->lv_aux, &vgp_font_zh_16, 0);
                    lv_obj_set_style_text_color(el->lv_aux, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                    lv_label_set_text(el->lv_aux, "0");
                }
#endif
                break;

            case VGP_KIND_TREND:
#if LV_USE_CHART
                el->lv_obj = lv_chart_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_chart_set_type(el->lv_obj, LV_CHART_TYPE_LINE);
                lv_chart_set_axis_range(el->lv_obj, LV_CHART_AXIS_PRIMARY_Y,
                                        (int32_t)(el->scale_min * 10.0), (int32_t)(el->scale_max * 10.0));
                apply_box_style(el->lv_obj, el->background, el->border_color, 1, 6);
                el->lv_extra = (lv_obj_t *)lv_chart_add_series(el->lv_obj, lv_color_hex(el->fill_color ? el->fill_color : (el->color ? el->color : 0x0050DFB6u)), LV_CHART_AXIS_PRIMARY_Y);
                vgp_trend_populate_chart((uint16_t)(sc->element_start + i), el->lv_obj, (lv_chart_series_t *)el->lv_extra);
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
                lv_table_set_row_count(el->lv_obj, el->kind == VGP_KIND_ALARM ? 2 : (el->table_rows ? el->table_rows : 4));
                lv_table_set_cell_value(el->lv_obj, 0, 0, el->kind == VGP_KIND_ALARM ? "時間" : "A");
                lv_table_set_cell_value(el->lv_obj, 0, 1, el->kind == VGP_KIND_ALARM ? "警報訊息" : "B");
                lv_table_set_cell_value(el->lv_obj, 0, 2, el->kind == VGP_KIND_ALARM ? "等級" : "C");
                lv_table_set_cell_value(el->lv_obj, 0, 3, el->kind == VGP_KIND_ALARM ? "狀態" : "D");
                if (el->kind == VGP_KIND_ALARM) {
                    apply_focus_style(el->lv_obj);
                    lv_obj_add_event_cb(el->lv_obj, alarm_ack_cb, LV_EVENT_CLICKED, el);
                    lv_table_set_column_width(el->lv_obj, 0, (ew * 20) / 100);
                    lv_table_set_column_width(el->lv_obj, 1, (ew * 46) / 100);
                    lv_table_set_column_width(el->lv_obj, 2, (ew * 16) / 100);
                    lv_table_set_column_width(el->lv_obj, 3, (ew * 18) / 100);
                    lv_table_set_cell_value(el->lv_obj, 1, 0, "--:--:--");
                    lv_table_set_cell_value(el->lv_obj, 1, 1, "無警報");
                    lv_table_set_cell_value(el->lv_obj, 1, 2, "-");
                    lv_table_set_cell_value(el->lv_obj, 1, 3, "正常");
                } else {
                    apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width ? el->border_width : 1, 0);
                    lv_obj_set_style_border_color(el->lv_obj, lv_color_hex(el->grid_color ? el->grid_color : 0x00334155u), LV_PART_ITEMS);
                    lv_obj_set_style_border_width(el->lv_obj, 1, LV_PART_ITEMS);
                }
#endif
                break;

            case VGP_KIND_DATETIME:
                el->lv_obj = lv_label_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_label_style(el->lv_obj, el, font, eh);
                lv_label_set_text(el->lv_obj, "2026/01/01 00:00:00");
                break;

            case VGP_KIND_INPUT:
                el->lv_obj = lv_button_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width ? el->border_width : 1, el->border_radius ? el->border_radius : 8);
                apply_focus_style(el->lv_obj);
                lv_obj_add_event_cb(el->lv_obj, input_click_cb, LV_EVENT_CLICKED, el);
                lv_obj_add_event_cb(el->lv_obj, input_defocus_cb, LV_EVENT_DEFOCUSED, el);

                el->lv_extra = lv_label_create(el->lv_obj);
                lv_obj_set_style_text_font(el->lv_extra, font, 0);
                lv_obj_set_style_text_color(el->lv_extra, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                lv_label_set_text(el->lv_extra, el->text[0] ? el->text : el->name);
                lv_obj_center(el->lv_extra);
                break;

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

            case VGP_KIND_SPINBOX: {
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width ? el->border_width : 1, el->border_radius ? el->border_radius : 6);
                apply_focus_style(el->lv_obj);
                lv_obj_set_style_pad_all(el->lv_obj, 0, 0);

                int32_t btn_w = (eh < ew / 3) ? eh : (ew / 3);
                if (btn_w < 24) btn_w = 24;
                lv_obj_t *dec_btn = lv_button_create(el->lv_obj);
                lv_obj_set_pos(dec_btn, 0, 0);
                lv_obj_set_size(dec_btn, btn_w, eh);
                apply_box_style(dec_btn, 0x00334155u, 0, 0, 4);
                lv_obj_add_event_cb(dec_btn, spinbox_dec_cb, LV_EVENT_CLICKED, el);
                lv_obj_t *dlbl = lv_label_create(dec_btn);
                lv_obj_set_style_text_font(dlbl, &vgp_font_zh_16, 0);
                lv_obj_set_style_text_color(dlbl, lv_color_hex(0x00FFFFFFu), 0);
                lv_label_set_text(dlbl, "-");
                lv_obj_center(dlbl);

                el->lv_aux = lv_button_create(el->lv_obj);
                lv_obj_set_pos(el->lv_aux, ew - btn_w, 0);
                lv_obj_set_size(el->lv_aux, btn_w, eh);
                apply_box_style(el->lv_aux, 0x00334155u, 0, 0, 4);
                lv_obj_add_event_cb(el->lv_aux, spinbox_inc_cb, LV_EVENT_CLICKED, el);
                lv_obj_t *ilbl = lv_label_create(el->lv_aux);
                lv_obj_set_style_text_font(ilbl, &vgp_font_zh_16, 0);
                lv_obj_set_style_text_color(ilbl, lv_color_hex(0x00FFFFFFu), 0);
                lv_label_set_text(ilbl, "+");
                lv_obj_center(ilbl);

                el->lv_extra = lv_label_create(el->lv_obj);
                lv_obj_set_style_text_font(el->lv_extra, font, 0);
                lv_obj_set_style_text_color(el->lv_extra, lv_color_hex(el->color ? el->color : 0x00FFFFFFu), 0);
                lv_label_set_text(el->lv_extra, "0");
                lv_obj_center(el->lv_extra);
                break;
            }

            case VGP_KIND_DROPDOWN:
#if LV_USE_DROPDOWN
                el->lv_obj = lv_dropdown_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_set_style_text_font(el->lv_obj, &vgp_font_zh_16, 0);
                apply_focus_style(el->lv_obj);
                {
                    char opts[256];
                    opts[0] = '\0';
                    size_t pos = 0u;
                    for (uint8_t m = 0; m < el->num_multistate_items; ++m) {
                        const char *lbl = el->multistate_items[m].label;
                        size_t l = strlen(lbl);
                        if (pos + l + 2u >= sizeof(opts)) break;
                        if (m > 0u) opts[pos++] = '\n';
                        memcpy(opts + pos, lbl, l);
                        pos += l;
                        opts[pos] = '\0';
                    }
                    if (opts[0]) lv_dropdown_set_options(el->lv_obj, opts);
                }
                lv_obj_add_event_cb(el->lv_obj, dropdown_change_cb, LV_EVENT_VALUE_CHANGED, el);
#endif
                break;

            case VGP_KIND_SCALE:
#if LV_USE_SCALE
                el->lv_obj = lv_scale_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_scale_set_mode(el->lv_obj, el->scale_vertical ? LV_SCALE_MODE_VERTICAL_LEFT : LV_SCALE_MODE_HORIZONTAL_BOTTOM);
                lv_scale_set_range(el->lv_obj, (int32_t)el->scale_min, (int32_t)el->scale_max);
                {
                    uint32_t maj = el->scale_major_ticks ? el->scale_major_ticks : 5u;
                    uint32_t min_t = el->scale_minor_ticks ? el->scale_minor_ticks : 4u;
                    uint32_t total = maj * (min_t + 1u) + 1u;
                    if (total > 101u) total = 101u;
                    lv_scale_set_total_tick_count(el->lv_obj, total);
                    lv_scale_set_major_tick_every(el->lv_obj, min_t + 1u);
                }
                lv_scale_set_label_show(el->lv_obj, el->scale_show_labels);
                lv_obj_set_style_line_color(el->lv_obj, lv_color_hex(el->color ? el->color : 0x0094A3B8u), LV_PART_MAIN);
                lv_obj_set_style_line_color(el->lv_obj, lv_color_hex(el->color ? el->color : 0x0094A3B8u), LV_PART_ITEMS);
                lv_obj_set_style_line_color(el->lv_obj, lv_color_hex(el->color ? el->color : 0x0094A3B8u), LV_PART_INDICATOR);
                lv_obj_set_style_text_font(el->lv_obj, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(el->lv_obj, lv_color_hex(el->color ? el->color : 0x0094A3B8u), 0);
#endif
                break;

            case VGP_KIND_IMAGE:
#if LV_USE_IMAGE
                el->lv_obj = lv_obj_create(parent_obj);
                lv_obj_set_pos(el->lv_obj, ex, ey);
                lv_obj_set_size(el->lv_obj, ew, eh);
                lv_obj_remove_style_all(el->lv_obj);
                lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(el->background), 0);
                lv_obj_set_style_bg_opa(el->lv_obj, LV_OPA_TRANSP, 0);
                lv_obj_remove_flag(el->lv_obj, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(el->lv_obj, LV_OBJ_FLAG_CLICKABLE);
                {
                    const lv_image_dsc_t *dsc = vgp_image_get_for_element(el, (uint16_t)(sc->element_start + i));
                    if (dsc && dsc->header.w > 0u && dsc->header.h > 0u) {
                        el->lv_extra = lv_image_create(el->lv_obj);
                        lv_image_set_src(el->lv_extra, dsc);
                        lv_obj_center(el->lv_extra);
                        if (el->image_fit == VGP_IMG_FIT_FILL) {
                            lv_obj_set_size(el->lv_extra, ew, eh);
                            lv_image_set_inner_align(el->lv_extra, LV_IMAGE_ALIGN_STRETCH);
                        } else if (el->image_fit != VGP_IMG_FIT_NONE) {
                            uint32_t sx = ((uint32_t)ew * 256u) / dsc->header.w;
                            uint32_t sy = ((uint32_t)eh * 256u) / dsc->header.h;
                            uint32_t sc_img = (el->image_fit == VGP_IMG_FIT_COVER) ?
                                              ((sx > sy) ? sx : sy) : ((sx < sy) ? sx : sy);
                            if (sc_img < 16u) sc_img = 16u;
                            if (sc_img > 2048u) sc_img = 2048u;
                            lv_image_set_scale(el->lv_extra, sc_img);
                        }
                    }
                }
#endif
                break;

            default:
                break;
        }

        if (el->lv_obj && !el->visible) {
            lv_obj_add_flag(el->lv_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void clear_screen_widget_ptrs(int32_t s_idx)
{
    if (s_idx < 0 || s_idx >= g_proj.num_screens) return;
    vgp_screen_t *sc = &g_proj.screens[s_idx];
    for (uint16_t i = 0; i < sc->num_elements; ++i) {
        vgp_element_t *el = &g_proj.elements[sc->element_start + i];
        el->lv_obj = NULL;
        el->lv_extra = NULL;
        el->lv_aux = NULL;
    }
    if (sc->parent_screen_id[0]) {
        int32_t pidx = find_screen(sc->parent_screen_id);
        if (pidx >= 0 && pidx != s_idx) clear_screen_widget_ptrs(pidx);
    }
}

static void switch_screen_to(int32_t idx)
{
    if (idx < 0 || idx >= g_proj.num_screens) return;
    release_momentary();
    cancel_input_edit();
    if (g_current_screen_idx >= 0 && g_current_screen_idx < g_proj.num_screens) {
        clear_screen_widget_ptrs(g_current_screen_idx);
    }
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
    g_last_alarm_gen = 0u;
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
    release_momentary();
    cancel_input_edit();
    if (g_active_popup_idx >= 0) {
        clear_screen_widget_ptrs(g_active_popup_idx);
    }
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

    vgp_alarm_init(&g_proj);
    vgp_image_init(&g_proj);
    vgp_trend_init(&g_proj);
    vgp_modbus_start(&g_proj);
    g_last_alarm_gen = 0u;

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

    /* Create alarm ticker bar if enabled in project.json */
    if (g_proj.alarm_ticker.enabled) {
        g_ticker_bar = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(g_ticker_bar);
        int32_t ty = g_proj.alarm_ticker.position_bottom ? ((int32_t)g_screen_h - 56) : 0;
        lv_obj_set_pos(g_ticker_bar, 0, ty);
        lv_obj_set_size(g_ticker_bar, (int32_t)g_screen_w, 26);
        lv_obj_set_style_bg_color(g_ticker_bar, lv_color_hex(0xDC2626u), 0);
        lv_obj_set_style_bg_opa(g_ticker_bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(g_ticker_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(g_ticker_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_ticker_bar, LV_OBJ_FLAG_HIDDEN);

        g_ticker_label = lv_label_create(g_ticker_bar);
        lv_obj_set_pos(g_ticker_label, 12, 4);
        lv_obj_set_style_text_font(g_ticker_label, &vgp_font_zh_16, 0);
        lv_obj_set_style_text_color(g_ticker_label, lv_color_hex(0xFFFFFFu), 0);
        lv_label_set_text(g_ticker_label, "");
    }

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

static void update_alarm_ticker(uint32_t now_ms)
{
    if (!g_ticker_bar || !g_ticker_label) return;
    uint16_t active[VGP_MAX_ALARM_EVENTS];
    uint16_t cnt = vgp_alarm_get_active_list(active, VGP_MAX_ALARM_EVENTS);
    if (cnt == 0u) {
        lv_obj_add_flag(g_ticker_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_ticker_bar, LV_OBJ_FLAG_HIDDEN);
    uint32_t interval = g_proj.alarm_ticker.interval_ms ? g_proj.alarm_ticker.interval_ms : 3000u;
    if (g_ticker_last_ms == 0u || (uint32_t)(now_ms - g_ticker_last_ms) >= interval) {
        g_ticker_last_ms = now_ms ? now_ms : 1u;
        g_ticker_idx = (uint16_t)((g_ticker_idx + 1u) % cnt);
    }
    if (g_ticker_idx >= cnt) g_ticker_idx = 0u;
    const vgp_alarm_event_t *ev = vgp_alarm_get_event(active[g_ticker_idx]);
    if (!ev) return;

    uint32_t bg = (ev->severity == VGP_SEV_CRITICAL) ? 0x00DC2626u :
                  (ev->severity == VGP_SEV_WARNING) ? 0x00D97706u : 0x000284C7u;
    lv_obj_set_style_bg_color(g_ticker_bar, lv_color_hex(bg), 0);

    sys_datetime_t dt;
    sys_clock_epoch_to_local(ev->raised_epoch, &dt);
    char line[160];
    snprintf(line, sizeof(line), "[%u/%u] %02u:%02u:%02u %s%s",
             (unsigned)(g_ticker_idx + 1u), (unsigned)cnt,
             (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second,
             ev->message, ev->acknowledged ? " (已確認)" : "");
    set_label_if_changed(g_ticker_label, line);
}

static void update_alarm_table(vgp_element_t *el, uint16_t el_idx)
{
#if LV_USE_TABLE
    uint8_t max_rows = el->alarm_max_rows ? el->alarm_max_rows : 10u;
    if (max_rows > 20u) max_rows = 20u;
    uint16_t ids[20];
    uint16_t n = vgp_alarm_query(el, el_idx, ids, max_rows);
    if (n == 0u) {
        lv_table_set_row_count(el->lv_obj, 2u);
        lv_table_set_cell_value(el->lv_obj, 1, 0, "--:--:--");
        lv_table_set_cell_value(el->lv_obj, 1, 1, "無警報");
        lv_table_set_cell_value(el->lv_obj, 1, 2, "-");
        lv_table_set_cell_value(el->lv_obj, 1, 3, "正常");
        return;
    }
    lv_table_set_row_count(el->lv_obj, (uint32_t)n + 1u);
    for (uint16_t i = 0; i < n; ++i) {
        const vgp_alarm_event_t *ev = vgp_alarm_get_event(ids[i]);
        if (!ev) continue;
        sys_datetime_t dt;
        sys_clock_epoch_to_local(ev->raised_epoch, &dt);
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02u:%02u:%02u",
                 (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
        const char *sev = (ev->severity == VGP_SEV_CRITICAL) ? "危險" :
                          (ev->severity == VGP_SEV_WARNING) ? "警告" : "資訊";
        const char *st = (ev->cleared_epoch != 0u) ? "已恢復" :
                         ev->acknowledged ? "已確認" : "觸發中";
        lv_table_set_cell_value(el->lv_obj, (uint32_t)i + 1u, 0, tbuf);
        lv_table_set_cell_value(el->lv_obj, (uint32_t)i + 1u, 1, ev->message);
        lv_table_set_cell_value(el->lv_obj, (uint32_t)i + 1u, 2, sev);
        lv_table_set_cell_value(el->lv_obj, (uint32_t)i + 1u, 3, st);
    }
#else
    (void)el;
    (void)el_idx;
#endif
}

void vgp_runtime_update(void)
{
    if (!g_active) return;
    g_sec_counter++;

    uint32_t now_ms = (uint32_t)OSTimeGet() * (1000u / (uint32_t)OS_TICKS_PER_SEC);
    vgp_trend_tick(now_ms);
    vgp_alarm_evaluate(now_ms);
    update_alarm_ticker(now_ms);
    uint32_t alarm_gen = vgp_alarm_generation();
    bool alarm_dirty = (alarm_gen != g_last_alarm_gen);

    if (g_status_label) {
        char wifi_str[128];
        network_snapshot(wifi_str, sizeof(wifi_str));
        const bootcfg_status_t cfg = bootcfg_status();
        const char *cfg_short = (cfg == BOOTCFG_OK) ? "wifi.txt OK" : bootcfg_status_text(cfg);
        char mb_str[64];
        vgp_modbus_status_summary(mb_str, sizeof(mb_str));
        const char *ntp_str = sys_clock_is_synced() ? " [NTP]" : "";

        char usb_str[64];
        if (usb_state == 4u) {
            snprintf(usb_str, sizeof(usb_str), "USB:OK(%04lx:%04lx%s k:%lu \"%s\")",
                (unsigned long)(usb_vid_pid >> 16), (unsigned long)(usb_vid_pid & 0xffffu),
                usb_mouse_connected ? " M:OK" : "",
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
        snprintf(full_status, sizeof(full_status), "[%s]%s%s%s [%s] [CFG: %s] %s | RX:%lu TX:%lu",
            soc_is_bcm2837 ? "Zero2W" : "ZeroW",
            ntp_str,
            mb_str[0] ? " [" : "", mb_str[0] ? mb_str : "",
            usb_str, cfg_short, wifi_str, (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
        if (mb_str[0]) {
            snprintf(full_status, sizeof(full_status), "[%s]%s [%s] [%s] [CFG: %s] %s | RX:%lu TX:%lu",
                soc_is_bcm2837 ? "Zero2W" : "ZeroW",
                ntp_str, mb_str,
                usb_str, cfg_short, wifi_str, (unsigned long)net_rx_frames, (unsigned long)net_tx_frames);
        }
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
            bool modbus_locked = (v && v->modbus.enabled && (!v->modbus.writable || v->quality != VGP_QUAL_GOOD));
            if (modbus_locked &&
                (el->kind == VGP_KIND_SWITCH || el->kind == VGP_KIND_INPUT ||
                 el->kind == VGP_KIND_SLIDER || el->kind == VGP_KIND_ARC ||
                 el->kind == VGP_KIND_SPINBOX || el->kind == VGP_KIND_DROPDOWN ||
                 (el->kind == VGP_KIND_BUTTON &&
                  (el->action == VGP_ACT_TOGGLE || el->action == VGP_ACT_SET ||
                   el->action == VGP_ACT_INCREMENT || el->action == VGP_ACT_RESET ||
                   el->action == VGP_ACT_MOMENTARY)))) {
                lv_obj_add_state(el->lv_obj, LV_STATE_DISABLED);
            } else if (!el->enabled_id[0] && v && v->modbus.enabled) {
                lv_obj_remove_state(el->lv_obj, LV_STATE_DISABLED);
            }

            switch (el->kind) {
                case VGP_KIND_NUMBER:
                    if (v && v->type == VGP_VAR_NUM) {
                        char buf[64];
                        if (v->quality == VGP_QUAL_BAD) snprintf(buf, sizeof(buf), "---");
                        else if (v->quality == VGP_QUAL_WAITING) snprintf(buf, sizeof(buf), "...");
                        else format_num_val(buf, sizeof(buf), v->val_num, el);
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
                        int32_t iv = (int32_t)v->val_num;
                        if (lv_bar_get_value(el->lv_obj) != iv) {
                            lv_bar_set_value(el->lv_obj, iv, LV_ANIM_OFF);
                        }
                        uint32_t fc = threshold_fill_color(el, v->val_num);
                        lv_obj_set_style_bg_color(el->lv_obj, lv_color_hex(fc), LV_PART_INDICATOR);
                        if (el->lv_aux) {
                            char buf[64];
                            format_num_val(buf, sizeof(buf), v->val_num, el);
                            set_label_if_changed(el->lv_aux, buf);
                        }
                    }
#endif
                    break;

                case VGP_KIND_GAUGE:
                case VGP_KIND_ARC:
#if LV_USE_ARC
                    if (v && v->type == VGP_VAR_NUM) {
                        int32_t iv = (int32_t)v->val_num;
                        if (!lv_obj_has_state(el->lv_obj, LV_STATE_PRESSED) && lv_arc_get_value(el->lv_obj) != iv) {
                            lv_arc_set_value(el->lv_obj, iv);
                        }
                        uint32_t fc = threshold_fill_color(el, v->val_num);
                        lv_obj_set_style_arc_color(el->lv_obj, lv_color_hex(fc), LV_PART_INDICATOR);
                        if (el->lv_aux) {
                            char buf[64];
                            format_num_val(buf, sizeof(buf), v->val_num, el);
                            set_label_if_changed(el->lv_aux, buf);
                        }
                    }
#endif
                    break;

                case VGP_KIND_SLIDER:
#if LV_USE_SLIDER
                    if (v && v->type == VGP_VAR_NUM) {
                        int32_t iv = (int32_t)v->val_num;
                        if (!lv_obj_has_state(el->lv_obj, LV_STATE_PRESSED) && lv_slider_get_value(el->lv_obj) != iv) {
                            lv_slider_set_value(el->lv_obj, iv, LV_ANIM_OFF);
                        }
                        if (el->lv_aux) {
                            char buf[64];
                            format_num_val(buf, sizeof(buf), v->val_num, el);
                            set_label_if_changed(el->lv_aux, buf);
                        }
                    }
#endif
                    break;

                case VGP_KIND_SPINBOX:
                    if (v && el->lv_extra) {
                        char buf[64];
                        if (v->quality == VGP_QUAL_BAD) snprintf(buf, sizeof(buf), "---");
                        else if (v->quality == VGP_QUAL_WAITING) snprintf(buf, sizeof(buf), "...");
                        else format_num_val(buf, sizeof(buf), v->val_num, el);
                        set_label_if_changed(el->lv_extra, buf);
                    }
                    break;

                case VGP_KIND_DROPDOWN:
#if LV_USE_DROPDOWN
                    if (v && el->num_multistate_items > 0u && !lv_dropdown_is_open(el->lv_obj)) {
                        double cur_val = (v->type == VGP_VAR_NUM) ? v->val_num : (v->val_bool ? 1.0 : 0.0);
                        for (uint8_t m = 0; m < el->num_multistate_items; ++m) {
                            if ((int32_t)el->multistate_items[m].value == (int32_t)cur_val) {
                                if (lv_dropdown_get_selected(el->lv_obj) != m) {
                                    lv_dropdown_set_selected(el->lv_obj, m);
                                }
                                break;
                            }
                        }
                    }
#endif
                    break;

                case VGP_KIND_TREND:
                    /* Sampled continuously in background by vgp_trend_tick() */
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
                        if (g_edit_el == el) {
                            snprintf(buf, sizeof(buf), "%s_", g_edit_buf);
                        } else if (v->quality == VGP_QUAL_BAD) {
                            snprintf(buf, sizeof(buf), "---");
                        } else if (v->quality == VGP_QUAL_WAITING) {
                            snprintf(buf, sizeof(buf), "...");
                        } else if (v->type == VGP_VAR_NUM) {
                            format_num_val(buf, sizeof(buf), v->val_num, el);
                        } else if (v->type == VGP_VAR_BOOL) {
                            snprintf(buf, sizeof(buf), "%s", v->val_bool ? "ON" : "OFF");
                        } else {
                            snprintf(buf, sizeof(buf), "%s", v->val_str);
                        }
                        set_label_if_changed(el->lv_extra, buf);
                    }
                    break;

                case VGP_KIND_ALARM:
                    if (alarm_dirty) {
                        update_alarm_table(el, (uint16_t)(sc->element_start + i));
                    }
                    break;

                case VGP_KIND_DATETIME: {
                    sys_datetime_t dt;
                    sys_clock_now_local_dt(&dt);
                    char dt_buf[40];
                    sys_clock_format(el->date_time_format, &dt, dt_buf, sizeof(dt_buf));
                    set_label_if_changed(el->lv_obj, dt_buf);
                    break;
                }

                default:
                    break;
            }
        }
    }
    if (alarm_dirty) {
        g_last_alarm_gen = alarm_gen;
    }
}

void vgp_runtime_handle_key(uint32_t key)
{
    if (!g_active) return;

    if (key == KEY_ESC) {
        if (g_edit_el) {
            cancel_input_edit();
            vgp_runtime_update();
            return;
        }
        if (g_active_popup_idx >= 0) {
            close_active_popup();
            vgp_runtime_update();
            return;
        }
    }

    if (!g_kbd_group) return;
    lv_obj_t *focused = lv_group_get_focused(g_kbd_group);
    if (!focused) return;

    int32_t active_s = (g_active_popup_idx >= 0) ? g_active_popup_idx : g_current_screen_idx;
    if (active_s < 0 || active_s >= g_proj.num_screens) return;
    vgp_screen_t *sc = &g_proj.screens[active_s];

    for (uint16_t i = 0; i < sc->num_elements; ++i) {
        vgp_element_t *el = &g_proj.elements[sc->element_start + i];
        if (el->lv_obj != focused) continue;

        if (el->kind == VGP_KIND_SPINBOX || el->kind == VGP_KIND_SLIDER || el->kind == VGP_KIND_ARC) {
            if (key == '+') {
                spinbox_step(el, 1.0);
            } else if (key == '-') {
                spinbox_step(el, -1.0);
            }
            return;
        }

        if (el->kind == VGP_KIND_INPUT) {
            int32_t vi = find_var_idx(el->variable_id);
            if (vi < 0) return;
            vgp_variable_t *v = &g_proj.variables[vi];
            if (v->type == VGP_VAR_NUM) {
                if ((key >= '0' && key <= '9') || key == '.') {
                    if (g_edit_el != el) {
                        g_edit_el = el;
                        g_edit_buf[0] = '\0';
                    }
                    size_t len = strlen(g_edit_buf);
                    if (key == '.' && strchr(g_edit_buf, '.') != NULL) return;
                    if (len + 1u < sizeof(g_edit_buf)) {
                        if (key == '.' && len == 0u) {
                            g_edit_buf[0] = '0';
                            g_edit_buf[1] = '.';
                            g_edit_buf[2] = '\0';
                        } else {
                            g_edit_buf[len] = (char)key;
                            g_edit_buf[len + 1u] = '\0';
                        }
                        vgp_runtime_update();
                    }
                } else if (key == KEY_BACKSPACE) {
                    if (g_edit_el != el) {
                        g_edit_el = el;
                        format_num_val(g_edit_buf, sizeof(g_edit_buf), v->val_num, el);
                    }
                    size_t len = strlen(g_edit_buf);
                    if (len > 0u) {
                        g_edit_buf[len - 1u] = '\0';
                        vgp_runtime_update();
                    }
                } else if (key == '-') {
                    if (g_edit_el != el) {
                        spinbox_step(el, -1.0);
                    } else {
                        size_t len = strlen(g_edit_buf);
                        if (len > 0u && g_edit_buf[0] == '-') {
                            memmove(g_edit_buf, g_edit_buf + 1u, len);
                        } else if (len + 1u < sizeof(g_edit_buf)) {
                            memmove(g_edit_buf + 1u, g_edit_buf, len + 1u);
                            g_edit_buf[0] = '-';
                        }
                        vgp_runtime_update();
                    }
                } else if (key == '+' && g_edit_el != el) {
                    spinbox_step(el, 1.0);
                }
            } else if (v->type == VGP_VAR_STR) {
                if (g_edit_el != el) {
                    g_edit_el = el;
                    strncpy(g_edit_buf, v->val_str, sizeof(g_edit_buf) - 1u);
                    g_edit_buf[sizeof(g_edit_buf) - 1u] = '\0';
                }
                size_t len = strlen(g_edit_buf);
                if (key >= 32 && key < 127 && len + 1u < sizeof(g_edit_buf)) {
                    g_edit_buf[len] = (char)key;
                    g_edit_buf[len + 1u] = '\0';
                    vgp_runtime_update();
                } else if (key == KEY_BACKSPACE && len > 0u) {
                    g_edit_buf[len - 1u] = '\0';
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

