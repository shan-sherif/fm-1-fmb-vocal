/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * The head around the vocal tract, side on: CHIMERA's 2D anatomy view's outlines (src/ui/render/profile.ts
 * buildProfile: skin, skull, nasal cavity, eye, vertebrae, palate, jaw, teeth) built from the anatomy's frame
 * (ch_tract.c, applied with `draw` set), with the airway (the anatomy's outer and inner walls) and the tongue. Body cm,
 * x forward, y up, the glottis near the origin. For the FM-1's screens: the boot screen, the flat view. */
#ifndef CH_PROFILE_H
#define CH_PROFILE_H
#include "ch_tract.h"

typedef struct {
    ch_v2 skin[160];             /* closed: the neck's front, the chin, the lips, the face, the cranium */
    int n_skin;
    ch_v2 airway[CH_MAXPTS * 2]; /* closed: the outer wall glottis -> upper lip, back along the inner wall */
    int n_airway;
    ch_v2 tongue[64];            /* closed */
    int n_tongue;
    ch_v2 nasal[48];             /* closed */
    int n_nasal;
    ch_v2 skull[52];             /* closed */
    int n_skull;
    ch_v2 brain[52], sulcus[16]; /* closed, open */
    int n_brain, n_sulcus;
    ch_v2 upper_lip[12], lower_lip[12];   /* inside edge to outside */
    ch_v2 velum[11];             /* the soft palate's oral surface */
    ch_v2 trachea[2][2];
    ch_v2 palate[10];            /* open: the hard palate's oral line */
    ch_v2 jaw[7];                /* open: the mandible's body */
    int n_jaw;
    ch_v2 upper_teeth[5], lower_teeth[7];
    ch_v2 vertebrae[9][2];       /* each a box: two corners */
    int n_vertebrae;
    ch_v2 eye_c;
    ch_real eye_rx, eye_ry;
} ch_profile_t;

/* the visible part of the head, body cm (profile.ts VIEW) */
#define CH_VIEW_X0 (-7.5f)
#define CH_VIEW_X1 (10.25f)
#define CH_VIEW_Y0 (-4.55f)
#define CH_VIEW_Y1 (19.9f)

/* t: applied with t->draw set (its frame's drawing parts filled in) */
void ch_profile_build(const ch_tract_t *t, ch_profile_t *p);

#endif
