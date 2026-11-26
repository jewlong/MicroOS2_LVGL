/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIFI_PORT_H
#define WIFI_PORT_H
#include "net_platform.h"
#include "whd.h"
#include "whd_resource_api.h"
#include "whd_network_types.h"
extern uint8_t net_board_mac[6];
extern whd_resource_source_t board_resources;
extern whd_buffer_funcs_t board_buffers;
bool board_resources_init(void);
#endif
