/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_parser.h"
#include <string.h>
#include <stdio.h>

static vgp_parse_report_t *g_cur_report = NULL;

static void report_warn(const char *ctx, const char *detail)
{
    if (!g_cur_report) return;
    g_cur_report->total_warnings++;
    if (g_cur_report->stored_warnings < VGP_MAX_PARSE_WARNINGS) {
        char *dst = g_cur_report->warnings[g_cur_report->stored_warnings++];
        if (ctx && ctx[0]) {
            snprintf(dst, 96u, "%s: %s", ctx, detail ? detail : "");
        } else {
            snprintf(dst, 96u, "%s", detail ? detail : "");
        }
    }
}

static void report_err(const char *msg)
{
    if (!g_cur_report || g_cur_report->error[0]) return;
    snprintf(g_cur_report->error, sizeof(g_cur_report->error), "%s", msg ? msg : "parse error");
}

static double parse_double_str(const char *s)
{
    if (!s) return 0.0;
    while (*s == ' ' || *s == '\t') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') { s++; }
    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (double)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp_val = 0;
        while (*s >= '0' && *s <= '9') {
            exp_val = exp_val * 10 + (*s - '0');
            s++;
        }
        while (exp_val > 0) {
            val = (exp_sign > 0) ? (val * 10.0) : (val * 0.1);
            exp_val--;
        }
    }
    return sign * val;
}

static uint32_t parse_hex_u32(const char *s)
{
    uint32_t v = 0;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static void skip_ws(const char *s, size_t len, size_t *p)
{
    while (*p < len && ((unsigned char)s[*p] <= ' ' || s[*p] == '\0')) {
        (*p)++;
    }
}

static bool match_char(const char *s, size_t len, size_t *p, char c)
{
    skip_ws(s, len, p);
    if (*p < len && s[*p] == c) {
        (*p)++;
        return true;
    }
    return false;
}

static bool parse_str(const char *s, size_t len, size_t *p, char *out, size_t max_out)
{
    skip_ws(s, len, p);
    if (*p >= len || s[*p] != '"') return false;
    (*p)++;
    size_t out_idx = 0;
    while (*p < len && s[*p] != '"') {
        char c = s[*p];
        if (c == '\\' && *p + 1 < len) {
            (*p)++;
            c = s[*p];
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == '"') c = '"';
            else if (c == '\\') c = '\\';
            else if (c == '/') c = '/';
        }
        if (out && out_idx + 1 < max_out) {
            out[out_idx++] = c;
        }
        (*p)++;
    }
    if (out && max_out > 0) out[out_idx] = '\0';
    if (*p < len && s[*p] == '"') {
        (*p)++;
        return true;
    }
    return false;
}

static bool parse_num(const char *s, size_t len, size_t *p, double *out)
{
    skip_ws(s, len, p);
    if (*p >= len) return false;
    size_t start = *p;
    if (s[*p] == '-' || s[*p] == '+') (*p)++;
    bool has_digit = false;
    while (*p < len && s[*p] >= '0' && s[*p] <= '9') {
        has_digit = true;
        (*p)++;
    }
    if (*p < len && s[*p] == '.') {
        (*p)++;
        while (*p < len && s[*p] >= '0' && s[*p] <= '9') {
            has_digit = true;
            (*p)++;
        }
    }
    if (has_digit && *p < len && (s[*p] == 'e' || s[*p] == 'E')) {
        (*p)++;
        if (*p < len && (s[*p] == '-' || s[*p] == '+')) (*p)++;
        while (*p < len && s[*p] >= '0' && s[*p] <= '9') (*p)++;
    }
    if (!has_digit) {
        *p = start;
        return false;
    }
    char buf[40];
    size_t n = *p - start;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s + start, n);
    buf[n] = '\0';
    if (out) *out = parse_double_str(buf);
    return true;
}

static bool parse_boolean(const char *s, size_t len, size_t *p, bool *out)
{
    skip_ws(s, len, p);
    if (*p + 4 <= len && memcmp(s + *p, "true", 4) == 0) {
        *p += 4;
        if (out) *out = true;
        return true;
    }
    if (*p + 5 <= len && memcmp(s + *p, "false", 5) == 0) {
        *p += 5;
        if (out) *out = false;
        return true;
    }
    return false;
}

static void skip_val(const char *s, size_t len, size_t *p)
{
    skip_ws(s, len, p);
    if (*p >= len) return;
    char c = s[*p];
    if (c == '"') {
        (*p)++;
        while (*p < len && s[*p] != '"') {
            if (s[*p] == '\\' && *p + 1 < len) (*p)++;
            (*p)++;
        }
        if (*p < len) (*p)++;
    } else if (c == '{' || c == '[') {
        char close_c = (c == '{') ? '}' : ']';
        int depth = 1;
        (*p)++;
        bool in_str = false;
        while (*p < len && depth > 0) {
            char ch = s[*p];
            if (in_str) {
                if (ch == '\\' && *p + 1 < len) (*p)++;
                else if (ch == '"') in_str = false;
            } else {
                if (ch == '"') in_str = true;
                else if (ch == c) depth++;
                else if (ch == close_c) depth--;
            }
            (*p)++;
        }
    } else {
        while (*p < len && s[*p] != ',' && s[*p] != '}' && s[*p] != ']' &&
               (unsigned char)s[*p] > ' ') {
            (*p)++;
        }
    }
}

static uint32_t parse_color_hex(const char *str, uint32_t def_val)
{
    if (!str || str[0] != '#') return def_val;
    const char *hex = str + 1;
    size_t l = strlen(hex);
    if (l == 6) {
        return parse_hex_u32(hex) & 0x00FFFFFFu;
    } else if (l == 3) {
        uint32_t val = parse_hex_u32(hex);
        uint32_t r = (val >> 8) & 0xF;
        uint32_t g = (val >> 4) & 0xF;
        uint32_t b = val & 0xF;
        return ((r | (r << 4)) << 16) | ((g | (g << 4)) << 8) | (b | (b << 4));
    }
    return def_val;
}

static vgp_kind_t parse_kind_str(const char *str)
{
    if (!str) return VGP_KIND_UNKNOWN;
    if (strcmp(str, "text") == 0) return VGP_KIND_TEXT;
    if (strcmp(str, "rectangle") == 0) return VGP_KIND_RECTANGLE;
    if (strcmp(str, "circle") == 0) return VGP_KIND_CIRCLE;
    if (strcmp(str, "line") == 0) return VGP_KIND_LINE;
    if (strcmp(str, "button") == 0) return VGP_KIND_BUTTON;
    if (strcmp(str, "indicator") == 0) return VGP_KIND_INDICATOR;
    if (strcmp(str, "number") == 0) return VGP_KIND_NUMBER;
    if (strcmp(str, "input") == 0) return VGP_KIND_INPUT;
    if (strcmp(str, "switch") == 0) return VGP_KIND_SWITCH;
    if (strcmp(str, "gauge") == 0) return VGP_KIND_GAUGE;
    if (strcmp(str, "bar") == 0) return VGP_KIND_BAR;
    if (strcmp(str, "trend") == 0) return VGP_KIND_TREND;
    if (strcmp(str, "alarm") == 0) return VGP_KIND_ALARM;
    if (strcmp(str, "datetime") == 0) return VGP_KIND_DATETIME;
    if (strcmp(str, "multistate") == 0) return VGP_KIND_MULTISTATE;
    if (strcmp(str, "scale") == 0) return VGP_KIND_SCALE;
    if (strcmp(str, "table") == 0) return VGP_KIND_TABLE;
    if (strcmp(str, "image") == 0) return VGP_KIND_IMAGE;
    if (strcmp(str, "slider") == 0) return VGP_KIND_SLIDER;
    if (strcmp(str, "arc") == 0) return VGP_KIND_ARC;
    if (strcmp(str, "spinbox") == 0) return VGP_KIND_SPINBOX;
    if (strcmp(str, "dropdown") == 0) return VGP_KIND_DROPDOWN;
    return VGP_KIND_UNKNOWN;
}

static vgp_action_t parse_action_str(const char *str)
{
    if (!str) return VGP_ACT_NONE;
    if (strcmp(str, "toggle") == 0) return VGP_ACT_TOGGLE;
    if (strcmp(str, "set") == 0) return VGP_ACT_SET;
    if (strcmp(str, "increment") == 0) return VGP_ACT_INCREMENT;
    if (strcmp(str, "reset") == 0) return VGP_ACT_RESET;
    if (strcmp(str, "momentary") == 0) return VGP_ACT_MOMENTARY;
    if (strcmp(str, "screen") == 0) return VGP_ACT_SCREEN;
    if (strcmp(str, "previousScreen") == 0) return VGP_ACT_PREV_SCREEN;
    if (strcmp(str, "openDialog") == 0) return VGP_ACT_OPEN_DIALOG;
    if (strcmp(str, "closeDialog") == 0) return VGP_ACT_CLOSE_DIALOG;
    if (strcmp(str, "popup") == 0) return VGP_ACT_POPUP;
    if (strcmp(str, "closePopup") == 0) return VGP_ACT_CLOSE_POPUP;
    return VGP_ACT_NONE;
}

static vgp_severity_t parse_severity_str(const char *str)
{
    if (!str) return VGP_SEV_INFO;
    if (strcmp(str, "critical") == 0) return VGP_SEV_CRITICAL;
    if (strcmp(str, "warning") == 0) return VGP_SEV_WARNING;
    return VGP_SEV_INFO;
}

static vgp_alarm_op_t parse_alarm_op_str(const char *str)
{
    if (!str) return VGP_OP_GT;
    if (strcmp(str, "gt") == 0) return VGP_OP_GT;
    if (strcmp(str, "gte") == 0) return VGP_OP_GTE;
    if (strcmp(str, "lt") == 0) return VGP_OP_LT;
    if (strcmp(str, "lte") == 0) return VGP_OP_LTE;
    if (strcmp(str, "eq") == 0) return VGP_OP_EQ;
    if (strcmp(str, "ne") == 0) return VGP_OP_NE;
    if (strcmp(str, "true") == 0) return VGP_OP_TRUE;
    if (strcmp(str, "false") == 0) return VGP_OP_FALSE;
    return VGP_OP_GT;
}

static vgp_alarm_src_t parse_alarm_src_str(const char *str)
{
    if (!str) return VGP_ALARM_SRC_LOCAL;
    if (strcmp(str, "global") == 0) return VGP_ALARM_SRC_GLOBAL;
    if (strcmp(str, "all") == 0) return VGP_ALARM_SRC_ALL;
    return VGP_ALARM_SRC_LOCAL;
}

static vgp_mb_area_t parse_mb_area_str(const char *str)
{
    if (!str) return VGP_MB_AREA_HOLDING_REG;
    if (strcmp(str, "coil") == 0) return VGP_MB_AREA_COIL;
    if (strcmp(str, "discreteInput") == 0) return VGP_MB_AREA_DISCRETE_INPUT;
    if (strcmp(str, "holdingRegister") == 0) return VGP_MB_AREA_HOLDING_REG;
    if (strcmp(str, "inputRegister") == 0) return VGP_MB_AREA_INPUT_REG;
    return VGP_MB_AREA_HOLDING_REG;
}

static vgp_mb_datatype_t parse_mb_datatype_str(const char *str)
{
    if (!str) return VGP_MB_TYPE_UINT16;
    if (strcmp(str, "bool") == 0) return VGP_MB_TYPE_BOOL;
    if (strcmp(str, "uint16") == 0) return VGP_MB_TYPE_UINT16;
    if (strcmp(str, "int16") == 0) return VGP_MB_TYPE_INT16;
    if (strcmp(str, "uint32") == 0) return VGP_MB_TYPE_UINT32;
    if (strcmp(str, "int32") == 0) return VGP_MB_TYPE_INT32;
    if (strcmp(str, "float32") == 0) return VGP_MB_TYPE_FLOAT32;
    return VGP_MB_TYPE_UINT16;
}

static vgp_img_fit_t parse_img_fit_str(const char *str)
{
    if (!str) return VGP_IMG_FIT_CONTAIN;
    if (strcmp(str, "cover") == 0) return VGP_IMG_FIT_COVER;
    if (strcmp(str, "fill") == 0) return VGP_IMG_FIT_FILL;
    if (strcmp(str, "none") == 0) return VGP_IMG_FIT_NONE;
    return VGP_IMG_FIT_CONTAIN;
}

/* Locate a JSON string value representing a data:image/(png|jpeg);base64,... URI without copying it. */
static bool parse_data_uri_ref(const char *s, size_t len, size_t *p,
                               const char **out_b64, size_t *out_len, bool *out_is_png)
{
    skip_ws(s, len, p);
    if (*p >= len || s[*p] != '"') return false;
    (*p)++;
    size_t start = *p;
    while (*p < len && s[*p] != '"') {
        if (s[*p] == '\\' && *p + 1 < len) (*p)++;
        (*p)++;
    }
    size_t end = *p;
    if (*p < len && s[*p] == '"') (*p)++;

    size_t slen = end - start;
    const char *ptr = s + start;
    const char *png_hdr = "data:image/png;base64,";
    const char *jpg_hdr = "data:image/jpeg;base64,";
    const size_t png_hlen = 22u;
    const size_t jpg_hlen = 23u;

    if (slen > png_hlen && memcmp(ptr, png_hdr, png_hlen) == 0) {
        if (out_b64) *out_b64 = ptr + png_hlen;
        if (out_len) *out_len = slen - png_hlen;
        if (out_is_png) *out_is_png = true;
        return true;
    }
    if (slen > jpg_hlen && memcmp(ptr, jpg_hdr, jpg_hlen) == 0) {
        if (out_b64) *out_b64 = ptr + jpg_hlen;
        if (out_len) *out_len = slen - jpg_hlen;
        if (out_is_png) *out_is_png = false;
        return true;
    }
    return false;
}

static void parse_element_obj(const char *s, size_t len, size_t *p, vgp_element_t *e, vgp_project_t *proj)
{
    memset(e, 0, sizeof(*e));
    e->visible = true;
    e->color = 0x00E2E8F0u;
    e->background = 0x0018263Bu;
    e->border_color = 0x00475569u;
    e->font_size = 16;
    e->scale_max = 100.0;
    e->fill_color = 0x0038BDF8u;
    e->track_color = 0x00334155u;
    e->action = VGP_ACT_NONE;
    e->alarm_max_rows = 20;
    e->alarm_source = VGP_ALARM_SRC_LOCAL;
    e->alarm_rule_start = proj ? proj->total_local_alarm_rules : 0u;
    e->input_step = 1.0;
    e->sample_interval_ms = 1000u;
    e->history_points = 60u;
    e->scale_show_axis = true;
    e->scale_show_labels = true;
    e->grid_color = 0x00334155u;
    e->word_wrap = true;
    e->shape_filled = true;
    e->line_height_x100 = 120u;
    e->border_style = VGP_BORDER_SOLID;
    e->show_value = true;
    e->show_scale = true;
    e->table_cols = 3u;
    e->table_rows = 4u;

    if (!match_char(s, len, p, '{')) return;

    char key[64], sval[128];
    while (*p < len && s[*p] != '}') {
        if (!parse_str(s, len, p, key, sizeof(key))) break;
        if (!match_char(s, len, p, ':')) break;

        if (strcmp(key, "id") == 0) {
            parse_str(s, len, p, e->id, sizeof(e->id));
        } else if (strcmp(key, "name") == 0) {
            parse_str(s, len, p, e->name, sizeof(e->name));
        } else if (strcmp(key, "kind") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->kind = parse_kind_str(sval);
                if (e->kind == VGP_KIND_UNKNOWN) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "unsupported kind '%s'", sval);
                    report_warn(e->id[0] ? e->id : "element", msg);
                }
            }
        } else if (strcmp(key, "x") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->x = (int32_t)v;
        } else if (strcmp(key, "y") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->y = (int32_t)v;
        } else if (strcmp(key, "width") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->width = (int32_t)v;
        } else if (strcmp(key, "height") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->height = (int32_t)v;
        } else if (strcmp(key, "text") == 0) {
            parse_str(s, len, p, e->text, sizeof(e->text));
        } else if (strcmp(key, "color") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->color = parse_color_hex(sval, e->color);
        } else if (strcmp(key, "background") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->background = parse_color_hex(sval, e->background);
        } else if (strcmp(key, "borderColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->border_color = parse_color_hex(sval, e->border_color);
                e->has_border_color = true;
            }
        } else if (strcmp(key, "fontSize") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->font_size = (uint16_t)v;
        } else if (strcmp(key, "borderWidth") == 0) {
            double v; if (parse_num(s, len, p, &v)) { e->border_width = (uint16_t)v; e->has_border_width = true; }
        } else if (strcmp(key, "borderRadius") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->border_radius = (uint16_t)v;
        } else if (strcmp(key, "variableId") == 0) {
            parse_str(s, len, p, e->variable_id, sizeof(e->variable_id));
        } else if (strcmp(key, "action") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->action = parse_action_str(sval);
        } else if (strcmp(key, "actionValue") == 0) {
            parse_str(s, len, p, e->action_value, sizeof(e->action_value));
        } else if (strcmp(key, "targetScreenId") == 0) {
            parse_str(s, len, p, e->target_screen_id, sizeof(e->target_screen_id));
        } else if (strcmp(key, "visible") == 0) {
            parse_boolean(s, len, p, &e->visible);
        } else if (strcmp(key, "conditionId") == 0) {
            parse_str(s, len, p, e->condition_id, sizeof(e->condition_id));
        } else if (strcmp(key, "enabledId") == 0) {
            parse_str(s, len, p, e->enabled_id, sizeof(e->enabled_id));
        } else if (strcmp(key, "scaleMin") == 0) {
            parse_num(s, len, p, &e->scale_min);
        } else if (strcmp(key, "scaleMax") == 0) {
            parse_num(s, len, p, &e->scale_max);
        } else if (strcmp(key, "fillColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->fill_color = parse_color_hex(sval, e->fill_color);
        } else if (strcmp(key, "trackColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->track_color = parse_color_hex(sval, e->track_color);
        } else if (strcmp(key, "orientation") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->orientation_vertical = (strcmp(sval, "vertical") == 0);
        } else if (strcmp(key, "decimals") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->decimals = (uint8_t)v;
        } else if (strcmp(key, "unit") == 0) {
            parse_str(s, len, p, e->unit, sizeof(e->unit));
        } else if (strcmp(key, "dateTimeFormat") == 0) {
            parse_str(s, len, p, e->date_time_format, sizeof(e->date_time_format));
        } else if (strcmp(key, "tableCols") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->table_cols = (uint8_t)v;
        } else if (strcmp(key, "tableRows") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->table_rows = (uint8_t)v;
        } else if (strcmp(key, "scaleMajorTicks") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->scale_major_ticks = (uint8_t)v;
        } else if (strcmp(key, "scaleMinorTicks") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->scale_minor_ticks = (uint8_t)v;
        } else if (strcmp(key, "imageAssetId") == 0) {
            parse_str(s, len, p, e->image_asset_id, sizeof(e->image_asset_id));
        } else if (strcmp(key, "image") == 0) {
            (void)parse_data_uri_ref(s, len, p, &e->inline_b64_ptr, &e->inline_b64_len, &e->inline_is_png);
        } else if (strcmp(key, "imageFit") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->image_fit = parse_img_fit_str(sval);
        } else if (strcmp(key, "inputMin") == 0) {
            if (parse_num(s, len, p, &e->input_min)) e->has_input_min = true;
        } else if (strcmp(key, "inputMax") == 0) {
            if (parse_num(s, len, p, &e->input_max)) e->has_input_max = true;
        } else if (strcmp(key, "inputStep") == 0) {
            double v; if (parse_num(s, len, p, &v) && v > 0.0) e->input_step = v;
        } else if (strcmp(key, "warningThreshold") == 0) {
            if (parse_num(s, len, p, &e->warning_threshold)) e->has_warning_threshold = true;
        } else if (strcmp(key, "criticalThreshold") == 0) {
            if (parse_num(s, len, p, &e->critical_threshold)) e->has_critical_threshold = true;
        } else if (strcmp(key, "showValue") == 0) {
            parse_boolean(s, len, p, &e->show_value);
        } else if (strcmp(key, "showScale") == 0) {
            parse_boolean(s, len, p, &e->show_scale);
        } else if (strcmp(key, "sampleInterval") == 0) {
            double v; if (parse_num(s, len, p, &v) && v >= 100.0) e->sample_interval_ms = (uint32_t)v;
        } else if (strcmp(key, "historyPoints") == 0) {
            double v; if (parse_num(s, len, p, &v) && v >= 2.0) e->history_points = (uint16_t)v;
        } else if (strcmp(key, "scaleOrientation") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->scale_vertical = (strcmp(sval, "vertical") == 0);
        } else if (strcmp(key, "scaleShowAxis") == 0) {
            parse_boolean(s, len, p, &e->scale_show_axis);
        } else if (strcmp(key, "scaleShowLabels") == 0) {
            parse_boolean(s, len, p, &e->scale_show_labels);
        } else if (strcmp(key, "gridColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->grid_color = parse_color_hex(sval, e->grid_color);
        } else if (strcmp(key, "numberFormat") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                if (strcmp(sval, "integer") == 0) e->number_format = VGP_NUMFMT_INTEGER;
                else if (strcmp(sval, "percent") == 0) e->number_format = VGP_NUMFMT_PERCENT;
                else if (strcmp(sval, "scientific") == 0) e->number_format = VGP_NUMFMT_SCIENTIFIC;
                else e->number_format = VGP_NUMFMT_DECIMAL;
            }
        } else if (strcmp(key, "numberLocale") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                if (strcmp(sval, "de-DE") == 0) e->number_locale = VGP_LOCALE_DE_DE;
                else if (strcmp(sval, "zh-TW") == 0) e->number_locale = VGP_LOCALE_ZH_TW;
                else e->number_locale = VGP_LOCALE_EN_US;
            }
        } else if (strcmp(key, "useGrouping") == 0) {
            parse_boolean(s, len, p, &e->use_grouping);
        } else if (strcmp(key, "numberPrefix") == 0) {
            parse_str(s, len, p, e->number_prefix, sizeof(e->number_prefix));
        } else if (strcmp(key, "textAlign") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                if (strcmp(sval, "center") == 0) e->text_align = VGP_ALIGN_CENTER;
                else if (strcmp(sval, "right") == 0) e->text_align = VGP_ALIGN_RIGHT;
                else e->text_align = VGP_ALIGN_LEFT;
            }
        } else if (strcmp(key, "verticalAlign") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                if (strcmp(sval, "middle") == 0) e->vertical_align = VGP_VALIGN_MIDDLE;
                else if (strcmp(sval, "bottom") == 0) e->vertical_align = VGP_VALIGN_BOTTOM;
                else e->vertical_align = VGP_VALIGN_TOP;
            }
        } else if (strcmp(key, "wordWrap") == 0) {
            parse_boolean(s, len, p, &e->word_wrap);
        } else if (strcmp(key, "underline") == 0) {
            parse_boolean(s, len, p, &e->underline);
        } else if (strcmp(key, "textBackground") == 0) {
            parse_boolean(s, len, p, &e->text_background);
        } else if (strcmp(key, "shapeFilled") == 0) {
            parse_boolean(s, len, p, &e->shape_filled);
        } else if (strcmp(key, "lineHeight") == 0) {
            double v;
            if (parse_num(s, len, p, &v) && v >= 1.0 && v <= 3.0) {
                e->line_height_x100 = (uint16_t)(v * 100.0 + 0.5);
            }
        } else if (strcmp(key, "borderStyle") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                if (strcmp(sval, "none") == 0) e->border_style = VGP_BORDER_NONE;
                else if (strcmp(sval, "dashed") == 0) e->border_style = VGP_BORDER_DASHED;
                else if (strcmp(sval, "dotted") == 0) e->border_style = VGP_BORDER_DOTTED;
                else e->border_style = VGP_BORDER_SOLID;
            }
        } else if (strcmp(key, "buttonPressedBackground") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->btn_pressed_bg = parse_color_hex(sval, 0u);
                e->has_btn_pressed_bg = true;
            }
        } else if (strcmp(key, "buttonPressedColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->btn_pressed_color = parse_color_hex(sval, 0x00FFFFFFu);
                e->has_btn_pressed_color = true;
            }
        } else if (strcmp(key, "buttonDisabledBackground") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->btn_disabled_bg = parse_color_hex(sval, 0u);
                e->has_btn_disabled_bg = true;
            }
        } else if (strcmp(key, "buttonDisabledColor") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) {
                e->btn_disabled_color = parse_color_hex(sval, 0x0094A3B8u);
                e->has_btn_disabled_color = true;
            }
        } else if (strcmp(key, "multistateItems") == 0) {
            if (match_char(s, len, p, '[')) {
                e->num_multistate_items = 0;
                while (*p < len && s[*p] != ']') {
                    if (match_char(s, len, p, '{')) {
                        if (e->num_multistate_items < VGP_MAX_MULTISTATE_ITEMS) {
                            vgp_multistate_item_t *item = &e->multistate_items[e->num_multistate_items++];
                            item->color = 0x0064748Bu;
                            item->text_color = 0x00FFFFFFu;
                            char ikey[32], isval[64];
                            while (*p < len && s[*p] != '}') {
                                if (!parse_str(s, len, p, ikey, sizeof(ikey))) break;
                                if (!match_char(s, len, p, ':')) break;
                                if (strcmp(ikey, "value") == 0) parse_num(s, len, p, &item->value);
                                else if (strcmp(ikey, "label") == 0) parse_str(s, len, p, item->label, sizeof(item->label));
                                else if (strcmp(ikey, "color") == 0) {
                                    if (parse_str(s, len, p, isval, sizeof(isval))) item->color = parse_color_hex(isval, item->color);
                                } else if (strcmp(ikey, "textColor") == 0) {
                                    if (parse_str(s, len, p, isval, sizeof(isval))) item->text_color = parse_color_hex(isval, item->text_color);
                                } else skip_val(s, len, p);
                                match_char(s, len, p, ',');
                            }
                        } else skip_val(s, len, p);
                        match_char(s, len, p, '}');
                    }
                    match_char(s, len, p, ',');
                }
                match_char(s, len, p, ']');
            } else skip_val(s, len, p);
        } else if (strcmp(key, "alarmMaxRows") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->alarm_max_rows = (uint8_t)v;
        } else if (strcmp(key, "alarmSource") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) e->alarm_source = parse_alarm_src_str(sval);
        } else if (strcmp(key, "alarmGroup") == 0) {
            parse_str(s, len, p, e->alarm_group, sizeof(e->alarm_group));
        } else if (strcmp(key, "alarmRules") == 0) {
            if (match_char(s, len, p, '[')) {
                e->alarm_rule_start = proj ? proj->total_local_alarm_rules : 0u;
                e->num_alarm_rules = 0u;
                while (*p < len && s[*p] != ']') {
                    if (proj && proj->total_local_alarm_rules < VGP_MAX_LOCAL_ALARM_RULES) {
                        if (match_char(s, len, p, '{')) {
                            vgp_local_alarm_rule_t *r = &proj->local_alarm_rules[proj->total_local_alarm_rules++];
                            memset(r, 0, sizeof(*r));
                            r->enabled = true;
                            char rkey[32], rsval[96];
                            while (*p < len && s[*p] != '}') {
                                if (!parse_str(s, len, p, rkey, sizeof(rkey))) break;
                                if (!match_char(s, len, p, ':')) break;
                                if (strcmp(rkey, "id") == 0) parse_str(s, len, p, r->id, sizeof(r->id));
                                else if (strcmp(rkey, "message") == 0) parse_str(s, len, p, r->message, sizeof(r->message));
                                else if (strcmp(rkey, "variableId") == 0) parse_str(s, len, p, r->variable_id, sizeof(r->variable_id));
                                else if (strcmp(rkey, "operator") == 0) {
                                    if (parse_str(s, len, p, rsval, sizeof(rsval))) r->op = parse_alarm_op_str(rsval);
                                } else if (strcmp(rkey, "threshold") == 0) parse_num(s, len, p, &r->threshold);
                                else if (strcmp(rkey, "severity") == 0) {
                                    if (parse_str(s, len, p, rsval, sizeof(rsval))) r->severity = parse_severity_str(rsval);
                                } else if (strcmp(rkey, "enabled") == 0) parse_boolean(s, len, p, &r->enabled);
                                else skip_val(s, len, p);
                                match_char(s, len, p, ',');
                            }
                            match_char(s, len, p, '}');
                            e->num_alarm_rules++;
                        } else skip_val(s, len, p);
                    } else skip_val(s, len, p);
                    match_char(s, len, p, ',');
                }
                match_char(s, len, p, ']');
            } else skip_val(s, len, p);
        } else {
            /* Ignore benign editor-only metadata; warn on unsupported runtime properties */
            if (strcmp(key, "locked") != 0 && strcmp(key, "groupId") != 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "ignored prop '%s'", key);
                report_warn(e->id[0] ? e->id : "element", msg);
            }
            skip_val(s, len, p);
        }
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, '}');
}

static void parse_screens_array(const char *s, size_t len, size_t *p, vgp_project_t *proj)
{
    if (!match_char(s, len, p, '[')) return;
    while (*p < len && s[*p] != ']' && proj->num_screens < VGP_MAX_SCREENS) {
        if (!match_char(s, len, p, '{')) break;

        vgp_screen_t *sc = &proj->screens[proj->num_screens++];
        memset(sc, 0, sizeof(*sc));
        sc->width = 960;
        sc->height = 600;
        sc->background = 0x00101C2Eu;
        sc->element_start = proj->total_elements;

        char key[64], sval[128];
        while (*p < len && s[*p] != '}') {
            if (!parse_str(s, len, p, key, sizeof(key))) break;
            if (!match_char(s, len, p, ':')) break;

            if (strcmp(key, "id") == 0) {
                parse_str(s, len, p, sc->id, sizeof(sc->id));
            } else if (strcmp(key, "name") == 0) {
                parse_str(s, len, p, sc->name, sizeof(sc->name));
            } else if (strcmp(key, "width") == 0) {
                double v; if (parse_num(s, len, p, &v)) sc->width = (int32_t)v;
            } else if (strcmp(key, "height") == 0) {
                double v; if (parse_num(s, len, p, &v)) sc->height = (int32_t)v;
            } else if (strcmp(key, "background") == 0) {
                if (parse_str(s, len, p, sval, sizeof(sval))) sc->background = parse_color_hex(sval, sc->background);
            } else if (strcmp(key, "type") == 0) {
                if (parse_str(s, len, p, sval, sizeof(sval))) {
                    sc->is_popup = (strcmp(sval, "popup") == 0 || strcmp(sval, "dialog") == 0);
                }
            } else if (strcmp(key, "parentScreenId") == 0 || strcmp(key, "inheritsScreenId") == 0) {
                parse_str(s, len, p, sc->parent_screen_id, sizeof(sc->parent_screen_id));
            } else if (strcmp(key, "elements") == 0) {
                if (match_char(s, len, p, '[')) {
                    while (*p < len && s[*p] != ']') {
                        if (proj->total_elements < VGP_MAX_TOTAL_ELEMENTS) {
                            vgp_element_t *el = &proj->elements[proj->total_elements++];
                            parse_element_obj(s, len, p, el, proj);
                            sc->num_elements++;
                        } else {
                            report_warn(sc->id[0] ? sc->id : "screen", "elements limit reached (768)");
                            skip_val(s, len, p);
                        }
                        match_char(s, len, p, ',');
                    }
                    match_char(s, len, p, ']');
                } else skip_val(s, len, p);
            } else {
                skip_val(s, len, p);
            }
            match_char(s, len, p, ',');
        }
        match_char(s, len, p, '}');
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, ']');
}

static void parse_variables_array(const char *s, size_t len, size_t *p, vgp_project_t *proj)
{
    if (!match_char(s, len, p, '[')) return;
    while (*p < len && s[*p] != ']' && proj->num_variables < VGP_MAX_VARIABLES) {
        if (!match_char(s, len, p, '{')) break;

        vgp_variable_t *var = &proj->variables[proj->num_variables++];
        memset(var, 0, sizeof(*var));

        char key[64], sval[128];
        while (*p < len && s[*p] != '}') {
            if (!parse_str(s, len, p, key, sizeof(key))) break;
            if (!match_char(s, len, p, ':')) break;

            if (strcmp(key, "id") == 0) {
                parse_str(s, len, p, var->id, sizeof(var->id));
            } else if (strcmp(key, "name") == 0) {
                parse_str(s, len, p, var->name, sizeof(var->name));
            } else if (strcmp(key, "type") == 0) {
                if (parse_str(s, len, p, sval, sizeof(sval))) {
                    if (strcmp(sval, "number") == 0) var->type = VGP_VAR_NUM;
                    else if (strcmp(sval, "boolean") == 0) var->type = VGP_VAR_BOOL;
                    else if (strcmp(sval, "string") == 0) var->type = VGP_VAR_STR;
                }
            } else if (strcmp(key, "initial") == 0) {
                skip_ws(s, len, p);
                if (*p < len && (s[*p] == 't' || s[*p] == 'f')) {
                    parse_boolean(s, len, p, &var->initial_bool);
                    var->val_bool = var->initial_bool;
                    var->type = VGP_VAR_BOOL;
                } else if (*p < len && s[*p] == '"') {
                    parse_str(s, len, p, var->initial_str, sizeof(var->initial_str));
                    strncpy(var->val_str, var->initial_str, sizeof(var->val_str) - 1);
                    var->type = VGP_VAR_STR;
                } else {
                    parse_num(s, len, p, &var->initial_num);
                    var->val_num = var->initial_num;
                    var->type = VGP_VAR_NUM;
                }
            } else if (strcmp(key, "modbus") == 0) {
                if (match_char(s, len, p, '{')) {
                    vgp_modbus_map_t *m = &var->modbus;
                    memset(m, 0, sizeof(*m));
                    m->enabled = true;
                    m->device_idx = -1;
                    m->scale = 1.0;
                    m->offset = 0.0;
                    char mkey[32], msval[64];
                    while (*p < len && s[*p] != '}') {
                        if (!parse_str(s, len, p, mkey, sizeof(mkey))) break;
                        if (!match_char(s, len, p, ':')) break;
                        if (strcmp(mkey, "deviceId") == 0) parse_str(s, len, p, m->device_id, sizeof(m->device_id));
                        else if (strcmp(mkey, "area") == 0) {
                            if (parse_str(s, len, p, msval, sizeof(msval))) m->area = parse_mb_area_str(msval);
                        } else if (strcmp(mkey, "address") == 0) {
                            double v; if (parse_num(s, len, p, &v)) m->address = (uint16_t)v;
                        } else if (strcmp(mkey, "dataType") == 0) {
                            if (parse_str(s, len, p, msval, sizeof(msval))) m->data_type = parse_mb_datatype_str(msval);
                        } else if (strcmp(mkey, "wordOrder") == 0) {
                            if (parse_str(s, len, p, msval, sizeof(msval))) m->low_word_first = (strcmp(msval, "lowFirst") == 0);
                        } else if (strcmp(mkey, "scale") == 0) {
                            parse_num(s, len, p, &m->scale);
                        } else if (strcmp(mkey, "offset") == 0) {
                            parse_num(s, len, p, &m->offset);
                        } else if (strcmp(mkey, "access") == 0) {
                            if (parse_str(s, len, p, msval, sizeof(msval))) m->writable = (strcmp(msval, "readWrite") == 0);
                        } else skip_val(s, len, p);
                        match_char(s, len, p, ',');
                    }
                    match_char(s, len, p, '}');
                } else skip_val(s, len, p);
            } else {
                skip_val(s, len, p);
            }
            match_char(s, len, p, ',');
        }
        match_char(s, len, p, '}');
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, ']');
}

static void parse_devices_array(const char *s, size_t len, size_t *p, vgp_project_t *proj)
{
    if (!match_char(s, len, p, '[')) return;
    while (*p < len && s[*p] != ']') {
        if (proj->num_devices < VGP_MAX_DEVICES && match_char(s, len, p, '{')) {
            vgp_device_t *d = &proj->devices[proj->num_devices++];
            memset(d, 0, sizeof(*d));
            d->enabled = true;
            d->unit_id = 1u;
            d->poll_interval_ms = 500u;
            d->timeout_ms = 2000u;
            d->port = 502u;
            d->baud_rate = 9600u;
            d->parity = VGP_MB_PARITY_NONE;
            d->stop_bits = 1u;

            char key[32], sval[64];
            while (*p < len && s[*p] != '}') {
                if (!parse_str(s, len, p, key, sizeof(key))) break;
                if (!match_char(s, len, p, ':')) break;
                if (strcmp(key, "id") == 0) parse_str(s, len, p, d->id, sizeof(d->id));
                else if (strcmp(key, "name") == 0) parse_str(s, len, p, d->name, sizeof(d->name));
                else if (strcmp(key, "enabled") == 0) parse_boolean(s, len, p, &d->enabled);
                else if (strcmp(key, "transport") == 0) {
                    if (parse_str(s, len, p, sval, sizeof(sval))) {
                        d->transport = (strcmp(sval, "rtu") == 0) ? VGP_MB_TRANSPORT_RTU : VGP_MB_TRANSPORT_TCP;
                    }
                } else if (strcmp(key, "unitId") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->unit_id = (uint8_t)v;
                } else if (strcmp(key, "pollIntervalMs") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->poll_interval_ms = (uint32_t)v;
                } else if (strcmp(key, "timeoutMs") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->timeout_ms = (uint32_t)v;
                } else if (strcmp(key, "host") == 0) {
                    parse_str(s, len, p, d->host, sizeof(d->host));
                } else if (strcmp(key, "port") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->port = (uint16_t)v;
                } else if (strcmp(key, "serialPort") == 0) {
                    parse_str(s, len, p, d->serial_port, sizeof(d->serial_port));
                } else if (strcmp(key, "baudRate") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->baud_rate = (uint32_t)v;
                } else if (strcmp(key, "parity") == 0) {
                    if (parse_str(s, len, p, sval, sizeof(sval))) {
                        if (strcmp(sval, "even") == 0) d->parity = VGP_MB_PARITY_EVEN;
                        else if (strcmp(sval, "odd") == 0) d->parity = VGP_MB_PARITY_ODD;
                        else d->parity = VGP_MB_PARITY_NONE;
                    }
                } else if (strcmp(key, "stopBits") == 0) {
                    double v; if (parse_num(s, len, p, &v)) d->stop_bits = (v >= 2.0) ? 2u : 1u;
                } else skip_val(s, len, p);
                match_char(s, len, p, ',');
            }
            match_char(s, len, p, '}');
            if (d->poll_interval_ms < 100u) d->poll_interval_ms = 100u;
            if (d->timeout_ms < 100u) d->timeout_ms = 100u;
        } else {
            report_warn("devices", "limit reached (16)");
            skip_val(s, len, p);
        }
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, ']');
}

static void parse_alarm_areas_array(const char *s, size_t len, size_t *p, vgp_project_t *proj)
{
    if (!match_char(s, len, p, '[')) return;
    while (*p < len && s[*p] != ']') {
        if (proj->num_alarm_areas < VGP_MAX_ALARM_AREAS && match_char(s, len, p, '{')) {
            vgp_alarm_area_t *a = &proj->alarm_areas[proj->num_alarm_areas++];
            memset(a, 0, sizeof(*a));
            a->enabled = true;
            a->update_interval_ms = 500u;

            char key[32], sval[96];
            while (*p < len && s[*p] != '}') {
                if (!parse_str(s, len, p, key, sizeof(key))) break;
                if (!match_char(s, len, p, ':')) break;
                if (strcmp(key, "id") == 0) parse_str(s, len, p, a->id, sizeof(a->id));
                else if (strcmp(key, "name") == 0) parse_str(s, len, p, a->name, sizeof(a->name));
                else if (strcmp(key, "variableId") == 0) parse_str(s, len, p, a->variable_id, sizeof(a->variable_id));
                else if (strcmp(key, "maskVariableId") == 0) parse_str(s, len, p, a->mask_variable_id, sizeof(a->mask_variable_id));
                else if (strcmp(key, "group") == 0) parse_str(s, len, p, a->group, sizeof(a->group));
                else if (strcmp(key, "updateIntervalMs") == 0) {
                    double v; if (parse_num(s, len, p, &v)) a->update_interval_ms = (uint32_t)v;
                } else if (strcmp(key, "enabled") == 0) {
                    parse_boolean(s, len, p, &a->enabled);
                } else if (strcmp(key, "rules") == 0) {
                    if (match_char(s, len, p, '[')) {
                        while (*p < len && s[*p] != ']') {
                            if (a->num_rules < VGP_MAX_WORD_RULES_PER_AREA && match_char(s, len, p, '{')) {
                                vgp_word_alarm_rule_t *r = &a->rules[a->num_rules++];
                                memset(r, 0, sizeof(*r));
                                r->enabled = true;
                                char rkey[32];
                                while (*p < len && s[*p] != '}') {
                                    if (!parse_str(s, len, p, rkey, sizeof(rkey))) break;
                                    if (!match_char(s, len, p, ':')) break;
                                    if (strcmp(rkey, "id") == 0) parse_str(s, len, p, r->id, sizeof(r->id));
                                    else if (strcmp(rkey, "bit") == 0) {
                                        double v; if (parse_num(s, len, p, &v)) r->bit = (uint8_t)v;
                                    } else if (strcmp(rkey, "message") == 0) parse_str(s, len, p, r->message, sizeof(r->message));
                                    else if (strcmp(rkey, "severity") == 0) {
                                        if (parse_str(s, len, p, sval, sizeof(sval))) r->severity = parse_severity_str(sval);
                                    } else if (strcmp(rkey, "enabled") == 0) parse_boolean(s, len, p, &r->enabled);
                                    else skip_val(s, len, p);
                                    match_char(s, len, p, ',');
                                }
                                match_char(s, len, p, '}');
                            } else skip_val(s, len, p);
                            match_char(s, len, p, ',');
                        }
                        match_char(s, len, p, ']');
                    } else skip_val(s, len, p);
                } else skip_val(s, len, p);
                match_char(s, len, p, ',');
            }
            match_char(s, len, p, '}');
            if (a->update_interval_ms < 100u) a->update_interval_ms = 100u;
        } else {
            report_warn("alarmAreas", "limit reached (32)");
            skip_val(s, len, p);
        }
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, ']');
}

static void parse_alarm_ticker(const char *s, size_t len, size_t *p, vgp_alarm_ticker_t *t)
{
    memset(t, 0, sizeof(*t));
    t->interval_ms = 3000u;
    if (!match_char(s, len, p, '{')) return;
    char key[32], sval[32];
    while (*p < len && s[*p] != '}') {
        if (!parse_str(s, len, p, key, sizeof(key))) break;
        if (!match_char(s, len, p, ':')) break;
        if (strcmp(key, "enabled") == 0) parse_boolean(s, len, p, &t->enabled);
        else if (strcmp(key, "position") == 0) {
            if (parse_str(s, len, p, sval, sizeof(sval))) t->position_bottom = (strcmp(sval, "bottom") == 0);
        } else if (strcmp(key, "intervalMs") == 0) {
            double v; if (parse_num(s, len, p, &v)) t->interval_ms = (uint32_t)v;
        } else skip_val(s, len, p);
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, '}');
    if (t->interval_ms < 1000u) t->interval_ms = 1000u;
}

static void parse_assets_array(const char *s, size_t len, size_t *p, vgp_project_t *proj)
{
    if (!match_char(s, len, p, '[')) return;
    while (*p < len && s[*p] != ']') {
        if (proj->num_assets < VGP_MAX_ASSETS && match_char(s, len, p, '{')) {
            vgp_asset_ref_t *a = &proj->assets[proj->num_assets];
            memset(a, 0, sizeof(*a));
            char key[32];
            while (*p < len && s[*p] != '}') {
                if (!parse_str(s, len, p, key, sizeof(key))) break;
                if (!match_char(s, len, p, ':')) break;
                if (strcmp(key, "id") == 0) {
                    parse_str(s, len, p, a->id, sizeof(a->id));
                } else if (strcmp(key, "data") == 0) {
                    (void)parse_data_uri_ref(s, len, p, &a->b64_ptr, &a->b64_len, &a->is_png);
                } else {
                    skip_val(s, len, p);
                }
                match_char(s, len, p, ',');
            }
            match_char(s, len, p, '}');
            if (a->id[0] && a->b64_ptr && a->b64_len > 0u) {
                proj->num_assets++;
            }
        } else {
            report_warn("assets", "limit reached (32)");
            skip_val(s, len, p);
        }
        match_char(s, len, p, ',');
    }
    match_char(s, len, p, ']');
}

static void vgp_link_project(vgp_project_t *proj)
{
    uint16_t mapped_count[VGP_MAX_DEVICES];
    memset(mapped_count, 0, sizeof(mapped_count));

    for (uint16_t i = 0; i < proj->num_variables; ++i) {
        vgp_variable_t *v = &proj->variables[i];
        if (!v->modbus.enabled || !v->modbus.device_id[0]) {
            v->modbus.enabled = false;
            v->modbus.device_idx = -1;
            v->quality = VGP_QUAL_INTERNAL;
            continue;
        }
        /* Read-only areas cannot be written */
        if (v->modbus.area == VGP_MB_AREA_DISCRETE_INPUT ||
            v->modbus.area == VGP_MB_AREA_INPUT_REG) {
            v->modbus.writable = false;
        }
        if (v->modbus.scale == 0.0) {
            v->modbus.scale = 1.0;
        }
        int16_t found = -1;
        for (uint16_t d = 0; d < proj->num_devices; ++d) {
            if (strcmp(proj->devices[d].id, v->modbus.device_id) == 0) {
                found = (int16_t)d;
                break;
            }
        }
        v->modbus.device_idx = found;
        if (found < 0 || !proj->devices[found].enabled) {
            v->quality = VGP_QUAL_BAD;
        } else {
            v->quality = VGP_QUAL_WAITING;
            mapped_count[found]++;
        }
    }

    for (uint16_t d = 0; d < proj->num_devices; ++d) {
        if (!proj->devices[d].enabled) {
            proj->devices[d].state = VGP_DEV_STATE_DISABLED;
        } else if (mapped_count[d] == 0u) {
            proj->devices[d].state = VGP_DEV_STATE_UNMAPPED;
        } else {
            proj->devices[d].state = VGP_DEV_STATE_CONNECTING;
        }
    }
}

bool vgp_parse_project_ex(const char *json, size_t len, vgp_project_t *proj, vgp_parse_report_t *report)
{
    if (report) memset(report, 0, sizeof(*report));
    g_cur_report = report;

    if (!json || len == 0 || !proj) {
        report_err("empty json input");
        g_cur_report = NULL;
        return false;
    }
    memset(proj, 0, sizeof(*proj));
    proj->alarm_ticker.interval_ms = 3000u;

    size_t p = 0;
    /* Skip UTF-8 BOM if present */
    if (len >= 3 && (uint8_t)json[0] == 0xEF && (uint8_t)json[1] == 0xBB && (uint8_t)json[2] == 0xBF) {
        p = 3;
    }
    /* Skip leading whitespace or non-brace text */
    while (p < len && json[p] != '{') p++;
    if (!match_char(json, len, &p, '{')) {
        report_err("missing root '{'");
        g_cur_report = NULL;
        return false;
    }

    char key[64];
    while (p < len && json[p] != '}') {
        if (!parse_str(json, len, &p, key, sizeof(key))) break;
        if (!match_char(json, len, &p, ':')) break;

        if (strcmp(key, "schemaVersion") == 0) {
            double v = 0.0;
            if (parse_num(json, len, &p, &v) && report) {
                report->schema_version = (uint32_t)v;
                report->has_schema_version = true;
            }
            if ((uint32_t)v != 1u) {
                char msg[64];
                snprintf(msg, sizeof(msg), "unsupported schemaVersion %u (expected 1)", (unsigned)(uint32_t)v);
                report_err(msg);
                g_cur_report = NULL;
                return false;
            }
        } else if (strcmp(key, "id") == 0) {
            parse_str(json, len, &p, proj->id, sizeof(proj->id));
        } else if (strcmp(key, "name") == 0) {
            parse_str(json, len, &p, proj->name, sizeof(proj->name));
        } else if (strcmp(key, "startScreenId") == 0) {
            parse_str(json, len, &p, proj->start_screen_id, sizeof(proj->start_screen_id));
        } else if (strcmp(key, "devices") == 0) {
            parse_devices_array(json, len, &p, proj);
        } else if (strcmp(key, "screens") == 0) {
            parse_screens_array(json, len, &p, proj);
        } else if (strcmp(key, "variables") == 0) {
            parse_variables_array(json, len, &p, proj);
        } else if (strcmp(key, "alarmAreas") == 0) {
            parse_alarm_areas_array(json, len, &p, proj);
        } else if (strcmp(key, "alarmTicker") == 0) {
            parse_alarm_ticker(json, len, &p, &proj->alarm_ticker);
        } else if (strcmp(key, "assets") == 0) {
            parse_assets_array(json, len, &p, proj);
        } else {
            if (strcmp(key, "description") != 0 && strcmp(key, "autoSave") != 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "unknown root key '%s'", key);
                report_warn("project", msg);
            }
            skip_val(json, len, &p);
        }
        match_char(json, len, &p, ',');
    }
    match_char(json, len, &p, '}');

    if (report && !report->has_schema_version) {
        report_warn("project", "missing schemaVersion (assuming v1)");
    }

    if (proj->num_screens > 0) {
        vgp_link_project(proj);
        g_cur_report = NULL;
        return true;
    }
    report_err("no valid screens in project.json");
    g_cur_report = NULL;
    return false;
}

bool vgp_parse_project(const char *json, size_t len, vgp_project_t *proj)
{
    return vgp_parse_project_ex(json, len, proj, NULL);
}
