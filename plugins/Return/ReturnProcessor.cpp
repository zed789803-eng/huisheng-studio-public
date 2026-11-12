#include "ReturnProcessor.h"

#include "AgentLauncher.h"
#include "ReturnEditor.h"

#include <algorithm>
#include <cmath>

ReturnProcessor::ReturnProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // The plug-in is transparent: whatever the host sends continues to the main
    // output untouched, and a copy goes to the virtual pair.
    setLatencySamples(0);
    openRings();
    startTimer(200);

    if (auto* c = control())
        c->returnHeartbeat.store(hs::nowNs(), std::memory_order_release);
}

ReturnProcessor::~ReturnProcessor()
{
    stopTimer();
}

hs::ControlBlock* ReturnProcessor::control() const noexcept
{
    return mapping.block();
}

bool ReturnProcessor::openRings()
{
    mapping.open();
    returnRing.open(hs::returnRingName()) || returnRing.create(hs::returnRingName(), hs::kDefaultRate);
    return returnRing.isOpen();
}

void ReturnProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    hostRate_ = sampleRate > 0 ? sampleRate : 48000.0;

    scratch.assign(static_cast<size_t>(juce::jmax(samplesPerBlock, 2048)) * 2, 0.0f);

    if (! returnRing.isOpen())
        openRings();

    if (returnRing.isOpen())
    {
        returnRing.setHostSampleRate(static_cast<uint32_t>(std::llround(hostRate_)));
        returnRing.setSampleRate(static_cast<uint32_t>(std::llround(hostRate_)));
    }

    hs::launchAgentIfNeeded();
    publish();
}

void ReturnProcessor::releaseResources() {}

bool ReturnProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
           && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ReturnProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // The mix must keep flowing to the main output; this plug-in only listens.
    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0 || buffer.getNumChannels() < 2)
        return;

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));
    auto* c = control();

    if (c != nullptr)
    {
        c->returnHostRate.store(rate, std::memory_order_relaxed);

        if (returnRing.isOpen())
            returnRing.setHostSampleRate(rate);
    }

    if (! returnRing.isOpen() || static_cast<size_t>(numSamples) * 2 > scratch.size())
        return;

    const auto* left = buffer.getReadPointer(0);
    const auto* right = buffer.getReadPointer(1);

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        scratch[static_cast<size_t>(i) * 2] = left[i];
        scratch[static_cast<size_t>(i) * 2 + 1] = right[i];
        peak = std::max(peak, std::max(std::abs(left[i]), std::abs(right[i])));
    }

    returnRing.write(scratch.data(), static_cast<uint32_t>(numSamples));
    level.store(peak, std::memory_order_relaxed);
    written.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* ReturnProcessor::createEditor()
{
    return new ReturnEditor(*this);
}

void ReturnProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("HuishengStudioReturn");

    {
        const juce::ScopedLock lock(stateLock);
        state.setProperty("pair", savedPair, nullptr);
    }

    juce::MemoryOutputStream stream(destData, true);
    state.writeToStream(stream);
}

void ReturnProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const auto state = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes));

    if (! state.isValid() || ! state.hasType("HuishengStudioReturn"))
        return;

    const int pair = static_cast<int>(state.getProperty("pair", hs::kDefaultReturnPair));
    selectPair(pair);
}

void ReturnProcessor::selectPair(int pairIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        savedPair = juce::jlimit(0, hs::kDevicePairs - 1, pairIndex);
    }

    publish();
}

int ReturnProcessor::selectedPair() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedPair;
}

void ReturnProcessor::publish()
{
    auto* c = control();

    if (c == nullptr)
        return;

    int pair = hs::kDefaultReturnPair;

    {
        const juce::ScopedLock lock(stateLock);
        pair = juce::jlimit(0, hs::kDevicePairs - 1, savedPair);
    }

    c->returnPair.store(pair, std::memory_order_relaxed);
    c->returnHostRate.store(static_cast<uint32_t>(std::llround(hostRate_)), std::memory_order_relaxed);
    c->returnSeq.fetch_add(1, std::memory_order_release);
}

void ReturnProcessor::timerCallback()
{
    if (auto* c = control())
        {
        c->returnHeartbeat.store(hs::nowNs(), std::memory_order_release);
        c->returnHostRate.store(static_cast<uint32_t>(std::llround(hostRate_)),
                                std::memory_order_relaxed);
    }

    if (! returnRing.isOpen())
    {
        if (++reconnectTicks >= 10)
        {
            reconnectTicks = 0;
            openRings();
        }
    }
    else
    {
        reconnectTicks = 0;

        if (auto* c = control())
            c->returnHostRate.store(static_cast<uint32_t>(std::llround(hostRate_)),
                                    std::memory_order_relaxed);
    }

    if (! agentAlive())
    {
        if (++launchTicks >= 10)
        {
            launchTicks = 0;
            hs::launchAgentIfNeeded();
        }
    }
    else
    {
        launchTicks = 0;
    }
}

void ReturnProcessor::reconnect()
{
    openRings();
    publish();
}

bool ReturnProcessor::agentAlive() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return false;

    const uint64_t heartbeat = c->heartbeat.load(std::memory_order_acquire);

    if (heartbeat == 0)
        return false;

    return hs::nowNs() - heartbeat < 2'000'000'000ull;
}

hs::State ReturnProcessor::returnState() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<hs::State>(c->returnState.load(std::memory_order_relaxed))
                        : hs::State::Stopped;
}

hs::Fault ReturnProcessor::returnFault() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<hs::Fault>(c->returnFault.load(std::memory_order_relaxed))
                        : hs::Fault::None;
}

float ReturnProcessor::outputPeak() const noexcept
{
    return level.load(std::memory_order_relaxed);
}

uint32_t ReturnProcessor::sampleRate() const noexcept
{
    return static_cast<uint32_t>(std::llround(hostRate_));
}

#ifndef HS_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReturnProcessor();
}
#endif
