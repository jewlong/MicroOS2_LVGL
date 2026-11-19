/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_MODBUS_H
#define VGP_MODBUS_H

#include "vgp_model.h"

void vgp_modbus_start(vgp_project_t *proj);
void vgp_modbus_task(void *arg);

/* Non-blocking write enqueue called from gui_task.
 * Returns false if tag is read-only, bad quality, or queue is full. */
bool vgp_modbus_enqueue_write(uint16_t var_idx, double num_val, bool bool_val);

/* Compact status string for the HMI bottom bar (empty if no devices). */
void vgp_modbus_status_summary(char *out, size_t max_out);

#endif
