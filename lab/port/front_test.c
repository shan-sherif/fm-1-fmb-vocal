/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the C front end (engine/ch_speech.c) against CHIMERA's, on what `lab fronttest` wrote:
 *   out/ft-lts.txt   the letter-to-sound rules on every plain dictionary word
 *   out/ft.txt       texts -> phrases -> gestures (plan_phrases), lyrics -> syllables -> note on / off gestures
 *   out/ft-dict.txt  the dictionary entries those use (the C side looks words up there)
 *
 *   front_test [out/]   (run from lab/) */
#include "ch_speech.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dict_w[4096], *dict_p[4096];
static int n_dict;
static const char *lookup(const char *w, void *user)
{
    int i;
    (void)user;
    for (i = 0; i < n_dict; i++)
        if (!strcmp(dict_w[i], w))
            return dict_p[i];
    return 0;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static void seg_str(const ch_seg_t *s, char *b)
{
    sprintf(b, "%s:%d:%d", s->ipa, s->stress, s->word_end);
}

static double worst;   /* the largest relative gesture difference */
static int g_bad, g_count, g_n_diff;

/* compare the C gestures with the "G ..." lines that follow in f */
static void compare_gestures(FILE *f, const ch_gesture_t *g, int n, const char *what, char *line, size_t cap, int *have_line)
{
    int i = 0, k;
    *have_line = 0;
    while (fgets(line, (int)cap, f)) {
        double v[30];
        char *p = line + 2, *e;
        if (strncmp(line, "G ", 2)) {
            *have_line = 1;
            break;
        }
        for (k = 0; k < 30; k++) {
            v[k] = strtod(p, &e);
            p = e;
        }
        if (i < n) {
            const ch_real *c = &g[i].t0;   /* t0, t1, ramp_in, ramp_out, strength, speed, release_hold, tag, targets */
            for (k = 0; k < 30; k++) {
                double x = (double)c[k], d;
                if (isnan(x) && isnan(v[k]))
                    continue;
                d = isnan(x) != isnan(v[k]) ? 1e9 : fabs(x - v[k]) / (fabs(v[k]) + 1e-3);
                if (d > worst)
                    worst = d;
                if (d > 1e-4) {
                    if (g_bad++ < 12)
                        printf("  %s: gesture %d field %d: C %.9g, CHIMERA %.9g\n", what, i, k, x, v[k]);
                }
            }
        }
        i++;
        g_count++;
    }
    if (i != n) {
        g_n_diff++;
        printf("  %s: %d gestures in C, %d in CHIMERA\n", what, n, i);
    }
}

int main(int argc, char **argv)
{
    static char line[1 << 16];
    static ch_seg_t segs[CH_SEG_MAX];
    static ch_phrase_t ph[CH_PHRASE_MAX];
    static ch_gesture_t gs[4096];
    static ch_syl_t syl[CH_SYL_MAX];
    const char *dir = argc > 1 ? argv[1] : "out/";
    char path[256];
    FILE *f;
    int lts_n = 0, lts_bad = 0, ph_bad = 0, syl_bad = 0, n_syl = 0, have = 0;
    /* the dictionary entries */
    snprintf(path, sizeof path, "%sft-dict.txt", dir);
    f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    while (fgets(line, sizeof line, f) && n_dict < 4096) {
        char *t = strchr(line, '\t');
        chomp(line);
        if (!t)
            continue;
        *t = 0;
        dict_w[n_dict] = strdup(line);
        dict_p[n_dict++] = strdup(t + 1);
    }
    fclose(f);
    /* the letter-to-sound rules */
    snprintf(path, sizeof path, "%sft-lts.txt", dir);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *t = strchr(line, '\t'), got[512] = "";
        int n, i;
        chomp(line);
        if (!t)
            continue;
        *t = 0;
        n = ch_letter_to_sound(line, segs, CH_SEG_MAX);
        for (i = 0; i < n; i++) {
            char b[16];
            sprintf(b, "%s%s%d", i ? " " : "", segs[i].ipa, segs[i].stress < 0 ? 0 : segs[i].stress);
            strcat(got, b);
        }
        lts_n++;
        if (strcmp(got, t + 1) && lts_bad++ < 5)
            printf("  letter-to-sound \"%s\": C [%s], CHIMERA [%s]\n", line, got, t + 1);
    }
    if (f)
        fclose(f);
    printf("letter-to-sound: %d words, %d differ\n", lts_n, lts_bad);
    /* texts and lyrics */
    snprintf(path, sizeof path, "%sft.txt", dir);
    f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    for (;;) {
        if (!have && !fgets(line, sizeof line, f))
            break;
        have = 0;
        chomp(line);
        if (!strncmp(line, "TEXT ", 5)) {
            char text[512];
            int n_segs, np, i, k;
            ch_plan_opts_t o;
            ch_plan_t plan;
            strcpy(text, line + 5);
            np = ch_text_to_phrases(text, lookup, 0, segs, CH_SEG_MAX, &n_segs, ph, CH_PHRASE_MAX);
            for (i = 0; i < np; i++) {
                char want[4096], got[4096];
                fgets(line, sizeof line, f);
                chomp(line);
                strcpy(want, line);
                sprintf(got, "PHRASE %d %g", ph[i].intonation, (double)ph[i].pause);
                for (k = 0; k < ph[i].n; k++) {
                    char b[32];
                    seg_str(&segs[ph[i].first + k], b);
                    strcat(got, " ");
                    strcat(got, b);
                }
                if (strcmp(want, got) && ph_bad++ < 6)
                    printf("  \"%s\" phrase %d:\n    C        %s\n    CHIMERA  %s\n", text, i, got, want);
            }
            ch_plan_defaults(&o);
            o.t0 = 0.05;
            o.f0 = 112.0;
            plan.g = gs;
            plan.cap = 4096;
            plan.n = 0;
            plan.dropped = 0;
            ch_plan_phrases(segs, ph, np, &o, &plan);
            compare_gestures(f, gs, plan.n, text, line, sizeof line, &have);
            chomp(line);
            if (have && !strncmp(line, "END ", 4)) {
                double e = atof(line + 4);
                if (fabs(e - (double)plan.end) > 1e-6)
                    printf("  \"%s\": end C %.9g, CHIMERA %.9g\n", text, (double)plan.end, e);
                have = 0;
            }
        } else if (!strncmp(line, "LYRIC ", 6)) {
            n_syl = ch_lyrics(line + 6, lookup, 0, syl, CH_SYL_MAX);
        } else if (!strncmp(line, "SYL ", 4)) {
            static int si;
            char got[1024] = "", b[32];
            int k;
            if (si < n_syl) {
                const ch_syl_t *s = &syl[si];
                sprintf(got, "SYL %s|", s->text);
                for (k = 0; k < s->n_onset; k++) {
                    seg_str(&s->onset[k], b);
                    if (k)
                        strcat(got, ",");
                    strcat(got, b);
                }
                strcat(got, "|");
                seg_str(&s->nucleus, b);
                strcat(got, b);
                strcat(got, "|");
                for (k = 0; k < s->n_coda; k++) {
                    seg_str(&s->coda[k], b);
                    if (k)
                        strcat(got, ",");
                    strcat(got, b);
                }
                sprintf(b, "|%s %s %s", s->melisma ? "true" : "false", s->word_end ? "true" : "false", s->phrase_end ? "true" : "false");
                strcat(got, b);
            }
            if (strcmp(got, line) && syl_bad++ < 8)
                printf("  syllable %d:\n    C        %s\n    CHIMERA  %s\n", si, got, line);
            si++;
        } else if (!strncmp(line, "NOTEON ", 7) || !strncmp(line, "NOTEOFF ", 8)) {
            int on = line[4] == 'O' && line[5] == 'N' && line[6] == ' ', i = atoi(line + (on ? 7 : 8));
            ch_real midi = (ch_real)(60 + i % 12);
            ch_plan_t plan;
            char what[32];
            plan.g = gs;
            plan.cap = 4096;
            plan.n = 0;
            plan.dropped = 0;
            if (i < n_syl) {
                if (on)
                    ch_plan_note_on(&syl[i], midi, 30.0, 2.0 * i, i % 2 ? &syl[i - 1] : 0, (ch_real)(60 + (i - 1) % 12), &plan);
                else
                    ch_plan_note_off(&syl[i], midi, &plan);
            }
            sprintf(what, "note %s %d", on ? "on" : "off", i);
            compare_gestures(f, gs, plan.n, what, line, sizeof line, &have);
        }
    }
    fclose(f);
    printf("texts: %d phrases differ; lyrics: %d syllables (C), %d differ\n", ph_bad, n_syl, syl_bad);
    printf("gestures: %d compared, %d values differ (> 1e-4 relative), %d lists of a different length; worst %.2e\n", g_count,
           g_bad, g_n_diff, worst);
    return lts_bad || ph_bad || syl_bad || g_bad || g_n_diff;
}
