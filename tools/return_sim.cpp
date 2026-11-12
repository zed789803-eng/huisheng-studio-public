// Simulates the Return plug-in's half of the control block. Used to check that
// the Agent starts the virtual output while a plug-in beats, and - just as
// important - that it stays stopped once the beat goes stale.
//
// Usage: hsreturnsim [beating-seconds] [silent-seconds]

#include "Config.h"
#include "Control.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

const char* stateName(uint32_t raw)
{
    switch (static_cast<hs::State>(raw))
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

uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

int main(int argc, char** argv)
{
    const int beatingSeconds = argc > 1 ? std::atoi(argv[1]) : 6;
    const int silentSeconds = argc > 2 ? std::atoi(argv[2]) : 6;

    hs::ControlMapping mapping;

    if (! mapping.open())
    {
        std::printf("cannot open the control block\n");
        return 1;
    }

    auto* c = mapping.block();
    const uint64_t age = c->returnHeartbeat.load() == 0 ? 0 : nowNs() - c->returnHeartbeat.load();

    std::printf("start: returnState=%s staleBeatAgeMs=%llu\n",
                stateName(c->returnState.load()), static_cast<unsigned long long>(age / 1'000'000));

    // ---- beat: the plug-in is alive ----------------------------------------
    c->returnHeartbeat.store(nowNs(), std::memory_order_release);
    c->returnPair.store(hs::kDefaultReturnPair, std::memory_order_relaxed);
    c->returnHostRate.store(48000, std::memory_order_relaxed);
    c->returnSeq.fetch_add(1, std::memory_order_release);

    for (int i = 0; i < beatingSeconds * 5; ++i)
    {
        c->returnHeartbeat.store(nowNs(), std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (i % 5 == 4)
            std::printf("beating: returnState=%s\n", stateName(c->returnState.load()));
    }

    // ---- silent: the plug-in went away -------------------------------------
    std::printf("stop beating\n");

    for (int i = 0; i < silentSeconds * 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("silent: returnState=%s\n", stateName(c->returnState.load()));
    }

    return 0;
}
