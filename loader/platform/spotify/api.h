// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
//
// Spotify's Web API for the screens: a worker thread asks (library, playlists, search, what is playing, covers) and the main thread
// takes the answers one by one. The token comes from librespot's own session (the "token" command, tools/patch_librespot.py), so no
// separate login is needed. Everything here is thread safe from the caller's side: request functions only queue.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { KIND_TRACK, KIND_ALBUM, KIND_PLAYLIST, KIND_ARTIST, KIND_LIKED } ItemKind;
typedef struct {
    ItemKind kind;
    char id[40];
    char uri[80];
    char name[112];
    char sub[112];    // artists, owner, ...
    char album[80];   // tracks: the album's name
    char image[176];  // cover URL ("" when none)
    unsigned duration_ms, total;
} Item;
typedef struct {
    Item *items;
    unsigned count;
} List;
typedef struct {
    bool valid, playing, shuffle, on_this_device;
    int repeat;  // 0 off, 1 all, 2 one
    int volume;  // percent, -1 unknown
    unsigned progress_ms, duration_ms;
    char title[112], artist[112], album[80], image[176], uri[80], device[48];
} Playback;

typedef enum { RES_PLAYLISTS, RES_ALBUMS, RES_RECENT, RES_TRACKS, RES_SEARCH, RES_PLAYBACK, RES_IMAGE, RES_STATUS, RES_READY } ResultType;
typedef struct {
    ResultType type;
    unsigned tag;      // RES_TRACKS / RES_SEARCH: the tag given with the request
    List list;         // RES_PLAYLISTS/ALBUMS/RECENT/TRACKS; RES_SEARCH: tracks
    List more[3];      // RES_SEARCH: albums, playlists, artists
    bool failed;       // the request did not work (list is empty)
    Playback playback; // RES_PLAYBACK
    char text[200];    // RES_STATUS: a message; RES_IMAGE: the URL
    unsigned image_width, image_height;
    uint32_t *pixels;  // RES_IMAGE: RGBA, malloc'd (NULL: could not be decoded)
} ApiResult;

void apiStart(const char *root);  // `root`: where librespot's cache directory is, as this program sees it
void apiResultFree(ApiResult *result);
bool apiTakeResult(ApiResult *result);  // false when there is none

void apiRequestLibrary(void);                                      // RES_PLAYLISTS, RES_ALBUMS, RES_RECENT
void apiRequestTracks(ItemKind kind, const char *id, unsigned tag);  // KIND_PLAYLIST / KIND_ALBUM / KIND_LIKED -> RES_TRACKS
void apiRequestSearch(const char *query, unsigned tag);            // RES_SEARCH
void apiRequestPlayback(void);                                     // RES_PLAYBACK (also asked by itself every few seconds)
void apiRequestImage(const char *url);                             // RES_IMAGE
// Plays on this console. `uris` (tracks) or `context` (playlist/album/artist uri) with an optional track `offset_uri`.
void apiPlay(const char *context, const char *const *uris, unsigned uri_count, const char *offset_uri);
