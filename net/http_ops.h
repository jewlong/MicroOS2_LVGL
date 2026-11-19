/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HTTP_OPS_H
#define HTTP_OPS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HTTP_OPS_PORT      8080u
#define VGP_OTA_MAX_BYTES  (2u * 1024u * 1024u)

/* Initialize HTTP server on port 8080 (must be called from tcpip_thread). */
void http_ops_init(void);

/* Called by gui_task to check if a new project.json was uploaded via POST /api/project.
 * If true, *out_json and *out_len point to the staging buffer until http_ops_ota_done() is called. */
bool http_ops_take_ota(const char **out_json, size_t *out_len);
void http_ops_ota_done(void);

#endif
