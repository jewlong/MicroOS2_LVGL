/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_IMAGE_H
#define VGP_IMAGE_H

#include "vgp_model.h"

/* Pre-decode project image assets (JPEG/PNG Base64) into LVGL image descriptors.
 * Must be called from gui_task after lv_init(). */
void vgp_image_init(const vgp_project_t *proj);

/* Look up decoded image descriptor for an element (by image_asset_id or inline image). */
const lv_image_dsc_t *vgp_image_get_for_element(const vgp_element_t *el, uint16_t element_idx);

#endif
