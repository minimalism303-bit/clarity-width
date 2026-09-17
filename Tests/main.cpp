// Technical test harness for the ClarityWidth DSP core. Links directly
// against the header-only DSP classes (no plugin wrapper needed) so it can
// run in CI/headless. Not a substitute for pluginval - see README for that
// step - but this is what actually exercises the algorithms numerically.

#include <juce_dsp/juce_dsp.h>
#include "../Source/DSP/MudProcessor.h"
#include "../Source/DSP/StereoWidth.h"
#include "../Source/DSP/Enhancer.h"
#include <iostream>
#include <random>
#include <chrono>

using namespace clarity;

static int failures = 0;

static void check (bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    if (! condition) ++failures;
}

static juce::AudioBuffer<float> makeSine (double sampleRate, int numSamples, float freq, float amp, bool stereo, float phaseOffsetR = 0.0f)
{
    juce::AudioBuffer<float> buf (stereo ? 2 : 1, numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        float l = amp * std::sin (2.0f * juce::MathConstants<float>::pi * freq * (float) i / (float) sampleRate);
        buf.setSample (0, i, l);
        if (stereo)
        {
            float r = amp * std::sin (2.0f * juce::MathConstants<float>::pi * freq * (float) i / (float) sampleRate + phaseOffsetR);
            buf.setSample (1, i, r);
        }
    }
    return buf;
}

static juce::AudioBuffer<float> makeNoise (int numChannels, int numSamples, float amp, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-amp, amp);
    juce::AudioBuffer<float> buf (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buf.setSample (ch, i, dist (rng));
    return buf;
}

static bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* d = b.getReadPointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (d[i])) return false;
    }
    return true;
}

static float rmsOf (const juce::AudioBuffer<float>& b, int ch)
{
    return b.getRMSLevel (ch, 0, b.getNumSamples());
}

int main()
{
    const double sr = 48000.0;
    const int blockSize = 512;
    const int numBlocks = 40; // ~426ms, enough for envelopes to settle
    const int numSamples = blockSize * numBlocks;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) blockSize, 2 };

    // =========================== MUD PROCESSOR ===========================
    {
        MudProcessor mud;
        mud.prepare (spec);

        // Test 1: pink-ish flat noise should receive very little reduction
        // even at 100% Mud Cut, because there's no *excess* low-mid energy
        // relative to the rest of the spectrum.
        auto flatNoise = makeNoise (2, numSamples, 0.2f, 1234);
        float lastReduction = 0.0f;
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> block (flatNoise.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
            juce::dsp::AudioBlock<float> ab (block);
            mud.setParameters (1.0f, 330.0f, 0.9f, 6.0f, 1.0f);
            mud.process (ab);
            lastReduction = mud.getGainReductionDbForMeter();
        }
        check (allFinite (flatNoise), "MudProcessor: output finite on white noise");
        check (lastReduction < 1.5f, "MudProcessor: flat noise gets minimal reduction (<1.5dB) at 100% - not a blind fixed cut (measured " + std::to_string (lastReduction) + " dB)");
    }
    {
        // Test 2: noise with an artificially boosted 300Hz band should
        // receive *measurable* dynamic reduction.
        MudProcessor mud;
        mud.prepare (spec);
        auto noise = makeNoise (2, numSamples, 0.1f, 5678);
        // Add a strong 300Hz tone on top to simulate a "muddy" resonance.
        for (int i = 0; i < numSamples; ++i)
        {
            float extra = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * (float) i / (float) sr);
            noise.addSample (0, i, extra);
            noise.addSample (1, i, extra);
        }
        float lastReduction = 0.0f;
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> block (noise.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
            juce::dsp::AudioBlock<float> ab (block);
            mud.setParameters (1.0f, 330.0f, 0.9f, 6.0f, 1.0f);
            mud.process (ab);
            lastReduction = mud.getGainReductionDbForMeter();
        }
        check (allFinite (noise), "MudProcessor: output finite with boosted 300Hz content");
        check (lastReduction > 2.0f, "MudProcessor: boosted 300Hz content triggers real reduction (measured " + std::to_string (lastReduction) + " dB, expected >2dB)");
        check (lastReduction <= 6.05f, "MudProcessor: reduction never exceeds configured max (6dB)");
    }
    {
        // Test 3: bypass equivalent (amount = 0) leaves signal untouched.
        MudProcessor mud;
        mud.prepare (spec);
        auto sine = makeSine (sr, blockSize, 300.0f, 0.5f, true);
        auto original = sine;
        juce::dsp::AudioBlock<float> ab (sine);
        mud.setParameters (0.0f, 330.0f, 0.9f, 6.0f, 1.0f);
        mud.process (ab);
        float maxDiff = 0.0f;
        for (int i = 0; i < blockSize; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (sine.getSample (0, i) - original.getSample (0, i)));
        check (maxDiff < 1.0e-6f, "MudProcessor: 0% amount is a true bypass");
    }

    // =========================== STEREO WIDTH ============================
    {
        // Test 4: mono-collapse safety. At 200% width, summing L+R should
        // still leave a musically useful (non-near-zero) result, i.e. the
        // widening must not rely on polarity inversion that cancels badly.
        StereoWidth width;
        width.prepare (spec);
        auto sine = makeSine (sr, numSamples, 1000.0f, 0.4f, true, juce::MathConstants<float>::pi * 0.5f);
        float originalMonoRms = 0.0f;
        {
            juce::AudioBuffer<float> monoRef (1, numSamples);
            for (int i = 0; i < numSamples; ++i)
                monoRef.setSample (0, i, 0.5f * (sine.getSample (0, i) + sine.getSample (1, i)));
            originalMonoRms = rmsOf (monoRef, 0);
        }
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> block (sine.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
            juce::dsp::AudioBlock<float> ab (block);
            width.setParameters (200.0f, 150.0f);
            width.process (ab);
        }
        check (allFinite (sine), "StereoWidth: output finite at 200% width");
        juce::AudioBuffer<float> monoSum (1, numSamples);
        for (int i = 0; i < numSamples; ++i)
            monoSum.setSample (0, i, 0.5f * (sine.getSample (0, i) + sine.getSample (1, i)));
        float summedRms = rmsOf (monoSum, 0);
        float dropDb = juce::Decibels::gainToDecibels (summedRms / juce::jmax (1.0e-9f, originalMonoRms));
        check (dropDb > -6.0f, "StereoWidth: mono-summed 200% width signal doesn't collapse (drop = " + std::to_string (dropDb) + " dB, threshold -6dB)");
    }
    {
        // Test 5: low frequencies stay centred - side energy below the
        // crossover should not be amplified beyond its natural level even
        // at 200% width.
        StereoWidth width;
        width.prepare (spec);
        // 80Hz sine, out of phase between channels -> pure "side" content.
        auto lowSine = makeSine (sr, numSamples, 80.0f, 0.3f, true, juce::MathConstants<float>::pi);
        auto originalSide = lowSine;
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> block (lowSine.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
            juce::dsp::AudioBlock<float> ab (block);
            width.setParameters (200.0f, 150.0f);
            width.process (ab);
        }
        float outSideRms = 0.0f, inSideRms = 0.0f;
        for (int i = numSamples / 2; i < numSamples; ++i) // settled region
        {
            float outSide = 0.5f * (lowSine.getSample (0, i) - lowSine.getSample (1, i));
            float inSide = 0.5f * (originalSide.getSample (0, i) - originalSide.getSample (1, i));
            outSideRms += outSide * outSide;
            inSideRms += inSide * inSide;
        }
        outSideRms = std::sqrt (outSideRms / (float) (numSamples / 2));
        inSideRms = std::sqrt (inSideRms / (float) (numSamples / 2));
        check (outSideRms < inSideRms * 1.1f, "StereoWidth: 80Hz side content is NOT amplified past natural level at 200% width (out/in ratio = " + std::to_string (outSideRms / inSideRms) + ")");
    }
    {
        // Test 6: filter stability with an impulse - output must decay,
        // never grow or produce NaN/Inf.
        StereoWidth width;
        width.prepare (spec);
        juce::AudioBuffer<float> impulse (2, blockSize);
        impulse.clear();
        impulse.setSample (0, 0, 1.0f);
        impulse.setSample (1, 0, -1.0f);
        juce::dsp::AudioBlock<float> ab (impulse);
        width.setParameters (200.0f, 150.0f);
        width.process (ab);
        check (allFinite (impulse), "StereoWidth: impulse response stays finite");
        float tailEnergy = 0.0f;
        for (int i = blockSize / 2; i < blockSize; ++i)
            tailEnergy += impulse.getSample (0, i) * impulse.getSample (0, i);
        check (tailEnergy < 10.0f, "StereoWidth: impulse response decays (no runaway filter state)");
    }

    // ============================ ENHANCER ===============================
    {
        // Test 7: stability at 100% enhance with hot input - no NaN/Inf,
        // no wild gain increase (level-conscious).
        Enhancer enh;
        enh.prepare (spec);
        auto noise = makeNoise (2, numSamples, 0.8f, 999);
        float inRms = rmsOf (noise, 0);
        for (int b = 0; b < numBlocks; ++b)
        {
            juce::AudioBuffer<float> block (noise.getArrayOfWritePointers(), 2, b * blockSize, blockSize);
            juce::dsp::AudioBlock<float> ab (block);
            enh.setParameters (1.0f, 1800.0f);
            enh.process (ab);
        }
        check (allFinite (noise), "Enhancer: output finite at 100% on hot (0.8 amp) noise");
        float outRms = rmsOf (noise, 0);
        float changeDb = juce::Decibels::gainToDecibels (outRms / juce::jmax (1.0e-9f, inRms));
        check (std::abs (changeDb) < 3.0f, "Enhancer: level-conscious - overall RMS doesn't balloon at 100% (change = " + std::to_string (changeDb) + " dB)");
        float peak = juce::jmax (noise.getMagnitude (0, 0, numSamples), noise.getMagnitude (1, 0, numSamples));
        check (peak < 4.0f, "Enhancer: no runaway/unstable output (peak = " + std::to_string (peak) + ")");
    }
    {
        // Test 8: 0% amount is a true bypass.
        Enhancer enh;
        enh.prepare (spec);
        auto sine = makeSine (sr, blockSize, 1000.0f, 0.5f, true);
        auto original = sine;
        juce::dsp::AudioBlock<float> ab (sine);
        enh.setParameters (0.0f, 1800.0f);
        enh.process (ab);
        float maxDiff = 0.0f;
        for (int i = 0; i < blockSize; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (sine.getSample (0, i) - original.getSample (0, i)));
        check (maxDiff < 1.0e-6f, "Enhancer: 0% amount is a true bypass");
    }

    // ===================== MULTIPLE BUFFER SIZES / SR =====================
    {
        bool allOk = true;
        for (double testSr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            for (int bs : { 32, 64, 128, 256, 512, 1024, 2048 })
            {
                MudProcessor mud; StereoWidth width; Enhancer enh;
                juce::dsp::ProcessSpec s { testSr, (juce::uint32) bs, 2 };
                mud.prepare (s); width.prepare (s); enh.prepare (s);
                auto buf = makeNoise (2, bs, 0.3f, 42);
                juce::dsp::AudioBlock<float> ab (buf);
                mud.setParameters (0.5f, 330.0f, 0.9f, 6.0f, 1.0f);
                mud.process (ab);
                width.setParameters (150.0f, 150.0f);
                width.process (ab);
                enh.setParameters (0.5f, 1800.0f);
                enh.process (ab);
                if (! allFinite (buf)) allOk = false;
            }
        }
        check (allOk, "All modules stay finite across sample rates {44.1k..192k} x buffer sizes {32..2048}");
    }

    // ============================ MONO INPUT ==============================
    {
        MudProcessor mud; StereoWidth width; Enhancer enh;
        juce::dsp::ProcessSpec monoSpec { sr, (juce::uint32) blockSize, 1 };
        mud.prepare (monoSpec); width.prepare (monoSpec); enh.prepare (monoSpec);
        auto buf = makeSine (sr, blockSize, 300.0f, 0.4f, false);
        juce::dsp::AudioBlock<float> ab (buf);
        mud.setParameters (0.6f, 330.0f, 0.9f, 6.0f, 1.0f);
        mud.process (ab);
        width.setParameters (150.0f, 150.0f);
        width.process (ab); // width should safely no-op on a true mono buffer
        enh.setParameters (0.6f, 1800.0f);
        enh.process (ab);
        check (allFinite (buf), "Mono (1-channel) buffer processed without crashing or producing NaN/Inf");
    }

    // ============================ CPU BENCHMARK ===========================
    {
        MudProcessor mud; StereoWidth width; Enhancer enh;
        mud.prepare (spec); width.prepare (spec); enh.prepare (spec);
        mud.setParameters (0.6f, 330.0f, 0.9f, 6.0f, 1.0f);
        width.setParameters (150.0f, 150.0f);
        enh.setParameters (0.6f, 1800.0f);

        auto testBuf = makeNoise (2, blockSize, 0.3f, 7);
        const int iterations = 2000; // 2000 * 512 samples @ 48kHz = ~21.3s of audio

        auto t0 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iterations; ++it)
        {
            juce::dsp::AudioBlock<float> ab (testBuf);
            mud.process (ab);
            enh.process (ab);
            width.process (ab);
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        double processedSeconds = (double) iterations * blockSize / sr;
        double wallSeconds = std::chrono::duration<double> (t1 - t0).count();
        double cpuPercentSingleInstance = 100.0 * wallSeconds / processedSeconds;
        double estimatedMaxInstances = 100.0 / juce::jmax (0.0001, cpuPercentSingleInstance);

        std::cout << "\n[BENCHMARK] Single instance (full chain, one CPU core, -O3+LTO release build):\n";
        std::cout << "  Processed " << processedSeconds << "s of audio in " << wallSeconds << "s wall time\n";
        std::cout << "  CPU load: " << cpuPercentSingleInstance << "% of one core\n";
        std::cout << "  Naive single-core budget allows roughly " << (int) estimatedMaxInstances << " simultaneous instances\n";
        check (allFinite (testBuf), "CPU benchmark buffer finite after 2000 iterations");
        check (cpuPercentSingleInstance < 25.0, "Single instance uses well under 25% of one core (measured " + std::to_string (cpuPercentSingleInstance) + "%) - supports 20-50 track target");
    }

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : std::to_string (failures) + " TEST(S) FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
