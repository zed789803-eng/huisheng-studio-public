#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RingBuffer.h"

namespace hs { struct ControlBlock; }

// The Agent's engine: owns the process tap, the virtual-input reader and the
// virtual-output player, and keeps them in sync with the shared control block.
//
// Everything here runs on the agent's main run loop except the audio callbacks.
class Core
{
public:
    Core();
    ~Core();

    Core(const Core&) = delete;
    Core& operator= (const Core&) = delete;

    bool open();
    void tick();

    // Called once by the installer so the prompts appear without the user
    // having to select an App first.
    void requestInstallPermissions();

    hs::ControlBlock* control() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
