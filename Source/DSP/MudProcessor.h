#pragma once
#include <juce_dsp/juce_dsp.h>

namespace clarity
{

/**
    Dynamic low-mid ("mud") suppressor.

    Rationale (see README "Mud Cut algorithm" section for the full writeup):
    Muddy low-mids are not a fixed frequency problem - the offending energy
    sits somewhere around 180-550 Hz depending on the instrument, and it is
    usually only a problem when it becomes *excessive relative to the rest
    of the signal*, not all the time. So this is built as a sidechain
    detector + dynamic bell gain, not a static EQ cut:

      1. A band-limited detector (2nd-order Butterworth bandpass, tunable
         centre/Q, default ~330 Hz covering the 180-550 Hz zone) extracts
         the candidate "mud band" energy via an RMS envelope follower.
      2. A second, wideband envelope follower tracks overall signal energy.
      3. We compute how much the mud band exceeds a *level-relative*
         threshold (band energy vs. overall energy ratio) rather than an
         absolute dB threshold, so quiet passages and loud passages are
         treated consistently ("level-conscious").
      4. The excess drives a soft-knee gain-reduction curve, capped at
         maxReductionDb (default 6 dB), which is smoothed to avoid clicks.
      5. That smoothed reduction amount is applied as the depth of a
         broad musical bell EQ (Q ~0.7, "musical" rather than surgical)
         centred at the same detector frequency, recomputed at block rate.

    This means: a track that never has excessive low-mid buildup receives
    close to 0 dB of reduction even at 100% Mud Cut, while a track that
    genuinely honks in the low-mids gets progressively cleaned up. This is
    the "intelligent cleanup, not a fixed cut" behaviour requested.
*/
class MudProcessor
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        for (auto* f : { &detectorBandpassL, &detectorBandpassR })
            f->prepare (spec);

        for (auto* f : { &correctionBellL, &correctionBellR })
            f->prepare (spec);

        reductionSmoothed.reset (sampleRate, 0.030);

        // One-pole envelope-follower coefficients: coeff = 1 - exp(-1 / (tau * fs))
        bandEnvCoeff = 1.0f - std::exp (-1.0f / (float) (0.015 * sampleRate));
        wideEnvCoeff = 1.0f - std::exp (-1.0f / (float) (0.120 * sampleRate));

        updateDetectorCoefficients();
        updateCorrectionCoefficients (0.0f);
        reset();
    }

    void reset()
    {
        for (auto* f : { &detectorBandpassL, &detectorBandpassR, &correctionBellL, &correctionBellR })
            f->reset();
        bandEnvVal = 0.0f;
        wideEnvVal = 0.0f;
        currentReductionDb = 0.0f;
        reductionSmoothed.setCurrentAndTargetValue (0.0f);
    }

    // Parameters, updated once per block from the APVTS.
    void setParameters (float amount01, float frequencyHz, float q, float maxReductionDbIn, float sensitivity01)
    {
        amount = juce::jlimit (0.0f, 1.0f, amount01);
        maxReductionDb = maxReductionDbIn;
        sensitivity = juce::jlimit (0.1f, 3.0f, sensitivity01);

        bool needsCoeffUpdate = std::abs (frequencyHz - centreFreq) > 0.5f || std::abs (q - detectorQ) > 0.001f;
        centreFreq = frequencyHz;
        detectorQ = q;
        if (needsCoeffUpdate && sampleRate > 0.0)
            updateDetectorCoefficients();
    }

    // Processes a stereo block already prepared. Call once per processBlock.
    void process (juce::dsp::AudioBlock<float>& block)
    {
        if (amount <= 0.0001f || block.getNumChannels() == 0)
        {
            currentGainReductionDbForMeter = 0.0f;
            return;
        }

        const auto numCh = block.getNumChannels();
        const auto numSamples = block.getNumSamples();

        // --- Detection pass (uses channel 0, or mono-sum for stereo) ---
        for (size_t i = 0; i < numSamples; ++i)
        {
            float wide = 0.0f;
            float bandSample = 0.0f;

            for (size_t ch = 0; ch < numCh; ++ch)
            {
                float x = block.getSample ((int) ch, (int) i);
                wide += x;
                float b = (ch == 0 ? detectorBandpassL.processSample (x)
                                    : detectorBandpassR.processSample (x));
                bandSample += b;
            }
            wide /= (float) numCh;
            bandSample /= (float) numCh;

            float bandRect = bandSample * bandSample;
            float wideRect = wide * wide;

            bandEnvVal += (bandRect - bandEnvVal) * bandEnvCoeff;
            wideEnvVal += (wideRect - wideEnvVal) * wideEnvCoeff;

            float bandRms = std::sqrt (juce::jmax (bandEnvVal, 1.0e-12f));
            float wideRms = std::sqrt (juce::jmax (wideEnvVal, 1.0e-12f));

            // Ratio of mud-band energy to overall energy, in dB. A perfectly
            // "flat" broadband signal would show a ratio near the band's
            // proportion of the spectrum; we care about signals that sit
            // well above their own average behaviour, so we compare band
            // level against a running reference and only react to excess.
            float ratioDb = 20.0f * std::log10 (bandRms / wideRms + 1.0e-9f);

            // Threshold is level-conscious (relative, not absolute) and
            // moves with the sensitivity control: lower sensitivity needs
            // more excess before reacting.
            float thresholdDb = juce::jmap (sensitivity, 0.1f, 3.0f, -3.0f, -14.0f);
            float excessDb = ratioDb - thresholdDb;

            float targetReductionDb = 0.0f;
            if (excessDb > 0.0f)
            {
                // Soft knee: quadratic for the first 6 dB of excess, then
                // approaches the ceiling asymptotically so it never cuts
                // suddenly or removes the fundamental body entirely.
                float knee = juce::jlimit (0.0f, 1.0f, excessDb / 6.0f);
                targetReductionDb = knee * knee * maxReductionDb * amount;
            }

            reductionSmoothed.setTargetValue (targetReductionDb);
            currentReductionDb = reductionSmoothed.getNextValue();

            // Recompute the correction filter only when the reduction has
            // moved enough to matter (avoids per-sample coefficient churn).
            if (std::abs (currentReductionDb - lastAppliedReductionDb) > 0.05f)
            {
                updateCorrectionCoefficients (-currentReductionDb);
                lastAppliedReductionDb = currentReductionDb;
            }

            for (size_t ch = 0; ch < numCh; ++ch)
            {
                float x = block.getSample ((int) ch, (int) i);
                float y = (ch == 0 ? correctionBellL.processSample (x)
                                    : correctionBellR.processSample (x));
                block.setSample ((int) ch, (int) i, y);
            }
        }

        currentGainReductionDbForMeter = currentReductionDb;
    }

    float getGainReductionDbForMeter() const { return currentGainReductionDbForMeter; }

private:
    void updateDetectorCoefficients()
    {
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate,
                                                                           juce::jlimit (80.0f, 2000.0f, centreFreq),
                                                                           juce::jlimit (0.3f, 2.0f, detectorQ));
        detectorBandpassL.coefficients = coeffs;
        detectorBandpassR.coefficients = coeffs;
    }

    void updateCorrectionCoefficients (float gainDb)
    {
        // Broad musical bell: Q ~0.7 keeps it far from a surgical notch.
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
            sampleRate, juce::jlimit (80.0f, 2000.0f, centreFreq), 0.7f,
            juce::Decibels::decibelsToGain (gainDb));
        correctionBellL.coefficients = coeffs;
        correctionBellR.coefficients = coeffs;
    }

    double sampleRate = 44100.0;
    float amount = 0.0f;
    float centreFreq = 330.0f;
    float detectorQ = 0.9f;
    float maxReductionDb = 6.0f;
    float sensitivity = 1.0f;

    float bandEnvVal = 0.0f;
    float wideEnvVal = 0.0f;
    float bandEnvCoeff = 0.1f;
    float wideEnvCoeff = 0.02f;
    float currentReductionDb = 0.0f;
    float lastAppliedReductionDb = -1.0f;
    float currentGainReductionDbForMeter = 0.0f;

    juce::LinearSmoothedValue<float> reductionSmoothed;

    juce::dsp::IIR::Filter<float> detectorBandpassL, detectorBandpassR;
    juce::dsp::IIR::Filter<float> correctionBellL, correctionBellR;
};

} // namespace clarity
