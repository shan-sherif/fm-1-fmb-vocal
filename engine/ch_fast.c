/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * CHIMERA's audio rate, rearranged to fit the FM-1. The physics is CHIMERA's (ch_voice.c is the
 * literal port it is measured against); what changes is how often the geometry is computed, and
 * where the per-sample values live.
 *
 * CHIMERA moves every area along a linear ramp each sample and recomputes, every sample, every term
 * that depends on the areas: 1/A, sqrt(A), each branch's inertance L, resistance R and kinetic
 * losses, each node's compliance, the wall and radiation coefficients. Most of the tract moves
 * slowly, and those terms barely change between samples: here they are recomputed every geo_every
 * samples (the update), the ramps advanced geo_every steps at once.
 *
 * Two things can't wait for the update:
 *  - A closing or opening constriction. L goes as 1/A, so as A ramps toward 0 it can grow tenfold
 *    in a few samples, and holding it lets too much air through. A node whose area (or minimum
 *    area) moves by more than DYN_REL of itself over one update is "dynamic" for that update: its
 *    geometry, the branches beside it and its own coefficients are recomputed every sample, exactly
 *    as CHIMERA does. During speech that is a handful of nodes at a time, often none.
 *  - The moving-wall injection. The output is the time derivative of the radiated flow, so a flow
 *    held for an update and then stepped comes out as a click every update. It is ramped linearly
 *    from one update's value to the next instead (its 60 Hz low-pass runs at the update rate).
 *
 * The per-sample stages read one struct per branch (ch_fbr_t) and per node (ch_fnd_t): CHIMERA's
 * separate arrays, a few kilobytes apart in the voice, cost the FM-1 an address computation for
 * nearly every access. The acoustic state lives there while rendering; between control blocks it
 * is copied back to CHIMERA's arrays (p, u, mom, wall), which the shared control-step code reads.
 *
 * The branch flow solves CHIMERA's quadratic  (L/T + R) U + k |U| U = c  as
 *   U = (c / bq) * 2 / (1 + sqrt(1 + x)),  x = 4 k |c| / bq^2,
 * the same root; when x < X_SERIES (no constriction: almost every branch, almost every sample)
 * 2 / (1 + sqrt(1 + x)) is taken from its series, 1 - x/4 + x^2/8 (relative error < 1e-10). */
#define CH_RAM   /* the audio rate in RAM on the FM-1 (ch_common.h CH_HOT) */
#include "ch_common.h"

#define X_SERIES R(1e-3)
#define DYN_REL R(0.02)
#if defined(__GNUC__) || defined(__clang__)
#define CH_NOINLINE __attribute__((noinline))   /* kept apart: they run once an update, not once a sample */
#else
#define CH_NOINLINE
#endif
/* -DCH_ANALYZE keeps the per-sample stages apart too, so tools/pi_cost.py can cost each one */
#ifdef CH_ANALYZE
#define CH_PART CH_NOINLINE
#else
#define CH_PART
#endif

/* ------------------------------------------------- the hot structs <-> CHIMERA's arrays --- */

/* the acoustic state back into CHIMERA's arrays (before the shared control-step code) */
CH_HOT static void sync_out(ch_voice_t *v)
{
    int k, j;
    for (k = 0; k < v->n; k++) {
        v->p[k] = v->fn[k].p;
        v->wall_v[k] = v->fn[k].wv;
        v->wall_y[k] = v->fn[k].wy;
    }
    for (j = 0; j < v->nb; j++) {
        v->u[j] = v->fb[j].u;
        v->mom[j] = v->fb[j].mom;
    }
}

/* and in again after it (it may have flushed them) */
CH_HOT static void sync_in(ch_voice_t *v)
{
    int k, j;
    for (k = 0; k < v->n; k++) {
        v->fn[k].p = v->p[k];
        v->fn[k].wv = v->wall_v[k];
        v->fn[k].wy = v->wall_y[k];
    }
    for (j = 0; j < v->nb; j++) {
        v->fb[j].u = v->u[j];
        v->fb[j].mom = v->mom[j];
    }
}

void ch_fast_init_state(ch_voice_t *v, int geo_every)
{
    int i;
    ch_real keep = 1;
    init_state(v);
    v->geo_every = (uint16_t)(geo_every < 1 ? 1 : geo_every);
    v->geo_left = 0;
    for (i = 0; i < v->geo_every; i++)
        keep *= R(1.0) - v->vol_lp_coef;
    v->vol_lp_coef_n = R(1.0) - keep;
    for (i = 0; i < v->n; i++) {
        ch_fnd_t *f = &v->fn[i];
        f->p = f->flow = f->kn = f->inj = f->dinj = 0;
        f->wa = f->winv = f->wsd = f->wv = f->wy = 0;
        f->wk = v->wall_k[i];
        f->wrad = v->wall_rad[i];
        f->rad = v->rad_of[i];
        f->fixed = v->fixed[i];
        f->wall = v->has_wall[i];
        f->sealed = 0;
        v->nd_vol_upd[i] = v->vol[i];
        v->nd_dyn[i] = 0;
    }
    for (i = 0; i < v->nb; i++) {
        ch_fbr_t *b = &v->fb[i];
        b->l = b->ibq = b->kf = b->kb = b->u = b->mom = b->l_prev = b->psrc = 0;
        b->a = v->b_a[i];
        b->b = v->b_b[i];
        b->closed = 1;
        b->pad = 0;
        v->br_dyn[i] = 0;
    }
    v->n_dyn_nodes = v->n_dyn_branches = v->n_dyn_coef = 0;
}

CH_HOT void ch_fast_block(ch_voice_t *v, const ch_block_t *b)
{
    sync_out(v);
    block_common(v, b);
    sync_in(v);
    v->geo_left = 0;   /* the update re-aligns with each control block */
}

/* ------------------------------------------------------------------ the parts of the geometry --- */

/* a node's area, minimum area and length `steps` samples along their ramps, and its derived terms */
CH_HOT static void node_geo(ch_voice_t *v, int k, ch_real steps)
{
    ch_real a = v->area[k] + steps * v->area_step[k], am, amk, d, ia, sa;
    if (a < 0)
        a = 0;
    v->area[k] = a;
    am = v->amin[k] + steps * v->amin_step[k];
    if (am < 0)
        am = 0;
    amk = am > a ? a : am;
    v->amin[k] = amk;
    v->dx[k] += steps * v->dx_step[k];
    d = v->dx[k];
    ia = R(1.0) / (a > R(1e-6) ? a : R(1e-6));
    v->inv_a[k] = ia;
    v->inv_amin[k] = R(1.0) / (amk > R(1e-6) ? amk : R(1e-6));
    sa = SQRT(a);
    v->sqrt_a[k] = sa;
    v->half_l[k] = d * ia;
    v->half_r[k] = v->wall_perim[k] * d * sa * ia * ia * v->visc_f[k];
    v->vol[k] = (a > v->min_node_area ? a : v->min_node_area) * d;
}

/* a branch's L, R, kinetic coefficients and 1 / (L/T + R) from its nodes' current terms */
CH_HOT static void branch_coef(ch_voice_t *v, int j)
{
    const int a = v->b_a[j], b = v->b_b[j];
    const ch_real half_rho = R(0.5) * RHO, closed = v->closed_area;
    const ch_real visc = SQRT((RHO * MU * v->viscous_omega) / R(2.0)) * v->viscous_scale;
    ch_fbr_t *f = &v->fb[j];
    ch_real l, r, kf, kb;
    if (v->b_kind[j] == CH_SERIES) {
        ch_real aa = v->area[a], ab = v->area[b], ma = v->amin[a], mb = v->amin[b];
        ch_real ia, ib, ima, imb, ef, eb, cf, cb, a_eff = ma < mb ? ma : mb;
        v->branch_area[j] = a_eff;
        if ((aa < ab ? aa : ab) < closed || a_eff < closed) {
            f->closed = 1;
            f->u = 0;
            f->mom = 0;
            return;
        }
        ia = v->inv_a[a];
        ib = v->inv_a[b];
        l = half_rho * (v->half_l[a] + v->half_l[b]);
        r = R(0.5) * visc * (v->half_r[a] + v->half_r[b]);
        ima = v->inv_amin[a];
        imb = v->inv_amin[b];
        ef = ima - ib;
        eb = imb - ia;
        kf = ef > 0 ? half_rho * ef * ef : 0;
        kb = eb > 0 ? half_rho * eb * eb : 0;
        cf = imb - ia;
        cb = ima - ib;
        if (cf > 0)
            kf += R(0.1) * half_rho * cf * cf;
        if (cb > 0)
            kb += R(0.1) * half_rho * cb * cb;
    } else {
        ch_real ap = v->port_area[j], ip, len, w, ex_f, ex_b, kin;
        v->branch_area[j] = ap;
        if (ap < closed) {
            f->closed = 1;
            f->u = 0;
            f->mom = 0;
            return;
        }
        ip = R(1.0) / ap;
        len = v->port_len[j];
        l = RHO * len * ip;
        if (v->port_half[j])
            l += half_rho * (v->half_l[a] + v->half_l[b]);
        w = v->port_slit[j];
        r = w > 0 ? (R(12.0) * MU * len * w * w) * ip * ip * ip : visc * R(2.0) * SQRT(CH_PI) * len * SQRT(ap) * ip * ip;
        r += v->port_extra_r[j];
        ex_f = ip - v->inv_a[b];
        ex_b = ip - v->inv_a[a];
        kin = v->port_kin[j] * half_rho * ip * ip;
        kf = kin + v->port_kex[j] * half_rho * (ex_f > 0 ? ex_f * ex_f : 0);
        kb = kin + v->port_kex[j] * half_rho * (ex_b > 0 ? ex_b * ex_b : 0);
    }
    if (v->symmetric_kinetic) {
        ch_real km = kf > kb ? kf : kb;
        kf = km;
        kb = km;
    }
    f->closed = 0;
    f->l = l;
    f->kf = kf;
    f->kb = kb;
    f->ibq = R(1.0) / (l * v->inv_t + r);
}

CH_HOT static void port_advance(ch_voice_t *v, int j, ch_real steps)
{
    v->port_area[j] += steps * v->port_area_step[j];
    if (v->port_area[j] < 0)
        v->port_area[j] = 0;
}

/* a node's wall, compliance (with the junction floor) and radiation coefficients */
CH_HOT static void node_coef(ch_voice_t *v, int k)
{
    const ch_real t = v->t, inv_t = v->inv_t, min_a = v->min_node_area, cl = v->closed_area;
    ch_fnd_t *f = &v->fn[k];
    ch_real c;
    if (f->wall) {
        ch_real m = v->wall_m[k], r = v->wall_r[k];
        ch_real sa = v->area[k] > min_a ? v->sqrt_a[k] : SQRT(min_a);
        f->wa = m * inv_t - R(0.5) * r;
        f->winv = R(1.0) / (m * inv_t + R(0.5) * r);
        f->wsd = v->wall_perim[k] * sa * v->dx[k];
    }
    if (f->fixed)
        return;
    f->sealed = v->area[k] < cl || v->amin[k] < cl;
    if (f->sealed)
        return;
    c = v->vol[k] / RHO_C2;
    if (v->is_junction[k] && v->enable_junction_floor) {
        ch_real c_min = (t * t * v->inv_lsum[k]) / R(2.4);
        if (c < c_min)
            c = c_min;
    }
    if (f->rad < 0) {
        f->kn = t / c;
    } else {
        int r = f->rad;
        ch_real a = v->area[k] > R(1e-4) ? v->area[k] : R(1e-4);
        ch_real lr = (R(8.0) * RHO) / (R(3.0) * CH_PI * SQRT(CH_PI * a));
        ch_real rr = (R(128.0) * RHO * C_SOUND) / (R(9.0) * CH_PI * CH_PI * a);
        ch_real lp = v->rad_l_prev[r], h, g;
        /* energy-safe: when the radiation inertance grows (aperture closing) conserve momentum */
        if (lp > 0 && lr > lp)
            v->rad_ul[r] *= lp / lr;
        v->rad_l_prev[r] = lr;
        h = t / (R(2.0) * lr);
        g = R(0.5) / rr;
        v->rad_h[r] = h;
        v->rad_g[r] = g;
        v->rad_num[r] = c * inv_t - R(0.5) * h - g;
        v->rad_iden[r] = R(1.0) / (c * inv_t + R(0.5) * h + g);
    }
}

CH_HOT static inline int moves_fast(ch_real step, ch_real level, ch_real s_n, ch_real floor_)
{
    ch_real d = FABS(step) * s_n, ref = level > floor_ ? level : floor_;
    return d > DYN_REL * ref;
}

/* --------------------------------------------------------------------------------- the update --- */
CH_HOT CH_NOINLINE static void geometry(ch_voice_t *v)
{
    const int n = v->n, nb = v->nb, gp = v->glottis_port;
    const ch_real s_n = (ch_real)v->geo_every, inv_t = v->inv_t, sq = v->squeeze_area, cl = v->closed_area;
    uint8_t coef_dyn[CH_MAXN];
    int k, j, e;

    /* which nodes and branches move too fast to hold for this update */
    v->n_dyn_nodes = v->n_dyn_branches = v->n_dyn_coef = 0;
    for (k = 0; k < n; k++) {
        v->nd_dyn[k] = !v->fixed[k] &&
                       (moves_fast(v->area_step[k], v->area[k], s_n, cl) || moves_fast(v->amin_step[k], v->amin[k], s_n, cl) ||
                        moves_fast(v->dx_step[k], v->dx[k], s_n, R(1e-3)));
        if (v->nd_dyn[k])
            v->dyn_nodes[v->n_dyn_nodes++] = (uint16_t)k;
        coef_dyn[k] = v->nd_dyn[k];
    }
    for (j = 0; j < nb; j++) {
        int a = v->b_a[j], b = v->b_b[j];
        v->br_dyn[j] = j != gp && (v->nd_dyn[a] || v->nd_dyn[b] ||
                                   (v->b_kind[j] == CH_PORT && moves_fast(v->port_area_step[j], v->port_area[j], s_n, cl)));
        if (v->br_dyn[j]) {
            v->dyn_branches[v->n_dyn_branches++] = (uint16_t)j;
            /* a junction on a dynamic branch follows its 1/L sum (the stability floor) per sample */
            if (v->is_junction[a])
                coef_dyn[a] = 1;
            if (v->is_junction[b])
                coef_dyn[b] = 1;
        }
    }
    for (k = 0; k < n; k++)
        if (coef_dyn[k])
            v->dyn_coef[v->n_dyn_coef++] = (uint16_t)k;

    /* 1) the slow nodes advance a whole update (the dynamic ones advance per sample) */
    for (k = 0; k < n; k++)
        if (!v->nd_dyn[k])
            node_geo(v, k, s_n);

    /* the moving-wall injection: CHIMERA's split of each node's −dV/dt, from the volume change
     * since the last update, low-passed at the update rate and ramped over the next update */
    if (v->enable_volume_source) {
        ch_real c = v->vol_lp_coef_n;
        for (k = 0; k < n; k++)
            v->spill[k] = 0;
        for (k = 0; k < n; k++) {
            ch_real raw = -(v->vol[k] - v->nd_vol_upd[k]) * inv_t / s_n;
            ch_real a = v->area[k], ae = v->amin[k] < a ? v->amin[k] : a;
            v->nd_vol_upd[k] = v->vol[k];
            if (raw != 0 && ae < sq) {
                ch_real tt = ae / sq, keep = tt * tt * (R(3.0) - R(2.0) * tt), sp;
                int s0 = v->nbr_start[k], s1 = v->nbr_start[k + 1], n_open = 0;
                v->spill[k] += raw * keep;
                sp = raw * (R(1.0) - keep);
                for (e = s0; e < s1; e++) {
                    int m = v->nbr_list[e];
                    if (mn(v->area[m], v->amin[m]) >= sq || v->amin_target[m] >= sq)
                        n_open++;
                }
                if (n_open > 0) {
                    ch_real share = sp / (ch_real)n_open;
                    for (e = s0; e < s1; e++) {
                        int m = v->nbr_list[e];
                        if (mn(v->area[m], v->amin[m]) >= sq || v->amin_target[m] >= sq)
                            v->spill[m] += share;
                    }
                }
            } else {
                v->spill[k] += raw;
            }
        }
        for (k = 0; k < n; k++) {
            v->vol_src1[k] += c * (v->spill[k] - v->vol_src1[k]);
            v->vol_src2[k] += c * (v->vol_src1[k] - v->vol_src2[k]);
            v->fn[k].dinj = (v->vol_src2[k] - v->fn[k].inj) / s_n;
        }
    }

    /* 3) the slow branches; the 1/L sums of the junctions over them */
    for (k = 0; k < n; k++)
        v->nd_lsum_static[k] = 0;
    for (j = 0; j < nb; j++) {
        int a = v->b_a[j], b = v->b_b[j];
        if (j == gp || v->br_dyn[j])
            continue;
        if (v->b_kind[j] == CH_PORT)
            port_advance(v, j, s_n);
        branch_coef(v, j);
        if (!v->fb[j].closed && (v->is_junction[a] | v->is_junction[b])) {
            ch_real il = R(1.0) / v->fb[j].l;
            v->nd_lsum_static[a] += il;
            v->nd_lsum_static[b] += il;
        }
    }
    for (k = 0; k < n; k++)
        v->inv_lsum[k] = v->nd_lsum_static[k];

    /* 4, 5) the slow nodes' coefficients (the others are done per sample) */
    for (k = 0; k < n; k++)
        if (!coef_dyn[k])
            node_coef(v, k);
}

/* the fast-moving part of the geometry, every sample */
CH_HOT CH_NOINLINE static void geometry_dyn(ch_voice_t *v)
{
    int i;
    for (i = 0; i < v->n_dyn_nodes; i++)
        node_geo(v, v->dyn_nodes[i], R(1.0));
    for (i = 0; i < v->n_dyn_coef; i++) {
        int k = v->dyn_coef[i];
        v->inv_lsum[k] = v->nd_lsum_static[k];
    }
    for (i = 0; i < v->n_dyn_branches; i++) {
        int j = v->dyn_branches[i], a = v->b_a[j], b = v->b_b[j];
        if (v->b_kind[j] == CH_PORT)
            port_advance(v, j, R(1.0));
        branch_coef(v, j);
        if (!v->fb[j].closed && (v->is_junction[a] | v->is_junction[b])) {
            ch_real il = R(1.0) / v->fb[j].l;
            v->inv_lsum[a] += il;
            v->inv_lsum[b] += il;
        }
    }
    for (i = 0; i < v->n_dyn_coef; i++)
        node_coef(v, v->dyn_coef[i]);
}

/* ------------------------------------------------------------------------------- the sample --- */

/* the turbulence noise as series pressure sources (CHIMERA's compute_noise over the hot structs).
 * Each node's flow starts from its injection at the end of the sample before (nodes()), and each
 * branch's psrc is cleared as flows() uses it. */
CH_HOT CH_PART static void sources(ch_voice_t *v)
{
    int i;
    if (v->n_noise) {
        ch_real scale = v->noise_scale * SQRT(R(96000.0) / v->fs);
        ch_real sat = R(12000.0) * R(12000.0) - RE_CRIT * RE_CRIT;
        for (i = 0; i < v->n_noise; i++) {
            ch_noise_t *s = &v->noise[i];
            ch_real a = v->branch_area[s->src], u = v->fb[s->src].u, amp = 0, w, hp_out, val;
            if (a > v->closed_area) {
                ch_real re = (FABS(u) * R(2.0)) / (NU * SQRT(CH_PI * a));
                if (re > RE_CRIT) {
                    ch_real x = re * re - RE_CRIT * RE_CRIT;
                    amp = (s->gain * scale * x) / (R(1.0) + x / sat);
                }
            }
            w = rng_bipolar(&v->rng) * R(1.732);
            s->hp += (R(1.0) - s->hp_coef) * (w - s->hp);
            hp_out = w - s->hp;
            s->lp1 += s->lp_coef * (hp_out - s->lp1);
            s->lp2 += s->lp_coef * (s->lp1 - s->lp2);
            s->level = amp;
            if (amp == 0)
                continue;
            val = amp * s->lp2;
            if (u >= 0)
                v->fb[s->fwd].psrc += val;
            else
                v->fb[s->bwd].psrc -= val;
        }
    }
}

/* 3) branch flows */
CH_HOT CH_PART static void flows(ch_voice_t *v)
{
    ch_fbr_t *b = v->fb, *be = v->fb + v->nb;
    ch_fnd_t *nd = v->fn;
    const ch_real inv_t = v->inv_t;
    const int safe = v->energy_safe;
    for (; b < be; b++) {
        ch_fnd_t *na, *nb;
        ch_real l, ibq, kk, m0, c, q, x, u;
        if (b->closed) {
            b->psrc = 0;
            continue;
        }
        na = nd + b->a;
        nb = nd + b->b;
        l = b->l;
        ibq = b->ibq;
        m0 = (safe && l < b->l_prev) ? l * b->u : b->mom;
        c = na->p - nb->p + b->psrc + m0 * inv_t;
        q = c * ibq;                   /* the flow without kinetic loss */
        kk = c >= 0 ? b->kf : b->kb;
        if (kk > 0) {
            x = R(4.0) * kk * FABS(c) * ibq * ibq;
            if (x < X_SERIES)
                u = q * (R(1.0) - x * (R(0.25) - R(0.125) * x));
            else
                u = q * R(2.0) / (R(1.0) + SQRT(R(1.0) + x));
        } else {
            u = q;
        }
        b->u = u;
        b->mom = l * u;
        b->l_prev = l;
        b->psrc = 0;
        na->flow -= u;
        nb->flow += u;
    }
}

/* 4) walls and 5) nodes, in one pass (a wall moves with its own node's pressure before the node is
 * updated, as CHIMERA's separate wall loop has it); then the node's flow for the next sample starts
 * from its moving-wall injection. Returns the radiated flow. */
CH_HOT CH_PART static ch_real nodes(ch_voice_t *v)
{
    const ch_real cl = v->closed_area, t = v->t;
    ch_fnd_t *f = v->fn, *fe = v->fn + v->n;
    ch_real rad_total = 0, wall_rad_sum = 0;
    for (; f < fe; f++) {
        int r;
        if (f->wall) {
            ch_real vel = (f->wa * f->wv + f->p - f->wk * f->wy) * f->winv, uw;
            f->wv = vel;
            f->wy += t * vel;
            uw = f->wsd * vel;
            f->flow -= uw;
            wall_rad_sum += uw * f->wrad;
        }
        if (f->fixed)
            goto next;
        r = f->rad;
        if (f->sealed) {
            if (r >= 0) {
                f->p *= R(0.98);
                v->rad_ul[r] *= R(0.995);
                v->rad_u[r] = v->rad_ul[r];
                rad_total += v->rad_ul[r];
            } else {
                int k = (int)(f - v->fn), cnt = 0, e;
                ch_real sum = 0;
                for (e = v->nbr_start[k]; e < v->nbr_start[k + 1]; e++) {
                    int m = v->nbr_list[e];
                    if (v->area[m] >= cl && v->amin[m] >= cl) {
                        sum += v->fn[m].p;
                        cnt++;
                    }
                }
                if (cnt > 0)
                    f->p += R(0.02) * (sum / (ch_real)cnt - f->p);
            }
            goto next;
        }
        if (r < 0) {
            f->p += f->kn * f->flow;
        } else {
            ch_real p_old = f->p, ul_prev = v->rad_ul[r], h = v->rad_h[r], g = v->rad_g[r], p_new, ul, ur;
            p_new = (p_old * v->rad_num[r] + f->flow - ul_prev) * v->rad_iden[r];
            f->p = p_new;
            ul = ul_prev + h * (p_old + p_new);
            v->rad_ul[r] = ul;
            ur = R(0.5) * (ul_prev + ul) + g * (p_new + p_old);
            v->rad_u[r] = ur;
            rad_total += ur;
        }
    next:
        f->flow = f->inj;
        f->inj += f->dinj;
    }
    return wall_rad_sum + rad_total;
}

CH_HOT static ch_real step(ch_voice_t *v)
{
    ch_real rad_total, out;
    if (v->geo_left == 0) {
        geometry(v);
        v->geo_left = v->geo_every;
    }
    v->geo_left--;
    if (v->n_dyn_nodes | v->n_dyn_branches)
        geometry_dyn(v);
    branch_coef(v, v->glottis_port);   /* the glottis port: its area is new every sample */
    sources(v);
    flows(v);
    rad_total = nodes(v);
    out = (rad_total - v->rad_prev_total) * v->inv_t;
    v->rad_prev_total = rad_total;
    return out;
}

CH_HOT void ch_fast_render(ch_voice_t *v, float *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        ch_real a, y, x, hp, av, al;
        v->p_lung_now += v->p_lung_step;
        v->fn[v->lungs].p = v->p_lung_now;
        a = glottis_next(&v->glottis, v->fn[v->sub_node].p - v->fn[v->supra].p);
        v->port_area[v->glottis_port] = a < 0 ? 0 : a;
        y = step(v);
        x = y * v->output_gain;
        hp = x - v->dc_x + v->dc_coef * v->dc_y;
        v->dc_x = x;
        v->dc_y = hp;
        x = hp;
        av = FABS(x);
        if (av >= v->lim_env) {
            v->lim_env = av;
            v->lim_hold = v->lim_hold_n;
        } else if (v->lim_hold > 0) {
            v->lim_hold -= 1;
        } else {
            v->lim_env *= v->lim_release;
        }
        if (v->lim_env > R(0.75))
            x *= R(0.75) / v->lim_env;
        al = FABS(x);
        if (al > R(0.85))
            x = (x < 0 ? R(-1.0) : R(1.0)) * (R(0.85) + R(0.15) * TANH((al - R(0.85)) / R(0.15)));
        out[i] = (float)x;
    }
}
