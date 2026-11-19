/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfg.h"
#include "platform.h"

#define ATAG_BASE     0x100u
#define ATAG_CORE     0x54410001u
#define ATAG_INITRD2  0x54420005u
#define INITRD_FIXED  0x02000000u /* Fixed initramfs address in config.txt */
#define RAM_LIMIT     0x20000000u /* BCM2835/BCM2837 512MB RAM ceiling. */
/* Never accept data that could overlap the kernel image/stacks (< 16 MiB). */
#define RANGE_MIN     0x01000000u

static wifi_settings_t wifi;
static bootcfg_status_t status = BOOTCFG_NOT_TRIED;
static const char *project_json_ptr = (void *)0;
static size_t project_json_size = 0u;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool blank(char c) { return c == ' ' || c == '\t'; }

/* Case-insensitive compare of key[0..n) with a lowercase literal. */
static bool key_is(const char *key, size_t n, const char *name)
{
    size_t i = 0;
    while (name[i] != '\0') {
        if (i >= n || lower(key[i]) != name[i]) { return false; }
        ++i;
    }
    return i == n;
}

bootcfg_status_t bootcfg_parse_wifi(const char *text, size_t length, wifi_settings_t *out)
{
    wifi_settings_t r;
    bool have_ssid = false, have_password = false, bad_country = false;
    size_t i = 0;
    for (size_t k = 0; k < sizeof(r); ++k) { ((char *)&r)[k] = 0; }
    while (i < length) {
        while (i < length && text[i] == '\0') { ++i; }
        if (i >= length) { break; }
        size_t start = i, end;
        while (i < length && text[i] != '\n' && text[i] != '\0') { ++i; }
        end = i;
        if (i < length) { ++i; }
        if (end - start >= 3u && (uint8_t)text[start] == 0xefu &&
            (uint8_t)text[start+1u] == 0xbbu && (uint8_t)text[start+2u] == 0xbfu) {
            start += 3u; /* Skip UTF-8 BOM from Notepad on any starting line. */
        }
        while (end > start && (text[end-1u] == '\r' || blank(text[end-1u]))) { --end; }
        while (start < end && blank(text[start])) { ++start; }
        if (start >= end || text[start] == '#') { continue; }
        if (text[start] == '{') { break; } /* Stop if JSON object begins on this line. */
        size_t eq = start;
        while (eq < end && text[eq] != '=') { ++eq; }
        if (eq >= end) { continue; } /* Unknown text without '=' is ignored. */
        size_t key_end = eq;
        while (key_end > start && blank(text[key_end-1u])) { --key_end; }
        size_t value = eq + 1u;
        while (value < end && blank(text[value])) { ++value; }
        const size_t key_len = key_end - start, value_len = end - value;
        if (key_is(text + start, key_len, "ssid")) {
            have_ssid = true;
            if (value_len >= 1u && value_len <= 32u) {
                for (size_t k = 0; k < value_len; ++k) { r.ssid[k] = text[value+k]; }
                r.ssid_len = (uint8_t)value_len;
            } else { r.ssid_len = 0u; }
        } else if (key_is(text + start, key_len, "password") ||
                   key_is(text + start, key_len, "psk")) {
            have_password = true;
            bool ok = value_len >= 8u && value_len <= 63u;
            for (size_t k = 0; ok && k < value_len; ++k) {
                const uint8_t c = (uint8_t)text[value+k];
                if (c < 0x20u || c > 0x7eu) { ok = false; }
            }
            if (ok) {
                for (size_t k = 0; k < value_len; ++k) { r.password[k] = text[value+k]; }
                r.password_len = (uint8_t)value_len;
            } else { r.password_len = 0u; }
        } else if (key_is(text + start, key_len, "country")) {
            const char a = value_len == 2u ? lower(text[value]) : '\0';
            const char b = value_len == 2u ? lower(text[value+1u]) : '\0';
            if (a >= 'a' && a <= 'z' && b >= 'a' && b <= 'z') {
                r.country[0] = (char)(a - 'a' + 'A');
                r.country[1] = (char)(b - 'a' + 'A');
                r.country_set = true; bad_country = false;
            } else { r.country_set = false; bad_country = true; }
        }
    }
    bootcfg_status_t result = BOOTCFG_OK;
    if (!have_ssid) { result = BOOTCFG_NO_SSID; }
    else if (r.ssid_len == 0u) { result = BOOTCFG_BAD_SSID; }
    else if (!have_password || r.password_len == 0u) { result = BOOTCFG_BAD_PASSWORD; }
    else if (bad_country) { result = BOOTCFG_BAD_COUNTRY; }
    if (result != BOOTCFG_OK) {
        for (size_t k = 0; k < sizeof(r); ++k) { ((volatile char *)&r)[k] = 0; }
    }
    *out = r;
    for (size_t k = 0; k < sizeof(r); ++k) { ((volatile char *)&r)[k] = 0; }
    return result;
}

static bool find_json_bounds(const char *raw, size_t size, size_t *out_start, size_t *out_end)
{
    bool line_clean = true;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = (uint8_t)raw[i];
        if (c == '\n' || c == '\r') { line_clean = true; continue; }
        if (c == '\0' || c == ' ' || c == '\t' || c == 0xefu || c == 0xbbu || c == 0xbfu) { continue; }
        if (c == '{' && line_clean) {
            size_t k = i + 1u;
            while (k < size && (raw[k] == ' ' || raw[k] == '\t' || raw[k] == '\r' || raw[k] == '\n')) { ++k; }
            if (k < size && raw[k] == '"') {
                bool in_str = false, escape = false;
                int32_t depth = 0;
                for (size_t j = i; j < size; ++j) {
                    const char ch = raw[j];
                    if (in_str) {
                        if (escape) { escape = false; }
                        else if (ch == '\\') { escape = true; }
                        else if (ch == '"') { in_str = false; }
                    } else {
                        if (ch == '"') { in_str = true; }
                        else if (ch == '{') { ++depth; }
                        else if (ch == '}') {
                            --depth;
                            if (depth == 0) {
                                *out_start = i;
                                *out_end = j + 1u;
                                return true;
                            }
                        }
                    }
                }
                return false;
            }
        }
        line_clean = false;
    }
    return false;
}

enum { ATAGS_ABSENT = 0, ATAGS_NO_INITRD, ATAGS_FOUND };

static int find_initrd(uint32_t *address, uint32_t *size)
{
    const volatile uint32_t *tag = (const volatile uint32_t *)(uintptr_t)ATAG_BASE;
    __asm__ volatile ("" : "+r" (tag));
    if (tag[1] != ATAG_CORE) { return ATAGS_ABSENT; }
    for (uint32_t guard = 0; guard < 64u; ++guard) {
        const uint32_t words = tag[0], id = tag[1];
        if (words < 2u || words > 256u || id == 0u) { return ATAGS_NO_INITRD; }
        if (id == ATAG_INITRD2 && words >= 4u) {
            *address = tag[2]; *size = tag[3];
            return ATAGS_FOUND;
        }
        tag += words;
    }
    return ATAGS_NO_INITRD;
}

void bootcfg_init(void)
{
    uint32_t address = 0u, size = 0u;
    if (status != BOOTCFG_NOT_TRIED) { return; }
    const int found = find_initrd(&address, &size);
    if (found != ATAGS_FOUND && soc_is_bcm2837 != 0u) {
        /* On Pi Zero 2 W, if firmware booted without ATAG_INITRD2 at 0x100,
         * check the fixed initramfs load address 0x02000000 directly. */
        const char *probe = (const char *)(uintptr_t)INITRD_FIXED;
        size_t probe_len = 0u;
        while (probe_len < BOOTCFG_MAX_BYTES && probe[probe_len] != '\0' && (uint8_t)probe[probe_len] != 0xffu) {
            ++probe_len;
        }
        if (probe_len >= 8u) {
            address = INITRD_FIXED;
            size = (uint32_t)probe_len;
        } else {
            status = (found == ATAGS_ABSENT) ? BOOTCFG_NO_ATAGS : BOOTCFG_NO_FILE;
            return;
        }
    } else if (found == ATAGS_ABSENT) {
        status = BOOTCFG_NO_ATAGS;
        return;
    } else if (found == ATAGS_NO_INITRD) {
        status = BOOTCFG_NO_FILE;
        return;
    }
    if (size == 0u || size > BOOTCFG_MAX_BYTES || address < RANGE_MIN ||
        (address & 3u) != 0u || address >= RAM_LIMIT || size > RAM_LIMIT - address) {
        status = BOOTCFG_BAD_RANGE; return;
    }

    const char *raw = (const char *)(uintptr_t)address;
    size_t json_start = 0u, json_end = 0u;

    if (find_json_bounds(raw, size, &json_start, &json_end)) {
        if (json_end - json_start > 20u) {
            project_json_ptr = raw + json_start;
            project_json_size = json_end - json_start;
        }
        status = BOOTCFG_NO_SSID;
        if (json_start > 0u) {
            status = bootcfg_parse_wifi(raw, json_start, &wifi);
        }
        if (status == BOOTCFG_NO_SSID && json_end < size) {
            status = bootcfg_parse_wifi(raw + json_end, size - json_end, &wifi);
        }
    } else {
        status = bootcfg_parse_wifi(raw, size, &wifi);
    }
}

bootcfg_status_t bootcfg_status(void) { return status; }

const wifi_settings_t *bootcfg_wifi(void) { return &wifi; }

const char *bootcfg_project_json(size_t *out_size)
{
    if (out_size) *out_size = project_json_size;
    return project_json_ptr;
}

const char *bootcfg_status_text(bootcfg_status_t s)
{
    switch (s) {
        case BOOTCFG_NOT_TRIED: return "not read";
        case BOOTCFG_OK: return "wifi.txt loaded";
        case BOOTCFG_NO_ATAGS: return "no boot tags; using built-in Wi-Fi settings";
        case BOOTCFG_NO_FILE: return "initramfs not loaded; using built-in settings";
        case BOOTCFG_BAD_RANGE: return "initramfs address/size invalid";
        case BOOTCFG_NO_SSID: return "no wifi.txt ssid= line (built-in/project mode)";
        case BOOTCFG_BAD_SSID: return "wifi.txt ssid must be 1-32 bytes";
        case BOOTCFG_BAD_PASSWORD: return "wifi.txt password must be 8-63 printable ASCII";
        case BOOTCFG_BAD_COUNTRY: return "wifi.txt country must be two letters";
        default: return "unknown";
    }
}
