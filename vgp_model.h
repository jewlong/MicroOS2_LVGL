/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_MODEL_H
#define VGP_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#define VGP_MAX_SCREENS             32
#define VGP_MAX_VARIABLES           256
#define VGP_MAX_ELEMENTS_PER_SCREEN 96
#define VGP_MAX_TOTAL_ELEMENTS      768
#define VGP_MAX_ID_LEN              48
#define VGP_MAX_NAME_LEN            48
#define VGP_MAX_TEXT_LEN            128
#define VGP_MAX_MULTISTATE_ITEMS    8

/* P0 Monitoring Core Limits */
#define VGP_MAX_DEVICES             16
#define VGP_MAX_ALARM_AREAS         32
#define VGP_MAX_WORD_RULES_PER_AREA 16
#define VGP_MAX_LOCAL_ALARM_RULES   128
#define VGP_MAX_ALARM_EVENTS        128

/* P1 HMI Assets & Trends */
#define VGP_MAX_ASSETS              32
#define VGP_MAX_TRENDS              16
#define VGP_TREND_MAX_POINTS        120

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
    VGP_KIND_IMAGE,
    VGP_KIND_SLIDER,
    VGP_KIND_ARC,
    VGP_KIND_SPINBOX,
    VGP_KIND_DROPDOWN
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

typedef enum {
    VGP_QUAL_INTERNAL = 0,
    VGP_QUAL_WAITING,
    VGP_QUAL_GOOD,
    VGP_QUAL_BAD
} vgp_quality_t;

typedef enum {
    VGP_MB_TRANSPORT_TCP = 0,
    VGP_MB_TRANSPORT_RTU
} vgp_mb_transport_t;

typedef enum {
    VGP_MB_PARITY_NONE = 0,
    VGP_MB_PARITY_EVEN,
    VGP_MB_PARITY_ODD
} vgp_mb_parity_t;

typedef enum {
    VGP_DEV_STATE_DISABLED = 0,
    VGP_DEV_STATE_UNMAPPED,
    VGP_DEV_STATE_CONNECTING,
    VGP_DEV_STATE_CONNECTED,
    VGP_DEV_STATE_ERROR
} vgp_dev_state_t;

typedef enum {
    VGP_MB_AREA_COIL = 0,
    VGP_MB_AREA_DISCRETE_INPUT,
    VGP_MB_AREA_HOLDING_REG,
    VGP_MB_AREA_INPUT_REG
} vgp_mb_area_t;

typedef enum {
    VGP_MB_TYPE_BOOL = 0,
    VGP_MB_TYPE_UINT16,
    VGP_MB_TYPE_INT16,
    VGP_MB_TYPE_UINT32,
    VGP_MB_TYPE_INT32,
    VGP_MB_TYPE_FLOAT32
} vgp_mb_datatype_t;

typedef enum {
    VGP_SEV_INFO = 0,
    VGP_SEV_WARNING,
    VGP_SEV_CRITICAL
} vgp_severity_t;

typedef enum {
    VGP_OP_GT = 0,
    VGP_OP_GTE,
    VGP_OP_LT,
    VGP_OP_LTE,
    VGP_OP_EQ,
    VGP_OP_NE,
    VGP_OP_TRUE,
    VGP_OP_FALSE
} vgp_alarm_op_t;

typedef enum {
    VGP_ALARM_SRC_LOCAL = 0,
    VGP_ALARM_SRC_GLOBAL,
    VGP_ALARM_SRC_ALL
} vgp_alarm_src_t;

typedef struct {
    double value;
    char label[32];
    uint32_t color;
    uint32_t text_color;
} vgp_multistate_item_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    bool enabled;
    vgp_mb_transport_t transport;
    uint8_t unit_id;
    uint32_t poll_interval_ms;
    uint32_t timeout_ms;
    /* TCP */
    char host[64];
    uint16_t port;
    /* RTU */
    char serial_port[32];
    uint32_t baud_rate;
    vgp_mb_parity_t parity;
    uint8_t stop_bits;
    /* Runtime status */
    vgp_dev_state_t state;
    uint32_t last_success_ms;
    uint32_t error_count;
    char last_error[48];
} vgp_device_t;

typedef struct {
    bool enabled;
    int16_t device_idx;
    char device_id[VGP_MAX_ID_LEN];
    vgp_mb_area_t area;
    uint16_t address;
    vgp_mb_datatype_t data_type;
    bool low_word_first;
    double scale;
    double offset;
    bool writable;
} vgp_modbus_map_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    uint8_t bit;
    char message[96];
    vgp_severity_t severity;
    bool enabled;
} vgp_word_alarm_rule_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    char variable_id[VGP_MAX_ID_LEN];
    char mask_variable_id[VGP_MAX_ID_LEN];
    char group[48];
    uint32_t update_interval_ms;
    bool enabled;
    uint8_t num_rules;
    vgp_word_alarm_rule_t rules[VGP_MAX_WORD_RULES_PER_AREA];
    uint32_t last_check_ms;
} vgp_alarm_area_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char message[96];
    char variable_id[VGP_MAX_ID_LEN];
    vgp_alarm_op_t op;
    double threshold;
    vgp_severity_t severity;
    bool enabled;
} vgp_local_alarm_rule_t;

typedef enum {
    VGP_IMG_FIT_CONTAIN = 0,
    VGP_IMG_FIT_COVER,
    VGP_IMG_FIT_FILL,
    VGP_IMG_FIT_NONE
} vgp_img_fit_t;

typedef enum {
    VGP_NUMFMT_DECIMAL = 0,
    VGP_NUMFMT_INTEGER,
    VGP_NUMFMT_PERCENT,
    VGP_NUMFMT_SCIENTIFIC
} vgp_num_format_t;

typedef enum {
    VGP_LOCALE_EN_US = 0,
    VGP_LOCALE_ZH_TW,
    VGP_LOCALE_DE_DE
} vgp_num_locale_t;

typedef enum {
    VGP_ALIGN_LEFT = 0,
    VGP_ALIGN_CENTER,
    VGP_ALIGN_RIGHT
} vgp_text_align_t;

typedef enum {
    VGP_VALIGN_TOP = 0,
    VGP_VALIGN_MIDDLE,
    VGP_VALIGN_BOTTOM
} vgp_vert_align_t;

typedef enum {
    VGP_BORDER_SOLID = 0,
    VGP_BORDER_NONE,
    VGP_BORDER_DASHED,
    VGP_BORDER_DOTTED
} vgp_border_style_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    const char *b64_ptr;
    size_t b64_len;
    bool is_png;
} vgp_asset_ref_t;

typedef struct {
    bool enabled;
    bool position_bottom;
    uint32_t interval_ms;
} vgp_alarm_ticker_t;

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

    /* Alarm widget fields */
    uint16_t alarm_rule_start;
    uint16_t num_alarm_rules;
    uint8_t alarm_max_rows;
    vgp_alarm_src_t alarm_source;
    char alarm_group[48];

    /* P1 Image / Input / Threshold / Trend / Scale / Table fields */
    char image_asset_id[VGP_MAX_ID_LEN];
    const char *inline_b64_ptr;
    size_t inline_b64_len;
    bool inline_is_png;
    vgp_img_fit_t image_fit;

    double input_min, input_max, input_step;
    bool has_input_min, has_input_max;
    double warning_threshold, critical_threshold;
    bool has_warning_threshold, has_critical_threshold;
    bool show_value;
    bool show_scale;
    uint32_t sample_interval_ms;
    uint16_t history_points;
    bool scale_vertical;
    bool scale_show_axis;
    bool scale_show_labels;
    uint32_t grid_color;

    /* P2 Number Formatting */
    vgp_num_format_t number_format;
    vgp_num_locale_t number_locale;
    bool use_grouping;
    char number_prefix[16];

    /* P2 Typography & Appearance */
    vgp_text_align_t text_align;
    vgp_vert_align_t vertical_align;
    bool word_wrap;
    bool underline;
    bool text_background;
    bool shape_filled;
    uint16_t line_height_x100;
    vgp_border_style_t border_style;

    /* P2 Button state colors */
    uint32_t btn_pressed_bg, btn_pressed_color;
    uint32_t btn_disabled_bg, btn_disabled_color;
    bool has_btn_pressed_bg, has_btn_pressed_color;
    bool has_btn_disabled_bg, has_btn_disabled_color;

    /* LVGL runtime object pointers */
    lv_obj_t *lv_obj;
    lv_obj_t *lv_extra;
    lv_obj_t *lv_aux;
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

    /* Modbus mapping & live quality */
    vgp_modbus_map_t modbus;
    vgp_quality_t quality;
    uint32_t updated_ms;
    bool write_failed;
} vgp_variable_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    char name[VGP_MAX_NAME_LEN];
    char start_screen_id[VGP_MAX_ID_LEN];
    uint16_t num_devices;
    vgp_device_t devices[VGP_MAX_DEVICES];
    uint16_t num_variables;
    vgp_variable_t variables[VGP_MAX_VARIABLES];
    uint16_t num_screens;
    vgp_screen_t screens[VGP_MAX_SCREENS];
    uint16_t total_elements;
    vgp_element_t elements[VGP_MAX_TOTAL_ELEMENTS];
    uint16_t num_alarm_areas;
    vgp_alarm_area_t alarm_areas[VGP_MAX_ALARM_AREAS];
    uint16_t total_local_alarm_rules;
    vgp_local_alarm_rule_t local_alarm_rules[VGP_MAX_LOCAL_ALARM_RULES];
    vgp_alarm_ticker_t alarm_ticker;
    uint16_t num_assets;
    vgp_asset_ref_t assets[VGP_MAX_ASSETS];
} vgp_project_t;

#endif
