/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESP_HTTP_OPS_H
#define ESP_HTTP_OPS_H

#include <stddef.h>
#include <stdbool.h>

#define VGP_OTA_MAX_BYTES        (2u * 1024u * 1024u)
#define VGP_PROJECT_STORAGE_PATH "/storage/project.json"

/* Start Port 8080 Field Ops HTTP server (/ , /api/status, /api/alarms, POST /api/project).
 * Pass optional ops_token (NULL or "" to disable token check). */
bool esp_http_ops_start(const char *ops_token);

/* Non-blocking check from gui_task: returns true if a newly uploaded project.json is ready. */
bool esp_http_ops_take_ota(const char **out_json, size_t *out_len);

/* Called by gui_task after vgp_runtime_reload() finishes (persists valid JSON to Flash). */
void esp_http_ops_ota_done(bool reload_ok);

#endif
