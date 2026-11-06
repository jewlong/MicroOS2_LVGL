/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include <stdint.h>
#include <stdbool.h>
#define FB_WIDTH  640u
#define FB_HEIGHT 480u
typedef enum {
    FB_OK = 0, FB_ALREADY_ATTEMPTED, FB_MAILBOX_ARGUMENT, FB_MAILBOX_FULL,
    FB_MAILBOX_TIMEOUT, FB_BAD_RESPONSE, FB_BAD_GEOMETRY, FB_BAD_FORMAT,
    FB_BAD_ALLOCATION
} fb_result_t;
typedef struct {
    volatile uint32_t *pixels;
    uint32_t width, height, pitch, size, pixel_order;
} framebuffer_t;

/* Call once before OSStart; no retry after timeout (GPU may still own message). */
fb_result_t framebuffer_init(void);
const framebuffer_t *framebuffer_info(void);
bool framebuffer_ready(void);
const char *framebuffer_error(fb_result_t result);
/* Single drawing owner; color arguments use canonical 0x00RRGGBB. */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_draw_text(uint32_t x, uint32_t y, const char *text,
                  uint32_t rgb, uint32_t scale);
void fb_draw_hex32(uint32_t x, uint32_t y, uint32_t value,
                   uint32_t rgb, uint32_t scale);
void fb_present(void);
#endif
