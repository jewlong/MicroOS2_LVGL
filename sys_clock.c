/* SPDX-License-Identifier: Apache-2.0 */
#include "sys_clock.h"
#include <ucos_ii.h>
#include <stdio.h>
#include <string.h>

/* Fallback base epoch when SNTP has not synced yet: 2026-01-01 00:00:00 UTC. */
#define SYS_CLOCK_FALLBACK_UTC 1767225600u

static volatile int8_t g_tz_offset_hours = 8;
static volatile bool g_synced = false;
static volatile uint32_t g_sync_utc = SYS_CLOCK_FALLBACK_UTC;
static volatile uint32_t g_sync_tick = 0u;

void sys_clock_init(int8_t tz_offset_hours)
{
    OS_CPU_SR cpu_sr = OS_CPU_SR_Save();
    if (tz_offset_hours >= -12 && tz_offset_hours <= 14) {
        g_tz_offset_hours = tz_offset_hours;
    } else {
        g_tz_offset_hours = 8;
    }
    g_synced = false;
    g_sync_utc = SYS_CLOCK_FALLBACK_UTC;
    g_sync_tick = 0u;
    OS_CPU_SR_Restore(cpu_sr);
}

void sys_clock_set_tz_offset(int8_t tz_offset_hours)
{
    if (tz_offset_hours < -12 || tz_offset_hours > 14) {
        return;
    }
    OS_CPU_SR cpu_sr = OS_CPU_SR_Save();
    g_tz_offset_hours = tz_offset_hours;
    OS_CPU_SR_Restore(cpu_sr);
}

int8_t sys_clock_get_tz_offset(void)
{
    return g_tz_offset_hours;
}

void sys_clock_set_utc(uint32_t utc_sec)
{
    if (utc_sec < 1577836800u) {
        /* Reject pre-2020 bogus timestamps. */
        return;
    }
    uint32_t now_tick = (uint32_t)OSTimeGet();
    OS_CPU_SR cpu_sr = OS_CPU_SR_Save();
    g_sync_utc = utc_sec;
    g_sync_tick = now_tick;
    g_synced = true;
    OS_CPU_SR_Restore(cpu_sr);
}

bool sys_clock_is_synced(void)
{
    return g_synced;
}

uint32_t sys_clock_now_utc(void)
{
    uint32_t now_tick = (uint32_t)OSTimeGet();
    OS_CPU_SR cpu_sr = OS_CPU_SR_Save();
    uint32_t base_utc = g_sync_utc;
    uint32_t base_tick = g_sync_tick;
    OS_CPU_SR_Restore(cpu_sr);

    uint32_t elapsed_sec = (uint32_t)(now_tick - base_tick) / (uint32_t)OS_TICKS_PER_SEC;
    return base_utc + elapsed_sec;
}

/* Howard Hinnant's civil-from-days algorithm (public domain, zero libc dependency). */
void sys_clock_epoch_to_local(uint32_t utc_sec, sys_datetime_t *out)
{
    if (!out) {
        return;
    }
    int64_t local_sec = (int64_t)utc_sec + (int64_t)g_tz_offset_hours * 3600;
    int64_t days = local_sec / 86400;
    int64_t rem = local_sec % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = mp + (mp < 10u ? 3u : (uint32_t)(-9));
    if (m <= 2u) {
        y += 1;
    }

    out->year = (int32_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)((rem % 3600) / 60);
    out->second = (uint8_t)(rem % 60);
}

void sys_clock_now_local_dt(sys_datetime_t *out)
{
    sys_clock_epoch_to_local(sys_clock_now_utc(), out);
}

static void append_dec2(char *out, size_t max_out, size_t *pos, uint32_t v)
{
    if (*pos + 2u < max_out) {
        out[(*pos)++] = (char)('0' + ((v / 10u) % 10u));
        out[(*pos)++] = (char)('0' + (v % 10u));
    }
}

static void append_dec4(char *out, size_t max_out, size_t *pos, int32_t v)
{
    if (v < 0) v = 0;
    uint32_t u = (uint32_t)v;
    if (*pos + 4u < max_out) {
        out[(*pos)++] = (char)('0' + ((u / 1000u) % 10u));
        out[(*pos)++] = (char)('0' + ((u / 100u) % 10u));
        out[(*pos)++] = (char)('0' + ((u / 10u) % 10u));
        out[(*pos)++] = (char)('0' + (u % 10u));
    }
}

void sys_clock_format(const char *fmt, const sys_datetime_t *dt, char *out, size_t max_out)
{
    if (!out || max_out == 0u || !dt) {
        return;
    }
    if (!fmt || !fmt[0]) {
        fmt = "YYYY/MM/DD HH:mm:ss";
    }
    size_t pos = 0u;
    const char *p = fmt;
    while (*p && pos + 1u < max_out) {
        if (p[0] == 'Y' && p[1] == 'Y' && p[2] == 'Y' && p[3] == 'Y') {
            append_dec4(out, max_out, &pos, dt->year);
            p += 4;
        } else if (p[0] == 'M' && p[1] == 'M') {
            append_dec2(out, max_out, &pos, dt->month);
            p += 2;
        } else if (p[0] == 'D' && p[1] == 'D') {
            append_dec2(out, max_out, &pos, dt->day);
            p += 2;
        } else if (p[0] == 'H' && p[1] == 'H') {
            append_dec2(out, max_out, &pos, dt->hour);
            p += 2;
        } else if (p[0] == 'm' && p[1] == 'm') {
            append_dec2(out, max_out, &pos, dt->minute);
            p += 2;
        } else if (p[0] == 's' && p[1] == 's') {
            append_dec2(out, max_out, &pos, dt->second);
            p += 2;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}
