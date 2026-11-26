/* SPDX-License-Identifier: Apache-2.0 */
/* Host-side unit tests for P0 monitoring core (modbus_codec, sys_clock, vgp_alarm, vgp_parser). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* Minimal mocks for freestanding uC/OS-II & LVGL types when compiling on host */
#define OS_TICKS_PER_SEC 100u
typedef unsigned int OS_CPU_SR;
static unsigned long g_mock_ticks = 0u;
static inline OS_CPU_SR OS_CPU_SR_Save(void) { return 0u; }
static inline void OS_CPU_SR_Restore(OS_CPU_SR sr) { (void)sr; }
static inline unsigned long OSTimeGet(void) { return g_mock_ticks; }

typedef struct _lv_obj_t lv_obj_t;

/* Pull in production headers/sources directly for isolated host test */
#define LVGL_H
#include "../vgp_model.h"
#include "../vgp_parser.h"
#include "../sys_clock.h"
#include "../vgp_alarm.h"
#include "../modbus_codec.h"

#include "../vgp_parser.c"
#include "../modbus_codec.c"
#include "../vgp_alarm.c"

/* Include sys_clock.c without <ucos_ii.h> by stubbing its guard */
#define OS_uCOS_II_H
#include "../sys_clock.c"

static void test_modbus_codec(void)
{
    /* 1. CRC-16 known vector: 01 03 00 00 00 02 -> C4 0B (0x0BC4) */
    const uint8_t frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02 };
    uint16_t crc = modbus_crc16(frame, sizeof(frame));
    assert(crc == 0x0BC4u);

    /* 2. Read request encoding */
    uint8_t pdu[MODBUS_MAX_PDU_LEN];
    size_t len = modbus_encode_read_req(VGP_MB_AREA_HOLDING_REG, 100u, 4u, pdu, sizeof(pdu));
    assert(len == 5u);
    assert(pdu[0] == 0x03u && pdu[1] == 0x00u && pdu[2] == 100u && pdu[3] == 0x00u && pdu[4] == 4u);

    /* 3. Decode uint16 with scale 0.1 & offset -10 */
    vgp_modbus_map_t m;
    memset(&m, 0, sizeof(m));
    m.enabled = true;
    m.area = VGP_MB_AREA_HOLDING_REG;
    m.address = 100u;
    m.data_type = VGP_MB_TYPE_UINT16;
    m.scale = 0.1;
    m.offset = -10.0;
    m.writable = true;

    const uint8_t resp_u16[] = { 0x03, 0x02, 0x01, 0x6D }; /* 365 * 0.1 - 10 = 26.5 */
    double num = 0.0;
    bool b = false;
    assert(modbus_decode_read_value(&m, 100u, resp_u16, sizeof(resp_u16), &num, &b));
    assert(fabs(num - 26.5) < 1e-6);

    /* 4. Encode write FC06 from display value 26.5 -> raw 365 (0x016D) */
    len = modbus_encode_write_req(&m, 26.5, false, false, pdu, sizeof(pdu));
    assert(len == 5u);
    assert(pdu[0] == 0x06u && pdu[3] == 0x01u && pdu[4] == 0x6Du);

    /* Reject non-integer after inverse scaling (26.55 -> 365.5) */
    assert(modbus_encode_write_req(&m, 26.55, false, false, pdu, sizeof(pdu)) == 0u);

    /* 5. Float32 highFirst & lowFirst & NaN rejection */
    m.data_type = VGP_MB_TYPE_FLOAT32;
    m.scale = 1.0;
    m.offset = 0.0;
    m.low_word_first = false;
    /* 12.5f = 0x41480000 */
    const uint8_t resp_f32_hi[] = { 0x03, 0x04, 0x41, 0x48, 0x00, 0x00 };
    assert(modbus_decode_read_value(&m, 100u, resp_f32_hi, sizeof(resp_f32_hi), &num, &b));
    assert(fabs(num - 12.5) < 1e-6);

    m.low_word_first = true;
    const uint8_t resp_f32_lo[] = { 0x03, 0x04, 0x00, 0x00, 0x41, 0x48 };
    assert(modbus_decode_read_value(&m, 100u, resp_f32_lo, sizeof(resp_f32_lo), &num, &b));
    assert(fabs(num - 12.5) < 1e-6);

    /* NaN = 0x7FC00000 must be rejected */
    const uint8_t resp_nan[] = { 0x03, 0x04, 0x00, 0x00, 0x7F, 0xC0 };
    assert(!modbus_decode_read_value(&m, 100u, resp_nan, sizeof(resp_nan), &num, &b));

    /* 6. Batch coalescing: contiguous merged, gap not crossed */
    vgp_project_t proj;
    memset(&proj, 0, sizeof(proj));
    proj.num_devices = 1;
    proj.devices[0].enabled = true;
    proj.num_variables = 3;
    /* addr 0 (uint16), addr 1 (float32 -> 1..2), addr 5 (uint16, gap!) */
    proj.variables[0].modbus.enabled = true; proj.variables[0].modbus.device_idx = 0;
    proj.variables[0].modbus.area = VGP_MB_AREA_HOLDING_REG; proj.variables[0].modbus.address = 0;
    proj.variables[0].modbus.data_type = VGP_MB_TYPE_UINT16;

    proj.variables[1].modbus.enabled = true; proj.variables[1].modbus.device_idx = 0;
    proj.variables[1].modbus.area = VGP_MB_AREA_HOLDING_REG; proj.variables[1].modbus.address = 1;
    proj.variables[1].modbus.data_type = VGP_MB_TYPE_FLOAT32;

    proj.variables[2].modbus.enabled = true; proj.variables[2].modbus.device_idx = 0;
    proj.variables[2].modbus.area = VGP_MB_AREA_HOLDING_REG; proj.variables[2].modbus.address = 5;
    proj.variables[2].modbus.data_type = VGP_MB_TYPE_UINT16;

    uint16_t sorted[8];
    vgp_mb_batch_t batches[8];
    uint16_t nb = 0;
    modbus_build_device_batches(&proj, 0, sorted, 8, batches, 8, &nb);
    assert(nb == 2u);
    assert(batches[0].start_addr == 0u && batches[0].quantity == 3u && batches[0].var_count == 2u);
    assert(batches[1].start_addr == 5u && batches[1].quantity == 1u && batches[1].var_count == 1u);

    printf("[PASS] test_modbus_codec\n");
}

static void test_sys_clock(void)
{
    sys_clock_init(8);
    /* 2024-02-29 23:30:00 UTC = 1709249400 -> UTC+8 = 2024-03-01 07:30:00 */
    sys_datetime_t dt;
    sys_clock_epoch_to_local(1709249400u, &dt);
    assert(dt.year == 2024 && dt.month == 3 && dt.day == 1 && dt.hour == 7 && dt.minute == 30 && dt.second == 0);

    char buf[40];
    sys_clock_format("YYYY/MM/DD HH:mm:ss", &dt, buf, sizeof(buf));
    assert(strcmp(buf, "2024/03/01 07:30:00") == 0);

    sys_clock_set_utc(1709249400u);
    assert(sys_clock_is_synced());
    g_mock_ticks += 1500u; /* +15 sec */
    assert(sys_clock_now_utc() == 1709249415u);

    printf("[PASS] test_sys_clock\n");
}

static void test_parser_and_alarms(void)
{
    const char *json =
        "{"
        "\"id\":\"p1\",\"name\":\"Test\",\"startScreenId\":\"s1\","
        "\"devices\":[{\"id\":\"plc1\",\"name\":\"PLC\",\"enabled\":true,\"transport\":\"tcp\","
        "\"host\":\"192.168.1.10\",\"port\":502,\"unitId\":1,\"pollIntervalMs\":500,\"timeoutMs\":1000}],"
        "\"variables\":["
        "{\"id\":\"aw\",\"name\":\"AlarmWord\",\"type\":\"number\",\"initial\":5,"
        "\"modbus\":{\"deviceId\":\"plc1\",\"area\":\"holdingRegister\",\"address\":10,\"dataType\":\"uint16\",\"wordOrder\":\"highFirst\",\"scale\":1,\"offset\":0,\"access\":\"read\"}},"
        "{\"id\":\"am\",\"name\":\"AlarmMask\",\"type\":\"number\",\"initial\":0}"
        "],"
        "\"screens\":[{\"id\":\"s1\",\"name\":\"Main\",\"width\":960,\"height\":600,\"background\":\"#000000\","
        "\"elements\":[{\"id\":\"al1\",\"kind\":\"alarm\",\"name\":\"Alarms\",\"x\":0,\"y\":0,\"width\":400,\"height\":200,"
        "\"alarmSource\":\"global\",\"alarmMaxRows\":10}]}],"
        "\"alarmAreas\":[{\"id\":\"a1\",\"name\":\"Pump\",\"variableId\":\"aw\",\"maskVariableId\":\"am\",\"group\":\"Pump\","
        "\"updateIntervalMs\":100,\"enabled\":true,\"rules\":["
        "{\"id\":\"r0\",\"bit\":0,\"message\":\"Overload\",\"severity\":\"critical\"},"
        "{\"id\":\"r2\",\"bit\":2,\"message\":\"Hot\",\"severity\":\"warning\"}"
        "]}],"
        "\"alarmTicker\":{\"enabled\":true,\"position\":\"top\",\"intervalMs\":3000}"
        "}";

    vgp_project_t proj;
    assert(vgp_parse_project(json, strlen(json), &proj));
    assert(proj.num_devices == 1u);
    assert(strcmp(proj.devices[0].host, "192.168.1.10") == 0);
    assert(proj.variables[0].modbus.enabled && proj.variables[0].modbus.device_idx == 0);
    assert(proj.variables[0].quality == VGP_QUAL_WAITING);
    assert(proj.variables[1].quality == VGP_QUAL_INTERNAL);
    assert(proj.num_alarm_areas == 1u && proj.alarm_areas[0].num_rules == 2u);
    assert(proj.alarm_ticker.enabled && !proj.alarm_ticker.position_bottom);

    vgp_alarm_init(&proj);

    /* While aw is WAITING, alarm evaluation must pause (0 alarms) */
    vgp_alarm_evaluate(1000u);
    uint16_t ids[16];
    assert(vgp_alarm_get_active_list(ids, 16) == 0u);

    /* Mark aw GOOD with value 5 (bits 0 and 2 set) -> 2 alarms raised */
    proj.variables[0].quality = VGP_QUAL_GOOD;
    vgp_alarm_evaluate(1200u);
    assert(vgp_alarm_get_active_list(ids, 16) == 2u);

    /* Mask bit 2 -> only bit 0 remains active, bit 2 cleared with MASKED */
    proj.variables[1].val_num = 4.0;
    vgp_alarm_evaluate(1400u);
    assert(vgp_alarm_get_active_list(ids, 16) == 1u);

    /* Mark aw BAD -> active alarm must be retained! */
    proj.variables[0].quality = VGP_QUAL_BAD;
    proj.variables[0].val_num = 0.0;
    vgp_alarm_evaluate(1600u);
    assert(vgp_alarm_get_active_list(ids, 16) == 1u);

    /* ACK via element */
    vgp_alarm_ack_for_element(&proj.elements[0], 0u);
    const vgp_alarm_event_t *ev = vgp_alarm_get_event(ids[0]);
    assert(ev && ev->acknowledged);

    printf("[PASS] test_parser_and_alarms\n");
}

int main(void)
{
    test_modbus_codec();
    test_sys_clock();
    test_parser_and_alarms();
    printf("ALL P0 HOST UNIT TESTS PASSED.\n");
    return 0;
}
