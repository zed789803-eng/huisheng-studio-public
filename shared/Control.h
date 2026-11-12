#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "RingBuffer.h"

#include "Config.h"

namespace hs {

// One capture slot per Capture plug-in instance. Everything the plug-in and the
// Agent have to agree on lives here. A slot owns one tap ring per App entry
// (index 0..appCount-1) plus the single virtual channel ring, so one instance
// can mix several Apps into one track and a second instance still gets its own
// slot, its own rings and its own selection.
struct alignas(64) CaptureSlot
{
    // ---- Capture plug-in -> Agent -------------------------------------------
    // owner is an instance token, not a pid: two instances of the same DAW share
    // a pid. 0 means the slot is free.
    std::atomic<uint32_t> owner { 0 };
    std::atomic<uint64_t> heartbeat { 0 };
    std::atomic<uint32_t> seq { 0 };           // bumped when any selection changes
    std::atomic<uint32_t> mode { 0 };          // CaptureMode
    std::atomic<uint32_t> appCount { 0 };      // 0..hs::kSlotApps
    std::atomic<int32_t>  appPID[hs::kSlotApps] = {};
    std::atomic<uint32_t> appObjectID[hs::kSlotApps] = {};
    std::atomic<int32_t>  pair { kDefaultCapturePair };
    std::atomic<uint32_t> hostRate { 0 };
    char appBundleID[kSlotApps][kRingNameSize] = {};
    char appName[kSlotApps][kRingNameSize] = {};

    // ---- Agent -> Capture plug-in -------------------------------------------
    std::atomic<uint32_t> tapState { 0 };      // State, aggregated over the Apps
    std::atomic<uint32_t> tapFault { 0 };      // Fault
    std::atomic<uint32_t> channelState { 0 };
    std::atomic<uint32_t> channelFault { 0 };
    std::atomic<uint32_t> tapRate { 0 };       // shared rate, 0 when Apps differ
    std::atomic<uint32_t> appState[hs::kSlotApps] = {};
    std::atomic<uint32_t> appFault[hs::kSlotApps] = {};
    std::atomic<uint32_t> appRate[hs::kSlotApps] = {};
    std::atomic<float>    tapGain { 1.0f };    // inverse of the tap headroom loss
    std::atomic<float>    tapPeak { 0.0f };    // summed over the Apps
    std::atomic<float>    appPeak[hs::kSlotApps] = {};
    std::atomic<float>    channelPeak { 0.0f };
};

// One control block shared by the Agent, the Capture plug-ins and the Return
// plug-in. Every field is a lock-free atomic; strings are written then published
// with a sequence counter so a reader never sees a half-written name.
//
// The capture side is per slot; the return side stays single, because a DAW
// sends one mix to one virtual output.
struct alignas(64) ControlBlock
{
    std::atomic<uint32_t> magic { 0 };
    std::atomic<uint32_t> version { 0 };

    CaptureSlot slots[kCaptureSlots];

    // Plug-in heartbeats. When one goes stale the Agent releases that path, so
    // a crashed DAW cannot leave an App muted.
    std::atomic<uint64_t> returnHeartbeat { 0 };

    // ---- Return plug-in -> Agent -------------------------------------------
    std::atomic<uint32_t> returnSeq { 0 };
    std::atomic<int32_t>  returnPair { kDefaultReturnPair };
    std::atomic<uint32_t> returnHostRate { 0 };

    // ---- Agent -> plug-ins -------------------------------------------------
    std::atomic<uint64_t> heartbeat { 0 };
    std::atomic<uint32_t> state { 0 };         // State, aggregated over slots
    std::atomic<uint32_t> fault { 0 };         // Fault
    std::atomic<uint32_t> returnState { 0 };
    std::atomic<uint32_t> returnFault { 0 };
    std::atomic<uint32_t> permissionRaw { 0xFFFFFFFFu };
    std::atomic<uint32_t> microphoneRaw { 0xFFFFFFFFu };
    std::atomic<uint32_t> permissionRequestSeq { 0 };
    std::atomic<float>    returnPeak { 0.0f };
    std::atomic<uint32_t> returnActive { 0 };
    std::atomic<uint32_t> activeSlots { 0 };   // slots currently leased
};

enum class CaptureMode : uint32_t { Off = 0, App = 1, Channel = 2, Both = 3 };

// Shared-memory control block mapping. Both the Agent and the plug-ins use it;
// the first process to run creates the object 0600 and owns its lifetime.
class ControlMapping
{
public:
    ControlMapping() = default;
    ~ControlMapping();

    ControlMapping(const ControlMapping&) = delete;
    ControlMapping& operator= (const ControlMapping&) = delete;

    bool open();
    void close();
    ControlBlock* block() const noexcept { return block_; }

private:
    int fd_ = -1;
    void* base_ = nullptr;
    size_t size_ = 0;
    ControlBlock* block_ = nullptr;
};

// shm_open names are per-machine, so the uid is part of every name and each
// object is created mode 0600: no other local account can read or inject audio.
inline std::string controlName()
{
    return "/huishengstudio." + std::to_string(static_cast<unsigned>(getuid())) + ".ctl";
}

// One ring per capture slot per App entry, plus one channel ring per slot.
// shm_open names are limited to PSHMNAMLEN characters (31, including the leading
// '/') and are per-machine, so the uid, the slot number and the App index are
// part of every name: "/huishengstudio.501.tap3a1".
inline std::string slotTapRingName(int slot, int app)
{
    return "/huishengstudio." + std::to_string(static_cast<unsigned>(getuid())) + ".tap"
           + std::to_string(slot) + "a" + std::to_string(app);
}

inline std::string slotChannelRingName(int slot)
{
    return "/huishengstudio." + std::to_string(static_cast<unsigned>(getuid())) + ".ch"
           + std::to_string(slot);
}

inline std::string returnRingName()
{
    return "/huishengstudio." + std::to_string(static_cast<unsigned>(getuid())) + ".ret";
}

inline void copyText(char* destination, size_t size, const std::string& text)
{
    if (destination == nullptr || size == 0)
        return;

    std::memset(destination, 0, size);
    std::memcpy(destination, text.data(), std::min(text.size(), size - 1));
}

inline ControlMapping::~ControlMapping()
{
    close();
}

inline bool ControlMapping::open()
{
    if (block_ != nullptr)
        return true;

    size_ = sizeof(ControlBlock);
    const std::string name = controlName();

    fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    const bool fresh = fd_ >= 0;

    if (fresh)
    {
        ::fchmod(fd_, 0600);
        ::ftruncate(fd_, static_cast<off_t>(size_));
    }
    else
    {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0600);
    }

    if (fd_ < 0)
        return false;

    base_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);

    if (base_ == MAP_FAILED)
    {
        base_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    block_ = static_cast<ControlBlock*>(base_);

    // The version check matters: a control block left over from an older build
    // has different field offsets, and reinterpreting it yields garbage.
    const bool wrongVersion = block_->version.load(std::memory_order_acquire) != kRingVersion;

    if (fresh || wrongVersion
        || block_->magic.load(std::memory_order_acquire) != kRingMagic)
    {
        new (block_) ControlBlock();
        block_->version.store(kRingVersion, std::memory_order_relaxed);
        block_->returnPair.store(kDefaultReturnPair, std::memory_order_relaxed);
        block_->magic.store(kRingMagic, std::memory_order_release);
    }

    return true;
}

inline void ControlMapping::close()
{
    if (base_ != nullptr)
        ::munmap(base_, size_);

    if (fd_ >= 0)
        ::close(fd_);

    base_ = nullptr;
    block_ = nullptr;
    fd_ = -1;
    size_ = 0;
}

} // namespace hs
