/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VGP_PARSER_H
#define VGP_PARSER_H

#include "vgp_model.h"

#define VGP_MAX_PARSE_WARNINGS 16

typedef struct {
    uint32_t schema_version;
    bool has_schema_version;
    char error[96];
    uint16_t total_warnings;
    uint16_t stored_warnings;
    char warnings[VGP_MAX_PARSE_WARNINGS][96];
} vgp_parse_report_t;

bool vgp_parse_project(const char *json, size_t len, vgp_project_t *proj);
bool vgp_parse_project_ex(const char *json, size_t len, vgp_project_t *proj, vgp_parse_report_t *report);

#endif
