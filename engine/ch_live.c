/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: Copyright (c) 2026 Shan-Sherif.
 *
 * A port of CHIMERA's live Sing mode (src/ui/singing.ts); see ch_live.h. The piano roll, the demo tunes and the
 * mouth's caption are the browser's and are left out. */
#include "ch_live.h"
#include "ch_common.h"
#include "ch_str.h"

static void syl_ah(ch_syl_t *s)   /* sung when there are no lyrics */
{
    memset(s, 0, sizeof *s);
    strcpy(s->text, "ah");
    strcpy(s->nucleus.ipa, "ɑ");
    s->nucleus.dur = (ch_real)__builtin_nanf("");
    s->nucleus.stress = 1;
    s->word_end = 1;
}

void ch_live_init(ch_live_t *l)
{
    memset(l, 0, sizeof *l);
    l->vibrato = R(35.0);
    l->next_tag = R(2.0);
}

int ch_live_lyrics(ch_live_t *l, const char *text, ch_lookup_fn lookup, void *user)
{
    l->n_syl = ch_lyrics(text, lookup, user, l->syl, CH_SYL_MAX);
    l->cursor = 0;
    return l->n_syl;
}

const ch_syl_t *ch_live_next(const ch_live_t *l) { return l->n_syl ? &l->syl[l->cursor % l->n_syl] : 0; }
const ch_syl_t *ch_live_sounding(const ch_live_t *l) { return l->sounding ? &l->s_syl : 0; }

static void cmd_release(ch_live_cmd_t *c, ch_real tag)
{
    if (c->n_release < CH_LIVE_KEYS)
        c->release[c->n_release++] = tag;
}

static ch_plan_t plan_into(ch_live_cmd_t *c)
{
    ch_plan_t p;
    p.g = c->g + c->n;
    p.cap = CH_LIVE_G - c->n;
    p.n = 0;
    p.dropped = 0;
    p.end = 0;
    p.last_t0 = p.last_t1 = 0;
    p.exhale = -1;
    return p;
}

/* breath level for a key velocity (1 at the on-screen keys' default of 96) */
static ch_real breath_for(ch_real v)
{
    return R(0.62) + R(0.4) * mn(R(1.0), mx(0, v / R(96.0))) + R(0.12) * mx(0, (v - R(96.0)) / R(31.0));
}

static void key_drop(ch_live_t *l, ch_real midi)
{
    int i, j = 0;
    for (i = 0; i < l->n_keys; i++)
        if (l->keys[i] != midi)
            l->keys[j++] = l->keys[i];
    l->n_keys = j;
}

static void start_note(ch_live_t *l, const ch_syl_t *syl, ch_real midi, ch_real velocity, ch_live_cmd_t *cmd)
{
    const ch_real tag = l->next_tag;
    const ch_real breath = breath_for(velocity);
    ch_plan_t p;
    int i;
    l->next_tag += R(2.0);
    /* earlier codas may finish, but their breath-out must not open the folds on this onset */
    if (l->exhales_live)
        for (i = 0; i < l->n_tails; i++)
            cmd_release(cmd, l->tails[i] + R(1.0));
    l->exhales_live = 0;
    if (l->sounding) {
        cmd_release(cmd, l->s_tag);
        cmd_release(cmd, l->s_tag + R(1.0));
    }
    p = plan_into(cmd);
    ch_plan_note_on(syl, midi, l->vibrato, tag, l->sounding ? &l->s_syl : 0, l->s_midi, &p);
    for (i = 0; i < p.n; i++)
        if (!(p.g[i].targets[CH_BREATH] != p.g[i].targets[CH_BREATH]))   /* (a breath target: not NaN) */
            p.g[i].targets[CH_BREATH] *= breath;
    cmd->n += p.n;
    cmd->dropped += p.dropped;
    l->sounding = 1;
    l->s_midi = midi;
    l->s_syl = *syl;
    l->s_tag = tag;
}

/* the held vowel moves to a new pitch (melisma, or back to a key still down) */
static void glide(ch_live_t *l, ch_real midi, ch_live_cmd_t *cmd)
{
    ch_plan_t p;
    cmd_release(cmd, l->s_tag + R(1.0));
    p = plan_into(cmd);
    ch_plan_melisma(midi, l->vibrato, l->s_tag, &p);
    cmd->n += p.n;
    l->s_midi = midi;
}

void ch_live_note_on(ch_live_t *l, ch_real midi, ch_real velocity, ch_live_cmd_t *cmd)
{
    ch_syl_t ah, s;
    const ch_syl_t *syl;
    int i;
    key_drop(l, midi);
    if (l->n_keys < CH_LIVE_KEYS)
        l->keys[l->n_keys++] = midi;
    if (!l->n_syl) {
        syl_ah(&ah);
        syl = &ah;
        i = 0;
    } else {
        i = l->cursor % l->n_syl;
        syl = &l->syl[i];
    }
    if (syl->melisma && l->sounding) {
        glide(l, midi, cmd);
    } else if (syl->melisma) {   /* a melisma with nothing held: sing its vowel again, without the onset */
        int k = i;
        while (k > 0 && l->syl[k].melisma)
            k--;
        s = l->syl[k];
        s.n_onset = 0;
        start_note(l, &s, midi, velocity, cmd);
    } else {
        start_note(l, syl, midi, velocity, cmd);
    }
    if (l->n_syl)
        l->cursor = (i + 1) % l->n_syl;
}

void ch_live_note_off(ch_live_t *l, ch_real midi, ch_live_cmd_t *cmd)
{
    ch_plan_t p;
    ch_real tag;
    int i;
    key_drop(l, midi);
    if (!l->sounding || l->s_midi != midi)
        return;
    if (l->n_keys) {
        glide(l, l->keys[l->n_keys - 1], cmd);
        return;
    }
    cmd_release(cmd, l->s_tag);
    cmd_release(cmd, l->s_tag + R(1.0));
    l->sounding = 0;
    /* a melisma comes next: this vowel carries on there, and the coda waits for the chain's end */
    if (l->n_syl && l->syl[l->cursor % l->n_syl].melisma)
        return;
    p = plan_into(cmd);
    ch_plan_note_off(&l->s_syl, midi, &p);
    if (!p.n)
        return;
    tag = l->next_tag;
    l->next_tag += R(2.0);
    for (i = 0; i < p.n; i++)
        p.g[i].tag = i == p.exhale ? tag + R(1.0) : tag;
    cmd->n += p.n;
    cmd->dropped += p.dropped;
    if (l->n_tails == 8) {   /* long finished by now */
        memmove(l->tails, l->tails + 1, sizeof l->tails[0] * 7);
        l->n_tails--;
    }
    l->tails[l->n_tails++] = tag;
    l->exhales_live = 1;
}

void ch_live_apply(const ch_live_cmd_t *cmd, ch_control_t *c)
{
    int i;
    for (i = 0; i < cmd->n_release; i++)
        ch_score_release(&c->score, cmd->release[i], c->clock);
    for (i = 0; i < cmd->n; i++) {
        ch_gesture_t g = cmd->g[i];
        g.t0 += c->clock;
        g.t1 += c->clock;
        ch_score_add(&c->score, &g);
    }
}
