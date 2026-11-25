/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the anatomy's (engine/ch_tract.c) stages timed on the host, over a moving articulation: where an
 * apply spends its time (the FM-1's core 1 runs one every ANAT_EVERY control steps). Built with -DCH_PROF=prof_mark.
 *   cc -O2 -std=c99 -DCH_FM1 -DPLAT_HOST -DCH_PROF=prof_mark -I../../base/include -I../../engine -I../../app/dsp \
 *      -include tract_prof.h -o ../out/tract_prof tract_prof.c ../../engine/ch_tract.c ../../engine/ch_control.c -lm */
#include <stdio.h>
#include <time.h>
#include "ch_control.h"
#include "voice_bodies.h"
extern unsigned long ch_count[8];   /* (ch_tract.c -DCH_COUNT: crossings, projections, gap segments, ...) */
static double acc[9];
static double last;
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
void prof_mark(int stage) { double n = now_ns(); if (stage > 0) acc[stage] += n - last; last = n; }
int main(void)
{
    static ch_tract_t t;
    static const char *const NAME[9] = {"", "jaw, velum", "outer contour", "tongue", "grid (rays)", "contact model", "areas", "fill + clamp", "noise sources"};
    ch_real a[CH_NARTIC];
    int i, k, n = 20000;
    double total = 0;
    body_tract_setup(&t, &CH_BODIES[0]);
    for (k = 0; k < CH_NARTIC; k++)
        a[k] = CH_NEUTRAL[k];
    for (i = 0; i < n; i++) {               /* the jaw, tongue and lips moving through vowels and closures */
        a[CH_JAW] = 0.6f + 0.6f * __builtin_sinf(i * 0.013f);
        a[CH_TB_X] = 2.9f + 0.6f * __builtin_sinf(i * 0.011f);
        a[CH_TB_Y] = 6.9f + 0.6f * __builtin_sinf(i * 0.017f);
        a[CH_LIP_AP] = 0.5f + 0.6f * __builtin_sinf(i * 0.021f);
        a[CH_TT_Y] = 0.4f + 0.5f * __builtin_sinf(i * 0.019f);
        ch_tract_apply(&t, a, 4 * 64 / 44100.0f);
    }
    for (k = 1; k < 9; k++)
        total += acc[k];
    for (k = 1; k < 9; k++)
        printf("%-15s %5.1f %%  (%.2f us an apply)\n", NAME[k], 100 * acc[k] / total, acc[k] / n / 1000);
    printf("total %.2f us an apply on this machine\n", total / n / 1000);
    printf("an apply: %.0f ray crossings tests, %.0f projection segments, %.0f gap segments\n", ch_count[0] / (double)n,
           ch_count[1] / (double)n, ch_count[2] / (double)n);
    return 0;
}
