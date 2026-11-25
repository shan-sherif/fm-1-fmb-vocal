/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the FM-1's alligator and elephant (engine/ch_creature.c) rendered as CHIMERA's `chimera render
 * creature alligator|elephant` renders its own, for comparing them: a sustained note at the default pitch (1.2 s, then
 * 0.4 s of release), then each call at its own pitch (its last key + 0.5 s). As the app runs them: the waveguide at a
 * fraction of 44.1 kHz, interpolated up, a control step every 64 output samples.
 *
 *   ../out/cre_test alligator|elephant OUT.wav   (lab switches: GAIN=x the output, FADE_N=the nose's loss a section,
 *                                                 PROBE=1 the valve's opening and flow every 50 ms) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ch_creature.h"
#include "ch_wave.h"

#define FS 44100
#define CTRL 64

static ch_cvoice_t v;
static ch_wave_t wave;
static ch_shape_t shape;
static ch_up_t up;
static float out[FS * 16];
static size_t n_out;
static int ele;

static void write_wav(const char *path, const float *x, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)n * 2, u32, fs = FS;
    uint16_t u16;
    size_t i;
    if (!f) {
        perror(path);
        exit(1);
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
        float s = x[i] * 32767.0f;
        int16_t q = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

static void render(double sec)
{
    const int div = ele ? CH_ELE_DIV : CH_GATOR_DIV;
    size_t n = (size_t)(sec * FS) / CTRL * CTRL, i;
    float low[CTRL];
    for (i = 0; i < n && n_out + CTRL <= sizeof out / sizeof out[0]; i += CTRL) {
        ch_glottis_params_t g;
        ch_real p_lung;
        ch_cvoice_step(&v, (ch_real)CTRL / FS);
        if (ele) {
            ch_wvalve_t vl;
            ch_elephant_control(&v, (ch_real)FS / div, &g, &p_lung, &shape, &vl);
            ch_wave_valve(&wave, &vl);
        } else {
            ch_gator_control(&v, (ch_real)FS / div, &g, &p_lung, &shape);
        {   /* (lab) NG0 / NG1: each noise source's gain scaled (the glottis's, the mouth's) */
            int q;
            for (q = 0; q < shape.n_noise; q++) {
                const char *e = getenv(q ? "NG1" : "NG0");
                if (e)
                    shape.noise[q].gain *= (ch_real)atof(e);
            }
        }
        }
        ch_wave_block(&wave, &shape, 1, &g, p_lung);
        ch_wave_render(&wave, low, CTRL / div);
        ch_up_run(&up, low, CTRL / div, out + n_out);
        if (getenv("PROBE") && (n_out / CTRL) % 34 == 0)
            printf("  %.2f s gate %.2f p_lung %5.1f cmH2O a_g %.3f u_g %7.1f | valve open %.3f u %7.1f dp %8.0f h %.3f\n",
                   (double)n_out / FS, (double)v.gate, (double)p_lung / 980.665, (double)wave.a_g, (double)wave.u_g,
                   (double)wave.vlv_open, (double)wave.vlv_u, (double)wave.vlv_dp, (double)wave.vlv_h);
        if (getenv("PROBE") && (n_out / CTRL) % 34 == 0 && wave.valve_m > 0)
            printf("      in1 %9.0f in2 %9.0f a1 %.2f a2 %.2f mode %.4f mom %.3f l %.4f\n",
                   (double)wave.nsec[wave.valve_m - 1].R[wave.p], (double)wave.nsec[wave.valve_m].L[wave.p],
                   (double)wave.nsec[wave.valve_m - 1].area, (double)wave.nsec[wave.valve_m].area, (double)wave.vlv.mode,
                   (double)wave.vlv_mom, (double)wave.vlv_l);
        if (getenv("NPROBE") && (n_out / CTRL) % 34 == 0) {   /* (lab) the noise sources: where, how loud */
            int q;
            printf("  %.2f s:", (double)n_out / FS);
            for (q = 0; q < wave.n_noise; q++) {
                const int sx = wave.n_src[q];
                const ch_real a = sx < 0 ? wave.a_g : (wave.sec[sx - 1].area < wave.sec[sx].area ? wave.sec[sx - 1].area : wave.sec[sx].area);
                const ch_real u = sx < 0 ? wave.u_g : (wave.sec[sx - 1].R[wave.p] - wave.sec[sx - 1].L[wave.p ^ 1]) * wave.sec[sx - 1].area / wave.rho_c;
                printf("  [src %d fwd %d a %.3f u %7.1f level %.3g]", sx, wave.n_fwd[q], (double)a, (double)u, (double)wave.noise[q].level);
            }
            printf("  lips sec %d a %.3f, nose port %.2f\n", wave.n - 1, (double)wave.sec[wave.n - 1].area, (double)wave.nsec[0].area);
        }
        n_out += CTRL;
    }
}

int main(int argc, char **argv)
{
    const ch_cdef_t *d;
    int c;
    ele = argc > 1 && !strcmp(argv[1], "elephant");
    d = ele ? &CH_ELEPHANT : &CH_GATOR;
    ch_cvoice_init(&v, d);
    if (ele)
        ch_elephant_wave(&wave, (ch_real)FS, CTRL);
    else
        ch_gator_wave(&wave, (ch_real)FS, CTRL);
    ch_up_init(&up, ele ? CH_ELE_DIV : CH_GATOR_DIV, getenv("CUT") ? (ch_real)atof(getenv("CUT")) : 0.9f);
    if (getenv("GAIN"))
        wave.output_gain *= (ch_real)atof(getenv("GAIN"));
    if (getenv("NOISE"))
        wave.noise_gain = (ch_real)atof(getenv("NOISE"));
    if (getenv("FADE_N"))
        wave.fade_n = (ch_real)atof(getenv("FADE_N"));
    ch_cvoice_gate(&v, 1);
    render(1.2);
    ch_cvoice_gate(&v, 0);
    render(0.4);
    for (c = 0; c < d->ncalls; c++) {
        const ch_ccall_t *k = &d->calls[c];
        ch_cvoice_call(&v, c, 0, 1);
        render(k->k[k->n - 1].t + 0.5);
    }
    write_wav(argc > 2 ? argv[2] : "creature.wav", out, n_out);
    printf("%s: %.2f s, sections %d + nose %d (valve at %d), %g Hz, resets %u\n", d->name, (double)n_out / FS, wave.n,
           wave.nn, wave.valve_m, (double)wave.fs, wave.resets);
    return 0;
}
