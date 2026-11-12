/*
 * Xnextbsd: the keyboard settings WindowServer uses.
 *
 * Copyright (c) 2026, Joseph Maloney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * A stopgap until WindowServer sends its compiled keymap
 * (apps-windowserver#5): the X keymap is compiled from the same settings
 * WindowServer compiles its own from, read the way WindowServer reads
 * them (server/config.c): built-in defaults, then the flat property list
 * /System/Library/Preferences/org.nextbsd.WindowServer.plist, then
 * WS_<Key> environment variables. WindowServer's command line can't be
 * seen from here.
 */

#include <dix-config.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os.h"

#include "nextbsd.h"

#define NB_WS_PREFERENCES \
    "/System/Library/Preferences/org.nextbsd.WindowServer.plist"

static void
nbConfigSet(nbKeyboardConfig *cfg, const char *key, const char *value)
{
    char *end;
    long v;

    if (strcmp(key, "KeyboardRules") == 0)
        strlcpy(cfg->rules, value, sizeof(cfg->rules));
    else if (strcmp(key, "KeyboardModel") == 0)
        strlcpy(cfg->model, value, sizeof(cfg->model));
    else if (strcmp(key, "KeyboardLayout") == 0)
        strlcpy(cfg->layout, value, sizeof(cfg->layout));
    else if (strcmp(key, "KeyboardVariant") == 0)
        strlcpy(cfg->variant, value, sizeof(cfg->variant));
    else if (strcmp(key, "KeyboardOptions") == 0)
        strlcpy(cfg->options, value, sizeof(cfg->options));
    else if (strcmp(key, "KeyRepeatDelay") == 0 ||
             strcmp(key, "KeyRepeatInterval") == 0) {
        errno = 0;
        v = strtol(value, &end, 0);
        if (errno != 0 || *end != '\0' || v < 0 || v > 10000)
            return;
        if (strcmp(key, "KeyRepeatDelay") == 0)
            cfg->repeat_delay = (int) v;
        else
            cfg->repeat_interval = (int) v;
    }
}

/* The text between <tag> and </tag> from p; returns what follows. */
static const char *
nbXMLText(const char *p, const char *tag, char *out, size_t outlen)
{
    char close[32];
    const char *e;
    size_t n;

    snprintf(close, sizeof(close), "</%s>", tag);
    e = strstr(p, close);
    if (e == NULL)
        return NULL;
    n = (size_t) (e - p);
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return e + strlen(close);
}

/* A single top-level <dict> of <key> and <string> or <integer>. */
static void
nbConfigLoad(nbKeyboardConfig *cfg, const char *path)
{
    char key[128], value[256];
    const char *p;
    char *buf;
    FILE *f;
    long len;

    f = fopen(path, "r");
    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        len > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    buf = malloc((size_t) len + 1);
    if (buf == NULL || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return;
    }
    buf[len] = '\0';
    fclose(f);

    p = buf;
    while ((p = strstr(p, "<key>")) != NULL) {
        p = nbXMLText(p + 5, "key", key, sizeof(key));
        if (p == NULL)
            break;
        while (isspace((unsigned char) *p))
            p++;
        if (strncmp(p, "<string>", 8) == 0)
            p = nbXMLText(p + 8, "string", value, sizeof(value));
        else if (strncmp(p, "<integer>", 9) == 0)
            p = nbXMLText(p + 9, "integer", value, sizeof(value));
        else
            continue;
        if (p == NULL)
            break;
        nbConfigSet(cfg, key, value);
    }
    free(buf);
}

void
nbReadKeyboardConfig(nbKeyboardConfig *cfg)
{
    static const char *const keys[] = {
        "KeyboardRules", "KeyboardModel", "KeyboardLayout",
        "KeyboardVariant", "KeyboardOptions",
        "KeyRepeatDelay", "KeyRepeatInterval",
    };
    char name[64];
    const char *v;
    size_t i;

    /* WindowServer's built-in defaults (server/config.c). */
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->rules, "evdev", sizeof(cfg->rules));
    strlcpy(cfg->model, "pc105", sizeof(cfg->model));
    strlcpy(cfg->layout, "us", sizeof(cfg->layout));
    cfg->repeat_delay = 500;
    cfg->repeat_interval = 33;

    nbConfigLoad(cfg, NB_WS_PREFERENCES);

    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        snprintf(name, sizeof(name), "WS_%s", keys[i]);
        v = getenv(name);
        if (v != NULL)
            nbConfigSet(cfg, keys[i], v);
    }
}
