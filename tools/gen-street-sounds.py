#!/usr/bin/env python3
"""Banana Street's sounds: the menu's six cues and its background loop, synthesised to the Super NES's habits --
32 kHz, pulse and triangle voices through a soft (Gaussian-like) low-pass, and the SPC700's echo (a delay line with
a filtered feedback). Nothing is sampled: every note is computed here, so the set is original and rebuilds the same.

    tools/gen-street-sounds.py [themes-dir]     writes <themes-dir>/street/sounds/{scroll,select,back,fav,ready,insert,bgm}.wav

The loop is 16 bars at 84 BPM (~46 s), F major, mixed soft: an electric piano melody over a warm pad and a triangle
bass. It is rendered twice and the second pass kept, so the echo's tail runs across the loop point without a seam.
"""
import os
import sys
import wave

import numpy as np

SR = 32000
rng = np.random.default_rng(1997)	# the noise (the cartridge's clunk, the hats) is the same every build


def hz(note):
    """'A4' / 'F#3' / 'Bb2' -> Hz"""
    names = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    n = names[note[0]]
    i = 1
    while note[i] in "#b":
        n += 1 if note[i] == "#" else -1
        i += 1
    octave = int(note[i:])
    return 440.0 * 2 ** ((n + 12 * (octave + 1) - 69) / 12)


def t_(dur):
    return np.arange(int(dur * SR)) / SR


def pulse(f, dur, duty=.5):
    ph = (t_(dur) * f) % 1.0
    return np.where(ph < duty, 1.0, -1.0)


def tri(f, dur):
    ph = (t_(dur) * f) % 1.0
    return 4 * np.abs(ph - .5) - 1


def sine(f, dur):
    return np.sin(2 * np.pi * f * t_(dur))


def env(n, a=.002, d=.08, s=0., r=.0, dur=None):
    """attack, decay to sustain level s, release over the last r seconds"""
    x = np.arange(n) / SR
    e = np.ones(n)
    e = np.where(x < a, x / max(a, 1e-6), e)
    dec = np.exp(-(x - a) / max(d, 1e-6))
    e = np.where(x >= a, s + (1 - s) * dec, e)
    if r > 0:
        tail = int(r * SR)
        if tail < n: e[n - tail:] *= np.linspace(1, 0, tail)
    return e


def lowpass(x, cutoff):
    """one pole, twice: the SNES's interpolation takes the edge off a pulse without sounding like a filter sweep"""
    a = np.exp(-2 * np.pi * cutoff / SR)
    for _ in range(2):
        y = np.empty_like(x)
        acc = 0.
        for i in range(len(x)):	# short clips only; the loop uses lowpass_fast
            acc = (1 - a) * x[i] + a * acc
            y[i] = acc
        x = y
    return x


def lowpass_fast(x, cutoff):
    from scipy.signal import lfilter	# optional; falls back to the loop above
    a = np.exp(-2 * np.pi * cutoff / SR)
    for _ in range(2): x = lfilter([1 - a], [1, -a], x)
    return x


def lp(x, cutoff):
    try:
        return lowpass_fast(x, cutoff)
    except ImportError:
        return lowpass(x, cutoff)


def echo(x, delay=.16, fb=.38, mix=.32, damp=3200, tail=0.):
    """the SPC700's echo: a delay line whose feedback is low-passed (its 8-tap FIR), mixed under the dry signal"""
    d = int(delay * SR)
    n = len(x) + int(tail * SR)
    dry = np.zeros(n); dry[:len(x)] = x
    # block-wise: each repeat is the last one, delayed, damped and scaled (exact for a linear feedback line)
    wet = np.zeros(n)
    rep = dry.copy()
    k = 1
    while True:
        rep = np.concatenate([np.zeros(d), rep[:-d]]) * (fb if k > 1 else 1)
        rep = lp(rep, damp)
        if np.max(np.abs(rep)) < 1e-4 or k > 40: break
        wet += rep
        k += 1
    return dry + mix * wet


def place(buf, clip, at):
    i = int(at * SR)
    j = min(len(buf), i + len(clip))
    buf[i:j] += clip[:j - i]


def write(path, x, peak=.8):
    x = x / max(1e-9, np.max(np.abs(x))) * peak
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())


# ---- the cues ------------------------------------------------------------------------------------------------------
# Short, soft and pitched in one key (F major) so they sit under the loop instead of across it. The scroll is the
# quietest and shortest: a held direction plays it many times a second.

def blip(note, dur, duty=.25, decay=.03, cut=5000):
    x = pulse(hz(note), dur, duty) * env(int(dur * SR), a=.001, d=decay)
    return lp(x, cut)


def scroll():
    return blip("C6", .035, duty=.125, decay=.012, cut=3800) * .5


def select():
    out = np.zeros(int(.16 * SR))
    place(out, blip("F5", .05, decay=.025), 0)
    place(out, blip("C6", .08, decay=.04), .045)
    return echo(out, delay=.09, fb=.3, mix=.25, tail=.12)


def back():
    out = np.zeros(int(.16 * SR))
    place(out, blip("C6", .05, decay=.025), 0)
    place(out, blip("F5", .08, decay=.04), .045)
    return echo(out, delay=.09, fb=.3, mix=.22, tail=.1) * .9


def fav():
    out = np.zeros(int(.3 * SR))
    for k, n in enumerate(("A5", "C6", "F6", "A6")):
        place(out, blip(n, .09, duty=.5, decay=.05, cut=6000) * (1 - k * .08), k * .045)
    return echo(out, delay=.12, fb=.35, mix=.3, tail=.3)


def ready():
    """the game is chosen: a quick rise into a held F major chord"""
    out = np.zeros(int(.6 * SR))
    for k, n in enumerate(("C5", "F5", "A5", "C6")):
        place(out, blip(n, .07, decay=.04, cut=5500) * .8, k * .04)
    chord = sum(pulse(hz(n), .42, .5) for n in ("F5", "A5", "C6", "F6")) / 4
    chord = lp(chord * env(len(chord), a=.004, d=.18, s=.15, r=.12), 4500)
    place(out, chord, .16)
    return echo(out, delay=.14, fb=.35, mix=.3, tail=.35)


def insert():
    """the cartridge going in: a low thump, the click of the slot, a short rattle"""
    n = int(.22 * SR)
    out = np.zeros(n)
    tt = t_(.12)
    thump = np.sin(2 * np.pi * (140 * tt - 220 * tt * tt)) * env(len(tt), a=.001, d=.04)
    place(out, thump * .9, 0)
    click = lp(rng.uniform(-1, 1, int(.012 * SR)), 6000) * env(int(.012 * SR), a=.0005, d=.003)
    place(out, click * .8, .005)
    rattle = lp(rng.uniform(-1, 1, int(.06 * SR)), 2200) * env(int(.06 * SR), a=.001, d=.015)
    place(out, rattle * .35, .06)
    return out


# ---- the loop ------------------------------------------------------------------------------------------------------
BPM = 84
BEAT = 60 / BPM
BAR = 4 * BEAT

# 16 bars: two 8-bar phrases. Chords as (root for the bass, the pad's notes)
CHORDS = [
    ("F2", ("A3", "C4", "E4")), ("E2", ("G3", "B3", "D4")), ("D2", ("F3", "A3", "C4")), ("C2", ("E3", "G3", "B3")),
    ("Bb1", ("D3", "F3", "A3")), ("A1", ("C3", "E3", "G3")), ("G1", ("Bb3", "D4", "F4")), ("C2", ("Bb3", "E4", "G4")),
    ("F2", ("A3", "C4", "E4")), ("D2", ("F3", "A3", "C4")), ("Bb1", ("D3", "F3", "A3")), ("C2", ("E3", "G3", "Bb3")),
    ("A1", ("C3", "E3", "G3")), ("D2", ("F3", "A3", "C4")), ("G1", ("Bb3", "D4", "F4")), ("C2", ("Bb3", "E4", "G4")),
]

# the melody, a note and its length in beats per bar ("." a rest): sparse, so it reads as a town and not a tune
MELODY = [
    [("C5", 1.5), ("A4", .5), ("C5", 1), ("E5", 1)],
    [("D5", 3), (".", 1)],
    [("F5", 1), ("E5", .5), ("D5", .5), ("C5", 1), ("A4", 1)],
    [("G4", 3), (".", 1)],
    [("F4", 1), ("A4", 1), ("D5", 1.5), ("C5", .5)],
    [("C5", 2), ("A4", 1), ("G4", 1)],
    [("Bb4", 1.5), ("A4", .5), ("G4", 1), ("F4", 1)],
    [("G4", 3), (".", 1)],
    [("A4", 1), ("C5", 1), ("F5", 1.5), ("E5", .5)],
    [("D5", 2), ("C5", 1), ("A4", 1)],
    [("Bb4", 1), ("D5", 1), ("F5", 1), ("D5", 1)],
    [("E5", 3), (".", 1)],
    [("E5", 1), ("C5", 1), ("A4", 1), ("C5", 1)],
    [("D5", 1.5), ("F5", .5), ("A5", 2)],
    [("G5", 1), ("F5", 1), ("D5", 1), ("Bb4", 1)],
    [("C5", 4)],
]


def epiano(f, dur):
    """a soft electric piano: a sine with a little of its octave and a bell partial, a slow decay"""
    n = int(dur * SR)
    tt = np.arange(n) / SR
    x = np.sin(2 * np.pi * f * tt) + .25 * np.sin(2 * np.pi * 2 * f * tt) * np.exp(-tt / .25) \
        + .08 * np.sin(2 * np.pi * 3.5 * f * tt) * np.exp(-tt / .08)
    return x * env(n, a=.004, d=.9, s=.25, r=min(.12, dur / 3))


def pad(notes, dur):
    n = int(dur * SR)
    x = sum(pulse(hz(nt) * d, dur, .5) for nt in notes for d in (.998, 1.003)) / (2 * len(notes))
    x = lp(x, 1400)
    return x * env(n, a=.35, d=10, s=1, r=.4)


def bass(f, dur):
    n = int(dur * SR)
    return lp(tri(f, dur), 900) * env(n, a=.004, d=.5, s=.55, r=.06)


def hat():
    n = int(.03 * SR)
    return lp(rng.uniform(-1, 1, n), 7000) * env(n, a=.0005, d=.008)


def loop_once():
    total = BAR * len(CHORDS)
    mel = np.zeros(int(total * SR) + SR)
    acc = np.zeros_like(mel)
    for b, (root, notes) in enumerate(CHORDS):
        t0 = b * BAR
        place(acc, pad(notes, BAR + .3) * .22, t0)
        # the bass: root on 1, fifth on 3, a pickup on the 4-and
        r = hz(root)
        place(acc, bass(r, BEAT * 1.8) * .5, t0)
        place(acc, bass(r * 1.5, BEAT * 1.3) * .4, t0 + 2 * BEAT)
        place(acc, bass(r * 2, BEAT * .45) * .3, t0 + 3.5 * BEAT)
        for k in range(8):
            if k % 2: place(acc, hat() * .05, t0 + k * BEAT / 2)
        at = t0
        for note, beats in MELODY[b]:
            if note != ".": place(mel, epiano(hz(note), beats * BEAT * .95) * .38, at)
            at += beats * BEAT
    return mel, acc, total


def bgm():
    mel, acc, total = loop_once()
    n = int(total * SR)
    # twice around, so the second pass carries the first one's echo and release over the loop point
    def twice(x):
        y = np.zeros(2 * n + len(x))
        y[:len(x)] += x; y[n:n + len(x)] += x
        return y
    mix = echo(twice(mel), delay=BEAT * .75, fb=.32, mix=.28, damp=2600) + echo(twice(acc), delay=BEAT * .75, fb=.2, mix=.15, damp=1800)
    return mix[n:2 * n]


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "system", "rootfs", "usr", "share", "banana", "themes")
    out = os.path.join(root, "street", "sounds")
    os.makedirs(out, exist_ok=True)
    # peaks set so each cue is as loud as the Classic set's (by RMS): the scroll and the star the quietest
    cues = {"scroll": (scroll, .18), "select": (select, .48), "back": (back, .44), "fav": (fav, .24), "ready": (ready, .62),
            "insert": (insert, .48)}
    for name, (fn, peak) in cues.items():
        write(os.path.join(out, name + ".wav"), fn(), peak)
    write(os.path.join(out, "bgm.wav"), bgm(), .7)
    print("gen-street-sounds: 7 sounds in %s" % out)


if __name__ == "__main__":
    main()
