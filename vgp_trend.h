/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_TREND_H
#define VGP_TREND_H

#include "vgp_model.h"

void vgp_trend_init(vgp_project_t *proj);

/* Called periodically from vgp_runtime_update(); samples ALL trend widgets in background. */
void vgp_trend_tick(uint32_t now_ms);

/* Populate a freshly created lv_chart with stored historical samples. */
void vgp_trend_populate_chart(uint16_t element_idx, lv_obj_t *chart, lv_chart_series_t *series);

#endif
