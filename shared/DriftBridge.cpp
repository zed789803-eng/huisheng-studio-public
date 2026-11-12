#include "DriftBridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hs {
namespace {

constexpr uint32_t kMargin = SincResampler::kTaps * 2;
constexpr uint32_t kScratchFrames = 4096;
constexpr double   kMaxCorrection = 600.0e-6;   // ±600 ppm covers warm-up and clock slop
constexpr double   kProportional  = 2.0e-6;     // 1/frame of error
constexpr double   kIntegral      = 2.0e-10;    // 1/(s*frame)

} // namespace

DriftBridge::DriftBridge()
{
    reset(48000);
}

void DriftBridge::reset(uint32_t sampleRate, uint32_t bufferCapacity)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    capacity_ = std::max<uint32_t>(bufferCapacity, 4096);
    buffer_.assign(static_cast<size_t>(capacity_) * 2, 0.0f);
    scratchRead_.assign(static_cast<size_t>(kScratchFrames) * 2, 0.0f);

    writeFrames_.store(0, std::memory_order_relaxed);
    readFrames_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    ratioPpm_.store(0, std::memory_order_relaxed);

    resampler_.reset();
    resampler_.reserve(kScratchFrames * 4);
    resampler_.setStep(baseStep_);
    lastStep_ = baseStep_;
    integral_ = 0.0;
    integralSamples_ = 0;
    processedFrames_ = 0;
    lastControllerFrames_ = 0;
    primed_ = false;
    starveBlocks_ = 0;
}

void DriftBridge::setInputRate(uint32_t inputRate)
{
    baseStep_ = inputRate > 0
                    ? static_cast<double>(inputRate) / static_cast<double>(sampleRate_)
                    : 1.0;

    resampler_.setStep(baseStep_);
    lastStep_ = baseStep_;
    integral_ = 0.0;
}

void DriftBridge::setTarget(uint32_t frames, uint32_t toleranceFrames)
{
    // `frames` is the intended end-to-end delay. The interpolator itself holds
    // kLookBack frames of look-back, so subtract it from the ring level.
    const uint32_t ringTarget = frames > SincResampler::latencyFrames()
                                    ? frames - SincResampler::latencyFrames()
                                    : 16u;

    target_ = std::min(std::max(ringTarget, 16u), capacity_ / 2);
    tolerance_ = std::max<uint32_t>(toleranceFrames, 8);
}

void DriftBridge::push(const float* interleaved, uint32_t frames)
{
    if (interleaved == nullptr || frames == 0)
        return;

    uint32_t w = writeFrames_.load(std::memory_order_relaxed);
    const uint32_t r = readFrames_.load(std::memory_order_acquire);

    const uint32_t used = w >= r ? w - r : capacity_ - (r - w);
    const uint32_t free = capacity_ - used - kMargin;

    if (frames > free)
    {
        // The consumer is not draining (paused DAW, missing device). Drop the
        // oldest audio instead of letting the buffer grow forever.
        const uint32_t drop = frames - free;
        readFrames_.store((r + drop) % capacity_, std::memory_order_release);
        overruns_.fetch_add(drop, std::memory_order_relaxed);
    }

    const uint32_t start = w;
    const uint32_t first = std::min(frames, capacity_ - start);

    std::memcpy(buffer_.data() + static_cast<size_t>(start) * 2, interleaved,
                static_cast<size_t>(first) * 2 * sizeof(float));

    if (frames > first)
        std::memcpy(buffer_.data(), interleaved + static_cast<size_t>(first) * 2,
                    static_cast<size_t>(frames - first) * 2 * sizeof(float));

    w = (w + frames) % capacity_;
    writeFrames_.store(w, std::memory_order_release);
}

uint32_t DriftBridge::buffered() const noexcept
{
    const uint32_t w = writeFrames_.load(std::memory_order_acquire);
    const uint32_t r = readFrames_.load(std::memory_order_relaxed);
    return w >= r ? w - r : capacity_ - (r - w);
}

uint32_t DriftBridge::readFrames(uint32_t frames)
{
    uint32_t r = readFrames_.load(std::memory_order_relaxed);
    const uint32_t w = writeFrames_.load(std::memory_order_acquire);
    const uint32_t avail = w >= r ? w - r : capacity_ - (r - w);
    const uint32_t take = std::min(frames, avail);

    const uint32_t first = std::min(take, capacity_ - r);

    std::memcpy(scratchRead_.data(), buffer_.data() + static_cast<size_t>(r) * 2,
                static_cast<size_t>(first) * 2 * sizeof(float));

    if (take > first)
        std::memcpy(scratchRead_.data() + static_cast<size_t>(first) * 2, buffer_.data(),
                    static_cast<size_t>(take - first) * 2 * sizeof(float));

    r = (r + take) % capacity_;
    readFrames_.store(r, std::memory_order_release);
    return take;
}

void DriftBridge::fillSilence(float* interleaved, uint32_t frames)
{
    std::memset(interleaved, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
}

void DriftBridge::updateController()
{
    const uint32_t bufferedFrames = buffered();
    const double error = static_cast<double>(target_) - static_cast<double>(bufferedFrames);

    const double seconds = static_cast<double>(processedFrames_ - lastControllerFrames_)
                           / static_cast<double>(sampleRate_);

    // Slow integral absorbs the steady-state offset of the proportional term
    // without letting a block-level measurement noise wind it up.
    if (seconds > 0.0)
        integral_ += kIntegral * error * seconds;

    integral_ = std::min(std::max(integral_, -kMaxCorrection), kMaxCorrection);

    // Dead band: inside it the interpolator runs at the exact nominal ratio -
    // a bit-exact copy when the two rates are equal.
    double correction = integral_;

    if (std::abs(error) > static_cast<double>(tolerance_))
        correction = std::min(std::max(kProportional * error + integral_, -kMaxCorrection),
                              kMaxCorrection);

    // step = input frames per output frame. Error is (target - buffered), so a
    // full buffer makes it negative and must *raise* the step to drain it.
    lastStep_ = baseStep_ - correction;
    resampler_.setStep(lastStep_);
    ratioPpm_.store(static_cast<int32_t>(std::lround(correction * 1.0e6)),
                    std::memory_order_relaxed);
}

void DriftBridge::pull(float* interleaved, uint32_t frames)
{
    if (interleaved == nullptr || frames == 0)
        return;

    if (! primed_)
    {
        // Let the producer build the standing buffer before the first real
        // block so playback starts at the target latency rather than
        // underrunning on every callback.
        if (buffered() < target_ + frames + SincResampler::kTaps)
        {
            fillSilence(interleaved, frames);
            underruns_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }

        primed_ = true;
        processedFrames_ = 0;
        lastControllerFrames_ = 0;
        integral_ = 0.0;
        resampler_.reset();
    }

    // Keep a constant look-ahead inside the interpolator: each block consumes
    // about frames*step input and gets topped up to frames*step + kTaps, so the
    // interpolator's internal level - and therefore the ring level - is stable.
    const uint32_t required = static_cast<uint32_t>(std::ceil(
        static_cast<double>(frames) * lastStep_)) + static_cast<uint32_t>(SincResampler::kTaps);

    while (resampler_.buffered() < required)
    {
        const uint32_t chunk = std::min(required - resampler_.buffered() + 32u, kScratchFrames);
        const uint32_t got = readFrames(chunk);

        if (got == 0)
        {
            fillSilence(interleaved, frames);
            primed_ = false;
            underruns_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }

        resampler_.push(scratchRead_.data(), got);
        processedFrames_ += got;

        if (got < chunk)
            break;
    }

    const uint32_t produced = resampler_.process(interleaved, frames);

    if (produced < frames)
    {
        fillSilence(interleaved + static_cast<size_t>(produced) * 2, frames - produced);
        primed_ = false;
        underruns_.fetch_add(frames - produced, std::memory_order_relaxed);
    }

    updateController();
    lastControllerFrames_ = processedFrames_;
}

} // namespace hs
