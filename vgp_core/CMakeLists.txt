# SPDX-License-Identifier: Apache-2.0
# Shared ViewGP HMI Core Component for ESP-IDF (ESP32-P4 / ESP32-S3)
# Ignored by the Pi Zero GNU Makefile, which compiles the same .c files via vpath.

idf_component_register(
    SRCS
        "vgp_parser.c"
        "vgp_runtime.c"
        "vgp_alarm.c"
        "vgp_trend.c"
        "vgp_image.c"
        "vgp_font_cjk.c"
        "modbus_codec.c"
        "sys_clock.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl
)
