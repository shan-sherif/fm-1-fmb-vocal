/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 *
 * The cat: FMB Vocal's own creature in the Bestiary (CHIMERA's Bestiary has none). It runs on the engine's creature
 * voice (ch_creature.h: its parameters, gate and keyframed calls) and the FM-1's waveguide, as the seal does, with its
 * own control here (the engine's files stay as they are, for fm-1-chimera-vocal's pulls). The patterns follow the
 * engine's mammals (ch_creature.c: a uniform throat, a mouth opening over its last cells, breath noise at the glottis,
 * MIT, Jameson Hodge); the cat's numbers are its own, from a small cat's airway; its level set in the simulator (not yet heard on the FM-1):
 *   - a short airway: the throat ~3.5 cm, the mouth ~5 cm, a 4.5 cm nose behind the velum;
 *   - a small larynx, pitch 120-1600 Hz (a meow sits at 400-900 Hz);
 *   - the purr: the voice pulsed at 20-30 Hz, each pulse a burst of the folds' low buzz (the larynx's muscles
 *     twitching), PURR also taking the pitch down toward 50 Hz;
 *   - the hiss: the folds apart, no voice, the air hissing at the teeth with the mouth open.
 * Included once, by app/app.c (the unity build). No allocation: its control runs in the audio interrupt. */
#ifndef FMB_CAT_H
#define FMB_CAT_H
#include "ch_creature.h"

enum { CAT_PITCH, CAT_MOUTH, CAT_PRESSURE, CAT_NOSE, CAT_ROUGH, CAT_PURR, CAT_PURR_RATE, CAT_HISS, CAT_NP };

#define CAT_LEN 9.0f                          /* its airway (cm): the waveguide's sections */
#define CAT_PH_LEN 3.5f
#define CAT_OR_LEN 5.5f
#define CAT_NPH 6
#define CAT_NOR 10                            /* (the mouth's opening its last two cells) */
#define CAT_CMH2O 980.665f
#define CAT_GAIN 1.3e-7f                      /* its output gain: a held note ~1 dB under the seal's, nothing clipping (cat.txt) */

static const ch_cparam_t CAT_P[CAT_NP] = {
    {"PITCH", 120.0f, 1600.0f, 650.0f, 0.012f, 1, 1, "HZ"},
    {"MOUTH", 0.0f, 1.0f, 0.55f, 0.025f, 0, 0, ""},
    {"BREATH", 2.0f, 20.0f, 8.0f, 0.015f, 0, 0, "CMH2O"},
    {"NOSE", 0.0f, 1.0f, 0.25f, 0.04f, 0, 0, ""},
    {"ROUGH", 0.0f, 0.8f, 0.06f, 0.05f, 0, 0, ""},
    {"PURR", 0.0f, 1.0f, 0.0f, 0.03f, 0, 0, ""},
    {"RATE", 18.0f, 35.0f, 26.0f, 0.05f, 0, 0, "HZ"},
    {"HISS", 0.0f, 1.0f, 0.0f, 0.03f, 0, 0, ""},
};
#define G CH_CK_GATE
static const ch_ckey_t CAT_MEOW[] = {         /* "mi-a-ow": the mouth opening as the pitch rises, closing as it falls */
    {0.0f, CH_CURVE_LIN, 5, {G, CAT_PITCH, CAT_MOUTH, CAT_HISS, CAT_PURR}, {1, 600.0f, 0.04f, 0, 0}},
    {0.25f, CH_CURVE_LIN, 2, {CAT_PITCH, CAT_MOUTH}, {790.0f, 0.95f}},
    {0.6f, CH_CURVE_LIN, 2, {CAT_PITCH, CAT_MOUTH}, {640.0f, 0.6f}},
    {0.85f, CH_CURVE_LIN, 2, {CAT_PITCH, CAT_MOUTH}, {520.0f, 0.06f}},
    {0.9f, CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t CAT_PURR_K[] = {       /* breathing out, a turn, breathing in (quieter), out again */
    {0.0f, CH_CURVE_LIN, 6, {G, CAT_NOSE, CAT_MOUTH, CAT_PURR, CAT_ROUGH, CAT_PRESSURE}, {1, 1.0f, 0, 1.0f, 0.35f, 8.0f}},
    {1.3f, CH_CURVE_LIN, 1, {CAT_PRESSURE}, {8.0f}},
    {1.45f, CH_CURVE_LIN, 1, {CAT_PRESSURE}, {5.5f}},
    {2.7f, CH_CURVE_LIN, 1, {CAT_PRESSURE}, {5.5f}},
    {2.85f, CH_CURVE_LIN, 1, {CAT_PRESSURE}, {8.0f}},
    {3.6f, CH_CURVE_LIN, 1, {G}, {1}},
    {3.8f, CH_CURVE_LIN, 6, {G, CAT_PURR, CAT_ROUGH, CAT_PRESSURE, CAT_MOUTH, CAT_NOSE}, {0, 0, 0.06f, 8.0f, 0.55f, 0.25f}},
};
static const ch_ckey_t CAT_HISS_K[] = {
    {0.0f, CH_CURVE_LIN, 4, {G, CAT_HISS, CAT_MOUTH, CAT_PRESSURE}, {0, 1.0f, 0.85f, 12.0f}},
    {0.06f, CH_CURVE_LIN, 1, {G}, {1}},
    {1.1f, CH_CURVE_LIN, 1, {G}, {1}},
    {1.3f, CH_CURVE_LIN, 4, {G, CAT_HISS, CAT_PRESSURE, CAT_MOUTH}, {0, 0, 8.0f, 0.55f}},
};
static const ch_ckey_t CAT_TRILL[] = {        /* the chirrup: short, rising, rolled, the mouth barely open */
    {0.0f, CH_CURVE_LIN, 6, {G, CAT_PITCH, CAT_MOUTH, CAT_PURR, CAT_PURR_RATE, CAT_HISS}, {1, 480.0f, 0.05f, 0.7f, 32.0f, 0}},
    {0.14f, CH_CURVE_EXP, 2, {CAT_PITCH, CAT_MOUTH}, {820.0f, 0.35f}},
    {0.2f, CH_CURVE_LIN, 3, {G, CAT_PURR, CAT_PURR_RATE}, {0, 0, 26.0f}},
};
static const ch_ckey_t CAT_YOWL[] = {         /* long and low, the mouth slowly open and shut */
    {0.0f, CH_CURVE_LIN, 5, {G, CAT_PITCH, CAT_MOUTH, CAT_ROUGH, CAT_PURR}, {1, 380.0f, 0.1f, 0.12f, 0}},
    {0.7f, CH_CURVE_LIN, 2, {CAT_PITCH, CAT_MOUTH}, {560.0f, 0.9f}},
    {1.5f, CH_CURVE_LIN, 2, {CAT_PITCH, CAT_MOUTH}, {470.0f, 0.7f}},
    {2.1f, CH_CURVE_LIN, 3, {CAT_PITCH, CAT_MOUTH, CAT_ROUGH}, {330.0f, 0.1f, 0.06f}},
    {2.2f, CH_CURVE_LIN, 1, {G}, {0}},
};
#undef G
static const ch_ccall_t CAT_CALLS[] = {
    {"MEOW", "OPENS AND CLOSES", 700.0f, 5, CAT_MEOW},
    {"PURR", "LOW, PULSED", 0.0f, 7, CAT_PURR_K},
    {"HISS", "OPEN MOUTH, AIR", 0.0f, 4, CAT_HISS_K},
    {"TRILL", "A ROLLED CHIRRUP", 600.0f, 3, CAT_TRILL},
    {"YOWL", "LONG, LOW, SLOW", 420.0f, 5, CAT_YOWL},
};
static const ch_cdef_t CH_CAT = {"CAT", CAT_NP, 5, CAT_P, CAT_CALLS, 0.008f, 0.04f};

typedef struct {
    float purr_phase;
} cat_t;

static void cat_init(cat_t *c) { c->purr_phase = 0; }

/* its waveguide anew (the output's rate): the glottal port of a small larynx, the nose behind the velum */
static void cat_wave(ch_wave_t *w, float fs, int ctrl)
{
    ch_wave_init(w, fs, ctrl, 1.0f, CAT_LEN);
    w->port_len = 0.3f;
    w->port_slit = 0.65f;
    w->wall_rad = 0.3f;
    w->glottis.threshold = 1.5f * CAT_CMH2O;
    w->output_gain = CAT_GAIN;
    ch_wave_nose(w, 0, 4.5f, 0.35f, 0.3f);
}

/* a noise source: its gain and band, where its flow is read, where it goes in (cm from the glottis; bwd -1: below) */
static void cat_noise(ch_shape_t *s, int j, float fs, float gain, float lo, float hi, float at, float fwd, float bwd)
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
    s->noise_s[j][1] = fwd;
    s->noise_s[j][2] = bwd;
}

/* a control step: the lungs (pulsed while it purrs), the glottis, the tubes, the velum, the breath and the hiss */
static void cat_control(cat_t *c, const ch_cvoice_t *v, float dt, float fs, ch_glottis_params_t *g, float *p_lung,
                        ch_shape_t *shape)
{
    const float *p = v->value, hiss = p[CAT_HISS], rough = p[CAT_ROUGH], purr = p[CAT_PURR];
    const float open = 0.08f + 2.0f * p[CAT_MOUTH], dor = CAT_OR_LEN / (float)CAT_NOR;
    float pulse, mod;
    int i;
    c->purr_phase += p[CAT_PURR_RATE] * dt;
    c->purr_phase -= fm_floorf(c->purr_phase);
    pulse = c->purr_phase < 0.45f ? fm_sinf(3.1415927f * c->purr_phase / 0.45f) : 0;
    mod = 1.0f - purr + purr * pulse;
    *p_lung = p[CAT_PRESSURE] * CAT_CMH2O * v->gate * (0.35f + 0.65f * mod);
    /* the glottis: a small larynx, apart while it hisses */
    g->f0 = p[CAT_PITCH] * fm_powf(50.0f / p[CAT_PITCH], purr);   /* (purring: down toward a 50 Hz buzz) */
    g->amplitude = 0.16f * (1.0f - hiss) * mod;
    g->abduction = 0.02f + 0.8f * hiss;
    g->chink = 0.008f;
    g->open_quotient = 0.5f;
    g->skew = 0.6f;
    g->closure_sharpness = 0.7f;
    g->effort = 0.7f;
    g->jitter = 0.004f + 0.03f * rough;
    g->shimmer = 0.02f + 0.2f * rough;
    g->vibrato_depth = 0;
    g->vibrato_rate = 5.0f;
    g->drift = 3.0f;
    g->fry = g->diplophonia = g->closure = 0;
    g->chaos = rough;
    /* the tubes: a narrow throat, the mouth opening over its last fifth; the velum's port as open as NOSE */
    shape->n_ph = CAT_NPH;
    shape->n_or = CAT_NOR;
    shape->ph_len = CAT_PH_LEN;
    shape->or_len = CAT_OR_LEN;
    for (i = 0; i < CAT_NPH; i++)
        shape->ph_areas[i] = shape->ph_min[i] = 0.8f;
    for (i = 0; i < CAT_NOR; i++)
        shape->or_areas[i] = shape->or_min[i] = ((float)i + 0.5f) / (float)CAT_NOR > 0.8f ? open : 1.0f;
    shape->velum_port_area = 0.02f + 0.5f * p[CAT_NOSE];
    /* the breath at the glottis (as the mammals'); the hiss at the teeth, the mouth's last inside cell */
    cat_noise(shape, 0, fs, 2e-6f + 2e-5f * hiss, 250.0f, 4000.0f, 0, 2.0f * CAT_PH_LEN / (float)CAT_NPH, -1);
    shape->n_noise = 1;
    if (hiss > 0.01f) {
        const float teeth = CAT_PH_LEN + (float)(CAT_NOR - 2) * dor;
        cat_noise(shape, 1, fs, 4e-5f * hiss, 2000.0f, 9000.0f, teeth, teeth + dor, teeth - dor);
        shape->n_noise = 2;
    }
}

#endif
