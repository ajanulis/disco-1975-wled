#!/usr/bin/env python3
"""Log frequency sweep 80 Hz -> 15 kHz -> 80 Hz through the speakers.
Watch the disco strip: the glow should walk red -> amber -> green -> blue
(ends toward center) and back.

Usage: sweep.py [seconds_per_direction] [f0] [f1]   (default 8 80 15000)
"""
import sys
import numpy as np
import sounddevice as sd

FS = 44100
T = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0   # one direction
F0 = float(sys.argv[2]) if len(sys.argv) > 2 else 80.0
F1 = float(sys.argv[3]) if len(sys.argv) > 3 else 15000.0

t = np.linspace(0, 2 * T, int(FS * 2 * T), endpoint=False)
tri = 1 - np.abs(t / T - 1)                    # 0 -> 1 -> 0
finst = np.exp(np.log(F0) + (np.log(F1) - np.log(F0)) * tri)
phase = 2 * np.pi * np.cumsum(finst) / FS
x = 0.5 * np.sin(phase)
edge = int(FS * 0.05)                          # 50 ms fade in/out, no clicks
x[:edge] *= np.linspace(0, 1, edge)
x[-edge:] *= np.linspace(1, 0, edge)

print(f"sweeping {F0:.0f} Hz -> {F1:.0f} Hz -> {F0:.0f} Hz over {2*T:.0f}s")
sd.play(x.astype(np.float32), FS, blocking=True)
print("done")
