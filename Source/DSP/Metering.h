#pragma once
#include <juce_dsp/juce_dsp.h>

namespace clarity
{

/** Simple peak-hold level follower used for the Input/Output meters. Cheap:
    no FFT, just a per-block peak scan with exponential decay ballistics,
    read by the GUI via getLevel() at the UI refresh rate. */
class PeakLevelFollower
{
public:
    void prepare (double sampleRate)
    {
        decayCoeff = std::exp (-1.0 / (sampleRate * 0.30)); // ~300ms release
    }

    void pushBlock (const juce::dsp::AudioBlock<float>& block)
    {
        float peak = 0.0f;
        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            peak = juce::jmax (peak, block.getChannelPointer (ch) != nullptr
                                          ? juce::FloatVectorOperations::findMaximum (block.getChannelPointer (ch), (int) block.getNumSamples())
                                          : 0.0f);

        float current = level.load (std::memory_order_relaxed);
        float next = peak > current ? peak : current * (float) decayCoeff;
        level.store (next, std::memory_order_relaxed);
    }

    float getLevel() const { return level.load (std::memory_order_relaxed); }

private:
    std::atomic<float> level { 0.0f };
    double decayCoeff = 0.99;
};

} // namespace clarity
