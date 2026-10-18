// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
// A software version of overlay.h for the host, so the screens can be rendered to a picture (tools/ui_preview). Same atlas, same maths as
// the shader in loader/src/overlay.c (only meant for previews: it is slow and not exact).
#include "overlay.h"
#include "overlay_assets.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define W 1920
#define H 1080
float *sw_canvas;  // RGB floats 0..1, W*H*3
static unsigned char *atlas;
static float clip_x0, clip_y0, clip_x1, clip_y1;

struct OverlayImage {
    unsigned width, height;
    uint32_t *pixels;
};
bool overlayBegin(unsigned w, unsigned h) {
    (void)w; (void)h;
    if (!sw_canvas) sw_canvas = calloc((size_t)W * H * 3, sizeof(float));
    if (!atlas) {
        uLongf n = (uLongf)overlay_font_width * overlay_font_height;
        atlas = malloc(n);
        uncompress(atlas, &n, overlay_font_alpha, overlay_font_alpha_size);
    }
    clip_x0 = clip_y0 = 0; clip_x1 = W; clip_y1 = H;
    return true;
}
void overlayEnd(void) {}
bool overlayRoundedAvailable(void) { return true; }
void overlayContextLost(void) {}
bool overlayPictureHasContent(unsigned w, unsigned h) { (void)w; (void)h; return true; }
void overlayLogo(float x, float y, float h) { (void)x; (void)y; (void)h; }

static void blend(int x, int y, uint32_t rgba, float coverage) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1 || x < 0 || y < 0 || x >= W || y >= H) return;
    float a = (float)(rgba & 255) / 255.0f * coverage;
    float *p = sw_canvas + ((size_t)y * W + x) * 3;
    float c[3] = {(float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f, (float)((rgba >> 8) & 255) / 255.0f};
    for (int i = 0; i < 3; ++i) p[i] = p[i] * (1 - a) + c[i] * a;
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float v = (float)((a >> s) & 255) * (1 - t) + (float)((b >> s) & 255) * t;
        r |= (uint32_t)(v + 0.5f) << s;
    }
    return r;
}
static float shape(float px, float py, float x, float y, float w, float h, float radius, float thickness) {
    if (radius < 0) return 1;
    float hw = w / 2, hh = h / 2, r = fminf(radius, fminf(hw, hh));
    float qx = fabsf(px - (x + hw)) - hw + r, qy = fabsf(py - (y + hh)) - hh + r;
    float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
    float a = fminf(fmaxf(0.5f - d, 0), 1);
    if (thickness > 0) a *= fminf(fmaxf(0.5f + (d + thickness), 0), 1);
    return a;
}
static void fillRect(float x, float y, float w, float h, float radius, float thickness, uint32_t top, uint32_t bottom) {
    for (int py = (int)floorf(y) - 1; py < (int)ceilf(y + h) + 1; ++py)
        for (int px = (int)floorf(x) - 1; px < (int)ceilf(x + w) + 1; ++px) {
            float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
            if (radius < 0 && (cx < x || cx > x + w || cy < y || cy > y + h)) continue;
            float cov = shape(cx, cy, x, y, w, h, radius, thickness);
            if (cov <= 0) continue;
            blend(px, py, mix(top, bottom, fminf(fmaxf((cy - y) / h, 0), 1)), cov);
        }
}
void overlayClear(uint32_t c) {
    for (int i = 0; i < W * H; ++i) {
        sw_canvas[i * 3] = (float)(c >> 24) / 255.0f;
        sw_canvas[i * 3 + 1] = (float)((c >> 16) & 255) / 255.0f;
        sw_canvas[i * 3 + 2] = (float)((c >> 8) & 255) / 255.0f;
    }
}
void overlayRect(float x, float y, float w, float h, uint32_t c) { fillRect(x, y, w, h, -1, 0, c, c); }
void overlayFrame(float x, float y, float w, float h, float t, uint32_t c) {
    overlayRect(x, y, w, t, c); overlayRect(x, y + h - t, w, t, c); overlayRect(x, y + t, t, h - 2 * t, c); overlayRect(x + w - t, y + t, t, h - 2 * t, c);
}
void overlayRoundRect(float x, float y, float w, float h, float r, uint32_t c) { fillRect(x, y, w, h, r, 0, c, c); }
void overlayRoundFrame(float x, float y, float w, float h, float r, float t, uint32_t c) { fillRect(x, y, w, h, r, t, c, c); }
void overlayCircle(float cx, float cy, float r, uint32_t c) { fillRect(cx - r, cy - r, 2 * r, 2 * r, r, 0, c, c); }
void overlayRectGradient(float x, float y, float w, float h, uint32_t a, uint32_t b) { fillRect(x, y, w, h, -1, 0, a, b); }
void overlayRoundGradient(float x, float y, float w, float h, float r, uint32_t a, uint32_t b) { fillRect(x, y, w, h, r, 0, a, b); }
void overlayClip(float x, float y, float w, float h) { clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h; }
void overlayClipEnd(void) { clip_x0 = clip_y0 = 0; clip_x1 = W; clip_y1 = H; }

static float sampleAtlas(float u, float v) {  // pixel coordinates in the atlas, bilinear
    float fx = u - 0.5f, fy = v - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0, s = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int x = x0 + i, y = y0 + j;
            x = x < 0 ? 0 : x >= (int)overlay_font_width ? (int)overlay_font_width - 1 : x;
            y = y < 0 ? 0 : y >= (int)overlay_font_height ? (int)overlay_font_height - 1 : y;
            s += (float)atlas[(size_t)y * overlay_font_width + (size_t)x] / 255.0f * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
        }
    return s;
}
static void atlasQuad(float x0, float y0, float x1, float y1, float sx, float sy, float sw, float sh, uint32_t c) {
    for (int py = (int)floorf(y0); py < (int)ceilf(y1); ++py)
        for (int px = (int)floorf(x0); px < (int)ceilf(x1); ++px) {
            float u = sx + ((float)px + 0.5f - x0) / (x1 - x0) * sw, v = sy + ((float)py + 0.5f - y0) / (y1 - y0) * sh;
            float cov = sampleAtlas(u, v);
            if (cov > 0.002f) blend(px, py, c, cov);
        }
}
static const OverlayGlyph *findGlyph(unsigned key) {
    for (unsigned i = 0; i < overlay_glyph_count; ++i)
        if (overlay_glyph_codes[i] == key) return &overlay_glyphs[i];
    return NULL;
}
static unsigned nextCode(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    unsigned c = *p++;
    if (c >= 0xC2 && c < 0xE0) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    else if (c >= 0xE0 && c < 0xF0) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xF0) { c = '?'; p += 3; }
    *cursor = p;
    return c;
}
static float run(float x, float y, const char *text, float size, uint32_t c, unsigned style, bool draw) {
    float scale = size / (float)overlay_font_line, pen = x;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        unsigned code = nextCode(&p);
        const OverlayGlyph *g = findGlyph((style << 24) | code);
        if (!g && style) g = findGlyph(code);
        if (!g) g = findGlyph('?');
        if (draw && code != ' ') {
            float x0 = pen - g->bearing * scale;
            atlasQuad(x0, y, x0 + (float)g->width * scale, y + (float)g->height * scale, (float)g->x, (float)g->y, (float)g->width, (float)g->height, c);
        }
        pen += g->advance * scale;
    }
    return pen - x;
}
float overlayText(float x, float y, const char *t, float s, uint32_t c) { return run(x, y, t, s, c, 0, true); }
float overlayTextBold(float x, float y, const char *t, float s, uint32_t c) { return run(x, y, t, s, c, 1, true); }
float overlayTextWidth(const char *t, float s) { return run(0, 0, t, s, 0, 0, false); }
float overlayTextBoldWidth(const char *t, float s) { return run(0, 0, t, s, 0, 1, false); }
float overlayTextCentered(float cx, float y, const char *t, float s, uint32_t c) { return overlayText(cx - overlayTextWidth(t, s) / 2, y, t, s, c); }
static void fit(float x, float y, const char *text, float size, float max, uint32_t c, unsigned style) {
    if (run(0, 0, text, size, 0, style, false) <= max) { run(x, y, text, size, c, style, true); return; }
    char line[512];
    size_t n = strlen(text);
    if (n > 500) n = 500;
    memcpy(line, text, n);
    for (; n; --n) {
        if ((line[n] & 0xC0) == 0x80) continue;
        memcpy(line + n, "...", 4);
        if (run(0, 0, line, size, 0, style, false) <= max) break;
    }
    run(x, y, line, size, c, style, true);
}
void overlayTextFit(float x, float y, const char *t, float s, float m, uint32_t c) { fit(x, y, t, s, m, c, 0); }
void overlayTextFitBold(float x, float y, const char *t, float s, float m, uint32_t c) { fit(x, y, t, s, m, c, 1); }
void overlayIcon(unsigned icon, float cx, float cy, float size, uint32_t c) {
    const OverlayGlyph *g = findGlyph((2u << 24) | icon);
    if (!g) return;
    atlasQuad(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2, (float)g->x, (float)g->y, (float)g->width, (float)g->height, c);
}
OverlayImage *overlayImageCreate(unsigned w, unsigned h, const uint32_t *rgba) {
    OverlayImage *i = calloc(1, sizeof(*i));
    i->width = w; i->height = h;
    i->pixels = malloc((size_t)w * h * 4);
    memcpy(i->pixels, rgba, (size_t)w * h * 4);
    return i;
}
void overlayImageDestroy(OverlayImage *i) { if (i) { free(i->pixels); free(i); } }
void overlayImageDraw(OverlayImage *img, float x, float y, float w, float h, float radius, uint32_t tint) {
    if (!img) return;
    for (int py = (int)floorf(y) - 1; py < (int)ceilf(y + h) + 1; ++py)
        for (int px = (int)floorf(x) - 1; px < (int)ceilf(x + w) + 1; ++px) {
            float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
            if (cx < x || cx > x + w || cy < y || cy > y + h) { if (radius <= 0) continue; }
            float cov = radius > 0 ? shape(cx, cy, x, y, w, h, radius, 0) : 1;
            if (cov <= 0) continue;
            int ix = (int)fminf(fmaxf((cx - x) / w * (float)img->width, 0), (float)img->width - 1);
            int iy = (int)fminf(fmaxf((cy - y) / h * (float)img->height, 0), (float)img->height - 1);
            uint32_t p = img->pixels[(size_t)iy * img->width + (size_t)ix];  // R,G,B,A in memory order, little endian: A B G R
            uint32_t rgba = ((p & 255) << 24) | (((p >> 8) & 255) << 16) | (((p >> 16) & 255) << 8) | (p >> 24);
            float tr = (float)(tint >> 24) / 255.0f, tg = (float)((tint >> 16) & 255) / 255.0f, tb = (float)((tint >> 8) & 255) / 255.0f, ta = (float)(tint & 255) / 255.0f;
            uint32_t r = (uint32_t)(((rgba >> 24) & 255) * tr) << 24 | (uint32_t)(((rgba >> 16) & 255) * tg) << 16 | (uint32_t)(((rgba >> 8) & 255) * tb) << 8 | (uint32_t)((rgba & 255) * ta);
            blend(px, py, r, cov);
        }
}
