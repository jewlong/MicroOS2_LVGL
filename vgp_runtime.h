/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_RUNTIME_H
#define VGP_RUNTIME_H

#include "vgp_model.h"

bool vgp_runtime_init(const char *json, size_t json_len, uint32_t screen_w, uint32_t screen_h, lv_group_t *kbd_group);
void vgp_runtime_update(void);
void vgp_runtime_handle_key(uint32_t key);
bool vgp_runtime_is_active(void);
const char *vgp_runtime_project_name(void);

#endif

