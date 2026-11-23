/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HID_TOUCH_H
#define HID_TOUCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HID_TOUCH_MAX_SLOTS 5u

typedef struct {
    bool has_tip;
    bool has_x;
    bool has_y;
    bool x_signed;
    bool y_signed;
    uint16_t tip_bit;
    uint16_t x_bit;
    uint16_t y_bit;
    uint8_t tip_size;
    uint8_t x_size;
    uint8_t y_size;
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
} hid_touch_slot_t;

typedef struct {
    bool valid;
    bool has_report_id;
    bool is_digitizer;
    uint8_t report_id;
    uint8_t num_slots;
    uint16_t min_report_bytes;
    hid_touch_slot_t slots[HID_TOUCH_MAX_SLOTS];
} hid_touch_layout_t;

typedef struct {
    bool pressed;
    uint16_t x_norm; /* 0..65535 */
    uint16_t y_norm; /* 0..65535 */
    int32_t raw_x;
    int32_t raw_y;
} hid_touch_sample_t;

bool hid_touch_parse_report_desc(const uint8_t *desc, size_t len, hid_touch_layout_t *out);
bool hid_touch_decode_report(const hid_touch_layout_t *layout, const uint8_t *report, size_t len,
                             hid_touch_sample_t *out);

#endif
