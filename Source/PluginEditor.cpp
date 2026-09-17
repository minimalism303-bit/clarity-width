#include "PluginProcessor.h"
#include "PluginEditor.h"

ClarityWidthAudioProcessorEditor::ClarityWidthAudioProcessorEditor (ClarityWidthAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      enhanceKnob (p.apvts, IDs::enhance, "ENHANCE"),
      widthKnob (p.apvts, IDs::width, "WIDTH"),
      mudCutKnob (p.apvts, IDs::mudCut, "MUD CUT"),
      inputControl (p.apvts, IDs::input, "Input"),
      outputControl (p.apvts, IDs::output, "Output"),
      mixControl (p.apvts, IDs::mix, "Mix"),
      inputMeterBar ([&p] { return p.inputMeter.getLevel(); }),
      outputMeterBar ([&p] { return p.outputMeter.getLevel(); }),
      mudReductionBar ([&p] { return p.mudGainReductionDbForUI.load(); }, 12.0f),
      correlationMeter ([&p] { return p.correlationForUI.load(); })
{
    setSize (700, 400);
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio (700.0 / 400.0);
    getConstrainer()->setSizeLimits (500, 286, 1100, 629);

    addAndMakeVisible (enhanceKnob);
    addAndMakeVisible (widthKnob);
    addAndMakeVisible (mudCutKnob);
    addAndMakeVisible (inputControl);
    addAndMakeVisible (outputControl);
    addAndMakeVisible (mixControl);
    addAndMakeVisible (inputMeterBar);
    addAndMakeVisible (outputMeterBar);
    addAndMakeVisible (mudReductionBar);
    addAndMakeVisible (correlationMeter);

    bypassButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    autoGainButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (bypassButton);
    addAndMakeVisible (autoGainButton);

    if (auto* bypassParam = p.apvts.getParameter (IDs::bypass))
        bypassAttachment = std::make_unique<juce::ButtonParameterAttachment> (*bypassParam, bypassButton);
    if (auto* autoGainParam = p.apvts.getParameter (IDs::autoGain))
        autoGainAttachment = std::make_unique<juce::ButtonParameterAttachment> (*autoGainParam, autoGainButton);

    presetBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    for (int i = 0; i < p.getNumPrograms(); ++i)
        presetBox.addItem (p.getProgramName (i), i + 1);
    presetBox.setSelectedItemIndex (p.getCurrentProgram(), juce::dontSendNotification);
    presetBox.onChange = [this] { processorRef.setCurrentProgram (presetBox.getSelectedItemIndex()); };
    addAndMakeVisible (presetBox);

    advancedToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    advancedToggle.onClick = [this]
    {
        advancedVisible = advancedToggle.getToggleState();
        advancedPanel.setVisible (advancedVisible);
        int extra = advancedVisible ? 90 : 0;
        setSize (getWidth(), 400 + extra);
    };
    addAndMakeVisible (advancedToggle);

    addAndMakeVisible (advancedPanel);
    advancedPanel.setVisible (false);

    struct AdvParam { const char* id; const char* label; };
    static const AdvParam advParams[] = {
        { IDs::mudFrequency.toRawUTF8(), "Mud Freq" },
        { IDs::mudQ.toRawUTF8(), "Mud Q" },
        { IDs::mudMaxReduction.toRawUTF8(), "Mud Max" },
        { IDs::mudSensitivity.toRawUTF8(), "Mud Sens" },
        { IDs::widthLowProtect.toRawUTF8(), "Low Protect" },
        { IDs::enhancerCharacter.toRawUTF8(), "Enh. Char" },
    };
    for (auto& ap : advParams)
    {
        auto control = std::make_unique<SmallControl> (p.apvts, juce::String (ap.id), ap.label);
        advancedPanel.addAndMakeVisible (*control);
        advancedControls.push_back (std::move (control));
    }
}

void ClarityWidthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14151a));

    auto bounds = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1e26), bounds.getTopLeft(),
                                              juce::Colour (0xff101115), bounds.getBottomLeft(), false));
    g.fillRect (bounds);

    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    g.drawText ("ClarityWidth", getLocalBounds().removeFromTop (34).withTrimmedLeft (16), juce::Justification::centredLeft);
}

void ClarityWidthAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop (34); // title bar

    auto topRow = bounds.removeFromTop (28).reduced (16, 0);
    presetBox.setBounds (topRow.removeFromLeft (160));
    topRow.removeFromLeft (12);
    bypassButton.setBounds (topRow.removeFromLeft (90));
    autoGainButton.setBounds (topRow.removeFromLeft (100));
    advancedToggle.setBounds (topRow.removeFromRight (100));

    // Reserve the bottom row(s) FIRST, out of the full remaining bounds,
    // so the knobs/meters area above never overlaps them.
    auto bottomRow = bounds.removeFromBottom (advancedVisible ? 160 : 70).reduced (16, 8);

    if (advancedVisible)
    {
        auto advArea = bottomRow.removeFromTop (90);
        advancedPanel.setBounds (advArea);
        auto w = advArea.getWidth() / (int) juce::jmax ((size_t) 1, advancedControls.size());
        auto localArea = advancedPanel.getLocalBounds();
        for (auto& c : advancedControls)
            c->setBounds (localArea.removeFromLeft (w).reduced (6));
    }

    auto smallW = bottomRow.getWidth() / 3;
    inputControl.setBounds (bottomRow.removeFromLeft (smallW).reduced (8));
    outputControl.setBounds (bottomRow.removeFromLeft (smallW).reduced (8));
    mixControl.setBounds (bottomRow.reduced (8));

    // Now lay out the knobs + meters in whatever's left above that.
    auto metersColumnWidth = 90;
    auto metersArea = bounds.removeFromRight (metersColumnWidth).reduced (10);

    auto barsRow = metersArea.removeFromTop (metersArea.getHeight() - 26);
    auto barW = barsRow.getWidth() / 3;
    inputMeterBar.setBounds (barsRow.removeFromLeft (barW).reduced (4));
    outputMeterBar.setBounds (barsRow.removeFromLeft (barW).reduced (4));
    mudReductionBar.setBounds (barsRow.reduced (4));

    correlationMeter.setBounds (metersArea.reduced (0, 4));

    auto knobsArea = bounds.reduced (20, 10);
    auto knobW = knobsArea.getWidth() / 3;
    enhanceKnob.setBounds (knobsArea.removeFromLeft (knobW).reduced (8));
    widthKnob.setBounds (knobsArea.removeFromLeft (knobW).reduced (8));
    mudCutKnob.setBounds (knobsArea.reduced (8));
}
