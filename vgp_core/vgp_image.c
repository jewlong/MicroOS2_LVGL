/* SPDX-License-Identifier: Apache-2.0 */
#include "vgp_image.h"
#include "vgp_platform.h"
#if defined(__has_include) && __has_include("src/libs/tjpgd/tjpgd.h")
#include "src/libs/tjpgd/tjpgd.h"
#include "src/libs/lodepng/lodepng.h"
#else
#include "libs/tjpgd/tjpgd.h"
#include "libs/lodepng/lodepng.h"
#endif
#include <string.h>

#define VGP_IMG_MAX_PIXELS (640u * 480u)
#define VGP_IMG_MAX_CACHE  (VGP_MAX_ASSETS + 8u)

typedef struct {
    bool valid;
    bool is_inline;
    char asset_id[VGP_MAX_ID_LEN];
    uint16_t element_idx;
    lv_image_dsc_t dsc;
    uint8_t *pixels;
} vgp_img_cache_entry_t;

static VGP_BSS_ATTR vgp_img_cache_entry_t g_cache[VGP_IMG_MAX_CACHE];
static uint16_t g_cache_count = 0u;
static VGP_BSS_ATTR uint8_t g_tjpgd_work[4096];

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t base64_decode(const char *src, size_t src_len, uint8_t *dst, size_t max_dst)
{
    uint32_t acc = 0u;
    int bits = 0;
    size_t out = 0u;
    for (size_t i = 0; i < src_len; ++i) {
        char c = src[i];
        if (c == '=') break;
        if (c == '\\' && i + 1 < src_len && src[i + 1] == '/') {
            c = '/';
            i++;
        }
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out < max_dst) {
                dst[out++] = (uint8_t)((acc >> bits) & 0xFFu);
            } else {
                break;
            }
        }
    }
    return out;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint32_t *out_pixels;
    uint16_t width;
    uint16_t height;
} jpg_ctx_t;

static size_t jpg_in_func(JDEC *jd, uint8_t *buff, size_t ndata)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jd->device;
    size_t rem = (ctx->pos < ctx->size) ? (ctx->size - ctx->pos) : 0u;
    size_t n = (ndata < rem) ? ndata : rem;
    if (buff && n > 0u) {
        memcpy(buff, ctx->data + ctx->pos, n);
    }
    ctx->pos += n;
    return n;
}

static int jpg_out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    uint16_t rw = (uint16_t)(rect->right - rect->left + 1u);
    for (uint16_t y = rect->top; y <= rect->bottom; ++y) {
        if (y >= ctx->height) break;
        uint32_t *dst_row = ctx->out_pixels + (uint32_t)y * ctx->width + rect->left;
        for (uint16_t x = 0u; x < rw; ++x) {
            if ((uint16_t)(rect->left + x) >= ctx->width) break;
            uint8_t r = src[0];
            uint8_t g = src[1];
            uint8_t b = src[2];
            dst_row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            src += 3;
        }
    }
    return 1;
}

static bool decode_jpeg_to_entry(const uint8_t *raw, size_t raw_len, vgp_img_cache_entry_t *entry)
{
    JDEC jdec;
    jpg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data = raw;
    ctx.size = raw_len;

    if (jd_prepare(&jdec, jpg_in_func, g_tjpgd_work, sizeof(g_tjpgd_work), &ctx) != JDR_OK) {
        return false;
    }
    uint32_t npix = (uint32_t)jdec.width * (uint32_t)jdec.height;
    if (jdec.width == 0u || jdec.height == 0u || npix > VGP_IMG_MAX_PIXELS) {
        return false;
    }

    uint32_t byte_size = npix * 4u;
    uint8_t *pixels = (uint8_t *)lv_malloc(byte_size);
    if (!pixels) return false;

    ctx.out_pixels = (uint32_t *)(void *)pixels;
    ctx.width = jdec.width;
    ctx.height = jdec.height;

    if (jd_decomp(&jdec, jpg_out_func, 0u) != JDR_OK) {
        lv_free(pixels);
        return false;
    }

    memset(&entry->dsc, 0, sizeof(entry->dsc));
    entry->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    entry->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    entry->dsc.header.w = jdec.width;
    entry->dsc.header.h = jdec.height;
    entry->dsc.header.stride = (uint32_t)jdec.width * 4u;
    entry->dsc.data_size = byte_size;
    entry->dsc.data = pixels;
    entry->pixels = pixels;
    entry->valid = true;
    return true;
}

static bool decode_png_to_entry(const uint8_t *raw, size_t raw_len, vgp_img_cache_entry_t *entry)
{
    unsigned char *rgba = NULL;
    unsigned w = 0u, h = 0u;
    unsigned err = lodepng_decode32(&rgba, &w, &h, raw, raw_len);
    if (err != 0u || !rgba) {
        if (rgba) lv_free(rgba);
        return false;
    }
    uint32_t npix = (uint32_t)w * (uint32_t)h;
    if (w == 0u || h == 0u || npix > VGP_IMG_MAX_PIXELS) {
        lv_free(rgba);
        return false;
    }
    /* Convert RGBA byte order to LVGL ARGB8888 little-endian (B, G, R, A) in-place */
    for (uint32_t i = 0; i < npix; ++i) {
        uint8_t *p = rgba + i * 4u;
        uint8_t r = p[0];
        uint8_t b = p[2];
        p[0] = b;
        p[2] = r;
    }

    memset(&entry->dsc, 0, sizeof(entry->dsc));
    entry->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    entry->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    entry->dsc.header.w = w;
    entry->dsc.header.h = h;
    entry->dsc.header.stride = (uint32_t)w * 4u;
    entry->dsc.data_size = npix * 4u;
    entry->dsc.data = rgba;
    entry->pixels = rgba;
    entry->valid = true;
    return true;
}

static bool decode_b64_to_entry(const char *b64, size_t b64_len, bool is_png, vgp_img_cache_entry_t *entry)
{
    if (!b64 || b64_len < 16u) return false;
    size_t max_raw = (b64_len * 3u) / 4u + 4u;
    if (max_raw > 512u * 1024u) return false;

    uint8_t *raw = (uint8_t *)lv_malloc(max_raw);
    if (!raw) return false;
    size_t raw_len = base64_decode(b64, b64_len, raw, max_raw);

    bool ok = false;
    if (raw_len >= 8u) {
        if (is_png) ok = decode_png_to_entry(raw, raw_len, entry);
        else ok = decode_jpeg_to_entry(raw, raw_len, entry);
    }
    lv_free(raw);
    return ok;
}

void vgp_image_init(const vgp_project_t *proj)
{
    for (uint16_t i = 0; i < g_cache_count; ++i) {
        if (g_cache[i].pixels) {
            lv_free(g_cache[i].pixels);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0u;
    if (!proj) return;

    /* 1. Decode project-level assets[] */
    for (uint16_t i = 0; i < proj->num_assets && g_cache_count < VGP_IMG_MAX_CACHE; ++i) {
        const vgp_asset_ref_t *a = &proj->assets[i];
        vgp_img_cache_entry_t *ent = &g_cache[g_cache_count];
        memset(ent, 0, sizeof(*ent));
        if (decode_b64_to_entry(a->b64_ptr, a->b64_len, a->is_png, ent)) {
            strncpy(ent->asset_id, a->id, sizeof(ent->asset_id) - 1u);
            ent->is_inline = false;
            g_cache_count++;
        }
    }

    /* 2. Decode inline element images if any */
    for (uint16_t i = 0; i < proj->total_elements && g_cache_count < VGP_IMG_MAX_CACHE; ++i) {
        const vgp_element_t *el = &proj->elements[i];
        if (el->kind != VGP_KIND_IMAGE || !el->inline_b64_ptr || el->inline_b64_len == 0u) continue;
        vgp_img_cache_entry_t *ent = &g_cache[g_cache_count];
        memset(ent, 0, sizeof(*ent));
        if (decode_b64_to_entry(el->inline_b64_ptr, el->inline_b64_len, el->inline_is_png, ent)) {
            ent->is_inline = true;
            ent->element_idx = i;
            g_cache_count++;
        }
    }
}

const lv_image_dsc_t *vgp_image_get_for_element(const vgp_element_t *el, uint16_t element_idx)
{
    if (!el) return NULL;
    if (el->image_asset_id[0]) {
        for (uint16_t i = 0; i < g_cache_count; ++i) {
            if (g_cache[i].valid && !g_cache[i].is_inline &&
                strcmp(g_cache[i].asset_id, el->image_asset_id) == 0) {
                return &g_cache[i].dsc;
            }
        }
    }
    for (uint16_t i = 0; i < g_cache_count; ++i) {
        if (g_cache[i].valid && g_cache[i].is_inline && g_cache[i].element_idx == element_idx) {
            return &g_cache[i].dsc;
        }
    }
    return NULL;
}
