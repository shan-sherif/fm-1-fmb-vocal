// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shan-Sherif
//! Test bench for porting CHIMERA (https://github.com/jamesondh/chimera-vocal-synth, MIT,
//! (c) 2026 Jameson Hodge) to the FM-1.
//!
//! `cargo run --release -- rates ["Text."]`: speaks the text at several output rates and
//! oversampling factors, writes each to out/*.wav, and prints the cost and how far each one's
//! long-term spectrum is from the reference (48 kHz, 2x oversampled, as CHIMERA ships).

use chimera::anatomy::human::{BODY_CHILD, BODY_FEMALE, BODY_MALE};
use chimera::phonetics::english::EnglishG2P;
use chimera::phonetics::planner::{PhraseInput, PlanOptions, plan_phrases};
use chimera::voice::human_voice::{HumanVoice, SourcePatch};
mod record;

use std::io::Read;
use std::time::Instant;

const DICT: &str = "../external/chimera-vocal-synth/public/data/cmudict.txt.gz";

fn g2p() -> EnglishG2P {
    let gz = std::fs::read(DICT).expect("cmudict");
    let mut text = String::new();
    flate2::read::GzDecoder::new(&gz[..]).read_to_string(&mut text).unwrap();
    let mut g = EnglishG2P::new();
    g.load_dict(&text);
    g
}

/// A voice with `text` scheduled, CHIMERA's `render text` path, and how many samples it lasts.
fn speaking(g: &EnglishG2P, text: &str, body: &str, fs: f64, os: usize) -> (HumanVoice, usize) {
    let (b, f0) = match body { "female" => (BODY_FEMALE, 205.0), "child" => (BODY_CHILD, 250.0), _ => (BODY_MALE, 112.0) };
    let inputs: Vec<PhraseInput> = g
        .to_phrases(text)
        .iter()
        .map(|p| PhraseInput { segments: p.segments.clone(), intonation: Some(p.intonation), pause: Some(p.pause) })
        .collect();
    let plan = plan_phrases(&inputs, &PlanOptions { body: body.into(), t0: 0.05, f0: Some(f0), ..Default::default() });
    let mut v = HumanVoice::new(fs, b, os);
    v.set_source(&SourcePatch::f0(f0));
    v.schedule(&plan.gestures);
    (v, ((plan.end + 0.3) * fs) as usize)
}

/// Speak `text`: the samples and the time it took as a fraction of real time.
fn say(g: &EnglishG2P, text: &str, body: &str, fs: f64, os: usize) -> (Vec<f32>, f64) {
    let (mut v, n) = speaking(g, text, body, fs, os);
    let mut out = vec![0f32; n];
    let t = Instant::now();
    for b in out.chunks_mut(32) {
        v.render(b);
    }
    let secs = t.elapsed().as_secs_f64();
    let load = secs / (out.len() as f64 / fs);
    (out, load)
}

fn write_wav(path: &str, x: &[f32], fs: u32) {
    let mut b = Vec::with_capacity(44 + x.len() * 2);
    let n = x.len() as u32 * 2;
    b.extend_from_slice(b"RIFF");
    b.extend_from_slice(&(36 + n).to_le_bytes());
    b.extend_from_slice(b"WAVEfmt ");
    for v in [16u32] {
        b.extend_from_slice(&v.to_le_bytes());
    }
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&fs.to_le_bytes());
    b.extend_from_slice(&(fs * 2).to_le_bytes());
    b.extend_from_slice(&2u16.to_le_bytes());
    b.extend_from_slice(&16u16.to_le_bytes());
    b.extend_from_slice(b"data");
    b.extend_from_slice(&n.to_le_bytes());
    for &s in x {
        let v = if s.is_finite() { s.clamp(-1.0, 1.0) } else { 0.0 };
        b.extend_from_slice(&((v * 32767.0).round() as i16).to_le_bytes());
    }
    std::fs::create_dir_all("out").unwrap();
    std::fs::write(path, b).unwrap();
}

/// Long-term average spectrum in third-octave bands from 100 Hz to 10 kHz (dB), by the DFT
/// at each band's frequencies over 2048-sample Hann frames (rate-independent, so renders at
/// different rates compare directly).
fn ltas(x: &[f32], fs: f64) -> Vec<f64> {
    let bands: Vec<f64> = (0..21).map(|i| 100.0 * 2f64.powf(i as f64 / 3.0)).collect();
    let n = 2048;
    let mut acc = vec![0.0; bands.len()];
    let mut frames = 0;
    let mut i = 0;
    while i + n <= x.len() {
        let fr = &x[i..i + n];
        for (bi, &fc) in bands.iter().enumerate() {
            // a few bins across the band
            let mut e = 0.0;
            for s in 0..5 {
                let f = fc * 2f64.powf((s as f64 - 2.0) / 18.0);
                let w = 2.0 * std::f64::consts::PI * f / fs;
                let (mut re, mut im) = (0.0, 0.0);
                for (k, &v) in fr.iter().enumerate() {
                    let h = 0.5 - 0.5 * (2.0 * std::f64::consts::PI * k as f64 / n as f64).cos();
                    let a = w * k as f64;
                    re += v as f64 * h * a.cos();
                    im -= v as f64 * h * a.sin();
                }
                e += re * re + im * im;
            }
            acc[bi] += e;
        }
        frames += 1;
        i += n / 2;
    }
    acc.iter().map(|e| 10.0 * (e / frames as f64 + 1e-20).log10()).collect()
}

/// Third-octave band energies (dB) of each 1024-sample frame (hop 512): DFT at five points a band.
fn frames(x: &[f32], fs: f64) -> Vec<(f64, Vec<f64>)> {
    let bands: Vec<f64> = (0..21).map(|i| 100.0 * 2f64.powf(i as f64 / 3.0)).collect();
    let n = 1024;
    let tw = std::f64::consts::TAU;
    // the DFT's twiddles once: frequency × sample
    let tab: Vec<Vec<(f64, f64)>> = bands
        .iter()
        .flat_map(|&fc| (0..5).map(move |s| fc * 2f64.powf((s as f64 - 2.0) / 18.0)))
        .map(|f| {
            (0..n)
                .map(|k| {
                    let h = 0.5 - 0.5 * (tw * k as f64 / n as f64).cos();
                    let a = tw * f / fs * k as f64;
                    (h * a.cos(), -h * a.sin())
                })
                .collect()
        })
        .collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i + n <= x.len() {
        let fr = &x[i..i + n];
        let rms = (fr.iter().map(|v| (*v as f64) * (*v as f64)).sum::<f64>() / n as f64).sqrt();
        let mut b = vec![0.0; bands.len()];
        for (q, t) in tab.iter().enumerate() {
            let (mut re, mut im) = (0.0, 0.0);
            for (k, &v) in fr.iter().enumerate() {
                re += v as f64 * t[k].0;
                im += v as f64 * t[k].1;
            }
            b[q / 5] += re * re + im * im;
        }
        out.push((20.0 * (rms + 1e-12).log10(), b.iter().map(|e| 10.0 * (e + 1e-20).log10()).collect()));
        i += n / 2;
    }
    out
}

fn read_wav(path: &str) -> Vec<f32> {
    let b = std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"));
    b[44..].chunks(2).map(|c| i16::from_le_bytes([c[0], c[1]]) as f32 / 32767.0).collect()
}

/// How different `b` sounds from `a`: frame by frame, the level difference and the spectral
/// distance (mean |band dB difference| with each frame's level matched), over the frames where `a`
/// is above -50 dBFS. Phase-blind: a waveform shifted in time compares as equal.
fn compare(a: &[f32], b: &[f32], fs: f64) {
    let (fa, fb) = (frames(a, fs), frames(b, fs));
    let mut lev = Vec::new();
    let mut spec = Vec::new();
    let mut worst = (0.0, 0usize);
    for (i, ((la, ba), (lb, bb))) in fa.iter().zip(&fb).enumerate() {
        if *la < -50.0 {
            continue;
        }
        lev.push(lb - la);
        // bands within 50 dB of the frame's loudest band (ignore the noise floor)
        let top = ba.iter().cloned().fold(f64::MIN, f64::max);
        let d: Vec<f64> = ba.iter().zip(bb).filter(|(x, _)| **x > top - 50.0).map(|(x, y)| y - x - (lb - la)).collect();
        let sd = d.iter().map(|v| v.abs()).sum::<f64>() / d.len() as f64;
        if sd > worst.0 {
            worst = (sd, i);
        }
        spec.push(sd);
    }
    let mean = |v: &[f64]| v.iter().sum::<f64>() / v.len() as f64;
    let mut s2 = spec.clone();
    s2.sort_by(|x, y| x.partial_cmp(y).unwrap());
    let peak = |x: &[f32]| x.iter().fold(0f32, |m, v| m.max(v.abs()));
    println!(
        "  level {:+.2} dB (mean |diff| {:.2} dB)   spectrum: mean {:.2} dB, 90% {:.2} dB, worst {:.1} dB at {:.2} s   peak {:.2} vs {:.2}",
        mean(&lev),
        lev.iter().map(|v| v.abs()).sum::<f64>() / lev.len() as f64,
        mean(&spec),
        s2[s2.len() * 9 / 10],
        worst.0,
        worst.1 as f64 * 512.0 / fs,
        peak(b),
        peak(a)
    );
}

/// 2x up: zeros between the samples, then a Kaiser-windowed sinc low-pass at the old Nyquist (63 taps)
fn upsample2(x: &[f32]) -> Vec<f32> {
    let taps = 63usize;
    let mid = (taps - 1) as f64 / 2.0;
    let beta: f64 = 8.0;
    let i0 = |v: f64| {
        let (mut s, mut t) = (1.0, 1.0);
        for k in 1..30 {
            let q = v / (2.0 * k as f64);
            t *= q * q;
            s += t;
        }
        s
    };
    let h: Vec<f64> = (0..taps)
        .map(|i| {
            let n = i as f64 - mid;
            let sinc = if n == 0.0 { 1.0 } else { (std::f64::consts::PI * n / 2.0).sin() / (std::f64::consts::PI * n / 2.0) };
            let r = 2.0 * i as f64 / (taps - 1) as f64 - 1.0;
            sinc * i0(beta * (1.0 - r * r).max(0.0).sqrt()) / i0(beta)
        })
        .collect();
    let mut z = vec![0f64; x.len() * 2];
    for (i, &v) in x.iter().enumerate() {
        z[2 * i] = v as f64;
    }
    (0..z.len())
        .map(|n| {
            let mut acc = 0.0;
            for (k, hk) in h.iter().enumerate() {
                let j = n as isize + k as isize - mid as isize;
                if j >= 0 && (j as usize) < z.len() {
                    acc += hk * z[j as usize];
                }
            }
            acc as f32
        })
        .collect()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(String::as_str).unwrap_or("rates");
    match cmd {
        "rates" => {
            let text = args.get(2).map(String::as_str).unwrap_or("She sells sea shells. Did you get the big red ball?");
            let body = args.get(3).map(String::as_str).unwrap_or("male");
            let g = g2p();
            let configs = [(48000.0, 2usize), (44100.0, 2), (44100.0, 1), (32000.0, 2)];
            let mut reference: Option<Vec<f64>> = None;
            println!("{:>8} {:>3} {:>8} {:>8} {:>10}", "fs", "os", "sim kHz", "load", "LTAS dB");
            for (fs, os) in configs {
                let (out, load) = say(&g, text, body, fs, os);
                let name = format!("out/say-{body}-{}k-os{}.wav", fs as u32 / 1000, os);
                write_wav(&name, &out, fs as u32);
                let l = ltas(&out, fs);
                // mean absolute band difference after matching overall level
                let d = match &reference {
                    None => {
                        reference = Some(l.clone());
                        0.0
                    }
                    Some(r) => {
                        let off: f64 = r.iter().zip(&l).map(|(a, b)| a - b).sum::<f64>() / r.len() as f64;
                        r.iter().zip(&l).map(|(a, b)| (a - b - off).abs()).sum::<f64>() / r.len() as f64
                    }
                };
                let finite = out.iter().all(|v| v.is_finite());
                let peak = out.iter().fold(0f32, |m, v| m.max(v.abs()));
                let limited = out.iter().filter(|v| v.abs() > 0.85).count() as f64 / out.len() as f64;
                println!("        peak {peak:.2}, samples in the soft clip {:.2}%", 100.0 * limited);
                println!(
                    "{:>8} {:>3} {:>8.1} {:>7.1}% {:>10.2}{}   {name}",
                    fs, os, fs * os as f64 / 1000.0, 100.0 * load, d, if finite { "" } else { "  NaN!" }
                );
                if let Some(r) = &reference {
                    let row: Vec<String> = r.iter().zip(&l).map(|(a, b)| format!("{:+.0}", b - a)).collect();
                    println!("        bands vs ref (100 Hz..10 kHz): {}", row.join(" "));
                }
            }
        }
        "record" => {
            // lab record ["Text."] [body] [out/name.rec]: CHIMERA at 44.1 kHz, no oversampling
            let text = args.get(2).map(String::as_str).unwrap_or("She sells sea shells. Did you get the big red ball?");
            let body = args.get(3).map(String::as_str).unwrap_or("male");
            let default = format!("out/{body}.rec");
            let path = args.get(4).map(String::as_str).unwrap_or(&default);
            let (v, n) = speaking(&g2p(), text, body, 44100.0, 1);
            let out = record::record(v, n, path);
            write_wav(&path.replace(".rec", "-chimera.wav"), &out, 44100);
            println!("wrote {path}: {} samples", out.len());
        }
        "psupra" => {
            // lab psupra from_s to_s ["Text."] [body]: CHIMERA's pressure just above the glottis and its voicing, every 3 ms
            let from: f64 = args[2].parse().unwrap();
            let to: f64 = args[3].parse().unwrap();
            let text = args.get(4).map(String::as_str).unwrap_or("She sells sea shells. Did you get the big red ball?");
            let body = args.get(5).map(String::as_str).unwrap_or("male");
            let (mut v, n) = speaking(&g2p(), text, body, 44100.0, 1);
            let supra = v.airway.pharynx.first_node();
            let mut one = [0f32; 1];
            for i in 0..n {
                v.render(&mut one);
                let t = i as f64 / 44100.0;
                if t >= from && t <= to && i % 132 == 0 {
                    print!("{t:.3}:{:.0}/{:.2}  ", v.airway.net.p[supra], v.glottis.voicing);
                }
            }
            println!();
        }
        "phonemes" => {
            // lab phonemes > ../engine/ch_phon_data.h: CHIMERA's phoneme inventory and calibrated vowels for its three
            // bodies (adult male, adult female, child), as CHIMERA's own phoneme() / vowel_artic() return them, as C
            // tables (engine/ch_speech.c picks the body's: ch_speech_body)
            use chimera::anatomy::human::{ArticKey, ArticPatch, BodyMix};
            use chimera::phonetics::inventory::{Glottal, Manner, Voicing, consonants, phoneme};
            use chimera::phonetics::planner::{vowel_artic, vowel_table};
            let num = |x: Option<f64>| x.map_or("NA".to_string(), |v| format!("{v:?}"));
            let row = |p: &ArticPatch| ArticKey::ALL.iter().map(|k| num(p.get(*k))).collect::<Vec<_>>().join(", ");
            let bodies = [("MALE", "male", BodyMix::MALE), ("FEMALE", "female", BodyMix::FEMALE), ("CHILD", "child", BodyMix::CHILD)];
            println!("/* SPDX-License-Identifier: MIT");
            println!(" * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)");
            println!(" * Generated by `lab phonemes` from CHIMERA's inventory.rs, consonantTargets.generated.json and");
            println!(" * vowelTargets.generated.json (the adult male, the adult female and the child, each calibrated; template");
            println!(" * coordinates): do not edit. */");
            println!("#ifndef CH_PHON_DATA_H\n#define CH_PHON_DATA_H\n#define NA __builtin_nanf(\"\")");
            let n_vow = vowel_table()["male"].len();
            for (name, key, body) in bodies.iter() {
                assert_eq!(vowel_table()[*key].len(), n_vow, "the bodies' vowel tables differ in size");
                println!("/* {key}: ipa, manner, voiced, nasal, glottal, place, dur (s), release, dominant (ArticKey bits), targets (NA: none) */");
                println!("static const ch_phon_t CH_PHONEMES_{name}[] = {{");
                for b in consonants() {
                    let d = phoneme(b.ipa, *body).unwrap();
                    let manner = match d.manner { Manner::Vowel => "CH_M_VOWEL", Manner::Stop => "CH_M_STOP", Manner::Nasal => "CH_M_NASAL", Manner::Fricative => "CH_M_FRICATIVE", Manner::Affricate => "CH_M_AFFRICATE", Manner::Approximant => "CH_M_APPROXIMANT", Manner::Lateral => "CH_M_LATERAL", Manner::Tap => "CH_M_TAP", Manner::Trill => "CH_M_TRILL", Manner::Glottal => "CH_M_GLOTTAL" };
                    let glottal = match d.glottal { None => "0", Some(Glottal::Open) => "CH_G_OPEN", Some(Glottal::Stop) => "CH_G_STOP", Some(Glottal::Breathy) => "CH_G_BREATHY" };
                    let dom: u32 = d.dominant.iter().map(|k| 1u32 << k.index()).sum();
                    println!("    {{\"{}\", {manner}, {}, {}, {glottal}, \"{}\", {:?}, {}, 0x{dom:05x}u, {{{}}}}},", d.ipa, (d.voicing == Voicing::Voiced) as u8, d.nasal.unwrap_or(false) as u8, d.place, d.dur,
                        d.release.map_or("0".to_string(), |r| format!("\"{r}\"")), row(&d.targets));
                }
                println!("}};");
                println!("/* {key}: the calibrated vowels: ipa, targets (the planner's vowel keys, and the tip's when the vowel is rhotic), F1, F2 (Hz) */");
                println!("static const ch_vowel_t CH_VOWELS_{name}[] = {{");
                for (ipa, t) in vowel_table()[*key].iter() {
                    let v = vowel_artic(ipa, *body).unwrap();
                    let f = |i: usize| t.formants.get(i).copied().unwrap_or(0.0);
                    println!("    {{\"{ipa}\", {{{}}}, {:?}, {:?}}},", row(&v), f(0), f(1));
                }
                println!("}};");
            }
            println!("static const ch_phon_t *const CH_PHONEMES_BODY[3] = {{CH_PHONEMES_MALE, CH_PHONEMES_FEMALE, CH_PHONEMES_CHILD}};");
            println!("static const ch_vowel_t *const CH_VOWELS_BODY[3] = {{CH_VOWELS_MALE, CH_VOWELS_FEMALE, CH_VOWELS_CHILD}};");
            println!("#undef NA\n#endif");
        }
        "fronttest" => {
            // lab fronttest: CHIMERA's front end on the test inputs, written to out/ft.txt for lab/port/front_test.c
            use chimera::anatomy::human::BodyMix;
            use chimera::control::score::Gesture;
            use chimera::phonetics::english::letter_to_sound;
            use chimera::phonetics::sing::{LegatoFrom, NoteOnOptions, plan_note_off, plan_note_on};
            use chimera::phonetics::syllables::lyrics_to_syllables;
            use chimera::phonetics::{Intonation, Segment};
            use std::fmt::Write as _;
            let g = g2p();
            let mut o = String::new();
            let gl = |o: &mut String, gs: &[Gesture]| {
                for x in gs {
                    let _ = write!(o, "G {:?} {:?} {:?} {:?} {:?} {:?} {:?} {:?}", x.t0, x.t1, x.ramp_in, x.ramp_out, x.strength, x.speed.unwrap_or(f64::NAN), x.release_hold.unwrap_or(f64::NAN), x.tag.unwrap_or(f64::NAN));
                    for v in x.targets.0.iter() {
                        let _ = write!(o, " {v:?}");
                    }
                    o.push('\n');
                }
            };
            let seg = |s: &Segment| format!("{}:{}:{}", s.ipa, s.stress.map_or(-1, |x| x as i32), (s.word_end == Some(true)) as u8);
            // the letter-to-sound rules on every plain word of the dictionary
            let gz = std::fs::read(DICT).expect("cmudict");
            let mut text = String::new();
            flate2::read::GzDecoder::new(&gz[..]).read_to_string(&mut text).unwrap();
            let mut f = String::new();
            for line in text.lines() {
                let Some((w, _)) = line.split_once('\t') else { continue };
                if w.bytes().all(|b| b.is_ascii_lowercase()) {
                    let _ = writeln!(f, "{w}\t{}", letter_to_sound(w).iter().map(|s| format!("{}{}", s.ipa, s.stress.unwrap_or(0))).collect::<Vec<_>>().join(" "));
                }
            }
            std::fs::write("out/ft-lts.txt", f).unwrap();
            let texts = ["She sells sea shells. Did you get the big red ball?", "Hello, world! How are you today?", "I have 21 apples and 3 pears.",
                "The quick brown fox jumps over the lazy dog.", "Water, butter, little bottles; she'll call tomorrow!", "Strong strings sprang through the scratched windows.",
                "Read the wind's lead: close the wound, live well.", "Twenty-two thousand, four hundred and five; 1234567."];
            let mut dict = std::collections::BTreeMap::new();
            for line in text.lines() {
                if let Some((w, p)) = line.split_once('\t') { dict.insert(w.to_string(), p.to_string()); }
            }
            let mut used = String::new();
            for t in texts {
                let _ = writeln!(o, "TEXT {t}");
                let phrases = g.to_phrases(t);
                for p in &phrases {
                    for w in &p.words { if let Some(e) = dict.get(w.trim_matches('\'')) { let _ = writeln!(used, "{}\t{e}", w.trim_matches('\'')); } }
                    let _ = writeln!(o, "PHRASE {} {:?} {}", match p.intonation { Intonation::Question => 1, Intonation::Flat => 2, _ => 0 }, p.pause, p.segments.iter().map(seg).collect::<Vec<_>>().join(" "));
                }
                let inputs: Vec<PhraseInput> = phrases.iter().map(|p| PhraseInput { segments: p.segments.clone(), intonation: Some(p.intonation), pause: Some(p.pause) }).collect();
                let plan = plan_phrases(&inputs, &PlanOptions { body: BodyMix::MALE, t0: 0.05, f0: Some(112.0), ..Default::default() });
                gl(&mut o, &plan.gestures);
                let _ = writeln!(o, "END {:?}", plan.end);
            }
            let lyric = "Twinkle, twinkle, little star - how I wonder what you are! Ti-ger fire ~ strengths, joy.";
            for w in lyric.split(|c: char| !c.is_ascii_alphabetic() && c != '\'') { let lw = w.to_lowercase(); if let Some(e) = dict.get(&lw) { let _ = writeln!(used, "{lw}\t{e}"); } }
            for w in ["ti", "ger"] { if let Some(e) = dict.get(w) { let _ = writeln!(used, "{w}\t{e}"); } }
            let syls = lyrics_to_syllables(lyric, &g);
            let _ = writeln!(o, "LYRIC {lyric}");
            for (i, s) in syls.iter().enumerate() {
                let _ = writeln!(o, "SYL {}|{}|{}|{}|{} {} {}", s.text, s.onset.iter().map(seg).collect::<Vec<_>>().join(","), seg(&s.nucleus), s.coda.iter().map(seg).collect::<Vec<_>>().join(","), s.melisma == Some(true), s.word_end, s.phrase_end);
                let midi = 60.0 + (i % 12) as f64;
                let legato = if i % 2 == 1 { Some(LegatoFrom { syl: syls[i - 1].clone(), midi: 60.0 + ((i - 1) % 12) as f64 }) } else { None };
                let on = plan_note_on(s, midi, &NoteOnOptions { body: BodyMix::MALE, vibrato: 30.0, tag: 2.0 * i as f64, legato_from: legato });
                let _ = writeln!(o, "NOTEON {i}");
                gl(&mut o, &on);
                let off = plan_note_off(s, midi, BodyMix::MALE);
                let _ = writeln!(o, "NOTEOFF {i}");
                gl(&mut o, &off);
            }
            std::fs::write("out/ft.txt", o).unwrap();
            std::fs::write("out/ft-dict.txt", used).unwrap();
            println!("wrote out/ft.txt, out/ft-dict.txt, out/ft-lts.txt");
        }
        "timeline" => {
            // lab timeline ref.wav other.wav [from_s] [to_s]: frame by frame (11.6 ms): ref level, other - ref level,
            // spectral distance, and the level difference in three regions (< 600 Hz, 600-2500, > 2500)
            let (a, b) = (read_wav(&args[2]), read_wav(&args[3]));
            let from: f64 = args.get(4).map(|s| s.parse().unwrap()).unwrap_or(0.0);
            let to: f64 = args.get(5).map(|s| s.parse().unwrap()).unwrap_or(1e9);
            let (fa, fb) = (frames(&a, 44100.0), frames(&b, 44100.0));
            let region = |v: &[f64], lo: usize, hi: usize| 10.0 * (v[lo..hi].iter().map(|d| 10f64.powf(d / 10.0)).sum::<f64>() + 1e-20).log10();
            println!("   t (s)  ref dB  diff  spec   low   mid  high");
            for (i, ((la, ba), (lb, bb))) in fa.iter().zip(&fb).enumerate() {
                let t = i as f64 * 512.0 / 44100.0;
                if t < from || t > to || *la < -60.0 {
                    continue;
                }
                let top = ba.iter().cloned().fold(f64::MIN, f64::max);
                let d: Vec<f64> = ba.iter().zip(bb).filter(|(x, _)| **x > top - 50.0).map(|(x, y)| y - x - (lb - la)).collect();
                let sd = d.iter().map(|v| v.abs()).sum::<f64>() / d.len() as f64;
                // bands: 100 Hz * 2^(i/3): < 600 Hz is 0..8, 600-2500 is 8..14, above is 14..21
                println!("  {t:6.3}  {la:6.1}  {:+5.1}  {sd:4.1}  {:+5.1} {:+5.1} {:+5.1}", lb - la,
                    region(bb, 0, 8) - region(ba, 0, 8), region(bb, 8, 14) - region(ba, 8, 14), region(bb, 14, 21) - region(ba, 14, 21));
            }
        }
        "compare" => {
            // lab compare ref.wav other.wav[@shift] [...]: each against the first; @N moves that file N
            // samples earlier (to take a known latency out of the comparison)
            let a = read_wav(&args[2]);
            for p in &args[3..] {
                let (path, shift) = match p.split_once('@') {
                    Some((f, n)) => (f, n.parse::<usize>().unwrap()),
                    None => (p.as_str(), 0),
                };
                println!("{p}:");
                let b = read_wav(path);
                compare(&a, &b[shift.min(b.len())..], 44100.0);
            }
        }
        "anatomy" => {
            // the control-rate geometry's cost: articulators -> area function, as HumanVoice runs it
            use chimera::anatomy::human::NEUTRAL;
            let fs = 44100.0;
            let mut air = chimera::voice::human_airway::HumanAirway::new(fs, BODY_MALE);
            let mut a = NEUTRAL;
            let steps = 20000;
            let t = Instant::now();
            for i in 0..steps {
                a.tb_x = 2.9 + 0.5 * ((i as f64) * 0.01).sin();
                a.jaw = 0.8 + 0.6 * ((i as f64) * 0.013).sin();
                a.lip_ap = 0.6 + 0.5 * ((i as f64) * 0.017).sin();
                air.apply(&a, 64.0 / fs);
                air.net.begin_block(64);
            }
            let per = t.elapsed().as_secs_f64() / steps as f64;
            let rate = fs / 64.0;
            println!("anatomy step: {:.1} us on this machine; at {:.0} steps/s that is {:.1}% of this core", per * 1e6, rate, 100.0 * per * rate);
            let f = air.frame.as_ref().unwrap();
            println!("grid lines {}", f.grid.len());
        }
        "bands" => {
            // lab bands ref.wav other.wav: the long-term spectrum's difference per third octave (100 Hz .. 10 kHz)
            let (a, b) = (read_wav(&args[2]), read_wav(&args[3]));
            let (la, lb) = (ltas(&a, 44100.0), ltas(&b, 44100.0));
            let off: f64 = la.iter().zip(&lb).take(12).map(|(x, y)| y - x).sum::<f64>() / 12.0;
            for (i, (x, y)) in la.iter().zip(&lb).enumerate() {
                println!("{:>6.0} Hz {:+6.1} dB", 100.0 * 2f64.powf(i as f64 / 3.0), y - x - off);
            }
        }
        "vowel" => {
            // lab vowel ref.wav other.wav T0 T1: the two spectra over [T0, T1] s, in 1/12 octave, 100 Hz .. 5 kHz (dB,
            // each to its own peak): where the formants are
            let (a, b) = (read_wav(&args[2]), read_wav(&args[3]));
            let (t0, t1): (f64, f64) = (args[4].parse().unwrap(), args[5].parse().unwrap());
            let seg = |x: &[f32]| x[(t0 * 44100.0) as usize..(t1 * 44100.0) as usize].to_vec();
            let (sa, sb) = (seg(&a), seg(&b));
            let spec = |x: &[f32]| -> Vec<f64> {
                (0..69)
                    .map(|i| {
                        let f = 100.0 * 2f64.powf(i as f64 / 12.0);
                        let w = std::f64::consts::TAU * f / 44100.0;
                        let n = x.len();
                        let (mut re, mut im) = (0.0, 0.0);
                        for (k, &v) in x.iter().enumerate() {
                            let h = 0.5 - 0.5 * (std::f64::consts::TAU * k as f64 / n as f64).cos();
                            re += v as f64 * h * (w * k as f64).cos();
                            im -= v as f64 * h * (w * k as f64).sin();
                        }
                        10.0 * (re * re + im * im + 1e-20).log10()
                    })
                    .collect()
            };
            let (pa, pb) = (spec(&sa), spec(&sb));
            let (ma, mb) = (pa.iter().cloned().fold(f64::MIN, f64::max), pb.iter().cloned().fold(f64::MIN, f64::max));
            for i in 0..69 {
                let f = 100.0 * 2f64.powf(i as f64 / 12.0);
                let bar = |v: f64| "#".repeat(((v + 40.0).max(0.0) / 2.0) as usize);
                println!("{f:6.0} {:6.1} {:6.1}  {:<20} {}", pa[i] - ma, pb[i] - mb, bar(pa[i] - ma), bar(pb[i] - mb));
            }
        }
        "say" => {
            // lab say FS OS out.wav ["Text."] [body]: CHIMERA at any rate (CHIMERA_MIN_SECTIONS / CHIMERA_CTRL_EVERY
            // from the environment, lab/patch-chimera.sh), upsampled 2x to 44.1 kHz when FS is 22050
            let fs: f64 = args[2].parse().unwrap();
            let os: usize = args[3].parse().unwrap();
            let path = &args[4];
            let text = args.get(5).map(String::as_str).unwrap_or("She sells sea shells. Did you get the big red ball?");
            let body = args.get(6).map(String::as_str).unwrap_or("male");
            let (v, n) = speaking(&g2p(), text, body, fs, os);
            let (nodes, branches, ctrl) = (v.airway.net.n_nodes, v.airway.net.n_branches, v.ctrl_every);
            let mut v = v;
            let mut out = vec![0f32; n];
            let t = Instant::now();
            for b in out.chunks_mut(32) {
                v.render(b);
            }
            let load = t.elapsed().as_secs_f64() / (n as f64 / fs);
            let finite = out.iter().all(|x| x.is_finite());
            let wav = if fs == 22050.0 { upsample2(&out) } else { out };
            write_wav(path, &wav, if fs == 22050.0 { 44100 } else { fs as u32 });
            println!("{path}: {fs} Hz x{os}: {nodes} nodes, {branches} branches, control every {ctrl} samples ({:.0} Hz), {:.2}% of this core{}",
                     fs * os as f64 / ctrl as f64, 100.0 * load, if finite { "" } else { ", NaN!" });
        }
        "nodes" => {
            for (name, b) in [("male", BODY_MALE), ("female", BODY_FEMALE), ("child", BODY_CHILD)] {
                for fs in [96000.0, 88200.0, 44100.0] {
                    let a = chimera::voice::human_airway::HumanAirway::new(fs, b);
                    println!("{name:>6} sim {:>5.1} kHz: {} nodes, {} branches (pharynx {}, oral {})", fs / 1000.0, a.net.n_nodes, a.net.n_branches, a.n_ph, a.n_or);
                }
            }
        }
        _ => eprintln!("usage: lab rates [\"text\"] [male|female|child] | lab nodes"),
    }
}
