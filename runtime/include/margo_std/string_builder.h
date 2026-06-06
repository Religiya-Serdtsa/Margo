#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/**
 * @file margo_std/string_builder.h
 * @brief Dynamic string builder for Margo.
 *
 * Usage:
 *   string_builder sb
 *   sb_init(&sb)
 *   sb_append(&sb, "hello")
 *   sb_append(&sb, " world")
 *   string s = sb_to_string(&sb)
 *   print(s, endl="\n")
 *   sb_free(&sb)
 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} string_builder;

static inline void sb_init(string_builder *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static inline bool sb_reserve(string_builder *sb, size_t additional) {
    size_t needed = sb->len + additional + 1;
    if (needed > sb->cap) {
        size_t new_cap = sb->cap ? sb->cap * 2 : 64;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *new_data = (char *)realloc(sb->data, new_cap);
        if (!new_data) {
            return false;
        }
        sb->data = new_data;
        sb->cap = new_cap;
    }
    return true;
}

static inline bool sb_append(string_builder *sb, const char *text) {
    if (!text || !*text) {
        return true;
    }
    size_t len = strlen(text);
    if (!sb_reserve(sb, len)) {
        return false;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

static inline bool sb_appendf(string_builder *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0) {
        return false;
    }
    if (!sb_reserve(sb, (size_t)n)) {
        return false;
    }
    va_start(args, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, args);
    va_end(args);
    sb->len += (size_t)n;
    return true;
}

static inline bool sb_append_char(string_builder *sb, char c) {
    if (!sb_reserve(sb, 1)) {
        return false;
    }
    sb->data[sb->len] = c;
    sb->len++;
    sb->data[sb->len] = '\0';
    return true;
}

static inline char *sb_to_string(string_builder *sb) {
    if (!sb->data) {
        char *empty = (char *)malloc(1);
        if (empty) {
            empty[0] = '\0';
        }
        return empty;
    }
    char *copy = (char *)malloc(sb->len + 1);
    if (copy) {
        memcpy(copy, sb->data, sb->len + 1);
    }
    return copy;
}

static inline void sb_clear(string_builder *sb) {
    sb->len = 0;
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static inline void sb_free(string_builder *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}
