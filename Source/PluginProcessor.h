#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/MudProcessor.h"
#include "DSP/StereoWidth.h"
#include "DSP/Enhancer.h"
#include "DSP/Metering.h"

namespace IDs
{
    static const juce::String input       { "input" };
    static const juce::String output      { "output" };
    static const juce::String mix         { "mix" };
    static const juce::String bypass      { "bypass" };
    static const juce::String enhance     { "enhance" };
    static const juce::String width       { "width" };
    static const juce::String mudCut      { "mudCut" };
    static const juce::String autoGain    { "autoGain" };

    static const juce::String mudFrequency  { "mudFrequency" };
    static const juce::String mudQ          { "mudQ" };
    static const juce::String mudMaxReduction { "mudMaxReduction" };
    static const juce::String mudSensitivity  { "mudSensitivity" };
    static const juce::String widthLowProtect { "widthLowProtect" };
    static const juce::String enhancerCharacter { "enhancerCharacter" };
    static const juce::String oversamplingOn  { "oversamplingOn" };
}

class ClarityWidthAudioProcessor final : public juce::AudioProcessor
{
public:
    ClarityWidthAudioProcessor();
    ~ClarityWidthAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    using AudioProcessor::processBlock;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return (int) presetNames.size(); }
    int getCurrentProgram() override { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override { return presetNames[(size_t) index]; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // For the GUI meters (lock-free reads).
    clarity::PeakLevelFollower inputMeter, outputMeter;
    std::atomic<float> mudGainReductionDbForUI { 0.0f };
    std::atomic<float> correlationForUI { 1.0f };

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void applyPreset (int index);

    clarity::MudProcessor mudProcessor;
    clarity::StereoWidth stereoWidth;
    clarity::Enhancer enhancer;

    juce::LinearSmoothedValue<float> inputGainSmoothed, outputGainSmoothed, mixSmoothed;

    // Auto-gain: short-window RMS matcher comparing dry vs wet.
    float dryRmsEnv = 0.0f, wetRmsEnv = 0.0f;
    float autoGainCorrectionDb = 0.0f;
    juce::LinearSmoothedValue<float> autoGainSmoothed;

    juce::AudioBuffer<float> dryBuffer;

    int currentPreset = 0;
    std::vector<juce::String> presetNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClarityWidthAudioProcessor)
};
