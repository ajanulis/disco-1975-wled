# Disco 1975 — retro color-music effect for WLED

A WLED usermod effect that recreates the character of 1970s–80s DIY **color
organs** (a.k.a. *tsvetomuzyka* — the Eastern-bloc teenager's rite of passage):
four colored lamps, each owning a frequency band, that **kick with the rhythm
and fade** — instead of tracking loudness like most modern sound-reactive
effects.

## What it looks like

The strip is mirrored around its center — four zones per half:

```
RED   YELLOW   GREEN   BLUE | BLUE   GREEN   YELLOW   RED
bass                 treble | treble                 bass
```

- A **bass kick** blooms red outward from its home spot, invades weaker
  neighbours' territory, then visibly **retreats home** at full brightness
  before the last few LEDs dim out — like the lamp inertia of the originals.
- A **sustained synth pad does nothing**: bands light only while their energy
  is *rising* above their own running average (beat-impulse envelope). The
  strip bounces with the rhythm, not the volume.
- In silence, dim **pilot dots** mark each band's home; after 5 s they start
  a slow **carousel** (the two halves counter-rotate, crossing at the center).
- After N silent minutes everything **dissolves into black** and the effect
  sleeps until a configurable wake hour — or until you touch it.

## Requirements

- WLED **0.15.x** built with the **AudioReactive** usermod (a digital I2S/PDM
  microphone). Tested on an ESP32 "sound reactive" controller
  (Athom / IoTorero) with 120 × WS2812.
- A 1D strip (not a 2D matrix), ideally 60+ LEDs.

## Install

1. Copy the `disco_1975/` folder into WLED's `usermods/` directory.
2. Register it in `wled00/usermods_list.cpp` (both blocks, next to the other
   usermods):

   ```cpp
   #ifdef USERMOD_DISCO_1975
     #include "../usermods/disco_1975/usermod_disco_1975.h"
   #endif
   ```
   ```cpp
   #ifdef USERMOD_DISCO_1975
   UsermodManager::add(new Disco1975Usermod());
   #endif
   ```

3. Add `-D USERMOD_DISCO_1975` to your PlatformIO env's `build_flags`
   (alongside the AudioReactive flags), e.g. in `platformio_override.ini`:

   ```ini
   [env:esp32dev_disco]
   board = esp32dev
   platform = ${esp32.platform}
   platform_packages = ${esp32.platform_packages}
   build_unflags = ${common.build_unflags}
   build_flags = ${common.build_flags} ${esp32.build_flags}
     ${esp32.AR_build_flags}
     -D USERMOD_DISCO_1975
   lib_deps = ${esp32.lib_deps}
     ${esp32.AR_lib_deps}
   board_build.partitions = ${esp32.default_partitions}
   ```

4. `pio run -e esp32dev_disco`, flash (OTA works), then pick **"Disco 1975"**
   from the effect list. Hard-refresh the browser if it doesn't show up —
   the WLED UI caches the effect list.

## Controls

Effect sliders (live, in the main UI):

| Control | Meaning |
|---|---|
| Speed (*Attack*) | how fast a glow spreads on a hit |
| Intensity (*Fade*) | how fast it retreats and fades |
| Custom1 (*Reach*) | how far a full-blast band can spread |
| Custom2 (*Pilot*) | brightness of the idle home dots (0 = off) |
| Option1 (*Carousel*) | idle rotation on/off |
| Option2 (*Sleep timer*) | silence dissolve + night sleep on/off |
| Palette | band colors — **Default** = the classic red/yellow/green/blue; any other palette is sampled at 4 even points (bass = palette start … treble = palette end) |

Settings (Config → Usermods → Disco1975):

| Setting | Meaning |
|---|---|
| `sleepAfterMin` | minutes of silence before sleeping (default 5) |
| `wakeHour` | hour (0–23) the effect auto-wakes; **-1** = wake manually only |

## Tuning for your microphone

The band split (`d75_binLo` / `d75_binHi` in the source) maps WLED's 16 GEQ
bins onto the 4 colors. The defaults are **measured, not theoretical**: a PDM
mic inside a controller enclosure hears very little above ~1.4 kHz with room
music, so the four bands split the *usable* spectrum (~43–129 / 129–301 /
301–560 / 560+ Hz) rather than textbook ranges.

To measure your own mic:

1. Enable *UDP Sound Sync → Send* in the AudioReactive settings.
2. Run `tools/fft_dump.py 20` on a computer on the same network while music
   plays — it prints per-bin mean/max.
3. Run `tools/sweep.py 10 60 15000` to play a frequency sweep through your
   speakers and find where your mic's response really ends.
4. Adjust the bin tables, rebuild, re-measure.

## The Python reference implementation

`reference/disco2.py` runs the **identical algorithm** on a computer (using
its microphone + numpy FFT) and streams pixels to WLED over UDP realtime
(DRGB). This is where the effect was designed: iterate on the *feel* at
conversation speed with no flash cycle, then transfer the constants to the
firmware.

```sh
pip install numpy sounddevice
python3 disco2.py <wled-ip>            # return mode (the classic)
python3 disco2.py <wled-ip> travel 60  # alt mode: kicks launch traveling pulses
```

It also has an extra toy the firmware doesn't: `travel` mode, where each beat
launches a pulse that flies along the strip and vanishes off the end.

## License

MIT — see [LICENSE](LICENSE).
