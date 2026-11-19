/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_ALARM_H
#define VGP_ALARM_H

#include "vgp_model.h"

typedef enum {
    VGP_CLR_NONE = 0,
    VGP_CLR_NORMAL,
    VGP_CLR_MASKED,
    VGP_CLR_DISABLED
} vgp_clear_reason_t;

typedef struct {
    bool used;
    bool is_global;
    uint16_t source_idx; /* area_idx if is_global, else element_idx */
    uint16_t rule_idx;
    char group[48];
    char message[96];
    vgp_severity_t severity;
    uint32_t raised_epoch;
    uint32_t cleared_epoch; /* 0 = still active */
    vgp_clear_reason_t clear_reason;
    bool acknowledged;
} vgp_alarm_event_t;

void vgp_alarm_init(vgp_project_t *proj);
void vgp_alarm_evaluate(uint32_t now_ms);
uint32_t vgp_alarm_generation(void);

uint16_t vgp_alarm_query(const vgp_element_t *el, uint16_t element_idx, uint16_t *out_indices, uint16_t max_out);
uint16_t vgp_alarm_get_active_list(uint16_t *out_indices, uint16_t max_out);
const vgp_alarm_event_t *vgp_alarm_get_event(uint16_t idx);
void vgp_alarm_ack_for_element(const vgp_element_t *el, uint16_t element_idx);

#endif
