/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MOUSE_QUEUE_H
#define MOUSE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

extern volatile uint32_t usb_mouse_connected;
extern volatile uint32_t usb_mouse_reports;

void mouse_post(int8_t dx, int8_t dy, uint8_t buttons);
void mouse_reset(void);
bool mouse_poll(int32_t *out_dx, int32_t *out_dy, bool *out_left);

#endif
