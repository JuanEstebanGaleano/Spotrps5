// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
#include "api.h"
#include "diagnostics.h"
#include "game.h"
#include "jpeg.h"
#include "json.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_NAME "Spotrps5"
#define API "https://api.spotify.com/v1"
#define IMAGE_WORKERS 2
#define MAX_BODY (3u * 1024 * 1024)
#define MAX_IMAGE (600u * 1024)

typedef enum { JOB_LIBRARY, JOB_TRACKS, JOB_SEARCH, JOB_PLAYBACK, JOB_PLAY, JOB_IMAGE } JobType;
typedef struct Job {
    JobType type;
    ItemKind kind;
    unsigned tag;
    char text[200];   // id / query / url / context uri
    char offset[80];  // JOB_PLAY: the track to start at
    char *uris;       // JOB_PLAY: the uris, separated by '\n'
    struct Job *next;
} Job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t api_wake = PTHREAD_COND_INITIALIZER, image_wake = PTHREAD_COND_INITIALIZER;
static Job *api_head, *api_tail, *image_head, *image_tail;
typedef struct ResultNode {
    ApiResult result;
    struct ResultNode *next;
} ResultNode;
static ResultNode *result_head, *result_tail;
static char cache_file[300];       // where this program reads the token file
static char cache_file_player[] = "/home/ps5/cache/token.txt";  // the same file as librespot sees it
static bool started;

// ---- small helpers ----------------------------------------------------------------------------------------------------------------------
static void copy(char *to, size_t size, const char *from) {
    if (!from) from = "";
    size_t n = strlen(from);
    if (n >= size) {
        n = size - 1;
        while (n && (from[n] & 0xC0) == 0x80) --n;  // not in the middle of a UTF-8 sequence
    }
    memcpy(to, from, n);
    to[n] = 0;
}
static void post(ResultNode *node) {
    pthread_mutex_lock(&lock);
    node->next = NULL;
    if (result_tail) result_tail->next = node;
    else result_head = node;
    result_tail = node;
    pthread_mutex_unlock(&lock);
}
static ResultNode *newResult(ResultType type, unsigned tag) {
    ResultNode *node = calloc(1, sizeof(*node));
    if (node) {
        node->result.type = type;
        node->result.tag = tag;
    }
    return node;
}
static void status(const char *format, const char *a) {
    ResultNode *node = newResult(RES_STATUS, 0);
    if (!node) return;
    snprintf(node->result.text, sizeof(node->result.text), format, a ? a : "");
    post(node);
}
void apiResultFree(ApiResult *r) {
    free(r->list.items);
    for (unsigned i = 0; i < 3; ++i) free(r->more[i].items);
    free(r->pixels);
    memset(r, 0, sizeof(*r));
}
bool apiTakeResult(ApiResult *out) {
    pthread_mutex_lock(&lock);
    ResultNode *node = result_head;
    if (node) {
        result_head = node->next;
        if (!result_head) result_tail = NULL;
    }
    pthread_mutex_unlock(&lock);
    if (!node) return false;
    *out = node->result;
    free(node);
    return true;
}
static void enqueue(Job **head, Job **tail, pthread_cond_t *wake, Job *job, bool front) {
    pthread_mutex_lock(&lock);
    job->next = NULL;
    if (front && *head) {
        job->next = *head;
        *head = job;
    } else if (*tail) {
        (*tail)->next = job;
        *tail = job;
    } else {
        *head = *tail = job;
    }
    if (!(*tail)) *tail = job;
    pthread_cond_signal(wake);
    pthread_mutex_unlock(&lock);
}
static Job *newJob(JobType type) {
    Job *job = calloc(1, sizeof(*job));
    if (job) job->type = type;
    return job;
}
static void submit(Job *job) {
    if (!job) return;
    if (!started) {
        free(job->uris);
        free(job);
        return;
    }
    enqueue(&api_head, &api_tail, &api_wake, job, false);
}

// ---- token --------------------------------------------------------------------------------------------------------------------------
static char token[1100];
static uint64_t token_valid_until_ns;
static unsigned nonce;
static uint64_t now(void) { return platformMonotonicNs(); }

// Asks librespot (the "token" command) and reads the file it writes. False when it gives none.
static bool tokenRefresh(void) {
    remove(cache_file);
    char command[400];
    snprintf(command, sizeof(command), "token %s %u", cache_file_player, ++nonce);
    gameSendLine(command);
    for (unsigned waited = 0; waited < 150; ++waited) {  // up to 15 s
        platformSleepNs(100u * 1000u * 1000u);
        FILE *file = fopen(cache_file, "rb");
        if (!file) continue;
        char text[1400];
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        if (!strstr(text, "\nEND\n")) continue;
        char expected[24], *line = text;
        snprintf(expected, sizeof(expected), "%u", nonce);
        char *first = strsep(&line, "\n"), *seconds = strsep(&line, "\n"), *value = strsep(&line, "\n");
        if (!first || !seconds || !value || strcmp(first, expected)) continue;
        if (!strcmp(value, "ERROR") || strlen(value) < 20 || strlen(value) >= sizeof(token)) return false;
        copy(token, sizeof(token), value);
        long left = atol(seconds);
        if (left < 120) left = 120;
        token_valid_until_ns = now() + (uint64_t)(left - 60) * 1000000000ull;
        return true;
    }
    return false;
}
static bool tokenEnsure(void) {
    if (token[0] && now() < token_valid_until_ns) return true;
    return tokenRefresh();
}

// ---- HTTP ---------------------------------------------------------------------------------------------------------------------------
// Returns the HTTP status (0: no answer); *out is the body, NUL-terminated, malloc'd.
static int fetch(const char *method, const char *url, bool auth, const char *body, char **out, size_t *out_length, size_t limit) {
    *out = NULL;
    if (out_length) *out_length = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        PlatformHttpHeader headers[3];
        unsigned count = 0;
        char bearer[1200];
        if (auth) {
            if (!tokenEnsure()) return 401;
            snprintf(bearer, sizeof(bearer), "Bearer %s", token);
            headers[count++] = (PlatformHttpHeader){"Authorization", bearer};
        }
        if (body) headers[count++] = (PlatformHttpHeader){"Content-Type", "application/json"};
        int code = 0;
        char error[120] = "";
        PlatformHttp *http = platformHttpSend(method, url, headers, count, body, body ? strlen(body) : 0, &code, error, sizeof(error));
        if (!http) {
            diagnosticsTrace("api: %s %s failed: %s", method, url, error);
            return 0;
        }
        size_t capacity = 16384, used = 0;
        char *buffer = malloc(capacity);
        bool ok = buffer != NULL;
        while (ok) {
            if (used + 8192 + 1 > capacity) {
                if (capacity >= limit) { ok = false; break; }
                char *bigger = realloc(buffer, capacity * 2);
                if (!bigger) { ok = false; break; }
                buffer = bigger;
                capacity *= 2;
            }
            int64_t n = platformHttpRead(http, buffer + used, capacity - used - 1);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            used += (size_t)n;
        }
        platformHttpClose(http);
        if (!ok) {
            free(buffer);
            return 0;
        }
        buffer[used] = 0;
        if (code == 401 && auth && attempt == 0) {  // the token was refused: ask for a new one once
            free(buffer);
            token[0] = 0;
            continue;
        }
        *out = buffer;
        if (out_length) *out_length = used;
        return code;
    }
    return 401;
}
static JsonDocument *getJson(const char *path, int *code_out) {
    char url[600];
    snprintf(url, sizeof(url), "%s%s", API, path);
    char *body;
    size_t length;
    int code = fetch("GET", url, true, NULL, &body, &length, MAX_BODY);
    if (code_out) *code_out = code;
    JsonDocument *document = NULL;
    if (code >= 200 && code < 300 && body && length) document = jsonParse(body, length);
    if (code >= 300 || (code >= 200 && length && !document && code != 204)) diagnosticsTrace("api: GET %s -> %d", path, code);
    free(body);
    return document;
}
static void escapeQuery(char *to, size_t size, const char *from) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)from; *p && n + 4 < size; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' || *p == '.') to[n++] = (char)*p;
        else if (*p == ' ') to[n++] = '+';
        else { to[n++] = '%'; to[n++] = hex[*p >> 4]; to[n++] = hex[*p & 15]; }
    }
    to[n] = 0;
}

// ---- reading the answers --------------------------------------------------------------------------------------------------------
// The smallest picture that is at least 200 pixels wide, else the biggest one.
static void pickImage(const Json *images, char *to, size_t size) {
    to[0] = 0;
    double best = 1e9, biggest = -1;
    const char *best_url = NULL, *biggest_url = NULL;
    for (const Json *i = images ? images->first : NULL; i; i = i->next) {
        const char *url = jsonString(jsonGet(i, "url"), NULL);
        if (!url) continue;
        double width = jsonNumber(jsonGet(i, "width"), 300);
        if (width >= 200 && width < best) { best = width; best_url = url; }
        if (width > biggest) { biggest = width; biggest_url = url; }
    }
    copy(to, size, best_url ? best_url : biggest_url);
}
static void artists(const Json *array, char *to, size_t size) {
    to[0] = 0;
    for (const Json *a = array ? array->first : NULL; a; a = a->next) {
        const char *name = jsonString(jsonGet(a, "name"), NULL);
        if (!name) continue;
        size_t used = strlen(to);
        if (used + 3 >= size) break;
        snprintf(to + used, size - used, "%s%s", used ? ", " : "", name);
    }
}
static bool addItem(List *list, unsigned *capacity, const Item *item) {
    if (list->count == *capacity) {
        unsigned bigger = *capacity ? *capacity * 2 : 32;
        Item *items = realloc(list->items, bigger * sizeof(Item));
        if (!items) return false;
        list->items = items;
        *capacity = bigger;
    }
    list->items[list->count++] = *item;
    return true;
}
static void readTrack(const Json *t, Item *item) {
    memset(item, 0, sizeof(*item));
    item->kind = KIND_TRACK;
    copy(item->id, sizeof(item->id), jsonString(jsonGet(t, "id"), ""));
    copy(item->uri, sizeof(item->uri), jsonString(jsonGet(t, "uri"), ""));
    copy(item->name, sizeof(item->name), jsonString(jsonGet(t, "name"), ""));
    artists(jsonGet(t, "artists"), item->sub, sizeof(item->sub));
    copy(item->album, sizeof(item->album), jsonString(jsonPath(t, "album.name"), ""));
    pickImage(jsonPath(t, "album.images"), item->image, sizeof(item->image));
    item->duration_ms = (unsigned)jsonNumber(jsonGet(t, "duration_ms"), 0);
}
static void readAlbum(const Json *a, Item *item) {
    memset(item, 0, sizeof(*item));
    item->kind = KIND_ALBUM;
    copy(item->id, sizeof(item->id), jsonString(jsonGet(a, "id"), ""));
    copy(item->uri, sizeof(item->uri), jsonString(jsonGet(a, "uri"), ""));
    copy(item->name, sizeof(item->name), jsonString(jsonGet(a, "name"), ""));
    artists(jsonGet(a, "artists"), item->sub, sizeof(item->sub));
    pickImage(jsonGet(a, "images"), item->image, sizeof(item->image));
    item->total = (unsigned)jsonNumber(jsonGet(a, "total_tracks"), 0);
}
static void readPlaylist(const Json *p, Item *item) {
    memset(item, 0, sizeof(*item));
    item->kind = KIND_PLAYLIST;
    copy(item->id, sizeof(item->id), jsonString(jsonGet(p, "id"), ""));
    copy(item->uri, sizeof(item->uri), jsonString(jsonGet(p, "uri"), ""));
    copy(item->name, sizeof(item->name), jsonString(jsonGet(p, "name"), ""));
    copy(item->sub, sizeof(item->sub), jsonString(jsonPath(p, "owner.display_name"), ""));
    pickImage(jsonGet(p, "images"), item->image, sizeof(item->image));
    const Json *total = jsonPath(p, "items.total");
    if (!total) total = jsonPath(p, "tracks.total");
    item->total = (unsigned)jsonNumber(total, 0);
}
static void readArtist(const Json *a, Item *item) {
    memset(item, 0, sizeof(*item));
    item->kind = KIND_ARTIST;
    copy(item->id, sizeof(item->id), jsonString(jsonGet(a, "id"), ""));
    copy(item->uri, sizeof(item->uri), jsonString(jsonGet(a, "uri"), ""));
    copy(item->name, sizeof(item->name), jsonString(jsonGet(a, "name"), ""));
    pickImage(jsonGet(a, "images"), item->image, sizeof(item->image));
}
typedef void (*Reader)(const Json *, Item *);
// Reads `array` (items) into the list; `inner`: the member holding the object ("track", "item"), or NULL when the element is the object.
static void readList(const Json *array, const char *inner, Reader reader, List *list) {
    unsigned capacity = 0;
    for (const Json *e = array ? array->first : NULL; e; e = e->next) {
        const Json *object = inner ? jsonGet(e, inner) : e;
        if (!object || object->type != JSON_OBJECT) continue;
        Item item;
        reader(object, &item);
        if (!item.uri[0]) continue;
        if (!addItem(list, &capacity, &item)) break;
    }
}

// ---- jobs ---------------------------------------------------------------------------------------------------------------------------
static void doLibrary(void) {
    int code;
    JsonDocument *d = getJson("/me/playlists?limit=50", &code);
    ResultNode *node = newResult(RES_PLAYLISTS, 0);
    if (node) {
        if (d) readList(jsonGet(jsonRoot(d), "items"), NULL, readPlaylist, &node->result.list);
        node->result.failed = !d;
        post(node);
    }
    jsonFree(d);
    d = getJson("/me/albums?limit=30", &code);
    if ((node = newResult(RES_ALBUMS, 0))) {
        if (d) readList(jsonGet(jsonRoot(d), "items"), "album", readAlbum, &node->result.list);
        node->result.failed = !d;
        post(node);
    }
    jsonFree(d);
    d = getJson("/me/player/recently-played?limit=40", &code);
    if ((node = newResult(RES_RECENT, 0))) {
        if (d) {
            // The recently played tracks, as their albums (each once).
            List all = {0};
            unsigned capacity = 0;
            for (const Json *e = jsonGet(jsonRoot(d), "items") ? jsonGet(jsonRoot(d), "items")->first : NULL; e; e = e->next) {
                const Json *album = jsonPath(e, "track.album");
                if (!album) continue;
                Item item;
                readAlbum(album, &item);
                bool seen = false;
                for (unsigned i = 0; i < all.count; ++i) seen |= !strcmp(all.items[i].uri, item.uri);
                if (!seen && item.uri[0]) addItem(&all, &capacity, &item);
            }
            node->result.list = all;
        }
        node->result.failed = !d;
        post(node);
    }
    jsonFree(d);
}
static void doTracks(const Job *job) {
    char path[300];
    ResultNode *node = newResult(RES_TRACKS, job->tag);
    if (!node) return;
    int code = 0;
    JsonDocument *d = NULL;
    if (job->kind == KIND_LIKED) {
        d = getJson("/me/tracks?limit=50", &code);
        if (d) readList(jsonGet(jsonRoot(d), "items"), "track", readTrack, &node->result.list);
    } else if (job->kind == KIND_ALBUM) {
        snprintf(path, sizeof(path), "/albums/%s", job->text);
        d = getJson(path, &code);  // the album itself carries its tracks and its cover
        if (d) {
            const Json *album = jsonRoot(d);
            Item cover;
            readAlbum(album, &cover);
            const Json *tracks = jsonPath(album, "tracks.items");
            unsigned capacity = 0;
            for (const Json *t = tracks ? tracks->first : NULL; t; t = t->next) {
                Item item;
                readTrack(t, &item);
                copy(item.album, sizeof(item.album), cover.name);
                copy(item.image, sizeof(item.image), cover.image);
                if (item.uri[0]) addItem(&node->result.list, &capacity, &item);
            }
        }
    } else {
        snprintf(path, sizeof(path), "/playlists/%s/items?limit=100", job->text);
        d = getJson(path, &code);
        if (!d && (code == 404 || code == 410 || code == 400)) {  // the older route
            snprintf(path, sizeof(path), "/playlists/%s/tracks?limit=100", job->text);
            d = getJson(path, &code);
        }
        if (d) {
            const Json *items = jsonGet(jsonRoot(d), "items");
            unsigned capacity = 0;
            for (const Json *e = items ? items->first : NULL; e; e = e->next) {
                const Json *t = jsonGet(e, "item");
                if (!t || t->type != JSON_OBJECT) t = jsonGet(e, "track");
                if (!t || t->type != JSON_OBJECT) continue;
                if (strcmp(jsonString(jsonGet(t, "type"), "track"), "track")) continue;  // episodes and the like
                Item item;
                readTrack(t, &item);
                if (item.uri[0]) addItem(&node->result.list, &capacity, &item);
            }
        }
    }
    node->result.failed = !d;
    jsonFree(d);
    post(node);
}
static void doSearch(const Job *job) {
    char query[300], path[500];
    escapeQuery(query, sizeof(query), job->text);
    snprintf(path, sizeof(path), "/search?q=%s&type=track,album,playlist,artist&limit=10", query);
    int code;
    JsonDocument *d = getJson(path, &code);
    ResultNode *node = newResult(RES_SEARCH, job->tag);
    if (!node) {
        jsonFree(d);
        return;
    }
    if (d) {
        const Json *r = jsonRoot(d);
        readList(jsonPath(r, "tracks.items"), NULL, readTrack, &node->result.list);
        readList(jsonPath(r, "albums.items"), NULL, readAlbum, &node->result.more[0]);
        readList(jsonPath(r, "playlists.items"), NULL, readPlaylist, &node->result.more[1]);
        readList(jsonPath(r, "artists.items"), NULL, readArtist, &node->result.more[2]);
    }
    node->result.failed = !d;
    jsonFree(d);
    post(node);
}
static void doPlayback(void) {
    int code;
    JsonDocument *d = getJson("/me/player", &code);
    ResultNode *node = newResult(RES_PLAYBACK, 0);
    if (!node) {
        jsonFree(d);
        return;
    }
    Playback *p = &node->result.playback;
    p->volume = -1;
    if (d) {
        const Json *r = jsonRoot(d), *t = jsonGet(r, "item");
        if (t && t->type == JSON_OBJECT) {
            p->valid = true;
            copy(p->title, sizeof(p->title), jsonString(jsonGet(t, "name"), ""));
            artists(jsonGet(t, "artists"), p->artist, sizeof(p->artist));
            copy(p->album, sizeof(p->album), jsonString(jsonPath(t, "album.name"), ""));
            copy(p->uri, sizeof(p->uri), jsonString(jsonGet(t, "uri"), ""));
            pickImage(jsonPath(t, "album.images"), p->image, sizeof(p->image));
            p->duration_ms = (unsigned)jsonNumber(jsonGet(t, "duration_ms"), 0);
        }
        p->playing = jsonBool(jsonGet(r, "is_playing"), false);
        p->progress_ms = (unsigned)jsonNumber(jsonGet(r, "progress_ms"), 0);
        p->shuffle = jsonBool(jsonGet(r, "shuffle_state"), false);
        const char *repeat = jsonString(jsonGet(r, "repeat_state"), "off");
        p->repeat = !strcmp(repeat, "track") ? 2 : !strcmp(repeat, "context") ? 1 : 0;
        p->volume = (int)jsonNumber(jsonPath(r, "device.volume_percent"), -1);
        copy(p->device, sizeof(p->device), jsonString(jsonPath(r, "device.name"), ""));
        p->on_this_device = !strcmp(p->device, DEVICE_NAME);
    }
    node->result.failed = !d && code != 204;
    jsonFree(d);
    post(node);
}
static char device_id[64];
static bool findDevice(void) {
    int code;
    JsonDocument *d = getJson("/me/player/devices", &code);
    bool found = false;
    const Json *devices = d ? jsonGet(jsonRoot(d), "devices") : NULL;
    for (const Json *e = devices ? devices->first : NULL; e; e = e->next)
        if (!strcmp(jsonString(jsonGet(e, "name"), ""), DEVICE_NAME)) {
            copy(device_id, sizeof(device_id), jsonString(jsonGet(e, "id"), ""));
            found = device_id[0] != 0;
            break;
        }
    jsonFree(d);
    return found;
}
static void appendJsonString(char *to, size_t size, const char *text) {
    size_t n = strlen(to);
    if (n + 2 >= size) return;
    to[n++] = '"';
    for (; *text && n + 3 < size; ++text) {
        if (*text == '"' || *text == '\\') to[n++] = '\\';
        if ((unsigned char)*text < 32) continue;
        to[n++] = *text;
    }
    to[n++] = '"';
    to[n] = 0;
}
static void doPlay(const Job *job) {
    if (!device_id[0] && !findDevice()) {
        platformSleepNs(1500u * 1000u * 1000u);
        if (!findDevice()) {
            status("No encuentro el dispositivo %s en tu cuenta", DEVICE_NAME);
            return;
        }
    }
    size_t size = 400 + (job->uris ? strlen(job->uris) * 2 : 0);
    char *body = calloc(1, size), url[200];
    if (!body) return;
    strcat(body, "{");
    if (job->uris) {
        strcat(body, "\"uris\":[");
        char *copyOf = strdup(job->uris), *save = NULL;
        bool first = true;
        for (char *u = copyOf ? strtok_r(copyOf, "\n", &save) : NULL; u; u = strtok_r(NULL, "\n", &save)) {
            if (!first) strcat(body, ",");
            first = false;
            appendJsonString(body, size, u);
        }
        free(copyOf);
        strcat(body, "]");
    } else {
        strcat(body, "\"context_uri\":");
        appendJsonString(body, size, job->text);
    }
    if (job->offset[0]) {
        strcat(body, ",\"offset\":{\"uri\":");
        appendJsonString(body, size, job->offset);
        strcat(body, "}");
    }
    strcat(body, "}");
    snprintf(url, sizeof(url), "%s/me/player/play?device_id=%s", API, device_id);
    char *answer;
    int code = fetch("PUT", url, true, body, &answer, NULL, 65536);
    if (code == 404) {  // the device id changed (the program restarted): look again, once
        device_id[0] = 0;
        free(answer);
        if (findDevice()) {
            snprintf(url, sizeof(url), "%s/me/player/play?device_id=%s", API, device_id);
            code = fetch("PUT", url, true, body, &answer, NULL, 65536);
        }
    }
    if (code < 200 || code >= 300) {
        diagnosticsTrace("api: play -> %d %.160s", code, answer ? answer : "");
        status("No se pudo reproducir (error %s)", code == 403 ? "403: ¿cuenta Premium?" : code == 0 ? "de red" : "de Spotify");
    }
    free(answer);
    free(body);
}
static void doImage(const Job *job) {
    ResultNode *node = newResult(RES_IMAGE, 0);
    if (!node) return;
    copy(node->result.text, sizeof(node->result.text), job->text);
    char *body;
    size_t length;
    int code = fetch("GET", job->text, false, NULL, &body, &length, MAX_IMAGE);
    if (code == 200 && body) node->result.pixels = jpegDecode((const uint8_t *)body, length, &node->result.image_width, &node->result.image_height);
    if (!node->result.pixels) diagnosticsTrace("api: image %.80s -> %d (%zu bytes) not shown", job->text, code, length);
    free(body);
    post(node);
}

static void *apiThread(void *unused) {
    (void)unused;
    uint64_t last_poll = 0;
    bool ready = false;
    for (;;) {
        pthread_mutex_lock(&lock);
        Job *job = api_head;
        if (!job) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            pthread_cond_timedwait(&api_wake, &lock, &until);
            job = api_head;
        }
        if (job) {
            api_head = job->next;
            if (!api_head) api_tail = NULL;
        }
        pthread_mutex_unlock(&lock);
        if (!ready) {  // nothing works before librespot has a session: ask for a token every few seconds
            static uint64_t last_try;
            if (!last_try || now() - last_try > 4000000000ull) {
                last_try = now();
                ready = tokenEnsure();
                if (ready) {
                    ResultNode *node = newResult(RES_READY, 0);
                    if (node) post(node);
                }
            }
            if (!ready) {
                if (job) {  // the request is kept for later
                    enqueue(&api_head, &api_tail, &api_wake, job, true);
                    platformSleepNs(300u * 1000u * 1000u);
                }
                continue;
            }
        }
        if (job) {
            switch (job->type) {
            case JOB_LIBRARY: doLibrary(); break;
            case JOB_TRACKS: doTracks(job); break;
            case JOB_SEARCH: doSearch(job); break;
            case JOB_PLAYBACK: doPlayback(); last_poll = now(); break;
            case JOB_PLAY: doPlay(job); break;
            default: break;
            }
            free(job->uris);
            free(job);
        } else if (now() - last_poll > 2500000000ull) {
            doPlayback();
            last_poll = now();
        }
    }
    return NULL;
}
static void *imageThread(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!image_head) pthread_cond_wait(&image_wake, &lock);
        Job *job = image_head;
        image_head = job->next;
        if (!image_head) image_tail = NULL;
        pthread_mutex_unlock(&lock);
        if (token[0]) doImage(job);  // covers wait for the first token too, so that the first requests (the library) go first
        else {
            platformSleepNs(500u * 1000u * 1000u);
            enqueue(&image_head, &image_tail, &image_wake, job, false);
            continue;
        }
        free(job);
    }
    return NULL;
}

void apiStart(const char *root) {
    if (started) return;
    snprintf(cache_file, sizeof(cache_file), "%s%s", root, cache_file_player);
    started = true;
    pthread_t thread;
    pthread_create(&thread, NULL, apiThread, NULL);
    for (unsigned i = 0; i < IMAGE_WORKERS; ++i) pthread_create(&thread, NULL, imageThread, NULL);
}
void apiRequestLibrary(void) { submit(newJob(JOB_LIBRARY)); }
void apiRequestTracks(ItemKind kind, const char *id, unsigned tag) {
    Job *job = newJob(JOB_TRACKS);
    if (!job) return;
    job->kind = kind;
    job->tag = tag;
    copy(job->text, sizeof(job->text), id);
    submit(job);
}
void apiRequestSearch(const char *query, unsigned tag) {
    Job *job = newJob(JOB_SEARCH);
    if (!job) return;
    job->tag = tag;
    copy(job->text, sizeof(job->text), query);
    submit(job);
}
void apiRequestPlayback(void) { submit(newJob(JOB_PLAYBACK)); }
void apiRequestImage(const char *url) {
    Job *job = newJob(JOB_IMAGE);
    if (!job || !started) {
        free(job);
        return;
    }
    copy(job->text, sizeof(job->text), url);
    enqueue(&image_head, &image_tail, &image_wake, job, false);
}
void apiPlay(const char *context, const char *const *uris, unsigned uri_count, const char *offset_uri) {
    Job *job = newJob(JOB_PLAY);
    if (!job) return;
    if (uri_count) {
        size_t size = 1;
        for (unsigned i = 0; i < uri_count; ++i) size += strlen(uris[i]) + 1;
        job->uris = malloc(size);
        if (!job->uris) {
            free(job);
            return;
        }
        job->uris[0] = 0;
        for (unsigned i = 0; i < uri_count; ++i) {
            strcat(job->uris, uris[i]);
            strcat(job->uris, "\n");
        }
    } else {
        copy(job->text, sizeof(job->text), context ? context : "");
    }
    copy(job->offset, sizeof(job->offset), offset_uri);
    submit(job);
}
