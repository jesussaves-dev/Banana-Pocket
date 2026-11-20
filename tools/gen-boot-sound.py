#!/usr/bin/env python3
"""The boot splash's sound, synthesised (nothing sampled, so it is original and rebuilds the same):

    tools/gen-boot-sound.py [out.wav]     default ui/assets/rnds/sounds/boot.wav

It follows ui/src/splash.cpp's clock. The clip starts when the first letter starts to move (CUE_AT), so the six
letters' landings are six rising bell ticks (C major pentatonic), under a soft rising shimmer; the word seats with a
bright chord, and the shine's sparkles twinkle out until the fade. 44.1 kHz mono 16-bit."""
import os
import sys
import wave

import numpy as np

SR = 44100
MS = 1000.0 / 60.0
CUE_AT = 8 * MS / 1000.0				# FIRST_FRAME: when splash.cpp fires the cue
FLIGHT, STAGGER, FIRST, N = 75 * MS / 1000.0, 5 * MS / 1000.0, 8 * MS / 1000.0, 6
SEAT = FIRST + STAGGER * (N - 1) + FLIGHT	# the leftmost letter seats: 1.8 s
END = 3.75
rng = np.random.default_rng(2002)
out = np.zeros(int((END - CUE_AT) * SR) + SR)


def hz(semi_from_c5):
    return 523.25 * 2 ** (semi_from_c5 / 12.0)


def bell(at, f, dur, gain):
    """a struck bell: a sine and its inharmonic partials, each dying faster than the one below"""
    n = int(dur * SR)
    t = np.arange(n) / SR
    s = np.zeros(n)
    for ratio, g, d in ((1.0, 1.0, 1.0), (2.0, .45, 1.6), (2.76, .25, 2.4), (5.4, .1, 4.0)):
        s += g * np.sin(2 * np.pi * f * ratio * t) * np.exp(-t * (3.0 * d) / dur * 1.8)
    s *= np.minimum(1.0, t / 0.003)		# no click on the strike
    i = int((at - CUE_AT) * SR)
    if i < 0 or i >= len(out):
        return
    j = min(len(out), i + n)
    out[i:j] += gain * s[: j - i]


# the shimmer under the flight: a rising, breathing tone, tremolo-ed
t = np.arange(int((SEAT - CUE_AT) * SR)) / SR
sweep = np.sin(2 * np.pi * np.cumsum(300 + 1500 * (t / t[-1]) ** 2) / SR)
sweep *= (t / t[-1]) ** 1.5 * (0.6 + 0.4 * np.sin(2 * np.pi * 9 * t)) * 0.10
out[: len(sweep)] += sweep

# six letters landing, rising up the pentatonic; the rightmost (first to start) flies first
for k, semi in enumerate((0, 2, 4, 7, 9, 12)):
    at = FIRST + STAGGER * k + FLIGHT
    bell(at, hz(semi), 0.5, 0.30)

# the word seats: a bright C major chord, long tail
for semi, g in ((0, .55), (4, .40), (7, .40), (12, .45), (16, .22)):
    bell(SEAT, hz(semi), 1.7, g * 0.7)

# the shine's sparkles, thinning out
for _ in range(26):
    at = SEAT + 0.1 + rng.random() ** 1.4 * 1.6
    semi = int(rng.choice((12, 16, 19, 24, 28)))
    bell(at, hz(semi), 0.35, 0.09 * (1 - (at - SEAT) / 2.0))

n = int((END - CUE_AT) * SR)
out = out[:n]
out *= np.minimum(1.0, (n - np.arange(n)) / (0.25 * SR))	# the last 250 ms fade
out /= max(1e-9, np.abs(out).max()) / 0.8
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "ui/assets/rnds/sounds/boot.wav")
with wave.open(path, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes((out * 32767).astype("<i2").tobytes())
print(f"{path}: {n / SR:.2f} s")
