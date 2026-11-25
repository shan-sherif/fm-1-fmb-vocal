/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: replay a `lab record` file through the C port of CHIMERA's audio rate and
 * compare it with CHIMERA's own output (the file's format is in lab/src/record.rs).
 *
 *   replay <in.rec> [out.wav] [--fast=N] [--tract=in.art [--drive | --wave]]
 * prints the error against CHIMERA and the cost, writes the WAV. --fast=N runs ch_fast.c with the
 * geometry recomputed every N samples instead of the literal port. --tract runs the C anatomy
 * (ch_tract.c) on the recorded articulation and compares its targets with CHIMERA's; with --drive
 * the voice then renders from the C anatomy's targets instead of the recorded ones; with --wave the waveguide
 * (ch_wave.c, the FM-1's tract) renders instead of the FDTD, from the C anatomy and CHIMERA's glottis and breath. */
#include "ch_voice.h"
#include "ch_tract.h"
#include "ch_wave.h"
#include "ch_control.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAGIC (4.0e9 + 1729.0)
#define BLOCK (-1.0)
#define END (-2.0)

static double *rec;
static size_t rec_n, rec_i;

static double rd(void)
{
    if (rec_i >= rec_n) {
        fprintf(stderr, "replay: file ends early\n");
        exit(1);
    }
    return rec[rec_i++];
}

typedef struct {
    ch_real area[CH_MAXN], amin[CH_MAXN], dx[CH_MAXN], port[CH_MAXB];
    ch_noise_t noise[CH_MAXNOISE];
    ch_block_t b;
    uint32_t n_samples;
    float *ref;
} block_t;

static void load_header(ch_voice_t *v)
{
    int k, j, nn;
    if (rd() != MAGIC || rd() != 1.0) {
        fprintf(stderr, "replay: not a version 1 recording\n");
        exit(1);
    }
    memset(v, 0, sizeof *v);
    v->fs = (ch_real)rd();
    v->ctrl_every = (uint16_t)rd();
    v->n = (uint16_t)rd();
    v->nb = (uint16_t)rd();
    v->nr = (uint16_t)rd();
    if (v->n > CH_MAXN || v->nb > CH_MAXB || v->nr > CH_MAXR) {
        fprintf(stderr, "replay: network too large (%d nodes, %d branches)\n", v->n, v->nb);
        exit(1);
    }
    v->closed_area = (ch_real)rd();
    v->min_node_area = (ch_real)rd();
    v->viscous_scale = (ch_real)rd();
    v->viscous_omega = (ch_real)rd();
    v->noise_scale = (ch_real)rd();
    v->squeeze_area = (ch_real)rd();
    v->vol_lp_coef = (ch_real)rd();
    v->enable_volume_source = (uint8_t)rd();
    v->enable_junction_floor = (uint8_t)rd();
    v->energy_safe = (uint8_t)rd();
    v->symmetric_kinetic = (uint8_t)rd();
    v->output_gain = (ch_real)rd();
    v->dc_coef = (ch_real)rd();
    v->lim_hold_n = (int32_t)rd();
    v->lim_release = (ch_real)rd();
    v->glottis_port = (uint16_t)rd();
    v->lungs = (uint16_t)rd();
    v->sub_node = (uint16_t)rd();
    v->supra = (uint16_t)rd();
    for (k = 0; k < 7; k++)   /* glottis coefficients and drift poles: ch_glottis_init computes them */
        rd();
    for (k = 0; k < v->n; k++) {
        v->fixed[k] = (uint8_t)rd();
        v->has_wall[k] = (uint8_t)rd();
        v->wall_m[k] = (ch_real)rd();
        v->wall_r[k] = (ch_real)rd();
        v->wall_k[k] = (ch_real)rd();
        v->wall_perim[k] = (ch_real)rd();
        v->wall_rad[k] = (ch_real)rd();
        v->visc_f[k] = (ch_real)rd();
        v->rad_of[k] = (int8_t)rd();
        v->is_junction[k] = (uint8_t)rd();
        v->area[k] = (ch_real)rd();
        v->amin[k] = (ch_real)rd();
        v->dx[k] = (ch_real)rd();
        v->vol[k] = (ch_real)rd();
        v->inv_a[k] = (ch_real)rd();
        v->nbr_start[k] = (uint16_t)rd();
    }
    v->nbr_start[v->n] = (uint16_t)rd();
    nn = v->nbr_start[v->n];
    for (k = 0; k < nn; k++)
        v->nbr_list[k] = (uint16_t)rd();
    for (j = 0; j < v->nb; j++) {
        v->b_a[j] = (uint16_t)rd();
        v->b_b[j] = (uint16_t)rd();
        v->b_kind[j] = (uint8_t)rd();
        v->port_area[j] = (ch_real)rd();
        v->port_len[j] = (ch_real)rd();
        v->port_kin[j] = (ch_real)rd();
        v->port_kex[j] = (ch_real)rd();
        v->port_slit[j] = (ch_real)rd();
        v->port_half[j] = (uint8_t)rd();
        v->port_extra_r[j] = (ch_real)rd();
    }
}

static int load_blocks(const ch_voice_t *v, block_t **out)
{
    int cap = 1024, nb = 0, k;
    block_t *bl = malloc(sizeof(block_t) * cap);
    for (;;) {
        double tag = rd();
        block_t *b;
        ch_real *g;
        if (tag == END)
            break;
        if (tag != BLOCK) {
            fprintf(stderr, "replay: bad block tag %g\n", tag);
            exit(1);
        }
        if (nb == cap) {
            cap *= 2;
            bl = realloc(bl, sizeof(block_t) * cap);
        }
        b = &bl[nb++];
        for (k = 0; k < v->n; k++)
            b->area[k] = (ch_real)rd();
        for (k = 0; k < v->n; k++)
            b->amin[k] = (ch_real)rd();
        for (k = 0; k < v->n; k++)
            b->dx[k] = (ch_real)rd();
        for (k = 0; k < v->nb; k++)
            b->port[k] = (ch_real)rd();
        b->b.n_noise = (uint8_t)rd();
        for (k = 0; k < b->b.n_noise; k++) {
            ch_noise_t *s = &b->noise[k];
            memset(s, 0, sizeof *s);
            s->src = (uint16_t)rd();
            s->fwd = (uint16_t)rd();
            s->bwd = (uint16_t)rd();
            s->gain = (ch_real)rd();
            s->lp_coef = (ch_real)rd();
            s->hp_coef = (ch_real)rd();
        }
        g = &b->b.glottis.f0;
        for (k = 0; k < 17; k++)
            g[k] = (ch_real)rd();
        b->b.p_lung = (ch_real)rd();
        b->n_samples = (uint32_t)rd();
        b->ref = malloc(sizeof(float) * b->n_samples);
        for (k = 0; k < (int)b->n_samples; k++)
            b->ref[k] = (float)rd();
    }
    for (k = 0; k < nb; k++) {   /* pointers last: realloc moves the blocks */
        bl[k].b.area_target = bl[k].area;
        bl[k].b.amin_target = bl[k].amin;
        bl[k].b.dx_target = bl[k].dx;
        bl[k].b.port_area_target = bl[k].port;
        bl[k].b.noise = bl[k].noise;
    }
    *out = bl;
    return nb;
}

static void write_wav(const char *path, const float *x, size_t n, uint32_t fs)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)n * 2, u32;
    uint16_t u16;
    size_t i;
    if (!f) {
        perror(path);
        return;
    }
    fwrite("RIFF", 1, 4, f);
    u32 = 36 + data; fwrite(&u32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u32 = 16; fwrite(&u32, 4, 1, f);
    u16 = 1; fwrite(&u16, 2, 1, f);
    u16 = 1; fwrite(&u16, 2, 1, f);
    fwrite(&fs, 4, 1, f);
    u32 = fs * 2; fwrite(&u32, 4, 1, f);
    u16 = 2; fwrite(&u16, 2, 1, f);
    u16 = 16; fwrite(&u16, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    for (i = 0; i < n; i++) {
        float s = isfinite(x[i]) ? (x[i] < -1 ? -1 : x[i] > 1 ? 1 : x[i]) : 0;
        int16_t q = (int16_t)lrintf(s * 32767.0f);
        fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

static ch_voice_t topo, voice;
static ch_tract_t tract;
long ch_wave_dbg_from = -1, ch_wave_dbg_pos;

/* the articulation file (lab/src/record.rs art_header) */
static double *art;
static size_t art_n, art_i;
static double ra(void)
{
    if (art_i >= art_n) {
        fprintf(stderr, "replay: articulation file ends early\n");
        exit(1);
    }
    return art[art_i++];
}

static void load_art(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    double sc, ph, tr, lw;
    int i;
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    art = malloc(sz);
    art_n = fread(art, 8, sz / 8, f);
    fclose(f);
    if (ra() != 4.0e9 + 1730.0 || ra() != 1.0) {
        fprintf(stderr, "replay: not a version 1 articulation file\n");
        exit(1);
    }
    memset(&tract, 0, sizeof tract);
    tract.fs = (ch_real)ra();
    sc = ra();
    ph = ra();
    tr = ra();
    lw = ra();
    tract.n_ph = (int)ra();
    tract.n_or = (int)ra();
    tract.dx_stable = (ch_real)ra();
    ra();   /* trachea's first node */
    tract.tr_branch = (uint16_t)ra();
    tract.tr_n = (uint16_t)ra();
    tract.ph_first = (uint16_t)ra();
    tract.ph_branch = (uint16_t)ra();
    ra();
    tract.or_first = (uint16_t)ra();
    tract.or_branch = (uint16_t)ra();
    ra();
    tract.glottis_port = (uint16_t)ra();
    tract.velum_port = (uint16_t)ra();
    tract.gain_glottis = (ch_real)ra();
    tract.gain_constriction = (ch_real)ra();
    tract.gain_teeth = (ch_real)ra();
    tract.max_area_rate = (ch_real)ra();
    tract.area_rate_gain = (ch_real)ra();
    tract.seal_rate = (ch_real)ra();
    tract.seal_lo = (ch_real)ra();
    tract.seal_hi = (ch_real)ra();
    tract.seal_ease = (ch_real)ra();
    for (i = 0; i < 8; i++)
        tract.law[i] = (ch_real)ra();
    ch_tract_init(&tract, (ch_real)sc, (ch_real)ph, (ch_real)tr, (ch_real)lw);
}

static void next_artic(ch_real *a)
{
    int i;
    for (i = 0; i < CH_NARTIC; i++)
        a[i] = (ch_real)ra();
}

/* the gestures file (lab/src/record.rs gestures_file) into a fresh control */
static void load_gst(const char *path, ch_control_t *c, ch_real fs, int ctrl_every)
{
    FILE *f = fopen(path, "rb");
    static double *d;
    long sz;
    size_t n, j = 0;
    int ng, i, k;
    ch_source_t s;
    double bs, mp, f0;
#define GD() (j < n ? d[j++] : (fprintf(stderr, "replay: gestures file ends early\n"), exit(1), 0.0))
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(d);
    d = malloc(sz);
    n = fread(d, 8, sz / 8, f);
    fclose(f);
    if (GD() != 4.0e9 + 1731.0 || GD() != 1.0) {
        fprintf(stderr, "replay: not a version 1 gestures file\n");
        exit(1);
    }
    ng = (int)GD();
    s.f0 = GD(); s.glide = GD(); s.pressure = GD(); s.breathiness = GD(); s.tension = GD(); s.fry = GD();
    s.roughness = GD(); s.vibrato_depth = GD(); s.vibrato_rate = GD(); s.drift = GD();
    s.voiced = GD() != 0;
    s.brightness = GD();
    bs = GD();
    mp = GD();
    f0 = GD();
    ch_control_init(c, fs, ctrl_every, (ch_real)bs);
    ch_control_set_source(c, &s);
    c->max_penetration = (ch_real)mp;
    c->f0 = (ch_real)f0;
    for (i = 0; i < ng; i++) {
        ch_gesture_t g;
        g.t0 = GD(); g.t1 = GD(); g.ramp_in = GD(); g.ramp_out = GD(); g.strength = GD();
        g.speed = GD(); g.release_hold = GD(); g.tag = GD();
        for (k = 0; k < CH_NCONTROL; k++)
            g.targets[k] = (ch_real)GD();
        ch_score_add(&c->score, &g);
    }
#undef GD
}


/* tuning (lab only): CH_WAVE_RG, _RLIP, _RNOSE, _FADE, _FADEN, _NOISE override ch_wave_init's; _RADCONST, _NOWALLS,
 * _VARLEN switch */
static void wave_env(ch_wave_t *wv)
{
    if (getenv("CH_WAVE_RG")) wv->r_glottis = (ch_real)atof(getenv("CH_WAVE_RG"));
    if (getenv("CH_WAVE_RLIP")) wv->r_lip = (ch_real)atof(getenv("CH_WAVE_RLIP"));
    if (getenv("CH_WAVE_RNOSE")) wv->r_nose = (ch_real)atof(getenv("CH_WAVE_RNOSE"));
    if (getenv("CH_WAVE_FADE")) wv->fade = (ch_real)atof(getenv("CH_WAVE_FADE"));
    if (getenv("CH_WAVE_FADEN")) wv->fade_n = (ch_real)atof(getenv("CH_WAVE_FADEN"));
    if (getenv("CH_WAVE_NOISE")) wv->noise_gain = (ch_real)atof(getenv("CH_WAVE_NOISE"));
    if (getenv("CH_WAVE_RADCONST")) wv->rad_load = 0;
    if (getenv("CH_WAVE_NOWALLS")) wv->walls = 0;
    if (getenv("CH_WAVE_VARLEN")) wv->var_len = 1;
    if (getenv("CH_WAVE_NARROW")) wv->narrowest = (uint8_t)atoi(getenv("CH_WAVE_NARROW"));
    if (getenv("CH_WAVE_VNARROW")) wv->vol_narrow = (ch_real)atof(getenv("CH_WAVE_VNARROW"));
    if (getenv("CH_WAVE_VSMOOTH")) wv->vol_smooth = (ch_real)atof(getenv("CH_WAVE_VSMOOTH"));
    if (getenv("CH_WAVE_VOL")) wv->vol_src = (uint8_t)atoi(getenv("CH_WAVE_VOL"));
}

int main(int argc, char **argv)
{
    FILE *f;
    long sz;
    block_t *bl;
    int nblk, i, runs = 5, r;
    size_t total = 0, pos;
    float *out;
    double err2 = 0, ref2 = 0, maxerr = 0, best = 1e30;
    size_t first_div = (size_t)-1;
    const char *wav = 0, *art_path = 0, *gst_path = 0;
    int fast = 0, drive = 0, anat_every = 1, wave = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: replay <in.rec> [out.wav] [--fast=N]\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--fast=", 7))
            fast = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "--tract=", 8))
            art_path = argv[i] + 8;
        else if (!strcmp(argv[i], "--drive"))
            drive = 1;
        else if (!strcmp(argv[i], "--wave"))
            wave = 1;
        else if (!strncmp(argv[i], "--control=", 10))
            gst_path = argv[i] + 10;
        else if (!strncmp(argv[i], "--anat-every=", 13))
            anat_every = atoi(argv[i] + 13) < 1 ? 1 : atoi(argv[i] + 13);
        else
            wav = argv[i];
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    rec = malloc(sz);
    rec_n = fread(rec, 8, sz / 8, f);
    fclose(f);

    load_header(&topo);
    if (getenv("CH_NOVOL"))   /* diagnostics: no moving-wall volume source */
        topo.enable_volume_source = 0;
    nblk = load_blocks(&topo, &bl);
    for (i = 0; i < nblk; i++)
        total += bl[i].n_samples;
    if (art_path) {
        /* the C anatomy on the recorded articulation, block by block, against CHIMERA's targets */
        ch_real a[CH_NARTIC], area_t[CH_MAXN], amin_t[CH_MAXN], dx_t[CH_MAXN], port_t[CH_MAXB];
        double e_area = 0, e_amin = 0, e_dx = 0, e_port = 0, e_gain = 0, worst_rel = 0;
        int noise_mismatch = 0, worst_blk = -1, k, runs_t, r2;
        size_t art_start;
        struct timespec t0, t1;
        double best_t = 1e30;
        load_art(art_path);
        art_start = art_i;
        for (runs_t = 0; runs_t < 3; runs_t++) {   /* timing: the anatomy alone, the fastest of three */
            art_i = art_start;
            tract.have_base = 0;
            next_artic(a);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            ch_tract_apply(&tract, a, 0);
            for (i = 0; i < nblk; i++) {
                ch_real dt;
                next_artic(a);
                dt = (ch_real)ra();
                if (i % anat_every == 0)
                    ch_tract_apply(&tract, a, dt * (ch_real)anat_every);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            {
                double secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
                if (secs < best_t)
                    best_t = secs;
            }
        }
        art_i = art_start;
        tract.have_base = 0;
        next_artic(a);
        ch_tract_apply(&tract, a, 0);
        {
            /* the targets the anatomy last computed, and the ones before (--anat-every: the voice
             * glides from those to these over the next anat_every control steps) */
            static ch_real new_t[4][CH_MAXB], old_t[4][CH_MAXB];
            memcpy(new_t[0], bl[0].area, sizeof(ch_real) * topo.n);
            memcpy(new_t[1], bl[0].amin, sizeof(ch_real) * topo.n);
            memcpy(new_t[2], bl[0].dx, sizeof(ch_real) * topo.n);
            memcpy(new_t[3], bl[0].port, sizeof(ch_real) * topo.nb);
            ch_tract_targets(&tract, new_t[0], new_t[1], new_t[2], new_t[3]);
        for (i = 0; i < nblk; i++) {
            block_t *b = &bl[i];
            ch_real dt, f;
            int q;
            next_artic(a);
            dt = (ch_real)ra();
            if (i % anat_every == 0) {
                memcpy(old_t, new_t, sizeof old_t);
                ch_tract_apply(&tract, a, dt * (ch_real)anat_every);
                ch_tract_targets(&tract, new_t[0], new_t[1], new_t[2], new_t[3]);
            }
            f = (ch_real)(i % anat_every + 1) / (ch_real)anat_every;
            for (q = 0; q < topo.n; q++) {
                area_t[q] = old_t[0][q] + (new_t[0][q] - old_t[0][q]) * f;
                amin_t[q] = old_t[1][q] + (new_t[1][q] - old_t[1][q]) * f;
                dx_t[q] = old_t[2][q] + (new_t[2][q] - old_t[2][q]) * f;
            }
            for (q = 0; q < topo.nb; q++)   /* the other ports are not the anatomy's (the glottis is per sample) */
                port_t[q] = b->port[q];
            q = tract.velum_port;
            port_t[q] = old_t[3][q] + (new_t[3][q] - old_t[3][q]) * f;
            for (k = 0; k < topo.n; k++) {
                double ea = fabs((double)area_t[k] - b->area[k]), em = fabs((double)amin_t[k] - b->amin[k]);
                double rel = ea / (fabs((double)b->area[k]) + 0.01);
                if (ea > e_area)
                    e_area = ea;
                if (em > e_amin)
                    e_amin = em;
                if (fabs((double)dx_t[k] - b->dx[k]) > e_dx)
                    e_dx = fabs((double)dx_t[k] - b->dx[k]);
                if (rel > worst_rel) {
                    worst_rel = rel;
                    worst_blk = i;
                }
            }
            for (k = 0; k < topo.nb; k++)
                if (fabs((double)port_t[k] - b->port[k]) > e_port)
                    e_port = fabs((double)port_t[k] - b->port[k]);
            if (tract.n_noise != b->b.n_noise) {
                noise_mismatch++;
            } else {
                for (k = 0; k < tract.n_noise; k++) {
                    const ch_noise_t *x = &tract.noise[k], *y = &b->noise[k];
                    if (x->src != y->src || x->fwd != y->fwd || x->bwd != y->bwd)
                        noise_mismatch++;
                    else if (fabs((double)x->gain - y->gain) > e_gain)
                        e_gain = fabs((double)x->gain - y->gain);
                }
            }
            if (drive) {   /* render from the C anatomy's targets */
                memcpy(b->area, area_t, sizeof(ch_real) * topo.n);
                memcpy(b->amin, amin_t, sizeof(ch_real) * topo.n);
                memcpy(b->dx, dx_t, sizeof(ch_real) * topo.n);
                memcpy(b->port, port_t, sizeof(ch_real) * topo.nb);
                memcpy(b->noise, tract.noise, sizeof(ch_noise_t) * tract.n_noise);
                b->b.n_noise = (uint8_t)tract.n_noise;
            }
        }
        }
        printf("%s: C anatomy vs CHIMERA over %d control steps (anatomy every %d):\n", art_path, nblk, anat_every);
        printf("  max error: area %.2e cm2, minimum %.2e cm2, section length %.2e cm, velum port %.2e cm2\n", e_area, e_amin, e_dx, e_port);
        printf("  worst relative area error %.2e (block %d); noise lists differing in %d blocks, max gain error %.2e\n", worst_rel, worst_blk,
               noise_mismatch, e_gain);
        printf("  cost: %.1f us a control step on this core (%.2f%% of it at %.0f steps/s)\n", 1e6 * best_t / (nblk + 1),
               100.0 * best_t / ((double)total / topo.fs), (double)nblk / ((double)total / topo.fs));
        (void)r2;
#ifdef CH_COUNT
        {
            extern unsigned long ch_count[8];
            double st = (double)ch_count[6];
            printf("  per control step: %.0f ray-segment tests, %.0f box tests, %.0f roof projections, %.0f tongue-distance segments, %.0f area lookups\n",
                   ch_count[0] / st, ch_count[4] / st, ch_count[1] / st, ch_count[2] / st, ch_count[3] / st);
        }
#endif
    }
    out = malloc(sizeof(float) * total);

    if (gst_path) {
        /* the C control from CHIMERA's gestures: each step's articulation against the .art, the glottis's parameters
         * and the lungs' pressure against the .rec; then the waveguide renders from it (the whole pipeline in C) */
        static ch_control_t ctl;
        static ch_wave_t wv;
        static ch_shape_t shape;
        static const char *gname[17] = {"f0", "amplitude", "abduction", "chink", "open_quotient", "skew", "closure_sharpness", "effort",
                                        "jitter", "shimmer", "vibrato_depth", "vibrato_rate", "drift", "fry", "diplophonia", "closure", "chaos"};
        double e_x[CH_NARTIC] = {0}, e_g[17] = {0}, e_p = 0, e_x0 = 0, ctl_t = 1e30;
        int worst_x = -1, worst_g = -1, k, run, anat;
        ch_real a[CH_NARTIC];
        size_t art0;
        art_i = 0;
        if (!art_path) { fprintf(stderr, "replay: --control needs --tract=in.art\n"); return 2; }
        load_art(art_path);
        art0 = art_i;
        for (run = 0; run < 3; run++) {
            struct timespec t0, t1, c0, c1;
            double ct = 0;
            art_i = art0;
            load_gst(gst_path, &ctl, topo.fs, topo.ctrl_every);
            ctl.anat_every = anat_every;
            ctl.defer_contacts = getenv("CH_DEFER") != 0;
            next_artic(a);
            for (k = 0; k < CH_NARTIC; k++)
                if (fabs((double)ctl.x[k] - a[k]) > e_x0)
                    e_x0 = fabs((double)ctl.x[k] - a[k]);
            tract.have_base = 0;
            ch_tract_apply(&tract, ctl.x, 0);
            ch_wave_init(&wv, topo.fs, topo.ctrl_every, tract.k, tract.lip_s);
            wave_env(&wv);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0, pos = 0; i < nblk; i++) {
                const ch_real *g = &bl[i].b.glottis.f0, *cg = &ctl.glottis.f0;
                clock_gettime(CLOCK_MONOTONIC, &c0);
                anat = ch_control_step(&ctl, &tract);
                if (anat)
                    ch_tract_shape(&tract, &shape);
                clock_gettime(CLOCK_MONOTONIC, &c1);
                {
                    double st = (double)(c1.tv_sec - c0.tv_sec) + 1e-9 * (double)(c1.tv_nsec - c0.tv_nsec);
                    ct += st;
                    if (getenv("CH_STEP_TIMES") && run == 2)   /* each step: time (us), anatomy ran, reapplies so far */
                        fprintf(stderr, "%d %.2f %d %u\n", i, 1e6 * st, anat, ctl.reapplies);
                }
                if (run == 0) {
                    next_artic(a);
                    ra();   /* dt */
                    for (k = 0; k < CH_NARTIC; k++)
                        if (fabs((double)ctl.x[k] - a[k]) > e_x[k]) {
                            e_x[k] = fabs((double)ctl.x[k] - a[k]);
                            if (worst_x < 0 || e_x[k] >= e_x[worst_x])
                                worst_x = k;
                        }
                    for (k = 0; k < 17; k++) {
                        double e = fabs((double)cg[k] - g[k]) / (fabs((double)g[k]) + 1e-3);
                        if (e > e_g[k]) {
                            e_g[k] = e;
                            if (worst_g < 0 || e_g[k] >= e_g[worst_g])
                                worst_g = k;
                        }
                    }
                    if (fabs((double)ctl.p_lung - bl[i].b.p_lung) > e_p)
                        e_p = fabs((double)ctl.p_lung - bl[i].b.p_lung);
                }
                ch_wave_block(&wv, anat ? &shape : 0, anat_every, &ctl.glottis, ctl.p_lung);
                ch_wave_render(&wv, out + pos, bl[i].n_samples);
                if (getenv("CH_PSUPRA") && run == 0) {   /* "from,to" (s): the pressure above the glottis, the voicing */
                    double tf = atof(getenv("CH_PSUPRA")), tt = atof(strchr(getenv("CH_PSUPRA"), ',') + 1), tn = (double)pos / topo.fs;
                    if (tn >= tf && tn <= tt)
                    {
                        int q, qm = 0;
                        for (q = 1; q < wv.n; q++)
                            if (wv.sec[q].area < wv.sec[qm].area)
                                qm = q;
                        printf("  %.3f s  p_lung %6.0f  p_supra %6.0f  voicing %.2f  glottis %.4f cm2  abduction %.3f  shut %d  narrowest %d/%d %.4f  jets %d  velum %.3f\n", tn,
                               (double)wv.p_lung_now, (double)wv.p_supra, (double)wv.glottis.voicing, (double)wv.a_g,
                               (double)ctl.glottis.abduction, wv.n_shut, qm, wv.n, (double)wv.sec[qm].area, wv.n_jet, (double)tract.velum_port_area);
                    }
                }
                pos += bl[i].n_samples;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (ct < ctl_t)
                ctl_t = ct;
            (void)t0;
            (void)t1;
        }
        printf("%s: C control vs CHIMERA over %d control steps (%d gestures left at the end)%s\n", gst_path, nblk, ctl.score.n,
               ctl.score.dropped ? " (some dropped: CH_SCORE_CAP)" : "");
        printf("  start articulation error %.2e\n  max articulator error %.2e (%d); by articulator:", e_x0,
               worst_x < 0 ? 0.0 : e_x[worst_x], worst_x);
        for (k = 0; k < CH_NARTIC; k++)
            printf(" %.1e", e_x[k]);
        printf("\n  max relative glottis error %.2e (%s); p_lung max error %.2e dyn/cm2\n", worst_g < 0 ? 0.0 : e_g[worst_g],
               worst_g < 0 ? "-" : gname[worst_g], e_p);
        for (k = 0; k < 17; k++)
            if (e_g[k] > 1e-9)
                printf("    %s %.2e\n", gname[k], e_g[k]);
        printf("  cost: control + anatomy %.1f us a control step on this core (%.2f%% of it)\n", 1e6 * ctl_t / nblk,
               100.0 * ctl_t / ((double)total / topo.fs));
        if (wav)
            write_wav(wav, out, total, (uint32_t)topo.fs);
        return 0;
    }

    if (wave) {
        /* the waveguide from the C anatomy, the recording's glottis and breath; timed the same way */
        static ch_wave_t wv;
        static ch_shape_t shape;
        ch_real a[CH_NARTIC];
        size_t art0;
        if (!art_path) {
            fprintf(stderr, "replay: --wave needs --tract=in.art\n");
            return 2;
        }
        if (getenv("CH_WAVE_DUMP"))
            ch_wave_dbg_from = atol(getenv("CH_WAVE_DUMP"));
        art_i = 0;
        load_art(art_path);
        art0 = art_i;
        for (r = 0; r < runs; r++) {
            struct timespec t0, t1;
            double secs, tract_t = 0;
            art_i = art0;
            ch_wave_dbg_pos = r == 0 ? 0 : -1000000000L;
            tract.have_base = 0;
            next_artic(a);
            ch_tract_apply(&tract, a, 0);
            ch_wave_init(&wv, topo.fs, topo.ctrl_every, tract.k, tract.lip_s);
            /* tuning (lab only): CH_WAVE_RG, _RLIP, _RNOSE, _FADE, _FADEN override ch_wave_init's; CH_WAVE_NONOISE=1 */
            if (getenv("CH_WAVE_RG")) wv.r_glottis = (ch_real)atof(getenv("CH_WAVE_RG"));
            if (getenv("CH_WAVE_RLIP")) wv.r_lip = (ch_real)atof(getenv("CH_WAVE_RLIP"));
            if (getenv("CH_WAVE_RNOSE")) wv.r_nose = (ch_real)atof(getenv("CH_WAVE_RNOSE"));
            if (getenv("CH_WAVE_FADE")) wv.fade = (ch_real)atof(getenv("CH_WAVE_FADE"));
            if (getenv("CH_WAVE_FADEN")) wv.fade_n = (ch_real)atof(getenv("CH_WAVE_FADEN"));
            if (getenv("CH_WAVE_NOISE")) wv.noise_gain = (ch_real)atof(getenv("CH_WAVE_NOISE"));
            if (getenv("CH_WAVE_RADCONST")) wv.rad_load = 0;
            if (getenv("CH_WAVE_NOWALLS")) wv.walls = 0;
            if (getenv("CH_WAVE_VARLEN")) wv.var_len = 1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0, pos = 0; i < nblk; i++) {
                struct timespec a0, a1;
                ch_real dt;
                next_artic(a);
                dt = (ch_real)ra();
                clock_gettime(CLOCK_MONOTONIC, &a0);
                if (i % anat_every == 0) {   /* --anat-every: the shape glided to over that many steps */
                    ch_tract_apply(&tract, a, dt * (ch_real)anat_every);
                    ch_tract_shape(&tract, &shape);
                }
                clock_gettime(CLOCK_MONOTONIC, &a1);
                tract_t += (double)(a1.tv_sec - a0.tv_sec) + 1e-9 * (double)(a1.tv_nsec - a0.tv_nsec);
                ch_wave_block(&wv, i % anat_every == 0 ? &shape : 0, anat_every, &bl[i].b.glottis, bl[i].b.p_lung);
                if (getenv("CH_WAVE_NONOISE"))
                    wv.n_noise = 0;
                if (getenv("CH_WAVE_NOJETS"))
                    wv.n_jet = 0;
                if (getenv("CH_WAVE_AT") && r == 0) {   /* the tract at one moment (s): its sections' areas */
                    (void)0;
                    if (0) {
                        int q;
                        printf("  t %.3f s: p_lung %.0f, velum port %.3f, noise sources %d; areas (cm2) glottis -> lips:\n   ",
                               (double)pos / topo.fs, (double)bl[i].b.p_lung, (double)tract.velum_port_area, wv.n_noise);
                        for (q = 0; q < wv.n; q++)
                            printf(" %.2f", (double)(tract.ph_len + tract.or_len) > 0 ? (double)wv.sec[q].area + (double)wv.sec[q].darea * topo.ctrl_every : 0.0);
                        printf("\n");
                    }
                }
                ch_wave_render(&wv, out + pos, bl[i].n_samples);
                pos += bl[i].n_samples;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec) - tract_t;
            if (secs < best)
                best = secs;
        }
        printf("%s: waveguide, %d tract sections, %d nose sections, %zu samples (%.2f s)\n", argv[1], wv.n, wv.nn, total,
               (double)total / topo.fs);
        printf("  cost (the waveguide alone): %.2f%% of this core (%.1f ns a sample)\n", 100 * best / ((double)total / topo.fs),
               1e9 * best / (double)total);
        if (wav)
            write_wav(wav, out, total, (uint32_t)topo.fs);
        return 0;
    }
    for (r = 0; r < runs; r++) {   /* the same render several times: the fastest is the cost */
        struct timespec t0, t1;
        double secs;
        voice = topo;
        if (fast)
            ch_fast_init_state(&voice, fast);
        else
            ch_init_state(&voice);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0, pos = 0; i < nblk; i++) {
            if (fast) {
                ch_fast_block(&voice, &bl[i].b);
                ch_fast_render(&voice, out + pos, bl[i].n_samples);
            } else {
                ch_block(&voice, &bl[i].b);
                ch_render(&voice, out + pos, bl[i].n_samples);
            }
            pos += bl[i].n_samples;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
        if (secs < best)
            best = secs;
    }

    for (i = 0, pos = 0; i < nblk; i++) {
        uint32_t s;
        for (s = 0; s < bl[i].n_samples; s++, pos++) {
            double e = (double)out[pos] - (double)bl[i].ref[s];
            err2 += e * e;
            ref2 += (double)bl[i].ref[s] * bl[i].ref[s];
            if (fabs(e) > maxerr)
                maxerr = fabs(e);
            if (fabs(e) > 1e-4 && first_div == (size_t)-1)
                first_div = pos;
        }
    }
    printf("%s: %d nodes, %d branches, %zu samples (%.2f s), %s, ", argv[1], topo.n, topo.nb, total,
           (double)total / topo.fs, sizeof(ch_real) == 4 ? "float" : "double");
    if (fast)
        printf("fast, geometry every %d samples\n", fast);
    else
        printf("literal\n");
    printf("  vs CHIMERA: SNR %.1f dB, max error %.2e", 10 * log10(ref2 / (err2 + 1e-30)), maxerr);
    if (first_div != (size_t)-1)
        printf(", first > 1e-4 at %.3f s", (double)first_div / topo.fs);
    printf("\n  cost: %.2f%% of this core in real time (%.1f ns a sample)\n", 100 * best / ((double)total / topo.fs),
           1e9 * best / (double)total);
    if (wav)
        write_wav(wav, out, total, (uint32_t)topo.fs);
    return 0;
}
