/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_alarm.h"
#include "vgp_platform.h"
#include "sys_clock.h"
#include <string.h>

static vgp_project_t *g_proj = NULL;
static VGP_BSS_ATTR vgp_alarm_event_t g_events[VGP_MAX_ALARM_EVENTS];
static uint32_t g_generation = 0u;

static const vgp_variable_t *find_var(const char *id)
{
    if (!g_proj || !id || !id[0]) return NULL;
    for (uint16_t i = 0; i < g_proj->num_variables; ++i) {
        if (strcmp(g_proj->variables[i].id, id) == 0) {
            return &g_proj->variables[i];
        }
    }
    return NULL;
}

static bool is_var_available(const vgp_variable_t *v)
{
    if (!v) return false;
    return (v->quality == VGP_QUAL_INTERNAL || v->quality == VGP_QUAL_GOOD);
}

static bool is_valid_word(double val, uint16_t *out_word)
{
    if (val < 0.0 || val > 65535.0) return false;
    uint32_t u = (uint32_t)(val + 0.5);
    double diff = val - (double)u;
    if (diff < -1e-6 || diff > 1e-6) return false;
    if (u > 65535u) return false;
    *out_word = (uint16_t)u;
    return true;
}

static int32_t find_active_event(bool is_global, uint16_t source_idx, uint16_t rule_idx)
{
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        const vgp_alarm_event_t *ev = &g_events[i];
        if (ev->used && ev->cleared_epoch == 0u &&
            ev->is_global == is_global &&
            ev->source_idx == source_idx &&
            ev->rule_idx == rule_idx) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t alloc_event_slot(void)
{
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        if (!g_events[i].used) return (int32_t)i;
    }
    /* Evict oldest cleared & acknowledged event first. */
    int32_t best = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        if (g_events[i].cleared_epoch != 0u && g_events[i].acknowledged) {
            if (g_events[i].raised_epoch < oldest) {
                oldest = g_events[i].raised_epoch;
                best = (int32_t)i;
            }
        }
    }
    if (best >= 0) return best;

    /* Next, evict oldest cleared event. */
    oldest = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        if (g_events[i].cleared_epoch != 0u) {
            if (g_events[i].raised_epoch < oldest) {
                oldest = g_events[i].raised_epoch;
                best = (int32_t)i;
            }
        }
    }
    return best; /* Never evict active alarms */
}

static void raise_event(bool is_global, uint16_t source_idx, uint16_t rule_idx,
                        const char *group, const char *message, vgp_severity_t severity,
                        uint32_t now_epoch)
{
    int32_t slot = alloc_event_slot();
    if (slot < 0) return;
    vgp_alarm_event_t *ev = &g_events[slot];
    memset(ev, 0, sizeof(*ev));
    ev->used = true;
    ev->is_global = is_global;
    ev->source_idx = source_idx;
    ev->rule_idx = rule_idx;
    if (group) {
        strncpy(ev->group, group, sizeof(ev->group) - 1u);
    }
    if (message) {
        strncpy(ev->message, message, sizeof(ev->message) - 1u);
    }
    ev->severity = severity;
    ev->raised_epoch = now_epoch;
    ev->cleared_epoch = 0u;
    ev->clear_reason = VGP_CLR_NONE;
    ev->acknowledged = false;
    g_generation++;
}

static void clear_event(int32_t slot, uint32_t now_epoch, vgp_clear_reason_t reason)
{
    if (slot < 0 || slot >= VGP_MAX_ALARM_EVENTS) return;
    vgp_alarm_event_t *ev = &g_events[slot];
    if (!ev->used || ev->cleared_epoch != 0u) return;
    ev->cleared_epoch = now_epoch ? now_epoch : 1u;
    ev->clear_reason = reason;
    g_generation++;
}

static bool eval_local_op(const vgp_local_alarm_rule_t *r, const vgp_variable_t *v)
{
    if (v->type == VGP_VAR_BOOL) {
        if (r->op == VGP_OP_TRUE) return v->val_bool;
        if (r->op == VGP_OP_FALSE) return !v->val_bool;
        double n = v->val_bool ? 1.0 : 0.0;
        switch (r->op) {
            case VGP_OP_GT:  return n > r->threshold;
            case VGP_OP_GTE: return n >= r->threshold;
            case VGP_OP_LT:  return n < r->threshold;
            case VGP_OP_LTE: return n <= r->threshold;
            case VGP_OP_EQ:  return n == r->threshold;
            case VGP_OP_NE:  return n != r->threshold;
            default:         return false;
        }
    } else if (v->type == VGP_VAR_NUM) {
        double n = v->val_num;
        switch (r->op) {
            case VGP_OP_GT:    return n > r->threshold;
            case VGP_OP_GTE:   return n >= r->threshold;
            case VGP_OP_LT:    return n < r->threshold;
            case VGP_OP_LTE:   return n <= r->threshold;
            case VGP_OP_EQ:    return (n - r->threshold) > -1e-6 && (n - r->threshold) < 1e-6;
            case VGP_OP_NE:    return (n - r->threshold) <= -1e-6 || (n - r->threshold) >= 1e-6;
            case VGP_OP_TRUE:  return n != 0.0;
            case VGP_OP_FALSE: return n == 0.0;
            default:           return false;
        }
    }
    return false;
}

void vgp_alarm_init(vgp_project_t *proj)
{
    g_proj = proj;
    memset(g_events, 0, sizeof(g_events));
    g_generation = 1u;
}

void vgp_alarm_evaluate(uint32_t now_ms)
{
    if (!g_proj) return;
    uint32_t now_epoch = sys_clock_now_utc();

    /* 1. Global Word Alarm Areas */
    for (uint16_t a_idx = 0; a_idx < g_proj->num_alarm_areas; ++a_idx) {
        vgp_alarm_area_t *area = &g_proj->alarm_areas[a_idx];
        if (area->last_check_ms != 0u &&
            (uint32_t)(now_ms - area->last_check_ms) < area->update_interval_ms) {
            continue;
        }
        area->last_check_ms = now_ms ? now_ms : 1u;

        const vgp_variable_t *word_var = find_var(area->variable_id);
        const vgp_variable_t *mask_var = area->mask_variable_id[0] ? find_var(area->mask_variable_id) : NULL;

        /* Pause evaluation & retain existing alarms when bound Modbus tags are unavailable */
        if (word_var && !is_var_available(word_var)) continue;
        if (mask_var && !is_var_available(mask_var)) continue;

        uint16_t word = 0u, mask = 0u;
        bool valid = (word_var && word_var->type == VGP_VAR_NUM && is_valid_word(word_var->val_num, &word));
        if (valid && mask_var) {
            valid = (mask_var->type == VGP_VAR_NUM && is_valid_word(mask_var->val_num, &mask));
        }

        for (uint8_t r_idx = 0; r_idx < area->num_rules; ++r_idx) {
            const vgp_word_alarm_rule_t *rule = &area->rules[r_idx];
            if (rule->bit > 15u) continue;
            bool masked = valid && ((mask & (1u << rule->bit)) != 0u);
            bool triggered = area->enabled && rule->enabled && valid && !masked &&
                             ((word & (1u << rule->bit)) != 0u);
            int32_t existing = find_active_event(true, a_idx, r_idx);
            if (triggered && existing < 0) {
                raise_event(true, a_idx, r_idx, area->group, rule->message, rule->severity, now_epoch);
            } else if (!triggered && existing >= 0) {
                vgp_clear_reason_t reason = (!area->enabled || !rule->enabled) ? VGP_CLR_DISABLED :
                                            masked ? VGP_CLR_MASKED : VGP_CLR_NORMAL;
                clear_event(existing, now_epoch, reason);
            }
        }
    }

    /* 2. Element-level Local Alarm Rules */
    for (uint16_t e_idx = 0; e_idx < g_proj->total_elements; ++e_idx) {
        const vgp_element_t *el = &g_proj->elements[e_idx];
        if (el->kind != VGP_KIND_ALARM || el->num_alarm_rules == 0u) continue;

        for (uint16_t r_off = 0; r_off < el->num_alarm_rules; ++r_off) {
            uint16_t r_pool = el->alarm_rule_start + r_off;
            if (r_pool >= g_proj->total_local_alarm_rules) break;
            const vgp_local_alarm_rule_t *rule = &g_proj->local_alarm_rules[r_pool];
            const vgp_variable_t *v = find_var(rule->variable_id);
            if (v && !is_var_available(v)) continue;

            bool triggered = rule->enabled && v && eval_local_op(rule, v);
            int32_t existing = find_active_event(false, e_idx, r_off);
            if (triggered && existing < 0) {
                raise_event(false, e_idx, r_off, el->alarm_group, rule->message, rule->severity, now_epoch);
            } else if (!triggered && existing >= 0) {
                vgp_clear_reason_t reason = !rule->enabled ? VGP_CLR_DISABLED : VGP_CLR_NORMAL;
                clear_event(existing, now_epoch, reason);
            }
        }
    }
}

uint32_t vgp_alarm_generation(void)
{
    return g_generation;
}

static bool event_matches_element(const vgp_alarm_event_t *ev, const vgp_element_t *el, uint16_t element_idx)
{
    if (!ev->used) return false;
    if (el->alarm_source == VGP_ALARM_SRC_GLOBAL) {
        if (!ev->is_global) return false;
    } else if (el->alarm_source == VGP_ALARM_SRC_LOCAL) {
        if (ev->is_global || ev->source_idx != element_idx) return false;
    } else {
        /* VGP_ALARM_SRC_ALL: global + this element's local rules */
        if (!ev->is_global && ev->source_idx != element_idx) return false;
    }
    if (el->alarm_group[0] && strcmp(ev->group, el->alarm_group) != 0) {
        return false;
    }
    return true;
}

static bool event_comes_before(const vgp_alarm_event_t *a, const vgp_alarm_event_t *b)
{
    bool a_active = (a->cleared_epoch == 0u);
    bool b_active = (b->cleared_epoch == 0u);
    if (a_active != b_active) return a_active;
    return a->raised_epoch > b->raised_epoch;
}

uint16_t vgp_alarm_query(const vgp_element_t *el, uint16_t element_idx, uint16_t *out_indices, uint16_t max_out)
{
    if (!el || !out_indices || max_out == 0u) return 0u;
    uint16_t tmp[VGP_MAX_ALARM_EVENTS];
    uint16_t n = 0u;
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        if (event_matches_element(&g_events[i], el, element_idx)) {
            tmp[n++] = i;
        }
    }
    /* Insertion sort: active first, newest first */
    for (uint16_t i = 1; i < n; ++i) {
        uint16_t key = tmp[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && event_comes_before(&g_events[key], &g_events[tmp[j]])) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    if (n > max_out) n = max_out;
    for (uint16_t i = 0; i < n; ++i) out_indices[i] = tmp[i];
    return n;
}

uint16_t vgp_alarm_get_active_list(uint16_t *out_indices, uint16_t max_out)
{
    if (!out_indices || max_out == 0u) return 0u;
    uint16_t n = 0u;
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS && n < max_out; ++i) {
        if (g_events[i].used && g_events[i].cleared_epoch == 0u) {
            out_indices[n++] = i;
        }
    }
    return n;
}

const vgp_alarm_event_t *vgp_alarm_get_event(uint16_t idx)
{
    if (idx >= VGP_MAX_ALARM_EVENTS || !g_events[idx].used) return NULL;
    return &g_events[idx];
}

void vgp_alarm_ack_for_element(const vgp_element_t *el, uint16_t element_idx)
{
    if (!el) return;
    bool changed = false;
    for (uint16_t i = 0; i < VGP_MAX_ALARM_EVENTS; ++i) {
        if (event_matches_element(&g_events[i], el, element_idx) && !g_events[i].acknowledged) {
            g_events[i].acknowledged = true;
            changed = true;
        }
    }
    if (changed) {
        g_generation++;
    }
}
