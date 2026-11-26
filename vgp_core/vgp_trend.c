/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_trend.h"
#include "vgp_platform.h"
#include <string.h>

typedef struct {
    bool used;
    uint16_t element_idx;
    int32_t var_idx;
    uint32_t sample_interval_ms;
    uint16_t capacity;
    uint16_t count;
    uint16_t head;
    uint32_t last_sample_ms;
    int32_t samples[VGP_TREND_MAX_POINTS]; /* fixed-point x10 */
} vgp_trend_buf_t;

static vgp_project_t *g_proj = NULL;
static VGP_BSS_ATTR vgp_trend_buf_t g_trends[VGP_MAX_TRENDS];
static uint16_t g_num_trends = 0u;

static int32_t find_var_idx(const vgp_project_t *proj, const char *id)
{
    if (!proj || !id || !id[0]) return -1;
    for (uint16_t i = 0; i < proj->num_variables; ++i) {
        if (strcmp(proj->variables[i].id, id) == 0) return (int32_t)i;
    }
    return -1;
}

void vgp_trend_init(vgp_project_t *proj)
{
    g_proj = proj;
    memset(g_trends, 0, sizeof(g_trends));
    g_num_trends = 0u;
    if (!proj) return;

    for (uint16_t i = 0; i < proj->total_elements && g_num_trends < VGP_MAX_TRENDS; ++i) {
        const vgp_element_t *el = &proj->elements[i];
        if (el->kind != VGP_KIND_TREND) continue;
        vgp_trend_buf_t *tb = &g_trends[g_num_trends++];
        tb->used = true;
        tb->element_idx = i;
        tb->var_idx = find_var_idx(proj, el->variable_id);
        tb->sample_interval_ms = el->sample_interval_ms >= 100u ? el->sample_interval_ms : 1000u;
        uint16_t cap = el->history_points ? el->history_points : 60u;
        if (cap > VGP_TREND_MAX_POINTS) cap = VGP_TREND_MAX_POINTS;
        if (cap < 2u) cap = 2u;
        tb->capacity = cap;
        tb->count = 0u;
        tb->head = 0u;
        tb->last_sample_ms = 0u;
    }
}

void vgp_trend_tick(uint32_t now_ms)
{
    if (!g_proj) return;
    for (uint16_t t = 0; t < g_num_trends; ++t) {
        vgp_trend_buf_t *tb = &g_trends[t];
        if (!tb->used || tb->var_idx < 0 || tb->var_idx >= g_proj->num_variables) continue;
        if (tb->last_sample_ms != 0u &&
            (uint32_t)(now_ms - tb->last_sample_ms) < tb->sample_interval_ms) {
            continue;
        }
        tb->last_sample_ms = now_ms ? now_ms : 1u;

        const vgp_variable_t *v = &g_proj->variables[tb->var_idx];
        /* Skip unavailable Modbus tags per spec */
        if (v->quality != VGP_QUAL_INTERNAL && v->quality != VGP_QUAL_GOOD) continue;
        double dv = (v->type == VGP_VAR_BOOL) ? (v->val_bool ? 1.0 : 0.0) : v->val_num;
        int32_t s10 = (int32_t)(dv >= 0.0 ? (dv * 10.0 + 0.5) : (dv * 10.0 - 0.5));

        tb->samples[tb->head] = s10;
        tb->head = (uint16_t)((tb->head + 1u) % tb->capacity);
        if (tb->count < tb->capacity) tb->count++;

#if LV_USE_CHART
        vgp_element_t *el = &g_proj->elements[tb->element_idx];
        if (el->lv_obj && el->lv_extra) {
            lv_chart_set_next_value(el->lv_obj, (lv_chart_series_t *)el->lv_extra, s10);
        }
#endif
    }
}

void vgp_trend_populate_chart(uint16_t element_idx, lv_obj_t *chart, lv_chart_series_t *series)
{
#if LV_USE_CHART
    if (!chart || !series) return;
    for (uint16_t t = 0; t < g_num_trends; ++t) {
        const vgp_trend_buf_t *tb = &g_trends[t];
        if (!tb->used || tb->element_idx != element_idx) continue;
        lv_chart_set_point_count(chart, tb->capacity);
        if (tb->count == 0u) return;
        uint16_t start = (tb->count < tb->capacity) ? 0u : tb->head;
        for (uint16_t k = 0; k < tb->count; ++k) {
            uint16_t idx = (uint16_t)((start + k) % tb->capacity);
            lv_chart_set_next_value(chart, series, tb->samples[idx]);
        }
        return;
    }
#else
    (void)element_idx;
    (void)chart;
    (void)series;
#endif
}
