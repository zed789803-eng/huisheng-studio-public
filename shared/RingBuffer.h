#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hs {

inline constexpr uint32_t kRingMagic   = 0x574E4752; // 'HSR'
// 5: a capture slot now carries a list of Apps (one tap ring each) instead of a
// single App. Publishing a new version makes every process rebuild the objects
// it maps instead of reinterpreting the old layout.
inline constexpr uint32_t kRingVersion = 5;

inline constexpr uint32_t kChannels       = 2;
inline constexpr size_t   kDataOffset     = 4096;
inline constexpr size_t   kRingNameSize   = 128;
inline constexpr uint32_t kCapacityFrames = 192000; // 4 s at 48 kHz

// Agent states shared with the plug-ins.
enum class State : uint32_t { Stopped = 0, Starting = 1, Running = 2, Waiting = 3, Error = 4, Denied = 5 };
enum class Fault : uint32_t {
    None = 0,
    NoPermission = 1,
    ProcessGone = 2,
    DeviceMissing = 3,
    TapFailed = 4,
    InputFailed = 5,
    RingFailed = 6,
    NoRoute = 7,
    Unsupported = 8,  // the running macOS does not have this feature
    Unlicensed = 9    // no registration code: capture stays off on purpose
};

struct alignas(64) RingHeader
{
    std::atomic<uint32_t> magic { 0 };
    std::atomic<uint32_t> version { 0 };
    std::atomic<uint32_t> channels { 0 };
    std::atomic<uint32_t> capacityFrames { 0 };
    std::atomic<uint32_t> sampleRate { 0 };
    std::atomic<uint32_t> hostSampleRate { 0 };
    std::atomic<uint32_t> active { 0 };

    std::atomic<uint64_t> writeFrames { 0 };
    std::atomic<uint64_t> readFrames { 0 };
    std::atomic<uint64_t> producerHeartbeat { 0 };
    std::atomic<uint64_t> overruns { 0 };
    std::atomic<uint64_t> underruns { 0 };
    std::atomic<float>    peak { 0.0f };

    char sourceName[kRingNameSize] = {};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "lock-free 64-bit atomics required");
static_assert(sizeof(RingHeader) <= kDataOffset, "header must fit before the audio data");

inline uint64_t nowNs()
{
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// Single producer / single consumer float32 stereo ring living in POSIX shared
// memory (shm_open). Names are per-user and the object is created mode 0600, so
// no other local account can inject audio. No file ever appears in /tmp.
class RingBuffer
{
public:
    RingBuffer() = default;
    ~RingBuffer();

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator= (const RingBuffer&) = delete;

    bool create(const std::string& name, uint32_t sampleRate,
                uint32_t capacityFrames = kCapacityFrames);
    bool open(const std::string& name);
    void close();

    bool isOpen() const noexcept { return header_ != nullptr; }
    RingHeader* header() const noexcept { return header_; }

    uint32_t capacityFrames() const noexcept;
    uint32_t sampleRate() const noexcept;

    void setActive(bool active);
    void setSampleRate(uint32_t rate);
    void setHostSampleRate(uint32_t rate);
    void setSourceName(const std::string& name);

    // Producer restart: resets the timeline, publishes the rate and name, and
    // clears the active flag until the caller is ready to stream.
    void resetForProducer(uint32_t sampleRate, const std::string& sourceName);

    // Producer. Drops whole writes when the ring is full and counts the loss.
    uint32_t write(const float* interleaved, uint32_t frames);

    // Consumer. Returns the number of real frames stored; the remainder of the
    // destination is zero-filled and counted as underrun.
    uint32_t read(float* interleaved, uint32_t frames);

    uint32_t available() const noexcept;

    static void unlink(const std::string& name);

private:
    void map(void* base, size_t size, int fd);
    void unmap();
    bool validate() const;

    int fd_ = -1;
    void* mapping_ = nullptr;
    size_t mappingSize_ = 0;
    RingHeader* header_ = nullptr;
    float* data_ = nullptr;
};

} // namespace hs
