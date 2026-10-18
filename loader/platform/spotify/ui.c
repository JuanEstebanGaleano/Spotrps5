// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
#include "ui.h"
#include "api.h"
#include "game.h"
#include "overlay.h"
#include "overlay_icons.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- look ------------------------------------------------------------------------------------------------------------------------
#define C_BG 0x000000FFu
#define C_PANEL 0x121212FFu
#define C_RAISED 0x1F1F1FFFu
#define C_HOVER 0x2A2A2AFFu
#define C_TEXT 0xFFFFFFFFu
#define C_DIM 0xB3B3B3FFu
#define C_FAINT 0x7A7A7AFFu
#define C_GREEN 0x1ED760FFu
#define C_BLACK 0x000000FFu

#define SIDEBAR_X 12.0f
#define SIDEBAR_Y 88.0f
#define SIDEBAR_W 440.0f
#define PANEL_H 820.0f
#define MAIN_X 460.0f
#define MAIN_W 1448.0f
#define PAD 28.0f
#define CONTENT_W (MAIN_W - 2 * PAD)
#define PLAYER_Y 916.0f
#define STRIP_Y 1044.0f

enum { F_SIDEBAR, F_MAIN, F_TOP, F_PLAYER };
typedef enum { V_HOME, V_TRACKS, V_SEARCH } ViewType;
typedef enum { R_TILES, R_SHELF, R_TRACK, R_HERO, R_TITLE, R_TEXT } RowKind;
typedef struct {
    RowKind kind;
    const Item *items;
    unsigned count;
    const char *title;  // R_TITLE / R_TEXT
    float y, h;
    unsigned number;    // R_TRACK: the position in the list
} Row;
#define MAX_ROWS 240
typedef struct {
    ViewType type;
    Item source;       // V_TRACKS: the playlist / album / liked songs
    char query[96];    // V_SEARCH
    List tracks;       // V_TRACKS: its songs; V_SEARCH: found songs
    List more[3];      // V_SEARCH: albums, playlists, artists
    bool loading, failed;
    unsigned tag;
    int row, col;
    float scroll;
    uint8_t shelf_scroll[MAX_ROWS];
} View;
#define MAX_DEPTH 8

static struct {
    bool ready, quit_wanted;
    List playlists, albums, recent;
    bool library_loading;
    Item liked;
    Playback playback;
    uint64_t playback_stamp, optimistic_until;
    View stack[MAX_DEPTH];
    int depth;
    Row rows[MAX_ROWS];
    unsigned row_count;
    bool rows_dirty;
    float rows_total;
    int focus;
    int side_cursor;
    float side_scroll;
    int top_cursor, player_cursor;
    unsigned serial;
    char toast[200];
    uint64_t toast_until;
    char diagnostics[200];
    // keyboard
    bool keyboard;
    int key_row, key_col;
    char typed[96];
    // controller edges
    uint32_t held;
    uint64_t repeat_at[32];
    uint64_t circle_since;
    uint64_t last_poll_request;
    uint64_t now;
} ui;

// ---- covers ----------------------------------------------------------------------------------------------------------------------
typedef struct {
    char url[176];
    OverlayImage *image;
    uint8_t state;  // 0 asked, 1 ready, 2 failed
    uint32_t average;  // 0xRRGGBB of the picture
    unsigned used;
} Cover;
#define COVERS 200
static Cover covers[COVERS];
static unsigned frame_number;

static Cover *coverEntry(const char *url) {
    if (!url || !url[0]) return NULL;
    Cover *free_slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < COVERS; ++i) {
        Cover *c = &covers[i];
        if (c->url[0] && !strcmp(c->url, url)) {
            c->used = frame_number;
            return c;
        }
        if (!c->url[0] && !free_slot) free_slot = c;
        if (c->url[0] && c->state != 0 && (!oldest || c->used < oldest->used)) oldest = c;
    }
    Cover *slot = free_slot ? free_slot : oldest;
    if (!slot) return NULL;  // all of them are waiting for answers
    overlayImageDestroy(slot->image);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->used = frame_number;
    apiRequestImage(url);
    return slot;
}
static OverlayImage *coverImage(const char *url) {
    Cover *c = coverEntry(url);
    return c && c->state == 1 ? c->image : NULL;
}
static uint32_t coverColour(const char *url, uint32_t fallback) {
    Cover *c = coverEntry(url);
    return c && c->state == 1 ? c->average : fallback;
}
static void coverArrived(const ApiResult *r) {
    for (unsigned i = 0; i < COVERS; ++i) {
        Cover *c = &covers[i];
        if (!c->url[0] || strcmp(c->url, r->text)) continue;
        if (!r->pixels) {
            c->state = 2;
            return;
        }
        c->image = overlayImageCreate(r->image_width, r->image_height, r->pixels);
        c->state = c->image ? 1 : 2;
        unsigned long red = 0, green = 0, blue = 0, n = 0;
        const uint8_t *p = (const uint8_t *)r->pixels;
        for (size_t k = 0; k < (size_t)r->image_width * r->image_height; k += 7, ++n) {
            red += p[k * 4];
            green += p[k * 4 + 1];
            blue += p[k * 4 + 2];
        }
        if (n) c->average = ((uint32_t)(red / n) << 16) | ((uint32_t)(green / n) << 8) | (uint32_t)(blue / n);
        return;
    }
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
static uint32_t rgb(uint32_t colour, float darken, uint32_t alpha) {
    uint32_t r = (uint32_t)(((colour >> 16) & 255) * darken), g = (uint32_t)(((colour >> 8) & 255) * darken), b = (uint32_t)((colour & 255) * darken);
    return (r << 24) | (g << 16) | (b << 8) | alpha;
}
static uint32_t alphaOf(uint32_t colour, uint32_t alpha) { return (colour & 0xFFFFFF00u) | alpha; }
static View *view(void) { return &ui.stack[ui.depth - 1]; }
static void freeList(List *l) {
    free(l->items);
    l->items = NULL;
    l->count = 0;
}
static void freeView(View *v) {
    freeList(&v->tracks);
    for (unsigned i = 0; i < 3; ++i) freeList(&v->more[i]);
}
static void toast(const char *text) {
    snprintf(ui.toast, sizeof(ui.toast), "%s", text);
    ui.toast_until = ui.now + 4500000000ull;
}
static void formatTime(unsigned ms, char *out, size_t size) {
    unsigned s = ms / 1000;
    snprintf(out, size, "%u:%02u", s / 60, s % 60);
}
static void subtitle(const Item *item, char *out, size_t size) {
    switch (item->kind) {
    case KIND_PLAYLIST: snprintf(out, size, "Lista \xE2\x80\xA2 %s", item->sub); break;
    case KIND_ALBUM: snprintf(out, size, "%s", item->sub); break;
    case KIND_ARTIST: snprintf(out, size, "Artista"); break;
    case KIND_LIKED: snprintf(out, size, "Lista"); break;
    default: snprintf(out, size, "%s", item->sub); break;
    }
}
static void setRepeat(int state) {
    ui.playback.repeat = state;
    gameSendLine("repeat_track_off");
    if (state == 2) gameSendLine("repeat_track_on");
    else gameSendLine(state ? "repeat_on" : "repeat_off");
}
static void setVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ui.playback.volume = percent;
    char command[24];
    snprintf(command, sizeof(command), "volume %d", percent);
    gameSendLine(command);
}
static void focusRing(float x, float y, float w, float h, float r) { overlayRoundFrame(x - 3, y - 3, w + 6, h + 6, r + 3, 3, C_TEXT); }

// A cover (or a stand-in) with rounded corners.
static void drawCover(const Item *item, float x, float y, float size, float radius) {
    OverlayImage *image = item->kind == KIND_LIKED ? NULL : coverImage(item->image);
    if (image) {
        overlayImageDraw(image, x, y, size, size, radius, 0xFFFFFFFFu);
        return;
    }
    if (item->kind == KIND_LIKED) {
        overlayRoundGradient(x, y, size, size, radius, 0x4B2BE6FF, 0xA9C8E8FF);
        overlayIcon(OVERLAY_ICON_HEART_FILLED, x + size / 2, y + size / 2, size * 0.46f, C_TEXT);
    } else {
        overlayRoundGradient(x, y, size, size, radius, 0x3A3A3AFF, 0x232323FF);
        overlayIcon(item->kind == KIND_ARTIST ? OVERLAY_ICON_DEVICE : OVERLAY_ICON_NOTE, x + size / 2, y + size / 2, size * 0.4f, C_FAINT);
    }
}

// ---- rows of the main panel ---------------------------------------------------------------------------------------------------------
static Row *addRow(RowKind kind, float height) {
    if (ui.row_count >= MAX_ROWS) return NULL;
    Row *r = &ui.rows[ui.row_count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->h = height;
    r->y = ui.rows_total;
    ui.rows_total += height;
    return r;
}
static void addTitle(const char *title) {
    Row *r = addRow(R_TITLE, 68);
    if (r) r->title = title;
}
static void addText(const char *text) {
    Row *r = addRow(R_TEXT, 72);
    if (r) r->title = text;
}
static void addShelf(const char *title, const List *list) {
    if (!list->count) return;
    addTitle(title);
    Row *r = addRow(R_SHELF, 300);
    if (r) {
        r->items = list->items;
        r->count = list->count;
    }
}
static bool focusable(const Row *r) { return r->kind == R_TILES || r->kind == R_SHELF || r->kind == R_TRACK || r->kind == R_HERO; }
static unsigned columnsOf(const Row *r) { return r->kind == R_TILES || r->kind == R_SHELF ? r->count : 1; }

static void addTrackTable(const List *tracks) {
    Row *header = addRow(R_TITLE, 44);
    if (header) header->title = "";
    for (unsigned i = 0; i < tracks->count; ++i) {
        Row *r = addRow(R_TRACK, 68);
        if (!r) break;
        r->items = &tracks->items[i];
        r->count = 1;
        r->number = i + 1;
    }
}
static void buildRows(void) {
    ui.row_count = 0;
    ui.rows_total = 24;
    View *v = view();
    static Item shortcuts[8];
    if (v->type == V_HOME) {
        unsigned n = 0;
        shortcuts[n++] = ui.liked;
        for (unsigned i = 0; i < ui.playlists.count && n < 8; ++i) shortcuts[n++] = ui.playlists.items[i];
        for (unsigned i = 0; i < n; i += 4) {
            Row *r = addRow(R_TILES, 88);
            if (r) {
                r->items = &shortcuts[i];
                r->count = n - i < 4 ? n - i : 4;
            }
        }
        if (ui.library_loading && !ui.playlists.count) addText("Cargando tu biblioteca...");
        ui.rows_total += 12;
        addShelf("Escuchado recientemente", &ui.recent);
        addShelf("Tus listas", &ui.playlists);
        addShelf("Tus \xC3\xA1lbumes", &ui.albums);
    } else if (v->type == V_TRACKS) {
        Row *hero = addRow(R_HERO, 320);
        if (hero) {
            hero->items = &v->source;
            hero->count = 1;
        }
        if (v->loading) addText("Cargando canciones...");
        else if (v->failed) addText("No se pudieron cargar las canciones");
        else if (!v->tracks.count) addText("No hay canciones aqu\xC3\xAD");
        else addTrackTable(&v->tracks);
    } else {
        if (v->loading) addText("Buscando...");
        else if (v->failed) addText("La b\xC3\xBAsqueda no funcion\xC3\xB3");
        else if (!v->tracks.count && !v->more[0].count && !v->more[1].count && !v->more[2].count) addText("Sin resultados");
        if (v->tracks.count) {
            addTitle("Canciones");
            for (unsigned i = 0; i < v->tracks.count && i < 8; ++i) {
                Row *r = addRow(R_TRACK, 68);
                if (!r) break;
                r->items = &v->tracks.items[i];
                r->count = 1;
                r->number = i + 1;
            }
        }
        addShelf("\xC3\x81lbumes", &v->more[0]);
        addShelf("Listas", &v->more[1]);
        addShelf("Artistas", &v->more[2]);
    }
    ui.rows_total += 40;
    // keep the cursor on something that exists
    if (v->row >= (int)ui.row_count) v->row = (int)ui.row_count - 1;
    if (v->row < 0) v->row = 0;
    while (v->row < (int)ui.row_count && !focusable(&ui.rows[v->row])) ++v->row;
    if (v->row >= (int)ui.row_count) {
        v->row = (int)ui.row_count - 1;
        while (v->row > 0 && !focusable(&ui.rows[v->row])) --v->row;
    }
    if (ui.row_count && v->col >= (int)columnsOf(&ui.rows[v->row])) v->col = (int)columnsOf(&ui.rows[v->row]) - 1;
    if (v->col < 0) v->col = 0;
    ui.rows_dirty = false;
}

// ---- actions -----------------------------------------------------------------------------------------------------------------------
static void pushView(ViewType type, const Item *source, const char *query) {
    if (ui.depth >= MAX_DEPTH) {
        freeView(&ui.stack[1]);
        memmove(&ui.stack[1], &ui.stack[2], sizeof(View) * (MAX_DEPTH - 2));
        --ui.depth;
    }
    View *v = &ui.stack[ui.depth++];
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->tag = ++ui.serial;
    v->loading = true;
    if (source) v->source = *source;
    if (query) snprintf(v->query, sizeof(v->query), "%s", query);
    if (type == V_TRACKS) apiRequestTracks(source->kind, source->id, v->tag);
    else if (type == V_SEARCH) apiRequestSearch(query, v->tag);
    ui.rows_dirty = true;
    ui.focus = F_MAIN;
}
static void goBack(void) {
    if (ui.depth <= 1) return;
    freeView(view());
    --ui.depth;
    ui.rows_dirty = true;
}
static void goHome(void) {
    while (ui.depth > 1) goBack();
    view()->row = view()->col = 0;
    view()->scroll = 0;
    ui.focus = F_MAIN;
    ui.rows_dirty = true;
}
static void optimistic(const Item *track) {
    Playback *p = &ui.playback;
    p->valid = true;
    p->playing = true;
    p->progress_ms = 0;
    p->duration_ms = track->duration_ms;
    snprintf(p->title, sizeof(p->title), "%s", track->name);
    snprintf(p->artist, sizeof(p->artist), "%s", track->sub);
    snprintf(p->album, sizeof(p->album), "%s", track->album);
    snprintf(p->image, sizeof(p->image), "%s", track->image);
    snprintf(p->uri, sizeof(p->uri), "%s", track->uri);
    p->on_this_device = true;
    ui.playback_stamp = ui.now;
    ui.optimistic_until = ui.now + 6000000000ull;
}
static void playTrackAt(const List *list, unsigned index, const char *context) {
    const Item *track = &list->items[index];
    optimistic(track);
    if (context && context[0]) {
        apiPlay(context, NULL, 0, track->uri);
        return;
    }
    const char *uris[50];
    unsigned n = 0;
    for (unsigned i = index; i < list->count && n < 50; ++i) uris[n++] = list->items[i].uri;
    apiPlay(NULL, uris, n, NULL);
}
static void playHero(const View *v) {
    if (v->source.kind == KIND_LIKED || !v->source.uri[0]) {
        if (v->tracks.count) playTrackAt(&v->tracks, 0, NULL);
        return;
    }
    if (v->tracks.count) optimistic(&v->tracks.items[0]);
    apiPlay(v->source.uri, NULL, 0, NULL);
}
static void openItem(const Item *item) {
    if (item->kind == KIND_ARTIST) {
        apiPlay(item->uri, NULL, 0, NULL);
        toast("Reproduciendo artista");
        return;
    }
    pushView(V_TRACKS, item, NULL);
}
static void activate(void) {
    View *v = view();
    if (ui.focus == F_MAIN) {
        if (v->row < 0 || v->row >= (int)ui.row_count) return;
        const Row *r = &ui.rows[v->row];
        if (r->kind == R_HERO) {
            playHero(v);
        } else if (r->kind == R_TRACK) {
            if (v->type == V_TRACKS && v->source.kind != KIND_LIKED) playTrackAt(&v->tracks, r->number - 1, v->source.uri);
            else playTrackAt(&v->tracks, r->number - 1, NULL);
        } else if (v->col < (int)r->count) {
            openItem(&r->items[v->col]);
        }
    } else if (ui.focus == F_SIDEBAR) {
        unsigned total = 1 + ui.playlists.count + ui.albums.count;
        if ((unsigned)ui.side_cursor >= total) return;
        const Item *item = ui.side_cursor == 0 ? &ui.liked : ui.side_cursor <= (int)ui.playlists.count ? &ui.playlists.items[ui.side_cursor - 1]
                                                                                                      : &ui.albums.items[ui.side_cursor - 1 - (int)ui.playlists.count];
        openItem(item);
    } else if (ui.focus == F_TOP) {
        if (ui.top_cursor == 0) goHome();
        else {
            ui.keyboard = true;
            ui.key_row = ui.key_col = 0;
        }
    } else if (ui.focus == F_PLAYER) {
        static const char *const commands[] = {NULL, "prev", "play_pause", "next", NULL};
        switch (ui.player_cursor) {
        case 0: ui.playback.shuffle = !ui.playback.shuffle; gameSendLine(ui.playback.shuffle ? "shuffle_on" : "shuffle_off"); break;
        case 4:
            setRepeat((ui.playback.repeat + 1) % 3);
            break;
        case 2: ui.playback.playing = !ui.playback.playing; gameSendLine("play_pause"); break;
        default: gameSendLine(commands[ui.player_cursor]); break;
        }
        ui.last_poll_request = 0;
    }
}

// ---- keyboard --------------------------------------------------------------------------------------------------------------------
static const char *const key_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl\xC3\xB1", "zxcvbnm,.-"};
#define KEY_ROWS 4
#define KEY_COLS 10
static void keyboardSubmit(void) {
    if (!ui.typed[0]) return;
    ui.keyboard = false;
    while (ui.depth > 1 && view()->type == V_SEARCH) goBack();
    pushView(V_SEARCH, NULL, ui.typed);
}
static void typeChar(const char *utf8) {
    size_t n = strlen(ui.typed), k = strlen(utf8);
    if (n + k < sizeof(ui.typed)) memcpy(ui.typed + n, utf8, k + 1);
}
static void backspace(void) {
    size_t n = strlen(ui.typed);
    while (n && (ui.typed[n - 1] & 0xC0) == 0x80) --n;
    if (n) ui.typed[n - 1] = 0;
}
static void keyboardPress(void) {
    if (ui.key_row < KEY_ROWS) {
        const char *row = key_rows[ui.key_row];
        // each key is one UTF-8 character
        const char *p = row;
        for (int i = 0; i < ui.key_col && *p; ++i) p += ((unsigned char)*p >= 0xC0) ? 2 : 1;
        char one[4] = {0};
        size_t len = ((unsigned char)*p >= 0xC0) ? 2 : 1;
        memcpy(one, p, len);
        typeChar(one);
    } else if (ui.key_col < 6) typeChar(" ");
    else if (ui.key_col < 8) backspace();
    else keyboardSubmit();
}

// ---- input -----------------------------------------------------------------------------------------------------------------------
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L1, B_R1, B_L2, B_R2, B_OPTIONS, B_TOUCHPAD, B_COUNT };
static const uint32_t button_bits[B_COUNT] = {PLATFORM_PAD_UP,    PLATFORM_PAD_DOWN, PLATFORM_PAD_LEFT, PLATFORM_PAD_RIGHT, PLATFORM_PAD_CROSS,
                                              PLATFORM_PAD_CIRCLE, PLATFORM_PAD_SQUARE, PLATFORM_PAD_TRIANGLE, PLATFORM_PAD_L1,   PLATFORM_PAD_R1,
                                              PLATFORM_PAD_L2,    PLATFORM_PAD_R2,   PLATFORM_PAD_OPTIONS, PLATFORM_PAD_TOUCHPAD};
// Pressed now: a new press, or a held direction / volume key repeating.
static bool pressed(const PlatformPad *pad, int button) {
    bool down = (pad->buttons & button_bits[button]) != 0, was = (ui.held & button_bits[button]) != 0;
    bool repeats = button <= B_RIGHT || button == B_L2 || button == B_R2;
    if (down && !was) {
        ui.repeat_at[button] = ui.now + 380000000ull;
        return true;
    }
    if (down && repeats && ui.now >= ui.repeat_at[button]) {
        ui.repeat_at[button] = ui.now + 85000000ull;
        return true;
    }
    return false;
}
static void moveMain(int dx, int dy) {
    View *v = view();
    if (!ui.row_count) {
        if (dy < 0) ui.focus = F_TOP;
        else if (dy > 0) ui.focus = F_PLAYER;
        else if (dx < 0) ui.focus = F_SIDEBAR;
        return;
    }
    const Row *r = &ui.rows[v->row];
    if (dx) {
        int col = v->col + dx;
        if (col < 0) {
            if (r->kind == R_HERO || r->kind == R_TRACK || r->kind == R_TILES || r->kind == R_SHELF) ui.focus = F_SIDEBAR;
            return;
        }
        if (col < (int)columnsOf(r)) v->col = col;
        return;
    }
    int row = v->row + dy;
    while (row >= 0 && row < (int)ui.row_count && !focusable(&ui.rows[row])) row += dy;
    if (row < 0) {
        ui.focus = F_TOP;
        ui.top_cursor = 1;
        return;
    }
    if (row >= (int)ui.row_count) {
        ui.focus = F_PLAYER;
        ui.player_cursor = 2;
        return;
    }
    // Between a 4-wide tile row and a shelf, keep the column; clamp to what the new row has.
    v->row = row;
    int cols = (int)columnsOf(&ui.rows[row]);
    if (v->col >= cols) v->col = cols - 1;
}
static void handleKeyboard(const PlatformPad *pad) {
    int max_col = ui.key_row < KEY_ROWS ? KEY_COLS - 1 : 9;
    if (pressed(pad, B_LEFT)) ui.key_col = ui.key_col > 0 ? ui.key_col - 1 : max_col;
    if (pressed(pad, B_RIGHT)) ui.key_col = ui.key_col < max_col ? ui.key_col + 1 : 0;
    if (pressed(pad, B_UP)) ui.key_row = ui.key_row > 0 ? ui.key_row - 1 : KEY_ROWS;
    if (pressed(pad, B_DOWN)) ui.key_row = ui.key_row < KEY_ROWS ? ui.key_row + 1 : 0;
    if (ui.key_row < KEY_ROWS && ui.key_col >= KEY_COLS) ui.key_col = KEY_COLS - 1;
    if (pressed(pad, B_CROSS)) keyboardPress();
    if (pressed(pad, B_SQUARE)) backspace();
    if (pressed(pad, B_TRIANGLE)) typeChar(" ");
    if (pressed(pad, B_OPTIONS)) keyboardSubmit();
    if (pressed(pad, B_CIRCLE)) ui.keyboard = false;
}
static void handleInput(const PlatformPad *pad) {
    if (!pad->connected) return;
    if (ui.keyboard) {
        handleKeyboard(pad);
        return;
    }
    bool up = pressed(pad, B_UP), down = pressed(pad, B_DOWN), left = pressed(pad, B_LEFT), right = pressed(pad, B_RIGHT);
    if (pressed(pad, B_CROSS)) activate();
    if (pressed(pad, B_SQUARE)) {
        ui.playback.playing = !ui.playback.playing;
        gameSendLine("play_pause");
        ui.last_poll_request = 0;
    }
    if (pressed(pad, B_L1)) { gameSendLine("prev"); ui.last_poll_request = 0; }
    if (pressed(pad, B_R1)) { gameSendLine("next"); ui.last_poll_request = 0; }
    if (pressed(pad, B_R2)) {
        setVolume((ui.playback.volume < 0 ? 70 : ui.playback.volume) + 5);
    }
    if (pressed(pad, B_L2)) {
        setVolume((ui.playback.volume < 0 ? 70 : ui.playback.volume) - 5);
    }
    if (pressed(pad, B_TRIANGLE)) {
        ui.keyboard = true;
        ui.key_row = ui.key_col = 0;
        return;
    }
    if (pressed(pad, B_OPTIONS)) {
        ui.playback.shuffle = !ui.playback.shuffle;
        gameSendLine(ui.playback.shuffle ? "shuffle_on" : "shuffle_off");
    }
    if (pressed(pad, B_TOUCHPAD)) {
        setRepeat((ui.playback.repeat + 1) % 3);
    }
    // Circle: back; held for two seconds on the home screen, leave.
    if (pad->buttons & PLATFORM_PAD_CIRCLE) {
        if (!(ui.held & PLATFORM_PAD_CIRCLE)) {
            ui.circle_since = ui.now;
            if (ui.depth > 1) goBack();
            else if (ui.focus != F_MAIN) ui.focus = F_MAIN;
        } else if (ui.depth == 1 && ui.now - ui.circle_since > 2000000000ull) {
            ui.quit_wanted = true;
        }
    }
    if (ui.focus == F_MAIN) {
        if (up) moveMain(0, -1);
        else if (down) moveMain(0, 1);
        else if (left) moveMain(-1, 0);
        else if (right) moveMain(1, 0);
    } else if (ui.focus == F_SIDEBAR) {
        unsigned total = 1 + ui.playlists.count + ui.albums.count;
        if (up) {
            if (ui.side_cursor > 0) --ui.side_cursor;
            else { ui.focus = F_TOP; ui.top_cursor = 0; }
        } else if (down) {
            if ((unsigned)ui.side_cursor + 1 < total) ++ui.side_cursor;
            else { ui.focus = F_PLAYER; ui.player_cursor = 0; }
        } else if (right) {
            ui.focus = F_MAIN;
        }
    } else if (ui.focus == F_TOP) {
        if (left) ui.top_cursor = 0;
        else if (right) ui.top_cursor = 1;
        else if (down) ui.focus = ui.top_cursor == 0 ? F_SIDEBAR : F_MAIN;
    } else {
        if (left && ui.player_cursor > 0) --ui.player_cursor;
        else if (right && ui.player_cursor < 4) ++ui.player_cursor;
        else if (up) ui.focus = ui.player_cursor < 2 ? F_SIDEBAR : F_MAIN;
    }
}

// ---- answers ---------------------------------------------------------------------------------------------------------------------
static void takeList(List *to, List *from) {
    freeList(to);
    *to = *from;
    from->items = NULL;
    from->count = 0;
}
static void handleResults(void) {
    ApiResult r;
    while (apiTakeResult(&r)) {
        View *top = view();
        switch (r.type) {
        case RES_READY:
            ui.ready = true;
            ui.library_loading = true;
            apiRequestLibrary();
            apiRequestPlayback();
            break;
        case RES_PLAYLISTS: takeList(&ui.playlists, &r.list); ui.library_loading = false; ui.rows_dirty = true; break;
        case RES_ALBUMS: takeList(&ui.albums, &r.list); ui.rows_dirty = true; break;
        case RES_RECENT: takeList(&ui.recent, &r.list); ui.rows_dirty = true; break;
        case RES_TRACKS:
            if (r.tag == top->tag && top->type == V_TRACKS) {
                takeList(&top->tracks, &r.list);
                top->loading = false;
                top->failed = r.failed;
                ui.rows_dirty = true;
            }
            break;
        case RES_SEARCH:
            if (r.tag == top->tag && top->type == V_SEARCH) {
                takeList(&top->tracks, &r.list);
                for (unsigned i = 0; i < 3; ++i) takeList(&top->more[i], &r.more[i]);
                top->loading = false;
                top->failed = r.failed;
                ui.rows_dirty = true;
            }
            break;
        case RES_PLAYBACK:
            if (!r.failed && (r.playback.valid || ui.now >= ui.optimistic_until)) {
                ui.playback = r.playback;
                ui.playback_stamp = ui.now;
            } else if (!r.failed && r.playback.valid == false) {
                ui.playback.playing = ui.playback.playing;  // keep the guess for a moment
            }
            break;
        case RES_IMAGE: coverArrived(&r); break;
        case RES_STATUS: toast(r.text); break;
        }
        apiResultFree(&r);
    }
}

// ---- drawing ---------------------------------------------------------------------------------------------------------------------
static float progressNow(void) {
    const Playback *p = &ui.playback;
    double ms = p->progress_ms + (p->playing ? (double)(ui.now - ui.playback_stamp) / 1e6 : 0);
    if (p->duration_ms && ms > p->duration_ms) ms = p->duration_ms;
    return (float)ms;
}
static void drawTop(void) {
    overlayIcon(OVERLAY_ICON_NOTE, 48, 44, 40, C_GREEN);
    overlayCircle(130, 44, 30, C_RAISED);
    overlayIcon(OVERLAY_ICON_HOME, 130, 44, 34, C_TEXT);
    if (ui.focus == F_TOP && ui.top_cursor == 0) focusRing(100, 14, 60, 60, 30);
    overlayRoundRect(500, 14, 920, 60, 30, C_RAISED);
    overlayIcon(OVERLAY_ICON_SEARCH, 540, 44, 32, C_DIM);
    overlayText(572, 44 - 15, "\xC2\xBF" "Qu\xC3\xA9 quieres reproducir?", 30, C_FAINT);
    if (ui.focus == F_TOP && ui.top_cursor == 1) focusRing(500, 14, 920, 60, 30);
    overlayRoundRect(1450, 14, 220, 60, 30, C_RAISED);
    overlayIcon(OVERLAY_ICON_DEVICE, 1490, 44, 30, C_GREEN);
    overlayText(1518, 44 - 14, "Spotrps5", 28, C_TEXT);
    overlayCircle(1846, 44, 26, C_GREEN);
    overlayTextBold(1846 - 9, 44 - 17, "S", 34, C_BLACK);
}
static void drawSidebar(void) {
    overlayRoundRect(SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, PANEL_H, 16, C_PANEL);
    overlayIcon(OVERLAY_ICON_LIBRARY, 52, 128, 36, C_DIM);
    overlayTextBold(84, 128 - 17, "Tu biblioteca", 34, C_DIM);
    const float top = 166, row_h = 82, view_h = SIDEBAR_Y + PANEL_H - 12 - top;
    unsigned total = 1 + ui.playlists.count + ui.albums.count;
    float target = ui.side_scroll;
    if ((float)ui.side_cursor * row_h < target) target = (float)ui.side_cursor * row_h;
    if ((float)(ui.side_cursor + 1) * row_h > target + view_h) target = (float)(ui.side_cursor + 1) * row_h - view_h;
    ui.side_scroll += (target - ui.side_scroll) * 0.35f;
    overlayClip(SIDEBAR_X, top, SIDEBAR_W, view_h);
    for (unsigned i = 0; i < total; ++i) {
        float y = top + (float)i * row_h - ui.side_scroll;
        if (y > top + view_h || y + row_h < top) continue;
        const Item *item = i == 0 ? &ui.liked : i <= ui.playlists.count ? &ui.playlists.items[i - 1] : &ui.albums.items[i - 1 - ui.playlists.count];
        bool focused = ui.focus == F_SIDEBAR && (int)i == ui.side_cursor;
        if (focused) overlayRoundRect(SIDEBAR_X + 8, y, SIDEBAR_W - 16, row_h - 6, 12, C_HOVER);
        drawCover(item, SIDEBAR_X + 18, y + 6, 64, 8);
        char sub[160];
        subtitle(item, sub, sizeof(sub));
        overlayTextFitBold(SIDEBAR_X + 98, y + 8, item->name, 26, SIDEBAR_W - 120, C_TEXT);
        overlayTextFit(SIDEBAR_X + 98, y + 38, sub, 22, SIDEBAR_W - 120, C_DIM);
        if (focused) focusRing(SIDEBAR_X + 8, y, SIDEBAR_W - 16, row_h - 6, 12);
    }
    overlayClipEnd();
}
static void drawTile(const Item *item, float x, float y, float w, float h, bool focused) {
    overlayRoundRect(x, y, w, h, 8, focused ? C_HOVER : 0x2B2B2BFFu);
    drawCover(item, x, y, h, 8);
    overlayTextFitBold(x + h + 16, y + h / 2 - 15, item->name, 28, w - h - 28, C_TEXT);
    if (focused) focusRing(x, y, w, h, 8);
}
static void drawCard(const Item *item, float x, float y, float w, bool focused) {
    overlayRoundRect(x - 8, y - 8, w + 16, 300 - 8, 12, focused ? C_HOVER : alphaOf(C_PANEL, 0));
    drawCover(item, x, y, w, item->kind == KIND_ARTIST ? 8 : 8);
    char sub[160];
    subtitle(item, sub, sizeof(sub));
    overlayTextFitBold(x, y + w + 8, item->name, 26, w, C_TEXT);
    overlayTextFit(x, y + w + 38, sub, 22, w, C_DIM);
    if (focused) focusRing(x - 8, y - 8, w + 16, 292, 12);
}
static void drawTrackRow(const Row *r, float x, float y, float w, bool focused) {
    const Item *t = r->items;
    bool current = ui.playback.valid && !strcmp(ui.playback.uri, t->uri);
    if (focused) overlayRoundRect(x, y, w, r->h - 4, 10, C_HOVER);
    char number[16];
    snprintf(number, sizeof(number), "%u", r->number);
    if (focused) overlayIcon(OVERLAY_ICON_PLAY, x + 34, y + (r->h - 4) / 2, 30, C_TEXT);
    else if (current && ui.playback.playing) overlayIcon(OVERLAY_ICON_EQUALIZER, x + 34, y + (r->h - 4) / 2, 32, C_GREEN);
    else overlayTextCentered(x + 34, y + (r->h - 4) / 2 - 14, number, 28, current ? C_GREEN : C_DIM);
    drawCover(t, x + 72, y + 6, 52, 6);
    overlayTextFit(x + 144, y + 6, t->name, 27, 560, current ? C_GREEN : C_TEXT);
    overlayTextFit(x + 144, y + 35, t->sub, 22, 560, C_DIM);
    overlayTextFit(x + 760, y + (r->h - 4) / 2 - 14, t->album, 24, 380, C_DIM);
    char time[16];
    formatTime(t->duration_ms, time, sizeof(time));
    overlayText(x + w - 24 - overlayTextWidth(time, 24), y + (r->h - 4) / 2 - 14, time, 24, C_DIM);
    if (focused) focusRing(x, y, w, r->h - 4, 10);
}
static void drawHero(const View *v, float x, float y, float w, bool focused) {
    const Item *item = &v->source;
    uint32_t base = coverColour(item->image, item->kind == KIND_LIKED ? 0x4B2BE6 : 0x404040);
    overlayRectGradient(x - PAD, y - 24, MAIN_W, 344, rgb(base, 0.62f, 0xFF), C_PANEL);
    drawCover(item, x, y + 24, 248, 10);
    const char *label = item->kind == KIND_ALBUM ? "\xC3\x81lbum" : item->kind == KIND_LIKED ? "Lista" : "Lista";
    overlayTextBold(x + 280, y + 36, label, 24, C_TEXT);
    overlayTextFitBold(x + 280, y + 70, item->name, 72, w - 300, C_TEXT);
    char sub[200];
    if (item->kind == KIND_LIKED) snprintf(sub, sizeof(sub), "%u canciones", v->tracks.count);
    else if (item->kind == KIND_PLAYLIST) snprintf(sub, sizeof(sub), "%s \xE2\x80\xA2 %u canciones", item->sub, item->total ? item->total : v->tracks.count);
    else snprintf(sub, sizeof(sub), "%s \xE2\x80\xA2 %u canciones", item->sub, v->tracks.count);
    overlayTextFit(x + 280, y + 158, sub, 26, w - 300, C_DIM);
    overlayCircle(x + 280 + 44, y + 252, 44, C_GREEN);
    overlayIcon(OVERLAY_ICON_PLAY, x + 280 + 47, y + 252, 44, C_BLACK);
    if (focused) {
        overlayRoundFrame(x + 280 - 4, y + 252 - 48, 96, 96, 48, 3, C_TEXT);
    }
}
static void drawMain(void) {
    overlayRoundRect(MAIN_X, SIDEBAR_Y, MAIN_W, PANEL_H, 16, C_PANEL);
    View *v = view();
    float view_h = PANEL_H;
    if (ui.row_count) {
        const Row *r = &ui.rows[v->row];
        float target = v->scroll;
        if (r->y - 20 < target) target = r->y - 20;
        if (r->y + r->h + 20 > target + view_h) target = r->y + r->h + 20 - view_h;
        if (r->kind == R_HERO || v->row == 0) target = 0;
        if (target < 0) target = 0;
        v->scroll += (target - v->scroll) * 0.35f;
        if (fabsf(target - v->scroll) < 0.5f) v->scroll = target;
    }
    overlayClip(MAIN_X, SIDEBAR_Y, MAIN_W, PANEL_H);
    float x = MAIN_X + PAD;
    for (unsigned i = 0; i < ui.row_count; ++i) {
        const Row *r = &ui.rows[i];
        float y = SIDEBAR_Y + r->y - v->scroll;
        if (y > SIDEBAR_Y + PANEL_H || y + r->h < SIDEBAR_Y) continue;
        bool row_focus = ui.focus == F_MAIN && (int)i == v->row;
        switch (r->kind) {
        case R_TILES:
            for (unsigned c = 0; c < r->count; ++c) drawTile(&r->items[c], x + (float)c * 352, y, 336, 76, row_focus && (int)c == v->col);
            break;
        case R_SHELF: {
            const float stride = 224, card = 204;
            unsigned visible = (unsigned)((CONTENT_W + 20) / stride);
            unsigned first = v->shelf_scroll[i];
            if (row_focus) {
                if ((unsigned)v->col < first) first = (unsigned)v->col;
                if ((unsigned)v->col >= first + visible) first = (unsigned)v->col - visible + 1;
                v->shelf_scroll[i] = (uint8_t)first;
            }
            for (unsigned c = first; c < r->count && c < first + visible + 1; ++c)
                drawCard(&r->items[c], x + (float)(c - first) * stride, y + 8, card, row_focus && (int)c == v->col);
            if (first > 0) overlayIcon(OVERLAY_ICON_BACK, x - 4, y + 110, 40, C_DIM);
            break;
        }
        case R_TRACK: drawTrackRow(r, x, y, CONTENT_W, row_focus); break;
        case R_HERO: drawHero(v, x, y, CONTENT_W, row_focus); break;
        case R_TITLE:
            if (r->title[0]) overlayTextBold(x, y + 14, r->title, 38, C_TEXT);
            else {
                overlayText(x + 20, y + 8, "#", 22, C_DIM);
                overlayText(x + 144, y + 8, "T\xC3\xADtulo", 22, C_DIM);
                overlayText(x + 760, y + 8, "\xC3\x81lbum", 22, C_DIM);
                overlayRect(x, y + 38, CONTENT_W, 1, 0x333333FFu);
            }
            break;
        case R_TEXT: overlayText(x, y + 20, r->title, 30, C_DIM); break;
        }
    }
    overlayClipEnd();
}
static void drawPlayer(void) {
    const Playback *p = &ui.playback;
    float py = PLAYER_Y;
    // left: what plays
    if (p->valid) {
        Item as_item = {0};
        snprintf(as_item.image, sizeof(as_item.image), "%s", p->image);
        drawCover(&as_item, 28, py + 16, 88, 8);
        overlayTextFitBold(132, py + 22, p->title, 28, 400, C_TEXT);
        overlayTextFit(132, py + 56, p->artist, 24, 400, C_DIM);
        overlayIcon(OVERLAY_ICON_HEART, 560 - 70, py + 60, 30, C_DIM);
    } else {
        overlayText(28, py + 40, ui.ready ? "Elige algo para reproducir" : "Conectando...", 28, C_FAINT);
    }
    // centre: transport
    const float cx = 960;
    const float cy = py + 36;
    uint32_t on = C_GREEN;
    overlayIcon(OVERLAY_ICON_SHUFFLE, cx - 220, cy, 38, p->shuffle ? on : C_DIM);
    overlayIcon(OVERLAY_ICON_PREV, cx - 110, cy, 44, C_TEXT);
    overlayCircle(cx, cy, 30, C_TEXT);
    overlayIcon(p->playing ? OVERLAY_ICON_PAUSE : OVERLAY_ICON_PLAY, cx + (p->playing ? 0 : 2), cy, 36, C_BLACK);
    overlayIcon(OVERLAY_ICON_NEXT, cx + 110, cy, 44, C_TEXT);
    overlayIcon(p->repeat == 2 ? OVERLAY_ICON_REPEAT_ONE : OVERLAY_ICON_REPEAT, cx + 220, cy, 38, p->repeat ? on : C_DIM);
    if (ui.focus == F_PLAYER) {
        static const float offsets[5] = {-220, -110, 0, 110, 220};
        overlayRoundFrame(cx + offsets[ui.player_cursor] - 36, cy - 36, 72, 72, 36, 3, C_TEXT);
    }
    // progress
    float now_ms = progressNow();
    char left[16], right[16];
    formatTime((unsigned)now_ms, left, sizeof(left));
    formatTime(p->duration_ms, right, sizeof(right));
    const float bar_x = cx - 340, bar_w = 680, bar_y = py + 96;
    overlayText(bar_x - 18 - overlayTextWidth(left, 22), bar_y - 14, left, 22, C_DIM);
    overlayText(bar_x + bar_w + 18, bar_y - 14, right, 22, C_DIM);
    overlayRoundRect(bar_x, bar_y - 3, bar_w, 6, 3, 0x4D4D4DFFu);
    float fraction = p->duration_ms ? now_ms / (float)p->duration_ms : 0;
    if (fraction > 0) overlayRoundRect(bar_x, bar_y - 3, bar_w * fraction, 6, 3, C_TEXT);
    if (p->valid) overlayCircle(bar_x + bar_w * fraction, bar_y, 7, C_TEXT);
    // right: volume
    overlayIcon(OVERLAY_ICON_QUEUE, 1500, py + 40, 34, C_DIM);
    overlayIcon(p->volume == 0 ? OVERLAY_ICON_MUTE : OVERLAY_ICON_VOLUME, 1570, py + 40, 36, C_DIM);
    overlayRoundRect(1606, py + 37, 220, 6, 3, 0x4D4D4DFFu);
    int volume = p->volume < 0 ? 70 : p->volume;
    overlayRoundRect(1606, py + 37, 220.0f * (float)volume / 100.0f, 6, 3, C_TEXT);
    overlayCircle(1606 + 220.0f * (float)volume / 100.0f, py + 40, 7, C_TEXT);
}
static void drawStrip(void) {
    overlayRect(0, STRIP_Y, 1920, 36, 0x0B0B0BFFu);
    float x = 28;
    static const struct { const char *button; const char *text; } hints[] = {
        {"\x02", "Atr\xC3\xA1s"}, {"\x01", "Elegir"}, {"\x03", "Pausa"}, {"\x04", "Buscar"}, {"L1 R1", "Anterior / siguiente"}, {"L2 R2", "Volumen"}, {"OPTIONS", "Aleatorio"}};
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); ++i) {
        overlayText(x, STRIP_Y + 6, hints[i].button, 24, C_TEXT);
        x += overlayTextWidth(hints[i].button, 24) + 8;
        overlayText(x, STRIP_Y + 6, hints[i].text, 22, C_DIM);
        x += overlayTextWidth(hints[i].text, 22) + 34;
    }
    if (ui.playback.valid && ui.playback.playing && ui.playback.on_this_device) {
        const char *text = "Reproduciendo en Spotrps5";
        float w = overlayTextBoldWidth(text, 22) + 56;
        overlayRoundRect(1900 - w, STRIP_Y + 3, w, 30, 15, C_GREEN);
        overlayIcon(OVERLAY_ICON_EQUALIZER, 1900 - w + 22, STRIP_Y + 18, 22, C_BLACK);
        overlayTextBold(1900 - w + 40, STRIP_Y + 6, text, 22, C_BLACK);
    }
}
static void drawKeyboard(void) {
    overlayRect(0, 0, 1920, 1080, 0x000000C8u);
    const float key = 96, gap = 10, w = KEY_COLS * key + (KEY_COLS - 1) * gap + 80, x0 = (1920 - w) / 2, y0 = 190;
    overlayRoundRect(x0, y0, w, 730, 24, 0x1A1A1AFFu);
    overlayRoundRect(x0 + 40, y0 + 36, w - 80, 76, 38, C_RAISED);
    overlayIcon(OVERLAY_ICON_SEARCH, x0 + 84, y0 + 74, 34, C_DIM);
    float tw = overlayText(x0 + 116, y0 + 74 - 17, ui.typed, 34, C_TEXT);
    if (!ui.typed[0]) overlayText(x0 + 116, y0 + 74 - 17, "Artistas, canciones o listas", 34, C_FAINT);
    if (((ui.now / 500000000ull) & 1) == 0) overlayRect(x0 + 118 + tw, y0 + 74 - 20, 3, 40, C_GREEN);
    for (int r = 0; r < KEY_ROWS; ++r) {
        const char *p = key_rows[r];
        for (int c = 0; c < KEY_COLS && *p; ++c) {
            size_t len = ((unsigned char)*p >= 0xC0) ? 2 : 1;
            char one[4] = {0};
            memcpy(one, p, len);
            p += len;
            float x = x0 + 40 + (float)c * (key + gap), y = y0 + 144 + (float)r * (key - 8 + gap);
            bool focused = ui.key_row == r && ui.key_col == c;
            overlayRoundRect(x, y, key, key - 8, 14, focused ? C_GREEN : C_RAISED);
            overlayTextCentered(x + key / 2, y + (key - 8) / 2 - 18, one, 36, focused ? C_BLACK : C_TEXT);
        }
    }
    const float by = y0 + 144 + 4 * (key - 8 + gap);
    struct { const char *label; int c0, c1; } bottoms[3] = {{"Espacio", 0, 5}, {"Borrar", 6, 7}, {"Buscar", 8, 9}};
    for (int b = 0; b < 3; ++b) {
        float x = x0 + 40 + (float)bottoms[b].c0 * (key + gap), width = (float)(bottoms[b].c1 - bottoms[b].c0 + 1) * (key + gap) - gap;
        bool focused = ui.key_row == KEY_ROWS && ui.key_col >= bottoms[b].c0 && ui.key_col <= bottoms[b].c1;
        overlayRoundRect(x, by, width, key - 8, 14, focused ? C_GREEN : (b == 2 ? 0x2E6B45FFu : C_RAISED));
        overlayTextCentered(x + width / 2, by + (key - 8) / 2 - 16, bottoms[b].label, 32, focused ? C_BLACK : C_TEXT);
    }
    overlayText(x0 + 40, y0 + 680, "\x01 Teclear   \x03 Borrar   \x04 Espacio   OPTIONS Buscar   \x02 Cerrar", 24, C_DIM);
}
static void drawToast(void) {
    if (ui.now >= ui.toast_until || !ui.toast[0]) return;
    float w = overlayTextWidth(ui.toast, 28) + 60;
    overlayRoundRect(960 - w / 2, 960, w, 56, 28, C_TEXT);
    overlayTextCentered(960, 960 + 11, ui.toast, 28, C_BLACK);
}

// ---- public ----------------------------------------------------------------------------------------------------------------------
void uiInit(void) {
    memset(&ui, 0, sizeof(ui));
    ui.liked.kind = KIND_LIKED;
    snprintf(ui.liked.name, sizeof(ui.liked.name), "Canciones que te gustan");
    snprintf(ui.liked.uri, sizeof(ui.liked.uri), "spotrps5:liked");
    ui.depth = 1;
    ui.stack[0].type = V_HOME;
    ui.rows_dirty = true;
    ui.focus = F_MAIN;
    ui.playback.volume = -1;
}
bool uiReady(void) { return ui.ready; }
bool uiWantsQuit(void) { return ui.quit_wanted; }
void uiSetDiagnostics(const char *line) { snprintf(ui.diagnostics, sizeof(ui.diagnostics), "%s", line ? line : ""); }
void uiUpdate(const PlatformPad *pad, uint64_t now_ns) {
    ui.now = now_ns;
    handleResults();
    if (ui.ready) {
        if (ui.rows_dirty) buildRows();
        handleInput(pad);
        if (ui.rows_dirty) buildRows();
    }
    ui.held = pad->connected ? pad->buttons : 0;
    if (ui.last_poll_request == 0 && ui.ready) {  // after a command: look at the state again in a moment
        ui.last_poll_request = now_ns + 900000000ull;
    } else if (ui.ready && ui.last_poll_request && now_ns >= ui.last_poll_request && now_ns - ui.last_poll_request < 5000000000ull) {
        apiRequestPlayback();
        ui.last_poll_request = 1;  // any non-zero value that is in the past: done
    }
}
void uiDraw(uint64_t now_ns) {
    ui.now = now_ns;
    ++frame_number;
    overlayClear(C_BG);
    if (!ui.ready) return;
    drawTop();
    drawSidebar();
    drawMain();
    drawPlayer();
    drawStrip();
    if (ui.keyboard) drawKeyboard();
    drawToast();
}
