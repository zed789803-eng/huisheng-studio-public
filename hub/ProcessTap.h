#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RingBuffer.h"

// Captures one Core Audio process object with a stereo-mixdown tap and writes
// it into a ring. The tap lives inside a private aggregate whose clock is the
// tap itself, so no hardware device and no second clock is involved.
class ProcessTap
{
public:
    ProcessTap();
    ~ProcessTap();

    ProcessTap(const ProcessTap&) = delete;
    ProcessTap& operator= (const ProcessTap&) = delete;

    bool start(uint32_t processObjectID, hs::RingBuffer& ring,
               const std::string& sourceName, std::string& error);
    void stop();

    bool isRunning() const noexcept;
    double sampleRate() const noexcept;
    uint32_t tapChannels() const noexcept;

    // Re-reads the process output channel count and updates the headroom
    // compensation. Safe to call periodically from the agent's timer.
    void refreshGain();

    float gain() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
