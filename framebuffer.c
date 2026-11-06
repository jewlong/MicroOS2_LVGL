/* SPDX-License-Identifier: Apache-2.0 */
#include "framebuffer.h"
#include "mailbox.h"
#include "platform.h"

enum { PHYSICAL=2, VIRTUAL=7, DEPTH=12, ORDER=16, OFFSET=20,
       ALPHA=25, ALLOCATE=29, PITCH=34, END_TAG=38, MESSAGE_WORDS=39 };
/* Static storage is intentional: a GPU may finish writing after a timeout. */
_Alignas(16) static volatile uint32_t fb_message[MESSAGE_WORDS];
static framebuffer_t framebuffer;
static bool ready;
static bool attempted;

static bool tag_ok(uint32_t at, uint32_t id, uint32_t bytes)
{
    return fb_message[at] == id && fb_message[at+1u] == bytes &&
           fb_message[at+2u] == (0x80000000u | bytes);
}

fb_result_t framebuffer_init(void)
{
    if (attempted) { return FB_ALREADY_ATTEMPTED; }
    attempted = true;
    fb_message[0] = sizeof(fb_message);
    fb_message[1] = 0u;
    fb_message[PHYSICAL] = 0x48003u;
    fb_message[PHYSICAL+1] = 8u;
    fb_message[PHYSICAL+3] = FB_WIDTH;
    fb_message[PHYSICAL+4] = FB_HEIGHT;
    fb_message[VIRTUAL] = 0x48004u;
    fb_message[VIRTUAL+1] = 8u;
    fb_message[VIRTUAL+3] = FB_WIDTH;
    fb_message[VIRTUAL+4] = FB_HEIGHT;
    fb_message[DEPTH] = 0x48005u;
    fb_message[DEPTH+1] = 4u;
    fb_message[DEPTH+3] = 32u;
    fb_message[ORDER] = 0x48006u;
    fb_message[ORDER+1] = 4u;
    fb_message[ORDER+3] = 1u; /* RGB in memory; red in low 8 bits. */
    fb_message[OFFSET] = 0x48009u;
    fb_message[OFFSET+1] = 8u; /* x/y offset remain zero from BSS. */
    fb_message[ALPHA] = 0x48007u;
    fb_message[ALPHA+1] = 4u;
    fb_message[ALPHA+3] = 2u; /* Ignore alpha. */
    fb_message[ALLOCATE] = 0x40001u;
    fb_message[ALLOCATE+1] = 8u;
    fb_message[ALLOCATE+3] = 4096u;
    fb_message[PITCH] = 0x40008u;
    fb_message[PITCH+1] = 4u;
    fb_message[END_TAG] = 0u;
    const mailbox_result_t transport =
        mailbox_property_call(fb_message, sizeof(fb_message), 1000000u);
    if (transport == MAILBOX_BAD_ARGUMENT) { return FB_MAILBOX_ARGUMENT; }
    if (transport == MAILBOX_WRITE_TIMEOUT) { return FB_MAILBOX_FULL; }
    if (transport != MAILBOX_OK) { return FB_MAILBOX_TIMEOUT; }
    if (fb_message[0] != sizeof(fb_message) || fb_message[1] != 0x80000000u ||
        !tag_ok(PHYSICAL,0x48003u,8u) || !tag_ok(VIRTUAL,0x48004u,8u) ||
        !tag_ok(DEPTH,0x48005u,4u) || !tag_ok(ORDER,0x48006u,4u) ||
        !tag_ok(OFFSET,0x48009u,8u) || !tag_ok(ALPHA,0x48007u,4u) ||
        !tag_ok(ALLOCATE,0x40001u,8u) || !tag_ok(PITCH,0x40008u,4u) ||
        fb_message[END_TAG] != 0u) {
        return FB_BAD_RESPONSE;
    }
    if (fb_message[PHYSICAL+3] != FB_WIDTH || fb_message[PHYSICAL+4] != FB_HEIGHT ||
        fb_message[VIRTUAL+3] != FB_WIDTH || fb_message[VIRTUAL+4] != FB_HEIGHT ||
        fb_message[OFFSET+3] != 0u || fb_message[OFFSET+4] != 0u) {
        return FB_BAD_GEOMETRY;
    }
    if (fb_message[DEPTH+3] != 32u || fb_message[ORDER+3] > 1u ||
        fb_message[ALPHA+3] != 2u) {
        return FB_BAD_FORMAT;
    }
    /* Allocation returns a VC bus address; CPU stores use its RAM physical alias. */
    const uint32_t address = fb_message[ALLOCATE+3] & 0x3fffffffu;
    const uint32_t size = fb_message[ALLOCATE+4];
    const uint32_t pitch = fb_message[PITCH+3];
    if (pitch < FB_WIDTH*4u || pitch > 16384u || (pitch & 3u) != 0u ||
        size < pitch*FB_HEIGHT || address < 0x01000000u ||
        (address & 4095u) != 0u || address >= 0x20000000u ||
        size > 0x20000000u-address) {
        return FB_BAD_ALLOCATION;
    }
    framebuffer.pixels = (volatile uint32_t *)(uintptr_t)address;
    framebuffer.width = FB_WIDTH;
    framebuffer.height = FB_HEIGHT;
    framebuffer.pitch = pitch;
    framebuffer.size = size;
    framebuffer.pixel_order = fb_message[ORDER+3];
    arm_dmb();
    ready = true; /* Publish only after the entire response is validated. */
    return FB_OK;
}

const framebuffer_t *framebuffer_info(void) { return &framebuffer; }
bool framebuffer_ready(void) { return ready; }

const char *framebuffer_error(fb_result_t result)
{
    switch (result) {
        case FB_OK: return "OK";
        case FB_ALREADY_ATTEMPTED: return "already attempted; cold boot to retry";
        case FB_MAILBOX_ARGUMENT: return "mailbox argument or MMU/cache state";
        case FB_MAILBOX_FULL: return "mailbox full timeout";
        case FB_MAILBOX_TIMEOUT: return "mailbox reply timeout";
        case FB_BAD_RESPONSE: return "invalid property response/tag";
        case FB_BAD_GEOMETRY: return "firmware changed geometry/offset";
        case FB_BAD_FORMAT: return "unsupported depth/order/alpha";
        case FB_BAD_ALLOCATION: return "invalid address/size/pitch";
        default: return "unknown framebuffer error";
    }
}

static uint32_t pack_color(uint32_t rgb)
{
    rgb &= 0x00ffffffu;
    if (framebuffer.pixel_order == 1u) {
        return 0xff000000u | ((rgb & 0xff0000u) >> 16) |
               (rgb & 0x00ff00u) | ((rgb & 0x0000ffu) << 16);
    }
    return 0xff000000u | rgb;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!ready || x >= framebuffer.width || y >= framebuffer.height) { return; }
    if (w > framebuffer.width-x) { w = framebuffer.width-x; }
    if (h > framebuffer.height-y) { h = framebuffer.height-y; }
    const uint32_t color = pack_color(rgb);
    for (uint32_t row = 0; row < h; ++row) {
        volatile uint32_t *dst = (volatile uint32_t *)((uintptr_t)framebuffer.pixels +
                                     (y+row)*framebuffer.pitch + x*4u);
        for (uint32_t col = 0; col < w; ++col) { dst[col] = color; }
    }
}

/* Original compact 5x7 glyphs, uppercase ASCII demo subset. */
typedef struct { char ch; uint8_t rows[7]; } glyph_t;
static const glyph_t font[] = {
    {'?', {14u,17u,1u,2u,4u,0u,4u}},
    {' ', {0u,0u,0u,0u,0u,0u,0u}},
    {'0', {14u,17u,19u,21u,25u,17u,14u}},
    {'1', {4u,12u,4u,4u,4u,4u,14u}},
    {'2', {14u,17u,1u,2u,4u,8u,31u}},
    {'3', {30u,1u,1u,14u,1u,1u,30u}},
    {'4', {2u,6u,10u,18u,31u,2u,2u}},
    {'5', {31u,16u,16u,30u,1u,1u,30u}},
    {'6', {14u,16u,16u,30u,17u,17u,14u}},
    {'7', {31u,1u,2u,4u,8u,8u,8u}},
    {'8', {14u,17u,17u,14u,17u,17u,14u}},
    {'9', {14u,17u,17u,15u,1u,1u,14u}},
    {'A', {14u,17u,17u,31u,17u,17u,17u}},
    {'B', {30u,17u,17u,30u,17u,17u,30u}},
    {'C', {15u,16u,16u,16u,16u,16u,15u}},
    {'D', {30u,17u,17u,17u,17u,17u,30u}},
    {'E', {31u,16u,16u,30u,16u,16u,31u}},
    {'F', {31u,16u,16u,30u,16u,16u,16u}},
    {'G', {15u,16u,16u,23u,17u,17u,15u}},
    {'H', {17u,17u,17u,31u,17u,17u,17u}},
    {'I', {31u,4u,4u,4u,4u,4u,31u}},
    {'J', {7u,2u,2u,2u,18u,18u,12u}},
    {'K', {17u,18u,20u,24u,20u,18u,17u}},
    {'L', {16u,16u,16u,16u,16u,16u,31u}},
    {'M', {17u,27u,21u,21u,17u,17u,17u}},
    {'N', {17u,25u,21u,19u,17u,17u,17u}},
    {'O', {14u,17u,17u,17u,17u,17u,14u}},
    {'P', {30u,17u,17u,30u,16u,16u,16u}},
    {'Q', {14u,17u,17u,17u,21u,18u,13u}},
    {'R', {30u,17u,17u,30u,20u,18u,17u}},
    {'S', {15u,16u,16u,14u,1u,1u,30u}},
    {'T', {31u,4u,4u,4u,4u,4u,4u}},
    {'U', {17u,17u,17u,17u,17u,17u,14u}},
    {'V', {17u,17u,17u,17u,17u,10u,4u}},
    {'W', {17u,17u,17u,21u,21u,27u,17u}},
    {'X', {17u,17u,10u,4u,10u,17u,17u}},
    {'Y', {17u,17u,10u,4u,4u,4u,4u}},
    {'Z', {31u,1u,2u,4u,8u,16u,31u}},
    {':', {0u,4u,4u,0u,4u,4u,0u}},
    {'/', {1u,1u,2u,4u,8u,16u,16u}},
    {'-', {0u,0u,0u,31u,0u,0u,0u}},
    {'+', {0u,4u,4u,31u,4u,4u,0u}}
};

void fb_draw_text(uint32_t x, uint32_t y, const char *text,
                  uint32_t rgb, uint32_t scale)
{
    if (!ready || scale == 0u || scale > 4u || text == (void *)0 ||
        x >= framebuffer.width || y >= framebuffer.height) { return; }
    while (*text != '\0' && x < framebuffer.width) {
        char ch = *text++;
        if (ch >= 'a' && ch <= 'z') { ch = (char)(ch-'a'+'A'); }
        const glyph_t *glyph = &font[0]; /* Unknown characters use '?'. */
        for (uint32_t i = 0; i < sizeof(font)/sizeof(font[0]); ++i) {
            if (font[i].ch == ch) { glyph = &font[i]; break; }
        }
        for (uint32_t row = 0; row < 7u; ++row) {
            for (uint32_t col = 0; col < 5u; ++col) {
                if ((glyph->rows[row] & (1u << (4u-col))) != 0u) {
                    fb_fill_rect(x+col*scale,y+row*scale,scale,scale,rgb);
                }
            }
        }
        x += 6u*scale;
    }
}

void fb_draw_hex32(uint32_t x, uint32_t y, uint32_t value,
                   uint32_t rgb, uint32_t scale)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[11];
    text[0]='0'; text[1]='X';
    for (uint32_t i=0; i<8u; ++i) {
        text[i+2u]=digits[(value >> 28) & 15u]; value <<= 4;
    }
    text[10]='\0';
    fb_draw_text(x,y,text,rgb,scale);
}

void fb_present(void)
{
    const uint32_t zero = 0;
    /* Complete CPU framebuffer stores before the display engine observes them.
     * No cache maintenance is needed ONLY because D-cache stays disabled.
     * This is not a vblank/page-flip operation: single-buffer tearing is possible.
     */
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (zero) : "memory");
}
