#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RingBuffer.h"

// Reads a "huishengstudio" virtual input device (the loopback side of a virtual
// output) and writes it into a ring. The device already shares a clock with its
// own output, so this class only has to bridge the device clock to the consumer
// whenever the consumer asks for it; the bridge lives in DriftBridge.
class RingRecorder
{
public:
    RingRecorder();
    ~RingRecorder();

    RingRecorder(const RingRecorder&) = delete;
    RingRecorder& operator= (const RingRecorder&) = delete;

    bool start(const std::string& deviceUID, hs::RingBuffer& ring,
               const std::string& sourceName, std::string& error);
    void stop();

    bool isRunning() const noexcept;
    double sampleRate() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
