/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int32_t year;
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t hour;   /* 0..23 */
    uint8_t minute; /* 0..59 */
    uint8_t second; /* 0..59 */
} sys_datetime_t;

void sys_clock_init(int8_t tz_offset_hours);
void sys_clock_set_tz_offset(int8_t tz_offset_hours);
int8_t sys_clock_get_tz_offset(void);

/* Called from lwIP SNTP (tcpip_thread) when a valid UTC timestamp arrives. */
void sys_clock_set_utc(uint32_t utc_sec);
bool sys_clock_is_synced(void);

/* Current UTC epoch seconds (synced or monotonic fallback from 2026-01-01). */
uint32_t sys_clock_now_utc(void);

/* Convert a UTC epoch second value to local calendar date/time. */
void sys_clock_epoch_to_local(uint32_t utc_sec, sys_datetime_t *out);

/* Convenience: get current local calendar date/time. */
void sys_clock_now_local_dt(sys_datetime_t *out);

/* Format datetime using ViewGP format tokens: YYYY, MM, DD, HH, mm, ss. */
void sys_clock_format(const char *fmt, const sys_datetime_t *dt, char *out, size_t max_out);

#endif
