#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Shan-Sherif
"""The front page's animated previews: the device's screen from the host simulator (the firmware as it is), a frame
every FRAME_MS while it sings words, plays VOWEL and TONGUE, speaks, sings a tune and a melody, beatboxes and plays
the Bestiary's creatures, made into looping GIFs in app/site/img/ (fm1-base's site
copies app/site/img to the site's img/). Run after a change to a screen:

  .venv/bin/python tools/landing_anims.py"""
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "app" / "build" / "host" / "fmb-vocal_host"
WORK = ROOT / "app" / "build" / "host" / "anims"
OUT = ROOT / "app" / "site" / "img"
FRAME_MS = 60                                   # the home screen redraws at most every 60 ms while the voice moves


def shots(name, seconds):
    """'shot' lines every FRAME_MS for `seconds`, the frames named NAME-NNN.png"""
    return [f"wait {FRAME_MS}\nshot {name}-{i:03d}.png" for i in range(int(seconds * 1000 / FRAME_MS))]


def interleave(events, name, seconds):
    """the script's events ((ms, line)) between the shots of `seconds`: each event goes before the first shot at or
    after its time"""
    lines, t, i, ev = [], 0, 0, sorted(events)
    n = int(seconds * 1000 / FRAME_MS)
    for k in range(n):
        while i < len(ev) and ev[i][0] <= t:
            lines.append(ev[i][1])
            i += 1
        lines += [f"wait {FRAME_MS}", f"shot {name}-{k:03d}.png"]
        t += FRAME_MS
    return lines


def words():
    """WORDS: the keys sing slot 1, CHIMERA's "Hello there, I am a voice made of air.", a syllable a key, legato"""
    keys = [7, 9, 11, 12, 11, 9, 7, 4, 7, 9, 11]          # a little tune, a syllable each
    ev, t = [], 300
    for i, k in enumerate(keys):
        ev.append((t, f"key {k} down"))
        if i:
            ev.append((t + 10, f"key {keys[i - 1]} up"))
        t += 330
    ev.append((t, f"key {keys[-1]} up"))
    return interleave(ev, "words", (t + 600) / 1000)


def vowel():
    """VOWEL: a held note while Front / Back and Close / Open sweep the vowel space"""
    ev = [(0, "tap FX"), (300, "key 7 down")]
    t = 500
    for d in [("K1", 6)] * 4 + [("K2", 5)] * 4 + [("K1", -6)] * 4 + [("K2", -5)] * 4:
        ev.append((t, f"spin {d[0]} {d[1]}"))
        t += 180
    ev.append((t + 300, "key 7 up"))
    return interleave(ev, "vowel", (t + 900) / 1000)


def speak():
    """PLAY speaks slot 1"""
    return interleave([(200, "tap PLAY")], "speak", 3.6)


def beat():
    """SEQ: the beatbox playing "Boots and cats", the head making each sound, the playhead on the grid"""
    return interleave([(0, "tap SEQ"), (200, "tap PLAY")], "beat", 3.6)


def melody():
    """SEQ twice: MELODY, melody 1 singing slot 1 a syllable a note, the playhead on its 32 steps"""
    return interleave([(0, "tap SEQ\nwait 100\ntap SEQ\nwait 100"), (300, "tap PLAY")], "melody", 4.2)   # (MELODY from the first frame)


def tongue():
    """ARP: TONGUE, a held note while K1-K2 move the tongue's body and K3-K4 its tip"""
    ev = [(0, "tap ARP"), (300, "key 7 down")]
    t = 500
    for d in [("K1", 4)] * 3 + [("K2", 4)] * 3 + [("K3", 4)] * 3 + [("K4", 4)] * 3 + [("K1", -4)] * 3 + [("K2", -4)] * 3:
        ev.append((t, f"spin {d[0]} {d[1]}"))
        t += 180
    ev.append((t + 300, "key 7 up"))
    return interleave(ev, "tongue", (t + 900) / 1000)


def cat():
    """LFO: the cat (the Bestiary's first), a key held, then PLAY: its meow, the mouth opening and closing"""
    return interleave([(0, "tap LFO"), (700, "key 7 down"), (1600, "key 7 up"), (1900, "tap PLAY")], "cat", 3.2)


def seal():
    """LFO then ALGORITHM two on: the seal, a key held (its breath pulsing), then PLAY: the trill, falling"""
    return interleave([(0, "tap LFO\nwait 100\nspin ALGO 2\nwait 100"), (800, "key 7 down"), (1800, "key 7 up"), (2100, "tap PLAY")], "seal", 4.5)


def bird():
    """LFO then ALGORITHM: the songbird, a key held (its two voices), then PLAY: its flute phrase"""
    return interleave([(0, "tap LFO\nwait 100\nspin ALGO 1\nwait 100"), (900, "key 7 down"), (1800, "key 7 up"), (2000, "tap PLAY")], "bird", 3.6)


def gator():
    """LFO then ALGORITHM three on: the alligator, PLAY: its bellow rising into chaos"""
    return interleave([(0, "tap LFO\nwait 100\nspin ALGO 3\nwait 100"), (700, "tap PLAY")], "gator", 3.0)


def elephant():
    """LFO then ALGORITHM four on: the elephant, PRESET: the trumpet, PLAY: the lips buzzing into the trunk"""
    return interleave([(0, "tap LFO\nwait 100\nspin ALGO 4\nwait 100"), (600, "spin PRESET 1"), (1400, "tap PLAY")], "elephant", 3.0)


def sing():
    """ENV: slot 1 sung on the Dreamy tune, the head singing, the melody moving"""
    return interleave([(0, "tap ENV"), (300, "tap PLAY")], "sing", 4.2)


def gif(name, out):
    frames = sorted(WORK.glob(f"{name}-*.png"))
    imgs = [Image.open(f).convert("RGB") for f in frames]
    # one palette for all the frames (the screen's colours are few): no flicker between frames
    pal = imgs[len(imgs) // 2].quantize(colors=64, method=Image.Quantize.MEDIANCUT)
    q = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in imgs]
    q[0].save(out, save_all=True, append_images=q[1:], duration=FRAME_MS, loop=0, optimize=True, disposal=1)
    print(f"{out.relative_to(ROOT)}: {len(q)} frames, {out.stat().st_size // 1024} KB")


def main():
    subprocess.run([sys.executable, str(ROOT / "base" / "tools" / "build.py"), str(ROOT / "app"), "--host"], check=True,
                   capture_output=True)
    WORK.mkdir(parents=True, exist_ok=True)
    for f in WORK.glob("*.png"):
        f.unlink()
    OUT.mkdir(parents=True, exist_ok=True)
    for name, lines in (("words", words()), ("vowel", vowel()), ("speak", speak()), ("beat", beat()), ("melody", melody()), ("tongue", tongue()), ("cat", cat()), ("bird", bird()), ("seal", seal()), ("gator", gator()), ("elephant", elephant()), ("sing", sing())):
        script = WORK / f"{name}.txt"
        # (a fresh boot each: the voice at rest, slot 1, page 1)
        script.write_text("\n".join(["wait 400"] + lines) + "\n")
        subprocess.run([str(HOST), str(script), str(WORK)], check=True, capture_output=True)
        gif(name, OUT / f"preview-{name}.gif")


if __name__ == "__main__":
    main()
