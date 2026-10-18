// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Spotrps5 contributors
#include "json.h"
#include <stdlib.h>
#include <string.h>

struct JsonDocument {
    Json *root;
    Json *nodes;
    size_t node_count, node_capacity;
    char *text;  // strings (keys and values), each NUL-terminated
    size_t text_size, text_capacity;
};
typedef struct {
    JsonDocument *document;
    const char *p, *end;
    unsigned depth;
    bool failed;
} Parser;

// Nodes are reached by index while parsing (the array may move) and by pointer after: so they are fixed up at the end.
static Json *newNode(Parser *parser, JsonType type) {
    JsonDocument *d = parser->document;
    if (d->node_count == d->node_capacity) {
        parser->failed = true;  // capacity is reserved up front (see jsonParse)
        return NULL;
    }
    Json *node = &d->nodes[d->node_count++];
    memset(node, 0, sizeof(*node));
    node->type = type;
    return node;
}
static char *textSpace(Parser *parser, size_t bytes) {
    JsonDocument *d = parser->document;
    if (d->text_size + bytes > d->text_capacity) {
        parser->failed = true;
        return NULL;
    }
    char *at = d->text + d->text_size;
    d->text_size += bytes;
    return at;
}
static void skip(Parser *parser) {
    while (parser->p < parser->end && (*parser->p == ' ' || *parser->p == '\t' || *parser->p == '\n' || *parser->p == '\r')) ++parser->p;
}
static int hex4(const char *p) {
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= c - '0';
        else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
        else return -1;
    }
    return value;
}
static size_t putUtf8(char *out, unsigned code) {
    if (code < 0x80) { out[0] = (char)code; return 1; }
    if (code < 0x800) { out[0] = (char)(0xC0 | (code >> 6)); out[1] = (char)(0x80 | (code & 0x3F)); return 2; }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12)); out[1] = (char)(0x80 | ((code >> 6) & 0x3F)); out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code >> 18)); out[1] = (char)(0x80 | ((code >> 12) & 0x3F)); out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}
// Parses a string literal at p (on the opening quote) into the text area; the result is NUL-terminated.
static const char *parseString(Parser *parser) {
    if (parser->p >= parser->end || *parser->p != '"') { parser->failed = true; return NULL; }
    ++parser->p;
    // The unescaped text is never longer than the literal.
    const char *scan = parser->p;
    while (scan < parser->end && *scan != '"') scan += (*scan == '\\' && scan + 1 < parser->end) ? 2 : 1;
    if (scan >= parser->end) { parser->failed = true; return NULL; }
    char *out = textSpace(parser, (size_t)(scan - parser->p) + 1);
    if (!out) return NULL;
    char *w = out;
    while (parser->p < scan) {
        char c = *parser->p++;
        if (c != '\\') { *w++ = c; continue; }
        char e = *parser->p++;
        switch (e) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': *w++ = '\r'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'u': {
            if (parser->p + 4 > scan) { parser->failed = true; return NULL; }
            int code = hex4(parser->p);
            if (code < 0) { parser->failed = true; return NULL; }
            parser->p += 4;
            if (code >= 0xD800 && code < 0xDC00 && parser->p + 6 <= scan && parser->p[0] == '\\' && parser->p[1] == 'u') {
                int low = hex4(parser->p + 2);
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    parser->p += 6;
                }
            }
            w += putUtf8(w, (unsigned)code);  // at most 6 source bytes -> 4: fits
            break;
        }
        default: *w++ = e; break;  // \" \\ \/ and anything odd
        }
    }
    *w = 0;
    ++parser->p;  // closing quote
    return out;
}
static Json *parseValue(Parser *parser);
static Json *parseContainer(Parser *parser, bool object) {
    Json *node = newNode(parser, object ? JSON_OBJECT : JSON_ARRAY);
    if (!node || ++parser->depth > 64) { parser->failed = true; return NULL; }
    ++parser->p;  // [ or {
    Json *last = NULL;
    skip(parser);
    if (parser->p < parser->end && *parser->p == (object ? '}' : ']')) {
        ++parser->p;
        --parser->depth;
        return node;
    }
    while (!parser->failed) {
        skip(parser);
        const char *key = NULL;
        if (object) {
            key = parseString(parser);
            skip(parser);
            if (parser->failed || parser->p >= parser->end || *parser->p != ':') { parser->failed = true; return NULL; }
            ++parser->p;
        }
        Json *child = parseValue(parser);
        if (!child) { parser->failed = true; return NULL; }
        child->key = key;
        if (last) last->next = child;
        else node->first = child;
        last = child;
        ++node->count;
        skip(parser);
        if (parser->p >= parser->end) break;
        if (*parser->p == ',') { ++parser->p; continue; }
        if (*parser->p == (object ? '}' : ']')) { ++parser->p; --parser->depth; return node; }
        break;
    }
    parser->failed = true;
    return NULL;
}
static Json *parseValue(Parser *parser) {
    skip(parser);
    if (parser->p >= parser->end || parser->failed) { parser->failed = true; return NULL; }
    char c = *parser->p;
    if (c == '{' || c == '[') return parseContainer(parser, c == '{');
    if (c == '"') {
        const char *s = parseString(parser);
        Json *node = s ? newNode(parser, JSON_STRING) : NULL;
        if (node) node->string = s;
        return node;
    }
    size_t left = (size_t)(parser->end - parser->p);
    if (left >= 4 && !memcmp(parser->p, "true", 4)) { Json *n = newNode(parser, JSON_BOOL); if (n) n->number = 1; parser->p += 4; return n; }
    if (left >= 5 && !memcmp(parser->p, "false", 5)) { Json *n = newNode(parser, JSON_BOOL); parser->p += 5; return n; }
    if (left >= 4 && !memcmp(parser->p, "null", 4)) { Json *n = newNode(parser, JSON_NULL); parser->p += 4; return n; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buffer[48];
        size_t n = 0;
        while (parser->p < parser->end && n < sizeof(buffer) - 1 && strchr("+-.eE0123456789", *parser->p)) buffer[n++] = *parser->p++;
        buffer[n] = 0;
        Json *node = newNode(parser, JSON_NUMBER);
        if (node) node->number = strtod(buffer, NULL);
        return node;
    }
    parser->failed = true;
    return NULL;
}

JsonDocument *jsonParse(const char *text, size_t length) {
    if (!text || !length) return NULL;
    JsonDocument *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    // Every value takes at least one byte, so `length` nodes always suffice; the text area is as big as the input plus a NUL per string.
    // (Typical documents are far smaller than these bounds on nodes: the area is trimmed by realloc afterwards.)
    size_t node_bound = length / 2 + 2;
    d->node_capacity = node_bound;
    d->nodes = malloc(node_bound * sizeof(Json));
    d->text_capacity = length + 16;
    d->text = malloc(d->text_capacity);
    if (!d->nodes || !d->text) { jsonFree(d); return NULL; }
    Parser parser = {d, text, text + length, 0, false};
    d->root = parseValue(&parser);
    skip(&parser);
    if (parser.failed || !d->root) { jsonFree(d); return NULL; }
    Json *shrunk = realloc(d->nodes, d->node_count * sizeof(Json));
    if (shrunk && shrunk != d->nodes) {
        // The pointers inside the tree must follow the move.
        ptrdiff_t shift = (char *)shrunk - (char *)d->nodes;
        for (size_t i = 0; i < d->node_count; ++i) {
            if (shrunk[i].first) shrunk[i].first = (Json *)((char *)shrunk[i].first + shift);
            if (shrunk[i].next) shrunk[i].next = (Json *)((char *)shrunk[i].next + shift);
        }
        d->root = (Json *)((char *)d->root + shift);
        d->nodes = shrunk;
    }
    return d;
}
void jsonFree(JsonDocument *d) {
    if (!d) return;
    free(d->nodes);
    free(d->text);
    free(d);
}
const Json *jsonRoot(const JsonDocument *d) { return d ? d->root : NULL; }
const Json *jsonGet(const Json *object, const char *key) {
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (const Json *m = object->first; m; m = m->next)
        if (m->key && !strcmp(m->key, key)) return m;
    return NULL;
}
const Json *jsonAt(const Json *array, unsigned index) {
    if (!array || array->type != JSON_ARRAY) return NULL;
    const Json *item = array->first;
    for (; item && index; --index) item = item->next;
    return item;
}
const char *jsonString(const Json *v, const char *fallback) { return v && v->type == JSON_STRING && v->string ? v->string : fallback; }
double jsonNumber(const Json *v, double fallback) { return v && v->type == JSON_NUMBER ? v->number : fallback; }
bool jsonBool(const Json *v, bool fallback) { return v && v->type == JSON_BOOL ? v->number != 0 : fallback; }
const Json *jsonPath(const Json *v, const char *path) {
    char part[64];
    while (v && *path) {
        size_t n = 0;
        while (path[n] && path[n] != '.' && n < sizeof(part) - 1) { part[n] = path[n]; ++n; }
        part[n] = 0;
        path += n;
        if (*path == '.') ++path;
        if (v->type == JSON_ARRAY) {
            char *end;
            unsigned long index = strtoul(part, &end, 10);
            v = *end || end == part ? NULL : jsonAt(v, (unsigned)index);
        } else {
            v = jsonGet(v, part);
        }
    }
    return v;
}
