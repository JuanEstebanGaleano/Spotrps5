// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
// A small JSON reader: the whole tree lives in one pool and is freed at once.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;
typedef struct Json Json;
struct Json {
    JsonType type;
    const char *key;       // in an object: the member's name
    const char *string;    // JSON_STRING (UTF-8, unescaped)
    double number;         // JSON_NUMBER; JSON_BOOL 0/1
    Json *first, *next;    // children (array items or object members) and the next sibling
    unsigned count;
};
typedef struct JsonDocument JsonDocument;

JsonDocument *jsonParse(const char *text, size_t length);  // NULL when it is not JSON
void jsonFree(JsonDocument *document);
const Json *jsonRoot(const JsonDocument *document);

// Lookups never fail: a missing member or a wrong type gives NULL / the default.
const Json *jsonGet(const Json *object, const char *key);
const Json *jsonAt(const Json *array, unsigned index);
const char *jsonString(const Json *value, const char *fallback);
double jsonNumber(const Json *value, double fallback);
bool jsonBool(const Json *value, bool fallback);
// Chains of keys / indexes: "items.0.track.name" (digits index arrays).
const Json *jsonPath(const Json *value, const char *path);
