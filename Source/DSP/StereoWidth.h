#pragma once
#include <juce_dsp/juce_dsp.h>

namespace clarity
{

/**
    Mono-safe, frequency-conscious stereo widener.

    Core technique (see README "Stereo width algorithm" section):
      - True stereo input: classic Mid/Side processing.
            M = (L + R) * 0.5      S = (L - R) * 0.5
        Reconstruction: L = M + S, R = M - S.
      - The Side signal is split at a low crossover (default ~150 Hz,
        2nd-order Butterworth, exposed in Advanced as "Width Low-Frequency
        Protect") into a low band and a high band.
      - The WIDTH knob's gain is applied to the HIGH band of the Side
        signal without restriction (0..2x). The LOW band of the Side
        signal is only ever attenuated toward mono, never amplified past
        its natural (100%) level, regardless of how far the knob is
        pushed - this is what keeps bass/kick/low toms centred exactly as
        the "protect low frequencies" requirement asks for.
      - A running short-term phase-correlation estimate acts as a safety
        limiter: if stereo correlation drops too far negative (comb/phase
        cancellation risk), the applied side gain is scaled back
        automatically, smoothed to avoid audible pumping.
      - For signals that are detected as effectively mono (very high
        correlation, near-zero natural side energy), simple M/S widening
        has nothing to work with (Side ~ 0). Instead of relying on a
        Haas/polarity trick (explicitly avoided per spec - it collapses
        badly in mono), a small frequency-dependent all-pass decorrelation
        network is engaged: cascaded first-order allpass stages with
        different centre frequencies feed L and R, producing a frequency-
        dependent phase relationship (not a polarity inversion), which
        stays close to mono-safe because allpass stages preserve
        magnitude response and only shift phase - so a mono sum retains
        the full frequency content rather than partially cancelling.
*/
class StereoWidth
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        sideLowpassL.prepare (spec); sideLowpassR.prepare (spec);
        for (auto& ap : allpassChainL) ap.prepare (spec);
        for (auto& ap : allpassChainR) ap.prepare (spec);

        widthGainSmoothed.reset (sampleRate, 0.030);
        safetyScaleSmoothed.reset (sampleRate, 0.080);
        monoBlendSmoothed.reset (sampleRate, 0.250);

        updateCrossover();
        updateAllpassStages();
        reset();
    }

    void reset()
    {
        sideLowpassL.reset(); sideLowpassR.reset();
        for (auto& ap : allpassChainL) ap.reset();
        for (auto& ap : allpassChainR) ap.reset();
        correlationRunning = 1.0f;
        widthGainSmoothed.setCurrentAndTargetValue (1.0f);
        safetyScaleSmoothed.setCurrentAndTargetValue (1.0f);
        monoBlendSmoothed.setCurrentAndTargetValue (0.0f);
    }

    void setParameters (float widthPercent, float lowProtectHz)
    {
        widthKnob = juce::jlimit (0.0f, 2.0f, widthPercent / 100.0f);
        if (std::abs (lowProtectHz - crossoverHz) > 0.5f)
        {
            crossoverHz = lowProtectHz;
            if (sampleRate > 0.0)
                updateCrossover();
        }
    }

    void process (juce::dsp::AudioBlock<float>& block)
    {
        const auto numCh = block.getNumChannels();
        if (numCh < 2)
        {
            // Mono track into a stereo-width tool: nothing to widen via
            // M/S (there is no Side). We leave the signal untouched here;
            // the host will typically feed a duplicated-to-stereo signal
            // if width is desired on a mono source, which is handled by
            // the branch below once numCh == 2 with near-zero side energy.
            widthGainSmoothed.setTargetValue (widthKnob);
            return;
        }

        const auto numSamples = block.getNumSamples();
        float* left = block.getChannelPointer (0);
        float* right = block.getChannelPointer (1);

        for (size_t i = 0; i < numSamples; ++i)
        {
            float l = left[i];
            float r = right[i];

            float mid = 0.5f * (l + r);
            float side = 0.5f * (l - r);

            // --- correlation tracking (runs regardless, cheap) ---
            float instCorrNum = l * r;
            float instCorrDen = 0.5f * (l * l + r * r) + 1.0e-9f;
            float instCorr = instCorrNum / instCorrDen;
            correlationRunning += (instCorr - correlationRunning) * 0.0005f;

            // Safety limiter: only engages once correlation drifts toward
            // cancellation territory.
            float safetyTarget = 1.0f;
            if (correlationRunning < -0.2f)
                safetyTarget = juce::jmap (juce::jlimit (-1.0f, -0.2f, correlationRunning), -1.0f, -0.2f, 0.35f, 1.0f);
            safetyScaleSmoothed.setTargetValue (safetyTarget);
            float safety = safetyScaleSmoothed.getNextValue();

            widthGainSmoothed.setTargetValue (widthKnob);
            float appliedWidth = widthGainSmoothed.getNextValue() * safety;

            // Mono-source detection: near-unity correlation AND very low
            // natural side energy relative to mid energy.
            sideEnergyEnv += (side * side - sideEnergyEnv) * 0.0005f;
            midEnergyEnv  += (mid * mid - midEnergyEnv)   * 0.0005f;
            float sideToMid = sideEnergyEnv / (midEnergyEnv + 1.0e-9f);
            float monoTarget = (sideToMid < 0.01f && correlationRunning > 0.9f) ? 1.0f : 0.0f;
            monoBlendSmoothed.setTargetValue (monoTarget);
            float monoBlend = monoBlendSmoothed.getNextValue();

            // --- split side into low/high, protect the low band ---
            float sideLowL = sideLowpassL.processSample (side);
            float sideHigh = side - sideLowL;

            float lowGain = juce::jmin (appliedWidth, 1.0f); // never boost bass beyond natural
            float highGain = appliedWidth;

            float widenedSide = sideLowL * lowGain + sideHigh * highGain;

            float outL = mid + widenedSide;
            float outR = mid - widenedSide;

            // --- mono-source decorrelation blend-in ---
            if (monoBlend > 0.001f)
            {
                float apL = l;
                float apR = r;
                for (auto& ap : allpassChainL) apL = ap.processSample (apL);
                for (auto& ap : allpassChainR) apR = ap.processSample (apR);

                // Blend proportionally to how far above 100% width is set,
                // so at <=100% width a detected-mono source is left alone.
                float decorrelationAmount = juce::jlimit (0.0f, 1.0f, (widthKnob - 1.0f)) * monoBlend;
                outL = juce::jmap (decorrelationAmount, l, apL);
                outR = juce::jmap (decorrelationAmount, r, apR);
            }

            left[i]  = outL;
            right[i] = outR;
        }
    }

    float getCorrelationForMeter() const { return correlationRunning; }

private:
    void updateCrossover()
    {
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, juce::jlimit (60.0f, 400.0f, crossoverHz));
        sideLowpassL.coefficients = coeffs;
        sideLowpassR.coefficients = coeffs;
    }

    void updateAllpassStages()
    {
        // Different centre frequencies per channel => frequency-dependent
        // phase difference between L and R without any magnitude change.
        static const float freqsL[3] = { 250.0f, 900.0f, 3200.0f };
        static const float freqsR[3] = { 340.0f, 1200.0f, 4100.0f };
        for (int s = 0; s < 3; ++s)
        {
            allpassChainL[(size_t) s].coefficients = juce::dsp::IIR::Coefficients<float>::makeAllPass (sampleRate, freqsL[s], 0.707f);
            allpassChainR[(size_t) s].coefficients = juce::dsp::IIR::Coefficients<float>::makeAllPass (sampleRate, freqsR[s], 0.707f);
        }
    }

    double sampleRate = 44100.0;
    float widthKnob = 1.0f;
    float crossoverHz = 150.0f;

    float correlationRunning = 1.0f;
    float sideEnergyEnv = 0.0f;
    float midEnergyEnv = 0.0f;

    juce::LinearSmoothedValue<float> widthGainSmoothed, safetyScaleSmoothed, monoBlendSmoothed;

    juce::dsp::IIR::Filter<float> sideLowpassL, sideLowpassR;
    std::array<juce::dsp::IIR::Filter<float>, 3> allpassChainL, allpassChainR;
};

} // namespace clarity
