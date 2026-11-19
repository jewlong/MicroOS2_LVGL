/* SPDX-License-Identifier: Apache-2.0 */
#include "modbus_codec.h"
#include <string.h>

uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; ++b) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint16_t modbus_datatype_words(vgp_mb_datatype_t dt)
{
    switch (dt) {
        case VGP_MB_TYPE_UINT32:
        case VGP_MB_TYPE_INT32:
        case VGP_MB_TYPE_FLOAT32:
            return 2u;
        default:
            return 1u;
    }
}

static uint8_t area_read_fc(vgp_mb_area_t area)
{
    switch (area) {
        case VGP_MB_AREA_COIL:           return 0x01u;
        case VGP_MB_AREA_DISCRETE_INPUT: return 0x02u;
        case VGP_MB_AREA_HOLDING_REG:    return 0x03u;
        case VGP_MB_AREA_INPUT_REG:      return 0x04u;
        default:                         return 0u;
    }
}

size_t modbus_encode_read_req(vgp_mb_area_t area, uint16_t start_addr, uint16_t quantity,
                              uint8_t *pdu_out, size_t max_out)
{
    uint8_t fc = area_read_fc(area);
    if (!fc || !pdu_out || max_out < 5u || quantity == 0u) return 0u;
    pdu_out[0] = fc;
    pdu_out[1] = (uint8_t)(start_addr >> 8);
    pdu_out[2] = (uint8_t)(start_addr & 0xFFu);
    pdu_out[3] = (uint8_t)(quantity >> 8);
    pdu_out[4] = (uint8_t)(quantity & 0xFFu);
    return 5u;
}

static bool double_to_int_checked(double v, double min_v, double max_v, int64_t *out)
{
    if (v < min_v - 1e-7 || v > max_v + 1e-7) return false;
    int64_t iv = (v >= 0.0) ? (int64_t)(v + 0.5) : (int64_t)(v - 0.5);
    double diff = v - (double)iv;
    if (diff < -1e-7 || diff > 1e-7) return false;
    if ((double)iv < min_v || (double)iv > max_v) return false;
    *out = iv;
    return true;
}

static bool is_finite_f32(uint32_t bits)
{
    return (bits & 0x7F800000u) != 0x7F800000u;
}

size_t modbus_encode_write_req(const vgp_modbus_map_t *map, double display_val, bool bool_val,
                               bool is_bool_var, uint8_t *pdu_out, size_t max_out)
{
    if (!map || !map->enabled || !map->writable || !pdu_out) return 0u;

    if (map->area == VGP_MB_AREA_COIL) {
        if (max_out < 5u) return 0u;
        bool on = is_bool_var ? bool_val : (display_val != 0.0);
        pdu_out[0] = 0x05u;
        pdu_out[1] = (uint8_t)(map->address >> 8);
        pdu_out[2] = (uint8_t)(map->address & 0xFFu);
        pdu_out[3] = on ? 0xFFu : 0x00u;
        pdu_out[4] = 0x00u;
        return 5u;
    }

    if (map->area != VGP_MB_AREA_HOLDING_REG) {
        return 0u;
    }
    if (map->scale == 0.0) return 0u;
    double raw = (is_bool_var ? (bool_val ? 1.0 : 0.0) : display_val) - map->offset;
    raw /= map->scale;

    if (map->data_type == VGP_MB_TYPE_UINT16 || map->data_type == VGP_MB_TYPE_INT16 ||
        map->data_type == VGP_MB_TYPE_BOOL) {
        if (max_out < 5u) return 0u;
        int64_t iv = 0;
        if (map->data_type == VGP_MB_TYPE_INT16) {
            if (!double_to_int_checked(raw, -32768.0, 32767.0, &iv)) return 0u;
        } else {
            if (!double_to_int_checked(raw, 0.0, 65535.0, &iv)) return 0u;
        }
        uint16_t u16 = (uint16_t)(int16_t)iv;
        if (map->data_type != VGP_MB_TYPE_INT16) u16 = (uint16_t)iv;
        pdu_out[0] = 0x06u;
        pdu_out[1] = (uint8_t)(map->address >> 8);
        pdu_out[2] = (uint8_t)(map->address & 0xFFu);
        pdu_out[3] = (uint8_t)(u16 >> 8);
        pdu_out[4] = (uint8_t)(u16 & 0xFFu);
        return 5u;
    }

    /* 32-bit Holding Register (FC 16, 2 registers = 4 bytes) */
    if (max_out < 10u || map->address > 65534u) return 0u;
    uint32_t u32 = 0u;
    if (map->data_type == VGP_MB_TYPE_UINT32) {
        int64_t iv = 0;
        if (!double_to_int_checked(raw, 0.0, 4294967295.0, &iv)) return 0u;
        u32 = (uint32_t)iv;
    } else if (map->data_type == VGP_MB_TYPE_INT32) {
        int64_t iv = 0;
        if (!double_to_int_checked(raw, -2147483648.0, 2147483647.0, &iv)) return 0u;
        u32 = (uint32_t)(int32_t)iv;
    } else if (map->data_type == VGP_MB_TYPE_FLOAT32) {
        float f = (float)raw;
        memcpy(&u32, &f, 4u);
        if (!is_finite_f32(u32)) return 0u;
    } else {
        return 0u;
    }

    uint16_t hi = (uint16_t)(u32 >> 16);
    uint16_t lo = (uint16_t)(u32 & 0xFFFFu);
    uint16_t w0 = map->low_word_first ? lo : hi;
    uint16_t w1 = map->low_word_first ? hi : lo;

    pdu_out[0] = 0x10u;
    pdu_out[1] = (uint8_t)(map->address >> 8);
    pdu_out[2] = (uint8_t)(map->address & 0xFFu);
    pdu_out[3] = 0x00u;
    pdu_out[4] = 0x02u;
    pdu_out[5] = 0x04u;
    pdu_out[6] = (uint8_t)(w0 >> 8);
    pdu_out[7] = (uint8_t)(w0 & 0xFFu);
    pdu_out[8] = (uint8_t)(w1 >> 8);
    pdu_out[9] = (uint8_t)(w1 & 0xFFu);
    return 10u;
}

bool modbus_decode_read_value(const vgp_modbus_map_t *map, uint16_t batch_start_addr,
                              const uint8_t *pdu, size_t pdu_len,
                              double *out_num, bool *out_bool)
{
    if (!map || !pdu || pdu_len < 2u) return false;
    uint8_t expected_fc = area_read_fc(map->area);
    if (pdu[0] != expected_fc) return false;
    uint8_t byte_count = pdu[1];
    if (pdu_len < (size_t)byte_count + 2u) return false;
    if (map->address < batch_start_addr) return false;
    uint16_t rel = (uint16_t)(map->address - batch_start_addr);
    const uint8_t *data = pdu + 2u;

    if (map->area == VGP_MB_AREA_COIL || map->area == VGP_MB_AREA_DISCRETE_INPUT) {
        uint16_t byte_idx = (uint16_t)(rel / 8u);
        uint8_t bit_idx = (uint8_t)(rel % 8u);
        if (byte_idx >= byte_count) return false;
        bool b = ((data[byte_idx] >> bit_idx) & 1u) != 0u;
        if (out_bool) *out_bool = b;
        if (out_num) *out_num = b ? 1.0 : 0.0;
        return true;
    }

    uint16_t words = modbus_datatype_words(map->data_type);
    size_t offset_bytes = (size_t)rel * 2u;
    if (offset_bytes + (size_t)words * 2u > byte_count) return false;

    double raw = 0.0;
    if (words == 1u) {
        uint16_t w = (uint16_t)(((uint16_t)data[offset_bytes] << 8) | data[offset_bytes + 1u]);
        if (map->data_type == VGP_MB_TYPE_INT16) {
            raw = (double)(int16_t)w;
        } else {
            raw = (double)w;
        }
    } else {
        uint16_t w0 = (uint16_t)(((uint16_t)data[offset_bytes] << 8) | data[offset_bytes + 1u]);
        uint16_t w1 = (uint16_t)(((uint16_t)data[offset_bytes + 2u] << 8) | data[offset_bytes + 3u]);
        uint32_t u32 = map->low_word_first ?
            (((uint32_t)w1 << 16) | (uint32_t)w0) :
            (((uint32_t)w0 << 16) | (uint32_t)w1);
        if (map->data_type == VGP_MB_TYPE_UINT32) {
            raw = (double)u32;
        } else if (map->data_type == VGP_MB_TYPE_INT32) {
            raw = (double)(int32_t)u32;
        } else if (map->data_type == VGP_MB_TYPE_FLOAT32) {
            if (!is_finite_f32(u32)) return false;
            float f = 0.0f;
            memcpy(&f, &u32, 4u);
            raw = (double)f;
        } else {
            return false;
        }
    }

    double val = raw * map->scale + map->offset;
    /* Guard against NaN/Inf after scaling */
    if (val != val || val > 1e30 || val < -1e30) return false;
    if (out_num) *out_num = val;
    if (out_bool) *out_bool = (val != 0.0);
    return true;
}

bool modbus_validate_write_resp(const uint8_t *req_pdu, size_t req_len,
                                const uint8_t *resp_pdu, size_t resp_len,
                                uint8_t *out_exc_code)
{
    if (out_exc_code) *out_exc_code = 0u;
    if (!req_pdu || req_len < 5u || !resp_pdu || resp_len < 2u) return false;
    if (resp_pdu[0] == (uint8_t)(req_pdu[0] | 0x80u)) {
        if (out_exc_code) *out_exc_code = resp_pdu[1];
        return false;
    }
    if (resp_len < 5u) return false;
    if (req_pdu[0] == 0x05u || req_pdu[0] == 0x06u) {
        return memcmp(req_pdu, resp_pdu, 5u) == 0;
    }
    if (req_pdu[0] == 0x10u) {
        /* Echoes FC, start addr (2), quantity (2) */
        return memcmp(req_pdu, resp_pdu, 5u) == 0;
    }
    return false;
}

void modbus_build_device_batches(const vgp_project_t *proj, uint16_t device_idx,
                                 uint16_t *sorted_var_indices, uint16_t max_vars,
                                 vgp_mb_batch_t *batches, uint16_t max_batches,
                                 uint16_t *out_num_batches)
{
    if (out_num_batches) *out_num_batches = 0u;
    if (!proj || !sorted_var_indices || !batches || !out_num_batches) return;

    uint16_t n = 0u;
    for (uint16_t i = 0; i < proj->num_variables && n < max_vars; ++i) {
        const vgp_variable_t *v = &proj->variables[i];
        if (v->modbus.enabled && v->modbus.device_idx == (int16_t)device_idx) {
            sorted_var_indices[n++] = i;
        }
    }
    if (n == 0u) return;

    /* Insertion sort by (area, address) */
    for (uint16_t i = 1u; i < n; ++i) {
        uint16_t key = sorted_var_indices[i];
        const vgp_modbus_map_t *km = &proj->variables[key].modbus;
        int32_t j = (int32_t)i - 1;
        while (j >= 0) {
            const vgp_modbus_map_t *jm = &proj->variables[sorted_var_indices[j]].modbus;
            if (jm->area > km->area || (jm->area == km->area && jm->address > km->address)) {
                sorted_var_indices[j + 1] = sorted_var_indices[j];
                j--;
            } else {
                break;
            }
        }
        sorted_var_indices[j + 1] = key;
    }

    uint16_t nb = 0u;
    uint16_t idx = 0u;
    while (idx < n && nb < max_batches) {
        const vgp_modbus_map_t *first = &proj->variables[sorted_var_indices[idx]].modbus;
        uint16_t max_qty = (first->area == VGP_MB_AREA_COIL || first->area == VGP_MB_AREA_DISCRETE_INPUT)
                           ? 2000u : 125u;
        uint16_t b_start = first->address;
        uint32_t b_end = (uint32_t)first->address + modbus_datatype_words(first->data_type);
        uint16_t v_start = idx;
        idx++;

        while (idx < n) {
            const vgp_modbus_map_t *cur = &proj->variables[sorted_var_indices[idx]].modbus;
            if (cur->area != first->area) break;
            /* Contiguous or overlapping only (no cross-gap) */
            if ((uint32_t)cur->address > b_end) break;
            uint32_t cand_end = (uint32_t)cur->address + modbus_datatype_words(cur->data_type);
            if (cand_end < b_end) cand_end = b_end;
            if (cand_end - (uint32_t)b_start > max_qty) break;
            b_end = cand_end;
            idx++;
        }

        vgp_mb_batch_t *b = &batches[nb++];
        b->device_idx = device_idx;
        b->area = first->area;
        b->start_addr = b_start;
        b->quantity = (uint16_t)(b_end - (uint32_t)b_start);
        b->var_start = v_start;
        b->var_count = (uint16_t)(idx - v_start);
    }

    *out_num_batches = nb;
}
