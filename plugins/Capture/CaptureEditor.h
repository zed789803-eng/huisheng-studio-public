#pragma once

#include <JuceHeader.h>

#include "CaptureProcessor.h"
#include "License.h"
#include "LookAndFeel.h"
#include "ProcessList.h"

#include <memory>
#include <vector>

// Capture editor.
//
// Layout, top to bottom:
//   header     logo / product name / agent state dot
//   source     App picker + virtual pair picker, side by side
//   signal     one meter per source plus the summed output
//   footer     permission hint when it is missing, otherwise sample rate
//
// There is no prose: state is a dot colour and a three-word label.
class CaptureEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit CaptureEditor(CaptureProcessor& processor);
    ~CaptureEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshApps();
    void syncFromProcessor();
    void showAppMenu();
    void updateStatus();

    CaptureProcessor& processor;

    juce::Label title;
    juce::Label subtitle;
    hs::theme::StatusDot agentDot;
    juce::Label agentLabel;

    // One button, one menu: several Apps can be captured at once into the same
    // track, so the picker is a check list instead of a single choice.
    juce::TextButton appButton;
    juce::ComboBox pairBox;
    juce::TextButton refreshButton { "+" };

    juce::Label appCaption;
    juce::Label pairCaption;

    hs::theme::LevelMeter appMeter;
    hs::theme::LevelMeter pairMeter;
    hs::theme::LevelMeter outMeter;

    juce::Label appMeterLabel;
    juce::Label pairMeterLabel;
    juce::Label outMeterLabel;

    juce::Label footer;

    juce::TextButton registerButton;
    hs::license::Result licence;

    std::vector<AudioProcessInfo> processes;
    std::vector<juce::Image> images;
    juce::Image logo;

    int statusTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptureEditor)
};
