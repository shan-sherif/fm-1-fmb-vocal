/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Copyright (C) 2026 Shan-Sherif */
/* FMB Vocal: a voice engine based on CHIMERA's, on fm1-base (started from its template). The keys sing the word slot's
 * syllables (WORDS) or hold a vowel (VOWEL, FX), with velocity and pressure (lib/touch.h), and go out over USB-MIDI;
 * MIDI in plays and lights the keys; five knob pages, eight presets, SAVE. Part of the unity build (device/unity.c,
 * host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"

#define SET_MAGIC 0x46565331u            /* "FVS1": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t wave;                        /* unused (the template's wave): kept so saved settings keep their layout */
    uint8_t ble_off;                     /* Bluetooth switched off (REC held); 0, the default: on */
    uint8_t slot;                        /* the word slot the keys sing (0..WORDS_SLOTS-1) */
    uint8_t preset;                      /* the voice preset (0..NPRESETS-1) */
    uint8_t kb;                          /* the word editor's keys: 0 A-Z, 1 QWERTY (0.5.0; was spare, saved as 0) */
    uint8_t rsv[2];
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

/* ------------------------------------------------------------------ voice --- */
/* CHIMERA's voice (engine/, MIT, Jameson Hodge) on both cores, with the waveguide tract (ch_wave.c: CHIMERA's full
 * airway physics is about twice what the FM-1 can run). Each audio block is APP_BLOCK
 * samples, VM_CTRL to a control step: the second core renders the waveguide for this block from what the first core
 * computed during the block before (the anatomy's shape, the glottis's parameters, the breath), while the first core
 * computes the next: CHIMERA's control (ch_control.c) and its anatomy, every 2, 4 or 8 control steps (the GLO
 * page's K1; 4 to start), the tract gliding between. Each core's time is shown.
 *
 * The words: CHIMERA's front end (ch_speech.c) and its live Sing mode (ch_live.c) run here in the main loop; what they
 * plan goes to the audio interrupt through a mailbox and into the control's score there. The keys (and MIDI in) sing
 * the lyrics a syllable a key; PLAY speaks them. */
#include "ch_tract.h"
#include "ch_wave.h"
#include "ch_control.h"
#include "ch_live.h"
#include "ch_beat.h"                         /* CHIMERA's beatboxer (SEQ, 0.7.0) */
#include "ch_song.h"                         /* CHIMERA's Sing mode's tunes (ENV, 0.8.0) */
#define VM_FS 44100                           /* the voice's rate (Hz) */
#define VM_CTRL 64                            /* samples a control step (1.45 ms) */
#include "voice_bodies.h"                   /* CHIMERA's three bodies: male, female, child (ALGORITHM) */
#include "ch_creature.h"                    /* and its Bestiary's seal, the fourth (0.7.0) */

/* the words: the slots the Studio sends (below, "words"); CHIMERA's presets until it has (lab/tools/gen_words_default.py) */
#include "words_default.h"
static uint8_t body_cur;                     /* main loop: the body (0 male, 1 female, 2 child; CH_BODIES; 3 the seal) */
static uint8_t body_hum;                     /* the human body last chosen: the words', the vowel pad's, the drawn head's */
#define BODY_SEAL 3                          /* the Bestiary (LFO): CHIMERA's seal (ch_creature.c), in place of a body */
/* the waveguide's loss a section, by body (0.6.0): the child's smaller tract loses more to its walls than the male's
 * setting gives; tuned against CHIMERA's renders of each body (lab/port/replay --wave, CH_WAVE_FADE: the child's
 * spectrum's mean difference 6.0 -> 5.5 dB, its 90th percentile 11.2 -> 8.2 dB; the male's and the female's best at
 * the setting they had) */
static const float BODY_FADE[4] = {0.995f, 0.995f, 0.98f, 0.995f};
/* and its level after: the child's more loss makes it 4.6 dB quieter than CHIMERA's child (it was 1.1 dB louder);
 * +4.5 dB puts it back in line (the lab's level against CHIMERA's render) */
static const float BODY_GAIN[4] = {1.0f, 1.0f, 1.68f, 1.0f};   /* (the seal's level: its output gain, below) */
static volatile uint8_t body_req, body_audio; /* the body asked for, and the one the audio has (it switches at a block) */
#define SPEECH_F0 (mix_main.b.f0)          /* the body's speaking pitch (lab record: CHIMERA's render; a blend's between) */
/* the body as a blend of the three (1.0.0, CHIMERA's BodyMix: gender, the male-female axis, and age, toward the child;
 * weights male (1 - a)(1 - g), female (1 - a) g, child a): its anatomy's dimensions by weight, its speaking pitch and
 * its vowels' scale by weight in log, the waveguide's loss and the level by weight, the airway's indices and the name
 * the nearest body's */
typedef struct {
    ch_body_t b;
    float fade, gain;
} bodymix_t;
static bodymix_t mix_main;                    /* main loop: the body the knobs hold */
static bodymix_t mix_req;                     /* (for the audio, at its next block) */
static volatile uint8_t mix_seq;
static uint8_t mix_seq_audio;
static float body_gain_audio = 1.0f;
static void body_mix(const float w[3], int near, bodymix_t *m);
static void body_label(float g, float a, char *t);
static float body_g = -1.0f, body_a = -1.0f;  /* main loop: the blend applied (-1: none yet) */
#define BODY_BLENDED 3                         /* ALGORITHM's fourth body (kv[K_BODY]): GENDER and AGE's blend */
static const char *body_name(int b) { return b >= 0 && b < NBODIES ? CH_BODIES[b].name : "SEAL"; }

#define NSUB ((int)(APP_BLOCK / VM_CTRL))           /* control steps a block */
/* a block on plat_ticks24's clock (24 per microsecond). (256 * 24000000 overflows 32 bits: v0.0.1 showed every
 * share 3.32 times too high) */
#define BLOCK_TICKS (APP_BLOCK * (24000000u / 100u) / (VM_FS / 100u))
typedef struct {                             /* one control step, from the first core to the second */
    ch_shape_t shape;                        /* the anatomy's, when it ran at this step */
    uint8_t has_shape, steps;                /* (steps: how many control steps to glide to it over) */
    ch_glottis_params_t g;
    float p_lung;
    ch_wvalve_t valve;                       /* (the elephant's lips) */
} step_t;
static ch_tract_t tract PLAT_POOL;
static ch_control_t ctl PLAT_POOL;
static ch_wave_t wave PLAT_POOL;
static step_t steps[2][NSUB] PLAT_POOL;     /* [the block being rendered / the next one][its control steps] */
static float vbuf[APP_BLOCK] PLAT_POOL;
static int cur;                              /* steps[cur] is the block the waveguide renders now */
#define QUIET_BLOCKS 8u                      /* ~46 ms of silence after the voice before it rests */
static uint32_t quiet_blocks;
static int resting = 1;                      /* nothing simulated until the first note */
static int two_cores;
/* the anatomy every ANAT_EVERY control steps, the tract gliding between. Fixed: every 2nd overran core 1 while it
 * speaks (0.0.5: 210 %), every 4th fits (0.0.7 onwards: under 70 % at peak). A faster anatomy could try 2 here (the lab
 * measures any: replay --anat-every=N) */
#define ANAT_EVERY 4
static volatile uint8_t anat_every = ANAT_EVERY;   /* (the debug page's K1 tries 8, 4 or 2 until power-off; never every
                                                    * step: it overloads core 1) */
/* the debug page's K2 (0.8.0, until power-off): the human voice's treble shelf, -7 dB over ~2.5 kHz (ch_wave.h), to
 * hear against the voice without it */
static volatile uint8_t shelf_on;
#define SHELF_G (0.4467f - 1.0f)
/* the timings, on plat_ticks24's clock (24 MHz): the last block's, the sum and the worst since the screen read them */
static volatile uint32_t c1_sum, c1_peak, c0_sum, c0_peak, n_blk, c1_last;
static float gain_now;                       /* glides to MASTER: no step at a block's edge */

/* The mailbox, main loop -> audio interrupt: gestures (times from now) and, in order among them, tags to release
 * (release[k] goes before gesture at[k]), or the whole score cleared first. The main loop collects its commands in
 * `pend` (never waiting: in the host simulator the audio runs on the main loop's thread) and moves them into the
 * mailbox whenever the audio interrupt has emptied it (each block, 5.8 ms). */
#define MB_G 224
#define MB_R 32
typedef struct {
    ch_gesture_t g[MB_G];
    int n;
    ch_real release[MB_R];
    int16_t at[MB_R];
    int n_release;
    uint8_t clear;
    uint8_t song;                            /* a sung phrase (the SING page): 1 the song's first (its clock starts at the
                                              * take), 2 a later one; its times are from the song's start */
} mail_t;
static mail_t mb PLAT_POOL, pend PLAT_POOL;
static float song_clock0;                    /* (the audio's) the song's start on the voice's clock */
static volatile float song_now = -1.0f;      /* the song's time (s), for the main loop; -1: none started */
static volatile uint8_t song_on;             /* (the audio's) a song is running: its clock is published, no rest */
static volatile uint8_t mb_full;

/* Notes, audio interrupt (MIDI in) -> main loop: a small ring */
#define NQ 32u
static volatile uint32_t nq_head, nq_tail;
static volatile uint8_t nq_note[NQ], nq_vel[NQ];   /* velocity 0: off */

/* the beatbox's page (SEQ, below before input): on, the beat slot shown, step editing, its cursor, unsaved, playing */
static struct {
    uint8_t on, slot, edit, cur, dirty, play;
} bt;
/* the SING page (ENV, 0.8.0, below before input): on, singing, unsaved */
static struct {
    uint8_t on, playing, dirty;
    uint8_t mel, edit, cur;                   /* the MELODY page (SEQ twice): on, step editing, its cursor */
    uint8_t beast;                            /* (0.2.0) opened in the Bestiary: the creature plays the notes */
} sg;

static ch_beat_player_t seq PLAT_POOL;      /* the beatbox's sequencer (the audio interrupt's) */
static ch_live_t live PLAT_POOL;            /* the main loop's: the lyrics' syllables and the cursor */
static ch_live_cmd_t live_cmd PLAT_POOL;
static volatile int32_t voice_note = -1;     /* the note sounding (shown), -1 none */
static volatile uint8_t speaking;            /* PLAY's speech is in the score */

/* the knobs' section (below): VOWEL mode routes the keys there; the speech rate PLAY speaks at */
static uint8_t vowel_mode;
static float speech_rate = 1.0f;
static void vowel_on(int32_t note, uint32_t vel);
static void beat_note(int32_t note);           /* (the beatbox: MIDI in's note as its key's pad) */
static void seal_on(int32_t note);
static void seal_off(int32_t note);
static void creature_pick(int32_t e);
static void creatures_load(void);
static void song_load(void);
static void melody_load(void);
static void beat_open(void);
static void beast_start(int tune, float tempo, int shift);
static void beast_tick(void);
static void beast_release(void);
static void beast_sound_turn(int32_t e);
static void draw_beast_top(const char *status, int lit);
static int mel_count(int m);
static void beats_load(void);
static void vowel_off(int32_t note);
static void vp_take(void);
static void int_text(int32_t v, char *b);
static void fmt1(float v, char *b);
static void text_cat(char *d, const char *s);
#define NPRESETS 8
static void presets_load(void);
static void preset_apply(int i);

/* the words (below): the slot sung, the Studio's pronunciations */
static char lyric[160];
static const char *words_lookup(const char *w, void *user);
static void words_load(void);
static void words_select(int i);

/* the first core: one control step (HumanVoice::control: ch_control.c) */
/* expression (0.6.0): a held key's pressure, or MIDI in's aftertouch, deepens the vibrato; MIDI in's pitch bend bends
 * the pitch +-2 semitones; its mod wheel (CC1) adds breath (vp_send). The main loop and MIDI in set the targets, the
 * control step glides to them (no steps in the sound) and adds them to what the glottis is told */
#define EXPR_VIB_CENTS 50.0f                 /* full pressure: this much more vibrato (cents) */
#define EXPR_BEND_SEMIS 2.0f
static volatile float xvib_target, xbend_target = 1.0f;   /* cents; the pitch's ratio */
static float xvib_now, xbend_now = 1.0f;     /* (the audio interrupt's) */
static volatile uint8_t midi_prs, midi_mod;  /* MIDI in: the last aftertouch, the mod wheel (0..127) */
static volatile int16_t midi_bend;           /* MIDI in: the pitch bend, -8192..8191 */

/* the seal (0.7.0, the fourth body): its creature voice and its control, the audio interrupt's; the main loop's
 * settings, gate and calls come through a mailbox (one struct, a flag; a call when its number changes) */
typedef struct {
    float perf[CH_CP_MAX];                   /* the settings, the pitch included */
    float gate;
    int8_t call;
    uint8_t call_seq;                        /* (a new call: a new number) */
    float call_pitch;                        /* Hz; 0: its own */
} cmail_t;
static cmail_t cm_out;
static volatile uint8_t cm_dirty;
static ch_cvoice_t cvoice PLAT_POOL;
static ch_seal_t seal;
#include "cat.h"                             /* the cat (FMB Vocal's own creature) */
static cat_t cat;
#include "robot.h"                           /* the robot (FMB Vocal's own, 0.3.0): dubstep */
static robot_t robot;
#include "squid.h"                           /* Squidlad (FMB Vocal's own, 0.4.0): babble */
static squid_t squid;
static ch_bird_t bird PLAT_POOL;
#define NCRE 7                               /* the Bestiary's creatures, in ALGORITHM's order: the cat (FMB Vocal's own,
                                              * app/dsp/cat.h, the first), the songbird, the seal, the alligator, the
                                              * elephant (CHIMERA's), the robot (0.3.0, app/dsp/robot.h) and Squidlad
                                              * (0.4.0, app/dsp/squid.h), FMB Vocal's own (each new one last: the others
                                              * keep their numbers in the saves) */
enum { CRE_CAT, CRE_BIRD, CRE_SEAL, CRE_GATOR, CRE_ELEPHANT, CRE_ROBOT, CRE_SQUID };
static const ch_cdef_t *const CRE_DEF[NCRE] = {&CH_CAT, &CH_BIRD, &CH_SEAL, &CH_GATOR, &CH_ELEPHANT, &CH_ROBOT, &CH_SQUID};
/* ALGORITHM's order (0.4.0): FMB Vocal's own creatures first (the robot, Squidlad, the cat), CHIMERA's after (the
 * songbird, the seal, the alligator, the elephant). The numbers above stay each creature's own (the saves keep them);
 * this is only the order they are turned through, and the first is where a fresh Bestiary opens */
static const uint8_t CRE_ORDER[NCRE] = {CRE_ROBOT, CRE_SQUID, CRE_CAT, CRE_BIRD, CRE_SEAL, CRE_GATOR, CRE_ELEPHANT};
static int cre_pos(int which)                /* a creature's place in ALGORITHM's order */
{
    int i;
    for (i = 0; i < NCRE; i++)
        if (CRE_ORDER[i] == which)
            return i;
    return 0;
}
static volatile float ele_lips_max;         /* (the test: the elephant's lips, their widest) */
static ch_up_t cre_up;                       /* the alligator's and the elephant's: their half rate up to 44.1 kHz */
static volatile uint8_t cre_div = 1;         /* the audio's creature runs at VM_FS / cre_div */
#define CRE_LEVEL 1.6f                       /* the alligator's and the elephant's output gain on CHIMERA's (+4 dB: the
                                              * FM-1's voice is 4 dB over CHIMERA's; lab/port/cre_test: their levels
                                              * within ~1 dB of CHIMERA's renders) */
static volatile uint8_t cre_req, cre_audio;  /* the creature asked for (main loop), the one the audio has */
static volatile uint8_t bird_audio;          /* the audio renders the songbird (on the second core, whole) */
static uint8_t cm_seen;
static volatile uint8_t seal_audio;          /* the audio renders the seal */
static volatile float seal_breath;           /* (shown: its lungs' pressure, cmH2O) */

static void cm_take(void)                    /* audio interrupt: the seal's settings, now */
{
    int i;
    if (!cm_dirty)
        return;
    for (i = 0; i < CRE_DEF[cre_audio]->np; i++)
        ch_cvoice_set(&cvoice, i, cm_out.perf[i]);
    ch_cvoice_gate(&cvoice, cm_out.gate);
    if (cm_out.call_seq != cm_seen) {
        cm_seen = cm_out.call_seq;
        if (cm_out.call >= 0)
            ch_cvoice_call(&cvoice, cm_out.call, cm_out.call_pitch, 1.0f);
        else
            ch_cvoice_stop_call(&cvoice);
    }
    cm_dirty = 0;
}

static void control_step(step_t *d)
{
    if (bird_audio) {                        /* the songbird: all of it on the second core (tract_job) */
        d->has_shape = 0;
        return;
    }
    if (seal_audio) {                        /* the seal: its tubes every step (no anatomy), its glottis, its lungs; the
                                              * alligator and the elephant the same, at their rate */
        const float dt = (float)VM_CTRL / (float)VM_FS, fs = (float)VM_FS / (float)cre_div;
        ch_cvoice_step(&cvoice, dt);
        if (cre_audio == CRE_GATOR)
            ch_gator_control(&cvoice, fs, &d->g, &d->p_lung, &d->shape);
        else if (cre_audio == CRE_ELEPHANT)
            ch_elephant_control(&cvoice, fs, &d->g, &d->p_lung, &d->shape, &d->valve);
        else if (cre_audio == CRE_CAT)
            cat_control(&cat, &cvoice, dt, (float)VM_FS, &d->g, &d->p_lung, &d->shape);
        else if (cre_audio == CRE_ROBOT)
            robot_control(&robot, &cvoice, dt, (float)VM_FS, &d->g, &d->p_lung, &d->shape);
        else if (cre_audio == CRE_SQUID)
            squid_control(&squid, &cvoice, dt, (float)VM_FS, &d->g, &d->p_lung, &d->shape);
        else
            ch_seal_control(&seal, &cvoice, dt, (float)VM_FS, &d->g, &d->p_lung, &d->shape);
        d->has_shape = 1;
        d->steps = 1;
        xbend_now += (xbend_target - xbend_now) * 0.25f;
        d->g.f0 *= xbend_now;
        seal_breath = d->p_lung / 980.665f;
        return;
    }
    ctl.anat_every = anat_every;
    d->has_shape = (uint8_t)ch_control_step(&ctl, &tract);
    if (d->has_shape) {
        ch_tract_shape(&tract, &d->shape);
        d->steps = (uint8_t)ctl.anat_every;
    }
    d->g = ctl.glottis;
    xvib_now += (xvib_target - xvib_now) * 0.05f;    /* ~30 ms */
    xbend_now += (xbend_target - xbend_now) * 0.25f; /* ~5 ms: a bend follows the wheel closely */
    d->g.vibrato_depth += xvib_now;
    d->g.f0 *= xbend_now;
    d->p_lung = ctl.p_lung;
}

/* the second core (or the first, alone): the waveguide for a block. In RAM (.c1_text, as the engine's audio rate),
 * so what it calls in the flash it calls through PLAT_FAR */
__attribute__((section(".c1_text"))) static void tract_job(uint32_t which)
{
    uint32_t t0 = PLAT_FAR(plat_ticks24)();
    int s;
    if (bird_audio) {                        /* the songbird: its control steps and its airway at 2x, here */
        for (s = 0; s < NSUB; s++) {
            float *o = vbuf + (uint32_t)s * VM_CTRL;
            int i;
            PLAT_FAR(ch_cvoice_step)(&cvoice, (float)VM_CTRL / (float)VM_FS);
            PLAT_FAR(ch_bird_control)(&bird, &cvoice, VM_CTRL);
            bird.l.f0 *= xbend_target;       /* (MIDI in's pitch bend) */
            bird.r.f0 *= xbend_target;
            for (i = 0; i < VM_CTRL; i++)
                o[i] = (float)ch_bird_sample(&bird);   /* (in RAM, as the waveguide) */
        }
        seal_breath = bird.ps / 980.665f;
        c1_last = PLAT_FAR(plat_ticks24)() - t0;
        return;
    }
    wave.shelf_g = shelf_on && !seal_audio ? SHELF_G : 0.0f;
    if (cre_div > 1) {                       /* the alligator, the elephant: their waveguide at its rate, interpolated up */
        const int nl = VM_CTRL / cre_div;
        float low[VM_CTRL];
        for (s = 0; s < NSUB; s++) {
            step_t *d = &steps[which][s];
            if (cre_audio == CRE_ELEPHANT)
                ch_wave_valve(&wave, &d->valve);
            ch_wave_block(&wave, d->has_shape ? &d->shape : 0, d->steps, &d->g, d->p_lung);
            ch_wave_render(&wave, low, (uint32_t)nl);
            ch_up_run(&cre_up, low, nl, vbuf + (uint32_t)s * VM_CTRL);
            if (wave.vlv_open > ele_lips_max)
                ele_lips_max = wave.vlv_open;
        }
        c1_last = PLAT_FAR(plat_ticks24)() - t0;
        return;
    }
    for (s = 0; s < NSUB; s++) {
        step_t *d = &steps[which][s];
        ch_wave_block(&wave, d->has_shape ? &d->shape : 0, d->steps, &d->g, d->p_lung);
        ch_wave_render(&wave, vbuf + (uint32_t)s * VM_CTRL, VM_CTRL);
    }
    c1_last = PLAT_FAR(plat_ticks24)() - t0;
}

static void voice_init(void)
{
    int s;
    ch_source_t src;
    body_tract_setup(&tract, &CH_BODIES[0]);
    {   /* the male whole, until the knobs say otherwise */
        static const float w[3] = {1, 0, 0};
        body_mix(w, 0, &mix_main);
        mix_req = mix_main;
        mix_seq_audio = mix_seq;
        body_g = body_a = -1.0f;
    }
    ch_control_init(&ctl, (float)VM_FS, VM_CTRL, 1.0f);   /* HumanVoice::new: the neutral posture, at rest */
    src = ctl.source;
    src.f0 = SPEECH_F0;
    ch_control_set_source(&ctl, &src);
    ctl.defer_contacts = 1;                  /* no second anatomy run after a contact correction (lab: same sound) */
    ch_tract_apply(&tract, ctl.x, 0.0f);     /* (unslewed, as HumanVoice::new) */
    ch_wave_init(&wave, (float)VM_FS, VM_CTRL, tract.k, tract.lip_s);
    wave.fade = BODY_FADE[0];
    for (s = 0; s < NSUB; s++)
        control_step(&steps[0][s]);
    cur = 0;
    ch_live_init(&live);
    ch_beat_player_init(&seq);
    ch_cvoice_init(&cvoice, &CH_SEAL);
    ch_seal_init(&seal);
    ch_lts_device = 1;                       /* the FM-1's letter-to-sound additions (no room for the dictionary) */
}

/* main loop: the pending commands into the mailbox, when it is free */
static void mb_post(void)
{
    if (mb_full || (!pend.n && !pend.n_release && !pend.clear))
        return;
    mb = pend;
    pend.n = pend.n_release = 0;
    pend.clear = 0;
    pend.song = 0;
    mb_full = 1;
}

/* main loop: a live note's command after what is pending */
static void post_live(void)
{
    int i;
    for (i = 0; i < live_cmd.n_release && pend.n_release < MB_R; i++) {
        pend.release[pend.n_release] = live_cmd.release[i];
        pend.at[pend.n_release++] = (int16_t)pend.n;
    }
    for (i = 0; i < live_cmd.n && pend.n < MB_G; i++)
        pend.g[pend.n++] = live_cmd.g[i];
    mb_post();
}

static void live_on(int32_t note, uint32_t vel)
{
    if (bt.on) {                              /* the beatbox: a note is its key's pad */
        beat_note(note);
        return;
    }
    if (sg.on && (!sg.mel || sg.playing))    /* the SING page: the tune sings, not the keys (MELODY's keys sing
                                               * while it is stopped: a note to try, or one being written) */
        return;
    if (body_cur == BODY_SEAL) {              /* the seal: its pitch */
        seal_on(note);
        return;
    }
    if (vowel_mode) {
        vowel_on(note, vel);
        return;
    }
    live_cmd.n = live_cmd.n_release = 0;
    ch_live_note_on(&live, (float)note, (float)vel, &live_cmd);
    voice_note = note;
    post_live();
}

static void live_off(int32_t note)
{
    if (bt.on || (sg.on && (!sg.mel || sg.playing)))
        return;
    if (body_cur == BODY_SEAL) {
        seal_off(note);
        return;
    }
    if (vowel_mode) {
        vowel_off(note);
        return;
    }
    live_cmd.n = live_cmd.n_release = 0;
    ch_live_note_off(&live, (float)note, &live_cmd);
    if (!live.sounding)
        voice_note = -1;
    else
        voice_note = (int32_t)live.s_midi;
    post_live();
}

/* main loop: PLAY, the lyrics spoken (EnglishG2P::to_phrases, plan_phrases), from now; the keys' notes stop */
static void speak_text(const char *text)
{
    static ch_seg_t segs[CH_SEG_MAX];
    static ch_phrase_t ph[CH_PHRASE_MAX];
    ch_plan_opts_t o;
    ch_plan_t plan;
    int n_segs, np;
    np = ch_text_to_phrases(text, words_lookup, 0, segs, CH_SEG_MAX, &n_segs, ph, CH_PHRASE_MAX);
    ch_plan_defaults(&o);
    o.t0 = 0.05f;                            /* as CHIMERA's `render text` */
    o.f0 = SPEECH_F0;
    o.rate = speech_rate;
    plan.g = pend.g;                         /* (what was pending is dropped: the score is cleared anyway) */
    plan.cap = MB_G;
    plan.n = 0;
    plan.dropped = 0;
    ch_plan_phrases(segs, ph, np, &o, &plan);
    pend.n = plan.n;
    pend.n_release = 0;
    pend.clear = 1;
    live.sounding = 0;                       /* (the keys' notes are cleared with the score) */
    live.n_keys = 0;
    voice_note = -1;
    speaking = 1;
    mb_post();
}

/* audio interrupt: the mailbox into the score, now */
static void mb_take(void)
{
    int i, r = 0;
    if (!mb_full)
        return;
    if (mb.clear)
        ch_score_clear(&ctl.score);
    if (mb.song == 1) {
        song_clock0 = ctl.clock;
        song_on = 1;
    } else if (mb.clear) {                   /* (anything else that clears the score ends the song) */
        song_on = 0;
    }
    for (i = 0; i <= mb.n; i++) {
        while (r < mb.n_release && mb.at[r] <= i)
            ch_score_release(&ctl.score, mb.release[r++], ctl.clock);
        if (i < mb.n) {
            ch_gesture_t g = mb.g[i];
            const float base = mb.song ? song_clock0 : ctl.clock;
            g.t0 += base;
            g.t1 += base;
            ch_score_add(&ctl.score, &g);
        }
    }
    mb_full = 0;
}

/* The beatbox (0.7.0, SEQ): CHIMERA's step sequencer runs here in the audio interrupt, on the voice's clock, so its
 * hits are sample-accurate (engine/ch_beat.c); the main loop sends the beat (its steps, tempo, swing, playing) as one
 * struct and a flag, and the pads through a small ring. A pad sounds as soon as the mouth can make it (its lead-in) */
#define BEAT_STEPS 16
typedef struct {
    ch_beat_step_t s[BEAT_STEPS];
    float bpm, swing;
    uint8_t play;
} beat_cmd_t;
static beat_cmd_t bc_out;
static volatile uint8_t bc_dirty;
#define PQ 8u
static volatile uint32_t pq_head, pq_tail;
static volatile uint8_t pq_id[PQ], pq_note[PQ], pq_acc[PQ];
static volatile int8_t beat_now = -1;         /* the step under the playhead (shown), -1: stopped */
static volatile int8_t beat_sound = -1;       /* the sound heard last (shown): a step's or a pad's */
static volatile uint32_t beat_sound_at;       /* when, in blocks (beat_blocks: n_blk restarts with each timing
                                              * readout, every half second; the name flickered back and forth) */
static volatile uint32_t beat_blocks;         /* audio blocks since boot, for that */

/* audio interrupt: CHIMERA's close-mic chain for the beatbox (src/audio/AudioEngine.ts: a 120 Hz low shelf +9 dB, the
 * proximity effect; a 3.5 kHz presence peak +3 dB, Q 0.8; a fast compressor, -28 dB, knee 8, ratio 5, attack 2 ms,
 * release 120 ms, with WebAudio's own make-up gain; then x2.2), a sample at a time, a soft limit after it. The filters'
 * coefficients are WebAudio's biquads' at 44.1 kHz (lab: RBJ's cookbook) */
static float mic_z[4], mic_env, mic_gain = 1.0f;
static uint8_t mic_count;
static float mic(float x)
{
    static const float S[5] = {1.006353372f, -1.981182394f, 0.975315220f, -1.981339238f, 0.981511748f};
    static const float P[5] = {1.082903165f, -1.403469951f, 0.515178678f, -1.403469951f, 0.598081843f};
    float y, a;
    y = S[0] * x + mic_z[0];                  /* (transposed direct form II) */
    mic_z[0] = S[1] * x - S[3] * y + mic_z[1];
    mic_z[1] = S[2] * x - S[4] * y;
    x = y;
    y = P[0] * x + mic_z[2];
    mic_z[2] = P[1] * x - P[3] * y + mic_z[3];
    mic_z[3] = P[2] * x - P[4] * y;
    a = fm_fabsf(y);
    mic_env += (a - mic_env) * (a > mic_env ? 0.0112738f : 0.000188947f);
    if (++mic_count >= 8u) {                  /* the gain, every 8 samples: the knee's curve in dB */
        const float lvl = 6.0206f * fm_log2f(mic_env + 1e-9f), over = lvl + 28.0f;
        float gr = 0.0f, goal;
        mic_count = 0;
        if (over > 4.0f)
            gr = over * 0.8f;
        else if (over > -4.0f)
            gr = 0.8f * (over + 4.0f) * (over + 4.0f) / 16.0f;
        goal = fm_exp2f((13.44f + 6.85f - gr) / 6.0206f);   /* WebAudio's make-up (+13.4 dB here), then x2.2 */
        mic_gain += (goal - mic_gain) * 0.5f;
    }
    y *= mic_gain;
    a = fm_fabsf(y);
    if (a > 0.7f)                             /* the soft limit: nothing past 1 */
        y = (y < 0 ? -1.0f : 1.0f) * (0.7f + 0.3f * (1.0f - fm_exp2f(-(a - 0.7f) * 4.8f)));
    return y;
}

/* audio interrupt: the beat and the pads, now */
static void beat_take(void)
{
    if (bc_dirty) {
        ch_beat_set_steps(&seq, bc_out.s, BEAT_STEPS);
        ch_beat_set_tempo(&seq, bc_out.bpm, bc_out.swing);
        if (bc_out.play && !seq.playing)
            ch_beat_start(&seq, ctl.clock);
        else if (!bc_out.play && seq.playing)
            ch_beat_stop(&seq);
        bc_dirty = 0;
    }
    while (pq_tail != pq_head) {
        const uint32_t i = pq_tail % PQ;
        const int id = pq_id[i];
        ch_gesture_t g[CH_BEAT_MAX_G];
        ch_plan_t o;
        int j;
        o.g = g;
        o.cap = CH_BEAT_MAX_G;
        o.n = 0;
        o.dropped = 0;
        ch_beat_build(id, ch_midi_hz((float)(pq_note[i] ? pq_note[i] : CH_BEAT_NOTE)), 0.18f, pq_acc[i] ? CH_BEAT_ACCENT : 1.0f,
                      ctl.clock + ch_beat_lead_in(id), &o);
        for (j = 0; j < o.n; j++)
            ch_score_add(&ctl.score, &g[j]);
        beat_sound = (int8_t)id;
        beat_sound_at = beat_blocks;
        pq_tail++;
    }
}

/* audio interrupt: the steps whose hits come within the horizon, into the score; the playhead for the screen */
static void beat_tick(void)
{
    int s;
    if (!seq.playing) {
        beat_now = -1;
        return;
    }
    ch_beat_tick(&seq, ctl.clock, &ctl.score);
    s = ch_beat_step_at(&seq, ctl.clock);
    if (s != beat_now && s >= 0 && seq.steps[s].id >= 0) {
        beat_sound = seq.steps[s].id;
        beat_sound_at = beat_blocks;
    }
    beat_now = (int8_t)s;
}

static void nq_push(uint32_t note, uint32_t vel)   /* audio interrupt */
{
    uint32_t h = nq_head;
    if (h - nq_tail >= NQ)
        return;
    nq_note[h % NQ] = (uint8_t)note;
    nq_vel[h % NQ] = (uint8_t)vel;
    nq_head = h + 1;
}

static uint32_t midi_held[4];                /* main loop: MIDI in's notes down (128 bits), lit on the keys */

static void nq_run(void)                     /* main loop: MIDI in's notes */
{
    while (nq_tail != nq_head) {
        uint32_t t = nq_tail, n = nq_note[t % NQ] & 0x7Fu;
        if (nq_vel[t % NQ]) {
            midi_held[n >> 5] |= 1u << (n & 31u);
            live_on((int32_t)n, nq_vel[t % NQ]);
        } else {
            midi_held[n >> 5] &= ~(1u << (n & 31u));
            live_off((int32_t)n);
        }
        nq_tail = t + 1;
    }
}

static uint32_t midi_keys(int32_t octave)    /* the keys MIDI in's held notes fall on, at this octave */
{
    uint32_t m = 0, k;
    for (k = 0; k < NKEYS; k++) {
        int32_t n = 53 + (int32_t)k + 12 * octave;
        if (n >= 0 && n < 128 && (midi_held[n >> 5] >> (n & 31) & 1u))
            m |= 1u << k;
    }
    return m;
}

static volatile uint32_t midi_last, midi_last_port, midi_count;   /* the last message in (shown), and how many */
static void midi_in(void)                    /* audio interrupt: the packets that came in */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (st != 0xF8u && st != 0xFEu) {    /* (clock and active sensing would flood the screen) */
            midi_last = pkt;
            midi_last_port = (uint32_t)port;
            midi_count++;
        }
        if ((st & 0xF0u) == 0x90u && d2)
            nq_push(d1, d2);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            nq_push(d1, 0);
        else if ((st & 0xF0u) == 0xE0u)          /* pitch bend */
            midi_bend = (int16_t)((int32_t)(d1 | (d2 & 0x7Fu) << 7) - 8192);
        else if ((st & 0xF0u) == 0xB0u && d1 == 1u)   /* the mod wheel */
            midi_mod = (uint8_t)(d2 & 0x7Fu);
        else if ((st & 0xF0u) == 0xD0u)          /* channel pressure */
            midi_prs = (uint8_t)d1;
        else if ((st & 0xF0u) == 0xA0u)          /* poly aftertouch (the voice is one: the last one counts) */
            midi_prs = (uint8_t)(d2 & 0x7Fu);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, t0, c0;
    int s;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f * body_gain_audio;   /* MASTER, read once a block */
    int on2;
    midi_in();
    beat_blocks++;
    mb_take();
    vp_take();
    beat_take();
    cm_take();
    if (body_req != body_audio || (body_req == BODY_SEAL && cre_req != cre_audio)
        || (body_req != BODY_SEAL && mix_seq != mix_seq_audio)) {   /* another body (or blend) (CHIMERA's set_body): its anatomy, the waveguide anew. Here, as
                                              * the block starts, the second core is idle; this block's shapes were the
                                              * old body's, so it renders none; the sound fades back in (gain_now) */
        int s2;
        body_audio = body_req;
        seal_audio = body_audio == BODY_SEAL;
        bird_audio = 0;
        cre_div = 1;
        body_gain_audio = 1.0f;               /* (a creature's level is its output gain) */
        if (seal_audio) {                     /* the Bestiary: its creature's voice anew */
            cre_audio = cre_req;
            ch_cvoice_init(&cvoice, CRE_DEF[cre_audio]);
            cm_seen = cm_out.call_seq;        /* (no call carried over) */
            cm_dirty = 1;                     /* (its settings again, into the new voice) */
        }
        if (seal_audio && cre_audio == CRE_BIRD) {   /* the songbird: its airway at rest */
            ch_bird_init(&bird, &cvoice);
            bird_audio = 1;
            for (s2 = 0; s2 < NSUB; s2++)
                steps[cur][s2].has_shape = 0;
            gain_now = 0.0f;
            goto switched;
        }
        if (seal_audio && (cre_audio == CRE_GATOR || cre_audio == CRE_ELEPHANT)) {   /* at half the rate, interpolated */
            if (cre_audio == CRE_GATOR)
                ch_gator_wave(&wave, (float)VM_FS, VM_CTRL);
            else
                ch_elephant_wave(&wave, (float)VM_FS, VM_CTRL);
            wave.output_gain *= CRE_LEVEL;
            cre_div = (uint8_t)(cre_audio == CRE_GATOR ? CH_GATOR_DIV : CH_ELE_DIV);
            ch_up_init(&cre_up, cre_div, 0.9f);
            for (s2 = 0; s2 < NSUB; s2++)
                steps[cur][s2].has_shape = 0;
            gain_now = 0.0f;
            goto switched;
        }
        if (seal_audio && (cre_audio == CRE_CAT || cre_audio == CRE_ROBOT || cre_audio == CRE_SQUID)) {   /* the cat:
                                              * its small airway and nose; the robot, Squidlad: their tubes; at the full
                                              * rate */
            if (cre_audio == CRE_CAT) {
                cat_wave(&wave, (float)VM_FS, VM_CTRL);
                cat_init(&cat);
            } else if (cre_audio == CRE_ROBOT) {
                robot_wave(&wave, (float)VM_FS, VM_CTRL);
                robot_init(&robot);
            } else {
                squid_wave(&wave, (float)VM_FS, VM_CTRL);
                squid_init(&squid);
            }
            for (s2 = 0; s2 < NSUB; s2++)
                steps[cur][s2].has_shape = 0;
            gain_now = 0.0f;
            goto switched;
        }
        if (seal_audio) {                     /* the seal: its waveguide (CHIMERA's mammal glottal port, its output gain
                                               * x2.2: the balance against the human voice CHIMERA's renders have, the
                                               * FM-1's voice being 4 dB over CHIMERA's and its seal 3 dB under) */
            ch_wave_init(&wave, (float)VM_FS, VM_CTRL, 1.0f, CH_SEAL_LEN);
            wave.output_gain = CH_SEAL_GAIN * 2.2f;
            wave.port_len = 0.4f;
            wave.port_slit = 1.8f;
            wave.wall_rad = 0.6f;
            ch_seal_init(&seal);
            for (s2 = 0; s2 < NSUB; s2++)
                steps[cur][s2].has_shape = 0;
            gain_now = 0.0f;
            goto switched;
        }
        {   /* the body the main loop holds (a blend: its dimensions, loss and level between the bodies') */
            static bodymix_t m;
            mix_seq_audio = mix_seq;
            m = mix_req;
            body_tract_setup(&tract, &m.b);
            ctl.k = m.b.scale;
            ch_tract_apply(&tract, ctl.x, 0.0f);
            ch_wave_init(&wave, (float)VM_FS, VM_CTRL, tract.k, tract.lip_s);
            wave.fade = m.fade;
            body_gain_audio = m.gain;
        }
        for (s2 = 0; s2 < NSUB; s2++)
            steps[cur][s2].has_shape = 0;
        gain_now = 0.0f;
    switched:;
    }
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    /* At rest (the score empty, the breath gone, the sound died away for QUIET_BLOCKS blocks) nothing is simulated:
     * the output is silence and the voice keeps its state as it was. A note wakes it: the first block renders from
     * the targets of the block before the rest (one block, 5.8 ms, before the new ones arrive). v0.0.1 / v0.0.2 ran
     * the whole voice in silence too: on the FM-1 that kept the audio overloaded (a constant sound). */
    beat_tick();                             /* (before the rest: a beat playing wakes the voice and keeps it awake) */
    song_now = song_on ? ctl.clock - song_clock0 : -1.0f;
    if (resting && (seal_audio ? cvoice.gate_target <= 0.0f && cvoice.call < 0
                               : ctl.score.n == 0 && ctl.gate_target <= 0.0f && !seq.playing && !song_on)) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        n_blk++;                                 /* (a resting block costs nothing: it counts as 0 %) */
        return;
    }
    resting = 0;
    /* the waveguide for this block on the second core; the next block's control steps here, meanwhile */
    on2 = two_cores && plat_cpu2_on() && plat_cpu2_run(tract_job, (uint32_t)cur);
    t0 = plat_ticks24();
    for (s = 0; s < NSUB; s++)
        control_step(&steps[cur ^ 1][s]);
    c0 = plat_ticks24() - t0;
    if (!on2 || plat_cpu2_wait() != 0)
        PLAT_FAR(tract_job)((uint32_t)cur);   /* one core: the waveguide here too */
    cur ^= 1;
    c0_sum += c0;
    c1_sum += c1_last;
    if (c0 > c0_peak)
        c0_peak = c0;
    if (c1_last > c1_peak)
        c1_peak = c1_last;
    n_blk++;
    if (speaking && ctl.score.n == 0)
        speaking = 0;
    {
        float peak = 0.0f;
        for (i = 0; i < n; i++) {
            float x;
            gain_now += (gain - gain_now) * 0.004f;              /* ~5 ms: turning MASTER does not click */
            x = (bt.on ? mic(vbuf[i]) : vbuf[i]) * gain_now;   /* (the beatbox: the close mic) */
            lr[2 * i] = (int32_t)x;
            lr[2 * i + 1] = (int32_t)x;
            if (fm_fabsf(vbuf[i]) > peak)
                peak = fm_fabsf(vbuf[i]);
        }
#ifdef PLAT_HOST
        {   /* (the simulator: say when the voice stops being a number, and what the control was doing) */
            static int told;
            int bad = 0;
            for (i = 0; i < n; i++)
                bad |= !(vbuf[i] == vbuf[i] && vbuf[i] < 1e30f && vbuf[i] > -1e30f);
            if (!told && bad) {
                told = 1;
                int k;
                fprintf(stderr, "voice not finite at %.3f s: p_lung %g f0 %g glottis f0 %g gate %g score %d gestures (%u dropped)\n  x",
                        (double)ctl.clock, (double)ctl.p_lung, (double)ctl.f0, (double)ctl.glottis.f0, (double)ctl.gate,
                        ctl.score.n, ctl.score.dropped);
                for (k = 0; k < CH_NARTIC; k++)
                    fprintf(stderr, " %g", (double)ctl.x[k]);
                fprintf(stderr, "\n  glottis");
                for (k = 0; k < 17; k++)
                    fprintf(stderr, " %g", (double)(&ctl.glottis.f0)[k]);
                fprintf(stderr, "\n  wave: u_g %g p_supra %g a_g %g vol_u %g wall_v %g jets %d shut %d\n", (double)wave.u_g,
                        (double)wave.p_supra, (double)wave.a_g, (double)wave.vol_u, (double)wave.wall_v, wave.n_jet, wave.n_shut);
            }
        }
#endif
        /* rest once the score has played out, the breath gone and the output below 16-bit silence for a while */
        if ((seal_audio ? cvoice.gate_target <= 0.0f && cvoice.call < 0 && cvoice.gate < 1e-4f
                        : ctl.score.n == 0 && !mb_full && ctl.gate_target <= 0.0f && ctl.gate < 1e-4f && !seq.playing && !song_on)
            && peak < 2e-6f) {
            if (++quiet_blocks >= QUIET_BLOCKS)
                resting = 1;
        } else {
            quiet_blocks = 0;
        }
    }
}

void app_panic(void)
{
    voice_note = -1;
    ch_score_clear(&ctl.score);
    ctl.gate = 0.0f;
}

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.wave = 0;
    set.slot = 0;
    set.preset = 0;
    set.ble_off = 0;
    set.kb = 0;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2 || set.wave > 1u || set.ble_off > 1u)
        settings_defaults();
    if (set.slot >= 8u)
        set.slot = 0;
    if (set.preset >= NPRESETS)
        set.preset = 0;
    if (set.kb > 1u)
        set.kb = 0;
    voice_init();
    words_load();
    words_select(set.slot);
    presets_load();
    preset_apply(set.preset);
    beats_load();
    creatures_load();
    song_load();
    melody_load();
    sg.on = sg.playing = sg.dirty = sg.mel = sg.edit = sg.cur = 0;
    bt.on = bt.edit = bt.play = bt.dirty = bt.slot = bt.cur = 0;
    two_cores = plat_cpu2_set(1) == 0;       /* else one core: the waveguide runs here too */
}

/* ---------------------------------------------------------------------- words --- */
/* The word slots, as the Studio sends them (app/web/studio.html) and the store keeps them (OBJ_WORDS): a text
 * document, ASCII, at most PLAT_STORE_MAX bytes:
 *   VOCALWORDS 1                   the header
 *   1 Harder, better, faster       a slot (1..8) and its words
 *   @stronger S T R AO1 NG ER0     a word's pronunciation (CMUdict's ARPAbet), asked before the built-in table
 * The SysEx, F0 7D 56 4F cmd ... F7, 7-bit (numbers 7 bits a byte, most significant first):
 *   01            hello      -> 01 proto(1) max(3) length(3) version...
 *   02 len(3)     begin a document of len bytes        -> 7F 02 status
 *   03 off(3) b.. its bytes at off (in order, <= 512)  -> 7F 03 status
 *   04 sum(3)     the sum of its bytes (21 bits): check, save, sing slot 1  -> 7F 04 status
 *   05 off(3) n(2) read the document saved             -> 05 off(3) bytes
 *   06 slot       sing slot (0-based)                   -> 7F 06 status
 *   07 slot       sing slot and speak it now (PLAY)     -> 7F 07 status
 * status 0 ok, 1 out of range / order, 2 bad sum, 3 not a words document, 4 the flash refused it */
#define WORDS_SLOTS 8
#define WORDS_MAX PLAT_STORE_MAX
#define WORDS_PROTO 1
static char words_doc[WORDS_MAX + 1] PLAT_POOL, words_rx[WORDS_MAX + 1] PLAT_POOL;
static uint32_t words_len, rx_len, rx_got;
static const char *slot_text[WORDS_SLOTS];
static uint16_t slot_len[WORDS_SLOTS];
static const char *pron[256];             /* the "@word ARPA" lines */
static int n_pron, n_slots;

/* a document: its header and that it is printable ASCII lines */
static int words_valid(const char *d, uint32_t n)
{
    uint32_t i;
    if (n < 12 || memcmp(d, "VOCALWORDS 1", 12))
        return 0;
    for (i = 0; i < n; i++)
        if (!((d[i] >= 32 && d[i] < 127) || d[i] == '\n'))
            return 0;
    return 1;
}

/* index the document in words_doc: its slots and its pronunciations */
static void words_index(void)
{
    const char *p = words_doc, *end = words_doc + words_len;
    int i;
    for (i = 0; i < WORDS_SLOTS; i++) {
        slot_text[i] = 0;
        slot_len[i] = 0;
    }
    n_pron = n_slots = 0;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n')
            e++;
        if (*p == '@' && n_pron < 256) {
            pron[n_pron++] = p + 1;
        } else if (*p >= '1' && *p <= '0' + WORDS_SLOTS && p + 1 < e && p[1] == ' ') {
            i = *p - '1';
            slot_text[i] = p + 2;
            slot_len[i] = (uint16_t)(e - p - 2);
            if (i + 1 > n_slots)
                n_slots = i + 1;
        }
        p = e + 1;
    }
}

/* ch_speech's lookup: a pronunciation the document gives (copied out, the ARPAbet up to the line's end) */
static const char *words_lookup(const char *w, void *user)
{
    static char out[96];
    uint32_t n = 0;
    int i;
    (void)user;
    while (w[n])
        n++;
    for (i = 0; i < n_pron; i++) {
        const char *q = pron[i], *e;
        uint32_t k, j;
        for (j = 0; j < n && q[j] && q[j] == w[j]; j++)
            ;
        if (j < n || q[n] != ' ')
            continue;
        q += n + 1;
        for (e = q; *e && *e != '\n'; e++)
            ;
        k = (uint32_t)(e - q) < sizeof out - 1 ? (uint32_t)(e - q) : sizeof out - 1;
        memcpy(out, q, k);
        out[k] = 0;
        return out;
    }
    return 0;
}

/* the keys sing slot i (an empty one: the first that has words) */
static void words_select(int i)
{
    uint32_t n;
    if (i < 0 || i >= WORDS_SLOTS || !slot_text[i])
        for (i = 0; i < WORDS_SLOTS && !slot_text[i]; i++)
            ;
    if (i >= WORDS_SLOTS) {
        lyric[0] = 0;
        i = 0;
    } else {
        n = slot_len[i] < sizeof lyric - 1 ? slot_len[i] : sizeof lyric - 1;
        memcpy(lyric, slot_text[i], n);
        lyric[n] = 0;
    }
    set.slot = (uint8_t)i;
    ch_live_lyrics(&live, lyric, words_lookup, 0);
}

static void words_load(void)
{
    int n = plat_store_load(OBJ_WORDS, words_doc, WORDS_MAX);
    if (n > 0 && words_valid(words_doc, (uint32_t)n)) {
        words_len = (uint32_t)n;
    } else {
        words_len = sizeof WORDS_DEFAULT - 1;
        memcpy(words_doc, WORDS_DEFAULT, words_len);
    }
    words_doc[words_len] = 0;
    words_index();
}

static uint32_t sx_num(const uint8_t *p, int n)    /* 7 bits a byte, most significant first */
{
    uint32_t v = 0;
    while (n--)
        v = v << 7 | (*p++ & 0x7Fu);
    return v;
}
static void sx_put(uint8_t *p, uint32_t v, int n)
{
    while (n--) {
        p[n] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
    }
}
static void sx_status(uint8_t cmd, uint8_t st)
{
    uint8_t f[8] = {0xF0, 0x7D, 0x56, 0x4F, 0x7F, cmd, st, 0xF7};
    plat_sysex_send(f, 8);
}

int app_sysex(const uint8_t *p, uint32_t n)
{
    static uint8_t f[600];
    uint8_t cmd;
    if (n < 4 || p[0] != 0x7D || p[1] != 0x56 || p[2] != 0x4F)
        return 0;
    cmd = p[3];
    p += 4;
    n -= 4;
    switch (cmd) {
    case 0x01: {                             /* hello */
        static const char VER[] = APP_VERSION;
        uint32_t k = 0, i;
        f[k++] = 0xF0; f[k++] = 0x7D; f[k++] = 0x56; f[k++] = 0x4F; f[k++] = 0x01; f[k++] = WORDS_PROTO;
        sx_put(f + k, WORDS_MAX, 3);
        k += 3;
        sx_put(f + k, words_len, 3);
        k += 3;
        for (i = 0; VER[i] && i < 24; i++)
            f[k++] = (uint8_t)(VER[i] & 0x7F);
        f[k++] = 0xF7;
        plat_sysex_send(f, k);
        break;
    }
    case 0x02:                               /* begin */
        if (n < 3 || (rx_len = sx_num(p, 3)) > WORDS_MAX) {
            rx_len = 0;
            sx_status(cmd, 1);
            break;
        }
        rx_got = 0;
        sx_status(cmd, 0);
        break;
    case 0x03: {                             /* data, in order */
        uint32_t off, k;
        if (n < 3 || (off = sx_num(p, 3)) != rx_got || off + (n - 3) > rx_len) {
            sx_status(cmd, 1);
            break;
        }
        for (k = 0; k < n - 3; k++)
            words_rx[off + k] = (char)(p[3 + k] & 0x7F);
        rx_got += n - 3;
        sx_status(cmd, 0);
        break;
    }
    case 0x04: {                             /* commit: check, save, sing */
        uint32_t sum = 0, k;
        if (n < 3 || rx_got != rx_len || !rx_len) {
            sx_status(cmd, 1);
            break;
        }
        for (k = 0; k < rx_len; k++)
            sum += (uint8_t)words_rx[k];
        if ((sum & 0x1FFFFFu) != sx_num(p, 3)) {
            sx_status(cmd, 2);
            break;
        }
        if (!words_valid(words_rx, rx_len)) {
            sx_status(cmd, 3);
            break;
        }
        if (plat_store_save(OBJ_WORDS, words_rx, rx_len) != 0) {
            sx_status(cmd, 4);
            break;
        }
        memcpy(words_doc, words_rx, rx_len);
        words_len = rx_len;
        words_doc[words_len] = 0;
        rx_len = rx_got = 0;
        words_index();
        words_select(0);
        set_dirty = 1;
        app_say("WORDS", "RECEIVED");
        sx_status(cmd, 0);
        break;
    }
    case 0x05: {                             /* read */
        uint32_t off, len, k = 0, i;
        if (n < 5 || (off = sx_num(p, 3)) > words_len) {
            sx_status(cmd, 1);
            break;
        }
        len = sx_num(p + 3, 2);
        if (len > 512)
            len = 512;
        if (off + len > words_len)
            len = words_len - off;
        f[k++] = 0xF0; f[k++] = 0x7D; f[k++] = 0x56; f[k++] = 0x4F; f[k++] = 0x05;
        sx_put(f + k, off, 3);
        k += 3;
        for (i = 0; i < len; i++)
            f[k++] = (uint8_t)(words_doc[off + i] & 0x7F);
        f[k++] = 0xF7;
        plat_sysex_send(f, k);
        break;
    }
    case 0x06:                               /* sing slot */
    case 0x07:                               /* sing slot, and speak it */
        if (n < 1 || p[0] >= WORDS_SLOTS || !slot_text[p[0]]) {
            sx_status(cmd, 1);
            break;
        }
        words_select(p[0]);
        set_dirty = 1;
        if (cmd == 0x07)
            speak_text(lyric);
        sx_status(cmd, 0);
        break;
    default:
        sx_status(cmd, 1);
        break;
    }
    return 1;
}

/* ------------------------------------------------------- knobs, pages, presets --- */
/* K1..K4 are the page's four values (CHIMERA's controls, its ranges: src/ui/components/VoiceControls.tsx, SingPanel.tsx);
 * SELECT turns the page, HOME is WORDS and page 1, SEL steps pages 2-5, FX is VOWEL (the keys play pitch, the knobs
 * move through CHIMERA's vowel pad). ALGORITHM is Body (Bright on page 5's K4 from 0.5.0; they were the other way round before). PRESET picks a voice preset, loaded when the knob rests; SAVE
 * keeps the knobs in it (and the vowel snapshots). HOME held + PRESET picks the word slot. */
enum {
    K_VIB, K_RATE, K_BREATHY, K_RING,         /* 1 SING */
    K_JAW, K_LIPS, K_LARYNX, K_VELUM,         /* 2 SHAPE */
    K_ROUND, K_PROT, K_ROOT, K_CURL,          /* 3 MOUTH */
    K_BREATH, K_PRESSED, K_ROUGH, K_FRY,      /* 4 SOURCE */
    K_GLIDE, K_SPEED, K_FALSE, K_BODY,        /* 5 PLAY: K4 is Bright from 0.5.0 (the page table); Body on ALGORITHM */
    K_BRIGHT,                                 /* (the order is the saved presets': kept) */
    K_FRONT, K_OPEN,                          /* VOWEL (with K_VELUM as Nasal, K_ROUND as Rounding) */
    K_GENDER, K_AGE,                          /* BLENDED's blend (1.0.0, SEL held + PRESET / SELECT: CHIMERA's BodyMix) */
    K_TBX, K_TBY, K_TTX, K_TTY,               /* TONGUE (1.0.0, ARP): the tongue body and tip placed (cm) */
    NK
};
enum { F_PCT, F_CM, F_SCM, F_CENTS, F_HZ, F_MS, F_X, F_VELUM, F_BODY, F_F2, F_F1 };
typedef struct {
    const char *name;
    float min, max, def, step;                /* step: a detent's change (log knobs: its ratio) */
    uint8_t fmt, log;
} knob_t;
static const knob_t KNOB[NK] = {
    {"VIBRATO", 0, 80, 35, 1, F_CENTS, 0},   {"RATE", 3.5f, 7.5f, 6, 0.1f, F_HZ, 0},
    {"BREATHY", 0, 1, 0.15f, 0.02f, F_PCT, 0}, {"RING", 0, 1, 0, 0.02f, F_PCT, 0},
    {"JAW", 0, 2.4f, 0.6f, 0.05f, F_CM, 0},  {"LIPS", -0.3f, 2.2f, 0.7f, 0.05f, F_CM, 0},
    {"LARYNX", -1.5f, 1.5f, 0, 0.05f, F_SCM, 0}, {"PALATE", -0.25f, 1.2f, -0.15f, 0.025f, F_VELUM, 0},
    {"ROUND", 0, 1, 0.5f, 0.02f, F_PCT, 0},  {"PROTRUDE", -0.1f, 1.4f, 0.2f, 0.05f, F_CM, 0},
    {"ROOT", -0.9f, 0.9f, 0, 0.05f, F_SCM, 0}, {"CURL", 0, 1, 0, 0.02f, F_PCT, 0},
    {"BREATH", 2, 22, 8, 0.25f, F_X, 0},      {"PRESSED", 0, 1, 0.5f, 0.02f, F_PCT, 0},
    {"ROUGH", 0, 1, 0.1f, 0.02f, F_PCT, 0},  {"FRY", 0, 1, 0, 0.02f, F_PCT, 0},
    {"GLIDE", 0.005f, 0.6f, 0.06f, 1.08f, F_MS, 1}, {"SPEED", 0.5f, 1.8f, 1, 0.05f, F_X, 0},
    {"F.FOLDS", 0, 1, 0, 0.02f, F_PCT, 0},   {"BODY", 0, 3, 0, 1, F_BODY, 0},   /* (3: BLENDED, from 1.0.0) */
    {"BRIGHT", 0, 1, 0.25f, 0.02f, F_PCT, 0},
    {"FRONT", 0, 1, 0.45f, 0.02f, F_F2, 0},  {"OPEN", 0, 1, 0.35f, 0.02f, F_F1, 0},
    {"GENDER", 0, 1, 0.5f, 0.05f, F_PCT, 0}, {"AGE", 0, 1, 0, 0.05f, F_PCT, 0},   /* (BLENDED's own: half male, half female) */
    /* (the tongue: CHIMERA's neutral the start; the ranges a little past where its English puts it: the body
     * 1.25-3.8 cm front, 5.7-8.5 high, the tip 5.35-6.85 forward, 8.2-10 up) */
    {"FORWARD", 1.0f, 4.2f, 2.9f, 0.05f, F_CM, 0}, {"HEIGHT", 5.3f, 8.8f, 6.85f, 0.05f, F_CM, 0},
    {"TIP FWD", 5.0f, 7.3f, 6.45f, 0.05f, F_CM, 0}, {"TIP UP", 7.0f, 10.4f, 7.3f, 0.05f, F_CM, 0},
};
#define NPAGES 7
#define PAGE_VOWEL 5
#define PAGE_TONGUE 6
static const uint8_t PAGE[NPAGES][4] = {
    {K_VIB, K_RATE, K_BREATHY, K_RING}, {K_JAW, K_LIPS, K_LARYNX, K_VELUM}, {K_ROUND, K_PROT, K_ROOT, K_CURL},
    {K_BREATH, K_PRESSED, K_ROUGH, K_FRY}, {K_GLIDE, K_SPEED, K_FALSE, K_BRIGHT}, {K_FRONT, K_OPEN, K_VELUM, K_ROUND},
    {K_TBX, K_TBY, K_TTX, K_TTY},
};
static const char *const PAGE_NAME[NPAGES] = {"SING", "SHAPE", "MOUTH", "SOURCE", "PLAY", "VOWEL", "TONGUE"};
/* the pages SELECT turns through in each mode: WORDS, VOWEL, TONGUE (its own first, then the mouth's, then the voice's) */
static const uint8_t PAGES_W[] = {0, 1, 2, 3, 4}, PAGES_V[] = {0, 1, 2, 3, 4, 5}, PAGES_T[] = {6, 1, 2, 3, 0, 4};
/* the vowel pad's formant ranges (the adult male's: CHIMERA's VowelMap), log scales; FRONT 0 is the front (F2 high) */
#define PAD_F1_LO 230.0f
#define PAD_F1_HI 950.0f
#define PAD_F2_LO 700.0f
#define PAD_F2_HI 2700.0f
/* (a female's or a child's vowels sit higher: the ranges scale by the body's calibrated vowels, so a place on the pad,
 * and a snapshot, is the same vowel in any body) */
static float pad_f1(float open) { return mix_main.b.pad_f1 * PAD_F1_LO * fm_exp2f(open * fm_log2f(PAD_F1_HI / PAD_F1_LO)); }
static float pad_f2(float front) { return mix_main.b.pad_f2 * PAD_F2_HI * fm_exp2f(-front * fm_log2f(PAD_F2_HI / PAD_F2_LO)); }

/* the voice presets (this firmware's own: CHIMERA has none) and the vowel snapshots, as the store keeps them */
#define NSNAP 12
#define PRESETS_MAGIC 0x46565031u             /* "FVP1" (fm-1-chimera-vocal's CHP1..CHP3 are not read: its presets stay
                                               * its own) */
typedef struct {
    uint32_t magic;
    float v[NPRESETS][NK];
    float snap[NSNAP][4];                     /* FRONT, OPEN, PALATE (nasal), ROUND */
} presets_t;
static presets_t presets PLAT_POOL;
static const char *const PRESET_NAME[NPRESETS] = {"NATURAL", "BREATHY", "BELT", "OPERA", "AIRY", "ROUGH", "FRY", "TWANG"};
static float kv[NK];                          /* the knobs now */
static uint8_t page, vowel_mode;
static int8_t preset_pending = -1;            /* PRESET turned: the preset that loads when it rests */
static int8_t body_pending = -1;              /* ALGORITHM turned: the body it switches to when it rests */
static uint32_t body_turned_at;
static uint32_t preset_turned_at;

static void preset_factory(int i, float *v)
{
    int k;
    for (k = 0; k < NK; k++)
        v[k] = KNOB[k].def;
    switch (i) {
    case 1: v[K_BREATHY] = 0.7f; v[K_BREATH] = 6; v[K_PRESSED] = 0.3f; v[K_BRIGHT] = 0.15f; break;            /* BREATHY */
    case 2: v[K_BREATH] = 14; v[K_PRESSED] = 0.75f; v[K_RING] = 0.6f; v[K_BRIGHT] = 0.55f; v[K_JAW] = 0.9f; break;   /* BELT */
    case 3: v[K_VIB] = 60; v[K_RATE] = 5.5f; v[K_RING] = 0.8f; v[K_LARYNX] = -0.6f; v[K_PROT] = 0.5f;               /* OPERA */
            v[K_ROUND] = 0.65f; v[K_BREATH] = 12; v[K_BRIGHT] = 0.4f; break;
    case 4: v[K_BREATHY] = 1; v[K_BREATH] = 5; v[K_PRESSED] = 0.1f; v[K_BRIGHT] = 0.1f; v[K_ROUGH] = 0; break;     /* AIRY */
    case 5: v[K_ROUGH] = 0.65f; v[K_FRY] = 0.25f; v[K_BREATH] = 10; v[K_PRESSED] = 0.6f; break;                    /* ROUGH */
    case 6: v[K_FRY] = 0.8f; v[K_BREATH] = 4; v[K_PRESSED] = 0.65f; v[K_VIB] = 0; break;                          /* FRY */
    case 7: v[K_RING] = 0.95f; v[K_BRIGHT] = 0.8f; v[K_LARYNX] = 0.5f; v[K_PRESSED] = 0.6f; v[K_JAW] = 0.4f; break; /* TWANG */
    default: break;                                                                                            /* NATURAL */
    }
}

/* the snapshots before any is stored: twelve English vowels, where the pad has them (their calibrated formants), their
 * own rounding */
static void snap_factory(void)
{
    static const char *const V[NSNAP] = {"i", "ɪ", "e", "ɛ", "æ", "ɑ", "ɔ", "o", "ʊ", "u", "ʌ", "ə"};
    int i;
    for (i = 0; i < NSNAP; i++) {
        float f1 = 500, f2 = 1500, x[CH_NARTIC];
        ch_vowel_formants(V[i], &f1, &f2);
        presets.snap[i][0] = fm_clampf(fm_log2f(PAD_F2_HI / f2) / fm_log2f(PAD_F2_HI / PAD_F2_LO), 0, 1);
        presets.snap[i][1] = fm_clampf(fm_log2f(f1 / PAD_F1_LO) / fm_log2f(PAD_F1_HI / PAD_F1_LO), 0, 1);
        presets.snap[i][2] = KNOB[K_VELUM].def;
        x[CH_LIP_WIDTH] = 0.5f;
        ch_vowel_pad(f1, f2, x);
        presets.snap[i][3] = fm_clampf(1.0f - x[CH_LIP_WIDTH], 0, 1);
    }
}

static void presets_load(void)
{
    int i, n;
    for (i = 0; i < NK; i++)                  /* the knobs a preset leaves (the vowel pad's place) start at theirs */
        kv[i] = KNOB[i].def;
    n = plat_store_load(OBJ_PRESETS, &presets, sizeof presets);
    if (n == (int)sizeof presets && presets.magic == PRESETS_MAGIC)
        return;
    presets.magic = PRESETS_MAGIC;
    for (i = 0; i < NPRESETS; i++)
        preset_factory(i, presets.v[i]);
    snap_factory();
}

/* the voice's parameters from the knobs, the mode and the keys, into the audio interrupt (it takes them at its next
 * block: vp_take). The mailbox is one struct and a flag: the main loop fills it, the interrupt copies it out */
typedef struct {
    ch_source_t src;
    float target[CH_NARTIC];
    float gate;
    uint8_t tip_held;                         /* TONGUE: the tip where its knobs put it (else it rests behind the teeth) */
} vparams_t;
static vparams_t vp_out;
static volatile uint8_t vp_dirty;
static int32_t vkeys[16];                     /* VOWEL: the keys down, oldest first (MIDI notes) */
static int n_vkeys;
static float vowel_breath = 1.0f;             /* the last VOWEL key's breath (its velocity) */

static void vp_send(void)
{
    vparams_t p;
    int k;
    p.src = ctl.source;                       /* (the fields the knobs do not set: as they are) */
    p.src.glide = kv[K_GLIDE];
    p.src.pressure = kv[K_BREATH];
    p.src.breathiness = kv[K_BREATHY] + (1.0f - kv[K_BREATHY]) * (float)midi_mod / 127.0f;   /* + the mod wheel */
    p.src.tension = kv[K_PRESSED];
    p.src.fry = kv[K_FRY];
    p.src.roughness = kv[K_ROUGH];
    p.src.vibrato_rate = kv[K_RATE];
    p.src.brightness = kv[K_BRIGHT];
    p.src.vibrato_depth = vowel_mode ? kv[K_VIB] : 0.0f;   /* sung notes carry their own vibrato (ch_live) */
    p.src.voiced = 1;
    p.src.f0 = vowel_mode && n_vkeys ? 440.0f * fm_exp2f((float)(vkeys[n_vkeys - 1] - 69) / 12.0f)
                                     : vowel_mode ? ctl.source.f0 : SPEECH_F0;
    for (k = 0; k < CH_NARTIC; k++)
        p.target[k] = CH_NEUTRAL[k];
    if (vowel_mode == 1)                      /* the vowel pad: the tongue, the jaw, the lips, the root, the larynx */
        ch_vowel_pad(pad_f1(kv[K_OPEN]), pad_f2(kv[K_FRONT]), p.target);
    else {
        p.target[CH_JAW] = kv[K_JAW];
        p.target[CH_LIP_AP] = kv[K_LIPS];
        p.target[CH_LARYNX] = kv[K_LARYNX];
        p.target[CH_LIP_PROT] = kv[K_PROT];
        p.target[CH_ROOT] = kv[K_ROOT];
        p.target[CH_TT_CURL] = kv[K_CURL];
    }
    p.tip_held = vowel_mode == 2;
    if (p.tip_held) {                         /* TONGUE: the body and the tip where the knobs put them (CHIMERA's drag:
                                               * the anatomy keeps them out of the palate) */
        p.target[CH_TB_X] = kv[K_TBX];
        p.target[CH_TB_Y] = kv[K_TBY];
        p.target[CH_TT_X] = kv[K_TTX];
        p.target[CH_TT_Y] = kv[K_TTY];
    }
    p.target[CH_LIP_WIDTH] = 1.0f - kv[K_ROUND];
    p.target[CH_VELUM] = kv[K_VELUM];
    p.target[CH_EPILARYNX] = kv[K_RING];
    p.target[CH_VENTRICULAR] = kv[K_FALSE];
    p.gate = vowel_mode && n_vkeys ? vowel_breath : 0.0f;
    if (bt.on) {                              /* the beatbox: a beatboxer's ready posture, the breath off (the sounds
                                               * bring their own), as CHIMERA's beatbox mode */
        ch_real r[CH_NARTIC];
        ch_beat_ready(r);
        for (k = 0; k < CH_NARTIC; k++)
            if (r[k] == r[k])
                p.target[k] = r[k];
        p.src.f0 = 100.0f;
        p.src.vibrato_depth = 0.0f;
        p.gate = 0.0f;
    }
    live.vibrato = kv[K_VIB];
    speech_rate = kv[K_SPEED];
    vp_dirty = 0;                             /* (the interrupt does not take a half-written one) */
    vp_out = p;
    vp_dirty = 1;
}

/* audio interrupt: the parameters, now */
static void vp_take(void)
{
    int k;
    if (!vp_dirty)
        return;
    ch_control_set_source(&ctl, &vp_out.src);
    ctl.tip_free = (uint8_t)!vp_out.tip_held;
    for (k = 0; k < CH_NARTIC; k++)
        if (vp_out.tip_held || (k != CH_TT_X && k != CH_TT_Y))   /* (resting, rest_tip sets the tip behind the teeth) */
            ctl.target[k] = vp_out.target[k];
    ch_control_set_gate(&ctl, vp_out.gate);
    vp_dirty = 0;
}

static void mode_set(uint8_t v)
{
    if (v == vowel_mode)
        return;
    vowel_mode = v;
    n_vkeys = 0;
    voice_note = -1;
    if (v) {                                  /* the words' notes stop: their gestures go */
        pend.n = pend.n_release = 0;
        pend.clear = 1;
        live.sounding = 0;
        live.n_keys = 0;
        mb_post();
        page = v == 2 ? PAGE_TONGUE : PAGE_VOWEL;
    } else if (page == PAGE_VOWEL || page == PAGE_TONGUE) {
        page = 0;
    }
    vp_send();
    app_say(v == 2 ? "TONGUE" : v ? "VOWEL" : "WORDS", v ? "KEYS PLAY PITCH" : "KEYS SING");
}

/* VOWEL: the keys play pitch (the last key held), the breath on while one is down, at its velocity's level (CHIMERA's
 * breath for a velocity) */
static void vowel_on(int32_t note, uint32_t vel)
{
    int i, j = 0;
    for (i = 0; i < n_vkeys; i++)
        if (vkeys[i] != note)
            vkeys[j++] = vkeys[i];
    n_vkeys = j;
    if (n_vkeys < 16)
        vkeys[n_vkeys++] = note;
    vowel_breath = 0.62f + 0.4f * fm_clampf((float)vel / 96.0f, 0, 1) + 0.12f * fm_maxf(0, ((float)vel - 96.0f) / 31.0f);
    voice_note = note;
    vp_send();
}
static void vowel_off(int32_t note)
{
    int i, j = 0;
    for (i = 0; i < n_vkeys; i++)
        if (vkeys[i] != note)
            vkeys[j++] = vkeys[i];
    n_vkeys = j;
    voice_note = n_vkeys ? vkeys[n_vkeys - 1] : -1;
    vp_send();
}

static void knob_turn(int id, int32_t e)
{
    const knob_t *d = &KNOB[id];
    float v = kv[id];
    if (d->fmt == F_BODY) {                   /* ALGORITHM: the body, chosen now, switched when the knob rests */
        int i = (body_pending >= 0 ? body_pending : (int)(kv[K_BODY] + 0.5f)) + (int)e;   /* (from the knob: BLENDED's
                                                                                          * nearest body is not it) */
        if (body_cur == BODY_SEAL) {            /* the Bestiary: its creature */
            creature_pick(e);
            return;
        }
        i = i < 0 ? 0 : i > BODY_BLENDED ? BODY_BLENDED : i;
        body_pending = (int8_t)i;
        body_turned_at = plat_ms();
        if (i == BODY_BLENDED) {
            char t[24];
            body_label(kv[K_GENDER], kv[K_AGE], t);
            app_say("BLENDED", t);
        } else {
            app_say("BODY", body_name(i));
        }
        return;
    }
    v = d->log ? v * fm_exp2f((float)e * fm_log2f(d->step)) : v + (float)e * d->step;
    kv[id] = fm_clampf(v, d->min, d->max);
    set_dirty = 1;
    vp_send();
}

/* a knob's value as the screen shows it */
static void knob_text(int id, char *b)
{
    float v = kv[id];
    int32_t t;
    char d[12];
    b[0] = 0;
    switch (KNOB[id].fmt) {
    case F_PCT: int_text((int32_t)(v * 100.0f + 0.5f), b); text_cat(b, "%"); break;
    case F_CM: fmt1(v, b); break;
    case F_SCM: if (v > 0.04f) text_cat(b, "+"); fmt1(v, d); text_cat(b, d); break;
    case F_CENTS: int_text((int32_t)(v + 0.5f), b); break;
    case F_HZ: fmt1(v, b); break;
    case F_MS: int_text((int32_t)(v * 1000.0f + 0.5f), b); break;
    case F_X: fmt1(v, b); break;
    case F_VELUM:
        if (v <= 0.0f) {
            text_cat(b, "SHUT");
        } else {
            t = (int32_t)(v * 100.0f + 0.5f);
            int_text(t / 100, b);
            text_cat(b, ".");
            d[0] = (char)('0' + t / 10 % 10);
            d[1] = (char)('0' + t % 10);
            d[2] = 0;
            text_cat(b, d);
        }
        break;
    case F_BODY: text_cat(b, (int)(v + 0.5f) == BODY_BLENDED ? "BLENDED" : body_name((int)(v + 0.5f) % NBODIES)); break;
    case F_F2: int_text((int32_t)(pad_f2(v) + 0.5f), b); break;
    case F_F1: int_text((int32_t)(pad_f1(v) + 0.5f), b); break;
    }
}

/* the body the knobs hold (ALGORITHM, or a preset's): the front end's calibrated tables and the drawn head at once,
 * the audio's anatomy at its next block (app_render) */
static void home_body(void);
static void seal_mode_changed(void);
static uint8_t blend_buf[12288] PLAT_POOL;   /* the blended phoneme and vowel tables (ch_speech_blend; ~8 KB) */

/* the three bodies' weights for gender g and age a (CHIMERA's BodyMix::weights), and the nearest */
static int body_weights(float g, float a, float w[3])
{
    int b, near = 0;
    w[0] = (1.0f - a) * (1.0f - g);
    w[1] = (1.0f - a) * g;
    w[2] = a;
    for (b = 1; b < 3; b++)
        if (w[b] > w[near])
            near = b;
    return near;
}

static void body_mix(const float w[3], int near, bodymix_t *m)
{
    int b;
    float lf0 = 0, lp1 = 0, lp2 = 0;
    m->b = CH_BODIES[near];                   /* (its name and its airway's indices) */
    m->b.scale = m->b.pharynx = m->b.tongue_radius = m->b.lip_width = 0;
    m->fade = m->gain = 0;
    for (b = 0; b < NBODIES; b++) {
        const ch_body_t *d = &CH_BODIES[b];
        m->b.scale += w[b] * d->scale;
        m->b.pharynx += w[b] * d->pharynx;
        m->b.tongue_radius += w[b] * d->tongue_radius;
        m->b.lip_width += w[b] * d->lip_width;
        lf0 += w[b] * fm_log2f(d->f0);
        lp1 += w[b] * fm_log2f(d->pad_f1);
        lp2 += w[b] * fm_log2f(d->pad_f2);
        m->fade += w[b] * BODY_FADE[b];
        m->gain += w[b] * BODY_GAIN[b];
    }
    m->b.f0 = fm_exp2f(lf0);
    m->b.pad_f1 = fm_exp2f(lp1);
    m->b.pad_f2 = fm_exp2f(lp2);
    if (w[near] >= 1.0f) {                    /* a body whole: its own numbers exactly (as before 1.0.0) */
        m->b = CH_BODIES[near];
        m->fade = BODY_FADE[near];
        m->gain = BODY_GAIN[near];
    }
}

/* the blend as the screen shows it: a body's name when it is one whole, else the shares there are ("M 40  F 30  C 30") */
static void body_label(float g, float a, char *t)
{
    float w[3];
    static const char *const L[3] = {"M ", "F ", "C "};
    int b, near = body_weights(g, a, w);
    t[0] = 0;
    if (w[near] > 0.995f) {
        text_cat(t, CH_BODIES[near].name);
        return;
    }
    for (b = 0; b < 3; b++) {                 /* (the shares there are) */
        char d[6];
        if (w[b] < 0.005f)
            continue;
        if (t[0])
            text_cat(t, "  ");
        text_cat(t, L[b]);
        int_text((int32_t)(w[b] * 100.0f + 0.5f), d);
        text_cat(t, d);
    }
}

/* the gender and age the body knob gives: a body whole's place, or BLENDED's own */
static void body_place(int body, float *g, float *a)
{
    *g = body == BODY_BLENDED ? fm_clampf(kv[K_GENDER], 0, 1) : body == 1 ? 1.0f : 0.0f;
    *a = body == BODY_BLENDED ? fm_clampf(kv[K_AGE], 0, 1) : body == 2 ? 1.0f : 0.0f;
}

static void body_apply(void)
{
    float g, a, w[3];
    int b;
    body_place((int)(kv[K_BODY] + 0.5f), &g, &a);
    b = body_weights(g, a, w);
    if (b == body_cur && g == body_g && a == body_a)
        return;
    body_cur = (uint8_t)b;
    body_hum = (uint8_t)b;                    /* a human: the nearest body's (the tunes' octave) */
    body_g = g;
    body_a = a;
    body_mix(w, b, &mix_main);
    ch_speech_blend(w, blend_buf, (int)sizeof blend_buf);   /* its tables (a body whole: its own) */
    home_body();
    seal_mode_changed();
    mix_req = mix_main;                       /* the audio's anatomy at its next block */
    mix_seq++;
    body_req = (uint8_t)b;
}

static void preset_apply(int i)
{
    int k;
    for (k = 0; k < NK; k++)                  /* (the vowel pad's and the tongue's places are played, not the voice's) */
        if (k != K_FRONT && k != K_OPEN && k != K_TBX && k != K_TBY && k != K_TTX && k != K_TTY)
            kv[k] = fm_clampf(presets.v[i][k], KNOB[k].min, KNOB[k].max);
    set.preset = (uint8_t)i;
    body_apply();                             /* (a preset carries its body) */
    vp_send();
}

/* VOWEL: a snapshot recalled (FX + a key), or the vowel now stored in it (FX + a key held a second) */
static void snap_recall(int i)
{
    kv[K_FRONT] = presets.snap[i][0];
    kv[K_OPEN] = presets.snap[i][1];
    kv[K_VELUM] = presets.snap[i][2];
    kv[K_ROUND] = presets.snap[i][3];
    vp_send();
}
static void snap_store(int i)
{
    presets.snap[i][0] = kv[K_FRONT];
    presets.snap[i][1] = kv[K_OPEN];
    presets.snap[i][2] = kv[K_VELUM];
    presets.snap[i][3] = kv[K_ROUND];
    set_dirty = 1;
}

/* ---------------------------------------------------------------- the head --- */
/* The head side on, as the anatomy's frame gives it (ch_profile.c, CHIMERA's outlines), drawn in FMB Vocal's own
 * look: no outline, the brain (its gyri, the cores' load its colour), the spine as two lines, the airway and the moving
 * parts each in its colour. The engine's files stay as they are (fm-1-chimera-vocal's pulls). */
#include "ch_profile.h"
static ch_profile_t prof PLAT_POOL;
static void text_cat(char *d, const char *s);
/* the part of the head the screens show (body cm): the whole dome and the ears, the back of the head */
#define VIEW_X0 (-10.4f)
#define VIEW_Y0 (-2.6f)
#define VIEW_Y1 (25.0f)
/* where the head goes on the screen: px a cm, the view's left and top edges (screen px) */
static float head_s = 8.0f, head_x0 = 49.0f, head_y0 = 6.0f;
#define HEAD_WIDE 1.1f                        /* the head a tenth wider than it is tall: its own shape */
#define HEAD_X(x) (head_x0 + ((x) - VIEW_X0) * head_s * HEAD_WIDE)
#define HEAD_Y(y) (head_y0 + (VIEW_Y1 - (y)) * head_s)

static void poly_line(const ch_v2 *p, int n, int closed, int32_t y0, uint16_t c)
{
    int i;
    for (i = 0; i + 1 < n + closed; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        cv_line((int32_t)HEAD_X(a->x), (int32_t)HEAD_Y(a->y) - y0, (int32_t)HEAD_X(b->x), (int32_t)HEAD_Y(b->y) - y0, c);
    }
}

/* FMB Vocal's theme: black and grey, high contrast. The greys are white over black at fixed steps; white is the one
 * thing active (what sounds, what is being turned, the cursor). Colour only where it means something: the head (its
 * body's, its moving parts'), a warning (yellow), an error (red). (The idea of a black-and-white contrast palette from
 * Melodee's CONTRAST theme; none of its code.) The web pages use the same values (app/web/studio.html,
 * tools/make_site.py). */
#define T_TEXT RGB(0xff, 0xff, 0xff)          /* the active thing, values (100 %) */
#define T_HI RGB(0xd6, 0xd6, 0xd6)            /* primary lines and text (84 %) */
#define T_MID RGB(0x9e, 0x9e, 0x9e)           /* labels, secondary (62 %) */
#define T_DIM RGB(0x60, 0x60, 0x60)           /* inactive, empty (38 %) */
#define T_LINE RGB(0x2e, 0x2e, 0x2e)          /* rules, frames (18 %) */
#define T_SURF RGB(0x1a, 0x1a, 0x1a)          /* an empty cell's fill (10 %) */
#define T_WARN RGB(0xff, 0xd2, 0x3f)          /* a warning: yellow */
#define T_ERR RGB(0xff, 0x4d, 0x4d)           /* an error: red */
/* the knobs, each its own colour wherever its name and value show (K1 orange, K2 yellow, K3 violet, K4 cyan) */
#define T_K1 RGB(0xff, 0x9a, 0x3c)
#define T_K2 RGB(0xff, 0xd2, 0x3f)
#define T_K3 RGB(0xb4, 0x8c, 0xff)
#define T_K4 RGB(0x3f, 0xd0, 0xe0)
static const uint16_t KNOB_COL[4] = {T_K1, T_K2, T_K3, T_K4};
/* a "hot" beam's core: the colour 55 % toward white */
#define CORE(v) RGB((v) + (255 - (v)) * 55 / 100, (v) + (255 - (v)) * 55 / 100, (v) + (255 - (v)) * 55 / 100)

typedef struct {
    uint8_t r, g, b;
} rgb8_t;
/* the bodies' colours (MALE, FEMALE, CHILD) and the moving parts' (the tongue, the jaw, the lips; the soft palate white) */
static const rgb8_t BODY_HUE[3] = {{0x4d, 0x7c, 0xff}, {0xf0, 0x41, 0x5a}, {0x2e, 0xc2, 0x7e}};
static const rgb8_t HUE_TONGUE = {0xff, 0x9a, 0x3c}, HUE_JAW = {0xff, 0xd2, 0x3f}, HUE_LIPS = {0xb4, 0x8c, 0xff};

/* the head's colour: its body's, a blend's the three mixed by their shares (brought back to full brightness) */
static rgb8_t head_hue(void)
{
    float w[3], c[3] = {0, 0, 0}, top = 0, m;
    rgb8_t o;
    int i;
    body_weights(body_g < 0 ? 0 : body_g, body_a < 0 ? 0 : body_a, w);
    for (i = 0; i < 3; i++) {
        const rgb8_t *h = &BODY_HUE[i];
        const int mx = h->r > h->g ? (h->r > h->b ? h->r : h->b) : (h->g > h->b ? h->g : h->b);
        c[0] += w[i] * h->r;
        c[1] += w[i] * h->g;
        c[2] += w[i] * h->b;
        top += w[i] * (float)mx;
    }
    m = fm_maxf(c[0], fm_maxf(c[1], c[2]));
    for (i = 0; i < 3; i++)
        c[i] = m > 0 ? fm_clampf(c[i] * top / m, 0, 255) : 0;
    o.r = (uint8_t)c[0];
    o.g = (uint8_t)c[1];
    o.b = (uint8_t)c[2];
    return o;
}

static uint16_t shade(rgb8_t c, int pct) { return RGB(c.r * pct / 100, c.g * pct / 100, c.b * pct / 100); }
static uint16_t hot(rgb8_t c)              /* a beam's core: the colour 55 % toward white */
{
    return RGB(c.r + (255 - c.r) * 55 / 100, c.g + (255 - c.g) * 55 / 100, c.b + (255 - c.b) * 55 / 100);
}

/* a "hot" beam: the colour a pixel either side, its whitish core along the middle */
static void beam(const ch_v2 *p, int n, int closed, int32_t y0, uint16_t c, uint16_t core)
{
    int i;
    for (i = 0; i + 1 < n + closed; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        int32_t x0 = (int32_t)HEAD_X(a->x), ya = (int32_t)HEAD_Y(a->y) - y0, x1 = (int32_t)HEAD_X(b->x), yb = (int32_t)HEAD_Y(b->y) - y0;
        cv_line(x0 + 1, ya, x1 + 1, yb, c);
        cv_line(x0, ya + 1, x1, yb + 1, c);
    }
    poly_line(p, n, closed, y0, core);
}

/* a closed outline with bumps along it where it is above y_lo (the brain's gyri): pushed out from its centre */
static void bumpy(const ch_v2 *p, int n, float amp, float bumps, float y_lo, int32_t y0, uint16_t c)
{
    float cx = 0, cy = 0, len = 0, s = 0;
    int32_t px = 0, py = 0, fx = 0, fy = 0;
    int i, j, first = 1;
    if (n < 3)
        return;
    for (i = 0; i < n; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        cx += a->x;
        cy += a->y;
        len += fm_sqrtf((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y));
    }
    cx /= (float)n;
    cy /= (float)n;
    for (i = 0; i < n; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        const float dl = fm_sqrtf((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y));
        for (j = 0; j < 4; j++) {
            const float f = (float)j * 0.25f;
            float x = a->x + (b->x - a->x) * f, y = a->y + (b->y - a->y) * f;
            const float dx = x - cx, dy = y - cy, r = fm_sqrtf(dx * dx + dy * dy);
            const float up = fm_clampf((y - y_lo) / (amp * 3.0f), 0, 1);
            const float h = amp * up * fm_fabsf(fm_sinf(3.1415927f * bumps * (s + dl * f) / len));
            int32_t sx, sy;
            if (r > 0) {
                x += dx / r * h;
                y += dy / r * h;
            }
            sx = (int32_t)HEAD_X(x);
            sy = (int32_t)HEAD_Y(y) - y0;
            if (first) {
                fx = sx;
                fy = sy;
                first = 0;
            } else {
                cv_line(px, py, sx, sy, c);
            }
            px = sx;
            py = sy;
        }
        s += dl;
    }
    cv_line(px, py, fx, fy, c);
}

/* a cervical vertebra as a lateral X-ray shows it, from its box (the engine's: [0] top back, [1] bottom front): the
 * body with rounded corners and a slightly hollow front, the pedicles back to the arch, and the arch running back into
 * the spinous process, longer down the neck (k: 0 the lowest) */
static void vertebra(const ch_v2 *v, int k, int nv, int32_t y0, uint16_t bone, uint16_t arch)
{
    static const float BODY[9][2] = {{.14f, 0}, {.86f, 0}, {1, .14f}, {.95f, .5f}, {1, .86f}, {.86f, 1}, {.14f, 1},
                                     {0, .86f}, {0, .14f}};
    static const float ARCH[6][2] = {{-.3f, .86f}, {-.6f, .92f}, {-1.3f, .5f}, {-1.36f, .3f}, {-.6f, .42f}, {-.3f, .48f}};
    const float x0 = v[0].x, yb = v[1].y, w = v[1].x - v[0].x, h = v[0].y - v[1].y;
    const float reach = 1.25f + 0.6f * (float)(nv - 1 - k) / (float)(nv > 1 ? nv - 1 : 1);   /* (C7's the longest) */
    ch_v2 q[9];
    int i;
    for (i = 0; i < 9; i++) {
        q[i].x = x0 + BODY[i][0] * w;
        q[i].y = yb + BODY[i][1] * h;
    }
    poly_line(q, 9, 1, y0, bone);
    for (i = 0; i < 6; i++) {
        const float u = ARCH[i][0] < -.6f ? -.6f + (ARCH[i][0] + .6f) * reach : ARCH[i][0];
        q[i].x = x0 + u * w;
        q[i].y = yb + ARCH[i][1] * h;
    }
    poly_line(q, 6, 1, y0, arch);
    q[0].x = x0;                              /* the pedicles: the body's back to the arch */
    q[0].y = yb + .8f * h;
    q[1].x = x0 - .3f * w;
    q[1].y = yb + .84f * h;
    poly_line(q, 2, 0, y0, arch);
    q[0].y = yb + .5f * h;
    q[1].y = yb + .48f * h;
    poly_line(q, 2, 0, y0, arch);
}

/* the brain shows the two cores at work (stat_update: their average over the last half second, %): the second core's
 * load (the airway) its colour, cyan idle through green and yellow to red at 85 % (the room the voice needs), its
 * outline as bright as the head's other lines; the first core's (the control) its fill, the busier the more opaque */
static uint8_t brain_c2, brain_c1;
#ifdef PLAT_HOST
static uint8_t host_loads;                   /* (the simulator: "loads C2 C1" given) */
#endif
static rgb8_t brain_hue(void)
{
    static const struct {
        float at;
        rgb8_t c;
    } STOP[4] = {{0, {0x3f, 0xd0, 0xe0}}, {40, {0x2e, 0xc2, 0x7e}}, {65, {0xff, 0xd2, 0x3f}}, {85, {0xff, 0x4d, 0x4d}}};
    const float l2 = (float)brain_c2;
    rgb8_t o = STOP[3].c;
    int i;
    for (i = 0; i < 3; i++)
        if (l2 < STOP[i + 1].at) {
            const float f = fm_clampf((l2 - STOP[i].at) / (STOP[i + 1].at - STOP[i].at), 0, 1);
            o.r = (uint8_t)((float)STOP[i].c.r + f * (float)(STOP[i + 1].c.r - STOP[i].c.r));
            o.g = (uint8_t)((float)STOP[i].c.g + f * (float)(STOP[i + 1].c.g - STOP[i].c.g));
            o.b = (uint8_t)((float)STOP[i].c.b + f * (float)(STOP[i + 1].c.b - STOP[i].c.b));
            break;
        }
    return o;
}

/* the brain's fill: its outline's colour at BRAIN_FILL_MAX of it (over the black) when the first core is at
 * BRAIN_FILL_AT % or more, none when idle; filled row by row between the outline's crossings */
#define BRAIN_FILL_AT 70.0f
#define BRAIN_FILL_MAX 0.6f
static void brain_fill(int32_t y0, rgb8_t c)
{
    const float a = BRAIN_FILL_MAX * fm_clampf((float)brain_c1 / BRAIN_FILL_AT, 0, 1);
    const int n = prof.n_brain;
    float top = 1e9f, bot = -1e9f;
    int32_t y, ya, yb;
    int i;
    if (a < 0.02f || n < 3)
        return;
    for (i = 0; i < n; i++) {
        top = fm_minf(top, HEAD_Y(prof.brain[i].y));
        bot = fm_maxf(bot, HEAD_Y(prof.brain[i].y));
    }
    ya = (int32_t)top;
    yb = (int32_t)bot;
    for (y = ya; y <= yb; y++) {
        float xs[16];
        const float sy = (float)y + 0.5f;
        int m = 0, j, k;
        for (i = 0; i < n && m < 16; i++) {   /* where the row crosses the outline's edges */
            const ch_v2 *p = &prof.brain[i], *q = &prof.brain[(i + 1) % n];
            const float py = HEAD_Y(p->y), qy = HEAD_Y(q->y);
            if ((py <= sy) != (qy <= sy))
                xs[m++] = HEAD_X(p->x) + (sy - py) / (qy - py) * (HEAD_X(q->x) - HEAD_X(p->x));
        }
        for (j = 1; j < m; j++)               /* (a few: in order by insertion) */
            for (k = j; k > 0 && xs[k - 1] > xs[k]; k--) {
                const float t = xs[k];
                xs[k] = xs[k - 1];
                xs[k - 1] = t;
            }
        for (j = 0; j + 1 < m; j += 2) {
            const int32_t x0 = (int32_t)(xs[j] + 0.5f), x1 = (int32_t)(xs[j + 1] + 0.5f);
            if (x1 > x0)
                cv_rect(x0, y - y0, x1 - x0, 1, RGB((int)((float)c.r * a), (int)((float)c.g * a), (int)((float)c.b * a)));
        }
    }
}

/* the eye as last decided (eye_state): its openness 0 (shut) .. EYE_STEPS (open), EYE_RED added when it is red */
#define EYE_STEPS 8u
#define EYE_RED 16u
static uint8_t eye_drawn = EYE_STEPS;

static void draw_eye(int32_t y0, rgb8_t hue)
{
    static const rgb8_t RED = {0xff, 0x4d, 0x4d};
    const float open = (float)(eye_drawn & (EYE_RED - 1u)) / (float)EYE_STEPS;
    const rgb8_t c = (eye_drawn & EYE_RED) ? RED : hue;
    const float cx = prof.eye_c.x - 0.55f * prof.eye_rx, cy = prof.eye_c.y;   /* (a little back from the brow) */
    const float rx = prof.eye_rx * 1.3f, ry = prof.eye_rx * 0.8f * open;
    ch_v2 e[24];
    int i;
    for (i = 0; i < 24; i++) {              /* an almond: the lids meet in points at the corners; shut, a line */
        const float an = 6.2831853f * (float)i / 24.0f, sn = fm_sinf(an);
        e[i].x = cx + rx * fm_cosf(an);
        e[i].y = cy + ry * sn * fm_fabsf(sn);
    }
    poly_line(e, 24, 1, y0, shade(c, 100));
    if (open > 0.6f) {                      /* the pupil */
        const int32_t px = (int32_t)HEAD_X(cx + 0.15f * rx), py = (int32_t)HEAD_Y(cy) - y0;
        cv_rect(px - 1, py - 1, 3, 3, hot(c));
    }
}

/* the head (prof) on the canvas, whose top is screen row y0 */
static void draw_head(int32_t y0)
{
    const rgb8_t hue = head_hue();
    const uint16_t c_hi = shade(hue, 85), c_mid = shade(hue, 62), c_dim = shade(hue, 42);
    ch_v2 sp[2][18];
    int i, n = 0;
    /* no outline (no skin, skull or eye): the brain and its gyri, the spine as two lines (its front and back), the
     * airway and the moving parts */
    (void)c_dim;
    brain_fill(y0, brain_hue());
    bumpy(prof.brain, prof.n_brain, 0.62f * prof.eye_rx / 1.1f, 11.0f, prof.eye_c.y, y0, shade(brain_hue(), 100));
    poly_line(prof.sulcus, prof.n_sulcus, 0, y0, shade(brain_hue(), 100));
    for (i = 0; i < prof.n_vertebrae && n + 2 <= 18; i++) {   /* (bottom to top: each box's [0] top back, [1] bottom front) */
        const ch_v2 *v = prof.vertebrae[i];
        sp[0][n].x = v[0].x; sp[0][n].y = v[1].y;
        sp[1][n].x = v[1].x; sp[1][n].y = v[1].y;
        sp[0][n + 1].x = v[0].x; sp[0][n + 1].y = v[0].y;
        sp[1][n + 1].x = v[1].x; sp[1][n + 1].y = v[0].y;
        n += 2;
    }
    poly_line(sp[0], n, 0, y0, c_mid);
    poly_line(sp[1], n, 0, y0, c_mid);
    poly_line(prof.nasal, prof.n_nasal, 1, y0, c_mid);
    poly_line(prof.palate, 10, 0, y0, c_mid);
    poly_line(prof.trachea[0], 2, 0, y0, c_mid);
    poly_line(prof.trachea[1], 2, 0, y0, c_mid);
    poly_line(prof.upper_teeth, 5, 1, y0, T_HI);   /* (the teeth bone white) */
    poly_line(prof.lower_teeth, 7, 0, y0, T_HI);
    draw_eye(y0, hue);
    /* the airway's walls, the body's colour (a blend's mixed) */
    poly_line(prof.airway, prof.n_airway, 0, y0, c_hi);
    /* the moving parts, each its own colour: the tongue orange, the jaw yellow, the lips violet, the soft palate white */
    beam(prof.tongue, prof.n_tongue, 1, y0, shade(HUE_TONGUE, 100), hot(HUE_TONGUE));
    beam(prof.jaw, prof.n_jaw, 0, y0, shade(HUE_JAW, 100), hot(HUE_JAW));
    beam(prof.upper_lip, 12, 0, y0, shade(HUE_LIPS, 100), hot(HUE_LIPS));
    beam(prof.lower_lip, 12, 0, y0, shade(HUE_LIPS, 100), hot(HUE_LIPS));
    poly_line(prof.velum, 11, 0, y0, T_TEXT);
}

/* the boot screen: the name, and at the bottom the version and the credits */
static void boot_text(int32_t y0, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text((240 - text_w(f, s)) / 2, y - y0, f, s, c);
}

static void boot_strip(int32_t y0, int32_t h)
{
    static const char VER[] = APP_VERSION;
    char v[24] = "";
    cv_begin(240, (uint32_t)h, C_BLACK);
    boot_text(y0, 78, &FONT_L, "FMB", T_HI);
    boot_text(y0, 112, &FONT_L, "Vocal", T_HI);
    cv_rect(60, 158 - y0, 120, 1, T_LINE);
    if (VER[0] >= '0' && VER[0] <= '9')
        text_cat(v, "V");
    text_cat(v, VER);
    boot_text(y0, 170, &FONT_B, v, T_TEXT);
    boot_text(y0, 190, &FONT_XS, "Created by Shan-Sherif", T_MID);
    boot_text(y0, 206, &FONT_XS, "Voice engine based on CHIMERA", T_MID);
    boot_text(y0, 220, &FONT_XS, "by Jameson Hodge", T_MID);
    cv_blit(0, (uint32_t)y0);
}

void app_splash(void)
{
    boot_strip(0, 120);
    boot_strip(120, 120);
}

/* --------------------------------------------------------------------- UI --- */
static uint8_t page_debug;                   /* GLO: the debug page (the timings) over the page beneath */
static struct {
    char msg[2][24];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t vel[NKEYS], prs[NKEYS];      /* each key's last velocity, and its pressure while held */
    int32_t last_key;                    /* the key pressed last: its numbers in the header */
    uint8_t looking;                     /* Bluetooth MIDI devices: REC pressed, the scan's list up (lib/btdev) */
    uint32_t rec_t;                      /* REC went down (0: not held, or its hold already acted) */
    int32_t turns[NE];                   /* detents since boot, per encoder */
    uint32_t turned_at[NE];              /* when each was last turned: it shows lit for a moment */
    uint8_t redraw;
} ui;
/* the word editor (EDIT: below, before input) */
#define ED_MAX 150                            /* (lyric holds 160) */
static struct {
    uint8_t on, slot, dirty, drop_armed;
    uint8_t n, cur;                           /* the length, the cursor (0..n) */
    char t[ED_MAX + 1];
    uint32_t del_at;                          /* OCT- went down (its repeat), 0: not held */
} ed;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

/* the message's colour: an error red (a save that failed), a warning yellow (something left unsaved, dropped, full, a
 * device lost), else the colour the screen gives it */
static int same_text(const char *a, const char *b)   /* (no strcmp on the FM-1's build) */
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint16_t msg_col(uint16_t plain)
{
    static const char *const ERR[] = {"FAILED", "TOO LONG"};
    static const char *const WARN[] = {"UNSAVED", "SAVE FIRST", "DROPPED", "FULL", "LOST", "NO ANSWER"};
    uint32_t i;
    for (i = 0; i < sizeof ERR / sizeof ERR[0]; i++)
        if (same_text(ui.msg[0], ERR[i]) || same_text(ui.msg[1], ERR[i]))
            return T_ERR;
    for (i = 0; i < sizeof WARN / sizeof WARN[0]; i++)
        if (same_text(ui.msg[0], WARN[i]) || same_text(ui.msg[1], WARN[i]))
            return T_WARN;
    return plain;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {                          /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void octave_text(char *t)
{
    t[0] = set.octave < 0 ? '-' : '+';
    t[1] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave));
    t[2] = 0;
}

/* a screen reader that has just started listening (or asks) hears the whole screen */
static void speak(void)
{
    char t[96], o[4];
    if (ann_on() && ann_where()) {
        octave_text(o);
        t[0] = 0;
        if (ed.on) {                         /* the editor: the slot and its words as typed */
            char e[ED_MAX + 20] = "EDIT WORDS ", sl[4] = {(char)('1' + ed.slot), '.', ' ', 0};
            text_cat(e, sl);
            text_cat(e, ed.n ? ed.t : "EMPTY");
            ann_say(e, 0, ANN_NOW);
            ann_tick(plat_ms());
            return;
        }
        text_cat(t, APP_NAME ". OCTAVE ");
        text_cat(t, o);
        text_cat(t, ". WORDS ");
        text_cat(t, lyric);
        if (midi_count) {                    /* the last MIDI message in, as the screen shows it */
            static const char *const PORT[NPORTS] = {"USB", "BLE", "TRS"};
            static const char HEX[] = "0123456789ABCDEF";
            uint32_t m = midi_last, b;
            char h[10];
            for (b = 0; b < 3u; b++) {
                uint32_t v = (m >> (8u + 8u * b)) & 0xFFu;
                h[3 * b] = HEX[v >> 4];
                h[3 * b + 1] = HEX[v & 15u];
                h[3 * b + 2] = b < 2u ? ' ' : 0;
            }
            text_cat(t, ". MIDI IN ");
            text_cat(t, midi_last_port < NPORTS ? PORT[midi_last_port] : "");
            text_cat(t, " ");
            text_cat(t, h);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void note_name(int32_t note, char *b)
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char o[12];
    b[0] = 0;
    text_cat(b, N[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    {   /* a key held across the change would not find its note off: its note ends now */
        uint32_t k;
        for (k = 0; k < NKEYS; k++)
            if (ui.keys >> k & 1u)
                live_off(53 + (int32_t)k + 12 * set.octave);
    }
    set.octave = (int8_t)o;
    set_dirty = 1;
    octave_text(t);
    app_say("OCTAVE", t);
}

int template_ble_off(void) { return set.ble_off; }

/* REC held a second: Bluetooth off or on (kept with the settings, so it stays so at the next power-on) */
static void ble_toggle(void)
{
    set.ble_off = (uint8_t)!set.ble_off;
    set_dirty = 1;
    plat_ble_enable(!set.ble_off);
    app_say("BLUETOOTH", set.ble_off ? "OFF" : "ON");
}

/* REC: a Bluetooth MIDI device (FM1B_BLE_CENTRAL, lib/btdev): look for devices, then connect to the one ALGORITHM
 * picks, then let it go */
static void ble_device_say(void)
{
    char name[21];
    if (btdev_found(btdev_pick(), name, 0))
        app_say("DEVICE", name);
    else
        app_say("BLE", "SCANNING");
}
static void ble_device_button(void)
{
    int ev;
    if (!btdev_busy() && !ui.looking) {
        ui.looking = 1;
        app_say("BLE", "SCANNING");
        return;
    }
    ev = btdev_save();
    ui.looking = 0;
    app_say(ev == BTDEV_CONNECTING ? btdev_word(ev) : "BLE", ev == BTDEV_CONNECTING ? btdev_name() : btdev_word(ev));
}

/* ---------------------------------------------------------------- the word editor --- */
/* EDIT: a word slot's words typed on the keys, a letter a key: the 26 lowest keys are A..Z from the bottom up (the black
 * keys too), the top key (G5) a space. K1 moves the cursor; K2 changes the character before it (A..Z, space, then
 * , . ? ! ' -, for punctuation); OCT- deletes before the cursor (held: again and again), OCT+ after it. PRESET picks the
 * slot to edit (with nothing unsaved). PLAY speaks the words as they are; SAVE puts them in the slot (the Studio's
 * document, OBJ_WORDS: the Studio reads them back) and leaves; EDIT or HOME leaves (with changes, a second press drops
 * them). The keys don't sing meanwhile; MIDI in still does. FX switches the keys to QWERTY in two rows and back (kept
 * with the settings): the black keys QWERTYUIOP and a space (the last), the white keys ASDFGHJKL then ZXCVBNM. (Its
 * state, ed, is with the UI's, above.) */
static const char ED_CHARS[] = "abcdefghijklmnopqrstuvwxyz ,.?!'-";
static char ed_key_char(uint32_t k)           /* the character key k types, in the layout chosen */
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const char QW_WHITE[] = "asdfghjklzxcvbnm", QW_BLACK[] = "qwertyuiop ";
    uint32_t i;
    if (!set.kb)
        return k < 26u ? (char)('a' + k) : ' ';
    for (i = 0; i < 16u; i++)
        if (WK[i] == k)
            return QW_WHITE[i];
    for (i = 0; i < 11u; i++)
        if (BK[i] == k)
            return QW_BLACK[i];
    return ' ';
}
static uint32_t ed_row_keys(void)            /* QWERTY: the keys that start its rows, Q (the first black), A, Z (whites 1,
                                              * 10), and the space (the last black) */
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    return 1u << BK[0] | 1u << WK[0] | 1u << WK[9] | 1u << BK[10];
}

static void ed_slot_say(const char *a)
{
    char sl[12] = "WORDS ";
    sl[6] = (char)('1' + ed.slot);
    sl[7] = 0;
    app_say(a, sl);
}

static void ed_load(int slot)
{
    uint32_t n = slot_text[slot] ? slot_len[slot] : 0u, i;
    ed.slot = (uint8_t)slot;
    if (n > ED_MAX)
        n = ED_MAX;
    for (i = 0; i < n; i++)
        ed.t[i] = slot_text[slot][i];
    ed.t[n] = 0;
    ed.n = ed.cur = (uint8_t)n;
    ed.dirty = ed.drop_armed = 0;
}

static void ed_open(void)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)               /* the keys held stop singing: from now on they type */
        if (ui.keys >> k & 1u)
            live_off(53 + (int32_t)k + 12 * set.octave);
    ed_load(set.slot);
    ed.on = 1;
    ed.del_at = 0;
    ed_slot_say("EDIT");
}

static void ed_close(void)
{
    plat_lights_mid(0u, 0u);                  /* (the rows' lights: the middle level stays until it is cleared) */
    ed.on = 0;
    ed.drop_armed = 0;
    ui.redraw = 1;
}

static void ed_changed(void)
{
    ed.dirty = 1;
    ed.drop_armed = 0;
    ui.redraw = 1;
}

static void ed_insert(char c)
{
    uint32_t i;
    if (ed.n >= ED_MAX) {
        app_say("EDIT", "FULL");
        return;
    }
    for (i = ed.n; i > ed.cur; i--)
        ed.t[i] = ed.t[i - 1];
    ed.t[ed.cur++] = c;
    ed.t[++ed.n] = 0;
    ed_changed();
    if (ann_on()) {                           /* a screen reader hears the letter */
        char s[2] = {(char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c), 0};
        ann_say(c == ' ' ? "SPACE" : s, 0, ANN_NOW);
    }
}

static void ed_delete(int before)             /* the character before the cursor (1), or after it (0) */
{
    uint32_t i;
    if (before ? !ed.cur : ed.cur >= ed.n)
        return;
    if (before)
        ed.cur--;
    for (i = ed.cur; i < ed.n; i++)
        ed.t[i] = ed.t[i + 1];
    ed.n--;
    ed_changed();
}

static void ed_cycle(int32_t e)               /* K2: the character before the cursor through ED_CHARS */
{
    const int32_t m = (int32_t)sizeof ED_CHARS - 1;
    int32_t i = 0;
    char c;
    if (!ed.cur)
        return;
    c = ed.t[ed.cur - 1];
    if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    while (i < m && ED_CHARS[i] != c)
        i++;
    if (i == m)
        i = 0;
    ed.t[ed.cur - 1] = ED_CHARS[((i + e) % m + m) % m];
    ed_changed();
}

/* the document again with slot `slot` as t (n bytes; none: the slot goes), the other slots and the pronunciations as
 * they were; saved, then sung from. 0, -1 too long, -2 the flash refused it */
static int words_put(int slot, const char *t, uint32_t n)
{
    uint32_t k = 0, i, len;
    const char *s;
#define WPUT(c) do { if (k >= WORDS_MAX) return -1; words_rx[k++] = (c); } while (0)
    for (s = "VOCALWORDS 1\n"; *s; s++)
        WPUT(*s);
    for (i = 0; i < WORDS_SLOTS; i++) {
        s = (int)i == slot ? t : slot_text[i];
        len = (int)i == slot ? n : slot_len[i];
        if (!s || !len)
            continue;
        WPUT((char)('1' + i));
        WPUT(' ');
        while (len--)
            WPUT(*s++);
        WPUT('\n');
    }
    for (i = 0; i < (uint32_t)n_pron; i++) {
        WPUT('@');
        for (s = pron[i]; *s && *s != '\n'; s++)
            WPUT(*s);
        WPUT('\n');
    }
#undef WPUT
    rx_len = rx_got = 0;                      /* (a Studio upload in progress is dropped: it begins again) */
    if (plat_store_save(OBJ_WORDS, words_rx, k) != 0)
        return -2;
    memcpy(words_doc, words_rx, k);
    words_len = k;
    words_doc[k] = 0;
    words_index();
    return 0;
}

/* ARP: a random line of the built-in words (3 to 6, each pronounced as the FM-1 knows it), in place of the words;
 * unsaved until SAVE, as any edit. Another press, another line */
static uint32_t ed_rand(void)
{
    static uint32_t x;
    if (!x)
        x = plat_ticks24() * 2654435761u | 1u;   /* (seeded by when it was first asked) */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void ed_random(void)
{
    const uint32_t n_dict = (uint32_t)ch_dict_count(), want = 3u + ed_rand() % 4u;
    uint32_t got = 0, n = 0, tries = 0;
    int last = -1;
    while (got < want && tries++ < 40u) {
        int i = (int)(ed_rand() % n_dict);
        const char *w = ch_dict_word(i);
        uint32_t len = 0;
        while (w[len])
            len++;
        if (i == last || !len || n + len + (n ? 1u : 0u) > 60u)
            continue;
        if (n)
            ed.t[n++] = ' ';
        while (*w)
            ed.t[n++] = *w++;
        last = i;
        got++;
    }
    ed.t[n] = 0;
    ed.n = ed.cur = (uint8_t)n;
    ed_changed();
    app_say("RANDOM", "WORDS");
}

static void ed_save(void)
{
    uint32_t a = 0, b = ed.n;
    int r;
    while (a < b && ed.t[a] == ' ')           /* without the spaces around */
        a++;
    while (b > a && ed.t[b - 1] == ' ')
        b--;
    r = words_put(ed.slot, ed.t + a, b - a);
    if (r) {
        app_say("SAVE", r == -1 ? "TOO LONG" : "FAILED");
        return;
    }
    words_select(ed.slot);                    /* (an emptied slot: the first with words) */
    set_dirty = 1;
    ed_close();
    ed_slot_say("SAVED");
}

static void ed_leave(void)
{
    if (ed.dirty && !ed.drop_armed) {
        ed.drop_armed = 1;
        app_say("UNSAVED", "AGAIN: DROP");
        return;
    }
    if (ed.dirty)
        app_say("CHANGES", "DROPPED");
    else
        ed_slot_say("WORDS");
    ed_close();
}

/* the editor's input: the keys type, the buttons and knobs edit (input() hands everything here while it is open) */
static void ed_input(uint32_t keys, uint32_t btn, uint32_t now)
{
    uint32_t k;
    int32_t e;
    for (k = 0; k < NKEYS; k++)
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u))
            ed_insert(ed_key_char(k));
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN) {
            ed_delete(1);
            ed.del_at = now;
        } else if (k == B_OCTUP) {
            ed_delete(0);
        } else if (k == B_PLAY) {             /* try the words as typed */
            mode_set(0);
            speak_text(ed.t);
            ed_slot_say("SPEAKING");
        } else if (k == B_SAVE) {
            ed_save();
        } else if (k == B_ARP) {              /* a random line of words */
            ed_random();
        } else if (k == B_FX) {               /* the keys' layout: A-Z or QWERTY, kept at once */
            set.kb = (uint8_t)!set.kb;
            plat_store_save(OBJ_SETTINGS, &set, sizeof set);
            app_say("KEYS", set.kb ? "QWERTY" : "A TO Z");
        } else if (k == B_EDIT || k == B_HOME) {
            ed_leave();
        }
    }
    if (!(btn >> B_OCTDN & 1u))
        ed.del_at = 0;
    else if (ed.del_at && now - ed.del_at >= 450u) {   /* OCT- held: a character every 70 ms */
        ed_delete(1);
        ed.del_at = now - 380u;
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        if (k == EN_K1) {
            ed.cur = (uint8_t)clamp((int32_t)ed.cur + e, 0, ed.n);
            ui.redraw = 1;
        } else if (k == EN_K2) {
            ed_cycle(e);
        } else if (k == EN_PRESET) {          /* another slot (its words, empty or not) */
            if (ed.dirty) {
                app_say("SAVE FIRST", "OR LEAVE");
            } else {
                ed_load(((ed.slot + e) % WORDS_SLOTS + WORDS_SLOTS) % WORDS_SLOTS);
                ed_slot_say("EDIT");
            }
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {   /* the lights as plat_leds gives them (PLAY green, the unlit buttons glowing), and in QWERTY the rows' first
         * keys (Q, A, Z) and the space at the middle level (brighter than the glow), to find the rows by */
        uint32_t b = btn | 1u << B_EDIT | (ed.dirty ? 1u << B_SAVE : 0u), play = 1u << B_PLAY;
        uint32_t dim = ((1u << NB) - 1u) & ~b, lit = keys | midi_keys(set.octave);
        b = (b & ~play) | (b & play ? LED_PLAY_GREEN : 0u);
        dim = (dim & ~play) | (dim & play ? LED_PLAY_GREEN : 0u);
        plat_glow(1);
        plat_lights(b, lit, dim, 0u, 2);
        plat_lights_mid(0u, ed.on && set.kb ? ed_row_keys() & ~lit : 0u);   /* (none once this pass left the editor) */
    }
}

static uint8_t blend_pending;                 /* SEL held + PRESET / SELECT turned: the blend switches when they rest */
static uint32_t blend_turned_at;

static void blend_turn(int id, int32_t e)    /* gender or age, in 5 % steps (from a body whole: BLENDED, from its place) */
{
    char t[24];
    float v;
    const int body = body_pending >= 0 ? body_pending : (int)(kv[K_BODY] + 0.5f);
    if (body != BODY_BLENDED) {
        body_place(body, &kv[K_GENDER], &kv[K_AGE]);
        kv[K_BODY] = (float)BODY_BLENDED;
        body_pending = -1;
    }
    v = kv[id] + 0.05f * (float)e;
    v = (float)(int32_t)(v * 20.0f + (v >= 0 ? 0.5f : -0.5f)) / 20.0f;
    kv[id] = fm_clampf(v, 0, 1);
    blend_pending = 1;
    blend_turned_at = plat_ms();
    body_label(kv[K_GENDER], kv[K_AGE], t);
    app_say(id == K_GENDER ? "GENDER" : "AGE", t);   /* (BLENDED: in the header once it rests) */
}

static void body_settle(uint32_t now)          /* ALGORITHM at rest: the body switches; a blend likewise */
{
    if (blend_pending && now - blend_turned_at >= 400u) {
        blend_pending = 0;
        set_dirty = 1;
        body_apply();
        vp_send();
        ui.redraw = 1;
    }
    if (body_pending >= 0 && now - body_turned_at >= 400u) {
        kv[K_BODY] = (float)body_pending;   /* (BLENDED's gender and age are kept for it) */
        body_pending = -1;
        set_dirty = 1;
        body_apply();
        vp_send();
        ui.redraw = 1;
    }
}

/* ------------------------------------------------------------------- the Bestiary --- */
/* LFO (0.7.0): CHIMERA's Bestiary in place of the human voice, ALGORITHM choosing the creature: the seal, the songbird.
 * The keys play its pitch (its breath on while one is held, the last held sounding), SEL turns its two pages of
 * settings on K1-K4, PRESET picks a call, PLAY plays it at its own pitch, FX + a key at the key's. SAVE keeps each
 * one's settings and call (OBJ_CREATURES) */
#define CREATURES_MAGIC 0x46564331u           /* "FVC1" */
static struct {
    uint32_t magic;
    float set[NCRE][CH_CP_MAX];
    uint8_t call[NCRE], which, sound[NCRE];   /* sound (0.2.0, in 0.1.0's spare bytes): how it sings a note, 0 held, else
                                               * call sound - 1 */
} cre;
/* the older saves, the same fields for fewer creatures: 0.1.0's and 0.2.0's five, 0.3.0's six (the robot); told
 * apart by their size, read into this one at boot (the creatures since, from their defaults) */
#define CRE_SAVE(n) struct { uint32_t magic; float set[n][CH_CP_MAX]; uint8_t call[n], which, sound[n]; }
typedef CRE_SAVE(5) cre_v1_t;
typedef CRE_SAVE(6) cre_v2_t;
static uint8_t seal_page;                     /* its settings' page, 0 or 1 */
static int32_t skeys[16];                     /* its keys down, oldest first (MIDI notes) */
static int n_skeys;
static uint8_t seal_seq;                      /* (a call's number for the mailbox) */
static int8_t seal_playing = -1;              /* the call played last (shown), -1 none */
static uint32_t seal_played_at;
static const int8_t CRE_PAGE[NCRE][2][4] = {
    {{CAT_MOUTH, CAT_PRESSURE, CAT_NOSE, CAT_ROUGH}, {CAT_PURR, CAT_PURR_RATE, CAT_HISS, -1}},
    {{CH_BIRD_INTERVAL, CH_BIRD_VOICES, CH_BIRD_DRIVE, CH_BIRD_TENSION}, {CH_BIRD_PRESSURE, CH_BIRD_PURITY, CH_BIRD_OEC, CH_BIRD_GAPE}},
    {{CH_SEAL_PULSE_RATE, CH_SEAL_PULSE, CH_SEAL_DUTY, CH_SEAL_BRIGHT}, {CH_SEAL_ROUGH, CH_SEAL_PRESSURE, CH_SEAL_MOUTH, -1}},
    {{CH_GATOR_CHAOS, CH_GATOR_PRESSURE, CH_GATOR_HISS, -1}, {CH_GATOR_MOUTH, CH_GATOR_NOSTRILS, -1, -1}},
    {{CH_ELE_MODE, CH_ELE_LIP_PITCH, CH_ELE_PRESSURE, CH_ELE_CHAOS}, {CH_ELE_TRUNK, -1, -1, -1}},
    {{ROB_WOBBLE, ROB_DEPTH, ROB_GRIT, ROB_BUZZ}, {ROB_MOUTH, ROB_PRESSURE, -1, -1}},
    {{SQ_RATE, SQ_HOP, SQ_SQUISH, SQ_MOUTH}, {SQ_BABBLE, SQ_PRESSURE, -1, -1}}};
static const char *const CRE_PAGE_NAME[NCRE][2] = {{"VOICE", "PURR"}, {"SYRINX", "THROAT"}, {"PULSE", "VOICE"},
                                                   {"ROAR", "AIRWAY"}, {"VOICE", "AIRWAY"}, {"WOBBLE", "TUBE"},
                                                   {"BABBLE", "VOICE"}};
#define CRE (CRE_DEF[cre.which])
typedef char cre_size_check[sizeof(cre_v1_t) == 216 && sizeof(cre_v2_t) == 260 && sizeof cre != sizeof(cre_v1_t)
                            && sizeof cre != sizeof(cre_v2_t) ? 1 : -1];   /* (the saves told apart by their size) */

/* a creature's pitch: its setting the keys and the calls play (the elephant's is its second) */
static int cre_pitch(const ch_cdef_t *d)
{
    int i;
    for (i = 0; i < d->np; i++)
        if (d->p[i].pitch)
            return i;
    return 0;
}

/* an older save's n creatures into cre, the ones since from their defaults */
static void cre_from(int n, const float (*set)[CH_CP_MAX], const uint8_t *call, uint8_t which, const uint8_t *sound)
{
    int i, c;
    cre.magic = CREATURES_MAGIC;
    for (c = 0; c < NCRE; c++) {
        for (i = 0; i < CH_CP_MAX; i++)
            cre.set[c][i] = c < n ? set[c][i] : i < CRE_DEF[c]->np ? CRE_DEF[c]->p[i].def : 0.0f;
        cre.call[c] = c < n ? call[c] : 0;
        cre.sound[c] = c < n ? sound[c] : 0;
    }
    cre.which = which;
}

static void creatures_load(void)
{
    union {                                   /* (on the stack: needed once, at boot) */
        cre_v1_t v1;
        cre_v2_t v2;
        uint8_t b[sizeof cre];
    } u;
    int i, c, n = plat_store_load(OBJ_CREATURES, &u, sizeof u), ok = 1;
    if (n == (int)sizeof u.v1 && u.v1.magic == CREATURES_MAGIC) {          /* 0.1.0's, 0.2.0's: five */
        cre_from(5, u.v1.set, u.v1.call, u.v1.which, u.v1.sound);
        n = (int)sizeof cre;
    } else if (n == (int)sizeof u.v2 && u.v2.magic == CREATURES_MAGIC) {   /* 0.3.0's: six */
        cre_from(6, u.v2.set, u.v2.call, u.v2.which, u.v2.sound);
        n = (int)sizeof cre;
    } else if (n == (int)sizeof cre)
        memcpy(&cre, u.b, sizeof cre);
    if (n == (int)sizeof cre && cre.magic == CREATURES_MAGIC && cre.which < NCRE) {
        for (c = 0; c < NCRE; c++) {
            ok &= cre.call[c] < CRE_DEF[c]->ncalls;
            if (cre.sound[c] > CRE_DEF[c]->ncalls)   /* (0.1.0's spare bytes: 0, held) */
                cre.sound[c] = 0;
        }
        if (ok)
            return;
    }
    cre.magic = CREATURES_MAGIC;
    for (c = 0; c < NCRE; c++) {
        for (i = 0; i < CH_CP_MAX; i++)
            cre.set[c][i] = i < CRE_DEF[c]->np ? CRE_DEF[c]->p[i].def : 0.0f;
        cre.call[c] = 0;
    }
    cre.which = CRE_ORDER[0];                 /* (a fresh Bestiary: the first in ALGORITHM's order) */
}

/* the creature's settings, its keys' pitch and breath, a call (call >= 0: play it at pitch, Hz; 0 its own; -2: none) */
static void cm_send(int call, float pitch)
{
    cmail_t m;
    int i;
    for (i = 0; i < CH_CP_MAX; i++)
        m.perf[i] = cre.set[cre.which][i];
    if (n_skeys)
        m.perf[cre_pitch(CRE)] = ch_ckey_hz(&CRE->p[cre_pitch(CRE)], (float)skeys[n_skeys - 1]);   /* (its pitch setting) */
    m.gate = n_skeys ? 1.0f : 0.0f;
    m.call = cm_out.call;
    m.call_seq = cm_out.call_seq;
    m.call_pitch = cm_out.call_pitch;
    if (call != -2) {
        m.call = (int8_t)call;
        m.call_pitch = pitch;
        m.call_seq = ++seal_seq;
        if (call >= 0) {
            seal_playing = (int8_t)call;
            seal_played_at = plat_ms();
        }
    }
    cm_dirty = 0;
    cm_out = m;
    cm_dirty = 1;
}

static void seal_mode_changed(void)           /* into the Bestiary, out of it, or another creature: the notes stop */
{
    n_skeys = 0;
    voice_note = -1;
    seal_playing = -1;
    if (body_cur == BODY_SEAL) {
        if (vowel_mode)
            mode_set(0);
        pend.n = pend.n_release = 0;
        pend.clear = 1;
        live.sounding = 0;
        live.n_keys = 0;
        mb_post();
        seal_page = 0;
    }
    cre_req = cre.which;
    cm_send(-1, 0.0f);
}

static void seal_on(int32_t note)
{
    int i, j = 0;
    for (i = 0; i < n_skeys; i++)
        if (skeys[i] != note)
            skeys[j++] = skeys[i];
    n_skeys = j;
    if (n_skeys < 16)
        skeys[n_skeys++] = note;
    voice_note = note;
    cm_send(-2, 0.0f);
}

static void seal_off(int32_t note)
{
    int i, j = 0;
    for (i = 0; i < n_skeys; i++)
        if (skeys[i] != note)
            skeys[j++] = skeys[i];
    n_skeys = j;
    voice_note = n_skeys ? skeys[n_skeys - 1] : -1;
    cm_send(-2, 0.0f);
}

static void seal_call(int32_t note)           /* the call PRESET picked (note < 0: at its own pitch) */
{
    const int c = cre.call[cre.which];
    const float hz = note >= 0 ? ch_ckey_hz(&CRE->p[cre_pitch(CRE)], (float)note) : 0.0f;
    cm_send(c, hz);
    app_say(CRE->calls[c].name, CRE->calls[c].hint);
}

static void creature_pick(int32_t e)          /* ALGORITHM on the Bestiary page: the next creature, in CRE_ORDER */
{
    cre.which = CRE_ORDER[((cre_pos(cre.which) + (int)e) % NCRE + NCRE) % NCRE];
    set_dirty = 1;
    seal_mode_changed();
    app_say("CREATURE", CRE->name);
    ui.redraw = 1;
}

/* LFO: the Bestiary page in place of the human voice, and back to the body ALGORITHM holds */
static void bestiary_toggle(void)
{
    if (body_cur != BODY_SEAL) {
        body_cur = BODY_SEAL;
        body_pending = -1;
        seal_mode_changed();
        body_req = BODY_SEAL;
        app_say("BESTIARY", CRE->name);
    } else {
        body_cur = 0xFFu;                     /* (so body_apply switches) */
        body_apply();
        app_say("HUMAN", body_name(body_cur));
    }
    ui.redraw = 1;
}

static void seal_knob(int k, int32_t e)       /* K1..K4: the page's settings (log ones a semitone a detent) */
{
    const int id = CRE_PAGE[cre.which][seal_page][k];
    const ch_cparam_t *p;
    float v;
    if (id < 0)
        return;
    p = &CRE->p[id];
    v = cre.set[cre.which][id];
    v = p->log ? v * fm_exp2f((float)e / 12.0f) : v + (float)e * (p->max - p->min) / 50.0f;
    cre.set[cre.which][id] = fm_clampf(v, p->min, p->max);
    set_dirty = 1;
    cm_send(-2, 0.0f);
}

static void seal_text(int id, char *b)        /* a setting as the screen shows it */
{
    const ch_cparam_t *p = &CRE->p[id];
    const float v = cre.set[cre.which][id];
    b[0] = 0;
    if (p->unit[0] == 'S' && p->unit[1] == 'T') {   /* semitones, signed */
        if (v > 0.05f)
            text_cat(b, "+");
        fmt1(v, b + (v > 0.05f));
    } else if (p->unit[0]) {
        fmt1(v, b);
        if (v >= 100.0f)
            int_text((int32_t)(v + 0.5f), b);
    } else {
        int_text((int32_t)((v - p->min) / (p->max - p->min) * 100.0f + 0.5f), b);
        text_cat(b, "%");
    }
}

static void seal_page_say(void)
{
    char t[24] = "PAGE ";
    t[5] = (char)('1' + seal_page);
    t[6] = ':';
    t[7] = ' ';
    t[8] = 0;
    text_cat(t, CRE_PAGE_NAME[cre.which][seal_page]);
    app_say(CRE->name, t);
}

/* ---------------------------------------------------------------------- the tunes --- */
/* ENV (0.8.0): CHIMERA's Sing mode. PLAY sings the word slot on a tune (CHIMERA's six: the words fitted to its phrases,
 * engine/ch_song.c) and stops it; with no words it sings "la". PRESET picks the tune, K1 its tempo, K2 its key, K3 the
 * vibrato, K4 the word slot; ALGORITHM the body (each sings in its octave). The song is planned a phrase at a time,
 * a second before it is due (a whole song is more gestures than the mailbox carries). SAVE keeps the choices
 * (OBJ_SONG); ENV or HOME leaves */
#define SONG_MAGIC 0x46565431u                /* "FVT1" */
#define NMELODIES_ALL 4                       /* (the MELODY page's, after CHIMERA's six: SING sings them too) */
static struct {
    uint32_t magic;
    uint8_t tune, tempo, vibrato;             /* the tune; the tempo in % of its own (50..150); the vibrato (cents) */
    int8_t key;                               /* semitones (-12..12) */
} sng;
static ch_syl_t sg_syl[CH_SYL_MAX] PLAT_POOL;
static ch_sung_t sg_note[CH_SONG_MAX] PLAT_POOL;
static float sg_time[CH_SONG_MAX + 1];
static int sg_n, sg_next, sg_nsyl;
static float mel_end[CH_SONG_MAX];            /* MELODY: each note's end (a rest after it: before the next's start) */
static int8_t mel_sung = -1;                  /* the melody being sung (the MELODY page's, or one SING picked), -1 a tune */
static float mel_half = 0.3f;                 /* its step (s: an eighth at its tempo) */
static char sg_la[CH_SONG_MAX * 3];
#define SONG_LEAD 0.35f                       /* the first beat after PLAY (s: CHIMERA's) */

static void song_load(void)
{
    if (plat_store_load(OBJ_SONG, &sng, sizeof sng) == (int)sizeof sng && sng.magic == SONG_MAGIC && sng.tune < CH_NTUNES + NMELODIES_ALL
        && sng.tempo >= 50 && sng.tempo <= 150 && sng.key >= -12 && sng.key <= 12 && sng.vibrato <= 100)
        return;
    sng.magic = SONG_MAGIC;
    sng.tune = 0;
    sng.tempo = 100;
    sng.key = 0;
    sng.vibrato = 35;
}

static void tune_name(int i, char *t)         /* a tune's name: CHIMERA's, or MELODY N (the MELODY page's) */
{
    t[0] = 0;
    if (i < CH_NTUNES) {
        text_cat(t, CH_TUNES[i].name);
    } else {
        text_cat(t, "MELODY ");
        t[7] = (char)('1' + i - CH_NTUNES);
        t[8] = 0;
    }
}

static void song_say(const char *what)
{
    char t[16];
    tune_name(sng.tune, t);
    app_say(what, t);
}

/* the song's time (s, the first beat at SONG_LEAD; -1 none): the voice's clock, or in the Bestiary the main loop's (the
 * creature's notes are sent from here, as its keys are) */
static uint32_t beast_ms0;
static float sing_now(void)
{
    if (sg.beast)
        return sg.playing ? (float)(plat_ms() - beast_ms0) / 1000.0f : -1.0f;
    return song_now;
}

static void song_stop(void)
{
    if (!sg.playing)
        return;
    sg.playing = 0;
    if (sg.beast) {                           /* the creature: its note let go */
        beast_release();
        return;
    }
    pend.n = pend.n_release = 0;               /* the score cleared: the song ends where it is */
    pend.clear = 1;
    pend.song = 0;
    mb_post();
}

/* the next phrase into the mailbox, when it is due within a second (and the mailbox is free) */
static void song_tick(void)
{
    const float now = sing_now();
    ch_plan_t plan;
    if (sg.beast) {                           /* the Bestiary: the creature's notes, from here */
        beast_tick();
        return;
    }
    if (!sg.playing)
        return;
    if (sg_next >= sg_n) {                    /* all sent: done when the last note has ended */
        if (now >= 0.0f && now > SONG_LEAD + sg_time[sg_n] + 0.4f) {
            sg.playing = 0;
            ui.redraw = 1;
        }
        return;
    }
    if (mb_full || pend.n || (sg_next > 0 && (now < 0.0f || now + 1.0f < SONG_LEAD + sg_time[sg_next])))
        return;
    plan.g = pend.g;
    plan.cap = MB_G;
    plan.n = 0;
    plan.dropped = 0;
    {
        const int first = sg_next == 0;
        if (mel_sung >= 0) {                  /* a melody: the phrase's last note ends where its rest begins */
            int j = sg_next;
            float keep;
            while (j < sg_n - 1 && !sg_note[j].phrase_end && j - sg_next < 23)   /* (the planner's phrase: ch_speech.c) */
                j++;
            keep = sg_time[j + 1];
            sg_time[j + 1] = mel_end[j];
            sg_next = ch_song_plan_phrase(sg_note, sg_n, sg_next, sg_syl, sg_time, SONG_LEAD, (float)sng.vibrato, &plan);
            sg_time[j + 1] = keep;
        } else {
            sg_next = ch_song_plan_phrase(sg_note, sg_n, sg_next, sg_syl, sg_time, SONG_LEAD, (float)sng.vibrato, &plan);
        }
        pend.n = plan.n;
        pend.n_release = 0;
        pend.clear = (uint8_t)first;          /* (the first clears what was in the score) */
        pend.song = (uint8_t)(first ? 1 : 2);
    }
    mb_post();
}

static void mel_start_at(int m, float tempo, int shift);
static void song_start(void)
{
    if (sg.beast) {                           /* the Bestiary: the tune's notes (or a melody's), on the creature */
        beast_start(sng.tune, (float)sng.tempo / 100.0f, sng.key);
        return;
    }
    const ch_tune_t *t = &CH_TUNES[sng.tune < CH_NTUNES ? sng.tune : 0];
    if (sng.tune >= CH_NTUNES) {              /* one of the MELODY page's: as written, at SING's tempo and key */
        mel_start_at(sng.tune - CH_NTUNES, (float)sng.tempo / 100.0f, sng.key);
        return;
    }
    mel_sung = -1;
    const int oct = body_hum == 0 ? t->oct_male : body_hum == 1 ? t->oct_female : t->oct_child;
    sg_nsyl = ch_lyrics(lyric, words_lookup, 0, sg_syl, CH_SYL_MAX);
    if (!sg_nsyl) {                           /* no words: "la" on every note (fit.ts la) */
        int k, q, i = 0;
        for (k = 0; k < t->n_phrases; k++) {
            for (q = 0; q < t->phrase_len[k] && i + 4 < (int)sizeof sg_la; q++) {
                memcpy(sg_la + i, "la ", 3);
                i += 3;
            }
            if (i > 0)
                sg_la[i - 1] = '\n';
        }
        sg_la[i] = 0;
        sg_nsyl = ch_lyrics(sg_la, 0, 0, sg_syl, CH_SYL_MAX);
    }
    sg_n = ch_song_fit(t, sg_syl, sg_nsyl, 12 * oct + sng.key, sg_note, CH_SONG_MAX);
    if (!sg_n)
        return;
    ch_song_times(sg_note, sg_n, t->bpm * (float)sng.tempo / 100.0f, sg_time);
    sg_next = 0;
    sg.playing = 1;
    live.sounding = 0;
    live.n_keys = 0;
    voice_note = -1;
    song_tick();                              /* (the first phrase now) */
}

/* the note sounding now (-1: none), from the song's clock */
static int song_note_now(void)
{
    const float t = sing_now() - SONG_LEAD;
    int k;
    if (!sg.playing || sing_now() < 0.0f || t < 0.0f)
        return -1;
    for (k = 0; k < sg_n; k++)
        if (mel_sung >= 0 || sg.beast ? t >= sg_time[k] && t < mel_end[k] : t < sg_time[k + 1])   /* (a melody's rest: none) */
            return k;
    return -1;
}

/* the debug page's K1 and K2, over whichever page is beneath it (not kept): the anatomy's rate, the treble shelf.
 * Returns 1 when the turn was theirs */
static int debug_knob(uint32_t k, int32_t e)
{
    if (!page_debug)
        return 0;
    if (k == EN_K1) {
        static const uint8_t R[3] = {8, 4, 2};   /* (every step overloads core 1: not offered) */
        int i = anat_every == 8 ? 0 : anat_every == 4 ? 1 : 2;
        i = clamp(i + (e > 0 ? 1 : -1), 0, 2);   /* (stops at either end: no jump from 8 to 2) */
        anat_every = R[i];
        app_say("ANATOMY", anat_every == 8 ? "EVERY 8TH STEP" : anat_every == 4 ? "EVERY 4TH STEP" : "EVERY 2ND STEP");
        return 1;
    }
    if (k == EN_K2) {
        shelf_on = (uint8_t)(e > 0);
        app_say("TREBLE SHELF", shelf_on ? "ON: -7 DB" : "OFF");
        return 1;
    }
    return 0;
}

/* GLO: the debug page (the timings) / the page under it, from anywhere but the word editor */
static void debug_toggle(void)
{
    page_debug = (uint8_t)!page_debug;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

static void song_open(void)
{
    if (vowel_mode)
        mode_set(0);
    pend.n = pend.n_release = 0;
    pend.clear = 1;
    live.sounding = 0;
    live.n_keys = 0;
    voice_note = -1;
    mb_post();
    sg.on = 1;
    sg.playing = 0;
    sg.mel = 0;
    sg.beast = (uint8_t)(body_cur == BODY_SEAL);
    vp_send();
    song_say("SING");
}

static void song_close(void)
{
    song_stop();
    sg.on = 0;
    if (sg.beast) {                           /* back to the creature's page */
        app_say(sg.dirty ? (sg.mel ? "MELODY UNSAVED" : "SING UNSAVED") : "BESTIARY", sg.dirty ? "SAVE FIRST" : CRE->name);
        sg.mel = sg.edit = sg.beast = 0;
        return;
    }
    if (sg.mel)
        app_say(sg.dirty ? "MELODY UNSAVED" : "WORDS", sg.dirty ? "SAVE IN MELODY" : "KEYS SING");
    else
        app_say(sg.dirty ? "SING UNSAVED" : "WORDS", sg.dirty ? "SAVE IN SING" : "KEYS SING");
    sg.mel = sg.edit = 0;
}

static void sg_input(uint32_t keys, uint32_t btn, uint32_t now)
{
    uint32_t k;
    int32_t e;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            if (sg.playing) {
                song_stop();
                song_say("STOP");
            } else {
                song_start();
                if (sg.playing)
                    song_say(sg.beast ? "PLAYING" : "SINGING");
                else
                    song_say(sng.tune >= CH_NTUNES ? "NO NOTES" : "NO WORDS");
            }
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SONG, &sng, sizeof sng) == 0 && plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0
                && plat_store_save(OBJ_CREATURES, &cre, sizeof cre) == 0) {
                sg.dirty = 0;
                set_dirty = 0;
                song_say("SAVED");
            } else {
                app_say("SAVE", "FAILED");
            }
        } else if (k == B_ENV || k == B_HOME) {
            song_close();
        } else if (k == B_GLO) {
            debug_toggle();
        }
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (debug_knob(k, e)) {
        } else if (k == EN_PRESET) {          /* the tune (from the next PLAY) */
            const int nt = CH_NTUNES + NMELODIES_ALL, step = e > 0 ? 1 : -1;
            int i = sng.tune, n = e > 0 ? e : -e, tries;
            while (n--)                       /* (an empty melody passed over) */
                for (tries = 0; tries < nt; tries++) {
                    i = ((i + step) % nt + nt) % nt;
                    if (i < CH_NTUNES || mel_count(i - CH_NTUNES))
                        break;
                }
            sng.tune = (uint8_t)i;
            sg.dirty = 1;
            song_say("TUNE");
        } else if (k == EN_K1) {              /* tempo, x0.5 .. x1.5 */
            sng.tempo = (uint8_t)clamp(sng.tempo + 5 * e, 50, 150);
            sg.dirty = 1;
        } else if (k == EN_K2) {              /* key, -12 .. +12 semitones */
            sng.key = (int8_t)clamp(sng.key + e, -12, 12);
            sg.dirty = 1;
        } else if (sg.beast && k == EN_K3) {  /* (the Bestiary: no vibrato) */
        } else if (sg.beast && k == EN_K4) {  /* the Bestiary: how the creature plays a note */
            beast_sound_turn(e);
        } else if (sg.beast && k == EN_ALGO) {   /* the Bestiary: another creature */
            song_stop();
            creature_pick(e);
        } else if (k == EN_K3) {              /* vibrato, 0 .. 100 cents */
            sng.vibrato = (uint8_t)clamp(sng.vibrato + 5 * e, 0, 100);
            sg.dirty = 1;
        } else if (k == EN_K4) {              /* the word slot */
            int i = set.slot, step = e > 0 ? 1 : -1, tries;
            for (tries = 0; tries < 8; tries++) {
                i = (i + step + 8) % 8;
                if (slot_text[i])
                    break;
            }
            words_select(i);
            set_dirty = 1;
        } else if (k == EN_ALGO) {            /* the body, as everywhere */
            knob_turn(K_BODY, e);
        }
    }
    body_settle(now);
    (void)keys;
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | 1u << B_ENV | (sg.playing ? 1u << B_PLAY : 0u) | (sg.dirty ? 1u << B_SAVE : 0u),
              keys | midi_keys(set.octave));
}

/* ------------------------------------------------------------------- the MELODY page --- */
/* SEQ twice (FMB Vocal): your own melody, sung. Four melodies (PRESET), each 32 steps of an eighth note: a step is a
 * note, a rest or a tie (the note before held on). PLAY sings the word slot on it, a syllable a note, a tie holding
 * the vowel, a rest a breath; the melody again until every syllable is sung (the words again to finish its last
 * time). EDIT: step editing, a key writes its note at the cursor and moves on, OCT- a rest, OCT+ a tie, K3 the cursor;
 * stopped, the keys sing as in WORDS (a note to try). K1 the tempo, K2 the key, K4 the word slot, ALGORITHM the body.
 * SAVE keeps the melodies (OBJ_MELODY); SEQ goes back to the beatbox, HOME to the words. Sung by the SING page's planner (the notes built here, not
 * fitted: your rhythm as it is) */
#define MEL_STEPS 32
#define NMELODIES 4
#define MEL_REST 0
#define MEL_TIE 1
#define MELODY_MAGIC 0x46564D31u              /* "FVM1" */
typedef struct {
    uint8_t bpm;
    int8_t key;
    uint8_t s[MEL_STEPS];                     /* MEL_REST, MEL_TIE, else the MIDI note */
    uint8_t rsv[2];
} melody_t;
static struct {
    uint32_t magic;
    melody_t m[NMELODIES];
    uint8_t slot, rsv[3];
} mel;
#define MEL (mel.m[mel.slot])

static void mel_factory(int i)
{
    static const uint8_t A[MEL_STEPS] = {60, 1, 64, 1, 67, 1, 1, 0, 69, 67, 64, 1, 62, 1, 1, 0,
                                         60, 1, 64, 67, 72, 1, 71, 1, 69, 1, 67, 1, 1, 1, 0, 0};
    static const uint8_t B[MEL_STEPS] = {72, 1, 71, 69, 67, 1, 65, 64, 62, 1, 1, 0, 64, 65, 67, 1,
                                         69, 1, 67, 65, 64, 1, 62, 1, 60, 1, 1, 1, 0, 0, 0, 0};
    melody_t *m = &mel.m[i];
    memset(m, 0, sizeof *m);
    m->bpm = 100;
    if (i == 0)
        memcpy(m->s, A, MEL_STEPS);
    else if (i == 1)
        memcpy(m->s, B, MEL_STEPS);
}

static void melody_load(void)
{
    int i, ok = 0;
    if (plat_store_load(OBJ_MELODY, &mel, sizeof mel) == (int)sizeof mel && mel.magic == MELODY_MAGIC && mel.slot < NMELODIES) {
        ok = 1;
        for (i = 0; i < NMELODIES; i++)
            ok &= mel.m[i].bpm >= 40 && mel.m[i].bpm <= 200 && mel.m[i].key >= -12 && mel.m[i].key <= 12;
    }
    if (ok)
        return;
    memset(&mel, 0, sizeof mel);
    mel.magic = MELODY_MAGIC;
    for (i = 0; i < NMELODIES; i++)
        mel_factory(i);
}

static void mel_say(const char *what)
{
    char t[16] = "MELODY ";
    t[7] = (char)('1' + mel.slot);
    t[8] = 0;
    app_say(what, t);
}

typedef char mel_n_check[NMELODIES == NMELODIES_ALL ? 1 : -1];   /* (SING's count of them: the same) */

static int mel_count(int m)                   /* the notes melody m has (a tie after nothing is a rest) */
{
    int i, n = 0;
    for (i = 0; i < MEL_STEPS; i++)
        n += mel.m[m].s[i] > MEL_TIE;
    return n;
}

static int mel_notes(void) { return mel_count(mel.slot); }

/* the song from the melody: each note its syllable (the words in order, again from the first to finish the melody's
 * last time), its start and its end (s from the first beat), a phrase ending at a rest and at the melody's end */
/* melody m into the song's notes, `passes` times (each note its syllable, nsyl > 0: the words in order, again from the
 * first; else none), its tempo x tempo, shift semitones more: each note's start and end. Returns the notes */
static int mel_build(int m, float tempo, int shift, int passes, int nsyl)
{
    const melody_t *ml = &mel.m[m];
    const float half = 30.0f / ((float)ml->bpm * tempo);   /* (a step: an eighth, half a beat) */
    int p, i, n = 0;
    for (p = 0; p < passes; p++)
        for (i = 0; i < MEL_STEPS && n < CH_SONG_MAX; i++) {
            int j = i + 1;
            if (ml->s[i] <= MEL_TIE)
                continue;
            while (j < MEL_STEPS && ml->s[j] == MEL_TIE)
                j++;
            sg_note[n].midi = (uint8_t)clamp(ml->s[i] + ml->key + shift, 24, 108);
            sg_note[n].beats = (float)(j - i) * 0.5f;
            sg_note[n].syl = (int16_t)(nsyl > 0 ? n % nsyl : 0);
            sg_note[n].held = 0;
            sg_note[n].phrase_end = (uint8_t)(j >= MEL_STEPS || ml->s[j] == MEL_REST);
            sg_time[n] = (float)(p * MEL_STEPS + i) * half;
            mel_end[n] = (float)(p * MEL_STEPS + j) * half;
            n++;
        }
    sg_n = n;
    sg_time[n] = n ? mel_end[n - 1] : 0.0f;
    mel_sung = (int8_t)m;
    mel_half = half;
    return n;
}

static void mel_start_at(int m, float tempo, int shift)   /* melody m sung, its tempo x tempo, shift semitones more */
{
    const int nn = mel_count(m);
    int passes, i;
    if (!nn)
        return;
    sg_nsyl = ch_lyrics(lyric, words_lookup, 0, sg_syl, CH_SYL_MAX);
    if (!sg_nsyl) {                           /* no words: "la" on every note */
        int k = 0;
        for (i = 0; i < nn && k + 4 < (int)sizeof sg_la; i++) {
            memcpy(sg_la + k, "la ", 3);
            k += 3;
        }
        sg_la[k] = 0;
        sg_nsyl = ch_lyrics(sg_la, 0, 0, sg_syl, CH_SYL_MAX);
        if (!sg_nsyl)
            return;
    }
    passes = (sg_nsyl + nn - 1) / nn;
    while (passes > 1 && passes * nn > CH_SONG_MAX)
        passes--;
    mel_build(m, tempo, shift, passes, sg_nsyl);
    sg_next = 0;
    sg.playing = 1;
    live.sounding = 0;
    live.n_keys = 0;
    voice_note = -1;
    song_tick();                              /* (the first phrase now) */
}

static void mel_start(void)
{
    if (sg.beast)                             /* the Bestiary: the melody on the creature */
        beast_start(CH_NTUNES + mel.slot, 1.0f, 0);
    else
        mel_start_at(mel.slot, 1.0f, 0);
}

/* the note sounding now (-1: none, a rest), and the step the song is at (-1: not playing) */
static int mel_note_now(void) { return song_note_now(); }

/* where the words start again (the slot's first syllable after its last), as steps of the melody: up to cap, each
 * its step and the time through the melody it falls in (0 the first); none when there are no words (the la la la) */
static int mel_wraps(int *step, uint8_t *pass, int cap)
{
    static uint32_t sum_was = 1u;
    static int nsyl_was;
    const int nn = mel_notes();
    int nsyl, passes, k, n = 0, q, i, c;
    if (!nn || !lyric[0] || sg.beast)        /* (the Bestiary: no words) */
        return 0;
    if (sg.playing) {                         /* (singing: the song's count; its syllables in use, not counted again) */
        nsyl = sg_nsyl;
    } else {                                  /* stopped: the slot's syllables, counted again when its words change */
        uint32_t sum = 2166136261u;
        for (i = 0; lyric[i]; i++)
            sum = (sum ^ (uint8_t)lyric[i]) * 16777619u;
        if (sum != sum_was) {
            sum_was = sum;
            nsyl_was = ch_lyrics(lyric, words_lookup, 0, sg_syl, CH_SYL_MAX);
        }
        nsyl = nsyl_was;
    }
    if (nsyl <= 0)
        return 0;
    passes = (nsyl + nn - 1) / nn;
    while (passes > 1 && passes * nn > CH_SONG_MAX)
        passes--;
    for (k = nsyl; k < passes * nn && n < cap; k += nsyl) {   /* (the song's note k sings the first syllable again) */
        q = k % nn;
        for (i = 0, c = -1; i < MEL_STEPS; i++)   /* the q-th note's step */
            if (MEL.s[i] > MEL_TIE && ++c == q)
                break;
        step[n] = i;
        pass[n] = (uint8_t)(k / nn);
        n++;
    }
    return n;
}

static int mel_pass_now(void)                 /* the time through the melody being sung (0 the first; -1 stopped) */
{
    const float t = sing_now() - SONG_LEAD;
    if (!sg.playing || sing_now() < 0.0f || t < 0.0f)
        return -1;
    return (int)(t / (mel_half * (float)MEL_STEPS));
}

static int mel_step_now(void)
{
    const float t = sing_now() - SONG_LEAD;
    if (!sg.playing || sing_now() < 0.0f || t < 0.0f)
        return -1;
    return (int)(t / mel_half) % MEL_STEPS;
}

static void mel_open(void)
{
    song_open();                              /* (the SING page's: the words' notes stopped) */
    sg.mel = 1;
    sg.edit = 0;
    mel_say("MELODY");
}

static void mel_set(uint8_t v)                /* the step at the cursor written, the cursor on */
{
    MEL.s[sg.cur] = v;
    sg.cur = (uint8_t)((sg.cur + 1) % MEL_STEPS);
    sg.dirty = 1;
}

/* MELODY's ALGORITHM (0.4.0): one list, the voice's bodies then the Bestiary's creatures (MALE, FEMALE, CHILD,
 * BLENDED, then the creatures in CRE_ORDER: the robot, Squidlad, the cat ... the elephant), so the melody is sung or
 * played without leaving the page. A creature plays it in its own voice (K4 SOUND: HELD, the robot's wub, at first), no
 * word slot. Up past BLENDED: the first creature; down past it: BLENDED again (the list's last body, so the way back is
 * the way up) */
static void mel_to_beast(int pos)              /* the voice's MELODY: the creature at `pos` in CRE_ORDER plays it */
{
    song_stop();
    body_pending = -1;
    kv[K_BODY] = (float)BODY_BLENDED;
    cre.which = CRE_ORDER[clamp(pos, 0, NCRE - 1)];
    body_cur = BODY_SEAL;
    seal_mode_changed();
    body_req = BODY_SEAL;
    sg.beast = 1;
    set_dirty = 1;
    app_say("MELODY", CRE->name);
}

static void mel_to_voice(void)                 /* a creature's MELODY: the voice sings it again, BLENDED */
{
    char t[24];
    song_stop();
    kv[K_BODY] = (float)BODY_BLENDED;
    body_cur = 0xFFu;                         /* (so body_apply switches) */
    body_apply();
    sg.beast = 0;
    set_dirty = 1;
    body_label(kv[K_GENDER], kv[K_AGE], t);
    app_say("MELODY", t);
}

static void mel_input(uint32_t keys, uint32_t btn, uint32_t now)
{
    uint32_t k;
    int32_t e;
    for (k = 0; k < NKEYS; k++) {             /* the keys: sing (stopped); in step editing, also written */
        const uint32_t bit = 1u << k;
        const int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            ui.vel[k] = (uint8_t)touch_vel(k);
            ui.last_key = (int32_t)k;
            if (sg.edit && !sg.playing)
                mel_set((uint8_t)clamp(note, 24, 108));
            live_on(note, ui.vel[k]);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            live_off(note);
        }
    }
    for (k = 0; k < NB; k++) {
        const uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            if (sg.playing) {
                song_stop();
                mel_say("STOP");
            } else {
                mel_start();
                if (sg.playing)
                    mel_say(sg.beast ? "PLAYING" : "SINGING");
                else
                    app_say("MELODY", "NO NOTES");
            }
        } else if (k == B_EDIT) {
            sg.edit = (uint8_t)!sg.edit;
            app_say(sg.edit ? "STEP EDIT" : "KEYS SING", sg.edit ? "KEYS WRITE NOTES" : "PLAY: SING");
        } else if (k == B_OCTDN) {
            if (sg.edit)
                mel_set(MEL_REST);
            else
                octave_step(-1);
        } else if (k == B_OCTUP) {
            if (sg.edit)
                mel_set(MEL_TIE);
            else
                octave_step(1);
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_MELODY, &mel, sizeof mel) == 0 && plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0
                && plat_store_save(OBJ_CREATURES, &cre, sizeof cre) == 0) {
                sg.dirty = 0;
                set_dirty = 0;
                mel_say("SAVED");
            } else {
                app_say("SAVE", "FAILED");
            }
        } else if (k == B_HOME) {             /* the words */
            song_close();
        } else if (k == B_SEQ && sg.beast) {  /* the Bestiary (no beatbox): back to the creature */
            song_close();
        } else if (k == B_SEQ) {              /* SEQ again: back to the beatbox (HOME: the words) */
            const uint8_t unsaved = sg.dirty;
            song_stop();
            sg.on = sg.mel = sg.edit = 0;
            beat_open();
            if (unsaved)                      /* (the melody's changes not saved: still said) */
                app_say("MELODY UNSAVED", "SAVE IN MELODY");
        } else if (k == B_GLO) {
            debug_toggle();
        }
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (debug_knob(k, e)) {
        } else if (k == EN_PRESET) {          /* another melody (from the next PLAY) */
            mel.slot = (uint8_t)(((mel.slot + e) % NMELODIES + NMELODIES) % NMELODIES);
            sg.dirty = 1;
            mel_say("MELODY");
        } else if (k == EN_K1) {              /* tempo, 40 .. 200 BPM */
            MEL.bpm = (uint8_t)clamp(MEL.bpm + e, 40, 200);
            sg.dirty = 1;
        } else if (k == EN_K2) {              /* key, -12 .. +12 semitones */
            MEL.key = (int8_t)clamp(MEL.key + e, -12, 12);
            sg.dirty = 1;
        } else if (k == EN_K3) {              /* the step cursor */
            sg.cur = (uint8_t)(((sg.cur + e) % MEL_STEPS + MEL_STEPS) % MEL_STEPS);
        } else if (sg.beast && k == EN_K4) {  /* the Bestiary: how the creature plays a note */
            beast_sound_turn(e);
        } else if (sg.beast && k == EN_ALGO) {   /* a creature: the next in CRE_ORDER; down past the first, the voice;
                                              * the last the list's end */
            const int at = cre_pos(cre.which), to = clamp(at + (int)e, -1, NCRE - 1);
            song_stop();
            if (to < 0)
                mel_to_voice();
            else if (to != at) {
                creature_pick(to - at);
                app_say("MELODY", CRE->name);
            }
        } else if (k == EN_K4) {              /* the word slot */
            int i = set.slot, step = e > 0 ? 1 : -1, tries;
            for (tries = 0; tries < 8; tries++) {
                i = (i + step + 8) % 8;
                if (slot_text[i])
                    break;
            }
            words_select(i);
            set_dirty = 1;
        } else if (k == EN_ALGO) {            /* the body; up past BLENDED, the creatures */
            const int at = body_pending >= 0 ? body_pending : (int)(kv[K_BODY] + 0.5f);
            if (at + (int)e > BODY_BLENDED)
                mel_to_beast(at + (int)e - BODY_BLENDED - 1);
            else
                knob_turn(K_BODY, e);
        }
    }
    body_settle(now);
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | 1u << B_SEQ | (sg.playing ? 1u << B_PLAY : 0u) | (sg.edit ? 1u << B_EDIT : 0u) | (sg.dirty ? 1u << B_SAVE : 0u),
              keys | midi_keys(set.octave));
}

/* -------------------------------------------------------- the Bestiary sings (0.2.0) --- */
/* SING (ENV) and MELODY (SEQ) opened in the Bestiary: the creature plays the notes, a tune's or a melody's, from the
 * main loop as its keys do (each note's pitch the octave that centres its range on middle C, as a key's). Its SOUND
 * (K4, each creature's own, kept by SAVE): 0 its voice held for the note (a tie held, a rest silent, a note again
 * at the same pitch let go a moment first), else one of its calls at the note's pitch */
static int8_t beast_k = -1;                   /* the note it plays (-1 none) */

static const char *beast_sound_name(void)     /* HELD, or the call's name */
{
    const int s = cre.sound[cre.which];
    return s && s <= CRE->ncalls ? CRE->calls[s - 1].name : "HELD";
}

static void beast_sound_turn(int32_t e)
{
    const int n = CRE->ncalls + 1;
    cre.sound[cre.which] = (uint8_t)(((cre.sound[cre.which] + e) % n + n) % n);
    set_dirty = 1;
    app_say("SOUND", beast_sound_name());
}

static void beast_release(void)               /* its note let go (stopped, ended, another creature) */
{
    if (beast_k >= 0 && beast_k < sg_n && cre.sound[cre.which] == 0)
        seal_off(sg_note[beast_k].midi);
    beast_k = -1;
}

/* tune t's notes (a tune of CHIMERA's: once through, a breath at each phrase's end; or the MELODY page's), the
 * creature playing them from now */
static void beast_start(int tune, float tempo, int shift)
{
    int n = 0, k;
    if (tune >= CH_NTUNES) {                  /* a melody, once through, as written */
        if (!mel_count(tune - CH_NTUNES))
            return;
        n = mel_build(tune - CH_NTUNES, tempo, shift, 1, 0);
    } else {
        const ch_tune_t *t = &CH_TUNES[tune];
        const float beat = 60.0f / (t->bpm * tempo);
        float at = 0;
        int ph, q, i = 0;
        for (ph = 0; ph < t->n_phrases; ph++)
            for (q = 0; q < t->phrase_len[ph] && n < CH_SONG_MAX; q++, i++) {
                const float d = (float)t->notes[i].half_beats * 0.5f * beat;
                const int end = q == t->phrase_len[ph] - 1;
                sg_note[n].midi = (uint8_t)clamp(t->notes[i].midi + shift, 24, 108);
                sg_note[n].beats = (float)t->notes[i].half_beats * 0.5f;
                sg_note[n].syl = 0;
                sg_note[n].held = 0;
                sg_note[n].phrase_end = (uint8_t)end;
                sg_time[n] = at;
                mel_end[n] = at + d - (end ? fm_minf(0.12f, 0.3f * d) : 0.0f);   /* (a breath before the next) */
                at += d;
                n++;
            }
        sg_n = n;
        sg_time[n] = at;
        mel_sung = -1;
    }
    for (k = 0; k + 1 < n; k++)               /* a note again at the same pitch: let go a moment first */
        if (sg_note[k + 1].midi == sg_note[k].midi && mel_end[k] > sg_time[k + 1] - 0.001f)
            mel_end[k] -= fm_minf(0.04f, 0.3f * (mel_end[k] - sg_time[k]));
    beast_release();
    sg_next = sg_n;                           /* (nothing for the voice's planner) */
    beast_ms0 = plat_ms();
    sg.playing = 1;
    beast_tick();
}

/* the main loop's: the note now, played as SOUND says; done after the last */
static void beast_tick(void)
{
    const int k = song_note_now();
    if (!sg.playing)
        return;
    if (k != beast_k) {
        const int sd = cre.sound[cre.which];
        const int was = beast_k >= 0 ? sg_note[beast_k].midi : -1;
        if (k >= 0 && sd == 0)
            seal_on(sg_note[k].midi);
        else if (k >= 0 && sd <= CRE->ncalls)
            cm_send(sd - 1, ch_ckey_hz(&CRE->p[cre_pitch(CRE)], (float)sg_note[k].midi));
        if (was >= 0 && sd == 0 && !(k >= 0 && sg_note[k].midi == was))
            seal_off(was);
        beast_k = (int8_t)k;
        ui.redraw = 1;
    }
    if (sing_now() - SONG_LEAD > sg_time[sg_n] + 0.3f) {   /* the last note over */
        sg.playing = 0;
        beast_release();
        ui.redraw = 1;
    }
}

/* ------------------------------------------------------------------- the beatbox --- */
/* SEQ (0.7.0): CHIMERA's beatboxer. The white keys are pads for its sounds (the lowest nine: kick, snare, hi-hat,
 * open hat, rimshot, inward snare, tongue click, lip pop, crash; the next seven again from the kick), the black keys
 * hum the bass an octave below the key. PLAY starts and stops the beat; PRESET picks one of eight (CHIMERA's seven
 * and one empty), K1 the tempo, K2 the swing. EDIT writes steps: K3 (or the pads) moves the cursor, a pad writes its
 * sound into the step (FX held: accented), OCT- clears it and goes back, OCT+ clears it and goes on. SAVE keeps the
 * beats; HOME or SEQ leaves. */
#define NBEATS 8
#define BEATS_MAGIC 0x46564231u               /* "FVB1" */
typedef struct {
    uint32_t magic;
    struct {
        ch_beat_step_t s[BEAT_STEPS];
        uint8_t bpm, swing;                   /* bpm 40..200; swing in % (0..60) */
    } b[NBEATS];
} beats_t;
static beats_t beats PLAT_POOL;
static const uint8_t PADS[9] = {CH_BEAT_KICK, CH_BEAT_SNARE, CH_BEAT_HAT, CH_BEAT_OPEN_HAT, CH_BEAT_RIMSHOT,
                                CH_BEAT_INWARD, CH_BEAT_CLICK, CH_BEAT_LIP_POP, CH_BEAT_CRASH};

static const char *beat_name(int i) { return i < CH_BEAT_NPRESETS ? CH_BEAT_PRESETS[i].name : "MY BEAT"; }

static void beat_factory(int i)               /* CHIMERA's preset (or an empty beat) */
{
    int j, n = 0;
    for (j = 0; j < BEAT_STEPS; j++) {
        beats.b[i].s[j].id = -1;
        beats.b[i].s[j].note = beats.b[i].s[j].accent = 0;
    }
    beats.b[i].bpm = 96;
    beats.b[i].swing = 0;
    if (i < CH_BEAT_NPRESETS) {
        n = ch_beat_parse(CH_BEAT_PRESETS[i].text, beats.b[i].s, BEAT_STEPS);
        beats.b[i].bpm = (uint8_t)(CH_BEAT_PRESETS[i].bpm + 0.5f);
        beats.b[i].swing = (uint8_t)(CH_BEAT_PRESETS[i].swing * 100.0f + 0.5f);
    }
    (void)n;
}

static void beats_load(void)
{
    int i;
    if (plat_store_load(OBJ_BEATS, &beats, sizeof beats) == (int)sizeof beats && beats.magic == BEATS_MAGIC)
        return;
    beats.magic = BEATS_MAGIC;
    for (i = 0; i < NBEATS; i++)
        beat_factory(i);
}

/* the beat shown, to the audio interrupt (and whether it plays) */
static void beat_send(void)
{
    beat_cmd_t c;
    int j;
    for (j = 0; j < BEAT_STEPS; j++)
        c.s[j] = beats.b[bt.slot].s[j];
    c.bpm = (float)beats.b[bt.slot].bpm;
    c.swing = (float)beats.b[bt.slot].swing / 100.0f;
    c.play = bt.play;
    bc_dirty = 0;
    bc_out = c;
    bc_dirty = 1;
}

/* a key's pad: its sound (and the hum's note), -1 none */
static int beat_pad_of(uint32_t k, uint8_t *note)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    int i;
    *note = 0;
    for (i = 0; i < 16; i++)
        if (WK[i] == k)
            return PADS[i % 9];
    for (i = 0; i < 11; i++)
        if (BK[i] == k) {
            *note = (uint8_t)clamp(53 + (int32_t)k + 12 * set.octave - 12, 24, 96);
            return CH_BEAT_HUM;
        }
    return -1;
}

static void beat_pad(int id, uint8_t note, uint8_t acc)   /* a pad, as soon as the mouth can make it */
{
    const uint32_t h = pq_head;
    if (h - pq_tail >= PQ)
        return;
    pq_id[h % PQ] = (uint8_t)id;
    pq_note[h % PQ] = note;
    pq_acc[h % PQ] = acc;
    pq_head = h + 1;
}

static void beat_note(int32_t note)
{
    const int32_t k = note - 53 - 12 * set.octave;
    uint8_t nt;
    int id;
    if (k >= 0 && k < NKEYS && (id = beat_pad_of((uint32_t)k, &nt)) >= 0)
        beat_pad(id, nt, 0);
}

static void beat_say(const char *what)
{
    char t[24] = "";
    int_text(bt.slot + 1, t);
    text_cat(t, " ");
    text_cat(t, beat_name(bt.slot));
    app_say(what, t);
}

static void beat_open(void)
{
    if (vowel_mode)
        mode_set(0);
    pend.n = pend.n_release = 0;               /* the words' notes stop: their gestures go */
    pend.clear = 1;
    live.sounding = 0;
    live.n_keys = 0;
    voice_note = -1;
    mb_post();
    bt.on = 1;
    bt.edit = 0;
    bt.play = 0;
    vp_send();
    beat_send();
    beat_say("BEAT");
}

static void beat_close(void)
{
    bt.play = 0;
    beat_send();
    bt.on = 0;
    bt.edit = 0;
    vp_send();
    app_say(bt.dirty ? "BEAT UNSAVED" : "WORDS", bt.dirty ? "SAVE IN BEAT" : "KEYS SING");
}

static void beat_save(void)
{
    if (plat_store_save(OBJ_BEATS, &beats, sizeof beats) == 0) {
        bt.dirty = 0;
        beat_say("SAVED");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void beat_set(uint8_t cur, int id, uint8_t note, uint8_t acc)   /* a step written (id -1: cleared) */
{
    ch_beat_step_t *s = &beats.b[bt.slot].s[cur];
    s->id = (int8_t)id;
    s->note = id == CH_BEAT_HUM ? note : 0;
    s->accent = id >= 0 ? acc : 0;
    bt.dirty = 1;
    beat_send();
}

static void bt_input(uint32_t keys, uint32_t btn, uint32_t now)
{
    uint32_t k;
    int32_t e;
    const uint8_t fx = (uint8_t)(btn >> B_FX & 1u);
    for (k = 0; k < NKEYS; k++) {
        uint8_t note;
        int id;
        if (!(keys >> k & 1u) || (ui.keys >> k & 1u) || (id = beat_pad_of(k, &note)) < 0)
            continue;
        beat_pad(id, note, fx);
        if (bt.edit) {
            beat_set(bt.cur, id, note, fx);
            bt.cur = (uint8_t)((bt.cur + 1) % BEAT_STEPS);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            bt.play = (uint8_t)!bt.play;
            beat_send();
            beat_say(bt.play ? "PLAY" : "STOP");
        } else if (k == B_EDIT) {
            bt.edit = (uint8_t)!bt.edit;
            app_say(bt.edit ? "STEP EDIT" : "PADS", bt.edit ? "PADS WRITE STEPS" : "PADS PLAY");
        } else if (k == B_OCTDN) {
            if (bt.edit) {                    /* clear the step, back one */
                beat_set(bt.cur, -1, 0, 0);
                bt.cur = (uint8_t)((bt.cur + BEAT_STEPS - 1) % BEAT_STEPS);
            } else {
                octave_step(-1);
            }
        } else if (k == B_OCTUP) {
            if (bt.edit) {                    /* clear the step, on one */
                beat_set(bt.cur, -1, 0, 0);
                bt.cur = (uint8_t)((bt.cur + 1) % BEAT_STEPS);
            } else {
                octave_step(1);
            }
        } else if (k == B_SAVE) {
            beat_save();
        } else if (k == B_HOME) {
            beat_close();
        } else if (k == B_SEQ) {              /* SEQ again: the MELODY page (your own melody, sung) */
            const uint8_t unsaved = bt.dirty;
            beat_close();
            mel_open();
            if (unsaved)                      /* (the beat's changes not saved: still said) */
                app_say("BEAT UNSAVED", "SAVE IN BEAT");
        } else if (k == B_GLO) {
            debug_toggle();
        }
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (debug_knob(k, e)) {
        } else if (k == EN_K1) {              /* tempo */
            beats.b[bt.slot].bpm = (uint8_t)clamp(beats.b[bt.slot].bpm + e, 40, 200);
            bt.dirty = 1;
            beat_send();
        } else if (k == EN_K2) {              /* swing */
            beats.b[bt.slot].swing = (uint8_t)clamp(beats.b[bt.slot].swing + 2 * e, 0, 60);
            bt.dirty = 1;
            beat_send();
        } else if (k == EN_K3) {              /* the step cursor */
            bt.cur = (uint8_t)(((bt.cur + e) % BEAT_STEPS + BEAT_STEPS) % BEAT_STEPS);
        } else if (k == EN_PRESET) {          /* another beat (playing on, from where it is) */
            bt.slot = (uint8_t)(((bt.slot + e) % NBEATS + NBEATS) % NBEATS);
            beat_send();
            beat_say("BEAT");
        } else if (k == EN_ALGO) {            /* Body, as everywhere */
            knob_turn(K_BODY, e);
        }
    }
    body_settle(now);
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | 1u << B_SEQ | (bt.play ? 1u << B_PLAY : 0u) | (bt.edit ? 1u << B_EDIT : 0u) | (bt.dirty ? 1u << B_SAVE : 0u),
              keys | midi_keys(set.octave));
}

/* expression's targets, each pass: the hardest-pressed held key (not in the editor: its keys type), or MIDI in's
 * aftertouch while a MIDI note is held; the bend; the mod wheel into the source (vp_send) when it moves */
static void expr_update(uint32_t keys)
{
    static uint8_t mod_was;
    uint32_t k, p = 0;
    if (!ed.on)
        for (k = 0; k < NKEYS; k++)
            if ((keys >> k & 1u) && touch_pressure(k) > p)
                p = touch_pressure(k);
    if (midi_held[0] | midi_held[1] | midi_held[2] | midi_held[3]) {
        if (midi_prs > p)
            p = midi_prs;
    } else {
        midi_prs = 0;                         /* (no MIDI note: its aftertouch is over) */
    }
    xvib_target = (float)p / 127.0f * EXPR_VIB_CENTS;
    xbend_target = fm_exp2f((float)midi_bend / 8192.0f * (EXPR_BEND_SEMIS / 12.0f));
    if (midi_mod != mod_was) {
        mod_was = midi_mod;
        vp_send();
    }
}

static void input(void)
{
    static uint32_t snap_mask;               /* keys pressed with FX held: snapshots, not notes */
    static uint8_t fx_used;                  /* FX held for a snapshot: its release does not switch the mode */
    static int8_t snap_key = -1;             /* the snapshot key held (stored when held a second) */
    static uint32_t snap_at;
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    static uint8_t sel_used;                 /* SEL held for the blend: its release does not turn the page */
    const int fx_held = (int)(btn >> B_FX & 1u), home_held = (int)(btn >> B_HOME & 1u), sel_held = (int)(btn >> B_SEL & 1u);
    nq_run();                                /* MIDI in's notes, planned here */
    mb_post();                               /* what is pending, if the mailbox has been emptied */
    touch_poll();                            /* the sensed keys' presses, velocities and pressure */
    keys = touch_keys();
    expr_update(keys);
    if (ed.on) {                             /* the word editor: the keys type */
        ed_input(keys, btn, now);
        return;
    }
    if (bt.on) {                             /* the beatbox: the keys are pads */
        bt_input(keys, btn, now);
        return;
    }
    if (sg.on) {                             /* the SING page: a tune sings the words (MELODY: your own) */
        if (sg.mel)
            mel_input(keys, btn, now);
        else
            sg_input(keys, btn, now);
        return;
    }
    for (k = 0; k < NKEYS; k++) {            /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k, v;
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            v = touch_vel(k);
            ui.vel[k] = (uint8_t)v;
            ui.prs[k] = 0;
            ui.last_key = (int32_t)k;
            if (fx_held && body_cur == BODY_SEAL) {   /* the seal: FX + key, its call at the key's pitch */
                fx_used = 1;
                seal_call(note);
                continue;
            }
            if (fx_held && k < NSNAP) {      /* FX + key: a vowel snapshot (VOWEL mode) */
                char nm[12] = "SNAPSHOT ";
                int_text((int32_t)k + 1, nm + 9);
                fx_used = 1;
                snap_mask |= bit;
                mode_set(1);
                snap_recall((int)k);
                snap_key = (int8_t)k;
                snap_at = now;
                app_say(nm, "");
                continue;
            }
            live_on(note, v);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB | TO_BLE);   /* BLE: if built with it */
        } else if (!(keys & bit) && (ui.keys & bit)) {
            ui.prs[k] = 0;
            if (snap_mask & bit) {
                snap_mask &= ~bit;
                if (snap_key == (int8_t)k)
                    snap_key = -1;
                continue;
            }
            live_off(note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB | TO_BLE);
        } else if ((keys & bit) && touch_pressure(k) != ui.prs[k]) {   /* pressure: poly aftertouch */
            ui.prs[k] = touch_pressure(k);
            ui.redraw = 1;
            plat_midi_send(0x0Au | 0xA0u << 8 | (uint32_t)note << 16 | (uint32_t)ui.prs[k] << 24, TO_USB | TO_BLE);
        }
    }
    if (snap_key >= 0 && fx_held && now - snap_at >= 1000u) {   /* FX + key held a second: the vowel stored there */
        char nm[12] = "SNAPSHOT ";
        int_text((int32_t)snap_key + 1, nm + 9);
        snap_store(snap_key);
        app_say(nm, "STORED");
        snap_key = -1;
    }
    if ((ui.btn >> B_FX & 1u) && !fx_held) { /* FX released: alone, it switches WORDS <-> VOWEL (TONGUE -> VOWEL) */
        if (!fx_used && body_cur != BODY_SEAL)
            mode_set(vowel_mode == 1 ? 0 : 1);
        fx_used = 0;
        snap_key = -1;
    }
    if ((ui.btn >> B_SEL & 1u) && !sel_held) {   /* SEL released: alone, the next page (held, it blended the body) */
        if (!sel_used && body_cur != BODY_SEAL) {
            if (vowel_mode == 2)             /* TONGUE: its page, SHAPE, MOUTH */
                page = (uint8_t)(page == PAGE_TONGUE ? 1 : page == 1 ? 2 : PAGE_TONGUE);
            else                             /* pages 2-5 (EDIT until 0.5.0) */
                page = (uint8_t)(page >= 1 && page < 4 ? page + 1 : 1);
            app_say(PAGE_NAME[page], "");
        }
        sel_used = 0;
    }
    for (k = 0; k < NB; k++) {               /* a button pressed: what it does here */
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN)
            octave_step(-1);
        else if (k == B_OCTUP)
            octave_step(1);
        else if (k == B_FX)
            fx_used = 0;                         /* (it acts on release) */
        else if (k == B_HOME) {                  /* WORDS, page 1 */
            mode_set(0);
            page = 0;
            seal_page = 0;
        } else if (k == B_SEL && body_cur == BODY_SEAL) {   /* the seal's two pages */
            seal_page ^= 1u;
            seal_page_say();
        } else if (k == B_SEL) {                 /* (the page on its release: held, PRESET and SELECT blend the body) */
            sel_used = 0;
        } else if (k == B_ARP) {                 /* TONGUE (1.0.0): the tongue placed by hand, and back to WORDS */
            if (body_cur == BODY_SEAL)
                app_say("TONGUE", "LFO: BACK TO HUMAN");
            else
                mode_set(vowel_mode == 2 ? 0 : 2);
        } else if (k == B_EDIT) {                /* the word editor, on the slot sung (from the next input) */
            ed_open();
        } else if (k == B_ENV) {                 /* CHIMERA's tunes (0.8.0): a human body's */
            song_open();                     /* (in the Bestiary, the creature plays it: 0.2.0) */
        } else if (k == B_LFO) {                 /* the Bestiary (0.7.0), and back */
            bestiary_toggle();
        } else if (k == B_SEQ) {                 /* the beatbox (0.7.0): a human body's */
            if (body_cur == BODY_SEAL)       /* the Bestiary: MELODY, the creature playing (0.2.0; no beatbox) */
                mel_open();
            else
                beat_open();
        } else if (k == B_REC && FM1B_BLE)
            ui.rec_t = plat_ms();                /* a press: on release (a hold switches Bluetooth) */
        else if (k == B_GLO)                   /* the debug page (the timings), and back */
            debug_toggle(); else if (k == B_PLAY && body_cur == BODY_SEAL) {   /* the seal: its call */
            seal_call(-1);
        } else if (k == B_PLAY) {                /* speak the slot's words (in WORDS) */
            mode_set(0);
            speak_text(lyric);
            app_say("SPEAKING", "");
        } else if (k == B_SAVE) {                /* the settings, the knobs into the preset, the snapshots */
            int i;
            for (i = 0; i < NK; i++)
                presets.v[set.preset][i] = kv[i];
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0
                && plat_store_save(OBJ_PRESETS, &presets, sizeof presets) == 0
                && plat_store_save(OBJ_CREATURES, &cre, sizeof cre) == 0) {
                set_dirty = 0;
                app_say("SAVED", PRESET_NAME[set.preset]);
            } else {
                app_say("SAVE", "FAILED");
            }
        }
    }
    if (FM1B_BLE && ui.rec_t) {             /* REC: held a second, Bluetooth on / off; a press, a device */
        if (btn >> B_REC & 1u) {
            if (plat_ms() - ui.rec_t >= 1000u) {
                ui.rec_t = 0;
                ble_toggle();
            }
        } else {
            ui.rec_t = 0;
            if (FM1B_BLE_CENTRAL && !set.ble_off)
                ble_device_button();
            else
                app_say("BUTTON", "REC");
        }
    }
    for (k = 0; k < NE; k++) {               /* an encoder turned */
        char t[16];
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (k == EN_ALGO && FM1B_BLE_CENTRAL && ui.looking) {
            btdev_choose(btdev_pick() + e);
            ble_device_say();
        } else if (k == EN_SELECT && body_cur == BODY_SEAL) {
            seal_page ^= 1u;                     /* (either way: two pages) */
            seal_page_say();
        } else if (sel_held && body_cur != BODY_SEAL && (k == EN_PRESET || k == EN_SELECT)) {   /* the body's blend */
            sel_used = 1;
            blend_turn(k == EN_PRESET ? K_GENDER : K_AGE, e);
        } else if (k == EN_SELECT) {             /* the page (VOWEL's in VOWEL mode, TONGUE's in TONGUE) */
            const uint8_t *o = vowel_mode == 2 ? PAGES_T : vowel_mode ? PAGES_V : PAGES_W;
            const int n = vowel_mode == 2 ? (int)sizeof PAGES_T : vowel_mode ? (int)sizeof PAGES_V : (int)sizeof PAGES_W;
            int i, at = 0;
            for (i = 0; i < n; i++)
                if (o[i] == page)
                    at = i;
            page = o[((at + (int)e) % n + n) % n];
            app_say(PAGE_NAME[page], "");
        } else if (k == EN_ALGO) {               /* Body, always (male only for now) */
            knob_turn(K_BODY, e);
        } else if (k == EN_PRESET && home_held) {   /* HOME + PRESET: the word slot */
            int i = set.slot, step = e > 0 ? 1 : -1, tries;
            char nm[8] = "WORDS ";
            for (tries = 0; tries < 8; tries++) {
                i = (i + step + 8) % 8;
                if (slot_text[i])
                    break;
            }
            words_select(i);
            set_dirty = 1;
            nm[6] = (char)('1' + set.slot);
            nm[7] = 0;
            app_say(nm, "");
        } else if (k == EN_PRESET && body_cur == BODY_SEAL) {   /* the seal: its call */
            const int n = CRE->ncalls, c = ((cre.call[cre.which] + e) % n + n) % n;
            cre.call[cre.which] = (uint8_t)c;
            set_dirty = 1;
            app_say(CRE->calls[c].name, CRE->calls[c].hint);
        } else if (k == EN_PRESET) {             /* a voice preset: loaded when the knob rests */
            int i = preset_pending >= 0 ? preset_pending : set.preset;
            i = ((i + e) % NPRESETS + NPRESETS) % NPRESETS;
            preset_pending = (int8_t)i;
            preset_turned_at = now;
            t[0] = 'P';
            t[1] = (char)('1' + i);
            t[2] = 0;
            app_say(t, PRESET_NAME[i]);
        } else if (debug_knob(k, e)) {
        } else if (k >= EN_K1 && k <= EN_K4 && body_cur == BODY_SEAL) {
            seal_knob((int)(k - EN_K1), e);
        } else if (k >= EN_K1 && k <= EN_K4) {   /* the page's four values */
            knob_turn(PAGE[page][k - EN_K1], e);
        }
    }
    body_settle(now);
    if (preset_pending >= 0 && now - preset_turned_at >= 400u) {
        preset_apply(preset_pending);
        preset_pending = -1;
        ui.redraw = 1;
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);                            /* the unlit buttons glow, readable in the dark */
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u) | (vowel_mode == 1 ? 1u << B_FX : 0u) | (vowel_mode == 2 ? 1u << B_ARP : 0u)
                  | (body_cur == BODY_SEAL ? 1u << B_LFO : 0u),
              keys | midi_keys(set.octave));     /* the keys pressed, and the notes MIDI in plays */
}

void app_input_only(void) { input(); }

/* the timing test's readout, read from the audio interrupt's sums every half second */
static struct {
    uint32_t avg[2], peak[2], load, xruns, at;
} stat;
static int stat_update(void)
{
    uint32_t now = plat_ms(), n = n_blk;
    if (now - stat.at < 500u || !n)
        return 0;
    stat.at = now;
    /* (in float: 64-bit integer arithmetic beside loads is a known hazard of this CPU, felucca-bsp docs/CPU.md) */
    stat.avg[0] = (uint32_t)((float)c1_sum * 100.0f / ((float)n * (float)BLOCK_TICKS));
    stat.avg[1] = (uint32_t)((float)c0_sum * 100.0f / ((float)n * (float)BLOCK_TICKS));
    stat.peak[0] = (uint32_t)((float)c1_peak * 100.0f / (float)BLOCK_TICKS);
    stat.peak[1] = (uint32_t)((float)c0_peak * 100.0f / (float)BLOCK_TICKS);
    stat.load = plat_cpu_pct();
    stat.xruns = plat_xruns();
#ifdef PLAT_HOST
    if (!host_loads)                         /* (the simulator's stand-in loads, when a script gave them) */
#endif
    {
        brain_c2 = (uint8_t)(stat.avg[0] > 100u ? 100u : stat.avg[0]);   /* (the brain's colour and brightness) */
        brain_c1 = (uint8_t)(stat.avg[1] > 100u ? 100u : stat.avg[1]);
    }
    c1_sum = c0_sum = c1_peak = c0_peak = 0;   /* (the interrupt may add one block between: harmless) */
    n_blk = 0;
    return 1;
}

static void key_cell(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t k, uint16_t off)
{
    int32_t v, p;
    cv_rect(x, y, w, h, off);
    if (!(ui.keys >> k & 1u))
        return;
    v = (ui.vel[k] * h + 126) / 127;
    p = (ui.prs[k] * h + 126) / 127;
    cv_rect(x, y + h - v, w, v, C_HI);
    if (p)
        cv_rect(x + 2, y + h - p, w - 4, p, C_WHITE);
}

static void draw_debug(void)
{
    char t[24];
    int32_t note = voice_note;
    uint32_t i, now = plat_ms(), pct = plat_master() * 100u / 4096u;
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    /* the header: the name and the note, or a message for a moment */
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], msg_col(C_WHITE));
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], msg_col(C_HI));
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
        if (FM1B_BLE)                        /* Bluetooth: grey while it waits, lit while a computer is connected */
            x = cv_text(x + 8, 10, &FONT_XS, "BLE", plat_ble_connected() ? C_HI : C_DIM);
        if (FM1B_BLE_CENTRAL)                /* a Bluetooth MIDI device: lit while connected */
            cv_text(x + 6, 10, &FONT_XS, "DEV", plat_ble_device() == BLE_DEV_READY ? C_HI : C_DIM);
        if (note >= 0) {                     /* the note, and the last key's velocity and pressure */
            int32_t x;
            note_name(note, t);
            x = 230 - text_w(&FONT_B, t);
            cv_text(x, 6, &FONT_B, t, C_HI);
            if (ui.keys >> ui.last_key & 1u) {
                char n[24] = "V ";
                int_text(ui.vel[ui.last_key], t);
                text_cat(n, t);
                text_cat(n, "  P ");
                int_text(ui.prs[ui.last_key], t);
                text_cat(n, t);
                cv_text(x - 12 - text_w(&FONT_XS, n), 10, &FONT_XS, n, C_GRAY);
            }
        }
    }
    cv_blit(0, 0);
    /* the timing test: each core's share of the block, on average and at worst, over the last half second */
    cv_begin(240, 144, C_BLACK);
    {
        static const char *const ROW[4] = {"CORE 2 TRACT", "CORE 1 CONTROL", "AUDIO LOAD", "XRUNS"};
        for (i = 0; i < 4u; i++) {
            int32_t y = (int32_t)i * 26;
            cv_text(4, y + 4, &FONT_XS, ROW[i], C_GRAY);
            t[0] = 0;
            if (i < 2u) {
                char p[12];
                int_text((int32_t)stat.avg[i], p);
                text_cat(t, p);
                text_cat(t, "%  PEAK ");
                int_text((int32_t)stat.peak[i], p);
                text_cat(t, p);
                text_cat(t, "%");
            } else {
                int_text((int32_t)(i == 2u ? stat.load : stat.xruns), t);
                if (i == 2u)
                    text_cat(t, "%");
            }
            cv_text(116, y, &FONT_B, t, C_WHITE);
        }
        cv_text(4, 108, &FONT_XS, two_cores ? "TWO CORES" : "ONE CORE", two_cores ? C_HI : C_GRAY);
        {   /* the words: the syllable sounding (a hyphen when its word goes on) and the one the next key sings */
            char w[40] = "";
            const ch_syl_t *so = ch_live_sounding(&live), *nx = ch_live_next(&live);
            if (speaking) {
                text_cat(w, "SPEAKING (PLAY)");
            } else {
                if (so) {
                    text_cat(w, so->text);
                    if (!so->word_end)
                        text_cat(w, "-");
                    text_cat(w, "  ");
                }
                text_cat(w, "NEXT ");
                text_cat(w, nx ? nx->text : "AH");
            }
            for (i = 0; w[i]; i++)
                if (w[i] >= 'a' && w[i] <= 'z')
                    w[i] = (char)(w[i] - 'a' + 'A');
            cv_text(100, 108, &FONT_XS, w, speaking || so ? T_TEXT : C_GRAY);
        }
        {   /* K1 the anatomy's rate (every 8th, 4th or 2nd step), K2 the treble shelf */
            char e[32] = "ANATOMY ";
            char nb[4];
            int_text((int32_t)anat_every, nb);
            text_cat(e, nb);
            text_cat(e, shelf_on ? "  SHELF ON" : "  SHELF OFF");
            cv_text(4, 124, &FONT_XS, e, C_GRAY);
            cv_text(150, 124, &FONT_XS, "K1 / K2", C_DIM);
        }
    }
    cv_blit(0, 36);
    /* the keys, black above white: a held key fills to its velocity (amber), its pressure over that (white) */
    cv_begin(240, 60, C_BLACK);
    for (i = 0; i < 16u; i++)
        key_cell(4 + (int32_t)i * 14, 14, 13, 14, WK[i], C_DIM);
    for (i = 0; i < 11u; i++) {
        static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};   /* the white key each one follows */
        key_cell(4 + (int32_t)AT[i] * 14 + 8, 0, 11, 12, BK[i], C_LINE);
    }
    octave_text(t);
    cv_text(4, 36, &FONT_XS, "OCT", C_GRAY);
    cv_text(30, 36, &FONT_XS, t, C_HI);
    {   /* the word slot (PRESET) and the start of its words */
        char sl[2] = {(char)('1' + set.slot), 0}, wd[24];
        int i;
        cv_text(64, 36, &FONT_XS, "WORDS", C_GRAY);
        cv_text(100, 36, &FONT_XS, sl, C_HI);
        for (i = 0; i < 10 && lyric[i]; i++)
            wd[i] = (char)(lyric[i] >= 'a' && lyric[i] <= 'z' ? lyric[i] - 'a' + 'A' : lyric[i]);
        wd[i] = 0;
        if (lyric[i])
            text_cat(wd, "..");
        cv_text(110, 36, &FONT_XS, wd, T_TEXT);
    }
    if (midi_count) {                        /* the last MIDI message in, and where from */
        static const char *const PORT[NPORTS] = {"USB", "BLE", "TRS"};
        static const char HEX[] = "0123456789ABCDEF";
        uint32_t m = midi_last, b;
        char h[12];
        for (b = 0; b < 3u; b++) {
            uint32_t v = (m >> (8u + 8u * b)) & 0xFFu;
            h[3 * b] = HEX[v >> 4];
            h[3 * b + 1] = HEX[v & 15u];
            h[3 * b + 2] = b < 2u ? ' ' : 0;
        }
        cv_text(4, 48, &FONT_XS, "MIDI IN", C_GRAY);
        cv_text(52, 48, &FONT_XS, midi_last_port < NPORTS ? PORT[midi_last_port] : "", C_HI);
        cv_text(82, 48, &FONT_XS, h, C_WHITE);
    }
    cv_text(178, 36, &FONT_XS, "VOL", C_GRAY);
    int_text((int32_t)pct, t);
    text_cat(t, "%");
    cv_text(204, 36, &FONT_XS, t, C_HI);
    cv_blit(0, 180);
}


/* ------------------------------------------------------------------- home page --- */
/* the head, live, as the voice holds it now (its own anatomy, computed here from the control's
 * articulation when it moves, at most every HOME_MS), the words beside it, and the four articulators' readouts in
 * their colours (tongue green, jaw red, lips pink, soft palate white). GLO switches to the debug page (the timings). */
static ch_tract_t dtract PLAT_POOL;         /* the drawing's anatomy (the audio's is the voice's own) */
static float drawn_x[CH_NARTIC];
static uint32_t head_at;
static uint8_t head_ready;
#define HOME_MS 60u
#define HOME_TOP 24
#define HOME_HEAD_H 152

static void home_body(void)                  /* the drawn head's anatomy: the body's */
{
    body_tract_setup(&dtract, &mix_main.b);
    dtract.draw = 1;
    head_ready = 0;
    ui.redraw = 1;
}

static void home_init(void)
{
    home_body();
}

/* the head again if the voice's articulation moved (1: it was) */
/* the eye: a blink now and then (every 2.5 to 6 s, 200 ms shut and open again); shut while the keys hold the voice past
 * EYE_REST_MS (slowly, opening again when they let go), and red past EYE_RED_MS. Its state, EYE_STEPS steps of
 * openness and the red: the screen is drawn again only when it changes */
#define EYE_REST_MS 2000u
#define EYE_RED_MS 10000u
#define EYE_BLINK_MS 200u
#define EYE_SHUT_MS 300u                      /* (closing and opening while held) */
static uint32_t eye_seed = 0x2545f491u, eye_blink_at, eye_held_at, eye_let_go_at;
static uint8_t eye_held, eye_rested;

static uint8_t eye_state(uint32_t now)
{
    const int keys = voice_note >= 0 && !speaking && !bt.on && !sg.playing;   /* (the keys', not a tune's or PLAY's) */
    float open = 1.0f;
    int red = 0;
    if (keys && !eye_held) {
        eye_held = 1;
        eye_held_at = now;
    } else if (!keys && eye_held) {
        eye_held = 0;
        eye_rested = now - eye_held_at > EYE_REST_MS;
        eye_let_go_at = now;
    }
    if (eye_held) {
        const uint32_t t = now - eye_held_at;
        if (t > EYE_REST_MS)
            open = 1.0f - fm_clampf((float)(t - EYE_REST_MS) / (float)EYE_SHUT_MS, 0, 1);
        red = t > EYE_RED_MS;
    } else if (eye_rested) {
        open = fm_clampf((float)(now - eye_let_go_at) / (float)EYE_SHUT_MS, 0, 1);
        if (open >= 1.0f)
            eye_rested = 0;
    }
    if (!eye_blink_at)
        eye_blink_at = now + 3000u;
    if ((int32_t)(now - eye_blink_at) >= 0) {
        const uint32_t t = now - eye_blink_at;
        if (t < EYE_BLINK_MS)
            open = fm_minf(open, fm_fabsf((float)t - EYE_BLINK_MS / 2.0f) / (EYE_BLINK_MS / 2.0f));
        else {
            eye_seed = eye_seed * 1664525u + 1013904223u;   /* (the next one 2.5 to 6 s on) */
            eye_blink_at = now + 2500u + (eye_seed >> 8) % 3500u;
        }
    }
    return (uint8_t)((uint32_t)(open * (float)EYE_STEPS + 0.5f) | (red ? EYE_RED : 0u));
}

static int home_head_update(void)
{
    float a[CH_NARTIC], d = 0.0f;
    uint32_t now = plat_ms();
    int k;
    if (head_ready && now - head_at < HOME_MS)
        return 0;
    for (k = 0; k < CH_NARTIC; k++) {
        a[k] = ctl.x[k];
        d = fm_fabsf(a[k] - drawn_x[k]) > d ? fm_fabsf(a[k] - drawn_x[k]) : d;
    }
    if (head_ready && d < 0.004f)
        return 0;
    ch_tract_apply(&dtract, a, 0.0f);
    ch_profile_build(&dtract, &prof);
    for (k = 0; k < CH_NARTIC; k++)
        drawn_x[k] = a[k];
    head_at = now;
    head_ready = 1;
    return 1;
}

static void fmt1(float v, char *b)          /* one decimal */
{
    int32_t t = (int32_t)(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    char d[12];
    if (t < 0) {
        *b++ = '-';
        t = -t;
    }
    int_text(t / 10, d);
    *b = 0;
    text_cat(b, d);
    text_cat(b, ".");
    d[0] = (char)('0' + t % 10);
    d[1] = 0;
    text_cat(b, d);
}

static void upper(char *d, const char *s, int n)
{
    int i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 'a' + 'A' : s[i]);
    d[i] = 0;
}

static void draw_home(void)
{
    char t[40];
    int32_t note = voice_note, y0, x;
    uint32_t now = plat_ms();
    const ch_syl_t *so = ch_live_sounding(&live), *nx = ch_live_next(&live);
    /* the header: what the view is, and the note; or a message for a moment */
    cv_begin(240, HOME_TOP, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {                                 /* the page (or VOWEL), the preset; the note while one sounds */
        char pg[16] = "";
        text_cat(pg, PAGE_NAME[page]);
        if (page != PAGE_VOWEL && page != PAGE_TONGUE) {
            char d[4] = {' ', (char)('1' + page), '/', 0};
            text_cat(pg, d);
            text_cat(pg, "5");
        }
        x = cv_text(8, 7, &FONT_XS, pg, T_HI);
        cv_text(x + 10, 7, &FONT_XS, vowel_mode == 2 ? "TONGUE MODE" : vowel_mode ? "VOWEL MODE" : "WORDS", T_DIM);
        if (note >= 0) {
            note_name(note, t);
            cv_text(232 - text_w(&FONT_B, t), 4, &FONT_B, t, T_HI);
        } else {
            char pn[16] = {'P', (char)('1' + set.preset), ' ', 0};
            int32_t px;
            text_cat(pn, PRESET_NAME[set.preset]);
            px = 232 - text_w(&FONT_XS, pn);
            cv_text(px, 7, &FONT_XS, pn, T_MID);
            /* the body (ALGORITHM), before the preset: BLENDED's shares */
            {
                char bl[24];
                if ((int)(kv[K_BODY] + 0.5f) == BODY_BLENDED) {   /* (its shares; on a body whole's place, its name) */
                    float w[3];
                    const int nb = body_weights(body_g < 0 ? 0 : body_g, body_a < 0 ? 0 : body_a, w);
                    if (w[nb] > 0.995f)
                        text_cat((bl[0] = 0, bl), "BLENDED");
                    else
                        body_label(body_g < 0 ? 0 : body_g, body_a < 0 ? 0 : body_a, bl);
                }
                else
                    text_cat((bl[0] = 0, bl), body_name(body_cur));
                cv_text(px - 8 - text_w(&FONT_XS, bl), 7, &FONT_XS, bl, T_DIM);
            }
        }
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the head, and the words beside it: the syllable sounding, the next, the slot */
    head_s = (float)HOME_HEAD_H / (VIEW_Y1 - VIEW_Y0);
    head_x0 = 0.0f;
    head_y0 = (float)HOME_TOP;
    for (y0 = HOME_TOP; y0 < HOME_TOP + HOME_HEAD_H; y0 += HOME_HEAD_H / 2) {
        cv_begin(240, HOME_HEAD_H / 2, C_BLACK);
        if (head_ready)
            draw_head(y0);
        if (vowel_mode == 2) {               /* TONGUE: where the tongue is put, the vowel nearest */
            cv_text(126, HOME_TOP + 8 - y0, &FONT_XS, "TONGUE", T_MID);
            cv_text(126, HOME_TOP + 22 - y0, &FONT_L, ch_vowel_name(drawn_x), note >= 0 ? T_TEXT : T_HI);
            cv_text(126, HOME_TOP + 66 - y0, &FONT_XS, "K1-K2 BODY  K3-K4 TIP", T_HI);
            cv_text(126, HOME_TOP + 112 - y0, &FONT_XS, "SEL: SHAPE, MOUTH", T_MID);
            cv_text(126, HOME_TOP + 126 - y0, &FONT_XS, "ARP: BACK TO WORDS", T_DIM);
            cv_blit(0, (uint32_t)y0);
            continue;
        }
        if (vowel_mode) {                    /* the vowel made now, its formants, the snapshots' hint */
            char f[24] = "F1 ", d[8];
            cv_text(126, HOME_TOP + 8 - y0, &FONT_XS, "VOWEL", T_MID);
            cv_text(126, HOME_TOP + 22 - y0, &FONT_L, ch_vowel_name(drawn_x), note >= 0 ? T_TEXT : T_HI);
            int_text((int32_t)(pad_f1(kv[K_OPEN]) + 0.5f), d);
            text_cat(f, d);
            text_cat(f, "  F2 ");
            int_text((int32_t)(pad_f2(kv[K_FRONT]) + 0.5f), d);
            text_cat(f, d);
            cv_text(126, HOME_TOP + 66 - y0, &FONT_XS, f, T_HI);
            cv_text(126, HOME_TOP + 112 - y0, &FONT_XS, "FX + KEY: SNAPSHOT", T_MID);
            cv_text(126, HOME_TOP + 126 - y0, &FONT_XS, "HOLD 1 S: STORE", T_DIM);
            cv_blit(0, (uint32_t)y0);
            continue;
        }
        cv_text(126, HOME_TOP + 8 - y0, &FONT_XS, speaking ? "SPEAKING" : "NOW", T_MID);
        if (so && !speaking) {
            upper(t, so->text, 10);
            if (!so->word_end)
                text_cat(t, "-");
        } else {
            t[0] = 0;
        }
        cv_text(126, HOME_TOP + 22 - y0, &FONT_L, t[0] ? t : "-", so && !speaking ? T_TEXT : T_DIM);
        cv_text(126, HOME_TOP + 66 - y0, &FONT_XS, "NEXT", T_MID);
        upper(t, nx ? nx->text : "ah", 10);
        cv_text(126, HOME_TOP + 80 - y0, &FONT_B, t, T_HI);
        {
            char sl[12] = "WORDS ";
            sl[6] = (char)('1' + set.slot);
            sl[7] = 0;
            cv_text(126, HOME_TOP + 112 - y0, &FONT_XS, sl, T_MID);
            upper(t, lyric, 14);
            if (lyric[14])
                text_cat(t, "..");
            cv_text(126, HOME_TOP + 126 - y0, &FONT_XS, t, T_DIM);
        }
        cv_blit(0, (uint32_t)y0);
    }
    /* the readouts: the four articulators */
    cv_begin(240, 240 - HOME_TOP - HOME_HEAD_H, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    {   /* the page's four knobs, K1..K4 */
        const uint16_t *const col = KNOB_COL;
        int i;
        for (i = 0; i < 4; i++) {
            const int id = PAGE[page][i];
            int32_t cx = 8 + i * 58;
            const char *nm = page == PAGE_VOWEL && id == K_VELUM ? "NASAL" : KNOB[id].name;
            cv_rect(cx, 13, 4, 4, col[i]);
            cv_text(cx + 8, 9, &FONT_XS, nm, col[i]);
            knob_text(id, t);
            cv_text(cx, 28, &FONT_M, t, col[i]);
        }
    }
    cv_blit(0, (uint32_t)(HOME_TOP + HOME_HEAD_H));
}

/* ------------------------------------------------------------- the editor's page --- */
/* the words as typed (wrapped, the cursor green), the keyboard with each key's letter (lit while held), the controls */
#define ED_TOP 24
#define ED_LINE_H 22
#define ED_LINES 4
#define ED_KB_Y 132                            /* (four lines of words, three of controls below the keys) */
static uint8_t ed_wrap(uint8_t *start)        /* the lines' starts (word wrap at 224 px, FONT_B, upper case) */
{
    uint8_t nl = 1, i, sp = 0;
    int32_t w = 0;
    start[0] = 0;
    for (i = 0; i < ed.n && nl < 16; i++) {
        char c[2] = {(char)(ed.t[i] >= 'a' && ed.t[i] <= 'z' ? ed.t[i] - 'a' + 'A' : ed.t[i]), 0};
        int32_t cw = text_w(&FONT_B, c);
        if (ed.t[i] == ' ')
            sp = (uint8_t)(i + 1);
        if (w + cw > 224 && i > start[nl - 1]) {
            uint8_t b = sp > start[nl - 1] ? sp : i;   /* after the last space, else here */
            uint8_t j;
            start[nl++] = b;
            w = 0;
            for (j = b; j < i; j++) {
                char d[2] = {(char)(ed.t[j] >= 'a' && ed.t[j] <= 'z' ? ed.t[j] - 'a' + 'A' : ed.t[j]), 0};
                w += text_w(&FONT_B, d);
            }
        }
        w += cw;
    }
    return nl;
}

static void ed_text_strip(int32_t y0, int32_t h, const uint8_t *start, uint8_t nl, uint8_t first)
{
    uint8_t l;
    cv_begin(240, (uint32_t)h, C_BLACK);
    if (!ed.n)
        cv_text(8, ED_TOP + 8 - y0, &FONT_XS, "EMPTY: TYPE ON THE KEYS", T_DIM);
    for (l = first; l < nl && l < first + ED_LINES; l++) {
        uint8_t a = start[l], b = l + 1 < nl ? start[l + 1] : ed.n, i;
        int32_t y = ED_TOP + 6 + (l - first) * ED_LINE_H - y0, x = 8;
        for (i = a; i <= b; i++) {
            if (i == ed.cur && (i < b || l + 1 == nl || ed.cur == ed.n))
                cv_rect(x, y - 1, 2, 20, T_TEXT);
            if (i < b) {
                char c[2] = {(char)(ed.t[i] >= 'a' && ed.t[i] <= 'z' ? ed.t[i] - 'a' + 'A' : ed.t[i]), 0};
                x = cv_text(x + (i == ed.cur ? 3 : 0), y, &FONT_B, c, T_TEXT);
            }
        }
    }
    cv_blit(0, (uint32_t)y0);
}

static void draw_edit(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};   /* the white key each black one follows */
    uint8_t start[16], nl, cl = 0, first = 0, i;
    char t[24];
    int32_t x;
    /* the header: the slot and the length (or a message for a moment) */
    cv_begin(240, ED_TOP, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {
        t[0] = 0;
        x = cv_text(8, 7, &FONT_XS, "EDIT", T_HI);
        {
            char w[8] = "WORDS ";
            w[6] = (char)('1' + ed.slot);
            w[7] = 0;
            x = cv_text(x + 8, 7, &FONT_XS, w, T_TEXT);
        }
        if (ed.dirty)
            cv_text(x + 10, 7, &FONT_XS, "UNSAVED", T_WARN);
        int_text(ed.n, t);
        text_cat(t, "/150");
        x = cv_text(232 - text_w(&FONT_XS, t), 7, &FONT_XS, t, T_MID);
        {   /* the keys' layout, before the count */
            const char *lay = set.kb ? "QWERTY" : "A-Z";
            cv_text(232 - text_w(&FONT_XS, t) - 8 - text_w(&FONT_XS, lay), 7, &FONT_XS, lay, T_DIM);
        }
    }
    cv_rect(8, ED_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the words: the lines around the cursor */
    nl = ed_wrap(start);
    for (i = 0; i < nl; i++)
        if (ed.cur >= start[i])
            cl = i;
    if (cl >= ED_LINES)
        first = (uint8_t)(cl - ED_LINES + 1);
    ed_text_strip(ED_TOP, 60, start, nl, first);
    ed_text_strip(ED_TOP + 60, ED_KB_Y - ED_TOP - 60, start, nl, first);
    /* the keyboard: each key's letter, lit while held */
    cv_begin(240, 56, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    for (i = 0; i < 16; i++) {
        const uint32_t k = WK[i];
        const int on = (int)(ui.keys >> k & 1u);
        const char ch = ed_key_char(k);
        char c[3] = {ch == ' ' ? 'S' : (char)(ch - 'a' + 'A'), ch == ' ' ? 'P' : 0, 0};
        cv_rect(8 + i * 14, 8, 13, 44, on ? T_TEXT : T_SURF);
        cv_text(8 + i * 14 + (13 - text_w(&FONT_XS, c)) / 2, 36, &FONT_XS, c,
                on ? C_BLACK : set.kb && (ed_row_keys() >> k & 1u) ? T_TEXT : T_HI);   /* (a row's start: white) */
    }
    for (i = 0; i < 11; i++) {
        const uint32_t k = BK[i];
        const int on = (int)(ui.keys >> k & 1u);
        const char ch = ed_key_char(k);
        char c[2] = {ch == ' ' ? '_' : (char)(ch - 'a' + 'A'), 0};
        int32_t bx = 8 + AT[i] * 14 + 8;
        cv_rect(bx - 1, 7, 13, 25, C_BLACK);
        cv_rect(bx, 8, 11, 23, on ? T_TEXT : T_DIM);
        cv_text(bx + (11 - text_w(&FONT_XS, c)) / 2, 13, &FONT_XS, c,
                on ? C_BLACK : set.kb && (ed_row_keys() >> k & 1u) ? T_TEXT : T_TEXT);
    }
    cv_blit(0, ED_KB_Y);
    /* the controls, the knobs in their colours */
    cv_begin(240, 240 - ED_KB_Y - 56, C_BLACK);
    x = cv_text(8, 4, &FONT_XS, "K1", T_K1);
    x = cv_text(x + 4, 4, &FONT_XS, "MOVE", T_MID);
    x = cv_text(x + 10, 4, &FONT_XS, "K2", T_K2);
    x = cv_text(x + 4, 4, &FONT_XS, ", . ?", T_MID);
    x = cv_text(x + 10, 4, &FONT_XS, "OCT-", T_TEXT);
    x = cv_text(x + 4, 4, &FONT_XS, "DEL", T_MID);
    x = cv_text(x + 10, 4, &FONT_XS, "FX", T_TEXT);
    cv_text(x + 4, 4, &FONT_XS, set.kb ? "A-Z" : "QWERTY", T_MID);
    x = cv_text(8, 20, &FONT_XS, "PLAY", T_TEXT);
    x = cv_text(x + 4, 20, &FONT_XS, "TRY", T_MID);
    x = cv_text(x + 10, 20, &FONT_XS, "SAVE", T_TEXT);
    x = cv_text(x + 4, 20, &FONT_XS, "KEEP", T_MID);
    x = cv_text(x + 10, 20, &FONT_XS, "EDIT", T_TEXT);
    cv_text(x + 4, 20, &FONT_XS, "LEAVE", T_MID);
    x = cv_text(8, 36, &FONT_XS, "ARP", T_TEXT);
    x = cv_text(x + 4, 36, &FONT_XS, "RANDOM WORDS", T_MID);
    x = cv_text(x + 10, 36, &FONT_XS, "PRESET", T_TEXT);
    cv_text(x + 4, 36, &FONT_XS, "SLOT", T_MID);
    cv_blit(0, ED_KB_Y + 56);
}

/* ------------------------------------------------------------- the beatbox's page --- */
/* the head as it beatboxes, the sound heard (and how the air moves for it), the beat's sixteen steps (the playhead
 * lit, the cursor outlined while editing), the knobs */
#define BT_HEAD_H 120
#define BT_GRID_Y (HOME_TOP + BT_HEAD_H)
static uint16_t beat_role_col(int id)
{
    /* kick orange, snare violet, hi-hat yellow, percussion cyan, the hummed bass lime */
    static const uint16_t C[5] = {RGB(0xff, 0x9a, 0x3c), RGB(0xb4, 0x8c, 0xff), RGB(0xff, 0xd2, 0x3f), RGB(0x3f, 0xd0, 0xe0),
                                  RGB(0xa3, 0xe6, 0x35)};
    return id >= 0 && id < CH_BEAT_N ? C[CH_BEAT_SOUNDS[id].role] : T_DIM;
}

static void draw_beat(void)
{
    static const char *const AIR[5] = {"BREATH OUT", "EJECTIVE", "CLICK", "BREATH IN", "HUMMED"};
    const int now_step = beat_now, cur = bt.edit ? bt.cur : -1;
    const int heard = beat_sound >= 0 && beat_blocks - beat_sound_at < 60u ? beat_sound : -1;   /* (~0.35 s) */
    char t[24];
    int32_t x, y0, i;
    /* the header: the beat and its tempo (or a message for a moment) */
    cv_begin(240, HOME_TOP, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {
        x = cv_text(8, 7, &FONT_XS, "BEAT", T_HI);
        t[0] = 0;
        int_text(bt.slot + 1, t);
        text_cat(t, " ");
        text_cat(t, beat_name(bt.slot));
        x = cv_text(x + 8, 7, &FONT_XS, t, T_TEXT);
        if (bt.dirty)
            cv_text(x + 8, 7, &FONT_XS, "UNSAVED", T_WARN);
        int_text(beats.b[bt.slot].bpm, t);
        text_cat(t, " BPM");
        cv_text(232 - text_w(&FONT_XS, t), 7, &FONT_XS, t, T_MID);
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the head, and the sound beside it */
    head_s = (float)BT_HEAD_H / (VIEW_Y1 - VIEW_Y0);
    head_x0 = 0.0f;
    head_y0 = (float)HOME_TOP;
    for (y0 = HOME_TOP; y0 < HOME_TOP + BT_HEAD_H; y0 += BT_HEAD_H / 2) {
        cv_begin(240, BT_HEAD_H / 2, C_BLACK);
        if (head_ready)
            draw_head(y0);
        cv_text(112, HOME_TOP + 6 - y0, &FONT_XS, bt.edit ? "STEP EDIT" : bt.play ? "PLAYING" : "PADS", bt.edit ? T_TEXT : T_MID);
        if (heard >= 0) {
            cv_text(112, HOME_TOP + 22 - y0, &FONT_M, CH_BEAT_SOUNDS[heard].name, beat_role_col(heard));
            cv_text(112, HOME_TOP + 50 - y0, &FONT_XS, AIR[CH_BEAT_SOUNDS[heard].air], T_HI);
        } else {
            cv_text(112, HOME_TOP + 22 - y0, &FONT_M, "READY", T_DIM);
        }
        cv_text(112, HOME_TOP + 84 - y0, &FONT_XS, "PLAY  START / STOP", T_DIM);
        cv_text(112, HOME_TOP + 98 - y0, &FONT_XS, bt.edit ? "OCT-/+  CLEAR STEP" : "EDIT  WRITE STEPS", T_DIM);
        cv_blit(0, (uint32_t)y0);
    }
    /* the steps: two rows of eight, each its sound's symbol in its role's colour */
    cv_begin(240, 60, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    for (i = 0; i < BEAT_STEPS; i++) {
        const ch_beat_step_t *s = &beats.b[bt.slot].s[i];
        const int32_t cx = 8 + (i % 8) * 28, cy = 6 + (i / 8) * 27;
        const uint16_t col = beat_role_col(s->id);
        const int lit = i == now_step;
        const char *sym = s->id >= 0 ? CH_BEAT_SOUNDS[s->id].id : (i % 4 == 0 ? "|" : ".");
        cv_rect(cx, cy, 26, 24, lit ? (s->id >= 0 ? col : T_DIM) : T_SURF);
        if (i == cur) {                       /* the cursor: an outline */
            cv_rect(cx, cy, 26, 2, T_TEXT);
            cv_rect(cx, cy + 22, 26, 2, T_TEXT);
            cv_rect(cx, cy, 2, 24, T_TEXT);
            cv_rect(cx + 24, cy, 2, 24, T_TEXT);
        }
        if (s->accent && s->id >= 0)
            cv_rect(cx + 20, cy + 3, 3, 3, lit ? C_BLACK : col);
        cv_text(cx + (26 - text_w(&FONT_XS, sym)) / 2, cy + 5, &FONT_XS, sym, lit ? C_BLACK : s->id >= 0 ? col : T_DIM);
    }
    cv_blit(0, BT_GRID_Y);
    /* the knobs */
    cv_begin(240, 240 - BT_GRID_Y - 60, C_BLACK);
    x = cv_text(8, 6, &FONT_XS, "K1", T_K1);
    int_text(beats.b[bt.slot].bpm, t);
    x = cv_text(x + 4, 6, &FONT_XS, "TEMPO", T_MID);
    x = cv_text(x + 4, 6, &FONT_XS, t, T_K1);
    x = cv_text(x + 10, 6, &FONT_XS, "K2", T_K2);
    x = cv_text(x + 4, 6, &FONT_XS, "SWING", T_MID);
    int_text(beats.b[bt.slot].swing, t);
    text_cat(t, "%");
    x = cv_text(x + 4, 6, &FONT_XS, t, T_K2);
    x = cv_text(x + 10, 6, &FONT_XS, "K3", T_K3);
    x = cv_text(x + 4, 6, &FONT_XS, "STEP", T_MID);
    int_text(bt.cur + 1, t);
    cv_text(x + 4, 6, &FONT_XS, t, T_K3);
    x = cv_text(8, 21, &FONT_XS, "PRESET", T_TEXT);
    x = cv_text(x + 4, 21, &FONT_XS, "BEAT", T_MID);
    x = cv_text(x + 10, 21, &FONT_XS, "FX+PAD", T_TEXT);
    x = cv_text(x + 4, 21, &FONT_XS, "ACCENT", T_MID);
    x = cv_text(x + 10, 21, &FONT_XS, "SEQ", T_TEXT);
    cv_text(x + 4, 21, &FONT_XS, "LEAVE", T_MID);
    cv_blit(0, BT_GRID_Y + 60);
}

/* ------------------------------------------------------------- the Bestiary's page --- */
/* the creature (LFO; ALGORITHM picks it): the call PRESET picks, the pitch, the animal traced side on with its airway
 * inside (draw_airway), the call playing, the page's settings */
static uint16_t air_col(float breath)        /* the breath's light: dark grey to light grey */
{
    const int lit = (int)(fm_clampf(breath / 12.0f, 0, 1) * 255.0f);   /* (full at 12 cmH2O) */
    return RGB(0x1a + lit * 0xbc / 255, 0x1a + lit * 0xbc / 255, 0x1a + lit * 0xbc / 255);
}

/* ---- the creatures drawn: each animal traced side on, facing right as the head does, its airway inside it lit by the
 * breath; the live parts as they are (the mouth or beak as open as its setting, the nose as open as NOSE, the
 * songbird's valves and the elephant's lips lit as they open). Coordinates in a 136 x 72 box beside the pitch ---- */
#define AN_X 100                              /* the box's left, top (the strip's px) */
#define AN_Y 2
typedef uint8_t an_pt[2];
static ch_v2 an_buf[(48 - 1) * 4 + 1];        /* a traced outline, smoothed */

static void an_trace(const an_pt *p, int n, int closed, int32_t y0, uint16_t c)   /* a smooth line through p */
{
    ch_v2 q[48];
    int i, m;
    if (n + closed > 48)
        n = 48 - closed;
    for (i = 0; i < n + closed; i++) {
        q[i].x = (float)p[i % n][0];
        q[i].y = (float)p[i % n][1];
    }
    m = ch_catmull_rom(q, n + closed, 4, an_buf);
    for (i = 1; i < m; i++)
        cv_line(AN_X + (int32_t)an_buf[i - 1].x, AN_Y + (int32_t)an_buf[i - 1].y - y0, AN_X + (int32_t)an_buf[i].x,
                AN_Y + (int32_t)an_buf[i].y - y0, c);
}

static void an_tube(const an_pt *p, int n, int w, int32_t y0, uint16_t c)   /* a straight-jointed tube w px wide */
{
    int i, dx, dy;
    for (i = 1; i < n; i++)
        for (dy = -(w - 1) / 2; dy <= w / 2; dy++)
            for (dx = -(w - 1) / 2; dx <= w / 2; dx++)
                cv_line(AN_X + p[i - 1][0] + dx, AN_Y + p[i - 1][1] + dy - y0, AN_X + p[i][0] + dx, AN_Y + p[i][1] + dy - y0, c);
}

static void an_blob(int x, int y, int r, int32_t y0, uint16_t c)   /* a filled disc (the lungs, a valve) */
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        const int w = (int)fm_sqrtf((float)(r * r - dy * dy));
        cv_rect(AN_X + x - w, AN_Y + y + dy - y0, 2 * w + 1, 1, c);
    }
}

/* each animal's colour (the cat orange, the songbird yellow, the seal blue, the alligator green, the elephant grey,
 * the robot cyan, Squidlad pink) */
static const uint16_t AN_COL[NCRE] = {RGB(0xff, 0x9a, 0x3c), RGB(0xff, 0xd2, 0x3f), RGB(0x7f, 0xb2, 0xff),
                                      RGB(0x7b, 0xd3, 0x89), RGB(0xb8, 0xb8, 0xd0), RGB(0x3f, 0xd0, 0xe0),
                                      RGB(0xff, 0x5c, 0xd6)};

static void an_poly(const an_pt *p, int n, int closed, int32_t y0, uint16_t c)   /* straight lines through p (the robot's) */
{
    int i;
    for (i = 1; i < n + closed; i++)
        cv_line(AN_X + p[i - 1][0], AN_Y + p[i - 1][1] - y0, AN_X + p[i % n][0], AN_Y + p[i % n][1] - y0, c);
}

static void draw_airway(int32_t y0)          /* the creature, side on (the top strip's coordinates) */
{
    const uint16_t air = air_col(seal_breath), col = AN_COL[cre.which], dim = T_DIM;
    if (cre.which == CRE_SEAL) {             /* the Weddell seal, lying, its head raised */
        static const an_pt O[] = {{2, 56}, {14, 50}, {34, 45}, {60, 41}, {82, 35}, {98, 23}, {110, 15}, {122, 15},
                                  {131, 23}, {134, 27}, {128, 31}, {116, 33}, {106, 41}, {98, 55}, {82, 63}, {52, 66},
                                  {24, 64}, {6, 67}};
        static const an_pt FLIP[] = {{92, 58}, {86, 70}, {98, 67}};
        static const an_pt THROAT[] = {{100, 37}, {112, 29}}, MOUTH[] = {{112, 29}, {126, 27}}, OPEN[] = {{126, 27}, {132, 27}};
        const int open = 1 + (int)(cre.set[CRE_SEAL][CH_SEAL_MOUTH] * 4.0f);
        an_trace(O, 18, 1, y0, col);
        an_trace(FLIP, 3, 0, y0, col);
        an_blob(74, 52, 6, y0, air);                                       /* the lungs */
        an_tube(THROAT, 2, 3, y0, air);
        an_tube(MOUTH, 2, 3, y0, air);
        an_tube(OPEN, 2, open, y0, air);                                   /* its opening, as open as MOUTH */
        cv_rect(AN_X + 98, AN_Y + 35 - y0, 4, 1, T_MID);                   /* the glottis */
        an_blob(118, 20, 1, y0, col);                                      /* the eye */
    } else if (cre.which == CRE_BIRD) {      /* the songbird, perched */
        static const an_pt O[] = {{4, 58}, {20, 50}, {44, 37}, {66, 27}, {78, 15}, {92, 12}, {102, 18}, {106, 23},
                                  {104, 30}, {96, 38}, {90, 52}, {78, 62}, {60, 66}, {40, 64}, {24, 62}};
        static const an_pt WING[] = {{28, 50}, {50, 40}, {68, 44}, {56, 54}};
        static const an_pt LEG1[] = {{64, 66}, {62, 72}}, LEG2[] = {{74, 64}, {76, 72}};
        static const an_pt TRACHEA[] = {{80, 47}, {94, 32}}, BRL[] = {{70, 52}, {78, 49}}, BRR[] = {{70, 44}, {78, 46}};
        const int bl = bird_audio ? (int)(bird.l.opening * 255.0f) : 0, br = bird_audio ? (int)(bird.r.opening * 255.0f) : 0;
        const int gape = (int)(cre.set[CRE_BIRD][CH_BIRD_GAPE] * 6.0f);
        an_trace(O, 15, 1, y0, col);
        an_trace(WING, 4, 1, y0, dim);
        an_trace(LEG1, 2, 0, y0, col);
        an_trace(LEG2, 2, 0, y0, col);
        cv_line(AN_X + 105, AN_Y + 22 - y0, AN_X + 126, AN_Y + 25 - gape / 2 - y0, col);   /* the beak, as open as GAPE */
        cv_line(AN_X + 104, AN_Y + 28 - y0, AN_X + 124, AN_Y + 26 + gape - gape / 2 - y0, col);
        an_tube(BRL, 2, 2, y0, air);                                       /* the bronchi */
        an_tube(BRR, 2, 2, y0, air);
        an_blob(77, 51, 1, y0, RGB(0x40 + bl * 0xbf / 255, 0x40 + bl * 0xbf / 255, 0x40 + bl * 0xbf / 255));   /* the valves */
        an_blob(77, 45, 1, y0, RGB(0x40 + br * 0xbf / 255, 0x40 + br * 0xbf / 255, 0x40 + br * 0xbf / 255));
        an_tube(TRACHEA, 2, 2, y0, air);
        an_blob(98, 28, 4, y0, air);                                       /* the throat cavity */
        an_blob(94, 19, 1, y0, col);                                       /* the eye */
    } else if (cre.which == CRE_GATOR) {     /* the alligator, low and long */
        static const an_pt O[] = {{0, 50}, {20, 46}, {40, 40}, {64, 36}, {86, 36}, {100, 37}, {104, 34}, {108, 38},
                                  {128, 41}, {131, 39}, {135, 43}, {135, 48}, {112, 50}, {98, 53}, {91, 54}, {93, 64},
                                  {84, 64}, {82, 54}, {56, 54}, {53, 64}, {44, 64}, {45, 54}, {24, 52}};
        static const an_pt THROAT[] = {{90, 47}, {104, 46}}, MOUTH[] = {{104, 46}, {126, 46}}, OPEN[] = {{126, 46}, {134, 46}};
        static const an_pt NOSE[] = {{102, 45}, {112, 43}, {130, 42}};
        const float *v = cre.set[CRE_GATOR];
        const int open = 1 + (int)(v[CH_GATOR_MOUTH] * 4.0f);
        an_trace(O, 23, 1, y0, col);
        an_blob(62, 46, 5, y0, air);
        an_tube(THROAT, 2, 3, y0, air);
        an_tube(MOUTH, 2, 1, y0, air);
        an_tube(OPEN, 2, open, y0, air);
        an_tube(NOSE, 3, 1 + (int)(v[CH_GATOR_NOSTRILS] * 1.5f), y0, air);   /* the nose, as open as NOSE */
        cv_rect(AN_X + 89, AN_Y + 45 - y0, 1, 4, T_MID);
        an_blob(106, 36, 1, y0, col);
    } else if (cre.which == CRE_ELEPHANT) {  /* the elephant, its trunk hanging */
        static const an_pt O[] = {{10, 40}, {14, 20}, {40, 10}, {70, 10}, {84, 14}, {96, 7}, {108, 9}, {114, 20},
                                  {117, 30}, {124, 44}, {126, 60}, {122, 70}, {117, 69}, {120, 58}, {116, 44}, {108, 37},
                                  {100, 41}, {97, 47}, {97, 70}, {86, 70}, {86, 50}, {40, 51}, {38, 70}, {28, 70},
                                  {24, 51}, {12, 46}};
        static const an_pt EAR[] = {{84, 18}, {98, 18}, {97, 32}, {86, 34}};
        static const an_pt TUSK[] = {{110, 35}, {121, 39}};
        static const an_pt THROAT[] = {{92, 34}, {104, 35}}, MOUTH[] = {{104, 35}, {109, 37}};
        static const an_pt TRUNK[] = {{104, 33}, {114, 28}, {119, 40}, {122, 54}, {120, 68}};
        const float *v = cre.set[CRE_ELEPHANT];
        const float lo = seal_audio && cre_audio == CRE_ELEPHANT ? wave.vlv_open / 1.2f : 0.0f;   /* (CHIMERA's: of 1.2 cm2) */
        const int lips = (int)((lo > 1.0f ? 1.0f : lo) * 255.0f);
        an_trace(O, 26, 1, y0, col);
        an_trace(EAR, 4, 1, y0, dim);
        an_trace(TUSK, 2, 0, y0, T_HI);
        an_blob(58, 30, 7, y0, air);
        an_tube(THROAT, 2, 3, y0, air);
        an_tube(MOUTH, 2, 1 + (int)((1.0f - v[CH_ELE_TRUNK]) * 3.0f), y0, air);   /* the mouth, as TRUNK leaves it */
        an_tube(TRUNK, 5, 1 + (int)(v[CH_ELE_TRUNK] * 2.0f), y0, air);            /* the trunk */
        an_blob(114, 28, 2, y0, RGB(0x40 + lips * 0xbf / 255, 0x40 + lips * 0xbf / 255, 0x40 + lips * 0xbf / 255));   /* its lips */
        cv_rect(AN_X + 90, AN_Y + 32 - y0, 1, 4, T_MID);
        an_blob(106, 18, 1, y0, col);
    } else if (cre.which == CRE_ROBOT) {     /* the robot: a boxy head on a neck and a torso, an antenna; its tube from
                                              * the bellows in its chest to the grille at its mouth, the mouth as open as
                                              * the vowel the wobble makes now, the antenna's tip lit by the LFO */
        static const an_pt HEAD[] = {{44, 6}, {120, 6}, {125, 11}, {125, 33}, {120, 38}, {44, 38}, {39, 33}, {39, 11}};
        static const an_pt NECK[] = {{62, 38}, {62, 44}, {78, 44}, {78, 38}};
        static const an_pt BODY[] = {{26, 44}, {100, 44}, {104, 48}, {104, 70}, {22, 70}, {22, 48}};
        static const an_pt ANT[] = {{58, 6}, {52, 1}};
        static const an_pt EAR[] = {{48, 15}, {60, 15}, {60, 29}, {48, 29}};
        static const an_pt THROAT[] = {{44, 58}, {70, 58}, {70, 30}, {98, 30}};
        static const an_pt MOUTH[] = {{98, 30}, {125, 30}};
        const int live = seal_audio && cre_audio == CRE_ROBOT && seal_breath > 0.05f;
        const float w = live ? robot.open : cre.set[CRE_ROBOT][ROB_MOUTH];
        const int lit = live ? (int)(robot.lfo * 255.0f) : 0;
        int k;
        an_poly(HEAD, 8, 1, y0, col);
        an_poly(NECK, 4, 0, y0, col);
        an_poly(BODY, 6, 1, y0, col);
        an_poly(EAR, 4, 1, y0, dim);
        an_poly(ANT, 2, 0, y0, col);
        an_blob(52, 1, 1, y0, RGB(0x60 + lit * 0x9f / 255, 0x60 + lit * 0x9f / 255, 0x60 + lit * 0x9f / 255));
        cv_rect(AN_X + 98, AN_Y + 13 - y0, 20, 5, col);                    /* the visor */
        for (k = 0; k < 3; k++)                                            /* the chest's grille */
            cv_rect(AN_X + 78, AN_Y + 53 + 4 * k - y0, 16, 1, dim);
        an_blob(44, 58, 7, y0, air);                                       /* the bellows */
        an_tube(THROAT, 4, 3, y0, air);
        an_tube(MOUTH, 2, 1 + (int)(w * 6.0f), y0, air);                   /* the mouth, as open as the vowel now */
        cv_rect(AN_X + 68, AN_Y + 48 - y0, 5, 1, T_MID);                   /* the voice box */
    } else if (cre.which == CRE_SQUID) {     /* Squidlad: a small squid upright, its mantle a rounded point with a fin
                                              * either side, a big eye, its tentacles hanging (swaying a syllable at a
                                              * time); its airway from its breath to the mouth at its front, the mouth
                                              * as open as the lips now */
        static const an_pt MANTLE[] = {{62, 2}, {74, 8}, {86, 20}, {92, 34}, {90, 46}, {80, 52}, {64, 54}, {48, 52},
                                       {38, 46}, {36, 34}, {42, 20}, {52, 8}};
        static const an_pt FIN_R[] = {{82, 14}, {100, 12}, {90, 26}}, FIN_L[] = {{44, 14}, {26, 12}, {36, 26}};
        static const an_pt THROAT[] = {{58, 40}, {72, 42}, {84, 42}};
        const int live = seal_audio && cre_audio == CRE_SQUID && seal_breath > 0.05f;
        const float w = live ? squid.open : 0.3f + 0.6f * cre.set[CRE_SQUID][SQ_MOUTH];
        const int sway = live && (squid.syl & 1) ? 2 : 0;
        int k;
        an_trace(MANTLE, 12, 1, y0, col);
        an_trace(FIN_R, 3, 0, y0, col);
        an_trace(FIN_L, 3, 0, y0, col);
        for (k = 0; k < 5; k++) {                                          /* the tentacles, curling at the tips */
            const int x = 44 + k * 9, dx = (k & 1 ? sway : -sway);
            const an_pt T[4] = {{(uint8_t)x, 53}, {(uint8_t)(x - 3 + dx), 60}, {(uint8_t)(x + 1 + dx), 66},
                                {(uint8_t)(x - 2 - dx), 71}};
            an_trace(T, 4, 0, y0, k == 2 ? col : dim);
        }
        an_blob(74, 30, 5, y0, col);                                       /* the eye, big */
        an_blob(76, 29, 2, y0, C_BLACK);
        an_blob(52, 38, 4, y0, air);                                       /* its breath */
        an_tube(THROAT, 3, 2, y0, air);
        an_tube((const an_pt[]){{84, 42}, {90, 42}}, 2, 1 + (int)(w * 5.0f), y0, air);   /* the mouth, as open as the lips */
        cv_rect(AN_X + 62, AN_Y + 40 - y0, 1, 4, T_MID);                   /* the voice box */
    } else {                                 /* the cat, standing, its tail up */
        /* (the tail: a band 6 px wide either side of its curve, the rump to a round tip) */
        static const an_pt O[] = {{9, 3}, {11, 7}, {8, 16}, {8, 25}, {11, 33}, {18, 38}, {29, 41}, {46, 37}, {72, 37},
                                  {86, 33}, {89, 22}, {93, 13}, {95, 3}, {103, 10}, {110, 10}, {117, 3}, {118, 14},
                                  {123, 19}, {126, 25}, {122, 30}, {114, 34}, {104, 38}, {97, 47}, {97, 70}, {90, 70},
                                  {88, 53}, {42, 53}, {40, 70}, {33, 70}, {29, 50}, {27, 47}, {16, 44}, {7, 37}, {2, 27},
                                  {2, 16}, {5, 5}};
        static const an_pt THROAT[] = {{99, 37}, {108, 31}}, MOUTH[] = {{108, 31}, {118, 29}}, OPEN[] = {{118, 29}, {123, 29}};
        static const an_pt NOSE[] = {{108, 29}, {116, 24}, {125, 24}};
        const float *v = cre.set[CRE_CAT];
        an_trace(O, 36, 1, y0, col);
        an_blob(64, 45, 5, y0, air);
        an_tube(THROAT, 2, 2, y0, air);
        an_tube(MOUTH, 2, 2, y0, air);
        an_tube(OPEN, 2, 1 + (int)(v[CAT_MOUTH] * 3.0f), y0, air);
        an_tube(NOSE, 3, 1, y0, v[CAT_NOSE] > 0.05f ? air : dim);          /* the nose, lit as NOSE opens it */
        cv_rect(AN_X + 98, AN_Y + 35 - y0, 3, 1, T_MID);
        an_blob(114, 19, 1, y0, col);
    }
}

static void draw_seal(void)
{
    static const uint16_t *const col = KNOB_COL;
    const ch_cdef_t *d = CRE;
    char t[32];
    int32_t x, i;
    const int pick = cre.call[cre.which];
    const int playing = seal_playing >= 0 && (cvoice.call >= 0 || plat_ms() - seal_played_at < 300u);   /* (its call sounding) */
    const int pi = cre_pitch(d);
    float hz = n_skeys ? ch_ckey_hz(&d->p[pi], (float)skeys[n_skeys - 1]) : cre.set[cre.which][pi];
    if (cre_audio == cre.which && (cvoice.call >= 0 || cvoice.gate > 0.05f))   /* sounding: its pitch as it is */
        hz = cvoice.value[pi];
    if (cre.which == CRE_ELEPHANT && cre_audio == CRE_ELEPHANT && cvoice.value[CH_ELE_MODE] > 0.5f)
        hz = cvoice.value[CH_ELE_LIP_PITCH];   /* (trumpeting: the lips are what sounds) */
    else if (cre.which == CRE_ELEPHANT && !n_skeys && cvoice.call < 0 && cre.set[CRE_ELEPHANT][CH_ELE_MODE] > 0.5f)
        hz = cre.set[CRE_ELEPHANT][CH_ELE_LIP_PITCH];
    /* the header */
    cv_begin(240, HOME_TOP, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {
        t[0] = 0;
        text_cat(t, CRE_PAGE_NAME[cre.which][seal_page]);
        text_cat(t, seal_page ? " 2/2" : " 1/2");
        x = cv_text(8, 7, &FONT_XS, t, T_HI);
        x = cv_text(x + 10, 7, &FONT_XS, d->name, T_TEXT);
        t[0] = 0;
        text_cat(t, "CALL ");
        text_cat(t, d->calls[pick].name);
        cv_text(232 - text_w(&FONT_XS, t), 7, &FONT_XS, t, T_MID);
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the pitch, the airway, the call */
    for (i = 0; i < 2; i++) {
        const int32_t y0 = HOME_TOP + i * 76;
        cv_begin(240, 76, C_BLACK);
        if (i == 0) {
            int_text((int32_t)(hz + 0.5f), t);
            x = cv_text(8, 6, &FONT_L, t, n_skeys || cvoice.call >= 0 ? T_TEXT : T_HI);
            cv_text(x + 6, 20, &FONT_XS, "HZ", T_MID);
            cv_text(8, 46, &FONT_XS, n_skeys ? "KEYS: PITCH" : "PLAY A KEY", T_DIM);
            draw_airway(0);
        } else {
            const int c = playing ? seal_playing : pick;
            cv_rect(8, 0, 224, 1, T_LINE);
            cv_text(8, 8, &FONT_XS, playing ? "CALLING" : "PLAY: CALL  FX+KEY: AT ITS PITCH  LFO: HUMAN", playing ? T_TEXT : T_DIM);
            cv_text(8, 24, &FONT_M, d->calls[c].name, playing ? T_TEXT : T_HI);
            cv_text(8, 54, &FONT_XS, d->calls[c].hint, T_MID);
        }
        cv_blit(0, (uint32_t)y0);
    }
    /* the settings: the page's four, in the knobs' colours */
    cv_begin(240, 240 - HOME_TOP - 152, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    for (i = 0; i < 4; i++) {
        const int id = CRE_PAGE[cre.which][seal_page][i];
        const int32_t cx = 8 + i * 58;
        if (id < 0)
            continue;
        cv_rect(cx, 13, 4, 4, col[i]);
        cv_text(cx + 8, 9, &FONT_XS, d->p[id].name, col[i]);
        seal_text(id, t);
        cv_text(cx, 28, &FONT_M, t, col[i]);
    }
    cv_blit(0, (uint32_t)(HOME_TOP + 152));
}

/* ------------------------------------------------------------------ the SING page --- */
/* the head singing, the syllable now and next, the phrase's melody (each note a bar at its pitch, as long as its
 * beats; the note sounding lit), the knobs */
static void draw_song(void)
{
    const int mt = sng.tune - CH_NTUNES;      /* (>= 0: one of the MELODY page's) */
    const ch_tune_t *tn = &CH_TUNES[mt < 0 ? sng.tune : 0];
    const int now_k = song_note_now();
    char t[32];
    int32_t x, y0, i;
    /* the header: the tune and its tempo */
    cv_begin(240, HOME_TOP, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {
        char nm[16];
        tune_name(sng.tune, nm);
        x = cv_text(8, 7, &FONT_XS, "SING", T_HI);
        x = cv_text(x + 8, 7, &FONT_XS, nm, T_TEXT);
        if (sg.dirty)
            cv_text(x + 8, 7, &FONT_XS, "UNSAVED", T_WARN);
        int_text((int32_t)((mt < 0 ? tn->bpm : (float)mel.m[mt].bpm) * (float)sng.tempo / 100.0f + 0.5f), t);
        text_cat(t, " BPM");
        cv_text(232 - text_w(&FONT_XS, t), 7, &FONT_XS, t, T_MID);
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the head, and the syllables beside it */
    head_s = (float)BT_HEAD_H / (VIEW_Y1 - VIEW_Y0);
    head_x0 = 0.0f;
    head_y0 = (float)HOME_TOP;
    if (sg.beast)                             /* the Bestiary: the creature in the head's place */
        draw_beast_top(sg.playing ? "PLAYING" : "PLAY: START", sg.playing);
    else
    for (y0 = HOME_TOP; y0 < HOME_TOP + BT_HEAD_H; y0 += BT_HEAD_H / 2) {
        cv_begin(240, BT_HEAD_H / 2, C_BLACK);
        if (head_ready)
            draw_head(y0);
        cv_text(112, HOME_TOP + 6 - y0, &FONT_XS, sg.playing ? "SINGING" : "PLAY: SING", sg.playing ? T_TEXT : T_MID);
        t[0] = 0;
        if (now_k >= 0) {
            int b = now_k;
            while (b > 0 && sg_note[b].held)
                b--;
            upper(t, sg_syl[sg_note[b].syl].text, 10);
            if (sg_note[now_k].held)
                text_cat(t, "~");
        }
        cv_text(112, HOME_TOP + 22 - y0, &FONT_L, t[0] ? t : "-", now_k >= 0 ? T_TEXT : T_DIM);
        cv_text(112, HOME_TOP + 66 - y0, &FONT_XS, "WORDS", T_MID);
        {
            char sl[8] = "SLOT ";
            sl[5] = (char)('1' + set.slot);
            sl[6] = 0;
            cv_text(150, HOME_TOP + 66 - y0, &FONT_XS, sl, T_DIM);
        }
        upper(t, lyric[0] ? lyric : "(LA LA LA)", 14);
        if (lyric[14])
            text_cat(t, "..");
        cv_text(112, HOME_TOP + 80 - y0, &FONT_XS, t, T_HI);
        cv_blit(0, (uint32_t)y0);
    }
    /* the phrase's melody: the one singing (or the tune's first) */
    cv_begin(240, 60, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    {
        uint8_t m[40];
        float b[40], total = 0;
        int n = 0, cur = -1, lo = 127, hi = 0, a = 0;
        if (sg.playing && now_k >= 0) {        /* the fitted phrase around the note sounding */
            a = now_k;
            while (a > 0 && !sg_note[a - 1].phrase_end)
                a--;
            for (i = a; i < sg_n && n < 40; i++) {
                m[n] = sg_note[i].midi;
                b[n] = sg_note[i].beats;
                if (i == now_k)
                    cur = n;
                n++;
                if (sg_note[i].phrase_end)
                    break;
            }
        } else if (mt >= 0) {                 /* a melody's first phrase: its notes up to its first rest */
            const melody_t *ml = &mel.m[mt];
            for (i = 0; i < MEL_STEPS && n < 40; i++) {
                int j = i + 1;
                if (ml->s[i] <= MEL_TIE) {
                    if (n && ml->s[i] == MEL_REST)
                        break;
                    continue;
                }
                while (j < MEL_STEPS && ml->s[j] == MEL_TIE)
                    j++;
                m[n] = (uint8_t)clamp(ml->s[i] + ml->key + sng.key, 24, 108);
                b[n] = (float)(j - i) * 0.5f;
                n++;
            }
        } else {                              /* the tune's first phrase */
            for (i = 0; i < tn->phrase_len[0] && n < 40; i++) {
                m[n] = (uint8_t)(tn->notes[i].midi + sng.key);
                b[n] = (float)tn->notes[i].half_beats * 0.5f;
                n++;
            }
        }
        for (i = 0; i < n; i++) {
            total += b[i];
            lo = m[i] < lo ? m[i] : lo;
            hi = m[i] > hi ? m[i] : hi;
        }
        if (hi - lo < 6)
            hi = lo + 6;
        x = 8;
        for (i = 0; i < n; i++) {
            const int32_t w = (int32_t)(b[i] / total * 224.0f + 0.5f), y = 50 - (int32_t)((m[i] - lo) * 40 / (hi - lo));
            cv_rect(x + 1, y, w > 3 ? w - 2 : 1, 5, i == cur ? T_TEXT : i < cur ? T_DIM : T_HI);
            x += w;
        }
    }
    cv_blit(0, BT_GRID_Y);
    /* the knobs */
    cv_begin(240, 240 - BT_GRID_Y - 60, C_BLACK);
    {
        static const uint16_t *const col = KNOB_COL;
        static const char *const NM[4] = {"TEMPO", "KEY", "VIBRATO", "SLOT"};
        for (i = 0; i < 4; i++) {
            const int32_t cx = 8 + i * 58;
            t[0] = 0;
            if (i == 0) {
                text_cat(t, "X");
                fmt1((float)sng.tempo / 100.0f, t + 1);
                if (sng.tempo % 10) {         /* (two decimals when it has them) */
                    char d[2] = {(char)('0' + sng.tempo % 10), 0};
                    text_cat(t, d);
                }
            } else if (i == 1) {
                if (sng.key > 0)
                    text_cat(t, "+");
                int_text(sng.key, t + (sng.key > 0));
            } else if (i == 2) {
                if (sg.beast)                 /* (the Bestiary: no vibrato) */
                    continue;
                int_text(sng.vibrato, t);
            } else if (sg.beast) {            /* the Bestiary: SOUND */
                cv_rect(cx, 7, 4, 4, col[i]);
                cv_text(cx + 8, 3, &FONT_XS, "SOUND", col[i]);
                cv_text(cx, 22, &FONT_B, beast_sound_name(), col[i]);
                continue;
            } else {
                int_text(set.slot + 1, t);
            }
            cv_rect(cx, 7, 4, 4, col[i]);
            cv_text(cx + 8, 3, &FONT_XS, NM[i], col[i]);
            cv_text(cx, 18, &FONT_M, t, col[i]);
        }
    }
    cv_blit(0, BT_GRID_Y + 60);
}

/* SING and MELODY in the Bestiary (0.2.0): in the head's place, the creature (its pitch, the animal and its airway)
 * and how it plays a note; the strip's top at screen row HOME_TOP, BT_HEAD_H high */
static void draw_beast_top(const char *status, int lit)
{
    const ch_cdef_t *d = CRE;
    const int pi = cre_pitch(d), sounding = cre_audio == cre.which && (cvoice.call >= 0 || cvoice.gate > 0.05f);
    char t[24];
    int32_t x;
    cv_begin(240, 76, C_BLACK);
    cv_text(8, 6, &FONT_XS, status, lit ? T_TEXT : T_MID);
    if (sounding) {
        int_text((int32_t)(cvoice.value[pi] + 0.5f), t);
        x = cv_text(8, 22, &FONT_L, t, T_TEXT);
        cv_text(x + 6, 36, &FONT_XS, "HZ", T_MID);
    } else {
        cv_text(8, 22, &FONT_L, "-", T_DIM);
    }
    draw_airway(0);
    cv_blit(0, HOME_TOP);
    cv_begin(240, BT_HEAD_H - 76, C_BLACK);
    x = cv_text(8, 6, &FONT_XS, d->name, T_HI);
    x = cv_text(x + 10, 6, &FONT_XS, "SOUND", T_MID);
    cv_text(x + 6, 6, &FONT_XS, beast_sound_name(), T_K4);
    cv_text(8, 22, &FONT_XS, cre.sound[cre.which] ? "A CALL ON EACH NOTE" : "ITS VOICE HELD FOR EACH NOTE", T_DIM);
    cv_blit(0, HOME_TOP + 76);
}

/* ---------------------------------------------------------------- the MELODY page --- */
/* the head singing, the syllable now, the melody's 32 steps (each note a bar at its pitch as long as its ties; the
 * cursor outlined in step editing, the step singing lit), the knobs */
static void draw_melody(void)
{
    const int now_k = mel_note_now(), at = mel_step_now();
    char t[32];
    int32_t x, y0, i;
    cv_begin(240, HOME_TOP, C_BLACK);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], msg_col(T_TEXT));
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], msg_col(T_HI));
    } else {
        char nm[4] = {(char)('1' + mel.slot), 0};
        x = cv_text(8, 7, &FONT_XS, "MELODY", T_HI);
        x = cv_text(x + 8, 7, &FONT_XS, nm, T_TEXT);
        if (sg.dirty)
            cv_text(x + 8, 7, &FONT_XS, "UNSAVED", T_WARN);
        int_text(MEL.bpm, t);
        text_cat(t, " BPM");
        cv_text(232 - text_w(&FONT_XS, t), 7, &FONT_XS, t, T_MID);
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, T_LINE);
    cv_blit(0, 0);
    /* the head, the syllable */
    head_s = (float)BT_HEAD_H / (VIEW_Y1 - VIEW_Y0);
    head_x0 = 0.0f;
    head_y0 = (float)HOME_TOP;
    if (sg.beast)                             /* the Bestiary: the creature in the head's place */
        draw_beast_top(sg.playing ? "PLAYING" : sg.edit ? "STEP EDIT" : "PLAY: START", sg.playing || sg.edit);
    else
    for (y0 = HOME_TOP; y0 < HOME_TOP + BT_HEAD_H; y0 += BT_HEAD_H / 2) {
        cv_begin(240, BT_HEAD_H / 2, C_BLACK);
        if (head_ready)
            draw_head(y0);
        cv_text(112, HOME_TOP + 6 - y0, &FONT_XS, sg.playing ? "SINGING" : sg.edit ? "STEP EDIT" : "PLAY: SING",
                sg.playing || sg.edit ? T_TEXT : T_MID);
        t[0] = 0;
        if (now_k >= 0)
            upper(t, sg_syl[sg_note[now_k].syl].text, 10);
        cv_text(112, HOME_TOP + 22 - y0, &FONT_L, t[0] ? t : "-", now_k >= 0 ? T_TEXT : T_DIM);
        if (sg.edit) {
            cv_text(112, HOME_TOP + 66 - y0, &FONT_XS, "KEYS WRITE  OCT- REST", T_MID);
            cv_text(112, HOME_TOP + 80 - y0, &FONT_XS, "OCT+ TIE  EDIT: DONE", T_MID);
        } else {
            char sl[8] = "SLOT ";
            sl[5] = (char)('1' + set.slot);
            sl[6] = 0;
            cv_text(112, HOME_TOP + 66 - y0, &FONT_XS, "WORDS", T_MID);
            cv_text(150, HOME_TOP + 66 - y0, &FONT_XS, sl, T_DIM);
            upper(t, lyric[0] ? lyric : "(LA LA LA)", 14);
            if (lyric[14])
                text_cat(t, "..");
            cv_text(112, HOME_TOP + 80 - y0, &FONT_XS, t, T_HI);
        }
        cv_blit(0, (uint32_t)y0);
    }
    /* the steps */
    cv_begin(240, 60, C_BLACK);
    cv_rect(8, 0, 224, 1, T_LINE);
    {
        int lo = 127, hi = 0;
        for (i = 0; i < MEL_STEPS; i++)
            if (MEL.s[i] > MEL_TIE) {
                lo = MEL.s[i] < lo ? MEL.s[i] : lo;
                hi = MEL.s[i] > hi ? MEL.s[i] : hi;
            }
        if (hi < lo)
            lo = hi = 60;
        if (hi - lo < 8) {
            lo -= (8 - (hi - lo)) / 2;
            hi = lo + 8;
        }
        for (i = 0; i < MEL_STEPS; i += 4)    /* the beats: a tick every two (four steps) */
            cv_rect(8 + i * 7, 56, 1, 3, i % 8 ? T_LINE : T_DIM);
        if (at >= 0)                          /* the step singing */
            cv_rect(8 + at * 7, 3, 7, 52, T_SURF);
        for (i = 0; i < MEL_STEPS; i++) {
            int j = i + 1, lit = 0, q;
            int32_t y;
            if (MEL.s[i] <= MEL_TIE)
                continue;
            while (j < MEL_STEPS && MEL.s[j] == MEL_TIE)
                j++;
            for (q = i; q < j; q++)
                lit |= q == at;
            y = 50 - (int32_t)((MEL.s[i] - lo) * 36 / (hi - lo));   /* (the top row: the words' labels) */
            cv_rect(8 + i * 7 + 1, y, (j - i) * 7 - 2, 5, lit ? T_TEXT : T_HI);
        }
        {   /* where the words start again: a line before that step, cyan as SLOT (K4), labelled with the time through
             * the melody it falls in: stopped, 1ST, 2ND, ...; singing, NOW (this time through, bright), NEXT (the
             * next) and the later ones dim by number, the ones already sung gone */
            static const char *const ORD[8] = {"1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH"};
            int ws[8], k, q, nw;
            uint8_t wp[8];
            const int pass = mel_pass_now();
            const uint16_t dimc = RGB(0x1c, 0x5e, 0x66);
            nw = mel_wraps(ws, wp, 8);
            for (k = 0; k < nw; k++) {
                const int lit = pass < 0 || wp[k] == pass;
                const int32_t wx = 8 + ws[k] * 7;
                const char *lb = pass < 0 ? ORD[wp[k] & 7] : wp[k] == pass ? "NOW" : wp[k] == pass + 1 ? "NEXT"
                                                                                                    : ORD[wp[k] & 7];
                int32_t tw, tx;
                int dup = 0;
                if (pass >= 0 && wp[k] < pass)    /* (sung already) */
                    continue;
                for (q = 0; q < k; q++)           /* (two on one step: the first's label) */
                    dup |= ws[q] == ws[k] && !(pass >= 0 && wp[q] < pass);
                for (i = 13; i < 55; i += 3)      /* (dashed: a mark, not a note) */
                    cv_rect(wx, i, 1, 2, lit ? T_K4 : dimc);
                if (dup)
                    continue;
                tw = text_w(&FONT_T, lb);
                tx = wx + 2 + tw <= 232 ? wx + 2 : wx - 1 - tw;   /* (right of the line, or left at the edge) */
                cv_rect(wx, 3, 1, 10, lit ? T_K4 : dimc);
                cv_text(tx, 1, &FONT_T, lb, lit ? T_K4 : dimc);
            }
        }
        if (sg.edit) {                        /* the cursor */
            const int32_t cx = 8 + sg.cur * 7;
            cv_rect(cx, 2, 7, 1, T_TEXT);
            cv_rect(cx, 55, 7, 1, T_TEXT);
            cv_rect(cx, 2, 1, 54, T_TEXT);
            cv_rect(cx + 6, 2, 1, 54, T_TEXT);
        }
    }
    cv_blit(0, BT_GRID_Y);
    /* the knobs */
    cv_begin(240, 240 - BT_GRID_Y - 60, C_BLACK);
    {
        static const uint16_t *const col = KNOB_COL;
        static const char *const NM[4] = {"TEMPO", "KEY", "STEP", "SLOT"};
        for (i = 0; i < 4; i++) {
            const int32_t cx = 8 + i * 58;
            t[0] = 0;
            if (i == 0) {
                int_text(MEL.bpm, t);
            } else if (i == 1) {
                if (MEL.key > 0)
                    text_cat(t, "+");
                int_text(MEL.key, t + (MEL.key > 0));
            } else if (i == 2) {
                int_text(sg.cur + 1, t);
            } else if (sg.beast) {            /* the Bestiary: SOUND */
                cv_rect(cx, 7, 4, 4, col[i]);
                cv_text(cx + 8, 3, &FONT_XS, "SOUND", col[i]);
                cv_text(cx, 22, &FONT_B, beast_sound_name(), col[i]);
                continue;
            } else {
                int_text(set.slot + 1, t);
            }
            cv_rect(cx, 7, 4, 4, col[i]);
            cv_text(cx + 8, 3, &FONT_XS, NM[i], col[i]);
            cv_text(cx, 18, &FONT_M, t, col[i]);
        }
    }
    cv_blit(0, BT_GRID_Y + 60);
}

static void draw(void)
{
    if (ed.on)
        draw_edit();
    else if (page_debug)                     /* (over the beatbox and the SING page too: their load, measured) */
        draw_debug();
    else if (bt.on)
        draw_beat();
    else if (sg.on && sg.mel)
        draw_melody();
    else if (sg.on)
        draw_song();
    else if (body_cur == BODY_SEAL)
        draw_seal();
    else
        draw_home();
}

#ifdef PLAT_HOST
static int splash_hold;                      /* (the simulator: VOCAL_SPLASH=1 shows the boot screen and stays on it) */
#endif
void app_ui_init(void)
{
#ifdef PLAT_HOST
    if (getenv("VOCAL_SPLASH")) {
        app_splash();
        splash_hold = 1;
        return;
    }
#endif
    touch_on(1);                             /* velocity and pressure: the keys' readings on */
    /* the theme's greys for fm1-base's five roles: rules, inactive, labels, secondary, values */
    pal[0] = T_LINE;
    pal[1] = T_DIM;
    pal[2] = T_MID;
    pal[3] = T_HI;
    pal[4] = T_TEXT;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    home_init();
    ui.redraw = 1;
}

void app_frame(void)
{
#ifdef PLAT_HOST
    if (splash_hold)
        return;
#endif
    static uint32_t last_pct, lit_until, last_ble, last_midi;
    uint32_t i, now = plat_ms(), pct = plat_master() * 100u / 4096u;
    input();
    speak();
    song_tick();
    for (i = 0; i < NE; i++)                 /* an encoder's light goes out */
        if (ui.turned_at[i] && now - ui.turned_at[i] < 400u && now > lit_until)
            lit_until = ui.turned_at[i] + 300u;
    if (lit_until && now >= lit_until) {
        lit_until = 0;
        ui.redraw = 1;
    }
    if (FM1B_BLE_CENTRAL) {                  /* looking: a device found is named at once (REC connects to it) */
        static int found;
        int n = ui.looking ? btdev_count() : 0, ev;
        btdev_page(ui.looking && !set.ble_off);
        if (n > found && btdev_pick() < n)
            ble_device_say();
        found = n;
        while ((ev = btdev_event()) != BTDEV_NONE)   /* a device connected, given up or lost */
            if (ev == BTDEV_CONNECTED || ev == BTDEV_NO_ANSWER || ev == BTDEV_LOST)
                app_say("DEVICE", btdev_word(ev));
    }
    if (midi_count != last_midi) {
        last_midi = midi_count;
        ui.redraw = 1;
    }
    if (stat_update()) {
        static uint8_t drawn2 = 255, drawn1 = 255;   /* (the loads the brain was drawn with, in 5 % steps) */
        if (page_debug)
            ui.redraw = 1;
        else if (brain_c2 / 5u != drawn2 || brain_c1 / 5u != drawn1) {
            drawn2 = brain_c2 / 5u;
            drawn1 = brain_c1 / 5u;
            ui.redraw = 1;
        }
    }
    if (!page_debug && !ed.on && home_head_update())   /* the voice moved: the head again */
        ui.redraw = 1;
    if (!page_debug && !ed.on && body_cur != BODY_SEAL) {   /* the eye blinked, shut or opened, or turned red */
        const uint8_t e = eye_state(now);
        if (e != eye_drawn) {
            eye_drawn = e;
            ui.redraw = 1;
        }
    }
    if (body_cur == BODY_SEAL && !ed.on && !bt.on) {   /* the seal: its breath pulsing, ~20 times a second */
        static uint32_t seal_at;
        if (now - seal_at >= 50u && (seal_breath > 0.05f || cvoice.call >= 0 || plat_ms() - seal_played_at < 400u)) {
            seal_at = now;
            ui.redraw = 1;
        }
    }
    if (sg.on) {                             /* the SING page: the note sounding moved */
        static int was_note = -2;
        const int k = song_note_now();
        if (k != was_note) {
            was_note = k;
            ui.redraw = 1;
        }
    }
    if (bt.on) {                             /* the beatbox: the playhead moved, or a sound was heard (or faded) */
        static int8_t was_step = -2, was_sound = -2;
        const int8_t shown = beat_sound >= 0 && beat_blocks - beat_sound_at < 60u ? beat_sound : -1;
        if (beat_now != was_step || shown != was_sound) {
            was_step = beat_now;
            was_sound = shown;
            ui.redraw = 1;
        }
    }
    {
        static uint8_t was_speaking;
        if (speaking != was_speaking) {
            was_speaking = speaking;
            ui.redraw = 1;
        }
    }
    if (pct != last_pct || (uint32_t)(plat_ble_connected() | plat_ble_device() << 1) != last_ble ||
        (ui.msg_until && now >= ui.msg_until)) {
        last_pct = pct;
        last_ble = (uint32_t)(plat_ble_connected() | plat_ble_device() << 1);
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)c;
    (void)d;
    if (strcmp(cmd, "loads"))
        return 0;
    brain_c2 = (uint8_t)fm_clampf((float)atoi(a), 0, 100);
    brain_c1 = (uint8_t)fm_clampf((float)atoi(b), 0, 100);
    host_loads = 1;
    return 1;
}

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "wave"))
        *got = set.wave;
    else if (!strcmp(what, "note"))
        *got = voice_note;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "vel"))           /* the last key's velocity and pressure */
        *got = ui.vel[ui.last_key];
    else if (!strcmp(what, "prs"))
        *got = ui.prs[ui.last_key];
    else if (!strcmp(what, "edit"))          /* the word editor: open, its length, its cursor */
        *got = ed.on;
    else if (!strcmp(what, "edlen"))
        *got = ed.n;
    else if (!strcmp(what, "edcur"))
        *got = ed.cur;
    else if (!strcmp(what, "edchar"))        /* the character before the cursor (its code; 0 at the start) */
        *got = ed.cur ? (uint8_t)ed.t[ed.cur - 1] : 0;
    else if (!strcmp(what, "body"))          /* the body: the main loop's, and the audio's */
        *got = body_cur;
    else if (!strcmp(what, "body_audio"))
        *got = body_audio;
    else if (!strcmp(what, "f1x"))           /* the vowel pad's F1 at OPEN 0.5, Hz */
        *got = (int)(pad_f1(0.5f) + 0.5f);
    else if (!strcmp(what, "xvib"))          /* expression: the vibrato added (cents), the bend (cents), the breath (%) */
        *got = (int)(xvib_target + 0.5f);
    else if (!strcmp(what, "xbend"))
        *got = (int)(fm_log2f(xbend_target) * 1200.0f + (xbend_target >= 1.0f ? 0.5f : -0.5f));
    else if (!strcmp(what, "xbreath"))
        *got = (int)(vp_out.src.breathiness * 100.0f + 0.5f);
    else if (!strcmp(what, "kb"))
        *got = set.kb;
    else if (!strcmp(what, "slot"))
        *got = set.slot;
    else if (!strcmp(what, "bodysel"))       /* the body knob: 0-2 a body whole, 3 BLENDED */
        *got = (int)(kv[K_BODY] + 0.5f);
    else if (!strcmp(what, "gender"))        /* the blend (x100): its gender, its age; the audio's anatomy's scale (x1000) */
        *got = (int)(kv[K_GENDER] * 100.0f + 0.5f);
    else if (!strcmp(what, "age"))
        *got = (int)(kv[K_AGE] * 100.0f + 0.5f);
    else if (!strcmp(what, "bodyk"))
        *got = (int)(tract.k * 1000.0f + 0.5f);
    else if (!strcmp(what, "f0x"))           /* the speaking pitch (Hz) */
        *got = (int)(SPEECH_F0 + 0.5f);
    else if (!strcmp(what, "vmode"))         /* 0 WORDS, 1 VOWEL, 2 TONGUE; the tip held (TONGUE), the tongue body's
                                              * target forward (cm x100) */
        *got = vowel_mode;
    else if (!strcmp(what, "tipheld"))
        *got = !ctl.tip_free;
    else if (!strcmp(what, "tbx"))
        *got = (int)(ctl.target[CH_TB_X] * 100.0f + 0.5f);
    else if (!strcmp(what, "page"))
        *got = page;
    else if (!strcmp(what, "glo"))           /* the debug page shown */
        *got = page_debug;
    else if (!strcmp(what, "anat"))          /* the debug page's anatomy rate: every Nth control step */
        *got = anat_every;
    else if (!strcmp(what, "shelf"))         /* the debug page's treble shelf */
        *got = shelf_on;
    else if (!strncmp(what, "slotlen", 7))  /* slotlenN: slot N's length (0-based; 0: empty) */
        *got = slot_text[atoi(what + 7) % WORDS_SLOTS] ? slot_len[atoi(what + 7) % WORDS_SLOTS] : 0;
    else if (!strcmp(what, "sing"))          /* the SING page: open, singing, its tune, tempo (%), key, the note now, the notes
                                              * fitted, the phrases sent, the song's clock (ms) */
        *got = sg.on;
    else if (!strcmp(what, "singing"))
        *got = sg.playing;
    else if (!strcmp(what, "melody"))        /* the MELODY page: open, its melody, step editing, the cursor, step N's value
                                              * (0 rest, 1 tie, else the note), its notes, the notes sung, the step singing */
        *got = sg.on && sg.mel;
    else if (!strcmp(what, "melslot"))
        *got = mel.slot;
    else if (!strcmp(what, "meledit"))
        *got = sg.edit;
    else if (!strcmp(what, "melcur"))
        *got = sg.cur;
    else if (!strncmp(what, "melstep", 7))
        *got = MEL.s[atoi(what + 7) % MEL_STEPS];
    else if (!strcmp(what, "melnotes"))
        *got = mel_notes();
    else if (!strcmp(what, "melsung"))
        *got = sg_n;
    else if (!strcmp(what, "melat"))
        *got = mel_step_now();
    else if (!strcmp(what, "melnow"))
        *got = mel_note_now();
    else if (!strcmp(what, "beast"))         /* (0.2.0) SING / MELODY in the Bestiary: open there, the note the creature
                                              * plays (-1 none), its SOUND (0 held, else a call + 1) */
        *got = sg.on && sg.beast;
    else if (!strcmp(what, "beastk"))
        *got = beast_k;
    else if (!strcmp(what, "sound"))
        *got = cre.sound[cre.which];
    else if (!strcmp(what, "melpass"))       /* the time through the melody being sung (0: the 1st; -1 stopped) */
        *got = mel_pass_now();
    else if (!strncmp(what, "melwrappass", 11)) {   /* melwrappassN: the time through the melody it falls in (0: 1st) */
        int ws[8], n;
        uint8_t wp[8];
        n = mel_wraps(ws, wp, 8);
        *got = atoi(what + 11) < n ? wp[atoi(what + 11)] : -1;
    } else if (!strcmp(what, "melwraps"))      /* how many times the words start again in the song */
        *got = mel_wraps((int[8]){0}, (uint8_t[8]){0}, 8);
    else if (!strncmp(what, "melwrap", 7)) {  /* melwrapN: where the words start again, the Nth time (its step; -1 none) */
        int ws[8], n;
        uint8_t wp[8];
        n = mel_wraps(ws, wp, 8);
        *got = atoi(what + 7) < n ? ws[atoi(what + 7)] : -1;
    }
    else if (!strcmp(what, "tune"))
        *got = sng.tune;
    else if (!strcmp(what, "tempo"))
        *got = sng.tempo;
    else if (!strcmp(what, "key"))
        *got = sng.key;
    else if (!strcmp(what, "songnote"))
        *got = song_note_now();
    else if (!strcmp(what, "songnotes"))
        *got = sg_n;
    else if (!strcmp(what, "songsent"))
        *got = sg_next;
    else if (!strcmp(what, "songms"))
        *got = (int)(song_now * 1000.0f);
    else if (!strcmp(what, "beat"))          /* the beatbox: open, playing (the audio's), its step, slot, cursor, tempo */
        *got = bt.on;
    else if (!strcmp(what, "beatplay"))
        *got = seq.playing;
    else if (!strcmp(what, "beatstep"))
        *got = beat_now;
    else if (!strcmp(what, "beatslot"))
        *got = bt.slot;
    else if (!strcmp(what, "beatcur"))
        *got = bt.cur;
    else if (!strcmp(what, "beatbpm"))
        *got = beats.b[bt.slot].bpm;
    else if (!strcmp(what, "beatswing"))
        *got = beats.b[bt.slot].swing;
    else if (!strcmp(what, "beatedit"))
        *got = bt.edit;
    else if (!strcmp(what, "beatdirty"))
        *got = bt.dirty;
    else if (!strncmp(what, "beats", 5))    /* beatsN: step N's sound in the beat shown (-1 a rest) */
        *got = beats.b[bt.slot].s[atoi(what + 5) % BEAT_STEPS].id;
    else if (!strcmp(what, "sealaudio"))     /* the seal: the audio renders it, its call playing (-1 none), its pitch (Hz),
                                              * its gate (%), its page, its call picked, its breath setting (x10) */
        *got = seal_audio;
    else if (!strcmp(what, "elelips"))       /* the elephant's lips: the most they opened since the last ask (x1000 cm2) */
        *got = (int)(ele_lips_max * 1000.0f), ele_lips_max = 0.0f;
    else if (!strcmp(what, "sealcall"))
        *got = cvoice.call;
    else if (!strcmp(what, "brainc2"))       /* the brain's loads: core 2 (its colour), core 1 (its brightness), % */
        *got = brain_c2;
    else if (!strcmp(what, "brainc1"))
        *got = brain_c1;
    else if (!strcmp(what, "brainrgb"))      /* the brain's colour as drawn (0xRRGGBB) */
        *got = (int)((uint32_t)brain_hue().r << 16 | (uint32_t)brain_hue().g << 8 | brain_hue().b);
    else if (!strcmp(what, "squidsyl"))      /* Squidlad's syllables so far; its vowel now (0 EE .. 4 OO); its pitch's hop
                                              * now, tenths of a semitone */
        *got = squid.syl;
    else if (!strcmp(what, "squidvowel"))
        *got = squid.vowel;
    else if (!strcmp(what, "squidhop"))
        *got = (int)(squid.hop_now * 10.0f + (squid.hop_now < 0 ? -0.5f : 0.5f));
    else if (!strcmp(what, "robotwobble"))   /* the robot's WOBBLE setting, tenths of Hz */
        *got = (int)(cre.set[CRE_ROBOT][ROB_WOBBLE] * 10.0f + 0.5f);
    else if (!strcmp(what, "robotopen"))     /* the robot's vowel now, % ("oo" 0 .. "aa" 100: the wobble) */
        *got = (int)(robot.open * 100.0f + 0.5f);
    else if (!strcmp(what, "brainfill"))     /* the brain's fill, % opaque */
        *got = (int)(100.0f * BRAIN_FILL_MAX * fm_clampf((float)brain_c1 / BRAIN_FILL_AT, 0, 1) + 0.5f);
    else if (!strcmp(what, "sealhz"))
        *got = (int)(cvoice.value[cre_pitch(CRE_DEF[cre_audio])] + 0.5f);
    else if (!strcmp(what, "sealgate"))
        *got = (int)(cvoice.gate * 100.0f + 0.5f);
    else if (!strcmp(what, "sealpage"))
        *got = seal_page;
    else if (!strcmp(what, "sealpick"))
        *got = cre.call[cre.which];
    else if (!strcmp(what, "sealrate"))
        *got = (int)(cre.set[CRE_SEAL][CH_SEAL_PULSE_RATE] * 10.0f + 0.5f);
    else if (!strcmp(what, "creature"))      /* the Bestiary's creature (0 the seal, 1 the songbird, 2 the alligator, 3 the
                                              * elephant), the audio's (-1 the human voice), the songbird's interval (st x10) */
        *got = cre.which;
    else if (!strcmp(what, "creaudio"))
        *got = seal_audio ? cre_audio : -1;
    else if (!strcmp(what, "birdint"))
        *got = (int)(cre.set[CRE_BIRD][CH_BIRD_INTERVAL] * 10.0f + (cre.set[CRE_BIRD][CH_BIRD_INTERVAL] >= 0 ? 0.5f : -0.5f));
    else if (!strcmp(what, "beatage"))       /* audio blocks since that sound */
        *got = (int)(beat_blocks - beat_sound_at);
    else if (!strcmp(what, "beatname"))     /* the sound the beat page names now (-1: READY) */
        *got = beat_sound >= 0 && beat_blocks - beat_sound_at < 60u ? beat_sound : -1;
    else if (!strcmp(what, "heard"))         /* the sound heard last (-1: none yet) */
        *got = beat_sound;
    else if (!strncmp(what, "turns", 5))    /* turnsN: encoder N's count (plat.h EN_*) */
        *got = ui.turns[atoi(what + 5) % NE];
    else
        return 0;
    return 1;
}
#endif
