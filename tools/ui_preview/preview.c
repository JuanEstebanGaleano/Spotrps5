// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
// Renders the screens with made-up data to PNG-able PPM files: `make -C tools/ui_preview` then ./preview out_prefix.
#include "api.h"
#include "overlay.h"
#include "platform.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern float *sw_canvas;
// ---- stubs for what the screens call ---------------------------------------------------------------------------------------------------
static ApiResult queue[512];
static unsigned queue_head, queue_tail;
static void push(ApiResult r) { queue[queue_tail++ % 512] = r; }
void gameSendLine(const char *line) { printf("[command] %s\n", line); }
void apiResultFree(ApiResult *r) {
    free(r->list.items);
    for (int i = 0; i < 3; ++i) free(r->more[i].items);
    free(r->pixels);
    memset(r, 0, sizeof(*r));
}
bool apiTakeResult(ApiResult *r) {
    if (queue_head == queue_tail) return false;
    *r = queue[queue_head++ % 512];
    return true;
}
void apiStart(const char *root) { (void)root; }
static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
void apiRequestImage(const char *url) {
    ApiResult r = {0};
    r.type = RES_IMAGE;
    snprintf(r.text, sizeof(r.text), "%s", url);
    r.image_width = r.image_height = 120;
    r.pixels = malloc(120 * 120 * 4);
    unsigned h = hash(url);
    uint8_t *p = (uint8_t *)r.pixels;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 120; ++x) {
            float t = (float)(x + y) / 240.0f;
            p[(y * 120 + x) * 4 + 0] = (uint8_t)(((h >> 0) & 255) * (1 - t) + 40 * t);
            p[(y * 120 + x) * 4 + 1] = (uint8_t)(((h >> 8) & 255) * (1 - t) + 90 * t);
            p[(y * 120 + x) * 4 + 2] = (uint8_t)(((h >> 16) & 255) * (1 - t) + ((h >> 24) & 255) * t);
            p[(y * 120 + x) * 4 + 3] = 255;
        }
    push(r);
}
static Item make(ItemKind kind, const char *name, const char *sub, const char *album, unsigned ms) {
    Item i = {0};
    i.kind = kind;
    snprintf(i.name, sizeof(i.name), "%s", name);
    snprintf(i.sub, sizeof(i.sub), "%s", sub);
    snprintf(i.album, sizeof(i.album), "%s", album);
    snprintf(i.uri, sizeof(i.uri), "spotify:x:%s", name);
    snprintf(i.id, sizeof(i.id), "%s", name);
    snprintf(i.image, sizeof(i.image), "https://i.scdn.co/image/%s", kind == KIND_TRACK ? album : name);
    i.duration_ms = ms;
    i.total = 24;
    return i;
}
static List listOf(const Item *items, unsigned n) {
    List l = {malloc(n * sizeof(Item)), n};
    memcpy(l.items, items, n * sizeof(Item));
    return l;
}
static const char *playlists[] = {"Mix de lo mejor", "Reggaeton viejo", "Para entrenar", "Noches de rock", "Lo-fi para estudiar", "Salsa clásica", "Éxitos 2024", "Tranquilo en casa", "Road trip"};
static const char *albums[] = {"Un Verano Sin Ti", "Mañana Será Bonito", "Midnights", "Future Nostalgia", "Debí Tirar Más Fotos", "After Hours"};
void apiRequestLibrary(void) {
    ApiResult r = {0};
    Item list[16];
    unsigned n = 0;
    for (unsigned i = 0; i < 9; ++i) list[n++] = make(KIND_PLAYLIST, playlists[i], "Juan Esteban", "", 0);
    r.type = RES_PLAYLISTS; r.list = listOf(list, n); push(r);
    n = 0;
    for (unsigned i = 0; i < 6; ++i) list[n++] = make(KIND_ALBUM, albums[i], "Artista Famoso", "", 0);
    memset(&r, 0, sizeof(r)); r.type = RES_ALBUMS; r.list = listOf(list, n); push(r);
    n = 0;
    for (unsigned i = 5; i > 0; --i) list[n++] = make(KIND_ALBUM, albums[i], "Bad Bunny", "", 0);
    memset(&r, 0, sizeof(r)); r.type = RES_RECENT; r.list = listOf(list, n); push(r);
}
void apiRequestTracks(ItemKind kind, const char *id, unsigned tag) {
    (void)kind; (void)id;
    ApiResult r = {0};
    r.type = RES_TRACKS; r.tag = tag;
    Item t[20];
    static const char *names[] = {"Me Porto Bonito", "Tití Me Preguntó", "Ojitos Lindos", "Moscow Mule", "Party", "Efecto", "Neverita", "Un Ratito", "Después de la Playa", "Andrea", "Aguacero", "Callaíta"};
    for (unsigned i = 0; i < 12; ++i) t[i] = make(KIND_TRACK, names[i], "Bad Bunny, Bomba Estéreo", id, 180000 + i * 7000);
    r.list = listOf(t, 12);
    push(r);
}
void apiRequestSearch(const char *q, unsigned tag) {
    ApiResult r = {0};
    r.type = RES_SEARCH; r.tag = tag;
    Item t[8];
    for (unsigned i = 0; i < 5; ++i) t[i] = make(KIND_TRACK, i % 2 ? "Dákiti" : "Yonaguni", q, "x", 200000);
    r.list = listOf(t, 5);
    for (unsigned i = 0; i < 4; ++i) t[i] = make(KIND_ALBUM, albums[i], "Bad Bunny", "", 0);
    r.more[0] = listOf(t, 4);
    for (unsigned i = 0; i < 3; ++i) t[i] = make(KIND_PLAYLIST, playlists[i], "Spotify", "", 0);
    r.more[1] = listOf(t, 3);
    for (unsigned i = 0; i < 3; ++i) t[i] = make(KIND_ARTIST, "Bad Bunny", "", "", 0);
    r.more[2] = listOf(t, 3);
    push(r);
}
void apiRequestPlayback(void) {}
void apiPlay(const char *c, const char *const *u, unsigned n, const char *o) { printf("[play] ctx=%s uris=%u off=%s\n", c ? c : "-", n, o ? o : "-"); }

// ---- the scenes ----------------------------------------------------------------------------------------------------------------------
static uint64_t clock_ns = 1000000000ull;
static PlatformPad pad;
static void step(uint32_t buttons) {
    pad.connected = true;
    pad.buttons = buttons;
    clock_ns += 40000000ull;
    uiUpdate(&pad, clock_ns);
    pad.buttons = 0;
    clock_ns += 400000000ull;
    uiUpdate(&pad, clock_ns);
}
static void settle(void) { for (int i = 0; i < 60; ++i) { clock_ns += 33000000ull; uiUpdate(&pad, clock_ns); } }
static void save(const char *prefix, const char *name) {
    settle();
    for (int pass = 0; pass < 3; ++pass) {  // the first draws ask for the covers, which arrive in between
        overlayBegin(1920, 1080);
        uiDraw(clock_ns);
        overlayEnd();
        settle();
    }
    char path[300];
    snprintf(path, sizeof(path), "%s_%s.ppm", prefix, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 1920 1080 255\n");
    for (int i = 0; i < 1920 * 1080 * 3; ++i) { float v = sw_canvas[i]; fputc((int)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f), f); }
    fclose(f);
    printf("wrote %s\n", path);
}
int main(int argc, char **argv) {
    const char *prefix = argc > 1 ? argv[1] : "/tmp/ui";
    uiInit();
    ApiResult ready = {0};
    ready.type = RES_READY;
    push(ready);
    // something is playing
    ApiResult pb = {0};
    pb.type = RES_PLAYBACK;
    pb.playback.valid = pb.playback.playing = pb.playback.on_this_device = true;
    pb.playback.shuffle = true; pb.playback.repeat = 1; pb.playback.volume = 70;
    snprintf(pb.playback.title, sizeof(pb.playback.title), "Me Porto Bonito");
    snprintf(pb.playback.artist, sizeof(pb.playback.artist), "Bad Bunny, Chencho Corleone");
    snprintf(pb.playback.image, sizeof(pb.playback.image), "https://i.scdn.co/image/x");
    snprintf(pb.playback.uri, sizeof(pb.playback.uri), "spotify:x:Me Porto Bonito");
    pb.playback.progress_ms = 123000; pb.playback.duration_ms = 212000;
    push(pb);
    settle();
    save(prefix, "home");
    step(PLATFORM_PAD_RIGHT); step(PLATFORM_PAD_DOWN); step(PLATFORM_PAD_DOWN);
    save(prefix, "home_shelf");
    step(PLATFORM_PAD_LEFT); step(PLATFORM_PAD_LEFT); step(PLATFORM_PAD_LEFT);
    save(prefix, "sidebar");
    step(PLATFORM_PAD_DOWN); step(PLATFORM_PAD_CROSS);
    save(prefix, "playlist");
    step(PLATFORM_PAD_DOWN); step(PLATFORM_PAD_DOWN); step(PLATFORM_PAD_DOWN);
    save(prefix, "playlist_row");
    step(PLATFORM_PAD_TRIANGLE);
    step(PLATFORM_PAD_CROSS); step(PLATFORM_PAD_RIGHT); step(PLATFORM_PAD_RIGHT); step(PLATFORM_PAD_DOWN); step(PLATFORM_PAD_CROSS);
    save(prefix, "keyboard");
    step(PLATFORM_PAD_OPTIONS);
    save(prefix, "search");
    return 0;
}
