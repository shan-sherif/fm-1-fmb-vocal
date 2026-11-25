#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
# Make the private fields of CHIMERA's airway, glottis and voice structs public, so `lab record`
# can read the network's per-block targets. Only visibility changes: the sound is untouched.
# Idempotent. Run from anywhere: sh lab/patch-chimera.sh
set -e
root="$(cd "$(dirname "$0")/.." && pwd)/external/chimera-vocal-synth/crates/chimera/src"
for f in airway/network.rs sources/glottis.rs voice/human_voice.rs voice/human_airway.rs; do
    # only inside `struct Name {` .. `}` blocks; fields are indented 4 spaces
    awk '
        /^(pub )?struct [A-Za-z0-9_]+.*\{$/ { in_s = 1; print; next }
        in_s && /^\}/ { in_s = 0 }
        in_s && /^    [a-z_][a-z_0-9]*: / { sub(/^    /, "    pub ") }
        { print }
    ' "$root/$f" > "$root/$f.tmp" && mv "$root/$f.tmp" "$root/$f"
done
echo "patched $root"

# Lab experiments (environment variables, read when an airway or a voice is built; unset = CHIMERA as it is):
#   CHIMERA_MIN_SECTIONS  the fewest sections a pharynx, oral or nasal tube may have (CHIMERA: 8). Below about
#                         30 kHz CHIMERA's 8 are shorter than one time step allows; fewer keep it stable.
#   CHIMERA_CTRL_EVERY    samples to a control step (CHIMERA: 64 at 1x)
ha="$root/voice/human_airway.rs"
hv="$root/voice/human_voice.rs"
if ! grep -q "lab_min_sections" "$ha"; then
    perl -0pi -e 's/\(8\.0f64\)\.max\(\(ph_min_len/(lab_min_sections()).max((ph_min_len/; s/\(8\.0f64\)\.max\(\(or_min_len/(lab_min_sections()).max((or_min_len/; s/js_max\(8\.0, \(na_len/js_max(lab_min_sections(), (na_len/' "$ha"
    cat >> "$ha" <<'RS'

/// lab: the fewest sections a tube may have (CHIMERA_MIN_SECTIONS; CHIMERA's own: 8)
fn lab_min_sections() -> f64 {
    std::env::var("CHIMERA_MIN_SECTIONS").ok().and_then(|s| s.parse().ok()).unwrap_or(8.0)
}
RS
fi
if ! grep -q "CHIMERA_CTRL_EVERY" "$hv"; then
    perl -0pi -e 's/ctrl_every: 64 \* m,/ctrl_every: std::env::var("CHIMERA_CTRL_EVERY").ok().and_then(|s| s.parse().ok()).unwrap_or(64) * m,/' "$hv"
fi
echo "lab experiments patched in"
