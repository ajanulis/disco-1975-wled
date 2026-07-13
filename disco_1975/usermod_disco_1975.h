#pragma once

#include "wled.h"

/*
 * Disco 1975 — retro color-music ("tsvetomuzyka") audio-reactive effect.
 *
 * Recreates the character of 1970s-80s DIY color organs: four colored lamps,
 * each owning a frequency band, that KICK with the rhythm and fade — instead
 * of tracking loudness like most modern sound-reactive effects.
 *
 * Layout: 4 mirrored frequency zones per strip half — red=bass at the outer
 * ends, then yellow, green, blue=treble meeting at the center.
 *
 * Behavior:
 *  - beat-impulse envelope: a band lights only while its energy RISES above
 *    its own running average; sustained loudness decays — the strip bounces
 *    with the rhythm, not the volume
 *  - attack: the glow spreads outward from the band's home position,
 *    invading weaker neighbours' territory (strongest band wins each pixel)
 *  - two-phase retreat: the lit block shrinks back home at full brightness,
 *    then the last few LEDs dim out (~0.5 s)
 *  - pilot dots: a few dim LEDs mark each band's home, always on
 *  - carousel: after 5 s of silence the pilot dots counter-rotate (halves
 *    drift toward each other, wrapping at the ends) until music resumes
 *  - night dissolve + sleep: across N silent minutes everything fades to
 *    black, then the effect sleeps until a configurable wake hour (or until
 *    the user reselects the effect / power-cycles)
 *
 * Controls (effect sliders):
 *   Speed    = Attack   — how fast the glow spreads on a hit
 *   Intensity= Fade     — how fast it retreats/fades
 *   Custom1  = Reach    — how far a full-blast band can spread
 *   Custom2  = Pilot    — brightness of the idle home dots (0 = off)
 *   Option1  = Carousel — idle rotation on/off
 *   Option2  = Sleep    — silence timeout + night sleep on/off
 *   Palette  — band colors. "Default" = classic red/yellow/green/blue;
 *              any other palette is sampled at 4 even points
 *              (bass = palette start ... treble = palette end)
 *
 * Settings (Config -> Usermods -> Disco1975):
 *   sleepAfterMin — minutes of silence before sleeping (default 5)
 *   wakeHour      — hour (0-23) the effect auto-wakes; -1 = manual wake only.
 *                   Also powers WLED back ON if the strip was switched off
 *                   while Disco 1975 was the main-segment effect
 *
 * Band mapping: fftResult[] bins -> 4 bands is set below (d75_binLo/Hi).
 * Defaults are measured for a boxed I2S PDM mic (Athom/IoTorero sound-
 * reactive controllers), which hears little above ~1.4 kHz through the
 * enclosure. If your mic is different, dump real spectra with the
 * tools/fft_dump.py + tools/sweep.py scripts and adjust the bins.
 *
 * Tuning workflow: the Python reference implementation (reference/disco2.py)
 * runs the identical algorithm on a computer, streaming to WLED via UDP
 * realtime — iterate on feel there, then transfer the constants here.
 */

#define D75_NB       4        // frequency bands
#define D75_TAIL     0.12f    // below this level: reach parked, brightness dims
#define D75_GATE     0.10f
#define D75_PUNCH    1.15f    // energy must exceed PUNCH x running avg
#define D75_IDLE_S   5.0f     // s of silence -> carousel starts

// ---- usermod settings (Config -> Usermods) ---------------------------------
static uint8_t d75_sleepMin = 5;    // minutes of silence -> sleep
static int8_t  d75_wakeHour = 21;   // auto-wake hour; -1 = manual wake only

typedef struct Disco1975Data {
  float    levels[D75_NB];
  float    prevT[D75_NB];
  float    avg[D75_NB];
  float    peak[D75_NB];
  float    roff;        // carousel offset (px)
  uint32_t lastActive;  // millis() of last detected beat
  uint32_t lastCall;
  bool     sleeping;
} disco1975_t;

// classic colors, used with the "Default" palette (authentic tsvetomuzyka)
static const uint32_t d75_colors[D75_NB] = {
  RGBW32(255,   0, 0, 0),   // red    — bass, outer ends
  RGBW32(255, 200, 0, 0),   // yellow
  RGBW32(  0, 255, 0, 0),   // green
  RGBW32(  0, 120, 255, 0)  // blue   — treble, center
};

// 16 GEQ bins -> 4 bands. Defaults measured on a boxed PDM mic (bins 9+ are
// near-silent with room music): ~43-129 / 129-301 / 301-560 / 560+ Hz.
static const uint8_t d75_binLo[D75_NB] = { 0, 2, 4,  6 };
static const uint8_t d75_binHi[D75_NB] = { 1, 3, 5, 15 };

static uint16_t mode_disco1975(void) {
  if (SEGLEN < 16 || !SEGENV.allocateData(sizeof(disco1975_t))) {
    SEGMENT.fill(SEGCOLOR(0));   // fallback: solid primary color
    return 350;
  }
  disco1975_t *st = reinterpret_cast<disco1975_t*>(SEGENV.data);

  um_data_t *um_data;
  if (!UsermodManager::getUMData(&um_data, USERMOD_ID_AUDIOREACTIVE))
    um_data = simulateSound(SEGMENT.soundSim);   // no audio source configured
  uint8_t *fftResult = (uint8_t*)um_data->u_data[2];

  if (SEGENV.call == 0) {
    memset(st, 0, sizeof(disco1975_t));
    for (int z = 0; z < D75_NB; z++) st->peak[z] = 1e-4f;
    st->lastActive = strip.now;
    st->lastCall   = strip.now;
  }

  float dt = (strip.now - st->lastCall) / 1000.0f;
  if (dt <= 0.0f || dt > 0.2f) dt = 0.02f;
  st->lastCall = strip.now;

  // slider mappings
  float riseRate  = 0.5f + 12.0f * (float)SEGMENT.speed / 255.0f;        // /s
  float fadeKeep  = powf(0.16f, dt * (float)SEGMENT.intensity / 100.0f); // fraction/s
  float avgKeep   = powf(0.40f, dt);                                     // bed tracker
  float peakKeep  = powf(0.94f, dt);

  // --- band energies + beat-impulse envelope -------------------------------
  bool anyBeat = false;
  for (int z = 0; z < D75_NB; z++) {
    float e = 0.0f;
    if (z == D75_NB - 1) {
      // treble: many of its bins can be silent (mic/enclosure dependent) —
      // take the strongest bin instead of a mean so spikes aren't diluted
      for (int b = d75_binLo[z]; b <= d75_binHi[z]; b++)
        e = fmaxf(e, (float)fftResult[b]);
      e *= 0.8f / 255.0f;   // max-of-bins runs hot; trim level with the others
    } else {
      for (int b = d75_binLo[z]; b <= d75_binHi[z]; b++) e += fftResult[b];
      e /= 255.0f * (d75_binHi[z] - d75_binLo[z] + 1);
    }

    st->peak[z] = fmaxf(e, st->peak[z] * peakKeep);
    st->avg[z]  = st->avg[z] * avgKeep + e * (1.0f - avgKeep);

    float headroom = st->peak[z] - D75_PUNCH * st->avg[z];
    float target = (headroom > 1e-4f) ? (e - D75_PUNCH * st->avg[z]) / headroom : 0.0f;
    target = constrain(target, 0.0f, 1.0f);
    if (target < D75_GATE) target = 0.0f;
    if (target > 0.0f) anyBeat = true;

    float goal = (target > st->prevT[z] + 0.02f) ? target : 0.0f;
    st->prevT[z] = target;

    if (goal > st->levels[z]) st->levels[z] = fminf(st->levels[z] + riseRate * dt, goal);
    else                      st->levels[z] *= fadeKeep;
  }

  // --- silence bookkeeping: carousel, dissolve, sleep ----------------------
  if (anyBeat && !st->sleeping) st->lastActive = strip.now;
  float sil    = (strip.now - st->lastActive) / 1000.0f;
  float sleepS = 60.0f * (float)d75_sleepMin;

  bool sleepEnabled = SEGMENT.check2;
  if (sleepEnabled && sil > sleepS) st->sleeping = true;
  if (st->sleeping) {
    if (d75_wakeHour >= 0 && hour(localTime) == d75_wakeHour
        && minute(localTime) == 0) {
      st->sleeping   = false;
      st->lastActive = strip.now;
      sil = 0.0f;
    } else {
      SEGMENT.fill(BLACK);
      return FRAMETIME;
    }
  }

  float dissolve = 1.0f;
  if (sleepEnabled && sil > D75_IDLE_S)
    dissolve = powf(constrain((sleepS - sil) / (sleepS - D75_IDLE_S),
                              0.0f, 1.0f), 1.5f);

  // --- geometry -------------------------------------------------------------
  int N    = SEGLEN;
  int half = N / 2;
  float zone = (float)half / D75_NB;
  float homes[D75_NB];
  for (int z = 0; z < D75_NB; z++) homes[z] = zone * z + zone * 0.5f;

  float maxReach = 1.3f * (float)half * (float)SEGMENT.custom1 / 200.0f;
  float pilotLvl = 0.18f * (float)SEGMENT.custom2 / 128.0f;

  // carousel dot positions (full-strip coords; halves counter-rotate).
  // keep roff wrapped to [0,N): unlike Python's %, C fmodf goes negative on
  // negative arguments, which corrupts the ring-distance math once roff grows
  bool idle = SEGMENT.check1 && (sil > D75_IDLE_S);
  if (idle) st->roff = fmodf(st->roff + 12.0f * dt, (float)N);
  else      st->roff = 0.0f;
  float dotPos[2 * D75_NB];
  for (int z = 0; z < D75_NB; z++) {
    dotPos[z]          = fmodf(homes[z] + st->roff, (float)N);
    dotPos[D75_NB + z] = fmodf((float)N - 1.0f - homes[z] - st->roff + 2.0f * N, (float)N);
  }

  float reach[D75_NB], bright[D75_NB];
  for (int z = 0; z < D75_NB; z++) {
    float lr  = constrain((st->levels[z] - D75_TAIL) / (1.0f - D75_TAIL), 0.0f, 1.0f);
    reach[z]  = 3.0f + lr * maxReach;
    float br  = constrain(st->levels[z] / D75_TAIL, 0.0f, 1.0f);
    bright[z] = br * br;
  }

  // --- band colors: classic set on "Default" palette, else sample the
  //     selected palette at 4 even points (bass = start ... treble = end) ----
  uint32_t bandCol[D75_NB];
  for (int z = 0; z < D75_NB; z++)
    bandCol[z] = (SEGMENT.palette == 0) ? d75_colors[z]
               : SEGMENT.color_from_palette(z * 255 / (D75_NB - 1),
                                            false, false, 255);

  // --- render (compute left half, mirror; pilot dots on full strip) --------
  for (int i = 0; i < half; i++) {
    int   best  = 0;
    float bestV = 0.0f;
    for (int z = 0; z < D75_NB; z++) {
      float d = fabsf((float)i - homes[z]);
      float v = bright[z] * constrain((reach[z] - d) / 1.5f, 0.0f, 1.0f);
      if (v > bestV) { bestV = v; best = z; }
    }
    for (int side = 0; side < 2; side++) {
      int   px  = side ? (N - 1 - i) : i;
      int   win = best;
      float v   = bestV;
      for (int z = 0; z < 2 * D75_NB; z++) {          // pilot dots (ring distance)
        float d = fabsf((float)px - dotPos[z]);
        d = fminf(d, (float)N - d);
        // flat-top dot, hard edge: feathered edges get gamma-crushed into
        // wrong hues (dim yellow loses green -> red corners)
        float pv = (d <= 2.0f) ? pilotLvl : 0.0f;
        if (pv > v) { v = pv; win = z % D75_NB; }
      }
      v = v * v * dissolve;                            // gamma ~2 + night dissolve
      uint32_t c = bandCol[win];
      // round, don't truncate: keeps hue true at low brightness
      SEGMENT.setPixelColor(px, RGBW32((uint8_t)(R(c) * v + 0.5f),
                                       (uint8_t)(G(c) * v + 0.5f),
                                       (uint8_t)(B(c) * v + 0.5f), 0));
    }
  }
  return FRAMETIME;
}

static const char _data_FX_MODE_DISCO1975[] PROGMEM =
  "Disco 1975@Attack,Fade,Reach,Pilot,,Carousel,Sleep timer;;!;1f;sx=92,ix=100,c1=200,c2=128,o1=1,o2=1,pal=0";

class Disco1975Usermod : public Usermod {
  private:
    static const char _name[];
    uint8_t fxId = 255;
  public:
    void setup() {
      fxId = strip.addEffect(255, &mode_disco1975, _data_FX_MODE_DISCO1975);
    }

    // Power-on wake: the effect can't run while WLED is off, so wakeHour is
    // also checked here — if the strip was switched OFF with Disco 1975 as
    // the main-segment effect, power WLED back on at wake time.
    void loop() {
      static unsigned long lastCheck   = 0;
      static uint8_t       lastFireDay = 0;   // once-per-day latch
      if (millis() - lastCheck < 2000) return;
      lastCheck = millis();
      if (d75_wakeHour < 0 || bri != 0) return;              // disabled / already on
      if (hour(localTime)   != d75_wakeHour) return;
      if (minute(localTime) != 0)            return;
      if (day(localTime)    == lastFireDay)  return;
      if (strip.getMainSegment().mode != fxId) return;       // only wake our effect
      lastFireDay = day(localTime);
      strip.getMainSegment().markForReset();                 // fresh state, not sleeping
      toggleOnOff();
      stateUpdated(CALL_MODE_BUTTON);
    }

    void addToConfig(JsonObject& root) {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top["sleepAfterMin"] = d75_sleepMin;
      top["wakeHour"]      = d75_wakeHour;
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[FPSTR(_name)];
      bool ok = !top.isNull();
      ok &= getJsonValue(top["sleepAfterMin"], d75_sleepMin, 5);
      ok &= getJsonValue(top["wakeHour"],      d75_wakeHour, 21);
      if (d75_sleepMin < 1)   d75_sleepMin = 1;
      if (d75_wakeHour > 23)  d75_wakeHour = -1;
      return ok;
    }

    uint16_t getId() { return USERMOD_ID_UNSPECIFIED; }
};

const char Disco1975Usermod::_name[] PROGMEM = "Disco1975";
