/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MODBUS_CODEC_H
#define MODBUS_CODEC_H

#include "vgp_model.h"

#define MODBUS_MAX_PDU_LEN    253u
#define MODBUS_MAX_BATCHES    64u
#define MODBUS_MAX_BATCH_VARS 64u

typedef struct {
    uint16_t device_idx;
    vgp_mb_area_t area;
    uint16_t start_addr;
    uint16_t quantity;
    uint16_t var_start; /* index into sorted var index array */
    uint16_t var_count;
} vgp_mb_batch_t;

/* CRC-16/Modbus (polynomial 0xA001, init 0xFFFF). */
uint16_t modbus_crc16(const uint8_t *data, size_t len);

/* Number of 16-bit registers (or 1 for coil/discrete) occupied by a data type. */
uint16_t modbus_datatype_words(vgp_mb_datatype_t dt);

/* Build read request PDU (FC 01/02/03/04). Returns PDU length (5) or 0 on error. */
size_t modbus_encode_read_req(vgp_mb_area_t area, uint16_t start_addr, uint16_t quantity,
                              uint8_t *pdu_out, size_t max_out);

/* Build single-tag write request PDU (FC 05/06/16) from display value.
 * Validates writable, integer tolerance (1e-7), and type bounds.
 * Returns PDU length or 0 on validation/encoding error. */
size_t modbus_encode_write_req(const vgp_modbus_map_t *map, double display_val, bool bool_val,
                               bool is_bool_var, uint8_t *pdu_out, size_t max_out);

/* Decode a single tag's display value from a read response PDU payload
 * (starting at the byte-count byte, i.e., pdu[1..]).
 * Returns false on malformed length or NaN/Inf float32. */
bool modbus_decode_read_value(const vgp_modbus_map_t *map, uint16_t batch_start_addr,
                              const uint8_t *pdu, size_t pdu_len,
                              double *out_num, bool *out_bool);

/* Validate write response PDU (FC 05/06/16 echo). */
bool modbus_validate_write_resp(const uint8_t *req_pdu, size_t req_len,
                                const uint8_t *resp_pdu, size_t resp_len,
                                uint8_t *out_exc_code);

/* Build coalesced read batches for all mapped variables of device_idx.
 * Populates sorted_var_indices[0..out_total_vars-1] and batches[0..*out_num_batches-1]. */
void modbus_build_device_batches(const vgp_project_t *proj, uint16_t device_idx,
                                 uint16_t *sorted_var_indices, uint16_t max_vars,
                                 vgp_mb_batch_t *batches, uint16_t max_batches,
                                 uint16_t *out_num_batches);

#endif
