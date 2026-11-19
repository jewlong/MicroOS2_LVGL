/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_MODEL_H
#define VGP_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#define VGP_MAX_SCREENS             16
#define VGP_MAX_VARIABLES           64
#define VGP_MAX_ELEMENTS_PER_SCREEN 48
#define VGP_MAX_TOTAL_ELEMENTS      256
#define VGP_MAX_ID_LEN              48
#define VGP_MAX_NAME_LEN            48
#define VGP_MAX_TEXT_LEN            128
#define VGP_MAX_MULTISTATE_ITEMS    8

typedef enum {
    VGP_KIND_UNKNOWN = 0,
    VGP_KIND_TEXT,
    VGP_KIND_RECTANGLE,
    VGP_KIND_CIRCLE,
    VGP_KIND_LINE,
    VGP_KIND_BUTTON,
    VGP_KIND_INDICATOR,
    VGP_KIND_NUMBER,
    VGP_KIND_INPUT,
    VGP_KIND_SWITCH,
    VGP_KIND_GAUGE,
    VGP_KIND_BAR,
    VGP_KIND_TREND,
    VGP_KIND_ALARM,
    VGP_KIND_DATETIME,
    VGP_KIND_MULTISTATE,
    VGP_KIND_SCALE,
    VGP_KIND_TABLE,
    VGP_KIND_IMAGE
} vgp_kind_t;

typedef enum {
    VGP_ACT_NONE = 0,
    VGP_ACT_TOGGLE,
    VGP_ACT_SET,
    VGP_ACT_INCREMENT,
    VGP_ACT_RESET,
    VGP_ACT_MOMENTARY,
    VGP_ACT_SCREEN,
    VGP_ACT_PREV_SCREEN,
    VGP_ACT_OPEN_DIALOG,
    VGP_ACT_CLOSE_DIALOG,
    VGP_ACT_POPUP,
    VGP_ACT_CLOSE_POPUP
} vgp_action_t;

typedef enum {
    VGP_VAR_UNKNOWN = 0,
    VGP_VAR_NUM,
    VGP_VAR_BOOL,
    VGP_VAR_STR
} vgp_var_type_t;

typedef struct {
    double value;
    char label[32];
    uint32_t color;
    uint32_t text_color;
} vgp_multistate_item_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    vgp_kind_t kind;
    int32_t x, y, width, height;
    char text[VGP_MAX_TEXT_LEN];
    uint32_t color;
    uint32_t background;
    uint32_t border_color;
    uint16_t font_size;
    uint16_t border_width;
    uint16_t border_radius;
    char variable_id[VGP_MAX_ID_LEN];
    vgp_action_t action;
    char action_value[32];
    char target_screen_id[VGP_MAX_ID_LEN];
    bool visible;
    char condition_id[VGP_MAX_ID_LEN];
    char enabled_id[VGP_MAX_ID_LEN];
    double scale_min, scale_max;
    uint32_t fill_color, track_color;
    bool orientation_vertical;
    uint8_t decimals;
    char unit[16];
    char date_time_format[32];
    uint8_t num_multistate_items;
    vgp_multistate_item_t multistate_items[VGP_MAX_MULTISTATE_ITEMS];
    uint8_t table_cols, table_rows;
    uint8_t scale_major_ticks, scale_minor_ticks;

    /* LVGL runtime object pointers */
    lv_obj_t *lv_obj;
    lv_obj_t *lv_extra;
} vgp_element_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    int32_t width, height;
    uint32_t background;
    bool is_popup;
    char parent_screen_id[VGP_MAX_ID_LEN];
    uint16_t element_start;
    uint16_t num_elements;

    /* LVGL runtime screen object */
    lv_obj_t *lv_screen;
} vgp_screen_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    vgp_var_type_t type;
    double val_num;
    bool val_bool;
    char val_str[64];
    double initial_num;
    bool initial_bool;
    char initial_str[64];
} vgp_variable_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    char start_screen_id[VGP_MAX_ID_LEN];
    uint16_t num_variables;
    vgp_variable_t variables[VGP_MAX_VARIABLES];
    uint16_t num_screens;
    vgp_screen_t screens[VGP_MAX_SCREENS];
    uint16_t total_elements;
    vgp_element_t elements[VGP_MAX_TOTAL_ELEMENTS];
} vgp_project_t;

#endif

