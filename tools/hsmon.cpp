// hsmon — prints the shared-memory state of the running bridge.
//
//   hsmon              summary: agent heartbeat, every capture slot and its Apps
//   hsmon -w           watch mode, one line every 500 ms
//   hsmon -r <name>    raw counters for one ring
//                      (tap<slot>a<app> | channel<slot> | return)

#include "Control.h"
#include "Config.h"
#include "RingBuffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char* stateText(hs::State state)
{
    switch (state)
    {
        case hs::State::Stopped: return "stopped";
        case hs::State::Starting: return "starting";
        case hs::State::Running: return "running";
        case hs::State::Waiting: return "waiting";
        case hs::State::Error: return "error";
        case hs::State::Denied: return "denied";
    }
    return "?";
}

const char* faultText(hs::Fault fault)
{
    switch (fault)
    {
        case hs::Fault::None: return "none";
        case hs::Fault::NoPermission: return "no-permission";
        case hs::Fault::ProcessGone: return "process-gone";
        case hs::Fault::DeviceMissing: return "device-missing";
        case hs::Fault::TapFailed: return "tap-failed";
        case hs::Fault::InputFailed: return "input-failed";
        case hs::Fault::RingFailed: return "ring-failed";
        case hs::Fault::NoRoute: return "no-route";
        case hs::Fault::Unsupported: return "unsupported";
        case hs::Fault::Unlicensed: return "unlicensed";
    }
    return "?";
}

float db(float linear)
{
    return linear > 0.0f ? 20.0f * std::log10(linear) : -120.0f;
}

// A capture slot owns one tap ring per App it captures, plus the single virtual
// channel ring. A slot is what one plug-in instance keeps for as long as it keeps
// beating.
//
//   tap<slot>a<app>   the tap ring of App <app> in slot <slot>   (e.g. tap3a1)
//   channel<slot>     the virtual channel ring of slot <slot>
std::string ringNameFor(const std::string& kind)
{
    if (kind == "return")
        return hs::returnRingName();

    const bool isTap = kind.rfind("tap", 0) == 0;
    const bool isChannel = kind.rfind("channel", 0) == 0;

    if (! isTap && ! isChannel)
        return kind;

    if (isChannel)
    {
        const int slot = std::atoi(kind.substr(7).c_str());

        if (slot < 0 || slot >= hs::kCaptureSlots)
            return kind;

        return hs::slotChannelRingName(slot);
    }

    const std::string rest = kind.substr(3);
    const auto separator = rest.find('a');
    const std::string slotText = separator == std::string::npos ? rest : rest.substr(0, separator);
    const std::string appText = separator == std::string::npos ? std::string() : rest.substr(separator + 1);
    const int slot = slotText.empty() ? 0 : std::atoi(slotText.c_str());
    const int app = appText.empty() ? 0 : std::atoi(appText.c_str());

    if (slot < 0 || slot >= hs::kCaptureSlots || app < 0 || app >= hs::kSlotApps)
        return kind;

    return hs::slotTapRingName(slot, app);
}

int raw(const std::string& name)
{
    hs::RingBuffer ring;

    if (! ring.open(ringNameFor(name)))
    {
        std::fprintf(stderr, "cannot open %s\n", ringNameFor(name).c_str());
        return 1;
    }

    auto* h = ring.header();
    std::printf("ring %-28s rate=%u active=%u\n", name.c_str(), h->sampleRate.load(),
                h->active.load());
    std::printf("  frames written=%llu read=%llu available=%u\n",
                (unsigned long long) h->writeFrames.load(std::memory_order_relaxed),
                (unsigned long long) h->readFrames.load(std::memory_order_relaxed),
                ring.available());
    std::printf("  overruns=%llu underruns=%llu peak=%.1f dB source=\"%.*s\"\n",
                (unsigned long long) h->overruns.load(), (unsigned long long) h->underruns.load(),
                db(h->peak.load()),
                static_cast<int>(sizeof(h->sourceName)), h->sourceName);
    return 0;
}

void summary()
{
    hs::ControlMapping mapping;
    mapping.open();

    auto* c = mapping.block();

    hs::RingBuffer ret;
    ret.open(hs::returnRingName());

    if (c == nullptr)
    {
        std::printf("no control block\n");
        return;
    }

    const uint64_t heartbeat = c->heartbeat.load();
    const double age = heartbeat == 0 ? -1.0 : static_cast<double>(hs::nowNs() - heartbeat) / 1e9;

    std::printf("agent    %s  (heartbeat %.1f s ago)\n", age >= 0.0 && age < 2.0 ? "online" : "offline", age);
    std::printf("slots    %u in use of %d   return pair %d   return state %s\n",
                c->activeSlots.load(), hs::kCaptureSlots, c->returnPair.load(),
                stateText(static_cast<hs::State>(c->returnState.load())));

    float tapPeak = 0.0f;
    float channelPeak = 0.0f;
    uint32_t tapRate = 0;
    uint32_t channelRate = 0;
    uint32_t hostRate = 0;      // 每路自己报工程采样率，这里取第一路非零的
    int appTotal = 0;

    for (int i = 0; i < hs::kCaptureSlots; ++i)
    {
        auto* slot = &c->slots[i];

        if (slot->owner.load() == 0)
            continue;

        const int count = std::min<int>(hs::kSlotApps,
                                        static_cast<int>(slot->appCount.load()));

        hs::RingBuffer channel;
        channel.open(hs::slotChannelRingName(i));

        std::printf("slot %d   mode %u   apps %d   capture pair %d   owner %08x   host %u Hz\n",
                    i, slot->mode.load(), count, slot->pair.load(), slot->owner.load(),
                    slot->hostRate.load());

        for (int a = 0; a < count; ++a)
        {
            hs::RingBuffer tap;
            tap.open(hs::slotTapRingName(i, a));

            const uint32_t rateHz = tap.isOpen() ? tap.sampleRate() : 0u;

            std::printf("  app %d  %-9s  pid %-6d \"%.*s\"   %u Hz   peak %6.1f dB",
                        a, stateText(static_cast<hs::State>(slot->appState[a].load())),
                        slot->appPID[a].load(),
                        static_cast<int>(sizeof(slot->appName[0])), slot->appName[a],
                        slot->appRate[a].load(), db(slot->appPeak[a].load()));

            if (slot->appFault[a].load() != 0)
                std::printf("   fault %s", faultText(static_cast<hs::Fault>(slot->appFault[a].load())));

            std::printf("\n");

            // The tap's own counters are on its ring header: they are what tells
            // a stolen frame apart from a quiet App.
            if (tap.isOpen())
            {
                auto* h = tap.header();
                std::printf("           ring overruns %llu   underruns %llu   available %u\n",
                            (unsigned long long) h->overruns.load(),
                            (unsigned long long) h->underruns.load(), tap.available());
            }

            if (tapRate == 0 && rateHz != 0)
                tapRate = rateHz;

            ++appTotal;
        }

        std::printf("         tap %-9s  channel %-9s   peak tap %6.1f dB   channel %6.1f dB   buffers channel %6u\n",
                    stateText(static_cast<hs::State>(slot->tapState.load())),
                    stateText(static_cast<hs::State>(slot->channelState.load())),
                    db(slot->tapPeak.load()), db(slot->channelPeak.load()),
                    channel.isOpen() ? channel.available() : 0u);

        tapPeak = std::max(tapPeak, slot->tapPeak.load());
        channelPeak = std::max(channelPeak, slot->channelPeak.load());

        const auto channelRateHz = channel.isOpen() ? channel.sampleRate() : 0u;

        if (channelRateHz != 0 && channelRate == 0)
            channelRate = channelRateHz;

        if (hostRate == 0 && slot->hostRate.load() != 0)
            hostRate = slot->hostRate.load();
    }

    std::printf("peak     tap %6.1f dB   channel %6.1f dB   return %6.1f dB\n",
                db(tapPeak), db(channelPeak), db(c->returnPeak.load()));
    std::printf("rates    host %u Hz   tap %u Hz   channel %u Hz\n",
                hostRate, tapRate, channelRate);
    std::printf("apps     %d in %u slot(s)\n", appTotal, c->activeSlots.load());
    std::printf("buffers  return %6u\n", ret.isOpen() ? ret.available() : 0u);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "-r") == 0)
        return raw(argv[2]);

    if (argc >= 2 && std::strcmp(argv[1], "-w") == 0)
    {
        while (true)
        {
            std::printf("\033[2J\033[H");
            summary();
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    summary();
    return 0;
}
