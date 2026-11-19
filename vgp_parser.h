/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_PARSER_H
#define VGP_PARSER_H

#include "vgp_model.h"

bool vgp_parse_project(const char *json, size_t len, vgp_project_t *proj);

#endif

