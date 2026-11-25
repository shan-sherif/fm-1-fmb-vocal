/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * A port of CHIMERA's tunes (src/song/tunes.ts; "Made of air" and "Twinkle, twinkle" from phonetics/songs.rs) and its
 * fitting of the words to them (src/song/fit.ts); see ch_song.h. Its plan_song is in ch_speech.c, with the other
 * singing planners. The comments are CHIMERA's, shortened. */
#include "ch_song.h"
#include "ch_common.h"

/* [midi, half beats]: all written for this instrument but Twinkle (traditional). Each phrase is one line of lyrics,
 * and ends on a long note: the breath before the next line comes out of it */
#define N(m, hb) {m, hb}
static const ch_tnote_t DREAMY_N[] = {   /* "Made of air", the instrument's own: minor, unhurried */
    N(64, 2), N(60, 2), N(62, 2), N(57, 2), N(57, 1), N(60, 1), N(64, 2), N(62, 1), N(60, 3),
    N(65, 2), N(64, 1), N(62, 1), N(60, 1), N(62, 3), N(64, 1), N(62, 1), N(60, 1), N(59, 1), N(57, 4)};
static const uint8_t DREAMY_P[] = {9, 10};
static const ch_tnote_t JOYFUL_N[] = {   /* major, bright, climbing */
    N(64, 2), N(64, 2), N(67, 2), N(69, 2), N(67, 3), N(64, 1), N(62, 4),
    N(60, 2), N(62, 2), N(64, 2), N(67, 2), N(64, 3), N(62, 1), N(60, 4),
    N(69, 2), N(69, 2), N(72, 2), N(69, 2), N(67, 3), N(64, 1), N(67, 4),
    N(65, 2), N(64, 2), N(62, 2), N(64, 2), N(62, 2), N(59, 2), N(60, 4)};
static const uint8_t SEVENS[] = {7, 7, 7, 7};
static const ch_tnote_t SOMBER_N[] = {   /* minor, slow, falling back home */
    N(57, 2), N(60, 2), N(64, 3), N(62, 1), N(60, 2), N(59, 2), N(57, 4),
    N(57, 2), N(60, 2), N(64, 2), N(67, 2), N(65, 3), N(64, 1), N(64, 4),
    N(65, 2), N(64, 2), N(62, 2), N(60, 2), N(62, 3), N(60, 1), N(59, 4),
    N(57, 2), N(59, 2), N(60, 2), N(62, 2), N(64, 2), N(59, 2), N(57, 4)};
static const ch_tnote_t PLAYFUL_N[] = {  /* quick, short notes, room for many words */
    N(67, 1), N(67, 1), N(64, 1), N(67, 1), N(69, 2), N(67, 2), N(64, 1), N(62, 1), N(60, 2), N(62, 4),
    N(60, 1), N(60, 1), N(62, 1), N(64, 1), N(67, 2), N(64, 2), N(62, 1), N(60, 1), N(62, 2), N(64, 4),
    N(67, 1), N(67, 1), N(69, 1), N(71, 1), N(72, 2), N(71, 2), N(69, 1), N(67, 1), N(64, 2), N(67, 4),
    N(69, 1), N(67, 1), N(64, 1), N(62, 1), N(60, 2), N(62, 2), N(64, 1), N(62, 1), N(60, 2), N(60, 4)};
static const uint8_t TENS[] = {10, 10, 10, 10};
static const ch_tnote_t GENTLE_N[] = {   /* a slow waltz, long notes */
    N(65, 4), N(69, 2), N(72, 4), N(69, 2), N(67, 4), N(65, 2), N(67, 6),
    N(65, 4), N(69, 2), N(72, 4), N(74, 2), N(72, 4), N(69, 2), N(67, 6),
    N(70, 4), N(69, 2), N(67, 4), N(65, 2), N(64, 4), N(65, 2), N(67, 6),
    N(65, 4), N(69, 2), N(67, 4), N(64, 2), N(62, 4), N(64, 2), N(65, 6)};
static const ch_tnote_t TWINKLE_N[] = {  /* traditional (public domain) */
    N(60, 2), N(60, 2), N(67, 2), N(67, 2), N(69, 2), N(69, 2), N(67, 4),
    N(65, 2), N(65, 2), N(64, 2), N(64, 2), N(62, 2), N(62, 2), N(60, 4)};
#undef N
const ch_tune_t CH_TUNES[CH_NTUNES] = {
    {"DREAMY", R(84.0), -1, 0, 0, 2, DREAMY_P, DREAMY_N},
    {"JOYFUL", R(104.0), -1, 0, 0, 4, SEVENS, JOYFUL_N},
    {"SOMBER", R(76.0), -1, 0, 0, 4, SEVENS, SOMBER_N},
    {"PLAYFUL", R(120.0), -1, 0, 0, 4, TENS, PLAYFUL_N},
    {"GENTLE", R(96.0), -1, 0, 0, 4, SEVENS, GENTLE_N},
    {"TWINKLE", R(100.0), -1, 0, 0, 2, SEVENS, TWINKLE_N},
};

/* ------------------------------------------------------------------------- fit.ts --- */
#define LINES_MAX 64
#define LINE_NOTES 64

/* the tune's typical phrase, in notes: the line length the lyrics are cut to */
static int phrase_size(const ch_tune_t *t)
{
    int k, n = 0;
    for (k = 0; k < t->n_phrases; k++)
        n += t->phrase_len[k];
    n = (int)js_round_c((ch_real)n / (ch_real)t->n_phrases);
    return n < 1 ? 1 : n;
}

/* the lyrics' syllables in lines of about `size`: each line [start, end) into ls / le; returns the count */
static int song_lines(const ch_syl_t *syls, int n, int size, int *ls, int *le)
{
    int raw_s[LINES_MAX], raw_e[LINES_MAX], nr = 0, m_s[LINES_MAX], m_e[LINES_MAX], nm = 0, nl = 0, i, cur = 0, k;
    const ch_real third = (ch_real)size / R(3.0), most = R(1.5) * (ch_real)size;
    for (i = 0; i < n && nr < LINES_MAX; i++)
        if (syls[i].phrase_end) {
            raw_s[nr] = cur;
            raw_e[nr++] = i + 1;
            cur = i + 1;
        }
    if (cur < n && nr < LINES_MAX) {
        raw_s[nr] = cur;
        raw_e[nr++] = n;
    }
    /* "Oh, yeah, baby": a line of a word or two joins the next one (the last joins the one before) */
    for (k = 0; k < nr; k++) {
        if (nm && (ch_real)(m_e[nm - 1] - m_s[nm - 1]) < third
            && (ch_real)(m_e[nm - 1] - m_s[nm - 1] + raw_e[k] - raw_s[k]) <= most) {
            m_e[nm - 1] = raw_e[k];
        } else {
            m_s[nm] = raw_s[k];
            m_e[nm++] = raw_e[k];
        }
    }
    if (nm > 1 && (ch_real)(m_e[nm - 1] - m_s[nm - 1]) < third
        && (ch_real)(m_e[nm - 2] - m_s[nm - 2] + m_e[nm - 1] - m_s[nm - 1]) <= most) {
        m_e[nm - 2] = m_e[nm - 1];
        nm--;
    }
    /* a long line: cut at the word ends nearest equal parts (never before a held vowel) */
    for (k = 0; k < nm && nl < LINES_MAX; k++) {
        const int len = m_e[k] - m_s[k];
        int parts, start = 0, c;
        if ((ch_real)len <= most) {
            ls[nl] = m_s[k];
            le[nl++] = m_e[k];
            continue;
        }
        parts = (len + size - 1) / size;
        for (c = 1; c < parts && nl < LINES_MAX; c++) {
            const int target = (int)js_round_c((ch_real)(c * len) / (ch_real)parts);
            int cut = -1, q;
            for (q = start + 1; q < len; q++)
                if (syls[m_s[k] + q - 1].word_end && !syls[m_s[k] + q].melisma
                    && (cut < 0 || (q - target < 0 ? target - q : q - target) < (cut - target < 0 ? target - cut : cut - target)))
                    cut = q;
            if (cut < 0)
                break;
            ls[nl] = m_s[k] + start;
            le[nl++] = m_s[k] + cut;
            start = cut;
        }
        if (nl < LINES_MAX) {
            ls[nl] = m_s[k] + start;
            le[nl++] = m_e[k];
        }
    }
    return nl;
}

/* one line (n syllables from s0) onto a phrase's notes: the longest note splits until there are enough; with more
 * than twice as many notes, the shortest neighbours merge (the first's pitch); note j sings syllable at(j), counted
 * from the end so the spare notes go to the line's last syllables */
static int fit_line(int s0, int n, const ch_tnote_t *ph, int np, int shift, ch_sung_t *out, int cap)
{
    uint8_t midi[LINE_NOTES];
    ch_real beats[LINE_NOTES];
    int m = np, k, i, j;
    if (n <= 0)
        return 0;
    for (k = 0; k < np && k < LINE_NOTES; k++) {
        midi[k] = ph[k].midi;
        beats[k] = (ch_real)ph[k].half_beats * R(0.5);
    }
    while (m < n && m < LINE_NOTES) {
        k = 0;
        for (i = 1; i < m; i++)
            if (beats[i] > beats[k])
                k = i;
        for (i = m; i > k + 1; i--) {
            midi[i] = midi[i - 1];
            beats[i] = beats[i - 1];
        }
        beats[k] *= R(0.5);
        midi[k + 1] = midi[k];
        beats[k + 1] = beats[k];
        m++;
    }
    while (m > 2 * n) {
        const int last = m > 2 ? m - 2 : m - 1;
        k = 0;
        for (i = 1; i < last; i++)
            if (beats[i] + beats[i + 1] < beats[k] + beats[k + 1])
                k = i;
        beats[k] += beats[k + 1];
        for (i = k + 1; i < m - 1; i++) {
            midi[i] = midi[i + 1];
            beats[i] = beats[i + 1];
        }
        m--;
    }
    for (j = 0; j < m && j < cap; j++) {
        const int at = n - 1 - ((m - 1 - j) * n) / m, prev = j > 0 ? n - 1 - ((m - j) * n) / m : -1;
        out[j].midi = (uint8_t)(midi[j] + shift);
        out[j].beats = beats[j];
        out[j].syl = (int16_t)(s0 + at);
        out[j].held = (uint8_t)(j > 0 && prev == at);
        out[j].phrase_end = (uint8_t)(j == m - 1);
    }
    return j;
}

int ch_song_fit(const ch_tune_t *t, const ch_syl_t *syls, int n_syls, int shift, ch_sung_t *out, int cap)
{
    int ls[LINES_MAX], le[LINES_MAX], nl = song_lines(syls, n_syls, phrase_size(t), ls, le), k, n = 0;
    for (k = 0; k < nl && n < cap; k++) {
        const int p = k % t->n_phrases;
        int off = 0, q;
        for (q = 0; q < p; q++)
            off += t->phrase_len[q];
        n += fit_line(ls[k], le[k] - ls[k], t->notes + off, t->phrase_len[p], shift, out + n, cap - n);
    }
    return n;
}

void ch_song_times(const ch_sung_t *notes, int n, ch_real bpm, ch_real *times)
{
    const ch_real beat = R(60.0) / bpm;
    ch_real t = 0;
    int k;
    for (k = 0; k < n; k++) {
        times[k] = t;
        t += notes[k].beats * beat;
    }
    times[n] = t;
}
