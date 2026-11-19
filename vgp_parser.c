/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_parser.h"
#include <string.h>

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

static void parse_element_obj(const char *s, size_t len, size_t *p, vgp_element_t *e)
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
            if (parse_str(s, len, p, sval, sizeof(sval))) e->kind = parse_kind_str(sval);
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
            if (parse_str(s, len, p, sval, sizeof(sval))) e->border_color = parse_color_hex(sval, e->border_color);
        } else if (strcmp(key, "fontSize") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->font_size = (uint16_t)v;
        } else if (strcmp(key, "borderWidth") == 0) {
            double v; if (parse_num(s, len, p, &v)) e->border_width = (uint16_t)v;
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
        } else {
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
                    while (*p < len && s[*p] != ']' && proj->total_elements < VGP_MAX_TOTAL_ELEMENTS) {
                        vgp_element_t *el = &proj->elements[proj->total_elements++];
                        parse_element_obj(s, len, p, el);
                        sc->num_elements++;
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

bool vgp_parse_project(const char *json, size_t len, vgp_project_t *proj)
{
    if (!json || len == 0 || !proj) return false;
    memset(proj, 0, sizeof(*proj));

    size_t p = 0;
    /* Skip UTF-8 BOM if present */
    if (len >= 3 && (uint8_t)json[0] == 0xEF && (uint8_t)json[1] == 0xBB && (uint8_t)json[2] == 0xBF) {
        p = 3;
    }
    /* Skip leading whitespace or non-brace text */
    while (p < len && json[p] != '{') p++;
    if (!match_char(json, len, &p, '{')) return false;

    char key[64];
    while (p < len && json[p] != '}') {
        if (!parse_str(json, len, &p, key, sizeof(key))) break;
        if (!match_char(json, len, &p, ':')) break;

        if (strcmp(key, "id") == 0) {
            parse_str(json, len, &p, proj->id, sizeof(proj->id));
        } else if (strcmp(key, "name") == 0) {
            parse_str(json, len, &p, proj->name, sizeof(proj->name));
        } else if (strcmp(key, "startScreenId") == 0) {
            parse_str(json, len, &p, proj->start_screen_id, sizeof(proj->start_screen_id));
        } else if (strcmp(key, "screens") == 0) {
            parse_screens_array(json, len, &p, proj);
        } else if (strcmp(key, "variables") == 0) {
            parse_variables_array(json, len, &p, proj);
        } else {
            skip_val(json, len, &p);
        }
        match_char(json, len, &p, ',');
    }
    match_char(json, len, &p, '}');

    return (proj->num_screens > 0);
}

