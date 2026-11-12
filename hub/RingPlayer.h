#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "DriftBridge.h"
#include "RingBuffer.h"

// Plays a ring into a "huishengstudio" virtual output device. The bridge absorbs the
// clock difference between the producer (a DAW or a tapped App) and the virtual
// device's mach-time clock, so nothing is ever dropped.
class RingPlayer
{
public:
    RingPlayer();
    ~RingPlayer();

    RingPlayer(const RingPlayer&) = delete;
    RingPlayer& operator= (const RingPlayer&) = delete;

    bool start(const std::string& deviceUID, hs::RingBuffer& ring, std::string& error);
    void stop();

    bool isRunning() const noexcept;
    double sampleRate() const noexcept;
    std::string deviceUID() const noexcept;

    void setTarget(uint32_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
