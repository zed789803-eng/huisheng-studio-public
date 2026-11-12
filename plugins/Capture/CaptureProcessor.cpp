#include "CaptureProcessor.h"

#include "SystemCompat.h"

#include "AgentLauncher.h"
#include "CaptureEditor.h"
#include "ProcessList.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kControlTimerMs = 200;
constexpr int kLaunchIntervalTicks = 10;   // 2 s at 200 ms
constexpr int kRingFlushGuard = 512;       // ~4 s of audio at 128 frames per read

hs::CaptureMode modeFor(bool app, bool channel)
{
    if (app && channel) return hs::CaptureMode::Both;
    if (app) return hs::CaptureMode::App;
    if (channel) return hs::CaptureMode::Channel;
    return hs::CaptureMode::Off;
}

// A random non-zero token per instance. It is what makes "release my slot" safe
// even when another instance has taken that slot over in the meantime.
uint32_t makeSlotOwner()
{
    const auto raw = static_cast<uint32_t>(juce::Random::getSystemRandom().nextInt());
    return raw != 0 ? raw : 1u;
}

} // namespace

CaptureProcessor::CaptureProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    setLatencySamples(0);
    slotOwner_ = makeSlotOwner();

    // Opens the control block and takes a slot if one is free. The selection is
    // published from prepareToPlay, once the host has told us the project rate.
    openRings();
    claimSlot();

    startTimer(kControlTimerMs);
}

CaptureProcessor::~CaptureProcessor()
{
    stopTimer();
    releaseSlot();

    for (auto& ring : tapRings)
        ring.close();

    channelRing.close();
}

//==================================================================================================
#pragma mark Setup
//==================================================================================================

hs::ControlBlock* CaptureProcessor::control() const noexcept
{
    return mapping.block();
}

hs::CaptureSlot* CaptureProcessor::slotCtl() const noexcept
{
    auto* c = control();

    if (c == nullptr || slot_ < 0)
        return nullptr;

    return &c->slots[slot_];
}

// One ring per App entry. Every ring is mapped as soon as the slot is known:
// opening or closing a ring while the audio thread is reading it is a torn
// pointer, and adding the second App to the list is exactly when that used to
// happen. An App that is not being tapped simply has no frames in its ring, and
// the bridge fills silence.
void CaptureProcessor::syncTapRings()
{
    if (slot_ < 0)
        return;

    for (int i = 0; i < hs::kSlotApps; ++i)
    {
        auto& ring = tapRings[i];

        if (ring.isOpen())
            continue;

        const auto name = hs::slotTapRingName(slot_, i);

        if (! ring.open(name))
            ring.create(name, hs::kDefaultRate);
    }
}

bool CaptureProcessor::openRings()
{
    mapping.open();

    if (slot_ < 0)
        claimSlot();

    if (slot_ < 0)
        return false;

    syncTapRings();

    const auto channelName = hs::slotChannelRingName(slot_);

    if (! channelRing.open(channelName))
        channelRing.create(channelName, hs::kDefaultRate);

    return channelRing.isOpen();
}

// One slot per instance. A slot is free when nobody owns it or its last owner
// stopped beating; taking one over is what lets a second instance capture a
// second App instead of fighting the first instance for the same rings.
void CaptureProcessor::claimSlot()
{
    auto* c = control();

    if (c == nullptr || slot_ >= 0)
        return;

    if (slotOwner_ == 0)
        slotOwner_ = makeSlotOwner();

    const uint64_t now = hs::nowNs();

    for (int i = 0; i < hs::kCaptureSlots; ++i)
    {
        auto* slot = &c->slots[i];
        const uint32_t owner = slot->owner.load(std::memory_order_acquire);
        const uint64_t beat = slot->heartbeat.load(std::memory_order_acquire);

        const bool free = owner == 0 || owner == slotOwner_
                          || beat == 0 || now - beat > hs::kSlotLeaseNanos;

        if (! free)
            continue;

        slot->owner.store(slotOwner_, std::memory_order_release);
        slot->heartbeat.store(now, std::memory_order_release);
        slot->mode.store(static_cast<uint32_t>(hs::CaptureMode::Off), std::memory_order_relaxed);
        slot->appCount.store(0, std::memory_order_relaxed);

        for (int a = 0; a < hs::kSlotApps; ++a)
        {
            slot->appPID[a].store(-1, std::memory_order_relaxed);
            slot->appObjectID[a].store(0, std::memory_order_relaxed);
            slot->appBundleID[a][0] = '\0';
            slot->appName[a][0] = '\0';
        }

        slot->pair.store(static_cast<int32_t>(hs::kDefaultCapturePair), std::memory_order_relaxed);
        slot->hostRate.store(static_cast<uint32_t>(std::llround(hostRate_)),
                             std::memory_order_relaxed);
        slot->seq.store(1, std::memory_order_release);

        slot_ = i;
        juce::Logger::writeToLog ("huishengstudio: capture instance " + juce::String (slotOwner_)
                                  + " took slot " + juce::String (i));
        return;
    }

    juce::Logger::writeToLog ("huishengstudio: every capture slot ("
                              + juce::String (hs::kCaptureSlots) + ") is in use");
}

void CaptureProcessor::releaseSlot()
{
    auto* slot = slotCtl();

    if (slot != nullptr && slot->owner.load(std::memory_order_relaxed) == slotOwner_)
    {
        // Heartbeat first, then the owner: the Agent sees an unowned slot and
        // releases exactly this slot's taps, without touching the others.
        slot->heartbeat.store(0, std::memory_order_release);
        slot->owner.store(0, std::memory_order_release);
        slot->mode.store(static_cast<uint32_t>(hs::CaptureMode::Off), std::memory_order_relaxed);
        slot->appCount.store(0, std::memory_order_relaxed);

        for (int a = 0; a < hs::kSlotApps; ++a)
        {
            slot->appPID[a].store(-1, std::memory_order_relaxed);
            slot->appObjectID[a].store(0, std::memory_order_relaxed);
        }

        slot->seq.fetch_add(1, std::memory_order_release);
        juce::Logger::writeToLog ("huishengstudio: capture instance " + juce::String (slotOwner_)
                                  + " released slot " + juce::String (slot_));
    }

    slot_ = -1;
}

// A slot's rings outlive the instance that created them (the Agent may still be
// mapped to them), so a fresh owner starts by throwing away whatever the
// previous owner left behind instead of playing it out.
void CaptureProcessor::flushRing(hs::RingBuffer& ring)
{
    if (! ring.isOpen())
        return;

    std::vector<float> scratch(2048 * 2, 0.0f);
    int guard = 0;

    while (ring.available() > 0 && guard++ < kRingFlushGuard)
    {
        const uint32_t take = std::min<uint32_t>(ring.available(), 2048);
        ring.read(scratch.data(), take);
    }

    if (auto* h = ring.header())
        h->peak.store(0.0f, std::memory_order_relaxed);
}

void CaptureProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    hostRate_ = sampleRate > 0 ? sampleRate : 48000.0;

    const auto frames = static_cast<size_t>(juce::jmax(samplesPerBlock, 2048));
    tapScratch.assign(frames * 2, 0.0f);
    appScratch.assign(frames * 2, 0.0f);
    channelScratch.assign(frames * 2, 0.0f);
    readScratch.assign(frames * 2, 0.0f);

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));

    for (auto& bridge : tapBridges)
    {
        bridge.reset(rate);
        bridge.setTarget(hs::kTargetFrames);
    }

    channelBridge.reset(rate);
    channelBridge.setTarget(hs::kTargetFrames);

    if (! ringsOpen())
        openRings();

    syncTapRings();

    for (auto& ring : tapRings)
        if (ring.isOpen())
            ring.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);

    hs::launchAgentIfNeeded();
    publish();
}

void CaptureProcessor::releaseResources() {}

bool CaptureProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    return (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo())
           && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

//==================================================================================================
#pragma mark Audio
//==================================================================================================

void CaptureProcessor::pump(bool wantApp, bool wantChannel, uint32_t frames)
{
    // One tap ring per App. Each one runs on the clock of the App it taps, so
    // each one gets its own bridge: the Apps drift independently and none of
    // them may lose frames because a sibling is late.
    for (int i = 0; i < hs::kSlotApps; ++i)
    {
        auto& ring = tapRings[i];

        if (! ring.isOpen())
            continue;

        if (! wantApp)
            continue;

        const uint32_t available = ring.available();

        if (available > 0)
        {
            const uint32_t take = std::min(available, frames + 64);

            if (static_cast<size_t>(take) * 2 <= readScratch.size())
            {
                ring.read(readScratch.data(), take);
                tapBridges[i].push(readScratch.data(), take);
            }
        }

        if (ring.sampleRate() != 0)
            tapBridges[i].setInputRate(ring.sampleRate());
    }

    if (channelRing.isOpen())
    {
        const uint32_t available = channelRing.available();

        if (wantChannel && available > 0)
        {
            const uint32_t take = std::min(available, frames + 64);

            if (static_cast<size_t>(take) * 2 <= readScratch.size())
            {
                channelRing.read(readScratch.data(), take);
                channelBridge.push(readScratch.data(), take);
            }
        }

        if (wantChannel && channelRing.sampleRate() != 0)
            channelBridge.setInputRate(channelRing.sampleRate());
    }
}

void CaptureProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int numSamples = buffer.getNumSamples();

    if (numSamples <= 0)
        return;

    // This insert replaces the track input entirely: the room microphone on the
    // same bus must not leak into the recording.
    buffer.clear();

    auto* c = control();

    const auto needed = static_cast<size_t>(numSamples) * 2;

    if (c == nullptr || needed > tapScratch.size() || needed > appScratch.size())
    {
        outputPeakValue.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // This instance's own slot. Without one there is nothing to play, which is
    // what a DAW that opened more instances than there are slots would see.
    auto* slot = slotCtl();

    if (slot == nullptr)
    {
        outputPeakValue.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const auto rawMode = slot->mode.load(std::memory_order_relaxed);
    const bool wantApp = rawMode == static_cast<uint32_t>(hs::CaptureMode::App)
                         || rawMode == static_cast<uint32_t>(hs::CaptureMode::Both);
    const bool wantChannel = rawMode == static_cast<uint32_t>(hs::CaptureMode::Channel)
                             || rawMode == static_cast<uint32_t>(hs::CaptureMode::Both);

    pump(wantApp, wantChannel, static_cast<uint32_t>(numSamples));

    // Every App goes into the same track: sum the App bridges first, then add
    // the virtual channel. Each tap runs at its own App's device rate and the
    // virtual input at the driver's rate; every bridge resamples, so a rate that
    // differs from the project is normal operation and never a reason to mute
    // the track. (Muting on a mismatch is what made a 44.1 kHz App silent in a
    // 48 kHz project.)
    std::fill(tapScratch.begin(), tapScratch.begin() + numSamples * 2, 0.0f);

    if (wantApp)
    {
        for (int i = 0; i < hs::kSlotApps; ++i)
        {
            if (! tapRings[i].isOpen())
                continue;

            tapBridges[i].pull(appScratch.data(), static_cast<uint32_t>(numSamples));

            for (size_t s = 0; s < needed; ++s)
                tapScratch[s] += appScratch[s];
        }
    }

    if (wantChannel && channelRing.isOpen())
        channelBridge.pull(channelScratch.data(), static_cast<uint32_t>(numSamples));
    else
        std::fill(channelScratch.begin(), channelScratch.begin() + numSamples * 2, 0.0f);

    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = tapScratch[static_cast<size_t>(i) * 2] + channelScratch[static_cast<size_t>(i) * 2];
        const float r = tapScratch[static_cast<size_t>(i) * 2 + 1] + channelScratch[static_cast<size_t>(i) * 2 + 1];

        if (right != nullptr)
        {
            // No clamp: float32 all the way through, so a hot App (or a group of
            // them) keeps its transients instead of being flattened here.
            left[i] = l;
            right[i] = r;
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
        }
        else
        {
            const float mono = (l + r) * 0.5f;
            left[i] = mono;
            peak = std::max(peak, std::abs(mono));
        }
    }

    outputPeakValue.store(peak, std::memory_order_relaxed);
    processedFrames.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);
}

void CaptureProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Keep the capture flowing even when the host natively bypasses the insert.
    processBlock(buffer, midi);
}

juce::AudioProcessorEditor* CaptureProcessor::createEditor()
{
    return new CaptureEditor(*this);
}

//==================================================================================================
#pragma mark State
//==================================================================================================

void CaptureProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Deliberately avoid juce::ValueTree/var here. A ValueTree created inside
    // this plug-in used to abort in its destructor while the host was asking
    // for state during instantiation (the VST3 wrapper calls getState() right
    // after createInstance). Keep the state as plain little-endian primitives:
    // no ref-counted var, no cross-module juce::String destruction.
    constexpr juce::int32 stateMagic = 0x574E4743;   // 'HSC'
    constexpr juce::int16 stateVersion = 3;

    juce::MemoryOutputStream stream (destData, false);
    stream.writeInt (stateMagic);
    stream.writeShort (stateVersion);

    {
        const juce::ScopedLock lock (stateLock);
        stream.writeInt (savedAppCount);

        for (int i = 0; i < savedAppCount; ++i)
        {
            stream.writeString (savedBundleID[i]);
            stream.writeString (savedName[i]);
            stream.writeInt (savedPID[i]);
        }

        stream.writeInt (savedPair);
        stream.writeBool (savedUsePair);
    }
}

void CaptureProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    juce::MemoryInputStream stream (data, static_cast<size_t> (sizeInBytes), false);

    // Version 1 states were ValueTree blobs written by older builds. There is
    // no safe way to parse those through var without risking the same abort,
    // so ignore them and start with the default selection instead. Version 2
    // held a single App and is read as a one-entry list.
    if (stream.readInt() != 0x574E4743)
        return;

    const auto version = stream.readShort();

    if (version < 2 || version > 3)
        return;

    int count = 1;

    if (version >= 3)
        count = stream.readInt();

    count = juce::jlimit (0, hs::kSlotApps, count);

    juce::String bundleID[hs::kSlotApps];
    juce::String name[hs::kSlotApps];
    int32_t pid[hs::kSlotApps] = {};

    for (int i = 0; i < count; ++i)
    {
        bundleID[i] = stream.readString();
        name[i] = stream.readString();
        pid[i] = stream.readInt();
    }

    const auto pair = stream.readInt();
    const auto usePair = stream.readBool();

    if (stream.getNumBytesRemaining() < 0)
        return;

    const juce::ScopedLock lock (stateLock);
    savedAppCount = count;

    for (int i = 0; i < hs::kSlotApps; ++i)
    {
        savedBundleID[i] = i < count ? bundleID[i] : juce::String();
        savedName[i] = i < count ? name[i] : juce::String();
        savedPID[i] = i < count ? pid[i] : -1;
    }

    // A trailing entry that carries neither an App nor a pid is just leftover
    // room from a longer list.
    while (savedAppCount > 0
           && savedPID[savedAppCount - 1] <= 0
           && savedBundleID[savedAppCount - 1].isEmpty())
        --savedAppCount;

    savedPair = juce::jlimit (0, hs::kDevicePairs - 1, pair);
    savedUsePair = usePair;

    restorePending = true;
    lastRestoreMs = 0;
}

//==================================================================================================
#pragma mark Selection
//==================================================================================================

int CaptureProcessor::findApp(const juce::String& bundleID, int32_t pid) const
{
    const juce::ScopedLock lock (stateLock);

    for (int i = 0; i < savedAppCount; ++i)
    {
        if (bundleID.isNotEmpty() && savedBundleID[i] == bundleID)
            return i;

        if (pid > 0 && savedPID[i] == pid)
            return i;
    }

    return -1;
}

void CaptureProcessor::addApp(int32_t pid, const juce::String& bundleID, const juce::String& name)
{
    // A project saved on 14.2+ may still name an App when it is opened on macOS
    // 12 or 13: remember nothing there rather than a choice that can never be
    // honoured, and never ask the Agent for a tap.
    if (! hs::systemSupportsAppCapture())
        return;

    {
        const juce::ScopedLock lock (stateLock);
        const int existing = findApp (bundleID, pid);

        if (existing >= 0)
        {
            savedPID[existing] = pid;

            if (bundleID.isNotEmpty())
                savedBundleID[existing] = bundleID;

            if (name.isNotEmpty())
                savedName[existing] = name;
        }
        else if (savedAppCount < hs::kSlotApps)
        {
            const int index = savedAppCount++;
            savedBundleID[index] = bundleID;
            savedName[index] = name;
            savedPID[index] = pid;
        }
        else
        {
            // The list is full: refresh the last entry, so the newest click is
            // the one that takes effect.
            const int index = hs::kSlotApps - 1;
            savedBundleID[index] = bundleID;
            savedName[index] = name;
            savedPID[index] = pid;
        }

        restorePending = false;
    }

    publish();
}

void CaptureProcessor::removeApp(int index)
{
    {
        const juce::ScopedLock lock (stateLock);

        if (index < 0 || index >= savedAppCount)
            return;

        for (int i = index; i + 1 < savedAppCount; ++i)
        {
            savedBundleID[i] = savedBundleID[i + 1];
            savedName[i] = savedName[i + 1];
            savedPID[i] = savedPID[i + 1];
        }

        --savedAppCount;
        savedBundleID[savedAppCount] = {};
        savedName[savedAppCount] = {};
        savedPID[savedAppCount] = -1;
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::clearApps()
{
    {
        const juce::ScopedLock lock (stateLock);
        savedAppCount = 0;

        for (int i = 0; i < hs::kSlotApps; ++i)
        {
            savedBundleID[i] = {};
            savedName[i] = {};
            savedPID[i] = -1;
        }

        restorePending = false;
    }

    publish();
}

void CaptureProcessor::setUsePair(bool use)
{
    {
        const juce::ScopedLock lock(stateLock);
        savedUsePair = use;
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::selectPair(int pairIndex)
{
    {
        const juce::ScopedLock lock(stateLock);
        savedPair = juce::jlimit(0, hs::kDevicePairs - 1, pairIndex);
        savedUsePair = true;
        restorePending = false;
    }

    publish();
}

void CaptureProcessor::publish()
{
    if (control() == nullptr)
        return;

    if (slot_ < 0)
        claimSlot();

    auto* slot = slotCtl();

    if (slot == nullptr)
        return;

    int count = 0;
    juce::String bundleID[hs::kSlotApps];
    juce::String name[hs::kSlotApps];
    int32_t pid[hs::kSlotApps] = {};
    int pair = hs::kDefaultCapturePair;
    bool usePair = true;

    {
        const juce::ScopedLock lock(stateLock);
        count = savedAppCount;

        for (int i = 0; i < hs::kSlotApps; ++i)
        {
            bundleID[i] = i < count ? savedBundleID[i] : juce::String();
            name[i] = i < count ? savedName[i] : juce::String();
            pid[i] = i < count ? savedPID[i] : -1;
        }

        pair = juce::jlimit(0, hs::kDevicePairs - 1, savedPair);
        usePair = savedUsePair;
    }

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));

    // Re-assert ownership: a plug-in that was restored from a project may sit in
    // the same slot for hours, and the Agent only ever leases it while the
    // heartbeat keeps coming.
    slot->owner.store(slotOwner_, std::memory_order_release);
    slot->heartbeat.store(hs::nowNs(), std::memory_order_release);

    for (int i = 0; i < hs::kSlotApps; ++i)
    {
        const int32_t entryPID = i < count ? pid[i] : -1;

        slot->appPID[i].store(entryPID, std::memory_order_relaxed);
        slot->appObjectID[i].store(entryPID > 0 ? processObjectForPID(entryPID) : 0u,
                                   std::memory_order_relaxed);
        hs::copyText(slot->appBundleID[i], hs::kRingNameSize, bundleID[i].toStdString());
        hs::copyText(slot->appName[i], hs::kRingNameSize, name[i].toStdString());
    }

    slot->appCount.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
    slot->pair.store(pair, std::memory_order_relaxed);
    slot->hostRate.store(rate, std::memory_order_relaxed);
    slot->mode.store(static_cast<uint32_t>(modeFor(count > 0, usePair)), std::memory_order_relaxed);
    slot->seq.fetch_add(1, std::memory_order_release);

    syncTapRings();

    for (auto& ring : tapRings)
        if (ring.isOpen())
            ring.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);
}

void CaptureProcessor::restore()
{
    int count = 0;
    juce::String bundleID[hs::kSlotApps];
    juce::String name[hs::kSlotApps];
    int32_t pid[hs::kSlotApps] = {};

    {
        const juce::ScopedLock lock(stateLock);

        if (! restorePending)
            return;

        const auto now = juce::Time::getMillisecondCounter();

        if (lastRestoreMs != 0 && now - lastRestoreMs < 3000)
            return;

        lastRestoreMs = now;
        count = savedAppCount;

        for (int i = 0; i < hs::kSlotApps; ++i)
        {
            bundleID[i] = i < count ? savedBundleID[i] : juce::String();
            name[i] = i < count ? savedName[i] : juce::String();
            pid[i] = i < count ? savedPID[i] : -1;
        }
    }

    bool anyPending = false;

    if (count > 0)
    {
        // An App is remembered by its bundle id, not by its pid: the pid changes
        // every time the App is restarted, and an App that is not playing
        // anything right now does not appear in the audio process list at all.
        const auto processes = listAudioProcesses(false);

        for (int i = 0; i < count; ++i)
        {
            if (pid[i] <= 0 && bundleID[i].isEmpty())
                continue;

            const AudioProcessInfo* match = nullptr;

            if (bundleID[i].isNotEmpty())
            {
                for (const auto& process : processes)
                {
                    if (juce::String::fromUTF8(process.bundleID.c_str()) != bundleID[i])
                        continue;

                    if (match == nullptr || (process.audioRunning && ! match->audioRunning))
                        match = &process;
                }
            }

            if (match == nullptr && pid[i] > 0)
            {
                for (const auto& process : processes)
                {
                    if (process.pid == pid[i])
                    {
                        match = &process;
                        break;
                    }
                }
            }

            if (match != nullptr)
            {
                addApp(match->pid,
                       juce::String::fromUTF8(match->bundleID.c_str()),
                       name[i].isNotEmpty() ? name[i] : juce::String::fromUTF8(match->name.c_str()));
            }
            else if (pid[i] > 0)
            {
                anyPending = true;   // the App is not up yet: keep trying
            }
        }
    }

    {
        const juce::ScopedLock lock(stateLock);
        restorePending = anyPending;
    }
}

//==================================================================================================
#pragma mark Timer
//==================================================================================================

void CaptureProcessor::timerCallback()
{
    // Losing the control block, or the Agent wiping a slot, must not take the
    // instance down: keep asking for a slot until one is granted.
    if (! ringsOpen())
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
    }

    if (slot_ >= 0)
    {
        const auto* slot = slotCtl();
        const bool lost = slot == nullptr
                          || slot->owner.load(std::memory_order_acquire) != slotOwner_;

        // The Agent wipes a slot when it dies or when it rebuilds the block, and
        // a block rebuilt by another process no longer carries our token. Drop
        // the index instead of keeping a slot that nobody knows about.
        if (lost)
        {
            for (auto& ring : tapRings)
                ring.close();

            channelRing.close();
            slot_ = -1;
        }
    }

    // An install replaces the service bundle, but the copy that is already
    // running keeps the old shared-memory layout: it answers at other offsets,
    // this instance never sees a heartbeat, and launching is a no-op because a
    // service with that bundle id is "already running". Ask the old copies to
    // quit so the launcher below can start the installed one.
    if (agentVersionStale())
    {
        ++staleServiceTicks;

        const uint64_t nowMs = hs::nowNs() / 1'000'000ull;

        // A second copy built against the old layout can keep re-writing the
        // block, so the restart is rate limited: the mismatch has to persist for
        // five seconds, and at most one restart per half minute. Two versions
        // fighting over the block must never kill the service in a loop.
        if (staleServiceTicks >= 25
            && (staleRestartAtMs == 0 || nowMs - staleRestartAtMs > 30'000))
        {
            staleServiceTicks = 0;
            staleRestartAtMs = nowMs;
            hs::restartAgent();
        }
    }
    else
    {
        staleServiceTicks = 0;
        staleRestartAtMs = 0;
    }

    if (slot_ < 0)
    {
        // A fresh slot starts empty, so hand it the selection this instance is
        // still showing: otherwise the rebuild that dropped the slot would also
        // silently drop the App the user picked.
        openRings();
        publish();
    }

    const auto rate = static_cast<uint32_t>(std::llround(hostRate_));

    if (auto* slot = slotCtl())
    {
        slot->heartbeat.store(hs::nowNs(), std::memory_order_release);

        // Process object IDs change when an App starts a new audio session even
        // while its pid stays the same.
        const uint32_t count = std::min<uint32_t>(slot->appCount.load(std::memory_order_relaxed),
                                                  static_cast<uint32_t>(hs::kSlotApps));

        bool needRestore = false;

        for (uint32_t i = 0; i < count; ++i)
        {
            const int32_t pid = slot->appPID[i].load(std::memory_order_relaxed);

            if (pid <= 0)
                continue;

            const uint32_t object = processObjectForPID(pid);

            // The App was restarted: the pid is gone but its identity is not.
            // Let restore() find it by bundle id so the user does not have to
            // pick it again every time.
            if (object == 0)
            {
                needRestore = true;
                continue;
            }

            if (object != slot->appObjectID[i].load(std::memory_order_relaxed))
            {
                slot->appObjectID[i].store(object, std::memory_order_relaxed);
                slot->seq.fetch_add(1, std::memory_order_release);
            }
        }

        if (needRestore)
        {
            const juce::ScopedLock lock(stateLock);
            restorePending = true;
            lastRestoreMs = 0;
        }
    }

    restore();

    for (auto& ring : tapRings)
        if (ring.isOpen())
            ring.setHostSampleRate(rate);

    if (channelRing.isOpen())
        channelRing.setHostSampleRate(rate);

    if (! agentAlive())
    {
        if (agentMissingTicks < 1000)
            ++agentMissingTicks;

        if (++launchTicks >= kLaunchIntervalTicks)
        {
            launchTicks = 0;
            hs::launchAgentIfNeeded();
        }
    }
    else
    {
        launchTicks = 0;
        agentMissingTicks = 0;
    }
}

void CaptureProcessor::reconnect()
{
    openRings();
    claimSlot();
    syncTapRings();

    for (auto& ring : tapRings)
        flushRing(ring);

    flushRing(channelRing);
    publish();
}

//==================================================================================================
#pragma mark Status
//==================================================================================================

int CaptureProcessor::appCount() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedAppCount;
}

juce::String CaptureProcessor::appName(int index) const
{
    const juce::ScopedLock lock(stateLock);

    if (index < 0 || index >= savedAppCount)
        return {};

    return savedName[index];
}

juce::String CaptureProcessor::appBundleID(int index) const
{
    const juce::ScopedLock lock(stateLock);

    if (index < 0 || index >= savedAppCount)
        return {};

    return savedBundleID[index];
}

int32_t CaptureProcessor::appPID(int index) const noexcept
{
    const juce::ScopedLock lock(stateLock);

    if (index < 0 || index >= savedAppCount)
        return -1;

    return savedPID[index];
}

hs::State CaptureProcessor::appState(int index) const noexcept
{
    const auto* slot = slotCtl();

    if (slot == nullptr || index < 0 || index >= hs::kSlotApps)
        return hs::State::Stopped;

    return static_cast<hs::State>(slot->appState[index].load(std::memory_order_relaxed));
}

uint32_t CaptureProcessor::appRate(int index) const noexcept
{
    const auto* slot = slotCtl();

    if (slot == nullptr || index < 0 || index >= hs::kSlotApps)
        return 0u;

    return slot->appRate[index].load(std::memory_order_relaxed);
}

bool CaptureProcessor::pairSelected() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedUsePair;
}

int CaptureProcessor::selectedPair() const noexcept
{
    const juce::ScopedLock lock(stateLock);
    return savedPair;
}

bool CaptureProcessor::agentAlive() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return false;

    const uint64_t heartbeat = c->heartbeat.load(std::memory_order_acquire);

    if (heartbeat == 0)
        return false;

    return hs::nowNs() - heartbeat < 2'000'000'000ull;
}

bool CaptureProcessor::agentVersionStale() const noexcept
{
    const auto* c = mapping.block();
    return c != nullptr && c->version.load(std::memory_order_relaxed) != hs::kRingVersion;
}

bool CaptureProcessor::agentLaunchStalled() const noexcept
{
    return agentMissingTicks >= 50; // 50 ticks at 200 ms: ten seconds without a service
}

hs::State CaptureProcessor::agentState() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<hs::State>(c->state.load(std::memory_order_relaxed))
                        : hs::State::Stopped;
}

hs::Fault CaptureProcessor::agentFault() const noexcept
{
    const auto* c = control();
    return c != nullptr ? static_cast<hs::Fault>(c->fault.load(std::memory_order_relaxed))
                        : hs::Fault::None;
}

hs::State CaptureProcessor::tapState() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? static_cast<hs::State>(slot->tapState.load(std::memory_order_relaxed))
                           : hs::State::Stopped;
}

hs::State CaptureProcessor::channelState() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? static_cast<hs::State>(slot->channelState.load(std::memory_order_relaxed))
                           : hs::State::Stopped;
}

hs::Fault CaptureProcessor::tapFault() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? static_cast<hs::Fault>(slot->tapFault.load(std::memory_order_relaxed))
                           : hs::Fault::None;
}

hs::Fault CaptureProcessor::channelFault() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? static_cast<hs::Fault>(slot->channelFault.load(std::memory_order_relaxed))
                           : hs::Fault::None;
}

int CaptureProcessor::audioCapturePermissionRaw() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return -1;

    const uint32_t raw = c->permissionRaw.load(std::memory_order_relaxed);
    return raw == 0xFFFFFFFFu ? -1 : static_cast<int>(raw);
}

int CaptureProcessor::microphonePermissionRaw() const noexcept
{
    const auto* c = control();

    if (c == nullptr)
        return -1;

    const uint32_t raw = c->microphoneRaw.load(std::memory_order_relaxed);
    return raw == 0xFFFFFFFFu ? -1 : static_cast<int>(raw);
}

float CaptureProcessor::tapPeak() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? slot->tapPeak.load(std::memory_order_relaxed) : 0.0f;
}

float CaptureProcessor::channelPeak() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? slot->channelPeak.load(std::memory_order_relaxed) : 0.0f;
}

float CaptureProcessor::outputPeak() const noexcept
{
    return outputPeakValue.load(std::memory_order_relaxed);
}

uint32_t CaptureProcessor::tapRate() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? slot->tapRate.load(std::memory_order_relaxed) : 0u;
}

uint32_t CaptureProcessor::channelRate() const noexcept
{
    return channelRing.isOpen() ? channelRing.sampleRate() : 0u;
}

float CaptureProcessor::tapGain() const noexcept
{
    const auto* slot = slotCtl();
    return slot != nullptr ? slot->tapGain.load(std::memory_order_relaxed) : 1.0f;
}

int CaptureProcessor::driftPPM() const noexcept
{
    // Several Apps run on several clocks: report the worst correction in use, so
    // the footer keeps showing something meaningful when they differ.
    double worst = 0.0;

    for (const auto& bridge : tapBridges)
    {
        const double ppm = bridge.appliedRatioPPM();

        if (std::abs(ppm) > std::abs(worst))
            worst = ppm;
    }

    return static_cast<int>(std::lround(worst));
}

uint64_t CaptureProcessor::underruns() const noexcept
{
    uint64_t total = channelBridge.underruns();

    for (const auto& bridge : tapBridges)
        total += bridge.underruns();

    return total;
}

#ifndef HS_PREVIEW
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CaptureProcessor();
}
#endif
