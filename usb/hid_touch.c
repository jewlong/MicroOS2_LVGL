/* SPDX-License-Identifier: Apache-2.0 */
#include "hid_touch.h"
#include <string.h>

#define HID_MAX_GLOBAL_STACK 4u
#define HID_MAX_LOCAL_USAGES 16u
#define HID_MAX_REPORT_IDS   16u
#define HID_MAX_COLL_DEPTH   8u

#define UPAGE_GENERIC_DESKTOP 0x0001u
#define UPAGE_BUTTON          0x0009u
#define UPAGE_DIGITIZERS      0x000Du

#define USAGE_GD_POINTER      0x0001u
#define USAGE_GD_MOUSE        0x0002u
#define USAGE_GD_X            0x0030u
#define USAGE_GD_Y            0x0031u

#define USAGE_DIG_DIGITIZER   0x0001u
#define USAGE_DIG_PEN         0x0002u
#define USAGE_DIG_TOUCHSCREEN 0x0004u
#define USAGE_DIG_FINGER      0x0022u
#define USAGE_DIG_TIP_SWITCH  0x0042u

typedef struct {
    uint16_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint32_t logical_max_u;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
    bool has_report_id;
} hid_global_t;

typedef struct {
    uint32_t usages[HID_MAX_LOCAL_USAGES];
    uint8_t usage_count;
    uint32_t usage_min;
    uint32_t usage_max;
    bool has_range;
} hid_local_t;

typedef struct {
    uint8_t id;
    uint16_t bit_off;
} hid_rid_offset_t;

typedef struct {
    bool active;
    bool has_report_id;
    bool is_digitizer;
    uint8_t report_id;
    uint8_t cur_slot;
    uint8_t num_slots;
    hid_touch_slot_t slots[HID_TOUCH_MAX_SLOTS];
} hid_touch_cand_t;

static int32_t sign_extend(uint32_t v, uint8_t bytes)
{
    if (bytes == 1u) return (int32_t)(int8_t)(v & 0xFFu);
    if (bytes == 2u) return (int32_t)(int16_t)(v & 0xFFFFu);
    return (int32_t)v;
}

static uint16_t *get_rid_bit_offset(hid_rid_offset_t *table, uint8_t *count, uint8_t rid)
{
    for (uint8_t i = 0; i < *count; ++i) {
        if (table[i].id == rid) return &table[i].bit_off;
    }
    if (*count >= HID_MAX_REPORT_IDS) return NULL;
    table[*count].id = rid;
    table[*count].bit_off = 0u;
    (*count)++;
    return &table[*count - 1u].bit_off;
}

static uint32_t resolve_usage(const hid_local_t *loc, uint16_t global_page, uint32_t idx)
{
    if (loc->has_range) {
        uint32_t u = loc->usage_min + idx;
        if (u > loc->usage_max) u = loc->usage_max;
        if ((u & 0xFFFF0000u) == 0u) u |= ((uint32_t)global_page << 16);
        return u;
    }
    if (loc->usage_count == 0u) return 0u;
    uint32_t u = (idx < loc->usage_count) ? loc->usages[idx] : loc->usages[loc->usage_count - 1u];
    if ((u & 0xFFFF0000u) == 0u) u |= ((uint32_t)global_page << 16);
    return u;
}

static bool slot_complete(const hid_touch_slot_t *s)
{
    return s->has_tip && s->has_x && s->has_y && (s->x_max > s->x_min) && (s->y_max > s->y_min);
}

static void record_touch_field(hid_touch_cand_t *cand, bool is_dig, bool has_rid, uint8_t rid,
                               uint32_t full_usage, uint16_t bit_off, uint8_t bit_size,
                               int32_t lmin, int32_t lmax, uint32_t lmax_u)
{
    uint16_t page = (uint16_t)(full_usage >> 16);
    uint16_t uid = (uint16_t)(full_usage & 0xFFFFu);

    bool is_tip = false;
    bool is_x = false;
    bool is_y = false;

    if (is_dig) {
        if (page == UPAGE_DIGITIZERS && uid == USAGE_DIG_TIP_SWITCH) is_tip = true;
        else if (page == UPAGE_GENERIC_DESKTOP && uid == USAGE_GD_X) is_x = true;
        else if (page == UPAGE_GENERIC_DESKTOP && uid == USAGE_GD_Y) is_y = true;
    } else {
        if (page == UPAGE_BUTTON && uid == 1u) is_tip = true;
        else if (page == UPAGE_GENERIC_DESKTOP && uid == USAGE_GD_X) is_x = true;
        else if (page == UPAGE_GENERIC_DESKTOP && uid == USAGE_GD_Y) is_y = true;
    }

    if (!is_tip && !is_x && !is_y) return;

    if (!cand->active) {
        memset(cand, 0, sizeof(*cand));
        cand->active = true;
        cand->is_digitizer = is_dig;
        cand->has_report_id = has_rid;
        cand->report_id = rid;
        cand->cur_slot = 0u;
    } else if (cand->has_report_id != has_rid || cand->report_id != rid) {
        /* Keep the first valid report ID; if slot 0 is still incomplete, switch to this report ID. */
        if (slot_complete(&cand->slots[0])) return;
        memset(cand, 0, sizeof(*cand));
        cand->active = true;
        cand->is_digitizer = is_dig;
        cand->has_report_id = has_rid;
        cand->report_id = rid;
        cand->cur_slot = 0u;
    }

    if (cand->cur_slot >= HID_TOUCH_MAX_SLOTS) return;
    hid_touch_slot_t *s = &cand->slots[cand->cur_slot];

    /* If this field is already populated in the current slot, advance to next contact slot. */
    if ((is_tip && s->has_tip) || (is_x && s->has_x) || (is_y && s->has_y)) {
        if (slot_complete(s)) {
            if (cand->cur_slot + 1u >= HID_TOUCH_MAX_SLOTS) return;
            cand->cur_slot++;
            s = &cand->slots[cand->cur_slot];
        } else {
            return;
        }
    }

    /* Handle common touch-controller quirk: Logical Maximum encoded as 0x7FFF/0xFFFF without sign byte. */
    int32_t eff_max = lmax;
    if (lmin >= 0 && lmax < 0 && lmax_u <= 0x7FFFFFFFu) {
        eff_max = (int32_t)lmax_u;
    }

    if (is_tip) {
        if (bit_size == 0u || bit_size > 8u) return;
        s->has_tip = true;
        s->tip_bit = bit_off;
        s->tip_size = bit_size;
    } else if (is_x) {
        if (bit_size < 4u || bit_size > 32u || eff_max <= lmin) return;
        s->has_x = true;
        s->x_bit = bit_off;
        s->x_size = bit_size;
        s->x_min = lmin;
        s->x_max = eff_max;
        s->x_signed = (lmin < 0);
    } else if (is_y) {
        if (bit_size < 4u || bit_size > 32u || eff_max <= lmin) return;
        s->has_y = true;
        s->y_bit = bit_off;
        s->y_size = bit_size;
        s->y_min = lmin;
        s->y_max = eff_max;
        s->y_signed = (lmin < 0);
    }

    if (slot_complete(s) && cand->num_slots <= cand->cur_slot) {
        cand->num_slots = (uint8_t)(cand->cur_slot + 1u);
    }
}

static bool finalize_candidate(const hid_touch_cand_t *cand, hid_touch_layout_t *out)
{
    if (!cand->active || cand->num_slots == 0u) return false;
    memset(out, 0, sizeof(*out));
    out->has_report_id = cand->has_report_id;
    out->report_id = cand->report_id;
    out->is_digitizer = cand->is_digitizer;

    uint16_t max_bit_slot0 = 0u;
    uint8_t valid_slots = 0u;
    for (uint8_t i = 0; i < cand->num_slots && i < HID_TOUCH_MAX_SLOTS; ++i) {
        if (!slot_complete(&cand->slots[i])) break;
        out->slots[valid_slots] = cand->slots[i];
        if (valid_slots == 0u) {
            uint16_t t_end = (uint16_t)(cand->slots[i].tip_bit + cand->slots[i].tip_size);
            uint16_t x_end = (uint16_t)(cand->slots[i].x_bit + cand->slots[i].x_size);
            uint16_t y_end = (uint16_t)(cand->slots[i].y_bit + cand->slots[i].y_size);
            max_bit_slot0 = t_end;
            if (x_end > max_bit_slot0) max_bit_slot0 = x_end;
            if (y_end > max_bit_slot0) max_bit_slot0 = y_end;
        }
        valid_slots++;
    }
    if (valid_slots == 0u) return false;
    out->num_slots = valid_slots;
    out->min_report_bytes = (uint16_t)(((max_bit_slot0 + 7u) / 8u) + (out->has_report_id ? 1u : 0u));
    out->valid = true;
    return true;
}

bool hid_touch_parse_report_desc(const uint8_t *desc, size_t len, hid_touch_layout_t *out)
{
    if (!desc || !out || len < 4u) return false;
    memset(out, 0, sizeof(*out));

    hid_global_t global;
    hid_global_t gstack[HID_MAX_GLOBAL_STACK];
    uint8_t gsp = 0u;
    hid_local_t local;
    hid_rid_offset_t rid_offsets[HID_MAX_REPORT_IDS];
    uint8_t rid_count = 0u;

    /* App collection classification: 0=other, 1=digitizer touchscreen/pen, 2=generic pointer/mouse */
    uint8_t app_kind_stack[HID_MAX_COLL_DEPTH];
    uint8_t coll_depth = 0u;
    uint8_t cur_app_kind = 0u;

    hid_touch_cand_t dig_cand;
    hid_touch_cand_t ptr_cand;
    memset(&global, 0, sizeof(global));
    memset(&local, 0, sizeof(local));
    memset(&dig_cand, 0, sizeof(dig_cand));
    memset(&ptr_cand, 0, sizeof(ptr_cand));

    size_t pos = 0u;
    while (pos < len) {
        uint8_t prefix = desc[pos++];
        if (prefix == 0xFEu) {
            /* Long item: bDataSize, bLongItemTag, data[bDataSize] */
            if (pos + 2u > len) break;
            uint8_t dsz = desc[pos];
            pos += 2u;
            if (pos + dsz > len) break;
            pos += dsz;
            continue;
        }

        uint8_t bSizeCode = prefix & 3u;
        uint8_t bSize = (bSizeCode == 3u) ? 4u : bSizeCode;
        uint8_t bType = (prefix >> 2) & 3u;
        uint8_t bTag = (prefix >> 4) & 0x0Fu;
        if (pos + bSize > len) break;

        uint32_t uval = 0u;
        for (uint8_t i = 0; i < bSize; ++i) {
            uval |= ((uint32_t)desc[pos + i]) << (i * 8u);
        }
        pos += bSize;

        if (bType == 1u) {
            /* Global item */
            switch (bTag) {
                case 0x0u: global.usage_page = (uint16_t)(uval & 0xFFFFu); break;
                case 0x1u: global.logical_min = sign_extend(uval, bSize); break;
                case 0x2u:
                    global.logical_max = sign_extend(uval, bSize);
                    global.logical_max_u = uval;
                    break;
                case 0x7u: global.report_size = uval; break;
                case 0x8u:
                    global.report_id = (uint8_t)(uval & 0xFFu);
                    global.has_report_id = (global.report_id != 0u);
                    break;
                case 0x9u: global.report_count = uval; break;
                case 0xAu: /* Push */
                    if (gsp < HID_MAX_GLOBAL_STACK) gstack[gsp++] = global;
                    break;
                case 0xBu: /* Pop */
                    if (gsp > 0u) global = gstack[--gsp];
                    break;
                default: break;
            }
        } else if (bType == 2u) {
            /* Local item */
            switch (bTag) {
                case 0x0u: /* Usage */
                    if (local.usage_count < HID_MAX_LOCAL_USAGES) {
                        uint32_t fu = (bSize == 4u) ? uval : (((uint32_t)global.usage_page << 16) | (uval & 0xFFFFu));
                        local.usages[local.usage_count++] = fu;
                    }
                    break;
                case 0x1u: /* Usage Minimum */
                    local.usage_min = (bSize == 4u) ? uval : (((uint32_t)global.usage_page << 16) | (uval & 0xFFFFu));
                    local.has_range = true;
                    break;
                case 0x2u: /* Usage Maximum */
                    local.usage_max = (bSize == 4u) ? uval : (((uint32_t)global.usage_page << 16) | (uval & 0xFFFFu));
                    local.has_range = true;
                    break;
                default: break;
            }
        } else if (bType == 0u) {
            /* Main item */
            if (bTag == 0xAu) {
                /* Collection */
                uint8_t ctype = (uint8_t)(uval & 0xFFu);
                if (coll_depth < HID_MAX_COLL_DEPTH) {
                    app_kind_stack[coll_depth] = cur_app_kind;
                }
                if (ctype == 0x01u) { /* Application collection */
                    uint32_t app_u = resolve_usage(&local, global.usage_page, 0u);
                    uint16_t ap = (uint16_t)(app_u >> 16);
                    uint16_t au = (uint16_t)(app_u & 0xFFFFu);
                    if (ap == UPAGE_DIGITIZERS &&
                        (au == USAGE_DIG_TOUCHSCREEN || au == USAGE_DIG_PEN || au == USAGE_DIG_DIGITIZER)) {
                        cur_app_kind = 1u;
                    } else if (ap == UPAGE_GENERIC_DESKTOP &&
                               (au == USAGE_GD_POINTER || au == USAGE_GD_MOUSE)) {
                        cur_app_kind = 2u;
                    } else {
                        cur_app_kind = 0u;
                    }
                }
                coll_depth++;
            } else if (bTag == 0xCu) {
                /* End Collection */
                if (coll_depth > 0u) {
                    coll_depth--;
                    if (coll_depth < HID_MAX_COLL_DEPTH) {
                        cur_app_kind = app_kind_stack[coll_depth];
                    }
                }
            } else if (bTag == 0x8u) {
                /* Input item */
                uint8_t flags = (uint8_t)(uval & 0xFFu);
                bool is_const = (flags & 0x01u) != 0u;
                bool is_var   = (flags & 0x02u) != 0u;
                bool is_rel   = (flags & 0x04u) != 0u;
                uint16_t *bit_off_ptr = get_rid_bit_offset(rid_offsets, &rid_count,
                                                           global.has_report_id ? global.report_id : 0u);
                if (bit_off_ptr && global.report_size <= 64u && global.report_count <= 256u) {
                    uint32_t cur_bit = *bit_off_ptr;
                    if (!is_const && is_var && !is_rel && cur_app_kind != 0u) {
                        for (uint32_t i = 0u; i < global.report_count; ++i) {
                            uint32_t fu = resolve_usage(&local, global.usage_page, i);
                            uint32_t fbit = cur_bit + i * global.report_size;
                            if (fbit < 4096u) {
                                if (cur_app_kind == 1u) {
                                    record_touch_field(&dig_cand, true, global.has_report_id, global.report_id,
                                                       fu, (uint16_t)fbit, (uint8_t)global.report_size,
                                                       global.logical_min, global.logical_max, global.logical_max_u);
                                } else if (cur_app_kind == 2u) {
                                    record_touch_field(&ptr_cand, false, global.has_report_id, global.report_id,
                                                       fu, (uint16_t)fbit, (uint8_t)global.report_size,
                                                       global.logical_min, global.logical_max, global.logical_max_u);
                                }
                            }
                        }
                    }
                    uint32_t total_bits = global.report_size * global.report_count;
                    if (cur_bit + total_bits < 65535u) {
                        *bit_off_ptr = (uint16_t)(cur_bit + total_bits);
                    }
                }
            }
            /* Clear local state after every Main item */
            memset(&local, 0, sizeof(local));
        }
    }

    if (finalize_candidate(&dig_cand, out)) return true;
    if (finalize_candidate(&ptr_cand, out)) return true;
    return false;
}

static bool extract_bits(const uint8_t *buf, size_t len, uint16_t bit_off, uint8_t bit_size,
                         bool is_signed, int32_t *out_val)
{
    if (!buf || bit_size == 0u || bit_size > 32u) return false;
    uint32_t end_bit = (uint32_t)bit_off + (uint32_t)bit_size;
    if ((end_bit + 7u) / 8u > len) return false;

    uint32_t val = 0u;
    for (uint8_t i = 0u; i < bit_size; ++i) {
        uint32_t b = (uint32_t)bit_off + i;
        uint8_t bit = (uint8_t)((buf[b >> 3] >> (b & 7u)) & 1u);
        val |= ((uint32_t)bit << i);
    }
    if (is_signed && bit_size < 32u) {
        uint32_t sign_bit = 1u << (bit_size - 1u);
        if (val & sign_bit) {
            val |= ~(sign_bit - 1u);
        }
    }
    *out_val = (int32_t)val;
    return true;
}

static uint16_t scale_to_u16(int32_t raw, int32_t vmin, int32_t vmax)
{
    if (vmax <= vmin) return 0u;
    if (raw <= vmin) return 0u;
    if (raw >= vmax) return 65535u;
    uint64_t num = (uint64_t)((int64_t)raw - (int64_t)vmin) * 65535u;
    uint64_t den = (uint64_t)((int64_t)vmax - (int64_t)vmin);
    return (uint16_t)((num + (den / 2u)) / den);
}

bool hid_touch_decode_report(const hid_touch_layout_t *layout, const uint8_t *report, size_t len,
                             hid_touch_sample_t *out)
{
    if (!layout || !layout->valid || !report || !out) return false;
    if (len < layout->min_report_bytes) return false;

    const uint8_t *payload = report;
    size_t plen = len;
    if (layout->has_report_id) {
        if (report[0] != layout->report_id) return false;
        payload = report + 1u;
        plen = len - 1u;
    }

    int32_t fallback_rx = 0, fallback_ry = 0;
    uint16_t fallback_nx = 0u, fallback_ny = 0u;
    bool have_fallback = false;

    for (uint8_t i = 0u; i < layout->num_slots && i < HID_TOUCH_MAX_SLOTS; ++i) {
        const hid_touch_slot_t *s = &layout->slots[i];
        int32_t tip = 0, rx = 0, ry = 0;
        if (!extract_bits(payload, plen, s->tip_bit, s->tip_size, false, &tip)) break;
        if (!extract_bits(payload, plen, s->x_bit, s->x_size, s->x_signed, &rx)) break;
        if (!extract_bits(payload, plen, s->y_bit, s->y_size, s->y_signed, &ry)) break;

        uint16_t nx = scale_to_u16(rx, s->x_min, s->x_max);
        uint16_t ny = scale_to_u16(ry, s->y_min, s->y_max);
        if (!have_fallback) {
            fallback_rx = rx;
            fallback_ry = ry;
            fallback_nx = nx;
            fallback_ny = ny;
            have_fallback = true;
        }
        if (tip != 0) {
            out->pressed = true;
            out->x_norm = nx;
            out->y_norm = ny;
            out->raw_x = rx;
            out->raw_y = ry;
            return true;
        }
    }

    if (!have_fallback) return false;
    out->pressed = false;
    out->x_norm = fallback_nx;
    out->y_norm = fallback_ny;
    out->raw_x = fallback_rx;
    out->raw_y = fallback_ry;
    return true;
}
