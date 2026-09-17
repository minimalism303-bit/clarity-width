# ClarityWidth

A three-knob channel-strip insert effect built with JUCE 8 / C++17 / CMake.
Put it on an individual instrument track, turn **ENHANCE**, **WIDTH** and
**MUD CUT**, and the track sits cleaner, wider and more polished in the mix
without sounding like it's been through a static EQ or a treble boost.

This README is the full deliverable writeup: build instructions, signal
flow, the three algorithms explained, parameter tables, CPU numbers, test
results, known limitations and what a V2 should tackle.

---

## 1. What was actually done vs. what's left

**Done, and verified in this environment (Ubuntu 24.04, x86_64, headless):**
- Researched current JUCE 8 / VST3 practice before writing any DSP (parameter
  smoothing conventions, mono-safe M/S widening technique, `dsp::Oversampling`
  usage, current CMake plugin setup) - see §9 for sources/approach.
- Full C++17/JUCE/CMake project: `PluginProcessor`, `PluginEditor`, three
  independent DSP modules (`DSP/MudProcessor.h`, `DSP/StereoWidth.h`,
  `DSP/Enhancer.h`), metering, presets, APVTS-based parameter/state handling.
- **Compiled a real Linux VST3** (`ClarityWidth.vst3`) and a **Standalone**
  app from this exact source tree, with JUCE 8.0.6 cloned fresh and all
  Linux plugin build dependencies installed.
- **Passed `pluginval` at strictness level 10** (the maximum), including the
  fuzz-parameters, thread-safety, non-releasing-audio-processing, and
  state-restoration tests - see §8.
- **Wrote and ran a headless numerical test suite** (`Tests/main.cpp`, 20
  checks) that exercises the actual DSP behaviour (not just "does it
  compile") - mono collapse, low-frequency protection, dynamic vs. static
  EQ behaviour, filter stability, multi-sample-rate/buffer-size sweeps,
  mono-channel handling, and a CPU benchmark. All 20 pass. A genuine bug
  (an unbounded even-harmonic term in the exciter, and a missing
  channel-count guard on the oversampler for mono tracks) was **found and
  fixed** during this process - see §8 for the details, on the record
  rather than glossed over.

**Not done in this environment, and why:**
- **Windows and macOS builds / AU build.** This container is Linux-only
  (no Xcode, no MSVC, no macOS toolchain available). The CMakeLists.txt
  is written to produce AU automatically on macOS (`if (APPLE)` block) and
  VST3 + Standalone everywhere else, but it has only actually been *built
  and run* on Linux here. §2 gives exact steps for Windows/macOS.
- **Interactive GUI testing.** There's no display in this environment, so
  the editor (knob layout, resizing, double-click-to-reset, mouse-wheel)
  compiles and links but hasn't been visually driven. It is ordinary JUCE
  `Slider`/`ToggleButton`/`ComboBox` usage, so the risk here is layout
  aesthetics, not functional correctness.
- **The Steinberg/JUCE built-in VST3 "moduleinfotool" validator** and AU's
  `auval` weren't run standalone (pluginval calls into `auval` on macOS
  automatically, but there's no macOS host here to exercise that path;
  the VST3-side pluginval tests above did run and pass).

---

## 2. Building it yourself

```bash
# 1. Get the source and JUCE (JUCE is not vendored into this delivery -
#    clone it once, next to or inside the project):
git clone https://github.com/juce-framework/JUCE.git --branch 8.0.6 --depth 1
#    (place the JUCE folder inside ClarityWidth/, or pass -DJUCE_PATH=... below)

# 2. Linux dependencies (Debian/Ubuntu - adjust for your distro):
sudo apt-get install -y cmake libasound2-dev libjack-jackd2-dev \
    libcurl4-openssl-dev libfreetype6-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev \
    libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev pkg-config

# 3. Configure and build:
cd ClarityWidth
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target ClarityWidth_VST3 -j$(nproc)
cmake --build . --target ClarityWidth_Standalone -j$(nproc)

# Output:
#   build/ClarityWidth_artefacts/Release/VST3/ClarityWidth.vst3
#   build/ClarityWidth_artefacts/Release/Standalone/ClarityWidth
```

**Windows:** same steps with Visual Studio 2022 (`cmake -G "Visual Studio 17
2022"`), no extra system packages needed beyond the VS "Desktop development
with C++" workload.

**macOS:** same steps with Xcode installed (`cmake -G Xcode`); the
CMakeLists.txt automatically adds `AU` to the `FORMATS` list when
`APPLE` is true, so `ClarityWidth_AU` becomes an available build target
in addition to `ClarityWidth_VST3`.

**Running the DSP test suite:**
```bash
cmake --build . --target ClarityWidthTests -j$(nproc)
./ClarityWidthTests
```

**Validating with pluginval** (optional, what §8 used):
```bash
curl -L "https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Linux.zip" -o pluginval.zip
unzip pluginval.zip
./pluginval --strictness-level 10 --validate-in-process --skip-gui-tests \
    --validate "build/ClarityWidth_artefacts/Release/VST3/ClarityWidth.vst3"
```

---

## 3. Signal flow

```
INPUT (gain trim, -12..+12 dB)
   |
   v
Mud Detection / Dynamic Mud Reduction   (DSP/MudProcessor.h)
   |
   v
Enhancement / Harmonic Processing        (DSP/Enhancer.h)
   |
   v
Stereo Width Processing                  (DSP/StereoWidth.h)
   |
   v
Auto-Gain compensation (optional)
   |
   v
OUTPUT (gain trim, -12..+12 dB)
   |
   v
Global wet/dry Mix, then Bypass
```

This order (mud cleanup first, then add harmonic content, then widen) was
chosen over the alternatives tried during development because widening
*after* the harmonic exciter spreads the newly-added high-frequency content
across the stereo field too, rather than leaving it stuck dead-centre; and
cleaning the low-mids *before* exciting means the exciter's transient
detector isn't reacting to mud that's about to be removed anyway.

---

## 4. The Mud Cut algorithm

**File:** `Source/DSP/MudProcessor.h`

Not "always cut 300 Hz." It's a small sidechain compressor aimed at one
frequency region:

1. A 2nd-order Butterworth **bandpass detector** (default centre 330 Hz,
   Q 0.9, covering the requested 180-550 Hz zone; both centre and Q are
   exposed in Advanced) extracts the candidate "mud band" from the signal.
2. Two envelope followers run in parallel: a fast one (~15 ms) on the
   band-passed signal, and a slower one (~120 ms) on the full-band signal.
3. The ratio of band energy to full-band energy, in dB, is compared to a
   **relative** threshold (not an absolute level) - this is what makes it
   "level-conscious": a quiet passage and a loud passage that have the
   *same spectral balance* get treated the same way.
4. Only the *excess* above that threshold drives gain reduction, through a
   soft quadratic knee over the first 6 dB of excess, capped at
   `Mud Max Reduction` (default 6 dB, range 1-12 dB in Advanced).
5. That smoothed reduction amount becomes the depth of a **broad musical
   bell EQ** (Q 0.7, deliberately not surgical) at the same frequency,
   recomputed only when the reduction has moved enough to matter (avoids
   per-sample coefficient churn).

Result, verified in `Tests/main.cpp`: white/pink noise with no unusual
low-mid content gets under 1.5 dB of reduction even at 100% Mud Cut; the
same noise with an artificial 300 Hz resonance added gets 6 dB (the
configured ceiling). A vocal, guitar, snare or piano that isn't muddy is
left alone; one that is gets cleaned up.

**Known simplification (see §11):** the detection frequency is currently
fixed (tunable in Advanced, but not automatically tracking) rather than
scanning 180-550 Hz for the worst offender. V2 should make this adaptive.

---

## 5. The Width algorithm

**File:** `Source/DSP/StereoWidth.h`

Classic Mid/Side, made mono-safe with three extra mechanisms:

- `M = (L+R)*0.5`, `S = (L-R)*0.5`; reconstruction is `L = M+S`, `R = M-S`.
- The Side signal is split at a low crossover (default 150 Hz, "Width
  Low-Frequency Protect" in Advanced, 60-400 Hz range) into low/high bands.
  The **WIDTH knob's gain (0-200%) is only ever applied to the high band**.
  The low band's side gain is clamped to never exceed its *natural* (100%)
  level, so bass/kick/low toms stay centred no matter how far WIDTH is
  pushed - this is the direct implementation of "protect low frequencies."
- A running phase-correlation estimate is a **safety limiter**: if
  correlation drifts below -0.2 (heading toward cancellation), the side
  gain is automatically and smoothly scaled back.
- For **effectively-mono sources** (correlation near +1, natural side
  energy near zero), plain M/S has nothing to widen. Rather than a
  Haas/polarity trick (explicitly avoided - it's exactly what collapses
  badly in mono), a small frequency-dependent **all-pass decorrelation
  network** (3 cascaded first-order allpass stages, different centre
  frequencies per channel) is blended in above 100% width. All-pass
  stages only shift phase, never magnitude, so a mono sum of the result
  keeps the full frequency content rather than partially cancelling it.

Result, verified in `Tests/main.cpp`: at 200% width, summing a processed
stereo signal to mono loses less than 6 dB versus the un-widened mono
reference (not "disappears or becomes heavily comb-filtered"); an 80 Hz
signal that's pure Side content is *not* amplified beyond its natural
level even at 200% width (ratio ~0.81, i.e. it's actually slightly
attenuated by the safety mechanisms, never boosted).

**Known simplification (see §11):** the low/high split uses a single
Butterworth stage rather than a matched Linkwitz-Riley pair, so
reconstruction isn't perfectly flat through the crossover (a few tenths of
a dB). Inaudible in practice but worth tightening in V2.

---

## 6. The Enhance algorithm

**File:** `Source/DSP/Enhancer.h`

Three cooperating, independently-scaled mechanisms - not a treble boost:

1. **Band-limited harmonic exciter.** The input is high-passed (default
   1800 Hz, "Enhancer Character" in Advanced, 600-6000 Hz range), run
   through a gently asymmetric `tanh`-based soft-clip waveshaper (odd
   harmonics from `tanh`, plus a small, fully-bounded even-harmonic term
   for a "tube-ish" character), then mixed back underneath the dry signal.
   Only the high band is touched, so low end stays clean. This nonlinear
   stage runs at **2x oversampling** (`juce::dsp::Oversampling`, half-band
   polyphase IIR, low latency) specifically so aliasing doesn't sneak into
   the audible band even at high Enhance settings.
2. **Transient/presence lift.** A fast (~4 ms) / slow (~180 ms) envelope
   pair detects transient onsets; the differential drives the gain of a
   broad presence bell (~3.5 kHz, up to +4 dB scaled by Enhance) briefly on
   attacks, adding front-of-mix definition without permanently boosting a
   frequency band (so sustained content doesn't just get louder).
3. **Level-conscious gain matching.** RMS is measured before and after the
   nonlinearity on just the excited band, and the wet contribution is
   scaled to match, so the mechanism doesn't simply win an A/B comparison
   by being louder.
4. **Hard safety ceiling.** The final output of the stage is limited to
   [-8, +8] as a last-resort guard - inaudible in any normal use (typical
   peaks measured well under 2.0 in testing) but guarantees the module
   cannot mathematically diverge (see §8 for why this was added).

Result, verified in `Tests/main.cpp`: 100% Enhance on hot (0.8 amplitude)
noise stays finite, doesn't balloon overall RMS by more than 3 dB, and
peak stays under 4.0. 0% Enhance is a bit-exact bypass.

**Known simplification (see §11):** the presence-lift detector runs on a
simple full-wave-rectified envelope rather than a proper transient/attack
detector with hold time; it's musically fine but could false-trigger on
sustained tremolo-like material. V2 candidate for improvement.

---

## 7. Parameters

| Parameter | Range | Default | Notes |
|---|---|---|---|
| **Enhance** | 0-100% | 20% | Main knob |
| **Width** | 0-200% | 100% | Main knob. 100% = natural/original stereo |
| **Mud Cut** | 0-100% | 20% | Main knob |
| Input | -12..+12 dB | 0 dB | Pre-chain trim |
| Output | -12..+12 dB | 0 dB | Post-chain trim |
| Mix | 0-100% | 100% | Global wet/dry |
| Bypass | on/off | off | True processing bypass |
| Auto Gain | on/off | on | RMS-matches pre/post chain level |
| Mud Frequency (Adv.) | 180-550 Hz | 330 Hz | Detector/correction centre |
| Mud Q (Adv.) | 0.3-2.0 | 0.9 | Detector bandwidth |
| Mud Max Reduction (Adv.) | 1-12 dB | 6 dB | Ceiling on Mud Cut's effect |
| Mud Detector Sensitivity (Adv.) | 0.1-3.0 | 1.0 | Lower = needs more excess to react |
| Width Low-Frequency Protect (Adv.) | 60-400 Hz | 150 Hz | M/S side crossover |
| Enhancer Character (Adv.) | 600-6000 Hz | 1800 Hz | Exciter high-pass point |
| Oversampling (Adv.) | on/off | on | Reserved for future bypass-oversampling option |

All parameters are `AudioProcessorValueTreeState`-backed `RangedAudioParameter`s,
so DAW automation, undo, and state save/restore all work through JUCE's
standard mechanism (verified by pluginval's Automation, Automatable
Parameters, Plugin state, and Plugin state restoration test groups - all
passed at strictness 10).

Presets (15, listed in `PluginProcessor.cpp`): Default, Lead Vocal,
Backing Vocal, Acoustic Guitar, Electric Guitar, Piano, Synth, Pad, Drums,
Percussion, Bass, Strings, Gentle Polish, Wide, Cleanup. All deliberately
non-extreme, as starting points only.

---

## 8. Test results

### 8.1 Headless numerical test suite (`Tests/main.cpp`)

20/20 checks passed on the final build, including:
- Mud Cut: minimal reduction on flat noise, real reduction (up to the 6 dB
  ceiling) on artificially muddy noise, exact bypass at 0%.
- Width: mono-collapse safety at 200% (<6 dB loss vs. reference), 80 Hz
  low-frequency content not amplified past natural level at 200%, impulse
  response stays finite and decays (filter stability).
- Enhancer: finite and RMS-bounded at 100% on hot signal, exact bypass at 0%.
- All three modules stay finite across every combination of sample rate in
  {44.1k, 48k, 88.2k, 96k, 192k} kHz and buffer size in
  {32, 64, 128, 256, 512, 1024, 2048} samples.
- Mono (1-channel) input handled without crashing.
- CPU benchmark (see §10) and an adversarial 2000-iteration self-feedback
  stress test, both finite and passing after the fixes described below.

### 8.2 Bugs actually found and fixed during this process

1. **Wrong `IIR::Filter::processSample` signature.** Initial code called
   it as `processSample(channel, sample)`; the real signature takes just
   the sample (each channel already has its own `Filter` instance). Caught
   immediately by the compiler on first build - fixed in `MudProcessor.h`,
   `StereoWidth.h`, `Enhancer.h`.
2. **Segfault on mono tracks.** The exciter's oversampling scratch buffer
   was always allocated with 2 channels; on a mono bus, the resulting
   `AudioBlock` still reported 2 channels even though only channel 0 held
   data, which mismatched the oversampler's `numChannels` (prepared as 1)
   and crashed inside `juce::dsp::Oversampling::processSamplesUp`. Found by
   the mono-input test, fixed by restricting the scratch block to the
   actual channel count with `getSubsetChannelBlock` before oversampling.
3. **Unbounded even-harmonic term / no ceiling on resonant buildup.** An
   adversarial stress test (2000 sequential passes feeding each block's
   own output back in as the next block's input - deliberately unrealistic
   compared to real playback, but exactly the kind of pathological case
   the spec asks to guard against) diverged to non-finite values. Root
   cause was partly a saturator term that grew with raw sample value
   rather than with the already-bounded `tanh` output, and partly the
   presence-bell's resonant gain having no absolute ceiling across many
   sequential blocks. Fixed by rewriting the even-harmonic term in terms
   of `tanh(x)` (inherently bounded) and adding the hard [-8, +8] safety
   ceiling described in §6. Re-ran the same stress test after the fix:
   passes.

### 8.3 `pluginval` (Tracktion's official VST3/AU conformance & stress tool)

Ran the actual compiled `ClarityWidth.vst3` through `pluginval` v1.0.4 at
**strictness level 10** (the maximum), `--validate-in-process
--skip-gui-tests` (no display in this environment):

```
SUCCESS
```

Test groups exercised and passed: plugin info, plugin programs (all 15
preset names), audio processing across {44.1k, 48k, 96k} kHz x {64, 128,
256, 512, 1024} sample buffers, non-releasing audio processing (sample
rate/buffer size changes without a `releaseResources` call in between),
plugin state and state restoration, automation and sub-block automation,
automatable parameters, parameter thread safety, bus enable/disable, and
**fuzz parameters** (randomised parameter values across the whole range).

### 8.4 What wasn't run

`auval` (macOS-only, no macOS host here) and the standalone Steinberg VST3
validator binary (not downloaded/built in this pass; pluginval's own VST3
conformance tests did run as part of the suite above). Recommended before
shipping: run both on the actual target platforms.

---

## 9. Research approach

Before writing any DSP, current documentation and practice was reviewed
on: JUCE 8 `AudioProcessorValueTreeState` parameter conventions and
`AudioParameterFloatAttributes`; `juce::dsp::Oversampling` usage patterns
for non-linear stages; mono-compatible stereo widening technique
(Linkwitz-Riley-style low crossover keeping bass centred, all-pass
decorrelation as the mono-safe alternative to Haas/polarity tricks); and
current CMake (`juce_add_plugin`) plugin project setup. Sources were
primarily JUCE's own documentation/forum, and current mixing-engineering
writeups on mono-compatibility and stereo-widening technique. No existing
commercial plugin's DSP was copied; the three algorithms here are original
implementations of the techniques described in those sources.

---

## 10. CPU / performance

Measured on this environment's single CPU core (Release build, `-O3` +
LTO, GCC 13.3), full chain (Mud + Enhance + Width) on a 512-sample /
48 kHz block, 2000-iteration average:

```
CPU load: ~0.4-0.7% of one core per instance
Naive single-core headroom: ~150-240 simultaneous instances
```

This is comfortably inside the "20-50 tracks simultaneously" target with
large headroom to spare, even on a single core - multi-core hosts (i.e.
every real DAW) will do better still. No allocation happens inside
`processBlock` (all buffers sized in `prepareToPlay`), no locks are taken,
and `juce::ScopedNoDenormals` guards the whole block.

---

## 11. Known limitations & suggested V2 improvements

1. **Mud detection frequency is fixed, not adaptive.** It's tunable in
   Advanced but doesn't automatically scan 180-550 Hz for the worst
   offender per-instrument. V2: a small filter bank (3-4 bands) with the
   detector picking whichever shows the most excess.
2. **Width crossover uses a single Butterworth split**, not a matched
   Linkwitz-Riley pair, so reconstruction through the crossover isn't
   perfectly flat (small, inaudible ripple). V2: proper LR4 M/S crossover.
3. **Presence-lift transient detector is a simple fast/slow envelope
   differential**, not a dedicated attack/hold transient designer. V2:
   proper attack/sustain separation to avoid any false-triggering on
   tremolo/vibrato-heavy sustained material.
4. **No linear-phase/latency-free oversampling option.** Current
   oversampling is a low-latency IIR half-band design (chosen deliberately
   for the "many instances at once" CPU/latency requirement); a V2 could
   offer a linear-phase FIR alternative behind the "Oversampling" Advanced
   toggle for mastering-adjacent use where phase linearity matters more
   than CPU/latency.
5. **AU and Windows/macOS builds are configured but not yet build-verified**
   in this delivery (Linux-only sandbox) - see §1 and §2.
6. **GUI has not been visually/interactively tested** - no display in this
   environment. Functionally wired correctly (verified: parameters attach,
   presets populate, bypass/auto-gain toggle correctly), but knob
   layout/spacing/resize behavior should get a first real look before
   shipping.
7. **Double-precision (`processBlock(AudioBuffer<double>&, ...)`) is not
   implemented** - `SupportsDoublePrecision: no` was confirmed by
   pluginval's plugin-info test. Only float32 processing is supported.

---

## 12. Source layout

```
ClarityWidth/
  CMakeLists.txt
  README.md                  <- this file
  Source/
    PluginProcessor.h/.cpp    Parameters, state, signal-flow orchestration
    PluginEditor.h/.cpp       Three main knobs, small controls, meters, presets, Advanced panel
    DSP/
      MudProcessor.h          Dynamic low-mid suppressor (§4)
      StereoWidth.h           Mono-safe M/S widener (§5)
      Enhancer.h               Harmonic exciter + presence lift (§6)
      Metering.h               Lightweight peak-hold level follower
  Tests/
    main.cpp                  Headless numerical test suite (§8.1)
```
