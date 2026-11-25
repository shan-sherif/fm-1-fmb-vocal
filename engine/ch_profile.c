/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * A port of CHIMERA's buildProfile (src/ui/render/profile.ts); see ch_profile.h. The brain, the turbinates, the
 * cords, the hyoid, the larynx's cartilages, the tongue's fibres and the gaze are the browser's and left out. */
#include "ch_profile.h"
#include "ch_common.h"

/* facial profile in true body coordinates (the tract's P() compresses the oral region's x, which would flatten the nose) */
static ch_v2 B(const ch_tract_t *t, ch_real x, ch_real y)
{
    ch_v2 r;
    r.x = x * t->k;
    r.y = (y < R(7.0) ? R(7.0) - (R(7.0) - y) * t->pharynx : y) * t->k;
    return r;
}

/* the posterior pharyngeal wall's x at height y (template cm) */
static ch_real wall_x(ch_real y) { return R(-1.2) - R(0.025) * y; }

void ch_profile_build(const ch_tract_t *t, ch_profile_t *p)
{
    const ch_real k = t->k;
    ch_v2 c[16], neck[40], face[48], cran[40];
    int n_neck, n_face, n_cran, i, n;
    const ch_v2 g = t->d_glottis;
#define BB(x, y) B(t, R(x), R(y))
#define JJ(x, y) ch_tract_jaw_point(t, R(x), R(y))
#define PP(x, y) ch_tract_point(t, R(x), R(y))
    /* the skin: the neck's front up to the chin, the lower lip's outer edge back to its edge, the upper lip's from its
     * edge to the nose, the face, the cranium down the back of the neck */
    c[0] = BB(2.3, -4.6); c[1] = BB(2.38, -1.5); c[2] = BB(2.52, 0.4); c[3] = BB(3.0, 1.7);
    c[4] = JJ(5.3, 3.0); c[5] = JJ(7.25, 3.35); c[6] = JJ(7.95, 4.6); c[7] = JJ(7.55, 5.7);
    n_neck = ch_catmull_rom(c, 8, 4, neck);
    c[0] = t->d_ulip_out[5];
    c[1] = BB(7.05, 10.6); c[2] = BB(7.75, 10.78); c[3] = BB(8.75, 11.05); c[4] = BB(9.25, 11.65); c[5] = BB(8.95, 12.6);
    c[6] = BB(8.35, 13.9); c[7] = BB(7.7, 15.0); c[8] = BB(7.95, 16.1); c[9] = BB(7.9, 18.1); c[10] = BB(6.9, 20.5);
    n_face = ch_catmull_rom(c, 11, 4, face);
    c[0] = BB(6.9, 20.5); c[1] = BB(3.0, 22.9); c[2] = BB(-2.2, 22.7); c[3] = BB(-6.9, 19.6); c[4] = BB(-8.7, 15.4);
    c[5] = BB(-8.3, 11.9); c[6] = BB(-7.4, 8.9); c[7] = BB(-6.8, 4.8); c[8] = BB(-6.6, 0.0); c[9] = BB(-6.6, -4.6);
    n_cran = ch_catmull_rom(c, 10, 4, cran);
    n = 0;
    for (i = 0; i < n_neck; i++)
        p->skin[n++] = neck[i];
    for (i = 5; i >= 0; i--)   /* the lower lip, outside: chin -> its edge */
        p->skin[n++] = t->d_llip_out[i];
    for (i = 0; i < 6; i++)    /* the upper lip, outside: its edge -> the nose */
        p->skin[n++] = t->d_ulip_out[i];
    for (i = 1; i < n_face; i++)
        p->skin[n++] = face[i];
    for (i = 1; i < n_cran; i++)
        p->skin[n++] = cran[i];
    p->n_skin = n;
    /* the skull's inner table */
    c[0] = BB(7.0, 16.2); c[1] = BB(7.3, 18.5); c[2] = BB(6.0, 20.7); c[3] = BB(3.0, 22.1); c[4] = BB(-2.0, 22.0);
    c[5] = BB(-6.3, 19.2); c[6] = BB(-7.9, 15.3); c[7] = BB(-7.4, 12.6); c[8] = BB(-4.6, 12.2); c[9] = BB(-2.2, 13.3);
    c[10] = BB(1.0, 13.7); c[11] = BB(4.1, 14.1); c[12] = BB(6.2, 14.9);
    p->n_skull = ch_catmull_rom(c, 13, 4, p->skull);
    /* the brain and a sulcus */
    c[0] = BB(6.55, 16.3); c[1] = BB(6.75, 18.4); c[2] = BB(5.5, 20.1); c[3] = BB(2.8, 21.4); c[4] = BB(-1.9, 21.3);
    c[5] = BB(-5.8, 18.8); c[6] = BB(-7.1, 15.4); c[7] = BB(-6.7, 13.1); c[8] = BB(-4.4, 12.8); c[9] = BB(-2.1, 13.9);
    c[10] = BB(1.0, 14.3); c[11] = BB(4.0, 14.7); c[12] = BB(5.9, 15.4);
    p->n_brain = ch_catmull_rom(c, 13, 4, p->brain);
    c[0] = BB(5.4, 19.4); c[1] = BB(2.4, 20.5); c[2] = BB(-1.4, 20.4); c[3] = BB(-4.8, 18.4);
    p->n_sulcus = ch_catmull_rom(c, 4, 4, p->sulcus);
    /* the lips (inside edge, then the outside: HumanFrame's upper_lip / lower_lip), the soft palate, the trachea */
    for (i = 0; i < 7; i++) {
        p->upper_lip[i] = t->upper_lip_in[i];
        p->lower_lip[i] = t->lower_lip_in[i];
    }
    for (i = 1; i < 6; i++) {
        p->upper_lip[6 + i] = t->d_ulip_out[i];
        p->lower_lip[6 + i] = t->d_llip_out[i];
    }
    for (i = 0; i < 11; i++)
        p->velum[i] = t->vel_oral[i];
    p->trachea[0][0].x = g.x - R(0.65) * k; p->trachea[0][0].y = g.y;
    p->trachea[0][1].x = g.x - R(0.75) * k; p->trachea[0][1].y = R(-4.8);
    p->trachea[1][0].x = g.x + R(0.75) * k; p->trachea[1][0].y = g.y;
    p->trachea[1][1].x = g.x + R(0.95) * k; p->trachea[1][1].y = R(-4.8);
    /* the nasal cavity */
    c[0] = BB(0.3, 11.2); c[1] = BB(1.1, 10.25); c[2] = BB(2.7, 10.75); c[3] = BB(4.6, 10.85); c[4] = BB(6.4, 10.65);
    c[5] = BB(7.6, 10.85); c[6] = BB(8.45, 11.3); c[7] = BB(8.0, 12.2); c[8] = BB(6.7, 13.4); c[9] = BB(4.4, 13.9);
    c[10] = BB(1.8, 13.6); c[11] = BB(0.2, 12.6);
    p->n_nasal = ch_catmull_rom(c, 12, 4, p->nasal);
    /* the eye */
    p->eye_c = BB(6.5, 15.4);
    p->eye_rx = R(1.1) * k;
    p->eye_ry = R(0.95) * k;
    /* the vertebrae: boxes behind the pharyngeal wall */
    p->n_vertebrae = 0;
    for (i = 0; i < 9; i++) {
        ch_real y0 = R(-4.4) + (ch_real)i * R(1.75), x0;
        if (y0 > R(10.5))
            break;
        x0 = wall_x(y0) - R(2.25);
        p->vertebrae[p->n_vertebrae][0] = ch_tract_point(t, x0, y0 + R(1.4));
        p->vertebrae[p->n_vertebrae][1] = ch_tract_point(t, x0 + R(1.6), y0);
        p->n_vertebrae++;
    }
    /* the hard palate (oral line) */
    p->palate[0] = PP(1.0, 9.6); p->palate[1] = PP(1.8, 9.92); p->palate[2] = PP(2.7, 10.18); p->palate[3] = PP(3.6, 10.3);
    p->palate[4] = PP(4.4, 10.25); p->palate[5] = PP(5.1, 10.05); p->palate[6] = PP(5.7, 9.72); p->palate[7] = PP(6.15, 9.36);
    p->palate[8] = PP(6.55, 9.15); p->palate[9] = PP(7.0, 9.6);
    /* the mandible's body only: its ramus is lateral, not in the midsagittal plane */
    p->n_jaw = 0;
    for (i = 0; i < 7; i++)
        if (t->d_jaw_bone[i].x > R(0.8) * k)
            p->jaw[p->n_jaw++] = t->d_jaw_bone[i];
    for (i = 0; i < 5; i++)
        p->upper_teeth[i] = t->d_upper_teeth[i];
    for (i = 0; i < 4; i++)
        p->lower_teeth[i] = t->d_lower_teeth[i];
    p->lower_teeth[4] = JJ(6.8, 6.0);
    p->lower_teeth[5] = JJ(6.4, 6.2);
    p->lower_teeth[6] = t->d_lower_teeth[0];
    /* the airway: the outer wall (above the glottis or in front of it) to the upper lip, back along the inner wall */
    n = 0;
    for (i = 0; i < t->outer.n; i++)
        if (t->outer.p[i].y > g.y || t->outer.p[i].x > 0)
            p->airway[n++] = t->outer.p[i];
    for (i = t->inner.n - 1; i >= 0; i--)
        p->airway[n++] = t->inner.p[i];
    p->n_airway = n;
    for (i = 0; i < t->n_tongue && i < 64; i++)
        p->tongue[i] = t->d_tongue[i];
    p->n_tongue = i;
#undef BB
#undef JJ
#undef PP
}
