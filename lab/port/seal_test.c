/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the FM-1's seal (engine/ch_creature.c) rendered as CHIMERA's `chimera render creature seal`
 * renders its own, for comparing them: a sustained note at its default pitch (1.2 s, then 0.4 s of release), then each
 * call at its own pitch (its last key + 0.5 s). The FM-1's waveguide at 44.1 kHz, a control step every 64 samples.
 *
 *   cc -O2 -std=c99 -DCH_FM1 -DPLAT_HOST -I../../base/include -I../../engine -o ../out/seal_test seal_test.c \
 *      ../../engine/ch_creature.c ../../engine/ch_wave.c ../../engine/ch_voice.c ../../engine/ch_fast.c \
 *      ../../engine/ch_tract.c -lm
 *   ../out/seal_test OUT.wav */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ch_creature.h"
#include "ch_wave.h"

#define FS 44100
#define CTRL 64

static ch_cvoice_t v;
static ch_seal_t seal;
static ch_wave_t wave;
static ch_shape_t shape;
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
    size_t n = (size_t)(sec * FS) / CTRL * CTRL, i;
    for (i = 0; i < n && n_out + CTRL <= sizeof out / sizeof out[0]; i += CTRL) {
        ch_glottis_params_t g;
        ch_real p_lung;
        ch_cvoice_step(&v, (ch_real)CTRL / FS);
        ch_seal_control(&seal, &v, (ch_real)CTRL / FS, (ch_real)FS, &g, &p_lung, &shape);
        ch_wave_block(&wave, &shape, 1, &g, p_lung);
        ch_wave_render(&wave, out + n_out, CTRL);
        n_out += CTRL;
    }
}

int main(int argc, char **argv)
{
    int c;
    ch_cvoice_init(&v, &CH_SEAL);
    ch_seal_init(&seal);
    ch_wave_init(&wave, (ch_real)FS, CTRL, 1.0f, CH_SEAL_LEN);
    wave.output_gain = CH_SEAL_GAIN;
    wave.port_len = 0.4f;                    /* the mammal's glottal port: PortSpec(0, 0.4).slit(sqrt(4) * 0.9) */
    wave.port_slit = 1.8f;
    wave.wall_rad = 0.6f;
    if (getenv("SEAL_GAIN"))
        wave.output_gain *= (ch_real)atof(getenv("SEAL_GAIN"));
    ch_cvoice_gate(&v, 1);
    render(1.2);
    ch_cvoice_gate(&v, 0);
    render(0.4);
    for (c = 0; c < CH_SEAL.ncalls; c++) {
        const ch_ccall_t *k = &CH_SEAL.calls[c];
        ch_cvoice_call(&v, c, 0, 1);
        render(k->k[k->n - 1].t + 0.5);
    }
    write_wav(argc > 1 ? argv[1] : "seal.wav", out, n_out);
    printf("%.2f s, waveguide sections %d, resets %u\n", (double)n_out / FS, wave.n, wave.resets);
    return 0;
}
