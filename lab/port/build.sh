#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
# Build the replay tool in its variants (host), into ../out/:
#   replay64   double (-DCH_DOUBLE), the literal port's check against CHIMERA
#   replay32   float, with libm
#   replay32f  float with the port's own fast sqrt and cos
#   replay_fm1 float with the FM-1 build's maths (fm1-base's fastmath.h, -DCH_FM1), as on the device
set -e
cd "$(dirname "$0")"
E=../../engine
SRC="replay.c $E/ch_voice.c $E/ch_fast.c $E/ch_tract.c $E/ch_wave.c $E/ch_control.c"
W="-std=c99 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -I$E"
cc -O2 $W -DCH_DOUBLE -o ../out/replay64 $SRC -lm
cc -O2 $W -o ../out/replay32 $SRC -lm
cc -O2 $W -DCH_FASTMATH -o ../out/replay32f $SRC -lm
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/replay_fm1 $SRC -lm
# the front end against CHIMERA's (lab fronttest first)
cc -O1 $W -DCH_DOUBLE -o ../out/front_test front_test.c $E/ch_speech.c $E/ch_control.c $E/ch_tract.c -lm
cc -O1 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/front_test_fm1 front_test.c $E/ch_speech.c $E/ch_control.c $E/ch_tract.c -lm
# the Studio against the firmware: node port/studio_test.mjs ../app/web/studio.html <cmudict.txt.gz> ../engine/ch_dict_data.h out,
# then out/studio_check out/doc.txt must print out/studio_syl.txt
cc -O1 $W -DCH_DOUBLE -o ../out/studio_check studio_check.c $E/ch_speech.c $E/ch_control.c $E/ch_tract.c -lm
# the beatbox and the seal against CHIMERA's renders (`chimera render beatbox OUT [preset] [body]`, `render creature seal
# OUT`): beat_test OUTDIR [body] [preset] (each sound, then a preset beat), seal_test OUT.wav (a note, then its calls).
# beat_test's lab switches: BEAT_DEBUG=N (sound N's pockets step by step), PAT_DEBUG=T (the beat's from T s),
# POCK_K / POCK_MAX / WALL_RAD / NOWALLS / NOVOL (the waveguide's pocket, wall and volume source settings)
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -I../../app/dsp -o ../out/beat_test beat_test.c $E/ch_beat.c $E/ch_song.c $E/ch_speech.c \
    $E/ch_control.c $E/ch_tract.c $E/ch_wave.c $E/ch_voice.c $E/ch_fast.c -lm
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/seal_test seal_test.c $E/ch_creature.c $E/ch_wave.c $E/ch_voice.c \
    $E/ch_fast.c $E/ch_tract.c -lm
# the songbird (bird_test OUT.wav: a note, then its calls) against `chimera render creature songbird OUT`; its syrinx
# tables: ../tools/gen_syrinx_waves.py (CHIMERA's syrinxTable.generated.json) > ../../engine/ch_syrinx_waves.h
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/bird_test bird_test.c $E/ch_creature.c $E/ch_wave.c $E/ch_voice.c \
    $E/ch_fast.c $E/ch_tract.c -lm
# the alligator and the elephant (cre_test alligator|elephant OUT.wav: a note, then its calls, at their fraction of the
# rate and interpolated up) against `chimera render creature alligator|elephant OUT`
cc -O2 $W -DCH_FM1 -DPLAT_HOST -I../../base/include -o ../out/cre_test cre_test.c $E/ch_creature.c $E/ch_wave.c $E/ch_voice.c \
    $E/ch_fast.c $E/ch_tract.c -lm
# the anatomy's stages timed over a moving articulation (-DCH_COUNT: its segment tests counted too)
cc -O2 $W -DCH_FM1 -DPLAT_HOST -DCH_COUNT -DCH_PROF=prof_mark -I../../base/include -I../../app/dsp -include tract_prof.h \
    -o ../out/tract_prof tract_prof.c $E/ch_tract.c $E/ch_control.c $E/ch_voice.c -lm
