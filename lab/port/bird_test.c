/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the FM-1's songbird (engine/ch_creature.c: its light waveguide at 2x) rendered as CHIMERA's
 * `chimera render creature songbird` renders its own: a sustained note at its default pitch with one voice (1.2 s,
 * 0.4 s of release), then each call (its last key + 0.5 s), both voices. BIRD_GAIN / BIRD_FADE / BIRD_FADEK: lab
 * overrides of its level and its trachea's and beak's losses.
 *
 *   cc -O2 -std=c99 -DCH_FM1 -DPLAT_HOST -I../../base/include -I../../engine -o ../out/bird_test bird_test.c \
 *      ../../engine/ch_creature.c -lm
 *   ../out/bird_test OUT.wav */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ch_creature.h"

#define FS 44100

static ch_cvoice_t v;
static ch_bird_t bird;
static float out[FS * 12];
static size_t n_out;

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
    const int ctrl = 64;                     /* output samples a control step */
    size_t n = (size_t)(sec * FS) / ctrl * ctrl, i;
    for (i = 0; i < n && n_out + ctrl <= sizeof out / sizeof out[0]; i += ctrl) {
        int s;
        ch_cvoice_step(&v, (ch_real)ctrl / FS);
        ch_bird_control(&bird, &v, ctrl);
        for (s = 0; s < ctrl; s++)
            out[n_out++] = (float)ch_bird_sample(&bird);
    }
}

int main(int argc, char **argv)
{
    int c;
    clock_t t0;
    ch_cvoice_init(&v, &CH_BIRD);
    ch_bird_init(&bird, &v);
    if (getenv("BIRD_GAIN"))
        bird.gain = (ch_real)atof(getenv("BIRD_GAIN"));
    if (getenv("BIRD_FADE"))
        bird.fade_t = (ch_real)atof(getenv("BIRD_FADE"));
    if (getenv("BIRD_FADEK"))
        bird.fade_k = (ch_real)atof(getenv("BIRD_FADEK"));
    t0 = clock();
    ch_cvoice_set(&v, CH_BIRD_VOICES, 0);   /* one voice for the pitch check */
    ch_cvoice_gate(&v, 1);
    render(1.2);
    ch_cvoice_gate(&v, 0);
    render(0.4);
    ch_cvoice_set(&v, CH_BIRD_VOICES, 0.5f);
    for (c = 0; c < CH_BIRD.ncalls; c++) {
        const ch_ccall_t *k = &CH_BIRD.calls[c];
        ch_cvoice_call(&v, c, 0, 1);
        render(k->k[k->n - 1].t + 0.5);
    }
    printf("%.2f s in %.0f ms\n", (double)n_out / FS, 1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
    write_wav(argc > 1 ? argv[1] : "bird.wav", out, n_out);
    return 0;
}
