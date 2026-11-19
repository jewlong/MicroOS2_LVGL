/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOTCFG_H
#define BOOTCFG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* wifi.txt / project.json on the SD card is loaded into RAM by the GPU firmware
 * (config.txt: "initramfs project.json,wifi.txt 0x02000000"). The firmware reports
 * address and size in an ATAG_INITRD2 record at 0x100; no SD/FAT driver is needed. */
#define BOOTCFG_LOAD_ADDRESS 0x02000000u
#define BOOTCFG_MAX_BYTES    (2u * 1024u * 1024u)

typedef enum {
    BOOTCFG_NOT_TRIED = 0,
    BOOTCFG_OK,
    BOOTCFG_NO_ATAGS,      /* No ATAG list at 0x100. */
    BOOTCFG_NO_FILE,       /* ATAGs exist but no initramfs record. */
    BOOTCFG_BAD_RANGE,     /* Address/size outside the accepted window. */
    BOOTCFG_NO_SSID,       /* File parsed but has no ssid= line. */
    BOOTCFG_BAD_SSID,      /* SSID not 1-32 bytes. */
    BOOTCFG_BAD_PASSWORD,  /* Missing, or not 8-63 printable ASCII characters. */
    BOOTCFG_BAD_COUNTRY    /* country= is not two letters. */
} bootcfg_status_t;

typedef struct {
    char ssid[33];
    char password[64];
    uint8_t ssid_len, password_len;
    char country[2];
    bool country_set;
    int8_t tz_offset_hours;
    bool tz_offset_set;
    char ntp_server[64];
    bool ntp_server_set;
} wifi_settings_t;

/* Pure parser over a text buffer (host-testable). On failure *out is zeroed. */
bootcfg_status_t bootcfg_parse_wifi(const char *text, size_t length, wifi_settings_t *out);
/* Call once, as early as possible in main(), before anything reuses low RAM. */
void bootcfg_init(void);
bootcfg_status_t bootcfg_status(void);
/* Valid only while bootcfg_status() == BOOTCFG_OK. */
const wifi_settings_t *bootcfg_wifi(void);
const char *bootcfg_status_text(bootcfg_status_t status);
/* Returns pointer and size of ViewGP Project JSON if present in initramfs */
const char *bootcfg_project_json(size_t *out_size);

#endif
