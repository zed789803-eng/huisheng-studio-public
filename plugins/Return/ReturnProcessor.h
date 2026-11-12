#pragma once

#include <JuceHeader.h>

#include "Config.h"
#include "Control.h"
#include "DriftBridge.h"
#include "RingBuffer.h"

#include <atomic>

// HuishengStudio Return — writes the host's Main/Stream bus into a virtual output
// pair. The Agent plays that ring into the device, so OBS / 抖音 / 微信 see the
// finished mix as a microphone.
class ReturnProcessor final : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    ReturnProcessor();
    ~ReturnProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HsAudio Return"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void selectPair(int pairIndex);
    int selectedPair() const noexcept;

    bool agentAlive() const noexcept;
    hs::State returnState() const noexcept;
    hs::Fault returnFault() const noexcept;
    float outputPeak() const noexcept;
    uint32_t sampleRate() const noexcept;

    uint32_t hostRate() const noexcept { return static_cast<uint32_t>(std::llround(hostRate_)); }
    bool ringsOpen() const noexcept { return returnRing.isOpen(); }

    void reconnect();

private:
    void timerCallback() override;
    bool openRings();
    void publish();
    hs::ControlBlock* control() const noexcept;

    hs::ControlMapping mapping;
    hs::RingBuffer returnRing;

    std::vector<float> scratch;
    std::atomic<float> level { 0.0f };
    std::atomic<uint64_t> written { 0 };

    double hostRate_ = 48000.0;

    juce::CriticalSection stateLock;
    int savedPair = hs::kDefaultReturnPair;
    int reconnectTicks = 0;
    int launchTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReturnProcessor)
};
