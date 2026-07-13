#!/usr/bin/env python3
"""Childhood disco v2 — territorial color-music.

Each band has a HOME position (mirrored on both strip halves).
Its glow expands from home with band energy, invading weaker
neighbours' territory, and fades back.

Modes:
  return  — glow expands/retracts around home with the level
  travel  — each kick launches a pulse that travels away from home,
            fading, and disappears off the far end

Usage: disco2.py <wled-ip> [return|travel] [seconds]
"""
import json, socket, sys, time, urllib.request
from datetime import datetime, timedelta
import numpy as np
import sounddevice as sd

if len(sys.argv) < 2:
    sys.exit("usage: disco2.py <wled-ip> [return|travel] [seconds]")
IP, PORT = sys.argv[1], 21324
N, FS, WIN, FPS = 120, 44100, 2048, 60
DECAY, GATE, GAMMA = 0.97, 0.10, 1.6
RISE = 0.08          # max level rise per frame: 0 -> full in ~0.2 s
TAIL = 0.12          # below this level: reach stays home-sized, brightness dims out
IDLE_AFTER = 5.0     # s of silence before the pilot dots start rotating
IDLE_SPEED = 12.0    # px/s carousel speed (~10 s per lap)
SLEEP_AFTER = 300.0  # s of silence -> strip off until 21:00 or manual on
WAKE_HOUR = 21       # next 9 PM


def wled_set_on(on):
    req = urllib.request.Request(f"http://{IP}/json/state",
                                 data=json.dumps({"on": on}).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=3).read()
    except OSError as e:
        print("wled_set_on failed:", e)


def wled_is_on():
    try:
        with urllib.request.urlopen(f"http://{IP}/json/state", timeout=3) as r:
            return json.load(r).get("on", False)
    except OSError:
        return None


def next_wake():
    nw = datetime.now().replace(hour=WAKE_HOUR, minute=0, second=0,
                                microsecond=0)
    if nw <= datetime.now():
        nw += timedelta(days=1)
    return nw

EDGES = [80, 300, 1100, 4000, 15000]
COLORS = np.array([
    (255, 0, 0),        # red    — bass, outermost
    (255, 200, 0),      # yellow
    (0, 255, 0),        # green
    (0, 120, 255),      # blue   — treble, center
], dtype=float)
NB = len(COLORS)

# homes: zone centers on left half + mirrored right half (4 x 15 LEDs)
HOMES_L = [7, 22, 37, 52]
HOMES = np.array(HOMES_L + [N - 1 - h for h in HOMES_L])       # 10 homes
HBAND = np.array(list(range(NB)) * 2)  # mirrored positions keep same band
IDX = np.arange(N)
DIST = np.abs(IDX[None, :] - HOMES[:, None]).astype(float)     # 10 x 120

# --- travel mode params ---
SPEED = 90.0        # px/s pulse velocity
SIGMA = 2.5         # pulse width
T_MAX = 130.0       # px traveled before a pulse dies
ONSET = 0.18        # level jump that spawns a pulse
COOLDOWN = 0.12     # s per band between pulses

ring = np.zeros(WIN, dtype=np.float32)
def audio_cb(indata, frames, t, status):
    global ring
    ring = np.roll(ring, -len(indata))
    ring[-len(indata):] = indata[:, 0]

freqs = np.fft.rfftfreq(WIN, 1 / FS)
band_bins = [(freqs >= lo) & (freqs < hi) for lo, hi in zip(EDGES, EDGES[1:])]
window = np.hanning(WIN).astype(np.float32)

mode = sys.argv[2] if len(sys.argv) > 2 else "return"
duration = float(sys.argv[3]) if len(sys.argv) > 3 else 60
print(f"=== mode: {mode} — {duration:.0f}s ===")

levels = np.zeros(NB)
prev_t = np.zeros(NB)
peaks = np.full(NB, 1e-6)
avg = np.zeros(NB)          # per-band running average (the "bed")
nfloor = np.full(NB, 1e-3)  # per-band noise-floor tracker
pulses = []                      # [band, amp, t0]
last_spawn = np.zeros(5)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

with sd.InputStream(channels=1, samplerate=FS, blocksize=WIN // 4,
                    callback=audio_cb):
    t0 = time.time()
    frame = 0
    last_active = t0
    roff = 0.0
    while (now := time.time()) - t0 < duration:
        mag = np.abs(np.fft.rfft(ring * window))
        energy = np.array([np.sqrt(np.mean(mag[b] ** 2)) for b in band_bins])
        peaks = np.maximum(energy, peaks * 0.999)
        # noise floor: sinks fast to the quietest recent level, rises slowly
        nfloor = np.minimum(nfloor * 1.003, energy)
        # rhythm normalization: brightness = how far the band punches
        # above its own running average, not its absolute level
        avg = 0.99 * avg + 0.01 * energy          # ~1.7 s bed tracker
        headroom = np.maximum(peaks - 1.15 * avg, 1e-6)
        target = np.clip((energy - 1.15 * avg) / headroom, 0, 1)
        target[(target < GATE) | (energy < 4 * nfloor)] = 0.0
        # beat-impulse envelope: a band lights only while its energy is
        # RISING; sustained loudness decays like silence -> strip bounces
        rise_amt = target - prev_t
        prev_t = target.copy()
        goal = np.where(rise_amt > 0.02, target, 0.0)
        if target.any():
            last_active = now

        if now - last_active > SLEEP_AFTER:
            wake_at = next_wake()
            print(f"sleep: 5 min of silence -> strip OFF until "
                  f"{wake_at:%a %H:%M} or manual switch-on", flush=True)
            wled_set_on(False)
            time.sleep(3)                    # let DRGB timeout expire
            while True:
                if datetime.now() >= wake_at:
                    print("wake: it's 9 PM", flush=True)
                    wled_set_on(True)
                    break
                if wled_is_on():
                    print("wake: switched on manually", flush=True)
                    break
                time.sleep(5)
            last_active = time.time()
            levels[:] = 0
            continue
        # limited attack: glow visibly spreads out instead of teleporting
        levels = np.where(goal > levels,
                          np.minimum(levels + RISE, goal),
                          levels * DECAY)

        lvl10 = levels[HBAND]                        # level per home (10,)

        if mode == "return":
            # reach grows with level: full blast crosses center & beyond
            # phase 1 (level 1.0 -> TAIL): lit block shrinks toward home at
            # FULL brightness — the retreat is the show.
            # phase 2 (level TAIL -> 0): reach parked at home size, the last
            # few LEDs dim out over ~0.5 s into the pilot glow.
            lvl_r = np.clip((lvl10 - TAIL) / (1 - TAIL), 0, 1)
            reach = 3.0 + lvl_r * 75.0
            bright = np.clip(lvl10 / TAIL, 0, 1) ** 2
            contrib = bright[:, None] * np.clip((reach[:, None] - DIST) / 1.5, 0, 1)
            # pilot glow: ~4 LEDs at each home, always on at min brightness.
            # after IDLE_AFTER s of silence the dots carousel around the strip
            # (ring wraparound); first note snaps them back home.
            if now - last_active > IDLE_AFTER:
                roff += IDLE_SPEED / FPS
                # halves counter-rotate: left dots drift right, right dots
                # drift left — crossing at center, mirror-symmetric throughout
                pos = np.concatenate(((HOMES[:NB] + roff) % N,
                                      (HOMES[NB:] - roff) % N))
                pd = np.abs(IDX[None, :] - pos[:, None])
                pd = np.minimum(pd, N - pd)
            else:
                roff = 0.0
                pd = DIST
            pilot = 0.18 * np.clip(1 - pd / 2.5, 0, 1) ** 0.7
            contrib = np.maximum(contrib, pilot)
        else:  # travel
            for z in range(NB):
                if rise_amt[z] > ONSET and target[z] > 0.22 \
                   and now - last_spawn[z] > COOLDOWN:
                    pulses.append([z, target[z], now])
                    last_spawn[z] = now
            # small standing glow at each home
            contrib = lvl10[:, None] * np.exp(-DIST ** 2 / (2 * 4.0 ** 2)) * 0.7
            alive = []
            for z, amp, tb in pulses:
                dp = SPEED * (now - tb)
                if dp < T_MAX:
                    alive.append([z, amp, tb])
                    fade = amp * (1 - dp / T_MAX)
                    g = fade * np.exp(-(DIST[[z, NB + z]] - dp) ** 2 / (2 * SIGMA ** 2))
                    contrib[[z, NB + z]] = np.maximum(contrib[[z, NB + z]], g)
            pulses = alive

        # territorial fight: strongest band wins each pixel
        win = np.argmax(contrib, axis=0)
        v = contrib[win, IDX] ** GAMMA
        # dissolve into the night: from idle start to the 5-min shut-off,
        # everything (carousel included) fades gradually to black
        sil = now - last_active
        if sil > IDLE_AFTER:
            v = v * np.clip((SLEEP_AFTER - sil) /
                            (SLEEP_AFTER - IDLE_AFTER), 0, 1) ** 1.5
        rgb = np.rint(COLORS[HBAND[win]] * v[:, None]).astype(np.uint8)

        sock.sendto(bytes([2, 2]) + rgb.tobytes(), (IP, PORT))

        frame += 1
        if frame % 30 == 0:
            bars = " ".join(f"{c}:{'#' * int(l * 10):<10}" for c, l in
                            zip(["LO", "lo", "hi", "HI"], levels))
            print(f"{bars}  pulses:{len(pulses) if mode=='travel' else '-'}",
                  flush=True)
        time.sleep(1 / FPS)

print("done — strip returns to WLED effect in ~2 s")
