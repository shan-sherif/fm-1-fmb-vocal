/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * A port of CHIMERA's CreatureVoice control (creatures/voice.rs) and its seal (creatures/seal.rs, mammal.rs); see
 * ch_creature.h. The comments are CHIMERA's, shortened. */
#define CH_RAM   /* the songbird's audio rate in RAM on the FM-1 (ch_common.h CH_HOT), with the waveguide */
#include "ch_creature.h"
#include "ch_common.h"

#define CMH2O R(980.665)

/* ----------------------------------------------------------------- the voice --- */
void ch_cvoice_init(ch_cvoice_t *v, const ch_cdef_t *def)
{
    int i;
    v->def = def;
    for (i = 0; i < def->np; i++)
        v->performer[i] = v->target[i] = v->value[i] = def->p[i].def;
    v->gate = v->gate_target = v->call_gate = v->clock = 0;
    v->call = -1;
    v->call_t0 = 0;
    v->call_pitch = v->call_amp = R(1.0);
}

void ch_cvoice_set(ch_cvoice_t *v, int i, ch_real x)
{
    const ch_cparam_t *p = &v->def->p[i];
    int k, owned = 0;
    x = clampr(x, p->min, p->max);
    v->performer[i] = x;
    if (v->call >= 0) {                       /* (a call playing keeps the parameters it moves) */
        const ch_ccall_t *c = &v->def->calls[v->call];
        for (k = 0; k < c->n && !owned; k++) {
            int j;
            for (j = 0; j < c->k[k].n; j++)
                if (c->k[k].key[j] == i)
                    owned = 1;
        }
    }
    if (!owned)
        v->target[i] = x;
}

void ch_cvoice_gate(ch_cvoice_t *v, ch_real g) { v->gate_target = g; }

/* the call ends: the parameters it moved go back to the performer */
void ch_cvoice_stop_call(ch_cvoice_t *v)
{
    int k, j;
    if (v->call >= 0) {
        const ch_ccall_t *c = &v->def->calls[v->call];
        for (k = 0; k < c->n; k++)
            for (j = 0; j < c->k[k].n; j++)
                if (c->k[k].key[j] >= 0)
                    v->target[c->k[k].key[j]] = v->performer[c->k[k].key[j]];
    }
    v->call = -1;
    v->call_gate = 0;
}

void ch_cvoice_call(ch_cvoice_t *v, int call, ch_real pitch, ch_real amp)
{
    const ch_real r = call >= 0 && call < v->def->ncalls ? v->def->calls[call].pitch_ref : 0;
    if (call < 0 || call >= v->def->ncalls)
        return;
    ch_cvoice_stop_call(v);
    v->call = call;
    v->call_t0 = v->clock;
    v->call_pitch = pitch > 0 && r > 0 ? pitch / r : R(1.0);
    v->call_amp = amp;
}

/* the call's automation at its time: each target between the keys around it (eval_call) */
static void eval_call(ch_cvoice_t *v)
{
    const ch_ccall_t *c;
    ch_real t;
    int name;
    if (v->call < 0)
        return;
    c = &v->def->calls[v->call];
    t = v->clock - v->call_t0;
    if (t > c->k[c->n - 1].t + R(0.05)) {
        ch_cvoice_stop_call(v);
        return;
    }
    for (name = CH_CK_GATE; name < v->def->np; name++) {
        int k, j, has_prev = 0, has_next = 0;
        ch_real prev_t = 0, prev_x = 0, next_t = 0, next_x = 0, x;
        uint8_t curve = CH_CURVE_LIN;
        for (k = 0; k < c->n; k++)
            for (j = 0; j < c->k[k].n; j++) {
                if (c->k[k].key[j] != name)
                    continue;
                if (c->k[k].t <= t) {
                    has_prev = 1;
                    prev_t = c->k[k].t;
                    prev_x = c->k[k].v[j];
                } else if (!has_next) {
                    has_next = 1;
                    next_t = c->k[k].t;
                    next_x = c->k[k].v[j];
                    curve = c->k[k].curve;
                }
            }
        if (!has_prev)
            continue;
        x = prev_x;
        if (has_next && curve != CH_CURVE_STEP) {
            const ch_real u = (t - prev_t) / mx(R(1e-6), next_t - prev_t);
            if (curve == CH_CURVE_EXP && prev_x > 0 && next_x > 0)
                x = prev_x * POW(next_x / prev_x, u);
            else
                x = prev_x + (next_x - prev_x) * u;
        }
        if (name == CH_CK_GATE) {
            v->call_gate = x * v->call_amp;
        } else {
            const ch_cparam_t *p = &v->def->p[name];
            if (p->pitch)
                x *= v->call_pitch;
            v->target[name] = clampr(x, p->min, p->max);
        }
    }
}

void ch_cvoice_step(ch_cvoice_t *v, ch_real dt)
{
    int i;
    ch_real goal, tau;
    eval_call(v);
    for (i = 0; i < v->def->np; i++) {
        const ch_cparam_t *p = &v->def->p[i];
        const ch_real a = R(1.0) - EXP(-dt / mx(R(0.001), p->smooth)), cur = v->value[i], tgt = v->target[i];
        v->value[i] = p->log && cur > 0 && tgt > 0 ? cur * POW(tgt / cur, a) : cur + (tgt - cur) * a;
    }
    goal = mx(v->gate_target, v->call_gate);
    tau = goal > v->gate ? v->def->attack : v->def->release;
    v->gate += (goal - v->gate) * (R(1.0) - EXP(-dt / tau));
    v->clock += dt;
}

ch_real ch_ckey_hz(const ch_cparam_t *p, ch_real midi)
{
    const ch_real c4 = R(261.6255653);
    ch_real f = R(440.0) * POW(R(2.0), (midi - R(69.0)) / R(12.0));
    ch_real r = SQRT(p->min * p->max) / c4;    /* round(log2(r)) octaves: r folded to [1/sqrt2, sqrt2) */
    while (r >= R(1.41421356)) {
        r *= R(0.5);
        f *= R(2.0);
    }
    while (r < R(0.70710678)) {
        r *= R(2.0);
        f *= R(0.5);
    }
    while (f > p->max)
        f *= R(0.5);
    while (f < p->min)
        f *= R(2.0);
    return mn(p->max, f);
}

/* ---------------------------------------------------------------------- the seal --- */
static const ch_cparam_t SEAL_P[CH_SEAL_NP] = {
    {"PITCH", R(40.0), R(6000.0), R(900.0), R(0.01), 1, 1, "HZ"},
    {"RATE", R(2.0), R(60.0), R(18.0), R(0.02), 1, 0, "HZ"},
    {"DEPTH", R(0.0), R(1.0), R(0.6), R(0.02), 0, 0, ""},
    {"DUTY", R(0.15), R(1.0), R(0.55), R(0.02), 0, 0, ""},
    {"BRIGHT", R(0.0), R(1.0), R(0.6), R(0.02), 0, 0, ""},
    {"ROUGH", R(0.0), R(0.8), R(0.05), R(0.05), 0, 0, ""},
    {"BREATH", R(2.0), R(25.0), R(10.0), R(0.01), 0, 0, "CMH2O"},
    {"MOUTH", R(0.0), R(1.0), R(0.35), R(0.03), 0, 0, ""},
};
#define G CH_CK_GATE
static const ch_ckey_t TRILL[] = {
    {R(0.0), CH_CURVE_LIN, 5, {G, CH_SEAL_PITCH, CH_SEAL_PULSE_RATE, CH_SEAL_PULSE, CH_SEAL_DUTY}, {1, R(3600.0), R(42.0), R(0.9), R(0.5)}},
    {R(3.6), CH_CURVE_EXP, 3, {G, CH_SEAL_PITCH, CH_SEAL_PULSE_RATE}, {1, R(95.0), R(6.0)}},
    {R(3.9), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t ZAP[] = {
    {R(0.0), CH_CURVE_LIN, 3, {G, CH_SEAL_PITCH, CH_SEAL_PULSE}, {1, R(6000.0), 0}},
    {R(0.15), CH_CURVE_EXP, 1, {CH_SEAL_PITCH}, {R(1100.0)}},
    {R(0.17), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t WHISTLE[] = {
    {R(0.0), CH_CURVE_LIN, 3, {G, CH_SEAL_PITCH, CH_SEAL_PULSE}, {1, R(1700.0), R(0.1)}},
    {R(0.45), CH_CURVE_EXP, 1, {CH_SEAL_PITCH}, {R(620.0)}},
    {R(0.95), CH_CURVE_EXP, 1, {CH_SEAL_PITCH}, {R(1500.0)}},
    {R(1.0), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t CHUG[] = {
    {R(0.0), CH_CURVE_LIN, 6, {G, CH_SEAL_PITCH, CH_SEAL_PULSE_RATE, CH_SEAL_PULSE, CH_SEAL_DUTY, CH_SEAL_ROUGH},
     {1, R(90.0), R(11.0), R(1.0), R(0.35), R(0.3)}},
    {R(0.9), CH_CURVE_LIN, 1, {G}, {1}},
    {R(0.95), CH_CURVE_LIN, 2, {G, CH_SEAL_ROUGH}, {0, R(0.05)}},
};
#undef G
static const ch_ccall_t SEAL_CALLS[] = {
    {"TRILL", "FALLING, PULSES SLOWING", R(3600.0), 3, TRILL},
    {"ZAP", "A LASER CHIRP", R(6000.0), 3, ZAP},
    {"WHISTLE", "DIPS AND RETURNS", R(1700.0), 4, WHISTLE},
    {"CHUG", "A LOW PULSED GRUNT", R(90.0), 3, CHUG},
};
const ch_cdef_t CH_SEAL = {"SEAL", CH_SEAL_NP, 4, SEAL_P, SEAL_CALLS, R(0.006), R(0.03)};

void ch_seal_init(ch_seal_t *s) { s->pulse_phase = 0; }

#define SEAL_NPH 8
#define SEAL_NOR 10                           /* (the mouth's last fifth: its last two cells) */

void ch_seal_control(ch_seal_t *s, const ch_cvoice_t *v, ch_real dt, ch_real fs, ch_glottis_params_t *g, ch_real *p_lung,
                     ch_shape_t *shape)
{
    const ch_real *p = v->value, mouth = p[CH_SEAL_MOUTH], b = p[CH_SEAL_BRIGHT];
    ch_real ph;
    int i;
    s->pulse_phase += p[CH_SEAL_PULSE_RATE] * dt;
    s->pulse_phase -= (ch_real)(long)s->pulse_phase;   /* (>= 0: its whole cycles) */
    ph = s->pulse_phase < p[CH_SEAL_DUTY] ? SIN((CH_PI * s->pulse_phase) / p[CH_SEAL_DUTY]) : 0;
    *p_lung = p[CH_SEAL_PRESSURE] * CMH2O * v->gate * (R(1.0) - p[CH_SEAL_PULSE] + p[CH_SEAL_PULSE] * ph);
    /* the glottis: CHIMERA's defaults, the seal's perturbations, its brightness */
    g->f0 = p[CH_SEAL_PITCH];
    g->amplitude = R(0.22);
    g->abduction = 0;
    g->chink = R(0.01);
    g->open_quotient = R(0.55) - R(0.25) * b;
    g->skew = R(0.62);
    g->closure_sharpness = R(0.4) + R(0.6) * b;
    g->effort = R(0.5) + R(0.5) * b;
    g->jitter = R(0.002);
    g->shimmer = R(0.01);
    g->vibrato_depth = 0;
    g->vibrato_rate = R(6.0);
    g->drift = R(2.0);
    g->fry = g->diplophonia = g->closure = 0;
    g->chaos = p[CH_SEAL_ROUGH];
    /* the tract: a uniform pharynx, the mouth opening at its end */
    shape->n_ph = SEAL_NPH;
    shape->n_or = SEAL_NOR;
    shape->ph_len = R(11.0);
    shape->or_len = R(12.0);
    shape->velum_port_area = 0;
    for (i = 0; i < SEAL_NPH; i++)
        shape->ph_areas[i] = shape->ph_min[i] = R(3.0);
    for (i = 0; i < SEAL_NOR; i++)
        shape->or_areas[i] = shape->or_min[i] = ((ch_real)i + R(0.5)) / (ch_real)SEAL_NOR > R(0.8) ? R(0.3) + R(4.0) * mouth : R(3.6);
    /* aspiration at the glottis (mammal_noise: 2e-6, 4 kHz low-pass, 250 Hz high-pass) */
    {
        ch_noise_t *n = &shape->noise[0];
        n->src = 0;
        n->fwd = 1;
        n->bwd = 0;
        n->gain = R(2e-6);
        n->lp_coef = R(1.0) - EXP((R(-2.0) * CH_PI * R(4000.0)) / fs);
        n->hp_coef = EXP((R(-2.0) * CH_PI * R(250.0)) / fs);
        n->hp = n->lp1 = n->lp2 = n->level = 0;
        shape->noise_s[0][0] = 0;
        shape->noise_s[0][1] = R(2.0) * shape->ph_len / (ch_real)SEAL_NPH;
        shape->noise_s[0][2] = -1;
        shape->n_noise = 1;
    }
}

/* ------------------------------------------------------------------ the songbird --- */

static const ch_cparam_t BIRD_P[CH_BIRD_NP] = {
    {"PITCH", R(400.0), R(9000.0), R(2600.0), R(0.008), 1, 1, "HZ"},
    {"INTERVAL", R(-12.0), R(12.0), R(7.0), R(0.02), 0, 0, "ST"},
    {"VOICES", R(0.0), R(1.0), R(0.5), R(0.02), 0, 0, ""},
    {"DRIVE", R(0.0), R(1.0), R(0.35), R(0.02), 0, 0, ""},
    {"TENSION", R(0.0), R(1.0), R(0.4), R(0.02), 0, 0, ""},
    {"BREATH", R(0.1), R(2.0), R(0.9), R(0.01), 0, 0, "KPA"},
    {"PURITY", R(0.0), R(1.0), R(0.8), R(0.02), 0, 0, ""},
    {"THROAT", R(0.05), R(2.0), R(0.4), R(0.03), 1, 0, "CM3"},
    {"GAPE", R(0.0), R(1.0), R(0.55), R(0.02), 0, 0, ""},
};
#define G CH_CK_GATE
#define P CH_BIRD_PITCH
static const ch_ckey_t FLUTE[] = {
    {R(0.0), CH_CURVE_LIN, 5, {G, P, CH_BIRD_VOICES, CH_BIRD_DRIVE, CH_BIRD_INTERVAL}, {0, R(2000.0), R(0.15), R(0.25), R(7.0)}},
    {R(0.02), CH_CURVE_LIN, 1, {G}, {1}},
    {R(0.2), CH_CURVE_EXP, 2, {G, P}, {1, R(2100.0)}},
    {R(0.23), CH_CURVE_LIN, 1, {G}, {0}},
    {R(0.3), CH_CURVE_STEP, 2, {G, P}, {1, R(2600.0)}},
    {R(0.5), CH_CURVE_EXP, 1, {P}, {R(2500.0)}},
    {R(0.53), CH_CURVE_LIN, 1, {G}, {0}},
    {R(0.6), CH_CURVE_STEP, 4, {G, P, CH_BIRD_VOICES, CH_BIRD_INTERVAL}, {1, R(3300.0), R(0.5), R(4.0)}},
    {R(1.1), CH_CURVE_EXP, 2, {P, CH_BIRD_INTERVAL}, {R(3500.0), R(5.0)}},
    {R(1.2), CH_CURVE_LIN, 1, {G}, {0}},
};
/* the trill, the chirps and the buzz are built as songbird.rs builds them (bird_calls) */
static ch_ckey_t TRILL_K[1 + 18 * 4], CHIRP_K[1 + 9 * 4], BUZZ_K[1 + 8 * 2 + 1];
static const ch_ccall_t BIRD_CALLS[] = {
    {"FLUTE", "THREE NOTES, A TWO-VOICE SHIMMER", R(2600.0), 10, FLUTE},
    {"TRILL", "16 UPSWEEPS A SECOND", R(3000.0), 1 + 18 * 4, TRILL_K},
    {"CHIRPS", "FALLING, FASTER AND FASTER", R(4500.0), 1 + 9 * 4, CHIRP_K},
    {"BUZZ", "RASPY, PULSED BY MINI-BREATHS", R(1200.0), 1 + 8 * 2 + 1, BUZZ_K},
};
const ch_cdef_t CH_BIRD = {"SONGBIRD", CH_BIRD_NP, 4, BIRD_P, BIRD_CALLS, R(0.004), R(0.012)};

static ch_ckey_t key2(ch_real t, uint8_t curve, int k0, ch_real v0, int k1, ch_real v1)
{
    ch_ckey_t k;
    k.t = t;
    k.curve = curve;
    k.n = (uint8_t)(k1 == -2 ? 1 : 2);
    k.key[0] = (int8_t)k0;
    k.v[0] = v0;
    k.key[1] = (int8_t)k1;
    k.v[1] = v1;
    return k;
}

static void bird_calls(void)
{
    static uint8_t done;
    int i, n;
    if (done)
        return;
    done = 1;
    /* the canary trill: 18 notes at 16 a second, each a 35 % upsweep from 3 kHz */
    TRILL_K[0] = key2(0, CH_CURVE_LIN, CH_BIRD_VOICES, R(0.15), CH_BIRD_DRIVE, R(0.3));
    for (i = 0, n = 1; i < 18; i++) {
        const ch_real per = R(1.0) / R(16.0), t = (ch_real)i * per, f0 = R(3000.0), sw = R(0.35);
        TRILL_K[n++] = key2(t, CH_CURVE_STEP, G, 0, P, f0 * (R(1.0) - sw / R(2.0)));
        TRILL_K[n++] = key2(t + per * R(0.08), CH_CURVE_LIN, G, 1, -2, 0);
        TRILL_K[n++] = key2(t + per * R(0.62), CH_CURVE_EXP, G, 1, P, f0 * (R(1.0) + sw / R(2.0)));
        TRILL_K[n++] = key2(t + per * R(0.75), CH_CURVE_LIN, G, 0, -2, 0);
    }
    /* the chirp cascade: nine falling chirps, each sooner than the last */
    CHIRP_K[0] = key2(0, CH_CURVE_LIN, CH_BIRD_VOICES, R(0.15), CH_BIRD_DRIVE, R(0.4));
    for (i = 0, n = 1; i < 9; i++) {
        const ch_real fi = (ch_real)i, t = fi * (R(0.14) - fi * R(0.008));
        CHIRP_K[n++] = key2(t, CH_CURVE_STEP, G, 0, P, R(7000.0) - fi * R(300.0));
        CHIRP_K[n++] = key2(t + R(0.01), CH_CURVE_LIN, G, 1, -2, 0);
        CHIRP_K[n++] = key2(t + R(0.06), CH_CURVE_EXP, G, 1, P, R(2400.0) - fi * R(80.0));
        CHIRP_K[n++] = key2(t + R(0.07), CH_CURVE_LIN, G, 0, -2, 0);
    }
    /* the finch buzz: raspy, both voices, eight mini-breaths */
    {
        ch_ckey_t k;
        k.t = 0;
        k.curve = CH_CURVE_LIN;
        k.n = 6;
        k.key[0] = CH_BIRD_VOICES; k.v[0] = R(0.5);
        k.key[1] = CH_BIRD_DRIVE; k.v[1] = R(0.95);
        k.key[2] = CH_BIRD_TENSION; k.v[2] = R(0.15);
        k.key[3] = CH_BIRD_INTERVAL; k.v[3] = R(0.5);
        k.key[4] = P; k.v[4] = R(1200.0);
        k.key[5] = CH_BIRD_PURITY; k.v[5] = R(0.1);
        BUZZ_K[0] = k;       /* (songbird.rs's first key also sets the gate 0: the gate starts there anyway) */
    }
    for (i = 0, n = 1; i < 8; i++) {
        BUZZ_K[n++] = key2(R(0.02) + (ch_real)i * R(0.07), CH_CURVE_LIN, G, 1, -2, 0);
        BUZZ_K[n++] = key2(R(0.06) + (ch_real)i * R(0.07), CH_CURVE_LIN, G, R(0.35), -2, 0);
    }
    BUZZ_K[n] = key2(R(0.6), CH_CURVE_LIN, G, 0, -2, 0);
}
#undef P
#undef G

/* ---------------------------------------------------------------------- the syrinx --- */
/* SyrinxLabia, by phase: CHIMERA's labia run freely (the airflow does not drive them), so at a pitch each plays the
 * limit cycle of its (alpha, beta); the FM-1 plays the cycles CHIMERA's equations give (ch_syrinx_waves.h, from
 * lab/tools/gen_syrinx_waves.py), blended over the grid, instead of integrating them (RK4 a sample each: ~30 % of a
 * core). Its start from rest (CHIMERA's settles onto the cycle in a few periods) is not kept */
#include "ch_syrinx_waves.h"
#define SYR_NA 13
#define SYR_NB 13
#define SYR_ALPHA0 R(0.07)                    /* CHIMERA's table: alpha 0.07 .. 0.6, beta 0.1 .. 1.3, evenly */
#define SYR_ALPHA1 R(0.6)
#define SYR_BETA0 R(0.1)
#define SYR_BETA1 R(1.3)

void ch_syrinx_init(ch_syrinx_t *s, ch_real fs, ch_real seed)
{
    s->phase = R(0.3) * seed;
    s->drive = R(0.5);
    s->tension = R(0.4);
    s->f0 = R(3000.0);
    s->gate = R(1.0);
    s->gate_sm = 0;
    s->fs = fs;
    s->opening = 0;
}

CH_HOT ch_real ch_syrinx_next(ch_syrinx_t *s)
{
    const ch_real a = R(0.09) + R(0.51) * clampr(s->drive, 0, R(1.0));        /* drive -> alpha 0.09 .. 0.6 */
    const ch_real b = R(0.2) + R(1.1) * clampr(s->tension, 0, R(1.0));        /* tension -> beta 0.2 .. 1.3 */
    const ch_real fa = clampr((a - SYR_ALPHA0) / (SYR_ALPHA1 - SYR_ALPHA0) * (ch_real)(SYR_NA - 1), 0, (ch_real)SYR_NA - R(1.0001));
    const ch_real fb = clampr((b - SYR_BETA0) / (SYR_BETA1 - SYR_BETA0) * (ch_real)(SYR_NB - 1), 0, (ch_real)SYR_NB - R(1.0001));
    const int ia = (int)fa, ib = (int)fb;
    const ch_real u = fa - (ch_real)ia, w = fb - (ch_real)ib, fp = s->phase * (ch_real)SYR_WN;
    const int i0 = (int)fp, i1 = (i0 + 1) % SYR_WN;
    const ch_real t = fp - (ch_real)i0;
#define W(B, A) (SYR_WAVE[B][A][i0] + (SYR_WAVE[B][A][i1] - SYR_WAVE[B][A][i0]) * t)
    const ch_real open = (R(1.0) - w) * ((R(1.0) - u) * W(ib, ia) + u * W(ib, ia + 1)) + w * ((R(1.0) - u) * W(ib + 1, ia) + u * W(ib + 1, ia + 1));
#undef W
    s->phase += s->f0 / s->fs;
    s->phase -= (ch_real)(int)s->phase;
    s->gate_sm += (s->gate - s->gate_sm) * R(0.002) * R(176400.0) / s->fs;   /* (CHIMERA's 0.002 a sample at its 4x) */
    s->opening = open * s->gate_sm;
    return s->opening;
}

/* ------------------------------------------------------------- the songbird's airway --- */
/* CHIMERA's songbird airway (songbird.rs create) in waveguide form at 2x (88.2 kHz): the air sac's pressure through
 * each bronchus (its port and tube lumped into an inertance) and its syringeal valve (a CHIMERA port: inertance,
 * viscous and kinetic losses, its exact quadratic), into the trachea (3.6 cm, uniform: two delay lines, its walls'
 * losses lumped), its neck port into the oropharyngeal-oesophageal cavity (a lumped Helmholtz volume), the beak (three
 * sections, their areas from the gape), its radiation load; the radiated flow's derivative decimated to 44.1 kHz by
 * CHIMERA's decimator. Its full network at 2x costs more than a core on the FM-1 (lab: ~1500 instructions a sample) */
#define SYR_AMAX R(0.03)                      /* a valve's widest (cm²) */
#define B_AT R(0.045)                         /* the trachea's area, the beak's sections' default */
#define B_KIN_V R(0.5)                        /* the valve's kinetic coefficients (PortSpec .kinetic(0.5, 1.0)) */
#define B_KEX_V R(1.0)
#define B_LEN_V R(0.08)
#define B_L_BRONCHUS (R(0.4) / R(0.05) + R(0.8) / R(0.02))   /* the sac's port and the bronchus: length / area */
#define B_NECK_A R(0.03)                      /* the cavity's neck: PortSpec(0.03, 0.25).kinetic(0.3, 0.8) */
#define B_NECK_LEN R(0.25)
#define B_CLOSED R(2e-4)

static ch_real visc_r(ch_real len, ch_real ap)  /* a port's viscous resistance (network.rs, no slit) */
{
    const ch_real visc = SQRT((RHO * MU * R(2.0) * CH_PI * R(700.0)) / R(2.0));
    return visc * R(2.0) * SQRT(CH_PI) * len * SQRT(ap) / (ap * ap);
}

/* a port's flow, CHIMERA's exact quadratic for c = the pressure across it plus its momentum's push */
CH_HOT static ch_real port_u(ch_real c, ch_real bq, ch_real kf, ch_real kb)
{
    if (c >= 0)
        return kf > 0 ? (R(2.0) * c) / (bq + SQRT(bq * bq + R(4.0) * kf * c)) : c / bq;
    if (kb > 0)
        return (R(2.0) * c) / (bq + SQRT(bq * bq - R(4.0) * kb * c));
    return c / bq;
}

static void rad_coefs2(ch_real fs, ch_real a, ch_real *b0, ch_real *b1, ch_real *a1)   /* (as ch_wave.c's) */
{
    ch_real lr, rr, zc, kk = R(2.0) * fs, d0;
    a = mx(a, R(1e-4));
    lr = (R(8.0) * RHO) / (R(3.0) * CH_PI * SQRT(CH_PI * a));
    rr = (R(128.0) * RHO * C_SOUND) / (R(9.0) * CH_PI * CH_PI * a);
    zc = RHO * C_SOUND / a;
    d0 = kk * lr * (rr + zc) + zc * rr;
    *b0 = (kk * lr * (rr - zc) - zc * rr) / d0;
    *b1 = -(kk * lr * (rr - zc) + zc * rr) / d0;
    *a1 = (zc * rr - kk * lr * (rr + zc)) / d0;
}

void ch_bird_init(ch_bird_t *b, const ch_cvoice_t *v)
{
    int i;
    const ch_real taps = (ch_real)(CH_BIRD_TAPS - 1);
    ch_real sum = 0;
    bird_calls();
    {   /* (all at rest: no library on the FM-1) */
        unsigned char *z = (unsigned char *)b;
        unsigned long k;
        for (k = 0; k < sizeof *b; k++)
            z[k] = 0;
    }
    b->fs = R(88200.0);
    b->gain = R(1.0);
    b->visc_v = SQRT((RHO * MU * R(2.0) * CH_PI * R(700.0)) / R(2.0)) * R(2.0) * SQRT(CH_PI) * B_LEN_V;
    ch_syrinx_init(&b->l, b->fs, 0);
    ch_syrinx_init(&b->r, b->fs, R(1.0));
    b->fade_t = R(0.9985);                    /* (lab: CHIMERA's levels and harmonics) */
    b->fade_k = R(0.999);
    /* CHIMERA's decimator: a Kaiser-windowed sinc, 2 x 24 + 1 taps, cutoff 0.46 / 2 of the input rate, beta 8.5 */
    for (i = 0; i < CH_BIRD_TAPS; i++) {
        const ch_real x = (ch_real)i - taps / R(2.0), r = R(2.0) * (ch_real)i / taps - R(1.0);
        const ch_real sinc = x == 0 ? R(0.46) : SIN(R(2.0) * CH_PI * R(0.23) * x) / (CH_PI * x);
        ch_real arg = R(8.5) * SQRT(mx(0, R(1.0) - r * r)), i0a = R(1.0), i0b = R(1.0), term = R(1.0), tb = R(1.0);
        int k;
        for (k = 1; k < 30; k++) {            /* I0, both */
            ch_real q = arg / (R(2.0) * (ch_real)k), qb = R(8.5) / (R(2.0) * (ch_real)k);
            term *= q * q;
            tb *= qb * qb;
            i0a += term;
            i0b += tb;
        }
        b->h[i] = sinc * i0a / i0b;
        sum += b->h[i];
    }
    for (i = 0; i < CH_BIRD_TAPS; i++)
        b->h[i] /= sum;
    b->dc_coef = EXP((R(-2.0) * CH_PI * R(12.0)) / R(44100.0));
    ch_bird_control(b, v, 1);
}

void ch_bird_control(ch_bird_t *b, const ch_cvoice_t *v, int samples)
{
    const ch_real *p = v->value, gape = p[CH_BIRD_GAPE];
    ch_real f_t, v_track, vol;
    int i;
    (void)samples;
    b->ps = p[CH_BIRD_PRESSURE] * R(1000.0) * v->gate * R(10.0);   /* kPa -> dyn/cm² */
    b->l.f0 = p[CH_BIRD_PITCH];
    b->r.f0 = p[CH_BIRD_PITCH] * POW(R(2.0), p[CH_BIRD_INTERVAL] / R(12.0));
    b->l.drive = b->r.drive = p[CH_BIRD_DRIVE];
    b->l.tension = b->r.tension = p[CH_BIRD_TENSION];
    /* voices: 0 the left alone, 0.5 both, 1 the right alone */
    b->l.gate = p[CH_BIRD_VOICES] <= R(0.5) ? R(1.0) : mx(0, R(2.0) - R(2.0) * p[CH_BIRD_VOICES]);
    b->r.gate = p[CH_BIRD_VOICES] >= R(0.5) ? R(1.0) : mx(0, R(2.0) * p[CH_BIRD_VOICES]);
    /* the cavity tracks the (lower) fundamental for pure tones, or keeps its own size */
    f_t = mn(b->l.f0, b->r.f0);
    v_track = B_NECK_A / (R(0.35) * (f_t / R(5570.0)) * (f_t / R(5570.0)));
    vol = mn(R(2.5), mx(R(0.04), p[CH_BIRD_PURITY] * v_track + (R(1.0) - p[CH_BIRD_PURITY]) * p[CH_BIRD_OEC]));
    b->oec_c = vol / RHO_C2;
    b->oec_a = vol / R(0.736806300);          /* (the cavity's area: its volume over cbrt(0.4), as CHIMERA's node) */
    /* the beak: its sections' areas, their junctions, its end's radiation */
    for (i = 0; i < CH_BIRD_NK; i++)
        b->ka[i] = mx(R(0.002), R(0.012) + gape * (R(0.05) + R(0.22) * ((ch_real)i + R(0.5)) / (ch_real)CH_BIRD_NK));
    for (i = 1; i < CH_BIRD_NK; i++)
        b->kk[i] = (b->ka[i - 1] - b->ka[i]) / (b->ka[i - 1] + b->ka[i]);
    rad_coefs2(b->fs, b->ka[CH_BIRD_NK - 1], &b->rb0, &b->rb1, &b->ra1);
    /* what the samples use, here once a step (a divide on the FM-1 costs tens of cycles) */
    {
        const ch_real T = R(1.0) / b->fs, zt = RHO * C_SOUND / B_AT, hr = R(0.5) * RHO, zk = RHO * C_SOUND / b->ka[0];
        const ch_real ip = R(1.0) / B_NECK_A, l = RHO * B_NECK_LEN * ip, ef = ip - R(1.0) / b->oec_a, eb = ip - R(1.0) / B_AT;
        b->neck_l = l;
        b->neck_bq = l / T + visc_r(B_NECK_LEN, B_NECK_A) + zt;
        b->neck_kf = R(0.3) * hr * ip * ip + R(0.8) * hr * (ef > 0 ? ef * ef : 0);
        b->neck_kb = R(0.3) * hr * ip * ip + R(0.8) * hr * (eb > 0 ? eb * eb : 0);
        b->cav_in = T / b->oec_c;
        b->cav_beak = R(2.0) / zk;
        b->cav_den = R(1.0) / (R(1.0) + T / (b->oec_c * zk));
        b->rad_k = b->ka[CH_BIRD_NK - 1] / (RHO * C_SOUND);
        b->fade_nt = POW(b->fade_t, (ch_real)CH_BIRD_NT);
    }
}

/* one sample at 88.2 kHz: the radiated flow's derivative */
CH_HOT static ch_real bird_step(ch_bird_t *b)
{
    const ch_real fs = b->fs, zt = RHO * C_SOUND / B_AT, hr = R(0.5) * RHO;
    const int it = b->ti, nt = CH_BIRD_NT;
    const ch_real l_in0 = b->tl[it] * b->fade_nt;           /* the trachea's backward wave, arriving at its start */
    const ch_real r_in = b->tr[it] * b->fade_nt;            /* its forward wave, arriving at its end */
    ch_real u_tot = 0, rout, lout, c, bq, u_n, p_c, nr[CH_BIRD_NK], nl[CH_BIRD_NK], y, in, rad;
    int j, m;
    /* the valves: each from the sac (its bronchus lumped) into the trachea's start, the other's flow last sample's */
    for (j = 0; j < 2; j++) {
        const ch_real ap = SYR_AMAX * ch_syrinx_next(j ? &b->r : &b->l);
        ch_real l, r, kf, kb, ip, u;
        if (ap < B_CLOSED) {
            b->uv[j] = b->mv[j] = 0;
            continue;
        }
        ip = R(1.0) / ap;
        l = RHO * (B_L_BRONCHUS + B_LEN_V * ip);
        r = b->visc_v * SQRT(ap) * ip * ip;   /* (visc_r: its constant part in visc_v) */
        {   /* entrance and exit: the expansion into the trachea forward, into the bronchus (0.02 cm²) back */
            const ch_real ef = ip - R(1.0) / B_AT, eb = ip - R(1.0) / R(0.02);
            kf = B_KIN_V * hr * ip * ip + B_KEX_V * hr * (ef > 0 ? ef * ef : 0);
            kb = B_KIN_V * hr * ip * ip + B_KEX_V * hr * (eb > 0 ? eb * eb : 0);
        }
        c = b->ps - R(2.0) * l_in0 - zt * b->uv[1 - j] + (b->lv[j] > l ? l * b->uv[j] : b->mv[j]) * fs;   /* (energy-safe) */
        bq = l * fs + r + zt;
        u = port_u(c, bq, kf, kb);
        b->uv[j] = u;
        b->mv[j] = l * u;
        b->lv[j] = l;
    }
    u_tot = b->uv[0] + b->uv[1];
    rout = l_in0 + zt * u_tot;                /* the trachea's start: its outgoing wave */
    /* the neck: from the trachea's end into the cavity */
    c = R(2.0) * r_in - b->pc + b->mn * fs;
    u_n = port_u(c, b->neck_bq, b->neck_kf, b->neck_kb);
    b->mn = b->neck_l * u_n;
    lout = r_in - zt * u_n;                   /* the trachea's end: its outgoing wave */
    /* the cavity, into the beak's first section (semi-implicit: its flow out at the new pressure) */
    p_c = (b->pc + b->cav_in * (u_n + b->cav_beak * b->kl[0])) * b->cav_den;
    b->pc = p_c;
    nr[0] = (p_c - b->kl[0]) * b->fade_k;
    /* the beak's junctions, its end's radiation */
    for (m = 1; m < CH_BIRD_NK; m++) {
        const ch_real in1 = b->kr[m - 1], in2 = b->kl[m], d = b->kk[m] * (in1 - in2);
        nr[m] = (in1 + d) * b->fade_k;
        nl[m - 1] = (in2 + d) * b->fade_k;
    }
    in = b->kr[CH_BIRD_NK - 1];
    y = b->rb0 * in + b->rb1 * b->r_in - b->ra1 * b->r_out;
    b->r_in = in;
    b->r_out = y;
    nl[CH_BIRD_NK - 1] = y * b->fade_k;
    rad = (in - y) * b->rad_k;
    for (m = 0; m < CH_BIRD_NK; m++) {
        b->kr[m] = nr[m];
        b->kl[m] = nl[m];
    }
    /* the trachea's delay lines */
    b->tr[it] = rout;
    b->tl[it] = lout;
    b->ti = (it + 1) % nt;
    y = (rad - b->rad_prev) * fs;
    b->rad_prev = rad;
    return y;
}

CH_HOT ch_real ch_bird_sample(ch_bird_t *b)
{
    int i;
    ch_real acc = 0, x, hp;
    for (i = 0; i < 2; i++) {                 /* two samples at 88.2 kHz into the decimator */
        const ch_real s = bird_step(b);
        b->buf[b->pos] = b->buf[b->pos + CH_BIRD_TAPS] = s;
        b->pos = b->pos + 1 == CH_BIRD_TAPS ? 0 : b->pos + 1;
    }
    for (i = 0; i < CH_BIRD_TAPS; i++)
        acc += b->h[CH_BIRD_TAPS - 1 - i] * b->buf[b->pos + i];
    /* CHIMERA's creature output: the gain, a 12 Hz DC blocker, a soft clip from 0.7 */
    x = acc * CH_BIRD_GAIN * b->gain;
    hp = x - b->dc_x + b->dc_coef * b->dc_y;
    b->dc_x = x;
    b->dc_y = hp;
    x = hp;
    if (FABS(x) > R(0.7))
        x = (x < 0 ? R(-1.0) : R(1.0)) * (R(0.7) + R(0.3) * TANH((FABS(x) - R(0.7)) / R(0.3)));
    return x;
}

/* ------------------------------------------------- the alligator and the elephant --- */
#define G CH_CK_GATE
#define MAM_NPH 12                             /* their anatomy cells: the pharynx's, the mouth's (a cell 1.2-2.7 cm; */
#define MAM_NOR 20                             /* the mouth's opening its last three, CHIMERA's last 15 %) */

/* CHIMERA's mammal_noise: aspiration at the glottis (into the pharynx's second branch; 4 kHz low-pass, 250 Hz
 * high-pass) and, with an oral gain, the mouth's narrowest point under 2 cm2 (5 kHz, 600 Hz) */
static void mammal_noise(ch_shape_t *s, ch_real fs, ch_real glottis, ch_real oral)
{
    const ch_real dph = s->ph_len / (ch_real)s->n_ph, dor = s->or_len / (ch_real)s->n_or;
    ch_noise_t *n = &s->noise[0];
    int i, mi = 0;
    ch_real mv = R(1e9);
    n->src = 0;
    n->fwd = 1;
    n->bwd = 0;
    n->gain = glottis;
    n->lp_coef = R(1.0) - EXP((R(-2.0) * CH_PI * R(4000.0)) / fs);
    n->hp_coef = EXP((R(-2.0) * CH_PI * R(250.0)) / fs);
    n->hp = n->lp1 = n->lp2 = n->level = 0;
    s->noise_s[0][0] = 0;
    s->noise_s[0][1] = R(2.0) * dph;
    s->noise_s[0][2] = -1;
    s->n_noise = 1;
    if (oral <= 0)
        return;
    for (i = 0; i < s->n_or - 1; i++)
        if (s->or_areas[i] > 0 && s->or_areas[i] < mv) {
            mv = s->or_areas[i];
            mi = i;
        }
    if (mv >= R(2.0))
        return;
    n = &s->noise[1];
    n->src = 1;
    n->fwd = 2;
    n->bwd = 3;
    n->gain = oral;
    n->lp_coef = R(1.0) - EXP((R(-2.0) * CH_PI * R(5000.0)) / fs);
    n->hp_coef = EXP((R(-2.0) * CH_PI * R(600.0)) / fs);
    n->hp = n->lp1 = n->lp2 = n->level = 0;
    /* (a branch's place: the end of the cell it leaves) */
    s->noise_s[1][0] = s->ph_len + (ch_real)(mi + 1) * dor;
    s->noise_s[1][1] = s->ph_len + (ch_real)((mi + 1 < s->n_or - 2 ? mi + 1 : s->n_or - 2) + 1) * dor;
    s->noise_s[1][2] = s->ph_len + (ch_real)((mi > 0 ? mi - 1 : 0) + 1) * dor;
    s->n_noise = 2;
}

/* a uniform pharynx, a mouth of `inside` cm2 opening to `open` cm2 over its last 15 % (fill_areas, u > 0.85) */
static void mammal_tubes(ch_shape_t *s, ch_real ph_len, ch_real ph_area, ch_real or_len, ch_real inside, ch_real open)
{
    int i;
    s->n_ph = MAM_NPH;
    s->n_or = MAM_NOR;
    s->ph_len = ph_len;
    s->or_len = or_len;
    for (i = 0; i < MAM_NPH; i++)
        s->ph_areas[i] = s->ph_min[i] = ph_area;
    for (i = 0; i < MAM_NOR; i++)
        s->or_areas[i] = s->or_min[i] = ((ch_real)i + R(0.5)) / (ch_real)MAM_NOR > R(0.85) ? open : inside;
}

/* the mammal's waveguide at fs / div: the glottal port (PortSpec(0, 0.4).kinetic(0.37, 1).slit(sqrt(trachea) * 0.9)),
 * the phonation threshold (1.5 cmH2O), the walls' radiation */
static void mammal_wave(ch_wave_t *w, ch_real fs, int ctrl, int div, ch_real tract_len, ch_real trachea_area,
                        ch_real wall_rad, ch_real gain)
{
    ch_wave_init(w, fs / (ch_real)div, ctrl / div, R(1.0), tract_len);
    w->port_len = R(0.4);
    w->port_slit = SQRT(trachea_area) * R(0.9);
    w->wall_rad = wall_rad;
    w->glottis.threshold = R(1.5) * CMH2O;
    w->output_gain = gain;
}

static const ch_cparam_t GATOR_P[CH_GATOR_NP] = {
    {"PITCH", R(12.0), R(120.0), R(28.0), R(0.05), 1, 1, "HZ"},
    {"CHAOS", R(0.0), R(1.0), R(0.75), R(0.05), 0, 0, ""},
    {"BREATH", R(3.0), R(40.0), R(18.0), R(0.03), 0, 0, "CMH2O"},
    {"MOUTH", R(0.0), R(1.0), R(0.02), R(0.05), 0, 0, ""},
    {"NOSE", R(0.0), R(1.0), R(0.8), R(0.05), 0, 0, ""},
    {"HISS", R(0.0), R(1.0), R(0.0), R(0.04), 0, 0, ""},
};
static const ch_ckey_t BELLOW[] = {
    {R(0.0), CH_CURVE_LIN, 5, {G, CH_GATOR_CHAOS, CH_GATOR_PITCH, CH_GATOR_HISS, CH_GATOR_MOUTH}, {0, R(0.55), R(30.0), 0, R(0.02)}},
    {R(0.15), CH_CURVE_LIN, 1, {G}, {1}},
    {R(1.6), CH_CURVE_LIN, 3, {G, CH_GATOR_CHAOS, CH_GATOR_PITCH}, {1, R(0.95), R(24.0)}},
    {R(2.0), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t GROWL[] = {
    {R(0.0), CH_CURVE_LIN, 4, {G, CH_GATOR_CHAOS, CH_GATOR_PITCH, CH_GATOR_MOUTH}, {1, R(0.7), R(45.0), R(0.15)}},
    {R(0.5), CH_CURVE_LIN, 1, {CH_GATOR_PITCH}, {R(38.0)}},
    {R(0.6), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t GHISS[] = {
    {R(0.0), CH_CURVE_LIN, 3, {G, CH_GATOR_HISS, CH_GATOR_MOUTH}, {0, R(1.0), R(0.9)}},
    {R(0.1), CH_CURVE_LIN, 1, {G}, {1}},
    {R(1.2), CH_CURVE_LIN, 1, {G}, {1}},
    {R(1.3), CH_CURVE_LIN, 3, {G, CH_GATOR_HISS, CH_GATOR_MOUTH}, {0, 0, R(0.02)}},
};
static const ch_ckey_t DANCE[] = {
    {R(0.0), CH_CURVE_LIN, 4, {G, CH_GATOR_CHAOS, CH_GATOR_PITCH, CH_GATOR_MOUTH}, {1, 0, R(18.0), 0}},
    {R(1.6), CH_CURVE_LIN, 1, {G}, {1}},
    {R(1.7), CH_CURVE_LIN, 2, {G, CH_GATOR_CHAOS}, {0, R(0.75)}},
};
static const ch_ccall_t GATOR_CALLS[] = {
    {"BELLOW", "RISING INTO CHAOS", R(28.0), 4, BELLOW},
    {"GROWL", "A SHORT WARNING", R(45.0), 3, GROWL},
    {"HISS", "OPEN MOUTH, AIR", R(0.0), 4, GHISS},
    {"DANCE", "A SUBSONIC THROB", R(18.0), 3, DANCE},
};
const ch_cdef_t CH_GATOR = {"ALLIGATOR", CH_GATOR_NP, 4, GATOR_P, GATOR_CALLS, R(0.012), R(0.06)};

void ch_gator_wave(ch_wave_t *w, ch_real fs, int ctrl)
{
    mammal_wave(w, fs, ctrl, CH_GATOR_DIV, R(42.0), R(6.0), R(1.0), CH_GATOR_GAIN);
    ch_wave_nose(w, 0, R(26.0), R(1.8), R(1.8));   /* the nasal tube: 26 cm of 1.8 cm2, the nostrils radiating */
}

void ch_gator_control(const ch_cvoice_t *v, ch_real fs, ch_glottis_params_t *g, ch_real *p_lung, ch_shape_t *shape)
{
    const ch_real *p = v->value, hiss = p[CH_GATOR_HISS], chaos = p[CH_GATOR_CHAOS];
    *p_lung = p[CH_GATOR_PRESSURE] * CMH2O * v->gate;
    *g = DEFAULT_GLOTTIS;
    g->f0 = p[CH_GATOR_PITCH];
    g->amplitude = R(0.9) * (R(1.0) - hiss);
    g->abduction = R(0.02) + R(0.8) * hiss;
    g->open_quotient = R(0.5);
    g->closure_sharpness = R(0.8);
    g->effort = R(1.0);
    g->chaos = chaos;
    g->jitter = R(0.01) + R(0.05) * chaos;
    g->shimmer = R(0.05) + R(0.25) * chaos;
    g->drift = R(8.0);
    mammal_tubes(shape, R(18.0), R(7.0), R(24.0), R(6.0), R(0.05) + R(12.0) * p[CH_GATOR_MOUTH]);
    shape->velum_port_area = R(0.4) + R(1.6) * p[CH_GATOR_NOSTRILS];
    mammal_noise(shape, fs, R(6e-6), R(4e-6));
}

static const ch_cparam_t ELE_P[CH_ELE_NP] = {
    {"TRUMPET", R(0.0), R(1.0), R(0.0), R(0.03), 0, 0, ""},
    {"PITCH", R(8.0), R(45.0), R(14.0), R(0.05), 1, 1, "HZ"},
    {"LIPS", R(150.0), R(900.0), R(470.0), R(0.02), 1, 0, "HZ"},
    {"BREATH", R(3.0), R(45.0), R(16.0), R(0.02), 0, 0, "CMH2O"},
    {"TRUNK", R(0.0), R(1.0), R(1.0), R(0.04), 0, 0, ""},
    {"CHAOS", R(0.0), R(1.0), R(0.2), R(0.05), 0, 0, ""},
};
static const ch_ckey_t RUMBLE[] = {
    {R(0.0), CH_CURVE_LIN, 4, {CH_ELE_MODE, G, CH_ELE_PITCH, CH_ELE_TRUNK}, {0, 0, R(15.0), R(1.0)}},
    {R(0.3), CH_CURVE_LIN, 1, {G}, {1}},
    {R(2.7), CH_CURVE_LIN, 2, {G, CH_ELE_PITCH}, {1, R(12.0)}},
    {R(3.1), CH_CURVE_LIN, 1, {G}, {0}},
};
static const ch_ckey_t TRUMPET[] = {
    {R(0.0), CH_CURVE_LIN, 5, {CH_ELE_MODE, G, CH_ELE_LIP_PITCH, CH_ELE_PRESSURE, CH_ELE_CHAOS}, {1, 0, R(380.0), R(26.0), R(0.1)}},
    {R(0.05), CH_CURVE_LIN, 1, {G}, {1}},
    {R(0.5), CH_CURVE_EXP, 1, {CH_ELE_LIP_PITCH}, {R(520.0)}},
    {R(1.1), CH_CURVE_EXP, 3, {CH_ELE_LIP_PITCH, CH_ELE_CHAOS, CH_ELE_PRESSURE}, {R(470.0), R(0.6), R(34.0)}},
    {R(1.25), CH_CURVE_LIN, 3, {G, CH_ELE_MODE, CH_ELE_PRESSURE}, {0, 0, R(16.0)}},
};
static const ch_ckey_t SQUEAK[] = {
    {R(0.0), CH_CURVE_LIN, 4, {CH_ELE_MODE, G, CH_ELE_LIP_PITCH, CH_ELE_PRESSURE}, {1, 0, R(820.0), R(20.0)}},
    {R(0.04), CH_CURVE_LIN, 1, {G}, {1}},
    {R(0.45), CH_CURVE_LIN, 1, {CH_ELE_LIP_PITCH}, {R(880.0)}},
    {R(0.5), CH_CURVE_LIN, 3, {G, CH_ELE_MODE, CH_ELE_PRESSURE}, {0, 0, R(16.0)}},
};
static const ch_ccall_t ELE_CALLS[] = {
    {"RUMBLE", "INFRASOUND, TRUNK", R(14.0), 4, RUMBLE},
    {"TRUMPET", "BRASS INTO CHAOS", R(470.0), 5, TRUMPET},
    {"SQUEAK", "TENSE LIPS, HIGH", R(850.0), 4, SQUEAK},
};
const ch_cdef_t CH_ELEPHANT = {"ELEPHANT", CH_ELE_NP, 3, ELE_P, ELE_CALLS, R(0.012), R(0.06)};

void ch_elephant_wave(ch_wave_t *w, ch_real fs, int ctrl)
{
    mammal_wave(w, fs, ctrl, CH_ELE_DIV, R(60.0), R(14.0), R(0.6), CH_ELE_GAIN);
    ch_wave_nose(w, 0, R(14.0), R(6.0), R(6.0));                  /* the trunk's base (CHIMERA's "nasal" tube) */
    w->valve_m = (int16_t)ch_wave_nose(w, 1, R(185.0), R(4.2), R(1.2));   /* the valve, then the trunk to its tip */
    w->fade_n = R(0.998);   /* the nose's loss a section (the human's 0.99 over the trunk's 117 left its trumpet 7 dB
                             * under CHIMERA's and too weak to buzz; lab/port/cre_test against its renders: the rumble,
                             * trumpet and squeak within 1-4 dB) */
}

void ch_elephant_control(const ch_cvoice_t *v, ch_real fs, ch_glottis_params_t *g, ch_real *p_lung, ch_shape_t *shape,
                         ch_wvalve_t *valve)
{
    const ch_real *p = v->value, mode = p[CH_ELE_MODE], to_trunk = p[CH_ELE_TRUNK];
    *p_lung = p[CH_ELE_PRESSURE] * CMH2O * v->gate;
    /* rumble: the folds vibrate; trumpet: the folds open, the valve buzzes */
    *g = DEFAULT_GLOTTIS;
    g->f0 = p[CH_ELE_PITCH];
    g->amplitude = R(1.6) * (R(1.0) - mode);
    g->abduction = R(0.05) + R(3.0) * mode;
    g->open_quotient = R(0.6);
    g->closure_sharpness = R(0.6);
    g->effort = R(0.9);
    g->chaos = p[CH_ELE_CHAOS] * R(0.6);
    valve->mode = mode;
    valve->f = p[CH_ELE_LIP_PITCH];
    valve->zeta = R(0.08);
    valve->h0 = R(0.003);                    /* a slight rest gap: buzzing lips start from just apart */
    valve->width = R(2.4);
    valve->surface_per_mass = R(40.0);
    mammal_tubes(shape, R(32.0), R(14.0), R(28.0), R(10.0), R(0.05) + R(10.0) * (R(1.0) - to_trunk));
    shape->velum_port_area = R(0.1) + R(6.0) * to_trunk;
    mammal_noise(shape, fs, R(4e-7) * (R(1.0) - mode), 0);
}
#undef G

/* ------------------------------------------------------------- the interpolator --- */
void ch_up_init(ch_up_t *u, int d, ch_real cut)
{
    const int len = CH_UP_TAPS * d;
    int k, j;
    u->d = d;
    for (k = 0; k < CH_UP_TAPS; k++)
        u->x[k] = 0;
    for (j = 0; j < d; j++) {
        ch_real sum = 0;
        for (k = 0; k < CH_UP_TAPS; k++) {
            const int i = j + d * k;
            const ch_real t = ((ch_real)i - R(0.5) * (ch_real)(len - 1)) / (ch_real)d;   /* (low-rate samples) */
            const ch_real a = CH_PI * cut * t;
            const ch_real wn = R(0.42) - R(0.5) * COS(R(2.0) * CH_PI * ((ch_real)i + R(0.5)) / (ch_real)len)
                               + R(0.08) * COS(R(4.0) * CH_PI * ((ch_real)i + R(0.5)) / (ch_real)len);   /* Blackman */
            u->h[j][k] = (FABS(a) < R(1e-6) ? R(1.0) : SIN(a) / a) * wn;
            sum += u->h[j][k];
        }
        for (k = 0; k < CH_UP_TAPS; k++)
            u->h[j][k] /= sum;                /* (each phase passes DC whole) */
    }
}

CH_HOT void ch_up_run(ch_up_t *u, const float *in, int n, float *out)
{
    const int d = u->d;
    int i, j, k;
    for (i = 0; i < n; i++) {
        for (k = CH_UP_TAPS - 1; k > 0; k--)
            u->x[k] = u->x[k - 1];
        u->x[0] = in[i];
        for (j = 0; j < d; j++) {
            ch_real y = 0;
            for (k = 0; k < CH_UP_TAPS; k++)
                y += u->h[j][k] * u->x[k];
            *out++ = (float)y;
        }
    }
}
