#!/usr/bin/env python3
"""Original 16-bit-flavoured dubstep for the Banana Pocket promo. 144 BPM, F minor, 20.0 s.
Grid is offset 0.667 s so the drop (4.0 s) and every 2-bar phrase land on the video's screen cuts.
Usage: gen-promo-music.py <out.wav>"""
import os, subprocess, sys, wave
import numpy as np

SR = 44100
DUR = 20.0
BPM = 144.0
BEAT = 60.0 / BPM
STEP = BEAT / 4
BAR = BEAT * 4
DROP = 4.0
N = int(SR * DUR)
rng = np.random.default_rng(1616)
t_all = np.arange(N) / SR

def midi(n): return 440.0 * 2 ** ((np.asarray(n, dtype=float) - 69) / 12)

def put(buf, sig, t0, gain=1.0):
    i = int(round(t0 * SR))
    if i >= len(buf) or i + len(sig) <= 0: return
    a, b = max(i, 0), min(i + len(sig), len(buf))
    buf[a:b] += sig[a - i:b - i] * gain

def crush(x, bits=6, hold=3):
    x = np.repeat(x[::hold], hold)[:len(x)]
    q = 2 ** (bits - 1)
    return np.round(x * q) / q

def hp(x, a=0.97):  # one-pole high-pass
    y = np.empty_like(x); p = 0.0; q = 0.0
    for i, v in enumerate(x):
        p = a * (p + v - q); q = v; y[i] = p
    return y

def lp(x, a=0.15):
    y = np.empty_like(x); p = 0.0
    for i, v in enumerate(x):
        p += a * (v - p); y[i] = p
    return y

# ---- drum voices -----------------------------------------------------------
def kick():
    t = np.arange(int(SR * 0.45)) / SR
    f = 42 + 140 * np.exp(-t * 38)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph) * np.exp(-t * 7.0)
    y += 0.5 * np.sin(2 * np.pi * 900 * t) * np.exp(-t * 300)
    return np.tanh(y * 2.4)

def snare(heavy=1.0):
    t = np.arange(int(SR * 0.35)) / SR
    n = rng.standard_normal(len(t))
    y = (n - lp(n, 0.1)) * np.exp(-t * 17)
    y += 0.9 * np.sin(2 * np.pi * (170 + 90 * np.exp(-t * 40)) * t) * np.exp(-t * 20)
    y = crush(np.tanh(y * 2.0 * heavy), 7, 2)
    return y * 0.9

def hat(open_=False):
    t = np.arange(int(SR * (0.22 if open_ else 0.05))) / SR
    n = rng.standard_normal(len(t))
    y = (n - lp(n, 0.12)) * np.exp(-t * (14 if open_ else 90))
    return crush(y, 7, 1) * 0.16

def crash(length=2.5):
    t = np.arange(int(SR * length)) / SR
    n = rng.standard_normal(len(t))
    y = (n - lp(n, 0.12)) * np.exp(-t * 3.2)
    return crush(y, 8, 1) * 0.28

# ---- tonal voices ----------------------------------------------------------
def pulse(freq, length, duty=0.5, decay=6.0, vib=0.0):
    t = np.arange(int(SR * length)) / SR
    f = freq * (1 + vib * np.sin(2 * np.pi * 6 * t))
    ph = np.cumsum(f) / SR
    y = np.where((ph % 1.0) < duty, 1.0, -1.0)
    env = np.minimum(t * 400, 1) * np.exp(-t * decay)
    return crush(y * env, 5, 2) * 0.28

def growl(freq, length, lfo_hz, seed=0):
    t = np.arange(int(SR * length)) / SR
    lfo = 0.5 + 0.5 * np.sin(2 * np.pi * lfo_hz * t - np.pi / 2)  # starts closed
    idx = 0.4 + 5.5 * lfo ** 1.5
    car = 2 * np.pi * freq * t
    mod = np.sin(car * 1.0 + 1.3 * np.sin(car * 0.5))
    y = np.sin(car + idx * mod)
    y = np.tanh(y * (1.5 + 2.5 * lfo))
    env = np.minimum(t * 200, 1) * np.minimum((length - t) * 120, 1)
    sub = np.sin(car) * 0.8
    return (y * 0.8 + sub) * env * 0.55

def sub(freq, length):
    t = np.arange(int(SR * length)) / SR
    return np.sin(2 * np.pi * freq * t) * np.minimum(t * 300, 1) * np.minimum((length - t) * 100, 1)

def riser(t0, t1):
    n = int((t1 - t0) * SR)
    t = np.arange(n) / SR
    k = t / (t1 - t0)
    noise = rng.standard_normal(n) * k ** 2.0
    noise = (noise - lp(noise, 0.15)) * 0.35
    f = 200 * 2 ** (k * 4.5)
    saw = ((np.cumsum(f) / SR) % 1.0) * 2 - 1
    return crush(noise * 0.4 + saw * 0.2 * k ** 1.5, 7, 1)

def chop(name, gain=1.0):
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blast-voice", name + ".wav")
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-f", "f32le", "-ac", "1", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    y = np.frombuffer(raw, dtype=np.float32).astype(float)
    y /= max(np.abs(y).max(), 1e-6)
    return crush(y, 7, 2) * gain

# ---- arrangement -----------------------------------------------------------
ROOT = {"F": 41, "Db": 37, "Ab": 44, "Eb": 39}
TRIAD = {"F": [0, 3, 7, 12], "Db": [0, 4, 7, 12], "Ab": [0, 4, 7, 12], "Eb": [0, 4, 7, 12]}
HOOK = {"F": [(0, 80, 3), (4, 77, 3), (8, 75, 2), (10, 77, 2), (12, 80, 4)],
        "Db": [(0, 77, 3), (4, 73, 3), (8, 72, 2), (10, 73, 2), (12, 77, 4)],
        "Ab": [(0, 84, 3), (4, 80, 3), (8, 79, 2), (10, 80, 2), (12, 84, 4)],
        "Eb": [(0, 82, 3), (4, 79, 3), (8, 77, 2), (10, 75, 2), (12, 79, 4)]}
SEQ = ["F", "Db", "Ab", "Eb"]

drums = np.zeros(N)
music = np.zeros(N)       # ducked by the kick
lead_fx = np.zeros(N)
kick_times = []

def bar_start(b): return DROP + b * BAR     # bar 0 = drop
def chord_of(b):
    if b < 0: return ["Db", "Eb"][b + 2]
    if b >= 9: return "F"
    return SEQ[b % 4]
def section(b):
    if b < 0: return 0
    if b < 2: return 1
    if b < 4: return 2
    if b < 6: return 3
    return 4

K, S, H, HO = kick(), snare(), hat(), hat(True)

def kick_at(t, g=1.0):
    put(drums, K, t, g); kick_times.append(t)

# intro (bars -2, -1): arp, riser, snare roll
for b in (-2, -1):
    ch = chord_of(b); r = ROOT[ch] + 24
    for s in range(16):
        iv = TRIAD[ch][[0, 1, 2, 3, 2, 1, 2, 3][s % 8]]
        vol = 0.35 + 0.35 * ((b + 2) * 16 + s) / 32
        put(music, pulse(midi(r + iv), STEP * 1.6, 0.25, 9), bar_start(b) + s * STEP, vol)
    if b == -2:
        put(music, sub(midi(ROOT[ch]), BAR) * 0.6, bar_start(b))
put(drums, riser(0.667, DROP), 0.667, 0.9)
roll = [(DROP - BAR + i * BEAT, 0.35 + 0.1 * i) for i in range(4)]
for tt, g in roll: put(drums, S, tt + BEAT * 0.5, g * 0.6)
for i in range(8): put(drums, S, DROP - BEAT * 2 + i * STEP * 2, 0.4 + 0.07 * i)
for i in range(8): put(drums, S, DROP - BEAT + i * STEP / 2, 0.55 + 0.05 * i)
put(drums, K, DROP - BAR, 0.8); kick_times.append(DROP - BAR)
put(drums, K, DROP - BAR * 0.5, 0.8); kick_times.append(DROP - BAR * 0.5)

# drops
for b in range(0, 10):
    ch = chord_of(b); sec = section(b); t0 = bar_start(b)
    if t0 >= DUR: break
    last_bar_of_phrase = (b % 2 == 1) and b < 9
    r = ROOT[ch]
    final = (b == 9)
    if final:
        kick_at(t0, 1.2)
        put(drums, crash(1.0), t0, 1.0)
        put(music, sub(midi(ROOT["F"]), 1.0) * 0.9, t0)
        continue
    # drums: halftime kick/snare
    ks = {1: [0, 10], 2: [0, 6, 10], 3: [0, 3, 6, 10, 14], 4: [0, 6, 10, 14]}[sec]
    if b == 0: ks = [0, 10]
    for s in ks:
        kick_at(t0 + s * STEP, 1.0)
    put(drums, S, t0 + 8 * STEP, 1.0)
    hat_step = 2 if sec < 3 else 1
    for s in range(0, 16, hat_step):
        put(drums, HO if s % 8 == 6 else H, t0 + s * STEP, 0.7 if s % 4 else 1.0)
    if last_bar_of_phrase:  # fill into the next cut
        for i in range(4): put(drums, S, t0 + 12 * STEP + i * STEP, 0.5 + 0.12 * i)
    if b % 2 == 0:
        put(drums, crash(2.0), t0, 0.8)
    # bass: wobble, rate changes with section
    rate = {1: 2.0, 2: 3.0, 3: 4.0, 4: 2.0}[sec] / BEAT
    bar_len = BAR
    notes = [r] if sec != 2 else [r, r, r + 12, r]
    seglen = bar_len / len(notes)
    for i, n in enumerate(notes):
        put(music, growl(midi(n), seglen, rate / 1.0, i), t0 + i * seglen, 1.0)
    # arp + hook
    if sec >= 2:
        for s in range(16):
            iv = TRIAD[ch][[0, 2, 1, 3, 2, 3, 1, 2][s % 8]]
            put(music, pulse(midi(ROOT[ch] + 36 + iv - 12), STEP * 1.2, 0.125, 14), t0 + s * STEP, 0.4)
        for (s, m, ln) in HOOK[ch]:
            seg = pulse(midi(m), STEP * ln, 0.5, 3.0, 0.004)
            put(music, seg, t0 + s * STEP, 1.3)
            put(lead_fx, seg, t0 + s * STEP + STEP * 3, 0.4)
    if sec == 1:
        for s in (0, 6, 10):
            put(music, pulse(midi(ROOT[ch] + 24), STEP * 2, 0.25, 8), t0 + s * STEP, 0.7)

# vocal chops
put(drums, chop("hey", 0.9), DROP - 0.03)
put(drums, chop("yeah", 0.8), bar_start(2) + 0.0)
put(drums, chop("what", 0.7), bar_start(4) - BEAT)
put(drums, chop("whoo", 1.0), bar_start(8))
put(drums, chop("hey", 0.9), bar_start(9))

# sidechain duck for music + lead echo
duck = np.ones(N)
for kt in kick_times:
    i = int(kt * SR); n = int(0.22 * SR)
    if i >= N: continue
    seg = 1 - 0.85 * np.exp(-np.arange(min(n, N - i)) / (0.07 * SR))
    duck[i:i + len(seg)] = np.minimum(duck[i:i + len(seg)], seg)
mix = drums * 1.0 + (music + lead_fx) * duck * 0.9

# stereo: tiny haas widening on the music bus
delay = int(0.011 * SR)
L = mix.copy(); R = mix.copy()
wide = (music + lead_fx) * duck * 0.9
R[delay:] += 0.25 * wide[:-delay]; L[delay:] -= 0.0

# fade tail
tail = np.ones(N)
tf = t_all > 19.0
tail[tf] = np.exp(-(t_all[tf] - 19.0) * 3.0)
L *= tail; R *= tail
out = np.stack([L, R], axis=1)
out *= 0.28 / np.sqrt((out ** 2).mean())
out = np.tanh(out * 1.6) / np.tanh(1.6) * 0.95
pcm = (out * 32767).astype("<i2")
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print("peak %.2f rms %.3f" % (np.abs(out).max(), np.sqrt((out ** 2).mean())))
