/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

extern volatile uint32_t usb_touch_connected;
extern volatile uint32_t usb_touch_reports;
extern volatile uint32_t usb_touch_x_max;
extern volatile uint32_t usb_touch_y_max;

void touch_post(uint16_t x_norm, uint16_t y_norm, bool pressed);
void touch_reset(void);
/* Returns true if additional queued events remain (for LVGL continue_reading). */
bool touch_poll(uint16_t *out_x_norm, uint16_t *out_y_norm, bool *out_pressed);

#endif
