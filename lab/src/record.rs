// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shan-Sherif
//! `lab record`: CHIMERA's voice, recorded at the boundary between its control rate and its audio
//! rate, so the C port of the audio rate (port/) can replay it and be compared sample by sample.
//!
//! The file is a flat sequence of little-endian f64 (counts and flags too), in this order:
//!
//!   header   MAGIC, VERSION, fs, ctrl_every, n_nodes, n_branches, n_rad
//!   network  closed_area, min_node_area, viscous_scale, viscous_omega, noise_scale, squeeze_area,
//!            vol_lp_coef, enable_volume_source, enable_junction_floor, energy_safe, symmetric_kinetic
//!   voice    output_gain, dc_coef, lim_hold_n, lim_release, glottis_port, lungs, sub_node, supra
//!   glottis  dp_coef, voice_coef, sm_coef, threshold, drift.a[0..3]
//!   nodes    per node: fixed, has_wall, wall_m, wall_r, wall_k, wall_perim, wall_rad, visc_f,
//!            rad_of, is_junction, area, amin, dx, vol, inv_a, nbr_start   (then nbr_start[n])
//!            nbr_list[nbr_start[n]]
//!   branches per branch: a, b, kind, port_area, port_len, port_kin, port_kex, port_slit,
//!            port_half, port_extra_r
//!   blocks   repeated: BLOCK,
//!              area_target[n], amin_target[n], dx_target[n], port_area_target[nb],
//!              n_noise, per noise: src, fwd, bwd, gain, lp_coef, hp_coef
//!              glottis params (17, GlottisParams order), p_lung,
//!              n_samples, then CHIMERA's output for the block (n_samples values)
//!   END
//!
//! A block is what one call of HumanVoice::control() leaves in the network and glottis; the C side
//! runs the same control-step bookkeeping (begin_block, flushes, the lung ramp) itself.

use chimera::voice::human_voice::HumanVoice;

pub const MAGIC: f64 = 4.0e9 + 1729.0;
pub const VERSION: f64 = 1.0;
pub const BLOCK: f64 = -1.0;
pub const END: f64 = -2.0;

struct W(Vec<u8>);
impl W {
    fn f(&mut self, v: f64) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn b(&mut self, v: bool) {
        self.f(if v { 1.0 } else { 0.0 })
    }
    fn all(&mut self, v: &[f64]) {
        for &x in v {
            self.f(x);
        }
    }
}

fn header(w: &mut W, v: &HumanVoice) {
    let net = &v.airway.net;
    let (n, nb) = (net.n_nodes, net.n_branches);
    w.all(&[MAGIC, VERSION, net.fs, v.ctrl_every as f64, n as f64, nb as f64, net.rad_nodes.len() as f64]);
    w.all(&[net.closed_area, net.min_node_area, net.viscous_scale, net.viscous_omega, net.noise_scale, net.squeeze_area, net.vol_lp_coef]);
    w.b(net.enable_volume_source);
    w.b(net.enable_junction_floor);
    w.b(net.energy_safe);
    w.b(net.symmetric_kinetic);
    let a = &v.airway;
    w.all(&[v.output_gain, v.dc_coef, v.lim_hold_n as f64, v.lim_release, a.glottis_port as f64, a.lungs as f64, a.trachea.last_node() as f64, a.pharynx.first_node() as f64]);
    let g = &v.glottis;
    w.all(&[g.dp_coef, g.voice_coef, g.sm_coef, g.threshold]);
    // Drift::new(fs / 32, 0.3 Hz, seed 4242): its private poles, computed the same way
    for f in [0.3, 0.3 * 3.0, 0.3 * 9.0] {
        w.f(chimera::math::exp((-chimera::math::TAU * f) / (g.fs / 32.0)));
    }
    for k in 0..n {
        w.b(net.fixed[k]);
        w.b(net.has_wall[k]);
        w.all(&[net.wall_m[k], net.wall_r[k], net.wall_k[k], net.wall_perim[k], net.wall_rad[k], net.visc_f[k], net.rad_of[k] as f64]);
        w.b(net.is_junction[k]);
        w.all(&[net.area[k], net.amin[k], net.dx[k], net.vol[k], net.inv_a[k], net.nbr_start[k] as f64]);
    }
    w.f(net.nbr_start[n] as f64);
    for &m in &net.nbr_list {
        w.f(m as f64);
    }
    for j in 0..nb {
        w.all(&[net.b_a[j] as f64, net.b_b[j] as f64, net.b_kind[j] as f64, net.port_area[j], net.port_len[j], net.port_kin[j], net.port_kex[j], net.port_slit[j]]);
        w.b(net.port_half[j]);
        w.f(net.port_extra_r[j]);
    }
}

/// What one control step left in the network and the glottis (written as the block starts).
fn snapshot(w: &mut W, v: &HumanVoice) {
    let net = &v.airway.net;
    w.f(BLOCK);
    w.all(&net.area_target);
    w.all(&net.amin_target);
    w.all(&net.dx_target);
    w.all(&net.port_area_target);
    w.f(net.noise.len() as f64);
    for s in &net.noise {
        w.all(&[s.src as f64, s.fwd as f64, s.bwd as f64, s.gain, s.lp_coef, s.hp_coef]);
    }
    let p = &v.glottis.params;
    w.all(&[
        p.f0, p.amplitude, p.abduction, p.chink, p.open_quotient, p.skew, p.closure_sharpness, p.effort, p.jitter,
        p.shimmer, p.vibrato_depth, p.vibrato_rate, p.drift, p.fry, p.diplophonia, p.closure, p.chaos,
    ]);
    w.f(v.p_lung);
}

/// The articulation side, written beside the recording as `<name>.art` (flat little-endian f64):
///
///   header   ART_MAGIC, ART_VERSION, fs,
///            body: scale, pharynx, tongue_radius, lip_width,
///            n_ph, n_or, dx_stable,
///            trachea: first node, first branch, n; pharynx: same; oral: same;
///            glottis_port, velum_port,
///            noise gains: glottis, constriction, teeth,
///            slew: max_area_rate, area_rate_gain, seal_rate, seal_lo, seal_hi, seal_ease,
///            area law: pharynx a b, oral a b, front a b, ahead a b,
///            the articulation HumanVoice::new applied (17, ArticKey order, unslewed)
///   blocks   per control step: the articulation its last commit used (17), dt
///
/// A block's targets are in the .rec (area_target, amin_target, dx_target, the velum port, the
/// noise list): the C port of the anatomy (engine/ch_tract.c) is checked against them.
pub const ART_MAGIC: f64 = 4.0e9 + 1730.0;
pub const ART_VERSION: f64 = 1.0;

fn art_header(w: &mut W, v: &HumanVoice) {
    let a = &v.airway;
    let b = &a.body;
    w.all(&[ART_MAGIC, ART_VERSION, a.net.fs, b.scale, b.pharynx, b.tongue_radius, b.lip_width]);
    w.all(&[a.n_ph as f64, a.n_or as f64, a.dx_stable]);
    for t in [a.trachea, a.pharynx, a.oral] {
        w.all(&[t.first as f64, t.branch_base as f64, t.n as f64]);
    }
    w.all(&[a.glottis_port as f64, a.velum_port as f64]);
    w.all(&[a.noise_gain.glottis, a.noise_gain.constriction, a.noise_gain.teeth]);
    w.all(&[a.max_area_rate, a.area_rate_gain, a.seal_rate, a.seal_lo, a.seal_hi, a.seal_ease]);
    let l = &a.tract.area_law;
    w.all(&[l.pharynx.0, l.pharynx.1, l.oral.0, l.oral.1, l.front.0, l.front.1, l.ahead.0, l.ahead.1]);
    w.all(&v.artic.x.to_array());
}

/// The control side's inputs, written beside the recording as `<name>.gst` (flat little-endian f64):
///
///   header   GST_MAGIC, GST_VERSION, n_gestures,
///            the voice source (f0, glide, pressure, breathiness, tension, fry, roughness, vibrato_depth, vibrato_rate,
///            drift, voiced, brightness), the body's scale, max_penetration, the voice's starting f0 (HumanVoice.f0)
///   gestures each: t0, t1, ramp_in, ramp_out, strength, speed, release_hold, tag (NaN: none), then its 22 targets
///            (ArticKey order, then abduction, closure, breath, f0, vibrato; NaN: absent)
///
/// The C control (engine/ch_control.c) runs from these and is checked against the .art (the articulators) and the
/// .rec (the glottis's parameters and the lungs' pressure at each control step).
pub const GST_MAGIC: f64 = 4.0e9 + 1731.0;
pub const GST_VERSION: f64 = 1.0;

fn gestures_file(v: &HumanVoice) -> W {
    let mut w = W(Vec::new());
    let gs = v.score.gestures();
    let s = &v.source;
    w.all(&[GST_MAGIC, GST_VERSION, gs.len() as f64]);
    w.all(&[s.f0, s.glide, s.pressure, s.breathiness, s.tension, s.fry, s.roughness, s.vibrato_depth, s.vibrato_rate, s.drift]);
    w.b(s.voiced);
    w.f(s.brightness);
    w.all(&[v.airway.body.scale, v.max_penetration, v.f0]);
    for g in gs {
        w.all(&[g.t0, g.t1, g.ramp_in, g.ramp_out, g.strength, g.speed.unwrap_or(f64::NAN), g.release_hold.unwrap_or(f64::NAN), g.tag.unwrap_or(f64::NAN)]);
        w.all(&g.targets.0);
    }
    w
}

/// Render `n` samples of `v` (no oversampling), one at a time so each control step is seen,
/// and write the recording to `path` (and the articulation and the gestures beside it). Returns CHIMERA's output.
pub fn record(mut v: HumanVoice, n: usize, path: &str) -> Vec<f32> {
    assert_eq!(v.m, 1, "record runs without oversampling: the C port has no decimator");
    let mut w = W(Vec::new());
    header(&mut w, &v);
    let mut art = W(Vec::new());
    art_header(&mut art, &v);
    let gst = gestures_file(&v);
    let mut out = Vec::with_capacity(n);
    let mut samples: Vec<f64> = Vec::new();
    let mut one = [0f32; 1];
    for _ in 0..n {
        let starts_block = v.ctrl_counter <= 0;
        v.render(&mut one);
        if starts_block {
            flush(&mut w, &mut samples);
            snapshot(&mut w, &v);
            art.all(&v.artic.x.to_array());
            art.f(v.ctrl_every as f64 / v.fs);
        }
        samples.push(one[0] as f64);
        out.push(one[0]);
    }
    flush(&mut w, &mut samples);
    w.f(END);
    std::fs::create_dir_all("out").unwrap();
    std::fs::write(path, w.0).unwrap();
    std::fs::write(path.replace(".rec", ".art"), art.0).unwrap();
    std::fs::write(path.replace(".rec", ".gst"), gst.0).unwrap();
    out
}

/// The reference output of the block just finished: its count, then the samples.
fn flush(w: &mut W, samples: &mut Vec<f64>) {
    if !samples.is_empty() {
        w.f(samples.len() as f64);
        w.all(samples);
        samples.clear();
    }
}
