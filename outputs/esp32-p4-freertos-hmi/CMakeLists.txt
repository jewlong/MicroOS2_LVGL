cmake_minimum_required(VERSION 3.16)

# Mount the cross-platform ViewGP HMI core as an ESP-IDF component (single source of truth)
set(EXTRA_COMPONENT_DIRS "../../vgp_core")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(viewgp_esp32p4_hmi)
