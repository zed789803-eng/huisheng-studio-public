// Capture slots: one ring per App, per plug-in instance.
//
// This covers the failure that shipped before the slot split: every instance
// shared "/huishengstudio.<uid>.tap", so a second instance advanced the same
// readFrames as the first and stole its frames — audible as dropouts and clicks
// as soon as a project held more than one instance. Then a slot captured a single
// App, so "several platforms into one track" needed several tracks. Now a slot
// carries a list of Apps, each with its own tap ring, and the plug-in sums the
// rings it reads: names must stay unique and short enough for PSHMNAMLEN, and no
// two Apps may share a ring.

#include "Config.h"
#include "Control.h"
#include "RingBuffer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());

    if (! condition)
        ++failures;
}

} // namespace

int main()
{
    // 1. One name per App per slot: unique, and inside the 30 characters shm_open
    //    accepts on macOS.
    std::vector<std::string> tapNames;
    std::vector<std::string> channelNames;

    for (int slot = 0; slot < hs::kCaptureSlots; ++slot)
    {
        const auto channel = hs::slotChannelRingName(slot);

        check(channel.size() <= 30, "slot " + std::to_string(slot) + " channel name fits PSHMNAMLEN");
        check(channel != hs::returnRingName(),
              "slot " + std::to_string(slot) + " channel name differs from the return ring");

        for (const auto& seen : channelNames)
            check(seen != channel, "channel name unique: " + channel);

        channelNames.push_back(channel);

        for (int app = 0; app < hs::kSlotApps; ++app)
        {
            const auto tap = hs::slotTapRingName(slot, app);

            check(tap.size() <= 30,
                  "slot " + std::to_string(slot) + " app " + std::to_string(app)
                      + " name fits PSHMNAMLEN");
            check(tap != channel,
                  "slot " + std::to_string(slot) + " app " + std::to_string(app)
                      + " name differs from its channel ring");
            check(tap != hs::returnRingName(),
                  "slot " + std::to_string(slot) + " app " + std::to_string(app)
                      + " name differs from the return ring");

            for (const auto& seen : tapNames)
                check(seen != tap, "tap name unique: " + tap);

            tapNames.push_back(tap);
        }
    }

    check(hs::kCaptureSlots >= 2, "at least two capture slots exist");
    check(hs::kSlotApps >= 2, "a slot can hold at least two Apps");
    check(tapNames.size() == static_cast<size_t>(hs::kCaptureSlots) * hs::kSlotApps,
          "every slot/App pair has its own name");

    // 2. Two slots are two independent rings, and so are two Apps inside one
    //    slot: that is what keeps one platform from stealing another's frames.
    const auto first = hs::slotTapRingName(0, 0);
    const auto second = hs::slotTapRingName(1, 0);
    const auto sameSlotSecondApp = hs::slotTapRingName(0, 1);

    hs::RingBuffer a;
    hs::RingBuffer b;
    hs::RingBuffer c;
    hs::RingBuffer::unlink(first);
    hs::RingBuffer::unlink(second);
    hs::RingBuffer::unlink(sameSlotSecondApp);

    check(a.create(first, 48000), "create slot 0 App 0 tap ring");
    check(b.create(second, 48000), "create slot 1 App 0 tap ring");
    check(c.create(sameSlotSecondApp, 48000), "create slot 0 App 1 tap ring");

    std::vector<float> frames(256 * 2, 0.25f);
    check(a.write(frames.data(), 256) == 256, "slot 0 App 0 accepts a block");
    check(a.available() == 256, "slot 0 App 0 holds the block");
    check(b.available() == 0, "slot 1 never sees slot 0's audio");
    check(c.available() == 0, "a second App in the same slot never sees the first one's audio");

    // 3. The source rate lives on the ring, which is what lets the plug-in
    //    resample a 44.1 kHz App inside a 48 kHz project instead of muting it.
    a.resetForProducer(44100, "slot-telemetry");
    c.resetForProducer(48000, "slot-telemetry-2");
    check(a.sampleRate() == 44100, "slot 0 App 0 publishes the rate its App runs at");
    check(c.sampleRate() == 48000, "two Apps in one slot keep separate rates");

    // 4. The control block carries one independent App list per slot.
    hs::ControlMapping mapping;
    check(mapping.open(), "open control block");

    auto* control = mapping.block();
    check(control != nullptr, "control block mapped");

    if (control != nullptr)
    {
        check(control->version.load() == hs::kRingVersion, "control block version matches");

        control->slots[0].owner.store(11);
        control->slots[1].owner.store(22);
        control->slots[0].mode.store(static_cast<uint32_t>(hs::CaptureMode::App));
        control->slots[1].mode.store(static_cast<uint32_t>(hs::CaptureMode::Channel));
        control->slots[0].pair.store(0);
        control->slots[1].pair.store(3);
        control->slots[0].tapPeak.store(0.5f);
        control->slots[0].appCount.store(2);
        control->slots[0].appPID[0].store(1001);
        control->slots[0].appPID[1].store(1002);
        control->slots[0].appPeak[0].store(0.25f);
        control->slots[0].appState[1].store(static_cast<uint32_t>(hs::State::Waiting));

        check(control->slots[0].owner.load() == 11 && control->slots[1].owner.load() == 22,
              "slot owners are independent");
        check(control->slots[0].mode.load() != control->slots[1].mode.load(), "slot modes are independent");
        check(control->slots[0].pair.load() != control->slots[1].pair.load(), "slot pairs are independent");
        check(control->slots[1].tapPeak.load() == 0.0f, "slot peaks are independent");
        check(control->slots[0].appCount.load() == 2
                  && control->slots[1].appCount.load() == 0,
              "App counts are independent per slot");
        check(control->slots[0].appPID[0].load() != control->slots[0].appPID[1].load(),
              "two Apps in one slot keep their own process id");
        check(control->slots[0].appPeak[0].load() != control->slots[0].appPeak[1].load(),
              "two Apps in one slot keep their own level");
        check(control->slots[0].appState[0].load() != control->slots[0].appState[1].load(),
              "two Apps in one slot keep their own state");

        // Put the block back the way we found it: a developer machine may have a
        // live Agent that would otherwise adopt these two fake owners.
        control->slots[0].owner.store(0);
        control->slots[1].owner.store(0);
        control->slots[0].mode.store(static_cast<uint32_t>(hs::CaptureMode::Off));
        control->slots[1].mode.store(static_cast<uint32_t>(hs::CaptureMode::Off));
        control->slots[0].tapPeak.store(0.0f);
        control->slots[0].appCount.store(0);

        for (int app = 0; app < hs::kSlotApps; ++app)
        {
            control->slots[0].appPID[app].store(-1);
            control->slots[0].appPeak[app].store(0.0f);
            control->slots[0].appState[app].store(0);
        }
    }

    a.close();
    b.close();
    c.close();
    hs::RingBuffer::unlink(first);
    hs::RingBuffer::unlink(second);
    hs::RingBuffer::unlink(sameSlotSecondApp);

    if (failures == 0)
        std::printf("all slot checks passed\n");
    else
        std::printf("%d slot check(s) failed\n", failures);

    return failures == 0 ? 0 : 1;
}
