#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
# The manual's screenshots from the host simulator (the firmware as it is), into app/docs/img: the boot screen,
# WORDS singing, a page of knobs, VOWEL mode, the word editor, the beatbox, the seal, the songbird, the alligator, the elephant, the tunes. Run after a change to a screen.
#   sh tools/manual_shots.sh
set -e
cd "$(dirname "$0")/.."
.venv/bin/python base/tools/build.py app --host >/dev/null
mkdir -p app/build/host/shots app/docs/img
printf 'wait 300\nshot boot.png\n' > app/build/host/shots/boot.txt
VOCAL_SPLASH=1 app/build/host/fmb-vocal_host app/build/host/shots/boot.txt app/build/host/shots >/dev/null
app/build/host/fmb-vocal_host app/tests/manual_shots.txt app/build/host/shots >/dev/null
app/build/host/fmb-vocal_host app/tests/cat.txt app/build/host/shots >/dev/null   # (the cat's)
app/build/host/fmb-vocal_host app/tests/melody.txt app/build/host/shots >/dev/null   # (MELODY's)
app/build/host/fmb-vocal_host app/tests/beastsing.txt app/build/host/shots >/dev/null   # (the Bestiary's SING)
for f in boot home-words page-source home-vowel edit edit-qwerty body-child blend tongue beat beat-edit seal bird gator elephant cat sing melody melody-edit beast-sing; do   # (the simulator's are uncompressed: Pillow packs them)
    .venv/bin/python -c "from PIL import Image; import sys; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2], optimize=True)" \
        app/build/host/shots/$f.png app/docs/img/$f.png
done
ls -la app/docs/img
