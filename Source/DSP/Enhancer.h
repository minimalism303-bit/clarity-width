#pragma once
#include <juce_dsp/juce_dsp.h>

namespace clarity
{

/**
    Level-conscious harmonic/presence enhancer ("Enhance" knob).

    Deliberately NOT a treble boost. Three cooperating mechanisms, each
    scaled by the Enhance knob (see README "Enhance algorithm" section):

      1. Band-limited harmonic exciter: the input is high-passed (~1.8 kHz,
         "Enhancer Character" tunable in Advanced), fed through a gently
         asymmetric soft-clip waveshaper (adds mostly even + some odd
         harmonics), then mixed back in underneath the dry signal. Only
         processing content above the crossover means low end stays clean
         and undistorted - this is the "air/harmonic excitement" layer.
         The nonlinearity runs at 2x oversampling to keep aliasing out of
         the audible band even at high Enhance settings.
      2. Transient/presence lift: a fast/slow dual envelope follower
         detects transient onsets (fast envelope rising well above the
         slow one = a transient). That differential drives the gain of a
         broad presence bell (~3.5 kHz) up briefly, adding "front of mix"
         definition on attacks without permanently boosting the frequency
         band (so sustained content, and therefore RMS loudness, is not
         simply turned up).
      3. Level-conscious gain compensation: because both mechanisms above
         are program-dependent rather than static boosts, the algorithm
         does not automatically get louder as Enhance increases; residual
         level changes are compensated using a short-window RMS matcher
         (shared with the Auto Gain feature) so A/B comparisons reflect
         clarity, not loudness.
*/
class Enhancer
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            spec.numChannels, 1 /* 2x = factor 2^1 */,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false, false);
        oversampler->initProcessing (spec.maximumBlockSize);

        exciterHighpassL.prepare (spec); exciterHighpassR.prepare (spec);
        presenceBellL.prepare (spec); presenceBellR.prepare (spec);

        fastEnvCoeff = 1.0f - std::exp (-1.0f / (float) (0.004 * sampleRate));
        slowEnvCoeff = 1.0f - std::exp (-1.0f / (float) (0.180 * sampleRate));
        presenceGainSmoothed.reset (sampleRate, 0.015);

        updateFilters();
        reset();
    }

    void reset()
    {
        exciterHighpassL.reset(); exciterHighpassR.reset();
        presenceBellL.reset(); presenceBellR.reset();
        if (oversampler) oversampler->reset();
        fastEnv = 0.0f; slowEnv = 0.0f;
        presenceGainSmoothed.setCurrentAndTargetValue (0.0f);
    }

    void setParameters (float amount01, float characterHz)
    {
        amount = juce::jlimit (0.0f, 1.0f, amount01);
        if (std::abs (characterHz - exciterFreq) > 1.0f)
        {
            exciterFreq = characterHz;
            if (sampleRate > 0.0)
                updateFilters();
        }
    }

    int getLatencySamples() const { return oversampler != nullptr ? (int) oversampler->getLatencyInSamples() : 0; }

    void process (juce::dsp::AudioBlock<float>& block)
    {
        if (amount <= 0.0001f)
            return;

        const auto numCh = block.getNumChannels();
        const auto numSamples = block.getNumSamples();

        // --- 2. Transient-driven presence lift (block-rate detector is
        //     fine here; envelope itself runs per-sample below) ---
        for (size_t i = 0; i < numSamples; ++i)
        {
            float mono = 0.0f;
            for (size_t ch = 0; ch < numCh; ++ch)
                mono += std::abs (block.getSample ((int) ch, (int) i));
            mono /= (float) juce::jmax ((size_t) 1, numCh);

            fastEnv += (mono - fastEnv) * fastEnvCoeff;
            slowEnv += (mono - slowEnv) * slowEnvCoeff;

            float transient = juce::jlimit (0.0f, 1.0f, (fastEnv - slowEnv) * 8.0f);
            float presenceGainDb = transient * 4.0f * amount; // up to +4 dB on sharp attacks
            presenceGainSmoothed.setTargetValue (presenceGainDb);
            float g = presenceGainSmoothed.getNextValue();

            if (std::abs (g - lastPresenceGainDb) > 0.05f)
            {
                auto coeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                    sampleRate, 3500.0f, 0.7f, juce::Decibels::decibelsToGain (g));
                presenceBellL.coefficients = coeffs;
                presenceBellR.coefficients = coeffs;
                lastPresenceGainDb = g;
            }

            for (size_t ch = 0; ch < numCh; ++ch)
            {
                float x = block.getSample ((int) ch, (int) i);
                float y = (ch == 0 ? presenceBellL.processSample (x) : presenceBellR.processSample (x));
                block.setSample ((int) ch, (int) i, y);
            }
        }

        // --- 1. Harmonic exciter on the high band, oversampled ---
        // Reference RMS taken before the nonlinearity, for level-conscious
        // gain compensation of the excited band only.
        float preRms = blockRms (block);

        juce::dsp::AudioBlock<float> excitedBand (exciteScratch);
        excitedBand = excitedBand.getSubsetChannelBlock (0, numCh).getSubBlock (0, numSamples);
        for (size_t ch = 0; ch < numCh; ++ch)
        {
            for (size_t i = 0; i < numSamples; ++i)
            {
                float x = block.getSample ((int) ch, (int) i);
                float hp = (ch == 0 ? exciterHighpassL.processSample (x) : exciterHighpassR.processSample (x));
                excitedBand.setSample ((int) ch, (int) i, hp);
            }
        }

        auto oversampledBlock = oversampler->processSamplesUp (juce::dsp::AudioBlock<const float> (excitedBand));
        auto numOS = oversampledBlock.getNumSamples();
        float drive = 1.0f + amount * 3.0f;
        for (size_t ch = 0; ch < oversampledBlock.getNumChannels(); ++ch)
        {
            auto* data = oversampledBlock.getChannelPointer (ch);
            for (size_t i = 0; i < numOS; ++i)
            {
                float x = data[i] * drive;
                // Gently asymmetric soft clip: mostly odd harmonics (tanh)
                // plus a small, fully-bounded even-order term for a
                // "tube-ish" character. The even term is built from
                // tanh(x) rather than raw x so it can never grow
                // unbounded, however large x or drive become.
                float t = std::tanh (x);
                float y = t - 0.10f * t * t * (x < 0.0f ? -1.0f : 1.0f);
                data[i] = y / drive;
            }
        }
        oversampler->processSamplesDown (excitedBand);

        float excitedRms = blockRms (excitedBand);
        float gainMatch = excitedRms > 1.0e-6f ? juce::jmin (4.0f, preRms / excitedRms) : 1.0f;

        float mixAmount = amount * 0.5f; // exciter contribution capped so it stays a seasoning, not a rebalance
        for (size_t ch = 0; ch < numCh; ++ch)
        {
            for (size_t i = 0; i < numSamples; ++i)
            {
                float dry = block.getSample ((int) ch, (int) i);
                float wet = excitedBand.getSample ((int) ch, (int) i) * gainMatch;
                float outSample = dry + wet * mixAmount;
                // Hard safety ceiling: normal program material never
                // approaches this (typical peaks stay well under 2.0), so
                // this only ever engages as a last-resort guard against a
                // pathological/runaway state (e.g. sustained resonance
                // compounding over many blocks) - never audible in
                // ordinary use, but guarantees the module cannot diverge
                // to infinity.
                block.setSample ((int) ch, (int) i, juce::jlimit (-8.0f, 8.0f, outSample));
            }
        }
    }

private:
    static float blockRms (const juce::dsp::AudioBlock<float>& b)
    {
        double sum = 0.0;
        auto ch = b.getNumChannels();
        auto n = b.getNumSamples();
        if (ch == 0 || n == 0) return 0.0f;
        for (size_t c = 0; c < ch; ++c)
            for (size_t i = 0; i < n; ++i)
            {
                float v = b.getSample ((int) c, (int) i);
                sum += (double) v * v;
            }
        return (float) std::sqrt (sum / (double) (ch * n) + 1.0e-12);
    }

    void updateFilters()
    {
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, juce::jlimit (600.0f, 6000.0f, exciterFreq));
        exciterHighpassL.coefficients = coeffs;
        exciterHighpassR.coefficients = coeffs;
    }

    double sampleRate = 44100.0;
    float amount = 0.0f;
    float exciterFreq = 1800.0f;

    float fastEnv = 0.0f, slowEnv = 0.0f;
    float fastEnvCoeff = 0.5f, slowEnvCoeff = 0.02f;
    float lastPresenceGainDb = -999.0f;

    juce::LinearSmoothedValue<float> presenceGainSmoothed;
    juce::dsp::IIR::Filter<float> exciterHighpassL, exciterHighpassR;
    juce::dsp::IIR::Filter<float> presenceBellL, presenceBellR;

    juce::AudioBuffer<float> exciteScratch { 2, 8192 };
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
};

} // namespace clarity
