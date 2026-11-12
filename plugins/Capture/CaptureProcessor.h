#pragma once

#include <JuceHeader.h>

#include "Config.h"
#include "Control.h"
#include "DriftBridge.h"
#include "RingBuffer.h"

#include <atomic>
#include <memory>
#include <string>

// HuishengStudio Capture — replaces the host buffer with the tapped Apps and/or the
// selected virtual pair. Both sources arrive through shared memory from the
// background Agent; the plug-in only mixes and reports state.
//
// Every instance owns one capture slot: its own rings, its own list of Apps and
// its own virtual pair. Each App keeps its own tap, its own ring and its own
// state, so adding or dropping one App never disturbs the others; the plug-in
// sums them into the one track the insert sits on. Several instances capture
// several platforms on several tracks, and unloading one leaves the others
// running.
class CaptureProcessor final : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    CaptureProcessor();
    ~CaptureProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HsAudio Capture"; }
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

    // ---- selection -------------------------------------------------------
    // Apps are additive: pick one after another and they all play into this
    // track. Adding an App that is already in the list only refreshes its pid.
    void addApp(int32_t pid, const juce::String& bundleID, const juce::String& name);
    void removeApp(int index);
    void clearApps();
    void setUsePair(bool use);
    void selectPair(int pairIndex);

    int appCount() const noexcept;
    bool appSelected() const noexcept { return appCount() > 0; }
    juce::String appName(int index) const;
    juce::String appBundleID(int index) const;
    int32_t appPID(int index) const noexcept;
    hs::State appState(int index) const noexcept;
    uint32_t appRate(int index) const noexcept;

    bool pairSelected() const noexcept;
    int selectedPair() const noexcept;

    // ---- this instance ---------------------------------------------------
    // The slot this instance owns (0 based, -1 before the first claim). Several
    // instances of the same plug-in run at once; each one takes its own slot.
    int slotIndex() const noexcept { return slot_; }
    bool slotClaimed() const noexcept { return slot_ >= 0; }

    // Slots the Agent currently leases: more than one means several instances
    // are capturing side by side.
    int activeSlots() const noexcept
    {
        const auto* c = control();
        return c != nullptr ? static_cast<int>(c->activeSlots.load(std::memory_order_relaxed)) : 0;
    }

    // ---- status ----------------------------------------------------------
    bool agentAlive() const noexcept;

    // True while the shared block still belongs to a service built with a
    // different layout: the running copy predates this install, so it will
    // never answer this plug-in and has to be replaced.
    bool agentVersionStale() const noexcept;

    // True when the service has been missing long enough that automatic
    // launching is not going to fix it (the App was never allowed to run, or
    // it cannot be found), so the footer should tell the user what to do.
    bool agentLaunchStalled() const noexcept;
    hs::State agentState() const noexcept;
    hs::Fault agentFault() const noexcept;
    hs::State tapState() const noexcept;
    hs::State channelState() const noexcept;
    hs::Fault tapFault() const noexcept;
    hs::Fault channelFault() const noexcept;
    int audioCapturePermissionRaw() const noexcept;
    int microphonePermissionRaw() const noexcept;

    float tapPeak() const noexcept;
    float channelPeak() const noexcept;
    float outputPeak() const noexcept;
    uint32_t tapRate() const noexcept;
    uint32_t channelRate() const noexcept;
    float tapGain() const noexcept;
    int driftPPM() const noexcept;
    uint64_t underruns() const noexcept;

    uint32_t hostRate() const noexcept { return static_cast<uint32_t>(std::llround(hostRate_)); }
    bool ringsOpen() const noexcept { return channelRing.isOpen(); }

    void reconnect();

private:
    void timerCallback() override;
    bool openRings();
    void claimSlot();
    void releaseSlot();
    void flushRing(hs::RingBuffer& ring);
    void syncTapRings();
    void publish();
    void restore();
    void pump(bool wantApp, bool wantChannel, uint32_t frames);
    hs::ControlBlock* control() const noexcept;
    hs::CaptureSlot* slotCtl() const noexcept;
    int findApp(const juce::String& bundleID, int32_t pid) const;

    hs::ControlMapping mapping;
    hs::RingBuffer tapRings[hs::kSlotApps];
    hs::RingBuffer channelRing;

    std::vector<float> tapScratch;
    std::vector<float> appScratch;
    std::vector<float> channelScratch;
    std::vector<float> readScratch;

    hs::DriftBridge tapBridges[hs::kSlotApps];
    hs::DriftBridge channelBridge;

    std::atomic<float> outputPeakValue { 0.0f };
    std::atomic<uint64_t> processedFrames { 0 };

    double hostRate_ = 48000.0;

    // Slot ownership. The token is random per instance so that releasing a slot
    // can never clear a slot that another instance has taken over in the
    // meantime.
    int slot_ = -1;
    uint32_t slotOwner_ = 0;

    juce::CriticalSection stateLock;
    int savedAppCount = 0;
    juce::String savedBundleID[hs::kSlotApps];
    juce::String savedName[hs::kSlotApps];
    int32_t savedPID[hs::kSlotApps] = {};
    int savedPair = hs::kDefaultCapturePair;
    bool savedUsePair = true;
    bool restorePending = false;
    uint64_t lastRestoreMs = 0;
    int reconnectTicks = 0;
    int launchTicks = 0;
    int staleServiceTicks = 0;
    uint64_t staleRestartAtMs = 0;
    int agentMissingTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CaptureProcessor)
};
