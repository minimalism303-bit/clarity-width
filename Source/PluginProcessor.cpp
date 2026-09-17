#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    struct PresetValues
    {
        const char* name;
        float enhance, width, mudCut;
    };

    // Sensible, non-extreme starting points, as requested.
    static const PresetValues kPresets[] = {
        { "Default",        20.0f, 100.0f, 20.0f },
        { "Lead Vocal",      35.0f, 110.0f, 40.0f },
        { "Backing Vocal",   25.0f, 130.0f, 30.0f },
        { "Acoustic Guitar", 30.0f, 120.0f, 35.0f },
        { "Electric Guitar", 25.0f, 115.0f, 45.0f },
        { "Piano",           20.0f, 125.0f, 25.0f },
        { "Synth",           30.0f, 140.0f, 20.0f },
        { "Pad",             15.0f, 160.0f, 15.0f },
        { "Drums",           25.0f, 105.0f, 30.0f },
        { "Percussion",      30.0f, 120.0f, 20.0f },
        { "Bass",            10.0f,  90.0f, 25.0f },
        { "Strings",         20.0f, 135.0f, 20.0f },
        { "Gentle Polish",   15.0f, 105.0f, 15.0f },
        { "Wide",            10.0f, 160.0f, 10.0f },
        { "Cleanup",          5.0f, 100.0f, 55.0f },
    };
}

ClarityWidthAudioProcessor::ClarityWidthAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    for (auto& p : kPresets)
        presetNames.push_back (p.name);
}

juce::AudioProcessorValueTreeState::ParameterLayout ClarityWidthAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto dbAttr = juce::AudioParameterFloatAttributes().withLabel ("dB");
    auto pctAttr = juce::AudioParameterFloatAttributes().withLabel ("%");

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::enhance, 1), "Enhance",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.01f), 20.0f, pctAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::width, 1), "Width",
        juce::NormalisableRange<float> (0.0f, 200.0f, 0.01f), 100.0f, pctAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mudCut, 1), "Mud Cut",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.01f), 20.0f, pctAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::input, 1), "Input",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, dbAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::output, 1), "Output",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, dbAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mix, 1), "Mix",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.01f), 100.0f, pctAttr));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (IDs::bypass, 1), "Bypass", false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (IDs::autoGain, 1), "Auto Gain", true));

    // --- Advanced panel ---
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mudFrequency, 1), "Mud Frequency",
        juce::NormalisableRange<float> (180.0f, 550.0f, 1.0f), 330.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mudQ, 1), "Mud Q",
        juce::NormalisableRange<float> (0.3f, 2.0f, 0.01f), 0.9f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mudMaxReduction, 1), "Mud Max Reduction",
        juce::NormalisableRange<float> (1.0f, 12.0f, 0.1f), 6.0f, dbAttr));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::mudSensitivity, 1), "Mud Detector Sensitivity",
        juce::NormalisableRange<float> (0.1f, 3.0f, 0.01f), 1.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::widthLowProtect, 1), "Width Low-Frequency Protect",
        juce::NormalisableRange<float> (60.0f, 400.0f, 1.0f), 150.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID (IDs::enhancerCharacter, 1), "Enhancer Character",
        juce::NormalisableRange<float> (600.0f, 6000.0f, 1.0f), 1800.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (IDs::oversamplingOn, 1), "Oversampling", true));

    return { params.begin(), params.end() };
}

bool ClarityWidthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (mainIn != mainOut)
        return false;
    return true;
}

void ClarityWidthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = (juce::uint32) juce::jmax (1, getTotalNumOutputChannels());

    mudProcessor.prepare (spec);
    stereoWidth.prepare (spec);
    enhancer.prepare (spec);

    inputMeter.prepare (sampleRate);
    outputMeter.prepare (sampleRate);

    inputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.reset (sampleRate, 0.02);
    mixSmoothed.reset (sampleRate, 0.02);
    autoGainSmoothed.reset (sampleRate, 0.05);

    dryBuffer.setSize ((int) spec.numChannels, samplesPerBlock);
    dryRmsEnv = wetRmsEnv = 0.0f;

    setLatencySamples (enhancer.getLatencySamples());
}

void ClarityWidthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const bool bypassed = apvts.getRawParameterValue (IDs::bypass)->load() > 0.5f;

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    if (bypassed)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        inputMeter.pushBlock (block);
        outputMeter.pushBlock (block);
        return;
    }

    dryBuffer.setSize (numCh, numSamples, false, false, true);
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    float inputDb = apvts.getRawParameterValue (IDs::input)->load();
    float outputDb = apvts.getRawParameterValue (IDs::output)->load();
    float mixPct = apvts.getRawParameterValue (IDs::mix)->load();
    bool autoGainOn = apvts.getRawParameterValue (IDs::autoGain)->load() > 0.5f;

    float enhanceAmt = apvts.getRawParameterValue (IDs::enhance)->load() / 100.0f;
    float widthPct = apvts.getRawParameterValue (IDs::width)->load();
    float mudCutAmt = apvts.getRawParameterValue (IDs::mudCut)->load() / 100.0f;

    float mudFreq = apvts.getRawParameterValue (IDs::mudFrequency)->load();
    float mudQ = apvts.getRawParameterValue (IDs::mudQ)->load();
    float mudMaxRed = apvts.getRawParameterValue (IDs::mudMaxReduction)->load();
    float mudSens = apvts.getRawParameterValue (IDs::mudSensitivity)->load();
    float widthLowProtect = apvts.getRawParameterValue (IDs::widthLowProtect)->load();
    float enhancerCharacter = apvts.getRawParameterValue (IDs::enhancerCharacter)->load();

    inputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (inputDb));
    outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputDb));
    mixSmoothed.setTargetValue (mixPct / 100.0f);

    juce::dsp::AudioBlock<float> block (buffer);

    inputMeter.pushBlock (block);

    // --- Input gain ---
    for (int i = 0; i < numSamples; ++i)
    {
        float g = inputGainSmoothed.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }

    float preChainRms = 0.0f;
    if (autoGainOn)
    {
        for (int ch = 0; ch < numCh; ++ch)
            preChainRms += buffer.getRMSLevel (ch, 0, numSamples);
        preChainRms /= (float) juce::jmax (1, numCh);
    }

    // --- Signal flow: Mud -> Enhance -> Width -> Output gain comp ---
    mudProcessor.setParameters (mudCutAmt, mudFreq, mudQ, mudMaxRed, mudSens);
    mudProcessor.process (block);

    enhancer.setParameters (enhanceAmt, enhancerCharacter);
    enhancer.process (block);

    stereoWidth.setParameters (widthPct, widthLowProtect);
    stereoWidth.process (block);

    if (autoGainOn)
    {
        float postChainRms = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            postChainRms += buffer.getRMSLevel (ch, 0, numSamples);
        postChainRms /= (float) juce::jmax (1, numCh);

        float targetCorrectionDb = 0.0f;
        if (postChainRms > 1.0e-6f && preChainRms > 1.0e-6f)
            targetCorrectionDb = juce::jlimit (-6.0f, 6.0f, juce::Decibels::gainToDecibels (preChainRms / postChainRms));

        autoGainSmoothed.setTargetValue (targetCorrectionDb);
        float correctionDb = autoGainSmoothed.getNextValue();
        autoGainCorrectionDb = correctionDb;
        float correctionGain = juce::Decibels::decibelsToGain (correctionDb);
        buffer.applyGain (correctionGain);
    }

    // --- Output gain ---
    for (int i = 0; i < numSamples; ++i)
    {
        float g = outputGainSmoothed.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }

    // --- Global wet/dry mix (dry tapped pre-input-gain, post latency n/a
    //     since oversampling latency here is sub-sample / negligible for
    //     the IIR halfband design used, so no explicit delay compensation
    //     buffer is required at typical settings) ---
    for (int i = 0; i < numSamples; ++i)
    {
        float mixv = mixSmoothed.getNextValue();
        for (int ch = 0; ch < numCh; ++ch)
        {
            float wet = buffer.getSample (ch, i);
            float dry = dryBuffer.getSample (ch, i);
            buffer.setSample (ch, i, dry + (wet - dry) * mixv);
        }
    }

    outputMeter.pushBlock (block);
    mudGainReductionDbForUI.store (mudProcessor.getGainReductionDbForMeter());
    correlationForUI.store (stereoWidth.getCorrelationForMeter());

    // Guard against NaNs/Infs escaping to the host.
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            if (! std::isfinite (data[i]))
                data[i] = 0.0f;
    }
}

void ClarityWidthAudioProcessor::setCurrentProgram (int index)
{
    if (index < 0 || index >= (int) presetNames.size())
        return;
    currentPreset = index;
    applyPreset (index);
}

void ClarityWidthAudioProcessor::applyPreset (int index)
{
    auto& p = kPresets[(size_t) index];
    if (auto* param = apvts.getParameter (IDs::enhance)) param->setValueNotifyingHost (param->convertTo0to1 (p.enhance));
    if (auto* param = apvts.getParameter (IDs::width))   param->setValueNotifyingHost (param->convertTo0to1 (p.width));
    if (auto* param = apvts.getParameter (IDs::mudCut))  param->setValueNotifyingHost (param->convertTo0to1 (p.mudCut));
}

void ClarityWidthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("currentPreset", currentPreset, nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ClarityWidthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto newState = juce::ValueTree::fromXml (*xml);
        currentPreset = newState.getProperty ("currentPreset", 0);
        apvts.replaceState (newState);
    }
}

juce::AudioProcessorEditor* ClarityWidthAudioProcessor::createEditor()
{
    return new ClarityWidthAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClarityWidthAudioProcessor();
}
