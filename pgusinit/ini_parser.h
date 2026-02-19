/*
 * ini_parser.h — Minimal, generic INI file parser for pgusinit (DOS/OpenWatcom C).
 *
 * Parses standard INI files:
 *   [section]
 *   key=value   ; inline comments supported
 *   # line comments supported
 *
 * Usage:
 *   ini_file_t ini;
 *   if (ini_load(&ini, "joy.ini") == 0) {
 *       const char* val = ini_get(&ini, "joystick", "profile");
 *       ini_free(&ini);
 *   }
 *
 * All strings are stored in a single heap allocation.  ini_free() releases it.
 * Maximum line length: INI_MAX_LINE.  Maximum entries: INI_MAX_ENTRIES.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INI_MAX_LINE    256
#define INI_MAX_ENTRIES 128

typedef struct {
    char* section;
    char* key;
    char* value;
} ini_entry_t;

typedef struct {
    ini_entry_t entries[INI_MAX_ENTRIES];
    int         count;
    char*       buf;   // single heap allocation holding all strings
} ini_file_t;

/* Trim leading and trailing whitespace in-place; returns pointer to trimmed start. */
static char* ini_trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/*
 * Load and parse an INI file.
 * Returns 0 on success, non-zero on error (file not found, out of memory, etc.).
 */
static int ini_load(ini_file_t* ini, const char* path) {
    FILE* fp;
    long  fsize;
    char* buf;
    char  line[INI_MAX_LINE];
    char  cur_section[INI_MAX_LINE] = "";
    char* p;
    int   n = 0;

    memset(ini, 0, sizeof(*ini));

    fp = fopen(path, "r");
    if (!fp) return -1;

    /* Measure file size for the heap allocation */
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    rewind(fp);

    /* Allocate enough space for all strings (worst case: every byte is a string) */
    buf = (char*)malloc((size_t)(fsize + INI_MAX_ENTRIES * INI_MAX_LINE));
    if (!buf) { fclose(fp); return -2; }
    ini->buf = buf;

    char* wp = buf; /* write pointer into the string pool */

    while (fgets(line, sizeof(line), fp)) {
        p = ini_trim(line);

        /* Skip blank lines and comment lines */
        if (!*p || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            /* Section header */
            char* end = strchr(p + 1, ']');
            if (end) {
                *end = '\0';
                strncpy(cur_section, p + 1, INI_MAX_LINE - 1);
                cur_section[INI_MAX_LINE - 1] = '\0';
            }
            continue;
        }

        /* key=value */
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = ini_trim(p);
        char* val = ini_trim(eq + 1);

        /* Strip inline comment from value */
        char* cmt = strchr(val, ';');
        if (cmt) { *cmt = '\0'; val = ini_trim(val); }
        cmt = strchr(val, '#');
        if (cmt) { *cmt = '\0'; val = ini_trim(val); }

        if (n >= INI_MAX_ENTRIES) break;

        /* Copy strings into pool */
        ini->entries[n].section = wp;
        strcpy(wp, cur_section); wp += strlen(cur_section) + 1;

        ini->entries[n].key = wp;
        strcpy(wp, key); wp += strlen(key) + 1;

        ini->entries[n].value = wp;
        strcpy(wp, val); wp += strlen(val) + 1;

        n++;
    }

    ini->count = n;
    fclose(fp);
    return 0;
}

/*
 * Look up a value by section and key (case-insensitive).
 * Returns the value string, or NULL if not found.
 */
static const char* ini_get(const ini_file_t* ini, const char* section, const char* key) {
    int i;
    for (i = 0; i < ini->count; i++) {
        if (stricmp(ini->entries[i].section, section) == 0 &&
            stricmp(ini->entries[i].key,     key)     == 0) {
            return ini->entries[i].value;
        }
    }
    return NULL;
}

/* Release heap memory allocated by ini_load(). */
static void ini_free(ini_file_t* ini) {
    if (ini->buf) { free(ini->buf); ini->buf = NULL; }
    ini->count = 0;
}
