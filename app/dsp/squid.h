/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 *
 * Squidlad: FMB Vocal's third creature of its own in the Bestiary (0.4.0), after the cat and the robot. A small, high,
 * chattering voice that babbles: no words, a quick run of made-up syllables, each a stop and a vowel at its own pitch,
 * wet and bubbly. On the same pieces as the others: the engine's creature voice (ch_creature.h: its parameters, gate
 * and keyframed calls) and the FM-1's waveguide, with its own control here (the engine's files stay as they are). Its
 * babble, every syllable (RATE a second, each a little longer or shorter):
 *   - a vowel picked at random from five (EE, EH, AH, OH, OO; MOUTH leans the choice bright or dark), the tube's three
 *     parts (the throat, the back and the front of the mouth) and the lips each at that vowel's area;
 *   - a stop before it, at the lips, the tongue's tip or its back (a "b", "d" or "g"), shut and let go over ~25 ms, a
 *     pop of air as it opens (SQUISH: how shut, how wet);
 *   - a pitch of its own, up to HOP semitones either side of PITCH, slid into over its first third;
 *   - the breath swelling through it and dipping at its edges, so the syllables come apart.
 * BABBLE 0: none of it, a steady vowel at MOUTH (its notes held). Its random numbers from a fixed seed, so a test
 * hears the same babble each time. Its level set in the simulator (not yet heard on the FM-1). Included once, by
 * app/app.c (the unity build). No allocation: its control runs in the audio interrupt. */
#ifndef FMB_SQUID_H
#define FMB_SQUID_H
#include "ch_creature.h"

enum { SQ_PITCH, SQ_RATE, SQ_HOP, SQ_SQUISH, SQ_MOUTH, SQ_BABBLE, SQ_PRESSURE, SQ_NP };

#define SQ_LEN 11.0f                          /* its tube (cm): a child's, shorter */
#define SQ_PH_LEN 4.5f
#define SQ_OR_LEN 6.5f
#define SQ_NPH 5
#define SQ_NOR 10                             /* the back of the mouth its first 4 cells, the front 4, the lips 2 */
#define SQ_CMH2O 980.665f
#define SQ_GAIN 1.05e-7f                      /* its output gain: a held note about as loud as the cat's and the
                                               * robot's (squid.txt) */
#define SQ_NV 5                               /* its vowels */

static const ch_cparam_t SQ_P[SQ_NP] = {
    {"PITCH", 150.0f, 1400.0f, 420.0f, 0.01f, 1, 1, "HZ"},   /* (keys: C4 an octave up, 523 Hz) */
    {"RATE", 3.0f, 18.0f, 10.0f, 0.05f, 1, 0, "HZ"},         /* syllables a second */
    {"HOP", 0.0f, 12.0f, 5.0f, 0.05f, 0, 0, "ST"},           /* the pitch's jumps, semitones either way */
    {"SQUISH", 0.0f, 1.0f, 0.55f, 0.05f, 0, 0, ""},
    {"MOUTH", 0.0f, 1.0f, 0.6f, 0.03f, 0, 0, ""},
    {"BABBLE", 0.0f, 1.0f, 1.0f, 0.03f, 0, 0, ""},
    {"BREATH", 4.0f, 20.0f, 10.0f, 0.015f, 0, 0, "CMH2O"},
};
#define G CH_CK_GATE
static const ch_ckey_t SQ_CHATTER[] = {       /* a run of babble, settling a little lower */
    {0.0f, CH_CURVE_LIN, 4, {G, SQ_PITCH, SQ_RATE, SQ_BABBLE}, {1, 520.0f, 11.0f, 1.0f}},
    {1.5f, CH_CURVE_LIN, 2, {G, SQ_PITCH}, {1, 460.0f}},
    {1.6f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t SQ_CHEER[] = {         /* fast and rising, into a long bright "aah" */
    {0.0f, CH_CURVE_LIN, 6, {G, SQ_PITCH, SQ_RATE, SQ_HOP, SQ_BABBLE, SQ_MOUTH}, {1, 520.0f, 14.0f, 7.0f, 1.0f, 0.8f}},
    {0.8f, CH_CURVE_EXP, 1, {SQ_PITCH}, {860.0f}},
    {0.85f, CH_CURVE_LIN, 2, {SQ_BABBLE, SQ_MOUTH}, {0, 1.0f}},
    {1.3f, CH_CURVE_EXP, 2, {G, SQ_PITCH}, {1, 940.0f}},
    {1.4f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t SQ_QUESTION[] = {      /* level, then up at the end */
    {0.0f, CH_CURVE_LIN, 4, {G, SQ_PITCH, SQ_RATE, SQ_BABBLE}, {1, 480.0f, 10.0f, 1.0f}},
    {0.6f, CH_CURVE_LIN, 1, {SQ_PITCH}, {470.0f}},
    {1.0f, CH_CURVE_EXP, 2, {SQ_PITCH, SQ_HOP}, {780.0f, 2.0f}},
    {1.05f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t SQ_GRUMBLE[] = {       /* low, slow, dark, falling */
    {0.0f, CH_CURVE_LIN, 6, {G, SQ_PITCH, SQ_RATE, SQ_HOP, SQ_MOUTH, SQ_SQUISH}, {1, 300.0f, 6.0f, 3.0f, 0.15f, 0.3f}},
    {0.0f, CH_CURVE_LIN, 1, {SQ_PRESSURE}, {18.0f}},   /* (low and dark is quiet: more breath) */
    {1.3f, CH_CURVE_EXP, 2, {G, SQ_PITCH}, {1, 230.0f}},
    {1.4f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t SQ_GIGGLE[] = {        /* quick, high, bright bursts */
    {0.0f, CH_CURVE_LIN, 6, {G, SQ_PITCH, SQ_RATE, SQ_HOP, SQ_MOUTH, SQ_SQUISH}, {1, 720.0f, 16.0f, 4.0f, 0.9f, 0.8f}},
    {0.9f, CH_CURVE_EXP, 2, {G, SQ_PITCH}, {1, 600.0f}},
    {0.95f, CH_CURVE_LIN, 1, {G}, {0}},
};
#undef G
static const ch_ccall_t SQ_CALLS[] = {
    {"CHATTER", "A RUN OF BABBLE", 520.0f, 3, SQ_CHATTER},
    {"CHEER", "FAST, RISING, AAH", 520.0f, 5, SQ_CHEER},
    {"QUESTION", "UP AT THE END", 480.0f, 4, SQ_QUESTION},
    {"GRUMBLE", "LOW, SLOW, DARK", 300.0f, 4, SQ_GRUMBLE},
    {"GIGGLE", "QUICK HIGH BURSTS", 720.0f, 3, SQ_GIGGLE},
};
static const ch_cdef_t CH_SQUID = {"SQUIDLAD", SQ_NP, 5, SQ_P, SQ_CALLS, 0.01f, 0.05f};

/* the vowels: the throat, the back of the mouth, its front, the lips (cm2) */
static const float SQ_VOWEL[SQ_NV][4] = {
    {2.6f, 2.4f, 0.3f, 1.4f},                 /* EE */
    {1.8f, 2.0f, 1.2f, 2.0f},                 /* EH */
    {0.7f, 2.4f, 3.2f, 3.2f},                 /* AH */
    {1.2f, 0.8f, 2.4f, 0.8f},                 /* OH */
    {2.4f, 0.4f, 2.4f, 0.25f},                /* OO */
};

typedef struct {
    uint32_t seed;
    float t, dur;                             /* the syllable: its time so far, its length (s) */
    int vowel, stop;                          /* its vowel; its stop: 0 the lips, 1 the tip, 2 the back */
    float hop, hop_from;                      /* its pitch (semitones from PITCH), the last one's */
    float area[4], area_from[4];              /* its vowel's areas, the last one's (glided between) */
    int syl;                                  /* (shown: syllables so far) */
    float open;                               /* (shown: the lips now, 0..1) */
    float hop_now;                            /* (shown: the pitch's hop now, semitones, BABBLE's share) */
} squid_t;

static float sq_rand(squid_t *s)              /* 0..1 */
{
    s->seed = s->seed * 1664525u + 1013904223u;
    return (float)(s->seed >> 8) / 16777216.0f;
}

static void sq_next(squid_t *s, const float *p)   /* the next syllable: its length, vowel, stop and pitch */
{
    int i;
    float r;
    s->t = 0;
    s->dur = (0.7f + 0.6f * sq_rand(s)) / p[SQ_RATE];
    for (i = 0; i < 4; i++)
        s->area_from[i] = s->area[i];
    r = sq_rand(s);                           /* MOUTH 1: EE, EH, AH (bright); 0: OH, OO (dark); between, a mix */
    if (sq_rand(s) < p[SQ_MOUTH])
        s->vowel = r < 0.34f ? 0 : r < 0.67f ? 1 : 2;
    else
        s->vowel = r < 0.5f ? 3 : 4;
    s->stop = (int)(sq_rand(s) * 3.0f) % 3;
    s->hop_from = s->hop;
    s->hop = (2.0f * sq_rand(s) - 1.0f) * p[SQ_HOP];
    s->syl++;
}

static void squid_init(squid_t *s)
{
    int i;
    s->seed = 0x5eed1e55u;                    /* (fixed: the same babble each time) */
    s->t = s->dur = 0;
    s->vowel = 2;
    s->stop = 0;
    s->hop = s->hop_from = 0;
    for (i = 0; i < 4; i++)
        s->area[i] = s->area_from[i] = SQ_VOWEL[2][i];
    s->syl = 0;
    s->open = 0;
    s->hop_now = 0;
}

/* its waveguide anew (the output's rate): a small larynx, no nose */
static void squid_wave(ch_wave_t *w, float fs, int ctrl)
{
    ch_wave_init(w, fs, ctrl, 1.0f, SQ_LEN);
    w->port_len = 0.25f;
    w->port_slit = 0.9f;
    w->wall_rad = 0.3f;
    w->output_gain = SQ_GAIN;
}

/* a noise source: its gain and band, read at `at` (cm from the glottis), going in either side of it */
static void sq_noise(ch_shape_t *s, int j, float fs, float gain, float lo, float hi, float at, float d)
{
    ch_noise_t *n = &s->noise[j];
    n->src = (uint16_t)j;
    n->fwd = (uint16_t)(j + 1);
    n->bwd = (uint16_t)(j ? j + 2 : 0);
    n->gain = gain;
    n->lp_coef = 1.0f - fm_expf(-2.0f * 3.1415927f * hi / fs);
    n->hp_coef = fm_expf(-2.0f * 3.1415927f * lo / fs);
    n->hp = n->lp1 = n->lp2 = n->level = 0;
    s->noise_s[j][0] = at;
    s->noise_s[j][1] = at + d;
    s->noise_s[j][2] = j ? at - d : -1;
}

/* a control step: the babble (its syllable, its stop, its pitch), the glottis, the lungs, the tube */
static void squid_control(squid_t *s, const ch_cvoice_t *v, float dt, float fs, ch_glottis_params_t *g, float *p_lung,
                          ch_shape_t *shape)
{
    const float *p = v->value, bab = p[SQ_BABBLE], squish = p[SQ_SQUISH];
    const float dor = SQ_OR_LEN / (float)SQ_NOR;
    float a[4], steady[4], glide, shut, swell, hop, mo;
    int i;
    if (v->gate < 0.01f) {                    /* (silent: the next note starts a syllable afresh) */
        s->t = s->dur;
    } else {
        s->t += dt;
    }
    if (s->t >= s->dur)
        sq_next(s, p);
    glide = fm_clampf(s->t / (0.33f * s->dur), 0, 1);
    glide = glide * glide * (3.0f - 2.0f * glide);   /* (smooth) */
    /* the vowel: the babble's, glided from the last; BABBLE 0, a steady one between OO and EE at MOUTH */
    mo = p[SQ_MOUTH];
    for (i = 0; i < 4; i++) {
        steady[i] = SQ_VOWEL[4][i] + (SQ_VOWEL[0][i] - SQ_VOWEL[4][i]) * mo;
        a[i] = s->area_from[i] + (SQ_VOWEL[s->vowel][i] - s->area_from[i]) * glide;
        s->area[i] = a[i];
        a[i] = steady[i] + (a[i] - steady[i]) * bab;
    }
    if (glide >= 1.0f)
        for (i = 0; i < 4; i++)
            s->area[i] = SQ_VOWEL[s->vowel][i];
    /* the stop: shut at the syllable's start, let go over ~25 ms (SQUISH: how shut) */
    shut = bab * (0.4f + 0.6f * squish) * (1.0f - fm_clampf(s->t / 0.025f, 0, 1));
    a[s->stop == 0 ? 3 : s->stop == 1 ? 2 : 1] *= 1.0f - 0.97f * shut;
    /* the breath: swelling through the syllable, dipping at its edges */
    swell = 1.0f - bab * 0.45f * (1.0f - fm_sinf(3.1415927f * fm_clampf(s->t / s->dur, 0, 1)));
    *p_lung = p[SQ_PRESSURE] * SQ_CMH2O * v->gate * swell;
    /* the voice: small and bright, its pitch hopping */
    hop = (s->hop_from + (s->hop - s->hop_from) * glide) * bab;
    s->hop_now = hop;
    g->f0 = p[SQ_PITCH] * fm_exp2f(hop / 12.0f);
    g->amplitude = 0.16f;
    g->abduction = 0.03f;
    g->chink = 0.006f;
    g->open_quotient = 0.5f;
    g->skew = 0.6f;
    g->closure_sharpness = 0.6f;
    g->effort = 0.6f;
    g->jitter = 0.008f;
    g->shimmer = 0.03f;
    g->vibrato_depth = 0;
    g->vibrato_rate = 5.0f;
    g->drift = 1.0f;
    g->fry = g->diplophonia = g->closure = g->chaos = 0;
    /* the tube */
    shape->n_ph = SQ_NPH;
    shape->n_or = SQ_NOR;
    shape->ph_len = SQ_PH_LEN;
    shape->or_len = SQ_OR_LEN;
    for (i = 0; i < SQ_NPH; i++)
        shape->ph_areas[i] = shape->ph_min[i] = a[0];
    for (i = 0; i < SQ_NOR; i++)
        shape->or_areas[i] = shape->or_min[i] = a[i < 4 ? 1 : i < SQ_NOR - 2 ? 2 : 3];
    shape->velum_port_area = 0;
    s->open = fm_clampf(a[3] / 3.2f, 0, 1);
    /* the breath at the glottis; the pop where the stop lets go (SQUISH: how wet) */
    sq_noise(shape, 0, fs, 1.5e-6f, 300.0f, 5000.0f, 0, 2.0f * SQ_PH_LEN / (float)SQ_NPH);
    shape->n_noise = 1;
    if (shut > 0.02f && squish > 0.02f) {
        const float at = s->stop == 0 ? SQ_PH_LEN + (float)(SQ_NOR - 2) * dor
                       : s->stop == 1 ? SQ_PH_LEN + 6.0f * dor : SQ_PH_LEN + 2.0f * dor;
        sq_noise(shape, 1, fs, 3e-5f * squish * shut, 800.0f, 7000.0f, at, dor);
        shape->n_noise = 2;
    }
}

#endif
