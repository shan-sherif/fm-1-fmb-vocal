/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shan-Sherif
 * fm-1-chimera-vocal: the FM-1's beatbox (engine/ch_beat.c) rendered as CHIMERA's `chimera render beatbox` renders
 * its own, for comparing them: each sound alone (a fresh voice in the ready posture, breath pressure 8, the gate shut,
 * the hit at 0.25 s, 0.5 s), then a preset beat through the sequencer. The voice is the FM-1's (app/app.c voice_init:
 * 44.1 kHz, the anatomy every 4th control step, the body's waveguide loss), its output before the app's gain.
 *
 *   cc -O2 -std=c99 -DCH_FM1 -DPLAT_HOST -I../../base/include -I../../engine -I../../app/dsp -o ../out/beat_test \
 *      beat_test.c ../../engine/ch_beat.c ../../engine/ch_speech.c ../../engine/ch_control.c ../../engine/ch_tract.c \
 *      ../../engine/ch_wave.c ../../engine/ch_voice.c ../../engine/ch_fast.c -lm
 *   ../out/beat_test OUTDIR [body 0..2] [preset 0..6] [bars] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ch_beat.h"
#include "ch_wave.h"
#include "voice_bodies.h"
#include "ch_song.h"

#define FS 44100
#define CTRL 64
#define ANAT 4
static const float BODY_FADE[3] = {0.995f, 0.995f, 0.98f};

static ch_control_t ctl;
static ch_tract_t tract;
static ch_wave_t wave;
static ch_shape_t shape;

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
        float v = x[i] * 32767.0f;
        int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

/* a fresh voice, as the app's voice_init and body switch make it, in the beatboxer's ready posture */
static void voice(int body)
{
    ch_source_t src;
    ch_real ready[CH_NARTIC];
    int k;
    ch_speech_body(body);
    body_tract_setup(&tract, &CH_BODIES[body]);
    ch_control_init(&ctl, (ch_real)FS, CTRL, 1.0f);
    ctl.k = CH_BODIES[body].scale;
    ctl.defer_contacts = 1;
    ctl.anat_every = ANAT;
    src = ctl.source;
    src.f0 = 100.0f;
    src.voiced = 1;
    src.pressure = 8.0f;
    ch_control_set_source(&ctl, &src);
    ch_control_set_gate(&ctl, 0);
    ch_beat_ready(ready);
    for (k = 0; k < CH_NARTIC; k++)
        if (ready[k] == ready[k])
            ctl.target[k] = ready[k];
    ch_tract_apply(&tract, ctl.x, 0.0f);
    ch_wave_init(&wave, (ch_real)FS, CTRL, tract.k, tract.lip_s);
    wave.fade = BODY_FADE[body];
    if (getenv("NARROW"))                     /* (lab) a section's area: its narrowest cell's (1) or its middle's (0) */
        wave.narrowest = (uint8_t)atoi(getenv("NARROW"));
    if (getenv("POCK_K"))
        wave.pock_k *= (ch_real)atof(getenv("POCK_K"));
    if (getenv("WALL_RAD"))
        wave.wall_rad = (ch_real)atof(getenv("WALL_RAD"));
    if (getenv("NOWALLS"))
        wave.walls = 0;
    if (getenv("NOVOL"))
        wave.vol_src = 0;
    if (getenv("POCK_MAX"))
        wave.pock_max = (ch_real)atof(getenv("POCK_MAX")) * 980.665f;
}

/* n samples (a whole number of control steps), the sequencer ticked each step when given */
static void render(float *out, size_t n, ch_beat_player_t *seq)
{
    size_t i;
    for (i = 0; i + CTRL <= n; i += CTRL) {
        int anat;
        if (seq)
            ch_beat_tick(seq, ctl.clock, &ctl.score);
        anat = ch_control_step(&ctl, &tract);
        if (anat)
            ch_tract_shape(&tract, &shape);
        ch_wave_block(&wave, anat ? &shape : 0, ANAT, &ctl.glottis, ctl.p_lung);
        if (seq && getenv("PAT_DEBUG") && anat && ctl.clock > atof(getenv("PAT_DEBUG")) && ctl.clock < atof(getenv("PAT_DEBUG")) + 0.25) {
            int c;
            printf("  %.3f fast %d(%d) pk s%d-%d step %d pockets %d p %.0f dorsum %.2f tby %.2f jaw %.2f velum %.2f port %.3f | ", (double)ctl.clock, wave.n_fast, wave.n_fast ? wave.fast[0] : -1, wave.n_pock ? wave.pock[0].a : -1, wave.n_pock ? wave.pock[0].b : -1, ch_beat_step_at(seq, ctl.clock), wave.n_pock,
                   wave.n_pock ? (double)wave.pock[0].p : 0.0, (double)ctl.x[CH_DORSUM], (double)ctl.x[CH_TB_Y], (double)ctl.x[CH_JAW], (double)ctl.x[CH_VELUM], (double)shape.velum_port_area);
            for (c = 0; c < shape.n_ph; c++) printf("%c", shape.ph_min[c] < wave.closed_area || shape.ph_areas[c] < wave.closed_area ? 'X' : '.');
            printf("|");
            for (c = 0; c < shape.n_or; c++) printf("%c", shape.or_min[c] < wave.closed_area || shape.or_areas[c] < wave.closed_area ? 'X' : '.');
            printf("\n");
        }
        if (getenv("AREA_AT") && anat && (ctl.clock - atof(getenv("AREA_AT"))) * (ctl.clock - atof(getenv("AREA_AT"))) < 9e-6) {
            /* (lab) AREA_AT=t: the waveguide's sections (cm2) and the noise sources' places then */
            int c;
            printf("t %.3f n %d len %.2f (ph %.2f or %.2f) teeth %.2f lips %.2f\n  areas:", (double)ctl.clock, wave.n,
                   (double)(shape.ph_len + shape.or_len), (double)shape.ph_len, (double)shape.or_len, (double)tract.teeth_s,
                   (double)tract.lip_s);
            for (c = 0; c < wave.n; c++)
                printf(" %d:%.3f", c, (double)wave.sec[c].area);
            printf("\n  oral cells (%.3f cm each):", (double)(shape.or_len / shape.n_or));
            for (c = 0; c < shape.n_or; c++)
                printf(" %.3f/%.3f", (double)shape.or_areas[c], (double)shape.or_min[c]);
            printf("\n");
            for (c = 0; c < wave.n_noise; c++)
                printf("  noise %d: src %d fwd %d bwd %d (cm %.2f %.2f %.2f) level %.3g lp %.3f hp %.3f\n", c, wave.n_src[c],
                       wave.n_fwd[c], wave.n_bwd[c], (double)shape.noise_s[c][0], (double)shape.noise_s[c][1],
                       (double)shape.noise_s[c][2], (double)wave.noise[c].level, (double)wave.noise[c].lp_coef,
                       (double)wave.noise[c].hp_coef);
        }
        if (getenv("BEAT_DEBUG") && anat && ctl.clock > 0.1 && ctl.clock < 0.3)
            printf("  %.3f closure %.2f a_g %.4f port %.3f larynx %.2f lip %.2f pockets %d: c%d-%d s%d-%d p %.0f -> %+.0f/step | cells shut:",
                   (double)ctl.clock, (double)ctl.glottis.closure, (double)wave.a_g, (double)shape.velum_port_area, (double)ctl.x[CH_LARYNX],
                   (double)ctl.x[CH_LIP_AP], wave.n_pock, wave.n_pock ? wave.pock[0].c0 : -1, wave.n_pock ? wave.pock[0].c1 : -1,
                   wave.n_pock ? wave.pock[0].a : -1, wave.n_pock ? wave.pock[0].b : -1, wave.n_pock ? (double)wave.pock[0].p : 0.0,
                   wave.n_pock ? (double)(wave.pock[0].dp * CTRL * ANAT) : 0.0);
        if (getenv("BEAT_DEBUG") && anat && ctl.clock > 0.1 && ctl.clock < 0.3) {
            int c;
            for (c = 0; c < shape.n_ph; c++) printf("%c", shape.ph_min[c] < wave.closed_area || shape.ph_areas[c] < wave.closed_area ? 'X' : '.');
            printf("|");
            for (c = 0; c < shape.n_or; c++) printf("%c", shape.or_min[c] < wave.closed_area || shape.or_areas[c] < wave.closed_area ? 'X' : '.');
            printf("\n");
        }
        ch_wave_render(&wave, out + i, CTRL);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const int body = argc > 2 ? atoi(argv[2]) : 0;
    const int preset = argc > 3 ? atoi(argv[3]) : 0;
    const double bars = argc > 4 ? atof(argv[4]) : 2.0;
    static float out[FS * 30];
    static ch_gesture_t buf[CH_BEAT_MAX_G];
    char path[512];
    int s;
    if (getenv("BREATH")) {                  /* (lab) BREATH="id:k,id:k": a sound's breath factor */
        const char *q = getenv("BREATH");
        while (*q) {
            int id = atoi(q);
            const char *c = strchr(q, ':');
            if (!c)
                break;
            ch_beat_breath[id] = (ch_real)atof(c + 1);
            q = strchr(c, ',');
            if (!q)
                break;
            q++;
        }
    }
    if (getenv("SONG")) {                    /* (lab) SONG="words" sung on TUNE (0..5), as the FM-1's SING page sings */
        static ch_syl_t syls[CH_SYL_MAX];
        static ch_sung_t notes[CH_SONG_MAX];
        static ch_real times[CH_SONG_MAX + 1];
        static ch_gesture_t g[256];
        const ch_tune_t *t = &CH_TUNES[getenv("TUNE") ? atoi(getenv("TUNE")) : 0];
        const int oct = body == 0 ? t->oct_male : body == 1 ? t->oct_female : t->oct_child;
        int ns = ch_lyrics(getenv("SONG"), 0, 0, syls, CH_SYL_MAX), nn, i, k, total = 0;
        size_t n;
        nn = ch_song_fit(t, syls, ns, 12 * oct, notes, CH_SONG_MAX);
        ch_song_times(notes, nn, t->bpm, times);
        voice(body);
        for (k = 0; k < CH_NARTIC; k++)
            ctl.target[k] = CH_NEUTRAL[k];
        for (i = 0; i < nn;) {               /* (all phrases at once here: the FM-1 streams them) */
            ch_plan_t plan;
            plan.g = g;
            plan.cap = 256;
            plan.n = 0;
            plan.dropped = 0;
            i = ch_song_plan_phrase(notes, nn, i, syls, times, 0.35f, 35.0f, &plan);
            for (k = 0; k < plan.n; k++)
                ch_score_add(&ctl.score, &g[k]);
            total += plan.n;
        }
        for (i = 0; i < nn; i++)
            printf("%s%s%d:%.1f", i ? " " : "", notes[i].held ? "~" : syls[notes[i].syl].text, notes[i].midi, (double)notes[i].beats);
        n = (size_t)((times[nn] + 1.5) * FS) / CTRL * CTRL;
        memset(out, 0, sizeof out);
        render(out, n, 0);
        snprintf(path, sizeof path, "%s/song.wav", dir);
        write_wav(path, out, n);
        printf("\nsung: %d syllables, %d notes, %d gestures, %.2f s, score dropped %u\n", ns, nn, total, (double)times[nn], ctl.score.dropped);
        return 0;
    }
    if (getenv("SAY")) {                     /* (lab) speech instead: SAY="text" spoken at the body's pitch, as PLAY does */
        static ch_seg_t segs[CH_SEG_MAX];
        static ch_phrase_t ph[CH_PHRASE_MAX];
        static ch_gesture_t g[256];
        ch_plan_opts_t o;
        ch_plan_t plan;
        int n_segs, np, i;
        size_t n = FS * 4 / CTRL * CTRL;
        voice(body);
        np = ch_text_to_phrases(getenv("SAY"), 0, 0, segs, CH_SEG_MAX, &n_segs, ph, CH_PHRASE_MAX);
        ch_plan_defaults(&o);
        o.t0 = 0.05f;
        o.f0 = CH_BODIES[body].f0;
        plan.g = g;
        plan.cap = 256;
        plan.n = 0;
        plan.dropped = 0;
        ch_plan_phrases(segs, ph, np, &o, &plan);
        for (i = 0; i < plan.n; i++)
            ch_score_add(&ctl.score, &g[i]);
        {   /* the speaking posture: the neutral one, the breath the score's (as the app's WORDS) */
            int k;
            for (k = 0; k < CH_NARTIC; k++)
                ctl.target[k] = CH_NEUTRAL[k];
        }
        memset(out, 0, sizeof out);
        render(out, n, 0);
        snprintf(path, sizeof path, "%s/say.wav", dir);
        write_wav(path, out, n);
        printf("said: %d gestures\n", plan.n);
        return 0;
    }
    for (s = 0; s < CH_BEAT_N; s++) {
        if (getenv("BEAT_DEBUG") && atoi(getenv("BEAT_DEBUG")) != s)
            continue;
        {
        const size_t n = FS / 2 / CTRL * CTRL;
        ch_plan_t o;
        int i;
        char file[16];
        voice(body);
        o.g = buf;
        o.cap = CH_BEAT_MAX_G;
        o.n = 0;
        o.dropped = 0;
        ch_beat_build(s, 100.0f, 0.15f, 1.0f, 0.25f, &o);
        for (i = 0; i < o.n; i++)
            ch_score_add(&ctl.score, &buf[i]);
        memset(out, 0, sizeof out);
        render(out, n, 0);
        for (i = 0; CH_BEAT_SOUNDS[s].id[i] && i < 15; i++) {
            char c = CH_BEAT_SOUNDS[s].id[i];
            file[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ? c : '_';
        }
        file[i] = 0;
        snprintf(path, sizeof path, "%s/%s.wav", dir, file);
        write_wav(path, out, n);
        printf("%-4s %-13s %2d gestures, lead-in %3.0f ms%s\n", CH_BEAT_SOUNDS[s].id, CH_BEAT_SOUNDS[s].name, o.n,
               ch_beat_lead_in(s) * 1000.0, o.dropped ? " (DROPPED)" : "");
        }
    }
    if (getenv("BEAT_DEBUG"))
        return 0;
    {   /* a preset beat, `bars` bars through the sequencer (CHIMERA's render: 0.5 s lead, then the bars, 1 s tail) */
        static ch_beat_step_t st[CH_BEAT_MAX_STEPS];
        static ch_beat_player_t seq;
        const ch_beat_preset_t *p = &CH_BEAT_PRESETS[preset];
        const int ns = ch_beat_parse(p->text, st, CH_BEAT_MAX_STEPS);
        size_t n;
        voice(body);
        ch_beat_player_init(&seq);
        ch_beat_set_steps(&seq, st, ns);
        ch_beat_set_tempo(&seq, p->bpm, p->swing);
        ch_beat_start(&seq, 0.5f - CH_BEAT_HORIZON);
        n = (size_t)((0.5 + bars * 16.0 * 60.0 / p->bpm / 4.0 + 1.0) * FS) / CTRL * CTRL;
        if (n > sizeof out / sizeof out[0])
            n = sizeof out / sizeof out[0] / CTRL * CTRL;
        memset(out, 0, sizeof out);
        render(out, n, &seq);
        snprintf(path, sizeof path, "%s/pattern.wav", dir);
        write_wav(path, out, n);
        printf("pattern \"%s\": %d steps, %.0f bpm, %.2f s, score dropped %u\n", p->name, ns, (double)p->bpm, (double)n / FS,
               ctl.score.dropped);
    }
    return 0;
}
