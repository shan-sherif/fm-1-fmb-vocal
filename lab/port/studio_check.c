/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif */
/* the C firmware's lyrics reader on the Studio's document: each slot's syllables, as the keys will sing them */
#include "ch_speech.h"
#include <stdio.h>
#include <string.h>
static char doc[8192];
static const char *lk(const char *w, void *u)
{
    static char out[96];
    const char *p = doc;
    size_t n = strlen(w);
    (void)u;
    while ((p = strchr(p, '@'))) {
        p++;
        if (!strncmp(p, w, n) && p[n] == ' ') {
            const char *e = strchr(p, '\n');
            size_t k = (size_t)(e - p - n - 1);
            memcpy(out, p + n + 1, k);
            out[k] = 0;
            return out;
        }
    }
    return 0;
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r");
    char line[256];
    size_t len = fread(doc, 1, sizeof doc - 1, f);
    doc[len] = 0;
    ch_lts_device = 1;
    for (char *p = doc; (p = strchr(p, '\n')); ) {
        p++;
        if (*p >= '1' && *p <= '8' && p[1] == ' ') {
            ch_syl_t s[CH_SYL_MAX];
            char *e = strchr(p, '\n');
            int n, i;
            memcpy(line, p + 2, (size_t)(e - p - 2));
            line[e - p - 2] = 0;
            n = ch_lyrics(line, lk, 0, s, CH_SYL_MAX);
            for (i = 0; i < n; i++)
                printf("%s%s", i ? " " : "", s[i].text);
            printf("\n");
        }
    }
    return 0;
}
