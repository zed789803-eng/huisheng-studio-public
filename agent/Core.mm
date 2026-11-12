#include "Core.h"

#include "AudioDevices.h"
#include "Control.h"
#include "Config.h"
#include "DriftBridge.h"
#include "License.h"
#include "Permissions.h"
#include "ProcessList.h"
#include "ProcessTap.h"
#include "RingPlayer.h"
#include "RingRecorder.h"
#include "SystemCompat.h"

#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr uint64_t kHeartbeatTimeout = 2'000'000'000ull;
constexpr uint64_t kRetryInterval = 2'000'000'000ull;

// A plug-in that is unloaded, or a DAW that has quit, stops beating. The shared
// block outlives both, so a stale beat means "nobody wants this path" - not
// "retry forever".
constexpr uint64_t kPluginTimeout = 5'000'000'000ull;

// A running tap is not rebuilt just because the App's process object id moved:
// a new private aggregate device disturbs every other App in the same slot, and
// some Apps (酷狗) re-register their audio session on every track change. The
// new id has to survive this long before the tap is rebuilt around it.
constexpr uint64_t kTapRestartCooldownNs = 5'000'000'000ull;

// The control block is a second shm object; it holds no audio, only atomics.
struct ControlMapping
{
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    hs::ControlBlock* block = nullptr;

    bool open()
    {
        if (block != nullptr)
            return true;

        size = sizeof(hs::ControlBlock);
        const std::string name = hs::controlName();

        fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);

        if (fd >= 0)
        {
            ::fchmod(fd, 0600);
            ::ftruncate(fd, static_cast<off_t>(size));
        }
        else
        {
            fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        }

        if (fd < 0)
            return false;

        base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (base == MAP_FAILED)
        {
            base = nullptr;
            ::close(fd);
            fd = -1;
            return false;
        }

        block = static_cast<hs::ControlBlock*>(base);

        if (block->magic.load(std::memory_order_acquire) != hs::kRingMagic)
        {
            new (block) hs::ControlBlock();
            block->version.store(hs::kRingVersion, std::memory_order_relaxed);
            block->magic.store(hs::kRingMagic, std::memory_order_release);
        }

        return true;
    }
};

// Every pair is two single-direction devices: the output half (SC) takes what
// an app plays into it, the input half (SR) hands the same audio back.
std::string pairOutputUID(int index)
{
    return index >= 0 && index < hs::kDevicePairs ? hs::kPairOutputUID[index] : std::string();
}

std::string pairInputUID(int index)
{
    return index >= 0 && index < hs::kDevicePairs ? hs::kPairInputUID[index] : std::string();
}

// Per-App capture runtime inside one slot. Each App keeps its own ring and its
// own tap, so a group of Apps can be tapped at once without one App's clock,
// gain or restart touching another's - and the plug-in sums the rings it reads.
struct SlotTapRuntime
{
    hs::RingBuffer ring;

    std::unique_ptr<ProcessTap> tap;

    uint32_t appliedObjectID = 0;
    int32_t appliedPID = 0;
    uint64_t lastGainCheck = 0;
    uint64_t lastRestartAt = 0;
};

// Per-slot capture runtime. One slot is one Capture plug-in instance: its own
// rings, its own taps and virtual-input reader, its own retry clocks. Two
// instances therefore never share a ring's read pointer (two consumers on one
// SPSC ring is what made several instances drift, glitch and click).
struct SlotRuntime
{
    hs::RingBuffer channelRing;
    SlotTapRuntime apps[hs::kSlotApps];

    std::unique_ptr<RingRecorder> channelReader;

    uint32_t appliedSeq = 0;
    int32_t appliedPair = -1;
    uint64_t lastTapAttempt = 0;
    uint64_t lastChannelAttempt = 0;
};

} // namespace

struct Core::Impl
{
    ControlMapping control;
    hs::RingBuffer returnRing;
    SlotRuntime slots[hs::kCaptureSlots];

    std::unique_ptr<RingPlayer> returnPlayer;

    uint32_t appliedReturnSeq = 0;
    int32_t appliedReturnPair = -1;
    uint32_t appliedReturnRate = 0;

    uint64_t lastHeartbeat = 0;
    uint64_t lastReturnAttempt = 0;
    uint64_t lastPermissionCheck = 0;
    uint64_t lastSlotLog = 0;

    bool micPrompted = false;
    bool tapPrompted = false;
    int lastMicRaw = -1;

    // 注册策略：kEnforcement == 1 时，没注册码就不启动采集。文件最多每秒读一次，
    // 这条路径跑在 10 Hz 的 tick 上，不会把文件 IO 拖到音频上下文里。
    // kEnforcement 默认是 0（只提示、不掐声音），这个判断就完全不生效。
    bool licensed = true;
    uint64_t lastLicenseCheck = 0;

    bool licenceAllowsCapture()
    {
        if (hs::license::kEnforcement <= 0)
            return true;

        const uint64_t now = hs::nowNs();

        if (now - lastLicenseCheck > 1'000'000'000ull)
        {
            lastLicenseCheck = now;
            licensed = hs::license::current().valid;
        }

        return licensed;
    }

    hs::ControlBlock* ctl() const { return control.block; }

    void publish(hs::State state, hs::Fault fault)
    {
        if (auto* c = ctl())
        {
            c->state.store(static_cast<uint32_t>(state), std::memory_order_release);
            c->fault.store(static_cast<uint32_t>(fault), std::memory_order_release);
        }
    }

    void publishSource(std::atomic<uint32_t>& stateField, std::atomic<uint32_t>& faultField,
                       hs::State state, hs::Fault fault)
    {
        stateField.store(static_cast<uint32_t>(state), std::memory_order_release);
        faultField.store(static_cast<uint32_t>(fault), std::memory_order_release);
    }

    // One App's line inside a slot. A slot can hold several Apps, each with its
    // own tap, so each publishes its own state and the plug-in can say which App
    // is the one that is not running.
    static void publishApp(hs::CaptureSlot* slot, int app, hs::State state, hs::Fault fault)
    {
        if (slot == nullptr || app < 0 || app >= hs::kSlotApps)
            return;

        slot->appState[app].store(static_cast<uint32_t>(state), std::memory_order_release);
        slot->appFault[app].store(static_cast<uint32_t>(fault), std::memory_order_release);
    }

    bool openRings()
    {
        if (! returnRing.isOpen())
        {
            if (! (returnRing.open(hs::returnRingName())
                   || returnRing.create(hs::returnRingName(), hs::kDefaultRate)))
                return false;
        }

        return returnRing.isOpen();
    }

    // Slot rings are created on demand: a slot nobody uses costs nothing, and a
    // slot that captures one App maps one tap ring instead of all eight. Rings
    // for App entries the plug-in has dropped are closed again, together with
    // the tap that was writing into them.
    bool ensureSlotRings(int index)
    {
        auto& slot = slots[index];
        const int count = std::min<int>(hs::kSlotApps,
                                        static_cast<int>(slotCtl(index)->appCount.load(
                                            std::memory_order_relaxed)));

        if (! slot.channelRing.isOpen())
        {
            if (! (slot.channelRing.open(hs::slotChannelRingName(index))
                   || slot.channelRing.create(hs::slotChannelRingName(index), hs::kDefaultRate)))
                return false;
        }

        for (int a = 0; a < hs::kSlotApps; ++a)
        {
            auto& entry = slot.apps[a];

            if (a < count)
            {
                if (entry.ring.isOpen())
                    continue;

                const std::string name = hs::slotTapRingName(index, a);

                if (! (entry.ring.open(name) || entry.ring.create(name, hs::kDefaultRate)))
                    return false;
            }
            else if (entry.tap != nullptr || entry.ring.isOpen())
            {
                entry.tap.reset();
                entry.ring.close();
                entry.appliedObjectID = 0;
                entry.appliedPID = 0;
            }
        }

        return true;
    }

    hs::CaptureSlot* slotCtl(int index) const
    {
        return &ctl()->slots[index];
    }

    // A slot counts as taken while its plug-in keeps beating. The lease is what
    // makes an unloaded or crashed instance release its own tap instead of
    // leaving an App muted for every other instance.
    bool slotLeased(int index, uint64_t now) const
    {
        const auto* slot = slotCtl(index);

        if (slot->owner.load(std::memory_order_acquire) == 0)
            return false;

        const uint64_t beat = slot->heartbeat.load(std::memory_order_acquire);
        return beat != 0 && now - beat <= hs::kSlotLeaseNanos;
    }

    hs::CaptureMode slotMode(int index) const
    {
        const auto raw = slotCtl(index)->mode.load(std::memory_order_relaxed);
        return raw <= 3 ? static_cast<hs::CaptureMode>(raw) : hs::CaptureMode::Off;
    }

    // True while one of the slot's Apps wants a tap but has none: the only case
    // worth retrying, because every attempt builds an aggregate device.
    bool slotTapIncomplete(int index) const
    {
        const auto& slot = slots[index];
        const auto* c = slotCtl(index);
        const int count = std::min<int>(hs::kSlotApps,
                                        static_cast<int>(c->appCount.load(std::memory_order_relaxed)));

        for (int a = 0; a < count; ++a)
        {
            if (c->appPID[a].load(std::memory_order_relaxed) > 0 && ! slot.apps[a].tap)
                return true;
        }

        return false;
    }

    // True while the slot still drives anything at all.
    bool slotHasEngine(int index) const
    {
        if (slots[index].channelReader)
            return true;

        for (const auto& entry : slots[index].apps)
        {
            if (entry.tap)
                return true;
        }

        return false;
    }

    int leasedSlots(uint64_t now) const
    {
        int count = 0;

        for (int i = 0; i < hs::kCaptureSlots; ++i)
            if (slotLeased(i, now))
                ++count;

        return count;
    }

    // True while a live slot reads the given pair through the virtual input:
    // writing the DAW mix back into that same pair would close a software
    // feedback loop around the microphone.
    bool captureUsesPair(int32_t pair, uint64_t now) const
    {
        for (int i = 0; i < hs::kCaptureSlots; ++i)
        {
            if (! slotLeased(i, now))
                continue;

            const auto mode = slotMode(i);

            if ((mode == hs::CaptureMode::Channel || mode == hs::CaptureMode::Both)
                && slotCtl(i)->pair.load(std::memory_order_relaxed) == pair)
                return true;
        }

        return false;
    }

    void releaseSlot(int index, const char* reason)
    {
        auto& slot = slots[index];
        auto* c = slotCtl(index);
        bool had = slot.channelReader != nullptr;

        for (auto& entry : slot.apps)
        {
            if (entry.tap)
                had = true;

            entry.tap.reset();
            entry.appliedObjectID = 0;
            entry.appliedPID = 0;
            entry.lastGainCheck = 0;
        }

        if (had)
            NSLog(@"huishengstudio: capture slot %d released (%s)", index, reason);

        slot.channelReader.reset();
        slot.appliedPair = -1;
        // Re-arm the sequence comparison so the next owner's first change is
        // seen even if it reuses the value the previous owner left behind.
        slot.appliedSeq = c->seq.load(std::memory_order_acquire);

        publishSource(c->tapState, c->tapFault, hs::State::Stopped, hs::Fault::None);
        publishSource(c->channelState, c->channelFault, hs::State::Stopped, hs::Fault::None);
        c->tapRate.store(0, std::memory_order_relaxed);
        c->tapGain.store(1.0f, std::memory_order_relaxed);
        c->tapPeak.store(0.0f, std::memory_order_relaxed);
        c->channelPeak.store(0.0f, std::memory_order_relaxed);

        for (int a = 0; a < hs::kSlotApps; ++a)
        {
            c->appState[a].store(static_cast<uint32_t>(hs::State::Stopped), std::memory_order_relaxed);
            c->appFault[a].store(static_cast<uint32_t>(hs::Fault::None), std::memory_order_relaxed);
            c->appRate[a].store(0, std::memory_order_relaxed);
            c->appPeak[a].store(0.0f, std::memory_order_relaxed);
        }
    }

    // Start (or stop) the process taps that belong to one capture slot: one tap
    // per App the plug-in listed, each writing into its own ring.
    //
    // Every tap runs at its own App's device rate; the rate the DAW runs at is
    // the plug-in's business, because the plug-in resamples each ring on its own.
    // Readiness therefore never compares rates: comparing them used to restart
    // the tap every retry on a 44.1 kHz App in a 48 kHz project, which is exactly
    // the stutter the App showed there.
    void applySlotTap(int index)
    {
        auto& slot = slots[index];
        auto* c = slotCtl(index);
        const int count = std::min<int>(hs::kSlotApps,
                                        static_cast<int>(c->appCount.load(std::memory_order_relaxed)));

        slot.lastTapAttempt = hs::nowNs();

        const auto stopAll = [&slot]()
        {
            for (auto& entry : slot.apps)
            {
                entry.tap.reset();
                entry.appliedObjectID = 0;
                entry.appliedPID = 0;
            }
        };

        if (! licenceAllowsCapture())
        {
            stopAll();

            for (int a = 0; a < hs::kSlotApps; ++a)
                publishApp(c, a, hs::State::Denied, hs::Fault::Unlicensed);

            publishSource(c->tapState, c->tapFault, hs::State::Denied, hs::Fault::Unlicensed);
            return;
        }

        const auto mode = slotMode(index);
        const bool wantApp = mode == hs::CaptureMode::App || mode == hs::CaptureMode::Both;

        if (! wantApp || count == 0)
        {
            stopAll();

            for (int a = 0; a < hs::kSlotApps; ++a)
                publishApp(c, a, hs::State::Stopped, hs::Fault::None);

            publishSource(c->tapState, c->tapFault, hs::State::Stopped, hs::Fault::None);
            return;
        }

        // macOS 12 and 13 have no process taps. Keep the state honest instead of
        // retrying an API that cannot exist there: the plug-in shows 不支持 and
        // the user can still run 虚拟通道.
        if (! hs::systemSupportsAppCapture())
        {
            NSLog(@"huishengstudio: App capture needs macOS 14.2 or newer (running %s)",
                  hs::systemVersionText().c_str());

            stopAll();

            for (int a = 0; a < hs::kSlotApps; ++a)
                publishApp(c, a, hs::State::Denied, hs::Fault::Unsupported);

            publishSource(c->tapState, c->tapFault, hs::State::Denied, hs::Fault::Unsupported);
            return;
        }

        const uint32_t hostRate = c->hostRate.load(std::memory_order_relaxed);
        bool anyRunning = false;
        bool anyWaiting = false;
        bool anyError = false;
        hs::Fault firstFault = hs::Fault::None;

        for (int a = 0; a < hs::kSlotApps; ++a)
        {
            auto& entry = slot.apps[a];

            // Entries the plug-in dropped: ensureSlotRings already closed their
            // rings, so only the leftover engine needs clearing here.
            if (a >= count)
            {
                entry.tap.reset();
                entry.appliedObjectID = 0;
                entry.appliedPID = 0;
                continue;
            }

            const int32_t pid = c->appPID[a].load(std::memory_order_relaxed);
            const uint32_t object = pid > 0 ? processObjectForPID(pid) : 0;
            const uint64_t nowNs = hs::nowNs();

            // Already tapping this very process: leave it alone, restarting it
            // would drop whatever it is playing right now.
            const bool sameProcess = object != 0 && entry.appliedPID == pid
                                     && entry.appliedObjectID == object;

            // A process object id can change while the tap itself is still
            // perfectly good: an App like 酷狗 re-registers its audio session
            // whenever it switches track, and an object id can also flap while
            // the App opens or closes its output. Rebuilding the private
            // aggregate device on every change disturbs the other Apps that
            // share this slot - which is exactly the "酷狗 and another App
            // together do not work" report. A tap that is still running is kept
            // until it really stops, or until the new id has survived longer
            // than the cooldown.
            const bool cooling = entry.lastRestartAt != 0
                                 && nowNs - entry.lastRestartAt < kTapRestartCooldownNs;

            if (entry.tap && entry.tap->isRunning() && (sameProcess || (object != 0 && cooling)))
            {
                if (! sameProcess)
                {
                    entry.appliedPID = pid;
                    entry.appliedObjectID = object;
                }

                anyRunning = true;
                publishApp(c, a, hs::State::Running, hs::Fault::None);
                continue;
            }

            entry.tap.reset();
            entry.lastRestartAt = nowNs;
            entry.appliedObjectID = 0;
            entry.appliedPID = 0;

            if (pid <= 0)
            {
                publishApp(c, a, hs::State::Stopped, hs::Fault::None);
                continue;
            }

            if (object == 0)
            {
                anyWaiting = true;
                publishApp(c, a, hs::State::Waiting, hs::Fault::ProcessGone);
                continue;
            }

            publishApp(c, a, hs::State::Starting, hs::Fault::None);

            auto candidate = std::make_unique<ProcessTap>();
            std::string error;
            const std::string name = c->appName[a][0] != '\0' ? std::string(c->appName[a]) : "App";

            if (! candidate->start(object, entry.ring, name, error))
            {
                NSLog(@"huishengstudio: slot %d app %d tap failed: %s", index, a, error.c_str());

                const bool gone = error.find("object") != std::string::npos;

                if (gone)
                    anyWaiting = true;
                else
                    anyError = true;

                if (firstFault == hs::Fault::None)
                    firstFault = gone ? hs::Fault::ProcessGone : hs::Fault::TapFailed;

                publishApp(c, a, gone ? hs::State::Waiting : hs::State::Error,
                           gone ? hs::Fault::ProcessGone : hs::Fault::TapFailed);
                continue;
            }

            const double tapRate = candidate->sampleRate();
            entry.tap = std::move(candidate);
            entry.appliedObjectID = object;
            entry.appliedPID = pid;
            anyRunning = true;
            publishApp(c, a, hs::State::Running, hs::Fault::None);
            NSLog(@"huishengstudio: slot %d app %d tapping %s (pid %d, %u ch @ %.0f Hz, project %.0f Hz)",
                  index, a, name.c_str(), (int) pid, entry.tap->tapChannels(), tapRate,
                  static_cast<double>(hostRate));
        }

        // The slot's own state is the summary of its Apps: one App in trouble is
        // enough for the plug-in to say so, and each App's own line says which.
        const auto state = anyError ? hs::State::Error
                                    : (anyRunning ? hs::State::Running
                                                  : (anyWaiting ? hs::State::Waiting
                                                                : hs::State::Stopped));
        const auto fault = anyError ? firstFault
                                    : (anyWaiting && ! anyRunning ? hs::Fault::ProcessGone
                                                                  : hs::Fault::None);
        publishSource(c->tapState, c->tapFault, state, fault);
    }

    void applySlotChannel(int index)
    {
        auto& slot = slots[index];
        auto* c = slotCtl(index);

        if (! licenceAllowsCapture())
        {
            slot.channelReader.reset();
            slot.appliedPair = -1;
            publishSource(c->channelState, c->channelFault, hs::State::Denied, hs::Fault::Unlicensed);
            return;
        }

        const auto mode = slotMode(index);
        const bool wantChannel = mode == hs::CaptureMode::Channel || mode == hs::CaptureMode::Both;
        const int32_t rawPair = c->pair.load(std::memory_order_relaxed);
        const int32_t pair = rawPair >= 0 && rawPair < hs::kDevicePairs ? rawPair
                                                                        : hs::kDefaultCapturePair;
        const uint32_t hostRate = c->hostRate.load(std::memory_order_relaxed);

        if (! wantChannel)
        {
            slot.channelReader.reset();
            slot.appliedPair = -1;
            publishSource(c->channelState, c->channelFault, hs::State::Stopped, hs::Fault::None);
            return;
        }

        if (slot.channelReader && slot.appliedPair == pair)
        {
            publishSource(c->channelState, c->channelFault, hs::State::Running, hs::Fault::None);
            return;
        }

        slot.channelReader.reset();
        slot.appliedPair = -1;
        slot.lastChannelAttempt = hs::nowNs();
        publishSource(c->channelState, c->channelFault, hs::State::Starting, hs::Fault::None);

        // The virtual input is asked for the project rate; if the driver hands
        // back another one we record at that rate and declare it there, because
        // the plug-in resamples a declared rate instead of refusing it.
        if (hostRate != 0)
            slot.channelRing.setSampleRate(hostRate);

        auto candidate = std::make_unique<RingRecorder>();
        std::string error;
        const std::string name = std::string(hs::kPairInputName[pair]);

        if (! candidate->start(pairInputUID(pair), slot.channelRing, name, error))
        {
            NSLog(@"huishengstudio: slot %d virtual input failed: %s", index, error.c_str());
            const bool missing = error.find("not found") != std::string::npos;

            // RingRecorder reports the raw OSStatus. -12660 is
            // kAudioHardwareSecuritySessionNotPermitted (no microphone grant);
            // the HAL device still opens, it just delivers silence.
            const bool denied = error.find("-12660") != std::string::npos;

            publishSource(c->channelState, c->channelFault,
                          missing ? hs::State::Waiting
                                  : (denied ? hs::State::Denied : hs::State::Error),
                          missing ? hs::Fault::DeviceMissing
                                  : (denied ? hs::Fault::NoPermission : hs::Fault::InputFailed));
            return;
        }

        slot.channelReader = std::move(candidate);
        slot.appliedPair = pair;
        publishSource(c->channelState, c->channelFault, hs::State::Running, hs::Fault::None);
        NSLog(@"huishengstudio: slot %d reading %s (%.0f Hz)", index, name.c_str(),
              slot.channelReader->sampleRate());
    }

    void applyReturn()
    {
        auto* c = ctl();

        // 回传也要过注册：不然没注册的机器照样能把宿主混音送进虚拟设备，
        // 只拦采集等于没拦。
        if (! licenceAllowsCapture())
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, hs::State::Denied, hs::Fault::Unlicensed);
            return;
        }

        const int32_t rawPair = c->returnPair.load(std::memory_order_relaxed);
        const int32_t pair = rawPair >= 0 && rawPair < hs::kDevicePairs ? rawPair
                                                                        : hs::kDefaultReturnPair;
        const uint32_t hostRate = c->returnHostRate.load(std::memory_order_relaxed);

        if (pair < 0 || pair >= hs::kDevicePairs)
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, hs::State::Stopped, hs::Fault::None);
            return;
        }

        // Never write the DAW mix back into a pair some live Capture instance is
        // reading: that closes a software feedback loop around the microphone.
        if (captureUsesPair(pair, hs::nowNs()))
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, hs::State::Error, hs::Fault::NoRoute);
            return;
        }

        // Only run the virtual output while a Return plug-in is actually alive.
        // The control block survives the DAW, so without this check a beat left
        // behind by a previous session would restart the player every two
        // seconds and tear it down again on the next tick.
        const uint64_t beat = c->returnHeartbeat.load(std::memory_order_acquire);

        if (beat == 0 || hs::nowNs() - beat > kPluginTimeout)
        {
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, hs::State::Stopped, hs::Fault::None);
            return;
        }

        if (returnPlayer && appliedReturnPair == pair)
        {
            publishSource(c->returnState, c->returnFault, hs::State::Running, hs::Fault::None);
            return;
        }

        returnPlayer.reset();
        appliedReturnPair = -1;
        lastReturnAttempt = hs::nowNs();
        publishSource(c->returnState, c->returnFault, hs::State::Starting, hs::Fault::None);

        if (hostRate != 0)
            returnRing.setSampleRate(hostRate);

        auto candidate = std::make_unique<RingPlayer>();
        std::string error;

        if (! candidate->start(pairOutputUID(pair), returnRing, error))
        {
            NSLog(@"huishengstudio: return output failed: %s", error.c_str());
            publishSource(c->returnState, c->returnFault, hs::State::Waiting, hs::Fault::DeviceMissing);
            return;
        }

        returnPlayer = std::move(candidate);
        appliedReturnPair = pair;
        publishSource(c->returnState, c->returnFault, hs::State::Running, hs::Fault::None);
        NSLog(@"huishengstudio: DAW mix -> %s", hs::kPairOutputName[pair]);
    }

    void updatePeaks()
    {
        auto* c = ctl();
        const uint64_t now = hs::nowNs();
        int active = 0;

        for (int i = 0; i < hs::kCaptureSlots; ++i)
        {
            auto& slot = slots[i];

            if (slotLeased(i, now))
                ++active;

            auto* sc = slotCtl(i);
            const int count = std::min<int>(hs::kSlotApps,
                                            static_cast<int>(sc->appCount.load(std::memory_order_relaxed)));
            float appSum = 0.0f;
            uint32_t commonRate = 0;
            bool ratesAgree = true;

            for (int a = 0; a < hs::kSlotApps; ++a)
            {
                auto& entry = slot.apps[a];
                float peak = 0.0f;
                uint32_t rate = 0;

                if (a < count && entry.ring.isOpen())
                {
                    peak = entry.ring.header()->peak.load(std::memory_order_relaxed);
                    rate = entry.ring.sampleRate();
                }

                sc->appPeak[a].store(peak, std::memory_order_relaxed);
                sc->appRate[a].store(rate, std::memory_order_relaxed);
                appSum += peak;

                if (rate != 0)
                {
                    if (commonRate == 0)
                        commonRate = rate;
                    else if (rate != commonRate)
                        ratesAgree = false;
                }
            }

            // The slot's tap meter is the sum of its Apps - what the plug-in
            // actually adds into the track - and the slot rate is only reported
            // when every App agrees on it (the plug-in shows each App's own rate
            // either way).
            sc->tapPeak.store(appSum, std::memory_order_relaxed);
            sc->tapRate.store(ratesAgree ? commonRate : 0u, std::memory_order_relaxed);

            if (slot.channelRing.isOpen())
            {
                sc->channelPeak.store(slot.channelRing.header()->peak.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            }
        }

        if (returnRing.isOpen())
            c->returnPeak.store(returnRing.header()->peak.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);

        c->returnActive.store(returnPlayer && returnPlayer->isRunning() ? 1u : 0u,
                              std::memory_order_relaxed);
        c->activeSlots.store(static_cast<uint32_t>(active), std::memory_order_relaxed);
    }

    void updateOverall()
    {
        auto* c = ctl();
        hs::State overall = hs::State::Stopped;
        hs::Fault fault = hs::Fault::None;

        bool anySelected = false;
        bool anyError = false;
        bool anyStarting = false;
        bool anyRunning = false;

        for (int i = 0; i < hs::kCaptureSlots; ++i)
        {
            const auto* slot = slotCtl(i);
            const auto tapState = static_cast<hs::State>(slot->tapState.load(std::memory_order_relaxed));
            const auto channelState =
                static_cast<hs::State>(slot->channelState.load(std::memory_order_relaxed));

            if (tapState != hs::State::Stopped || channelState != hs::State::Stopped)
                anySelected = true;

            anyError = anyError || tapState == hs::State::Error || channelState == hs::State::Error;
            anyStarting = anyStarting || tapState == hs::State::Starting
                          || channelState == hs::State::Starting;
            anyRunning = anyRunning || tapState == hs::State::Running
                         || channelState == hs::State::Running;
        }

        const auto retState = static_cast<hs::State>(c->returnState.load(std::memory_order_relaxed));

        if (retState != hs::State::Stopped)
            anySelected = true;

        anyError = anyError || retState == hs::State::Error;
        anyStarting = anyStarting || retState == hs::State::Starting;
        anyRunning = anyRunning || retState == hs::State::Running;

        if (! anySelected)
        {
            overall = hs::State::Stopped;
        }
        else if (anyError)
        {
            overall = hs::State::Error;
        }
        else if (anyStarting)
        {
            overall = hs::State::Starting;
        }
        else if (anyRunning)
        {
            overall = hs::State::Running;
        }
        else
        {
            overall = hs::State::Waiting;
        }

        c->state.store(static_cast<uint32_t>(overall), std::memory_order_release);
        c->fault.store(static_cast<uint32_t>(fault), std::memory_order_release);
    }

    void tick()
    {
        auto* c = ctl();

        if (c == nullptr || ! openRings())
            return;

        c->heartbeat.store(hs::nowNs(), std::memory_order_release);

        const uint64_t now = hs::nowNs();

        // A crashed or unloaded plug-in must not leave an App muted. Every slot
        // is leased by its own heartbeat, so an expired lease releases that
        // slot's sources only; the other instances keep running untouched.
        for (int i = 0; i < hs::kCaptureSlots; ++i)
        {
            if (! slotLeased(i, now) && slotHasEngine(i))
                releaseSlot(i, "plug-in gone");
        }

        const uint64_t returnBeat = c->returnHeartbeat.load(std::memory_order_acquire);
        const bool returnPluginLive = returnBeat != 0 && now - returnBeat <= kPluginTimeout;

        if (! returnPluginLive && returnPlayer != nullptr)
        {
            NSLog(@"huishengstudio: return plug-in stopped; releasing the virtual microphone");
            returnPlayer.reset();
            appliedReturnPair = -1;
            publishSource(c->returnState, c->returnFault, hs::State::Stopped, hs::Fault::None);
        }

        // Permissions: cheap to read, but TCC can block while a prompt is
        // pending, so poll at most once per second.
        if (now - lastPermissionCheck > 1'000'000'000ull)
        {
            lastPermissionCheck = now;
            const int raw = audioCapturePermissionRaw();
            c->permissionRaw.store(raw < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(raw),
                                   std::memory_order_relaxed);

            const int mic = microphonePermissionRaw();
            c->microphoneRaw.store(mic < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(mic),
                                   std::memory_order_relaxed);

            // Prompt once on our own as soon as a source that needs a grant is
            // selected. Relying on the installer alone is not enough: a user
            // who skipped the installer's prompt, or installed with the DAW
            // open, would otherwise only ever see "需要系统权限".
            bool wantsMic = false;
            bool wantsTap = false;

            for (int i = 0; i < hs::kCaptureSlots; ++i)
            {
                if (! slotLeased(i, now))
                    continue;

                const auto mode = slotMode(i);
                wantsMic = wantsMic || mode == hs::CaptureMode::Channel
                           || mode == hs::CaptureMode::Both;
                wantsTap = wantsTap || mode == hs::CaptureMode::App
                           || mode == hs::CaptureMode::Both;
            }

            if (! micPrompted && wantsMic && mic == 1)
            {
                micPrompted = true;
                NSLog(@"huishengstudio: asking for microphone access");
                requestMicrophonePermission([this](bool granted)
                {
                    NSLog(@"huishengstudio: microphone access %s", granted ? "granted" : "denied");
                    lastPermissionCheck = 0;
                });
            }

            if (! tapPrompted && wantsTap && raw == 1)
            {
                tapPrompted = true;
                NSLog(@"huishengstudio: asking for system audio recording access");
                requestAudioCapturePermission([this](bool granted)
                {
                    NSLog(@"huishengstudio: system audio recording %s", granted ? "granted" : "denied");
                    lastPermissionCheck = 0;
                });
            }

            if (mic == 2 && lastMicRaw != 2)
            {
                // The virtual inputs were opened before the grant landed.
                for (int i = 0; i < hs::kCaptureSlots; ++i)
                {
                    slots[i].channelReader.reset();
                    slots[i].appliedPair = -1;
                }
            }

            lastMicRaw = mic;
        }

        // Per slot: react to a selection change, then retry a source that is not
        // running, but never more often than every two seconds, because each
        // attempt creates and destroys an aggregate device.
        for (int i = 0; i < hs::kCaptureSlots; ++i)
        {
            if (! slotLeased(i, now) || ! ensureSlotRings(i))
                continue;

            auto& slot = slots[i];
            auto* sc = slotCtl(i);
            const uint32_t seq = sc->seq.load(std::memory_order_acquire);

            if (seq != slot.appliedSeq)
            {
                slot.appliedSeq = seq;
                applySlotTap(i);
                applySlotChannel(i);
                applyReturn();  // a pair may have just become (un)claimed
            }

            const auto mode = slotMode(i);
            const bool wantTap = mode == hs::CaptureMode::App || mode == hs::CaptureMode::Both;
            const bool wantChannel = mode == hs::CaptureMode::Channel
                                     || mode == hs::CaptureMode::Both;

            if (wantTap && slotTapIncomplete(i) && now - slot.lastTapAttempt > kRetryInterval)
                applySlotTap(i);

            if (wantChannel && ! slot.channelReader
                && now - slot.lastChannelAttempt > kRetryInterval)
                applySlotChannel(i);

            // Process objects move between audio sessions; re-resolve the gain
            // so the tap does not end up scaled for a dead channel layout.
            for (auto& entry : slot.apps)
            {
                if (entry.tap && now - entry.lastGainCheck > 1'000'000'000ull)
                {
                    entry.lastGainCheck = now;
                    entry.tap->refreshGain();
                }
            }
        }

        const uint32_t returnSeq = c->returnSeq.load(std::memory_order_acquire);

        if (returnSeq != appliedReturnSeq)
        {
            appliedReturnSeq = returnSeq;
            applyReturn();
        }

        if (! returnPlayer && returnPluginLive && now - lastReturnAttempt > kRetryInterval)
            applyReturn();

        updatePeaks();
        updateOverall();
    }
};

Core::Core() : impl(std::make_unique<Impl>()) {}

Core::~Core()
{
    for (auto& slot : impl->slots)
    {
        for (auto& entry : slot.apps)
        {
            if (entry.tap)
                entry.tap->stop();
        }

        if (slot.channelReader) slot.channelReader->stop();
    }

    if (impl->returnPlayer) impl->returnPlayer->stop();
}

bool Core::open()
{
    if (! impl->control.open())
        return false;

    auto* c = impl->ctl();

    if (! impl->openRings())
        return false;

    // The capture side needs no defaults any more: every Capture instance claims
    // its own slot and publishes its own selection there. The return side still
    // shares one virtual output, so seed that once.
    if (c->returnSeq.load(std::memory_order_acquire) == 0)
    {
        c->returnPair.store(hs::kDefaultReturnPair, std::memory_order_relaxed);
        c->returnSeq.store(1, std::memory_order_release);
    }

    impl->appliedReturnSeq = c->returnSeq.load(std::memory_order_acquire);

    for (int i = 0; i < hs::kCaptureSlots; ++i)
        impl->slots[i].appliedSeq = impl->slotCtl(i)->seq.load(std::memory_order_acquire);

    impl->applyReturn();
    impl->updateOverall();
    return true;
}

void Core::tick()
{
    impl->tick();
}

void Core::requestInstallPermissions()
{
    impl->micPrompted = true;
    impl->tapPrompted = true;

    requestMicrophonePermission([](bool granted) {
        NSLog(@"huishengstudio: microphone access %s", granted ? "granted" : "denied");
    });

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        requestAudioCapturePermission([](bool granted) {
            NSLog(@"huishengstudio: system audio recording %s", granted ? "granted" : "denied");
        });
    });
}

hs::ControlBlock* Core::control() const noexcept
{
    return impl->ctl();
}
