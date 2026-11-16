#!/usr/bin/env python3
"""Banana Blast's sounds: the menu's cues and its background loop, synthesised to the Mega Drive's habits -- YM2612
2-operator FM (a modulator with self-feedback into a sine carrier) for the bass, stabs, lead and blips, SN76489
squares and noise for the arps and hats, and the DAC channel's crunch for the voice: 8-bit samples at ~10 kHz. No
echo (the hardware had none: a part is doubled on a second channel, later and quieter, as the era's composers did).
Nothing is sampled; every note and every syllable is computed here, so the set is original and rebuilds the same.

    tools/gen-blast-sounds.py [--voice mix|sample|shout|vox|none] [themes-dir]   writes <themes-dir>/blast/sounds/{scroll,select,back,fav,ready,insert,boost,bgm}.wav

The loop is an original funk breakbeat: 16 bars at 104 BPM (~37 s) in A dorian, slap FM bass, brass stabs on the
off-beats, a lead hook in the second half, square arps and scratches, and formant-synthesised vocal chops ("yeah",
"go", "uh", "hey": short, high and shouted, so they read as a sample and not a person) crushed to 8 bits; --voice vox
swaps them for a wordless FM choir stab, --voice none drops them, --voice sample uses recorded CC0 shouts from
tools/blast-voice/ (see its CREDITS.txt), crushed the same way, and --voice mix (the default) adds two more of them. Notes that ring over the loop point are folded back
onto its start, so it loops without a seam.
"""
import os
import sys
import wave

import numpy as np
from scipy.signal import lfilter

SR = 32000
rng = np.random.default_rng(1992)	# the noise (hats, snare, the cart's click, breath) is the same every build


def hz(note):
    """'A4' / 'F#3' / 'Bb2' -> Hz"""
    names = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
    n = names[note[0]]
    i = 1
    while note[i] in "#b":
        n += 1 if note[i] == "#" else -1
        i += 1
    return 440.0 * 2 ** ((n + 12 * (int(note[i:]) + 1) - 69) / 12)


def t_(dur):
    return np.arange(int(dur * SR)) / SR


def adsr(n, a=.002, d=.1, s=0., r=0.):
    """attack, an exponential decay to sustain level s, a linear release over the last r seconds"""
    x = np.arange(n) / SR
    e = np.where(x < a, x / max(a, 1e-6), s + (1 - s) * np.exp(-(x - a) / max(d, 1e-6)))
    tail = int(r * SR)
    if 0 < tail < n: e[n - tail:] *= np.linspace(1, 0, tail)
    return e


def lp(x, cutoff, poles=1):
    a = np.exp(-2 * np.pi * cutoff / SR)
    for _ in range(poles): x = lfilter([1 - a], [1, -a], x)
    return x


def hp(x, cutoff):
    return x - lp(x, cutoff)


def place(buf, clip, at):
    i = int(round(at * SR))
    if i < 0: clip, i = clip[-i:], 0
    j = min(len(buf), i + len(clip))
    if j > i: buf[i:j] += clip[:j - i]


def mix(*clips):
    """clips of any length, summed from their starts"""
    out = np.zeros(max(len(c) for c in clips))
    for c in clips: out[:len(c)] += c
    return out


def write(path, x, target, whole=False):
    """scaled so its loudest 30 ms is as loud (RMS) as the street's matching cue -- or, whole, so the whole clip is
    (a loop of hits is quieter than a pad at the same peak) -- then a soft knee over the peaks instead of a clip"""
    n = int(.03 * SR)
    if whole or len(x) <= n: lvl = np.sqrt(np.mean(x * x))
    else: lvl = np.sqrt(np.max(np.convolve(x * x, np.ones(n) / n, "valid")))
    x = x * target / max(lvl, 1e-9)
    knee = .7
    over = np.abs(x) > knee
    x[over] = np.sign(x[over]) * (knee + (1 - knee) * np.tanh((np.abs(x[over]) - knee) / (1 - knee)))
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())


# ---- the chips -----------------------------------------------------------------------------------------------------

def fm(f, dur, ratio=1., index=2., idecay=.08, ifloor=.3, fb=0., a=.002, d=.2, s=0., r=.02, glide=None):
    """a YM2612 pair: modulator (at f*ratio, with self-feedback fb) into a sine carrier. The modulation index falls
    from index to index*ifloor over idecay -- the bright pluck that makes it sound like the chip. glide: a start
    frequency that falls (or rises) to f over the first 60 ms"""
    n = int(dur * SR)
    tt = np.arange(n) / SR
    if glide:
        fr = f + (glide - f) * np.exp(-tt / .02)
        ph = 2 * np.pi * np.cumsum(fr) / SR
    else:
        ph = 2 * np.pi * f * tt
    mph = ph * ratio
    mod = np.sin(mph + fb * np.sin(mph))	# feedback, to one step: the operator's own output bent back onto its phase
    ix = index * (ifloor + (1 - ifloor) * np.exp(-tt / max(idecay, 1e-4)))
    return np.sin(ph + ix * mod) * adsr(n, a, d, s, r)


def square(f, dur, duty=.5, a=.001, d=.1, s=0., r=.01):
    """an SN76489 tone channel: a square, its volume in 2 dB steps (16 levels)"""
    n = int(dur * SR)
    x = np.where((t_(dur) * f) % 1 < duty, 1., -1.)
    e = adsr(n, a, d, s, r)
    e = 10 ** (np.round(20 * np.log10(np.maximum(e, 1e-4)) / 2) * 2 / 20)	# the attenuator's steps
    e[e < 10 ** (-28 / 20)] = 0
    return x * e


def psg_noise(dur, rate=SR / 2, a=.0005, d=.03, s=0., r=0., periodic=False):
    """the SN76489's noise channel: an LFSR clocked at rate, so lower rates sound grainier"""
    n = int(dur * SR)
    m = int(np.ceil(n * rate / SR)) + 1
    if periodic:	# its "periodic" mode: one bit in 15 set, a buzzy tone
        bits = np.zeros(m); bits[::15] = 1
    else:
        bits = rng.integers(0, 2, m).astype(float)
    x = bits[(np.arange(n) * rate / SR).astype(int)] * 2 - 1
    return x * adsr(n, a, d, s, r)


def crush(x, rate=10000, bits=8):
    """the DAC channel: the sample as it was stored on the cart -- held at rate, 8 bits"""
    x = lp(x, rate * .45, 2)
    peak = max(np.max(np.abs(x)), 1e-9)
    step = SR / rate
    idx = (np.floor(np.arange(len(x)) / step) * step).astype(int)
    q = 2 ** (bits - 1) - 1
    return np.round(x[idx] / peak * q) / q * peak


# ---- the voice -----------------------------------------------------------------------------------------------------
# A source-filter voice, kept deliberately unhuman: a hype man's shout as it came off a cart, not a person speaking. It
# is high (240-300 Hz), short (a fifth of a second), buzzy (a hard-closing glottal pulse), its formants wide so they
# don't ring, no breath, and pushed into the DAC hot before the crush. A slow, low, breathy voice is the uncanny one.
# Each word is a list of (seconds, (F1, F2, F3), f0, level); the values ramp from one segment's middle to the next.

def resonate(x, freqs, bw):
    """x through a two-pole resonator whose centre frequency follows freqs (per sample), 4 ms blocks"""
    y = np.zeros_like(x)
    zi = np.zeros(2)
    blk = int(.004 * SR)
    r = np.exp(-np.pi * bw / SR)
    for i in range(0, len(x), blk):
        th = 2 * np.pi * freqs[i] / SR
        # unity gain at the centre, roughly: the cascade then keeps the vowel's balance
        g = (1 - r) * np.sqrt(1 - 2 * r * np.cos(2 * th) + r * r)
        y[i:i + blk], zi = lfilter([g], [1, -2 * r * np.cos(th), r * r], x[i:i + blk], zi=zi)
    return y


def word(segs, onset=0., crush_rate=10000):
    """segs: [(dur, (F1,F2,F3), f0, level), ...]; onset: seconds of consonant noise in front (the h of hey)"""
    durs = np.array([s[0] for s in segs])
    mids = np.cumsum(durs) - durs * .5
    n = int(durs.sum() * SR)
    tt = np.arange(n) / SR
    f0 = np.interp(tt, mids, [s[2] for s in segs])
    level = np.interp(tt, mids, [s[3] for s in segs])
    ph = (np.cumsum(f0) / SR) % 1
    flow = np.where(ph < .55, .5 - .5 * np.cos(np.pi * ph / .55), np.cos(np.pi * (ph - .55) / .3 * .5))
    flow[ph >= .85] = 0
    src = np.diff(flow, prepend=0) * level	# the flow's derivative: the sharp closure is what makes it buzz
    if onset:
        k = int(onset * SR)
        src[:k] += rng.uniform(-1, 1, k) * .02 * np.linspace(1, .3, k)
    out = src
    for j, bw in enumerate((130, 170, 260)):
        out = resonate(out, np.interp(tt, mids, [s[1][j] for s in segs]), bw)
    out += resonate(src, np.full(n, 3400.), 400) * .35	# a fixed presence peak: the shout's edge
    out = hp(out, 180)
    out = out * adsr(n, .003, 10, 1, .025)
    out = np.tanh(out / max(np.max(np.abs(out)), 1e-9) * 3)	# hot into the DAC, as samples were
    return crush(out, crush_rate)


def yeah(pitch=1.):
    return word([(.03, (320, 2200, 2900), 270 * pitch, .8), (.05, (520, 1850, 2600), 300 * pitch, 1),
                 (.08, (760, 1350, 2500), 285 * pitch, 1), (.05, (760, 1300, 2450), 230 * pitch, .8)])


def go(pitch=1.):
    burst = lp(rng.uniform(-1, 1, int(.01 * SR)), 2000) * adsr(int(.01 * SR), .0005, .003)
    body = word([(.03, (420, 1000, 2400), 290 * pitch, .8), (.07, (560, 900, 2500), 285 * pitch, 1),
                 (.07, (460, 820, 2450), 250 * pitch, 1), (.03, (420, 780, 2400), 210 * pitch, .7)])
    out = np.zeros(len(body) + int(.014 * SR))
    place(out, crush(burst) * .5, 0)
    place(out, body, .014)
    return out


def uh(pitch=1.):
    return word([(.02, (650, 1200, 2500), 250 * pitch, 1), (.05, (680, 1220, 2500), 235 * pitch, 1),
                 (.03, (640, 1180, 2450), 200 * pitch, .7)], crush_rate=8000)


def hey(pitch=1.):
    return word([(.03, (560, 1850, 2600), 290 * pitch, .3), (.05, (560, 1900, 2650), 300 * pitch, 1),
                 (.06, (430, 2150, 2800), 285 * pitch, 1), (.03, (330, 2300, 2950), 240 * pitch, .7)], onset=.03)


def vox(notes, vowel="a", dur=.22):
    """not a word: an FM choir stab (Chowning's formant FM: per formant, a carrier on the harmonic nearest it), on
    the chord's notes, crushed like a sampled orchestra hit"""
    F = {"a": ((750, 1), (1200, .5), (2500, .2)), "o": ((480, 1), (850, .45), (2400, .15)),
         "e": ((450, 1), (1900, .4), (2600, .2))}[vowel]
    n = int(dur * SR)
    tt = np.arange(n) / SR
    out = np.zeros(n)
    for note in notes:
        f0 = hz(note) * (1 + .004 * np.sin(2 * np.pi * 6 * tt))
        ph = 2 * np.pi * np.cumsum(f0) / SR
        for F_, amp in F:
            k = max(1, round(F_ / hz(note)))
            out += amp * np.sin(k * ph + (.8 + .8 * np.exp(-tt / .04)) * np.sin(ph))
    return crush(out * adsr(n, .004, .12, .35, .05), 10000)


def sample(name, semis=0., keep=.5):
    """a recorded chop from tools/blast-voice/<name>.wav (any rate, mono or stereo, 16-bit), as it would have gone
    onto a cart: the silence trimmed, pitched by resampling (semis), driven hot, then crushed to 8 bits at 10 kHz"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "blast-voice", name + ".wav")
    with wave.open(path) as w:
        ch, rate = w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float) / 32768
    x = x.reshape(-1, ch).mean(axis=1)
    env = np.convolve(np.abs(x), np.ones(160) / 160, "same")
    on = np.where(env > np.max(env) * .06)[0]
    x = x[max(0, on[0] - 80):on[-1] + 160]
    step = rate / SR * 2 ** (semis / 12)	# pitch and length together, the way a sample player did it
    x = np.interp(np.arange(0, len(x) - 1, step), np.arange(len(x)), x)
    x = x[:int(keep * SR)] * adsr(min(len(x), int(keep * SR)), .002, 10, 1, .02)
    x = hp(x, 150)
    x = np.tanh(x / max(np.max(np.abs(x)), 1e-9) * 2.2)
    return crush(x, 10000)


def scratch(src, dur, cycles=2.):
    """a record scratch: the sample read forward and back on a turntable curve"""
    n = int(dur * SR)
    tt = np.arange(n) / n
    pos = (.5 - .5 * np.cos(2 * np.pi * cycles * tt)) * min(len(src) - 2, int(.14 * SR))
    i = pos.astype(int)
    fr = pos - i
    x = src[i] * (1 - fr) + src[i + 1] * fr
    gate = (np.abs(np.gradient(pos)) > .25).astype(float)	# the fader cut on the turn
    return crush(x * lp(gate, 300), 9000)


# ---- the cues ------------------------------------------------------------------------------------------------------
# FM blips, bright and short. The scroll is the quietest and shortest: a held direction plays it many times a second.

def scroll():
    return fm(1180, .045, ratio=2, index=1.2, idecay=.02, d=.015)


def select():
    out = np.zeros(int(.22 * SR))
    place(out, fm(660, .07, index=2.2, idecay=.03, d=.03), 0)
    place(out, fm(990, .13, index=2.2, idecay=.04, d=.05), .065)
    return out


def back():
    out = np.zeros(int(.22 * SR))
    place(out, fm(620, .07, index=2, idecay=.03, d=.03), 0)
    place(out, fm(415, .12, index=2, idecay=.04, d=.05), .065)
    return out


def fav():
    """the star: a quick bell run up an A major arpeggio, its last note doubled late and quiet (the chip's echo)"""
    out = np.zeros(int(.45 * SR))
    for k, n in enumerate(("A5", "C#6", "E6", "A6")):
        place(out, fm(hz(n), .12, ratio=3.5, index=1.8, idecay=.04, d=.06) * (1 - k * .06), k * .045)
    place(out, fm(hz("A6"), .12, ratio=3.5, index=1.8, idecay=.04, d=.06) * .3, .135 + .09)
    return out


def ready():
    """the game is chosen: an FM fall that steps down with the palette fade, a stab, a soft noise sweep"""
    out = np.zeros(int(.8 * SR))
    for k, n in enumerate(("G5", "E5", "C5", "G4")):
        place(out, fm(hz(n), .1, ratio=3, index=1.5, idecay=.04, d=.05), k * .06)
    stab = sum(fm(hz(n), .4, ratio=1, index=2.6, idecay=.06, ifloor=.4, fb=.6, d=.16, s=.1, r=.1) for n in ("A3", "E4", "A4", "C#5")) / 4
    place(out, stab * 1.2, .25)
    sweep = lp(psg_noise(.5, rate=12000, a=.02, d=.18), 4000)
    place(out, sweep * .25, .2)
    return out


def insert():
    """the cart going in: an FM thump with a falling pitch, the noise channel's click, a short rattle"""
    out = np.zeros(int(.24 * SR))
    place(out, fm(55, .14, ratio=.5, index=3, idecay=.02, d=.04, glide=180), 0)
    place(out, psg_noise(.012, rate=SR / 2, d=.003) * .7, .004)
    place(out, lp(psg_noise(.06, rate=6000, d=.015), 2500) * .35, .06)
    return out


def boost():
    """L/R on the course: a whoosh (noise whose clock falls) over a low FM rev"""
    n = int(.38 * SR)
    tt = np.arange(n) / SR
    rate = 15000 * np.exp(-tt / .12) + 1500
    # the noise clock falls: hold each LFSR bit for a stretch that lengthens
    clock = np.cumsum(rate / SR).astype(int)
    bits = rng.integers(0, 2, clock[-1] + 1).astype(float) * 2 - 1
    whoosh = bits[clock] * adsr(n, .01, .12)
    out = lp(whoosh, 6000) * .6
    place(out, fm(110, .32, ratio=.5, index=5, idecay=.12, d=.12, glide=70) * .5, 0)
    return out


# ---- the loop ------------------------------------------------------------------------------------------------------
VOICE = "mix"	# --voice mix | sample (both from tools/blast-voice/) | shout | vox | none
BPM = 104
BEAT = 60 / BPM
STEP = BEAT / 4
BAR = 4 * BEAT
SWING = .1	# the odd 16ths a tenth of a step late: the break sits back


def at(bar, step):
    return bar * BAR + step * STEP + (SWING * STEP if step % 2 else 0)


# chords: (the bass's root, the stab voicing)
AM7 = ("A1", ("G3", "C4", "E4", "A4"))
D9 = ("D2", ("F#3", "C4", "E4", "A4"))
FM7 = ("F1", ("A3", "C4", "E4", "F4"))
E7 = ("E2", ("G#3", "D4", "G4", "B4"))
G6 = ("G1", ("G3", "B3", "D4", "E4"))
EM7 = ("E2", ("G3", "B3", "D4", "E4"))
CHORDS = [AM7, D9, AM7, D9, AM7, D9, FM7, E7, AM7, D9, AM7, D9, FM7, G6, EM7, E7]

# the slap bass, per bar: (step, semitones above the root, a pop (the octave, plucked harder))
BASS = [(0, 0, 0), (3, 12, 1), (6, 0, 0), (7, 0, 0), (10, 7, 0), (11, 12, 1), (14, 10, 0), (15, 12, 1)]
BASS_FILL = [(0, 0, 0), (3, 12, 1), (6, 0, 0), (8, 3, 0), (9, 5, 0), (10, 7, 0), (12, 10, 0), (13, 12, 1), (14, 15, 1), (15, 12, 1)]

# the drums, per bar, in 16ths: k kick, s snare, g ghost snare, h hat, o open hat
GROOVE = {"k": (0, 7, 10), "s": (4, 12), "g": (9, 15), "h": (0, 2, 4, 6, 8, 10, 12), "o": (14,)}
GROOVE_B = {"k": (0, 3, 7, 10), "s": (4, 12), "g": (6, 9), "h": (0, 2, 4, 6, 8, 10, 12, 14), "o": ()}
FILL = {"k": (0, 7), "s": (4, 10, 12, 13, 14, 15), "g": (9, 11), "h": (0, 2, 4, 6, 8), "o": ()}

# the lead hook, bars 9-12: (step, note, length in steps)
LEAD = {
    8: [(0, "E5", 2), (3, "G5", 1), (4, "A5", 3), (8, "G5", 2), (10, "E5", 2), (12, "D5", 2), (14, "E5", 2)],
    9: [(0, "F#5", 3), (4, "E5", 2), (6, "D5", 2), (8, "C5", 4), (12, "A4", 4)],
    10: [(0, "E5", 2), (3, "G5", 1), (4, "A5", 3), (8, "C6", 2), (10, "B5", 1), (11, "A5", 1), (12, "G5", 4)],
    11: [(0, "A5", 6), (8, "G5", 2), (10, "E5", 2), (12, "D5", 2), (14, "E5", 2)],
}


def kick():
    return mix(fm(50, .26, ratio=.5, index=2.5, idecay=.015, d=.09, glide=160), psg_noise(.008, d=.002) * .25)


def snare():
    body = fm(190, .14, ratio=1.41, index=3, idecay=.02, d=.04, glide=260) * .5
    return mix(body, lp(psg_noise(.18, rate=SR / 2, d=.055), 9000) * .8)


def hat(open_=False):
    return hp(psg_noise(.16 if open_ else .03, rate=SR / 2, d=.06 if open_ else .009), 6000)


def slap(f, pop):
    return fm(f, STEP * 1.7, ratio=1, index=7 if pop else 4.5, idecay=.02 if pop else .035, ifloor=.15, fb=.9,
              a=.001, d=.12 if pop else .2, s=.15, r=.02)


def stab(notes, dur=.16):
    # brass: two pairs a few cents apart, so the chord beats a little like two FM channels do
    return sum(fm(hz(n) * dt, dur, ratio=1, index=2.8, idecay=.05, ifloor=.35, fb=.5, a=.003, d=.09, s=.25, r=.04)
               for n in notes for dt in (.997, 1.003)) / (2 * len(notes))


def lead(note, steps):
    dur = steps * STEP * .92
    n = int(dur * SR)
    tt = np.arange(n) / SR
    vib = 1 + .006 * np.sin(2 * np.pi * 5.5 * tt) * np.clip((tt - .12) / .1, 0, 1)
    f = hz(note)
    ph = 2 * np.pi * np.cumsum(f * vib) / SR
    ix = 1.8 + 1.2 * np.exp(-tt / .05)
    x = np.sin(ph + ix * np.sin(ph * 2 + .4 * np.sin(ph * 2)))
    return x * adsr(n, .006, .4, .6, .04)


def loop():
    total = BAR * len(CHORDS)
    n = int(round(total * SR))
    buf = np.zeros(n + 3 * SR)
    yeahs = {1.: yeah(), .84: yeah(.84)}
    gos, uhs, heys = go(), uh(), hey()
    for b, (root, voicing) in enumerate(CHORDS):
        fill = b in (7, 15)
        # drums
        g = FILL if fill else GROOVE_B if b % 4 == 3 else GROOVE
        for s in g["k"]: place(buf, kick() * .9, at(b, s))
        for s in g["s"]: place(buf, snare() * (.75 if not fill or s in (4, 12) else .45 + .05 * (s - 10)), at(b, s))
        for s in g["g"]: place(buf, snare() * .18, at(b, s))
        for s in g["h"]: place(buf, hat() * (.22 if s % 4 == 2 else .12), at(b, s))
        for s in g["o"]: place(buf, hat(True) * .14, at(b, s))
        # bass
        r = hz(root)
        for s, semi, pop in (BASS_FILL if fill or b % 4 == 3 else BASS):
            place(buf, slap(r * 2 ** (semi / 12), pop) * (.5 if pop else .62), at(b, s))
        # stabs from bar 5: the and-of-2 and the 3-and, with a push on 15 in every other bar
        if b >= 4:
            for s in (6, 10) + ((15,) if b % 2 else ()):
                place(buf, stab(voicing) * .34, at(b, s))
        # the lead and its late, quiet double (the chip's echo)
        for s, note, ln in LEAD.get(b, ()):
            x = lead(note, ln)
            place(buf, x * .3, at(b, s))
            place(buf, x * .1, at(b, s) + 3 * STEP)
        # square arps under the last four bars, up and down the stab's chord
        if b >= 12:
            arp = list(voicing) + [voicing[1].replace("3", "5").replace("4", "5")]
            for s in range(16):
                nt = arp[[0, 1, 2, 3, 4, 3, 2, 1][s % 8]]
                place(buf, square(hz(nt) * 2, STEP * .8, duty=.5, d=.05) * .045, at(b, s))
    # the voice: shouted chops, a wordless FM choir, or none (VOICE, from --voice)
    if VOICE == "shout":
        y_, g_, u_, h_ = yeah(), go(), uh(), hey()
        stut = lambda x, ln: x[:int(ln * SR)] * adsr(int(ln * SR), .001, 10, 1, .006)
        hits = [(y_, .5, 0, 0), (y_, .5, 4, 0), (h_, .42, 12, 6), (h_, .42, 12, 14), (g_, .5, 15, 12),
                (stut(g_, .04), .5, 7, 8), (stut(g_, .04), .5, 7, 10), (g_, .5, 7, 12)]
        hits += [(u_, .4, b, 14) for b in (1, 3, 5, 9, 11)]
    elif VOICE == "vox":
        a_, o_ = vox(("A3", "E4", "A4"), "a"), vox(("A4",), "o", .12)
        stut = lambda x, ln: x[:int(ln * SR)] * adsr(int(ln * SR), .001, 10, 1, .006)
        hits = [(a_, .45, 0, 0), (a_, .45, 4, 0), (vox(("D4", "F#4", "A4"), "e"), .4, 12, 6),
                (vox(("E4", "G#4", "B4"), "a"), .45, 15, 12), (stut(a_, .04), .45, 7, 8), (stut(a_, .04), .45, 7, 10),
                (vox(("E4", "G#4", "B4"), "a"), .45, 7, 12)]
        hits += [(o_, .35, b, 14) for b in (1, 3, 5, 9, 11)]
    elif VOICE in ("sample", "mix"):
        y_, h_ = sample("yeah", 1), sample("hey", 3)
        cut = lambda x, ln: x[:int(ln * SR)] * adsr(int(ln * SR), .001, 10, 1, .006)
        hits = [(y_, .5, 0, 0), (y_, .5, 4, 0), (h_, .45, 12, 6), (h_, .45, 12, 14), (h_, .45, 15, 12),
                (cut(y_, .05), .5, 7, 8), (cut(y_, .05), .5, 7, 10), (y_, .5, 7, 12)]	# y-y-yeah!
        hits += [(cut(h_, .09), .4, b, 14) for b in (1, 3, 5, 9, 11)]
        if VOICE == "mix":	# the sample set, plus a "whoo!" as the hook comes in and a "WHAT?!" into the loop's top
            hits = [h for h in hits if (h[2], h[3]) != (15, 12)]
            hits += [(sample("whoo", 1, .6), .45, 8, 0), (sample("what", 0, .4), .5, 15, 8)]
    else:
        hits = []
    for x, gain, b, s in hits: place(buf, x * gain, at(b, s))
    # the scratches cut a brass stab, not the voice
    sc = stab(("A3", "C4", "E4", "G4"), .2)
    place(buf, scratch(sc, 2 * BEAT, 2.5) * .5, at(11, 8))
    place(buf, scratch(sc, BEAT, 2) * .4, at(15, 4))
    # the loop point: whatever rings past the end lands on the start
    out = buf[:n].copy()
    out[:len(buf) - n] += buf[n:]
    return lp(out, 9000)	# the console's output filter: the edge off, not the snap


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    global VOICE
    args = sys.argv[1:]
    if "--voice" in args:
        i = args.index("--voice"); VOICE = args[i + 1]; del args[i:i + 2]
    root = args[0] if args else os.path.join(here, "system", "rootfs", "usr", "share", "banana", "themes")
    out = os.path.join(root, "blast", "sounds")
    os.makedirs(out, exist_ok=True)
    # each as loud as the street's matching cue (its loudest 30 ms, by RMS); the boost as the select
    cues = {"scroll": (scroll, .075), "select": (select, .28), "back": (back, .26), "fav": (fav, .13), "ready": (ready, .29),
            "insert": (insert, .17), "boost": (boost, .26)}
    for name, (fn, target) in cues.items():
        write(os.path.join(out, name + ".wav"), fn(), target)
    write(os.path.join(out, "bgm.wav"), loop(), .15, whole=True)	# the street's is .18, but it is all pad
    print("gen-blast-sounds: %d sounds in %s" % (len(cues) + 1, out))


if __name__ == "__main__":
    main()
