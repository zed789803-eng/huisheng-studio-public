#include "RingBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hs {
namespace {

size_t ringSize(uint32_t capacityFrames)
{
    return kDataOffset + static_cast<size_t>(capacityFrames) * kChannels * sizeof(float);
}

} // namespace

RingBuffer::~RingBuffer()
{
    close();
}

bool RingBuffer::create(const std::string& name, uint32_t sampleRate, uint32_t capacityFrames)
{
    close();

    if (name.empty() || name[0] != '/' || capacityFrames == 0)
        return false;

    // O_EXCL in the first attempt: if another process owns a live object we
    // must not truncate it, we map it instead.
    int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        fd = ::shm_open(name.c_str(), O_RDWR, 0600);
        if (fd < 0)
            return false;
    }

    ::fchmod(fd, 0600);

    struct stat st {};
    if (::fstat(fd, &st) != 0)
    {
        ::close(fd);
        return false;
    }

    const size_t size = ringSize(capacityFrames);
    const bool fresh = static_cast<size_t>(st.st_size) != size;

    if (fresh && ::ftruncate(fd, static_cast<off_t>(size)) != 0)
    {
        ::close(fd);
        return false;
    }

    void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ::close(fd);
        return false;
    }

    map(base, size, fd);
    fd_ = fd;

    const bool valid = ! fresh
                       && header_->magic.load(std::memory_order_acquire) == kRingMagic
                       && header_->version.load(std::memory_order_relaxed) == kRingVersion
                       && header_->capacityFrames.load(std::memory_order_relaxed) == capacityFrames;

    if (! valid)
    {
        new (header_) RingHeader();
        header_->version.store(kRingVersion, std::memory_order_relaxed);
        header_->channels.store(kChannels, std::memory_order_relaxed);
        header_->capacityFrames.store(capacityFrames, std::memory_order_relaxed);
        header_->magic.store(kRingMagic, std::memory_order_release);
    }

    header_->sampleRate.store(sampleRate, std::memory_order_relaxed);
    header_->active.store(0, std::memory_order_release);

    return true;
}

bool RingBuffer::open(const std::string& name)
{
    close();

    if (name.empty())
        return false;

    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0)
        return false;

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kDataOffset))
    {
        ::close(fd);
        return false;
    }

    void* base = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ::close(fd);
        return false;
    }

    map(base, static_cast<size_t>(st.st_size), fd);
    fd_ = fd;

    if (! validate())
    {
        unmap();
        return false;
    }

    return true;
}

void RingBuffer::close()
{
    unmap();
}

void RingBuffer::map(void* base, size_t size, int)
{
    mapping_ = base;
    mappingSize_ = size;
    header_ = static_cast<RingHeader*>(base);
    data_ = reinterpret_cast<float*>(static_cast<char*>(base) + kDataOffset);
}

void RingBuffer::unmap()
{
    if (mapping_ != nullptr)
        ::munmap(mapping_, mappingSize_);

    mapping_ = nullptr;
    mappingSize_ = 0;
    header_ = nullptr;
    data_ = nullptr;

    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RingBuffer::validate() const
{
    if (header_ == nullptr || data_ == nullptr)
        return false;

    if (header_->magic.load(std::memory_order_acquire) != kRingMagic)
        return false;

    if (header_->version.load(std::memory_order_relaxed) != kRingVersion)
        return false;

    const uint32_t capacity = header_->capacityFrames.load(std::memory_order_relaxed);
    if (capacity == 0 || header_->channels.load(std::memory_order_relaxed) != kChannels)
        return false;

    return kDataOffset + static_cast<size_t>(capacity) * kChannels * sizeof(float) <= mappingSize_;
}

uint32_t RingBuffer::capacityFrames() const noexcept
{
    return header_ != nullptr ? header_->capacityFrames.load(std::memory_order_relaxed) : 0;
}

uint32_t RingBuffer::sampleRate() const noexcept
{
    return header_ != nullptr ? header_->sampleRate.load(std::memory_order_relaxed) : 0;
}

void RingBuffer::setActive(bool active)
{
    if (header_ != nullptr)
        header_->active.store(active ? 1u : 0u, std::memory_order_release);
}

void RingBuffer::setSampleRate(uint32_t rate)
{
    if (header_ != nullptr)
        header_->sampleRate.store(rate, std::memory_order_relaxed);
}

void RingBuffer::setHostSampleRate(uint32_t rate)
{
    if (header_ != nullptr)
        header_->hostSampleRate.store(rate, std::memory_order_relaxed);
}

void RingBuffer::setSourceName(const std::string& name)
{
    if (header_ == nullptr)
        return;

    std::memset(header_->sourceName, 0, sizeof(header_->sourceName));
    std::memcpy(header_->sourceName, name.data(),
                std::min(name.size(), sizeof(header_->sourceName) - 1));
}

void RingBuffer::resetForProducer(uint32_t sampleRate, const std::string& sourceName)
{
    if (header_ == nullptr)
        return;

    header_->active.store(0, std::memory_order_release);
    header_->writeFrames.store(0, std::memory_order_relaxed);
    header_->readFrames.store(0, std::memory_order_relaxed);
    header_->overruns.store(0, std::memory_order_relaxed);
    header_->underruns.store(0, std::memory_order_relaxed);
    header_->peak.store(0.0f, std::memory_order_relaxed);
    header_->sourceName[0] = '\0';
    setSampleRate(sampleRate);
    setSourceName(sourceName);
}

uint32_t RingBuffer::write(const float* interleaved, uint32_t frames)
{
    if (header_ == nullptr || data_ == nullptr || interleaved == nullptr || frames == 0)
        return 0;

    const uint32_t capacity = header_->capacityFrames.load(std::memory_order_relaxed);
    if (capacity == 0)
        return 0;

    header_->producerHeartbeat.store(nowNs(), std::memory_order_relaxed);

    uint64_t writePos = header_->writeFrames.load(std::memory_order_relaxed);
    uint64_t readPos = header_->readFrames.load(std::memory_order_acquire);

    if (writePos < readPos)
    {
        readPos = writePos;
        header_->readFrames.store(readPos, std::memory_order_release);
    }

    uint64_t used = writePos - readPos;
    if (used > capacity)
    {
        // The consumer is not running (or stalled for many seconds); resync.
        readPos = writePos;
        header_->readFrames.store(readPos, std::memory_order_release);
        used = 0;
    }

    const uint32_t free = static_cast<uint32_t>(capacity - used);
    const uint32_t toWrite = std::min(frames, free);

    if (toWrite == 0)
    {
        header_->overruns.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    const uint32_t start = static_cast<uint32_t>(writePos % capacity);
    const uint32_t first = std::min(toWrite, capacity - start);

    std::memcpy(data_ + static_cast<size_t>(start) * kChannels, interleaved,
                static_cast<size_t>(first) * kChannels * sizeof(float));

    if (toWrite > first)
        std::memcpy(data_, interleaved + static_cast<size_t>(first) * kChannels,
                    static_cast<size_t>(toWrite - first) * kChannels * sizeof(float));

    float blockPeak = 0.0f;
    for (uint32_t i = 0, n = toWrite * kChannels; i < n; ++i)
        blockPeak = std::max(blockPeak, std::abs(interleaved[i]));

    header_->peak.store(blockPeak, std::memory_order_relaxed);
    header_->writeFrames.store(writePos + toWrite, std::memory_order_release);

    if (toWrite < frames)
        header_->overruns.fetch_add(frames - toWrite, std::memory_order_relaxed);

    return toWrite;
}

uint32_t RingBuffer::read(float* interleaved, uint32_t frames)
{
    if (interleaved == nullptr || frames == 0)
        return 0;

    if (header_ == nullptr || data_ == nullptr)
    {
        std::memset(interleaved, 0, static_cast<size_t>(frames) * kChannels * sizeof(float));
        return 0;
    }

    const uint32_t capacity = header_->capacityFrames.load(std::memory_order_relaxed);
    if (capacity == 0)
    {
        std::memset(interleaved, 0, static_cast<size_t>(frames) * kChannels * sizeof(float));
        return 0;
    }

    uint64_t readPos = header_->readFrames.load(std::memory_order_relaxed);
    const uint64_t writePos = header_->writeFrames.load(std::memory_order_acquire);

    if (writePos < readPos)
    {
        readPos = writePos;
        header_->readFrames.store(readPos, std::memory_order_release);
    }

    const uint64_t avail = writePos - readPos;
    const uint32_t toRead = static_cast<uint32_t>(std::min<uint64_t>(frames, avail));

    if (toRead > 0)
    {
        const uint32_t start = static_cast<uint32_t>(readPos % capacity);
        const uint32_t first = std::min(toRead, capacity - start);

        std::memcpy(interleaved, data_ + static_cast<size_t>(start) * kChannels,
                    static_cast<size_t>(first) * kChannels * sizeof(float));

        if (toRead > first)
            std::memcpy(interleaved + static_cast<size_t>(first) * kChannels, data_,
                        static_cast<size_t>(toRead - first) * kChannels * sizeof(float));
    }

    if (toRead < frames)
    {
        std::memset(interleaved + static_cast<size_t>(toRead) * kChannels, 0,
                    static_cast<size_t>(frames - toRead) * kChannels * sizeof(float));
        header_->underruns.fetch_add(frames - toRead, std::memory_order_relaxed);
    }

    header_->readFrames.store(readPos + toRead, std::memory_order_release);
    return toRead;
}

uint32_t RingBuffer::available() const noexcept
{
    if (header_ == nullptr)
        return 0;

    const uint64_t w = header_->writeFrames.load(std::memory_order_acquire);
    const uint64_t r = header_->readFrames.load(std::memory_order_relaxed);
    return w > r ? static_cast<uint32_t>(w - r) : 0;
}

void RingBuffer::unlink(const std::string& name)
{
    if (! name.empty())
        ::shm_unlink(name.c_str());
}

} // namespace hs
