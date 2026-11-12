#pragma once

#include <JuceHeader.h>

#include "License.h"
#include "ReturnProcessor.h"
#include "LookAndFeel.h"

// Return editor.
//
// One decision — which virtual pair receives the mix — plus the matching input
// name to select in OBS, and a live level. Nothing else.
class ReturnEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit ReturnEditor(ReturnProcessor& processor);
    ~ReturnEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateStatus();

    ReturnProcessor& processor;

    juce::Label title;
    juce::Label subtitle;
    hs::theme::StatusDot agentDot;
    juce::Label agentLabel;

    juce::Label caption;
    juce::ComboBox pairBox;

    hs::theme::LevelMeter meter;
    juce::Label meterCaption;
    juce::Label monitor;

    juce::Label footer;

    juce::TextButton registerButton;
    hs::license::Result licence;

    juce::Image logo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReturnEditor)
};
