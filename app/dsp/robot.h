/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 *
 * The robot: FMB Vocal's second creature of its own in the Bestiary, after the cat. A dubstep voice on the same
 * pieces as the others: the engine's creature voice (ch_creature.h: its parameters, gate and keyframed calls) and the
 * FM-1's waveguide, with its own control here (the engine's files stay as they are, for fm-1-chimera-vocal's pulls).
 * The wobble is a vowel: a dubstep "wub" is a buzzing tone under a filter swept by an LFO, and a mouth going from "oo"
 * to "aa" sweeps the same formants, so the LFO moves the robot's tube between the two shapes:
 *   - its voice box: the glottis held steady (no jitter, drift or vibrato) and made harsh: closing sharply, open for a
 *     short part of the cycle (BUZZ); GRIT adds the growl, the folds' chaos and a subharmonic (diplophonia), and fizz;
 *   - its tube: 16 cm, as long as a man's tract, so the vowels' formants sit where a voice's do; "oo": the throat wide,
 *     the mouth narrow, the opening small; "aa": the throat narrow, the mouth wide, the opening wide;
 *   - the LFO (WOBBLE, its rate; DEPTH, how far it swings from MOUTH toward the full sweep), from "oo" at each note's
 *     start, so every note wubs the same way in.
 * Its level set in the simulator (not yet heard on the FM-1). Included once, by app/app.c (the unity build). No
 * allocation: its control runs in the audio interrupt. */
#ifndef FMB_ROBOT_H
#define FMB_ROBOT_H
#include "ch_creature.h"

enum { ROB_PITCH, ROB_WOBBLE, ROB_DEPTH, ROB_GRIT, ROB_BUZZ, ROB_MOUTH, ROB_PRESSURE, ROB_NP };

#define ROB_LEN 16.0f                         /* its tube (cm): the waveguide's sections */
#define ROB_PH_LEN 8.0f
#define ROB_OR_LEN 8.0f
#define ROB_NPH 8
#define ROB_NOR 12                            /* (the opening its last two cells) */
#define ROB_CMH2O 980.665f
#define ROB_GAIN 1.4e-7f                      /* its output gain: a held note about as loud as the cat's and under the
                                               * waveguide's limiter (robot.txt) */

static const ch_cparam_t ROB_P[ROB_NP] = {
    {"PITCH", 30.0f, 700.0f, 110.0f, 0.01f, 1, 1, "HZ"},   /* (keys: C4 an octave down, 131 Hz) */
    {"WOBBLE", 0.5f, 16.0f, 3.0f, 0.05f, 1, 0, "HZ"},
    {"DEPTH", 0.0f, 1.0f, 0.85f, 0.03f, 0, 0, ""},
    {"GRIT", 0.0f, 1.0f, 0.25f, 0.05f, 0, 0, ""},
    {"BUZZ", 0.0f, 1.0f, 0.7f, 0.05f, 0, 0, ""},
    {"MOUTH", 0.0f, 1.0f, 0.45f, 0.03f, 0, 0, ""},
    {"BREATH", 4.0f, 30.0f, 14.0f, 0.015f, 0, 0, "CMH2O"},
};
#define G CH_CK_GATE
static const ch_ckey_t ROB_WUB[] = {          /* the knobs' wobble, held two seconds */
    {0.0f, CH_CURVE_LIN, 2, {G, ROB_PITCH}, {1, 110.0f}},
    {1.9f, CH_CURVE_LIN, 1, {G}, {1}},
    {2.0f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t ROB_YOI[] = {          /* the growl: no LFO, the mouth swept open and shut by hand, gritty */
    {0.0f, CH_CURVE_LIN, 6, {G, ROB_PITCH, ROB_DEPTH, ROB_GRIT, ROB_MOUTH, ROB_BUZZ}, {1, 80.0f, 0, 0.75f, 0.05f, 1.0f}},
    {0.18f, CH_CURVE_LIN, 1, {ROB_MOUTH}, {0.95f}},
    {0.3f, CH_CURVE_LIN, 2, {ROB_MOUTH, ROB_PITCH}, {0.8f, 76.0f}},
    {0.55f, CH_CURVE_LIN, 1, {ROB_MOUTH}, {0.12f}},
    {0.6f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t ROB_LASER[] = {        /* a zap: high, diving fast, the mouth open, clean, softer breath (high
                                               * and open is loud: under the limiter) */
    {0.0f, CH_CURVE_LIN, 6, {G, ROB_PITCH, ROB_DEPTH, ROB_MOUTH, ROB_GRIT, ROB_PRESSURE}, {1, 700.0f, 0, 0.9f, 0, 6.0f}},
    {0.3f, CH_CURVE_EXP, 1, {ROB_PITCH}, {45.0f}},
    {0.33f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t ROB_RISER[] = {        /* up, the wobble faster and grittier as it goes */
    {0.0f, CH_CURVE_LIN, 5, {G, ROB_PITCH, ROB_WOBBLE, ROB_DEPTH, ROB_GRIT}, {1, 70.0f, 2.0f, 0.7f, 0.1f}},
    {2.4f, CH_CURVE_EXP, 3, {ROB_PITCH, ROB_WOBBLE, ROB_GRIT}, {560.0f, 16.0f, 0.5f}},
    {2.45f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t ROB_DROP[] = {         /* a dive into a slow, deep wub */
    {0.0f, CH_CURVE_LIN, 6, {G, ROB_PITCH, ROB_WOBBLE, ROB_DEPTH, ROB_GRIT, ROB_MOUTH}, {1, 440.0f, 3.0f, 0, 0.2f, 0.9f}},
    {0.0f, CH_CURVE_LIN, 1, {ROB_PRESSURE}, {6.0f}},   /* (the dive's softer breath, as LASER's) */
    {0.35f, CH_CURVE_EXP, 1, {ROB_PITCH}, {55.0f}},
    {0.36f, CH_CURVE_LIN, 3, {ROB_DEPTH, ROB_GRIT, ROB_PRESSURE}, {1.0f, 0.6f, 12.0f}},
    {2.4f, CH_CURVE_LIN, 1, {G}, {1}},
    {2.5f, CH_CURVE_LIN, 1, {G}, {0}},
};
#undef G
static const ch_ccall_t ROB_CALLS[] = {
    {"WUB", "THE KNOBS' WOBBLE", 110.0f, 3, ROB_WUB},
    {"YOI", "A GROWL, MOUTH SWEPT", 80.0f, 5, ROB_YOI},
    {"LASER", "A FALLING ZAP", 0.0f, 3, ROB_LASER},
    {"RISER", "UP, FASTER, GRITTIER", 70.0f, 3, ROB_RISER},
    {"DROP", "A DIVE INTO THE WUB", 110.0f, 6, ROB_DROP},
};
static const ch_cdef_t CH_ROBOT = {"ROBOT", ROB_NP, 5, ROB_P, ROB_CALLS, 0.005f, 0.03f};

typedef struct {
    float phase;                              /* the LFO's, 0..1 */
    float open;                               /* (shown: the vowel now, 0 "oo" .. 1 "aa") */
    float lfo;                                /* (shown: the LFO now, 0..1) */
} robot_t;

static void robot_init(robot_t *r)
{
    r->phase = 0;
    r->open = 0;
    r->lfo = 0;
}

/* its waveguide anew (the output's rate): a rigid tube, no nose */
static void robot_wave(ch_wave_t *w, float fs, int ctrl)
{
    ch_wave_init(w, fs, ctrl, 1.0f, ROB_LEN);
    w->port_len = 0.3f;
    w->port_slit = 1.4f;
    w->wall_rad = 0.1f;
    w->output_gain = ROB_GAIN;
}

/* a noise source at the glottis (as the cat's: its gain and band, where its flow is read) */
static void robot_noise(ch_shape_t *s, float fs, float gain)
{
    ch_noise_t *n = &s->noise[0];
    n->src = 0;
    n->fwd = 1;
    n->bwd = 0;
    n->gain = gain;
    n->lp_coef = 1.0f - fm_expf(-2.0f * 3.1415927f * 8000.0f / fs);
    n->hp_coef = fm_expf(-2.0f * 3.1415927f * 1000.0f / fs);
    n->hp = n->lp1 = n->lp2 = n->level = 0;
    s->noise_s[0][0] = 0;
    s->noise_s[0][1] = 2.0f * ROB_PH_LEN / (float)ROB_NPH;
    s->noise_s[0][2] = -1;
    s->n_noise = 1;
}

static float rob_lerp_log(float a, float b, float w) { return a * fm_powf(b / a, w); }

/* a control step: the LFO, the vowel it makes, the glottis, the lungs, the tube */
static void robot_control(robot_t *r, const ch_cvoice_t *v, float dt, float fs, ch_glottis_params_t *g, float *p_lung,
                          ch_shape_t *shape)
{
    const float *p = v->value, grit = p[ROB_GRIT], buzz = p[ROB_BUZZ];
    float w, ph_a, or_a;
    int i;
    if (v->gate < 0.01f)                      /* (silent: the next note wubs in from "oo") */
        r->phase = 0;
    else {
        r->phase += p[ROB_WOBBLE] * dt;
        r->phase -= fm_floorf(r->phase);
    }
    r->lfo = 0.5f - 0.5f * fm_cosf(6.2831853f * r->phase);
    w = p[ROB_MOUTH] + p[ROB_DEPTH] * (r->lfo - p[ROB_MOUTH]);   /* (DEPTH 0: MOUTH; 1: the LFO's whole sweep) */
    w = fm_clampf(w, 0, 1);
    r->open = w;
    *p_lung = p[ROB_PRESSURE] * ROB_CMH2O * v->gate;
    /* the voice box: steady and harsh; GRIT its growl */
    g->f0 = p[ROB_PITCH];
    g->amplitude = 0.2f;
    g->abduction = 0.02f;
    g->chink = 0.005f;
    g->open_quotient = 0.6f - 0.25f * buzz;
    g->skew = 0.6f + 0.25f * buzz;
    g->closure_sharpness = 0.5f + 0.5f * buzz;
    g->effort = 0.6f + 0.4f * buzz;
    g->jitter = 0.02f * grit;
    g->shimmer = 0.15f * grit;
    g->vibrato_depth = 0;
    g->vibrato_rate = 5.0f;
    g->drift = 0;
    g->fry = g->closure = 0;
    g->diplophonia = 0.8f * grit;
    g->chaos = 0.6f * grit;
    /* the tube: "oo" (w 0: the throat wide, the back of the mouth narrowed, the lips small and round) to "aa" (w 1: the
     * throat narrow, the mouth and the lips wide), each area between the two geometrically (an even sweep to the ear) */
    ph_a = rob_lerp_log(3.2f, 0.6f, w);
    shape->n_ph = ROB_NPH;
    shape->n_or = ROB_NOR;
    shape->ph_len = ROB_PH_LEN;
    shape->or_len = ROB_OR_LEN;
    for (i = 0; i < ROB_NPH; i++)
        shape->ph_areas[i] = shape->ph_min[i] = ph_a;
    for (i = 0; i < ROB_NOR; i++) {
        if (i < 4)                            /* the back of the mouth */
            or_a = rob_lerp_log(0.35f, 3.5f, w);
        else if (i < ROB_NOR - 2)             /* its middle */
            or_a = rob_lerp_log(2.5f, 4.0f, w);
        else                                  /* the lips */
            or_a = rob_lerp_log(0.08f, 5.0f, w);
        shape->or_areas[i] = shape->or_min[i] = or_a;
    }
    shape->velum_port_area = 0;
    robot_noise(shape, fs, 5e-7f + 8e-6f * grit);   /* (a little air; GRIT's fizz) */
}

#endif
