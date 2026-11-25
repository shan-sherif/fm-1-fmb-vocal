/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * CHIMERA's Sing mode, live (src/ui/singing.ts: singNoteOn, singNoteOff, startNote, glide): the lyrics become
 * syllables with a cursor; each key sings the next syllable at its pitch. A key pressed while the previous one is
 * still down sings legato (the previous syllable's coda runs straight into the next onset); a "-" in the lyrics holds
 * the vowel onto a new pitch (melisma). Releasing the sounding key while another is still down glides back to it.
 *
 * What it decides goes out as a command (gestures relative to now, tags to release now) for whoever owns the score:
 * on the FM-1 the main loop plans and the audio interrupt applies (ch_live_apply). */
#ifndef CH_LIVE_H
#define CH_LIVE_H
#include "ch_speech.h"

#define CH_LIVE_G 48             /* gestures a command carries (a legato note on: ~30) */
#define CH_LIVE_KEYS 16

typedef struct {
    ch_gesture_t g[CH_LIVE_G];
    int n;
    ch_real release[CH_LIVE_KEYS];   /* tags to release, before the gestures go in */
    int n_release;
    uint32_t dropped;
} ch_live_cmd_t;

typedef struct {
    ch_syl_t syl[CH_SYL_MAX];
    int n_syl, cursor;
    ch_real vibrato;             /* cents (CHIMERA's default 35) */
    /* the note sounding: its pitch, its syllable (a melisma keeps its parent's, for the coda), its tags */
    uint8_t sounding;
    ch_real s_midi, s_tag;
    ch_syl_t s_syl;
    ch_real keys[CH_LIVE_KEYS];  /* keys down, oldest first */
    int n_keys;
    ch_real next_tag;
    ch_real tails[8];            /* endings (glide, coda) that may still be playing: tag, and tag + 1 the breath-out */
    int n_tails;
    uint8_t exhales_live;        /* endings whose breath-out a following note has not dropped */
} ch_live_t;

void ch_live_init(ch_live_t *l);
/* new lyrics (the cursor back to the start); returns the syllable count */
int ch_live_lyrics(ch_live_t *l, const char *text, ch_lookup_fn lookup, void *user);
/* velocity 0..127 (CHIMERA's breath for it: 1 at 96) */
void ch_live_note_on(ch_live_t *l, ch_real midi, ch_real velocity, ch_live_cmd_t *cmd);
void ch_live_note_off(ch_live_t *l, ch_real midi, ch_live_cmd_t *cmd);
/* the syllable the next key sings, and the one sounding (NULL: none) */
const ch_syl_t *ch_live_next(const ch_live_t *l);
const ch_syl_t *ch_live_sounding(const ch_live_t *l);
/* a command into a control's score, now: its releases, then its gestures shifted to the control's clock */
void ch_live_apply(const ch_live_cmd_t *cmd, ch_control_t *c);

#endif
