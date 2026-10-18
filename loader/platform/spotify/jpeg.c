// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
#include "jpeg.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t counts[17];     // codes of each length 1..16
    uint8_t symbols[256];
    int mincode[17], maxcode[18], valptr[17];
    bool present;
} Huffman;
typedef struct {
    unsigned id, h, v, quant, dc_table, ac_table;
    int predictor;
    unsigned blocks_w, blocks_h;  // allocated (MCU-padded) blocks
    uint8_t *pixels;              // blocks_w*8 x blocks_h*8
} Component;
typedef struct {
    const uint8_t *p, *end;
    uint32_t bits;
    int count;
    bool failed;
} Reader;

static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                   41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                   30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static void buildHuffman(Huffman *h) {
    int code = 0, k = 0;
    for (int length = 1; length <= 16; ++length) {
        h->valptr[length] = k;
        h->mincode[length] = code;
        code += h->counts[length];
        k += h->counts[length];
        h->maxcode[length] = h->counts[length] ? code - 1 : -1;
        code <<= 1;
    }
    h->present = true;
}
static int readBit(Reader *r) {
    if (!r->count) {
        if (r->p >= r->end) { r->failed = true; return 0; }
        uint8_t b = *r->p++;
        if (b == 0xFF) {
            if (r->p < r->end && *r->p == 0) ++r->p;
            else { r->failed = true; --r->p; return 0; }  // a marker inside the data: stop
        }
        r->bits = b;
        r->count = 8;
    }
    --r->count;
    return (int)((r->bits >> r->count) & 1);
}
static int readBits(Reader *r, int n) {
    int v = 0;
    while (n--) v = (v << 1) | readBit(r);
    return v;
}
static int decodeSymbol(Reader *r, const Huffman *h) {
    int code = 0;
    for (int length = 1; length <= 16; ++length) {
        code = (code << 1) | readBit(r);
        if (r->failed) return -1;
        if (h->maxcode[length] >= 0 && code <= h->maxcode[length] && code >= h->mincode[length]) return h->symbols[h->valptr[length] + code - h->mincode[length]];
    }
    r->failed = true;
    return -1;
}
static int extend(int v, int bits) { return bits && v < (1 << (bits - 1)) ? v - (1 << bits) + 1 : v; }

static float cosines[8][8];  // cosines[x][u] = c(u) cos((2x+1)u pi / 16) / 2
static void idct(const float *in, uint8_t *out, unsigned stride) {
    float tmp[64];
    for (int y = 0; y < 8; ++y)  // rows
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += cosines[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; ++x)  // columns
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += cosines[y][v] * tmp[v * 8 + x];
            int value = (int)lroundf(s) + 128;
            out[y * stride + x] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
        }
}
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

uint32_t *jpegDecode(const uint8_t *data, size_t size, unsigned *width_out, unsigned *height_out) {
    static bool tables_ready;
    if (!tables_ready) {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) cosines[x][u] = (u ? 1.0f : (float)M_SQRT1_2) * cosf((float)((2 * x + 1) * u) * (float)M_PI / 16.0f) / 2.0f;
        tables_ready = true;
    }
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return NULL;
    uint16_t quant[4][64];
    memset(quant, 0, sizeof(quant));
    Huffman dc[4], ac[4];
    memset(dc, 0, sizeof(dc));
    memset(ac, 0, sizeof(ac));
    Component comps[3];
    memset(comps, 0, sizeof(comps));
    unsigned ncomp = 0, width = 0, height = 0, restart = 0, hmax = 1, vmax = 1;
    uint32_t *result = NULL;
    const uint8_t *p = data + 2, *end = data + size;
    bool decoded = false;
    while (p + 4 <= end && !decoded) {
        if (*p != 0xFF) { ++p; continue; }
        uint8_t marker = p[1];
        if (marker == 0xFF || marker == 0x00) { ++p; continue; }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { p += 2; continue; }
        if (marker == 0xD9) break;
        unsigned length = (unsigned)(p[2] << 8 | p[3]);
        const uint8_t *seg = p + 4, *seg_end = p + 2 + length;
        if (length < 2 || seg_end > end) goto fail;
        if (marker == 0xDB) {
            while (seg < seg_end) {
                unsigned precision = *seg >> 4, id = *seg & 15;
                ++seg;
                if (id > 3 || seg + 64 * (precision + 1) > seg_end) goto fail;
                for (int i = 0; i < 64; ++i) {
                    quant[id][zigzag[i]] = precision ? (uint16_t)(seg[2 * i] << 8 | seg[2 * i + 1]) : seg[i];
                }
                seg += 64 * (precision + 1);
            }
        } else if (marker == 0xC4) {
            while (seg + 17 <= seg_end) {
                unsigned cls = *seg >> 4, id = *seg & 15;
                if (id > 3 || cls > 1) goto fail;
                Huffman *h = cls ? &ac[id] : &dc[id];
                memset(h, 0, sizeof(*h));
                unsigned total = 0;
                for (int i = 1; i <= 16; ++i) { h->counts[i] = seg[i]; total += seg[i]; }
                if (total > 256 || seg + 17 + total > seg_end) goto fail;
                memcpy(h->symbols, seg + 17, total);
                buildHuffman(h);
                seg += 17 + total;
            }
        } else if (marker == 0xC0 || marker == 0xC1) {
            if (seg_end - seg < 6 || seg[0] != 8) goto fail;
            height = (unsigned)(seg[1] << 8 | seg[2]);
            width = (unsigned)(seg[3] << 8 | seg[4]);
            ncomp = seg[5];
            if ((ncomp != 1 && ncomp != 3) || !width || !height || width > 4096 || height > 4096 || seg_end - seg < 6 + 3 * (long)ncomp) goto fail;
            for (unsigned i = 0; i < ncomp; ++i) {
                comps[i].id = seg[6 + 3 * i];
                comps[i].h = seg[7 + 3 * i] >> 4;
                comps[i].v = seg[7 + 3 * i] & 15;
                comps[i].quant = seg[8 + 3 * i];
                if (!comps[i].h || !comps[i].v || comps[i].h > 4 || comps[i].v > 4 || comps[i].quant > 3) goto fail;
                if (comps[i].h > hmax) hmax = comps[i].h;
                if (comps[i].v > vmax) vmax = comps[i].v;
            }
        } else if (marker == 0xC2 || (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)) {
            goto fail;  // progressive, lossless, arithmetic: not handled
        } else if (marker == 0xDD) {
            if (seg_end - seg < 2) goto fail;
            restart = (unsigned)(seg[0] << 8 | seg[1]);
        } else if (marker == 0xDA) {
            if (!ncomp || seg_end - seg < 1 + 2 * (long)ncomp + 3 || seg[0] != ncomp) goto fail;
            for (unsigned i = 0; i < ncomp; ++i) {
                unsigned cid = seg[1 + 2 * i];
                for (unsigned j = 0; j < ncomp; ++j)
                    if (comps[j].id == cid) {
                        comps[j].dc_table = seg[2 + 2 * i] >> 4;
                        comps[j].ac_table = seg[2 + 2 * i] & 15;
                    }
            }
            for (unsigned i = 0; i < ncomp; ++i)
                if (comps[i].dc_table > 3 || comps[i].ac_table > 3 || !dc[comps[i].dc_table].present || !ac[comps[i].ac_table].present) goto fail;
            unsigned mcu_w = 8 * hmax, mcu_h = 8 * vmax, mcus_x = (width + mcu_w - 1) / mcu_w, mcus_y = (height + mcu_h - 1) / mcu_h;
            for (unsigned i = 0; i < ncomp; ++i) {
                comps[i].blocks_w = mcus_x * comps[i].h;
                comps[i].blocks_h = mcus_y * comps[i].v;
                comps[i].pixels = calloc((size_t)comps[i].blocks_w * 8 * comps[i].blocks_h * 8, 1);
                if (!comps[i].pixels) goto fail;
            }
            Reader r = {seg_end, end, 0, 0, false};
            unsigned until_restart = restart;
            for (unsigned my = 0; my < mcus_y && !r.failed; ++my)
                for (unsigned mx = 0; mx < mcus_x && !r.failed; ++mx) {
                    if (restart && !until_restart) {  // expect RSTn
                        r.count = 0;
                        while (r.p + 1 < r.end && !(r.p[0] == 0xFF && r.p[1] >= 0xD0 && r.p[1] <= 0xD7)) ++r.p;
                        if (r.p + 1 >= r.end) { r.failed = true; break; }
                        r.p += 2;
                        for (unsigned i = 0; i < ncomp; ++i) comps[i].predictor = 0;
                        until_restart = restart;
                    }
                    --until_restart;
                    for (unsigned c = 0; c < ncomp; ++c)
                        for (unsigned by = 0; by < comps[c].v; ++by)
                            for (unsigned bx = 0; bx < comps[c].h; ++bx) {
                                float block[64];
                                memset(block, 0, sizeof(block));
                                const uint16_t *q = quant[comps[c].quant];
                                int bits = decodeSymbol(&r, &dc[comps[c].dc_table]);
                                if (bits < 0 || bits > 16) { r.failed = true; break; }
                                comps[c].predictor += extend(readBits(&r, bits), bits);
                                block[0] = (float)(comps[c].predictor * q[0]);
                                for (int k = 1; k < 64 && !r.failed;) {
                                    int rs = decodeSymbol(&r, &ac[comps[c].ac_table]);
                                    if (rs < 0) break;
                                    int run = rs >> 4, size_bits = rs & 15;
                                    if (!size_bits) {
                                        if (run == 15) { k += 16; continue; }
                                        break;
                                    }
                                    k += run;
                                    if (k > 63) { r.failed = true; break; }
                                    block[zigzag[k]] = (float)(extend(readBits(&r, size_bits), size_bits) * q[zigzag[k]]);
                                    ++k;
                                }
                                if (r.failed) break;
                                unsigned stride = comps[c].blocks_w * 8;
                                idct(block, comps[c].pixels + (size_t)((my * comps[c].v + by) * 8) * stride + (mx * comps[c].h + bx) * 8, stride);
                            }
                }
            if (r.failed && 0) goto fail;  // a truncated file still shows what was decoded
            decoded = true;
            break;
        }
        p = seg_end;
    }
    if (!decoded) goto fail;
    result = malloc((size_t)width * height * 4);
    if (!result) goto fail;
    uint8_t *out = (uint8_t *)result;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            int yy[3] = {0, 128, 128};
            for (unsigned c = 0; c < ncomp; ++c) {
                unsigned cx = x * comps[c].h / hmax, cy = y * comps[c].v / vmax;
                yy[c] = comps[c].pixels[(size_t)cy * comps[c].blocks_w * 8 + cx];
            }
            uint8_t *px = out + ((size_t)y * width + x) * 4;
            if (ncomp == 1) {
                px[0] = px[1] = px[2] = (uint8_t)yy[0];
            } else {
                float Y = (float)yy[0], cb = (float)yy[1] - 128.0f, cr = (float)yy[2] - 128.0f;
                px[0] = clamp8((int)lroundf(Y + 1.402f * cr));
                px[1] = clamp8((int)lroundf(Y - 0.344136f * cb - 0.714136f * cr));
                px[2] = clamp8((int)lroundf(Y + 1.772f * cb));
            }
            px[3] = 255;
        }
    for (unsigned i = 0; i < ncomp; ++i) free(comps[i].pixels);
    *width_out = width;
    *height_out = height;
    return result;
fail:
    for (unsigned i = 0; i < 3; ++i) free(comps[i].pixels);
    free(result);
    return NULL;
}
