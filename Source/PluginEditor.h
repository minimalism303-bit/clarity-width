#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

//==============================================================================
class LevelMeterBar final : public juce::Component, private juce::Timer
{
public:
    explicit LevelMeterBar (std::function<float()> levelSource) : source (std::move (levelSource))
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1a1c22));
        g.fillRoundedRectangle (bounds, 2.0f);

        float level = juce::jlimit (0.0f, 1.0f, displayed);
        float db = juce::Decibels::gainToDecibels (level, -60.0f);
        float norm = juce::jmap (juce::jlimit (-60.0f, 0.0f, db), -60.0f, 0.0f, 0.0f, 1.0f);

        auto meterBounds = bounds.reduced (1.5f);
        auto filled = meterBounds.removeFromBottom (meterBounds.getHeight() * norm);
        g.setColour (norm > 0.9f ? juce::Colour (0xffe0574f) : juce::Colour (0xff5ad1c9));
        g.fillRoundedRectangle (filled, 1.5f);
    }

    void setSource (std::function<float()> newSource) { source = std::move (newSource); }

private:
    void timerCallback() override
    {
        if (source)
        {
            float v = source();
            displayed = v > displayed ? v : displayed * 0.85f;
            repaint();
        }
    }

    std::function<float()> source;
    float displayed = 0.0f;
};

//==============================================================================
/** Shows Mud Cut gain reduction (0 .. maxReductionDb) as a fill bar. */
class ReductionMeterBar final : public juce::Component, private juce::Timer
{
public:
    ReductionMeterBar (std::function<float()> reductionDbSource, float maxDbRange)
        : source (std::move (reductionDbSource)), maxDb (maxDbRange)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1a1c22));
        g.fillRoundedRectangle (bounds, 2.0f);

        float norm = juce::jlimit (0.0f, 1.0f, displayed / juce::jmax (0.1f, maxDb));
        auto meterBounds = bounds.reduced (1.5f);
        auto filled = meterBounds.removeFromBottom (meterBounds.getHeight() * norm);
        g.setColour (juce::Colour (0xffe8a33d));
        g.fillRoundedRectangle (filled, 1.5f);
    }

private:
    void timerCallback() override
    {
        if (source)
        {
            displayed = source();
            repaint();
        }
    }

    std::function<float()> source;
    float maxDb;
    float displayed = 0.0f;
};

//==============================================================================
class CorrelationMeter final : public juce::Component, private juce::Timer
{
public:
    explicit CorrelationMeter (std::function<float()> source) : getValue (std::move (source))
    {
        startTimerHz (20);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1a1c22));
        g.fillRoundedRectangle (bounds, 2.0f);

        float centreX = bounds.getCentreX();
        g.setColour (juce::Colours::grey.withAlpha (0.4f));
        g.drawVerticalLine ((int) centreX, bounds.getY(), bounds.getBottom());

        float x = juce::jmap (juce::jlimit (-1.0f, 1.0f, value), -1.0f, 1.0f, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
        g.setColour (value < -0.15f ? juce::Colour (0xffe0574f) : juce::Colour (0xff5ad1c9));
        g.fillRoundedRectangle (juce::jmin (x, centreX), bounds.getY() + 2.0f, std::abs (x - centreX) + 1.0f, bounds.getHeight() - 4.0f, 1.0f);
    }

private:
    void timerCallback() override
    {
        if (getValue)
        {
            value = getValue();
            repaint();
        }
    }

    std::function<float()> getValue;
    float value = 1.0f;
};

//==============================================================================
/** A large rotary knob with the value readout baked into the label below it. */
class MainKnob final : public juce::Component
{
public:
    MainKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& labelText)
        : attachment (state, paramId, slider)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 22);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
        slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff5ad1c9));
        slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2a2d36));
        slider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
        slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (slider);

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        label.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        addAndMakeVisible (label);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        label.setBounds (bounds.removeFromTop (24));
        slider.setBounds (bounds);
    }

private:
    juce::Slider slider;
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

//==============================================================================
class SmallControl final : public juce::Component
{
public:
    SmallControl (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& labelText)
        : attachment (state, paramId, slider)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 55, 20);
        slider.setColour (juce::Slider::trackColourId, juce::Colour (0xff5ad1c9));
        slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (slider);

        label.setText (labelText, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
        label.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (label);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        label.setBounds (bounds.removeFromTop (16));
        slider.setBounds (bounds);
    }

private:
    juce::Slider slider;
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

//==============================================================================
class ClarityWidthAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ClarityWidthAudioProcessorEditor (ClarityWidthAudioProcessor&);
    ~ClarityWidthAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ClarityWidthAudioProcessor& processorRef;

    MainKnob enhanceKnob, widthKnob, mudCutKnob;
    SmallControl inputControl, outputControl, mixControl;

    juce::ToggleButton bypassButton { "Bypass" };
    juce::ToggleButton autoGainButton { "Auto Gain" };
    std::unique_ptr<juce::ButtonParameterAttachment> bypassAttachment, autoGainAttachment;

    juce::ComboBox presetBox;

    LevelMeterBar inputMeterBar, outputMeterBar;
    ReductionMeterBar mudReductionBar;
    CorrelationMeter correlationMeter;

    juce::ToggleButton advancedToggle { "Advanced" };
    juce::Component advancedPanel;
    std::vector<std::unique_ptr<SmallControl>> advancedControls;
    bool advancedVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClarityWidthAudioProcessorEditor)
};
