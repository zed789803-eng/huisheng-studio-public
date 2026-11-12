#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Resampler.h"

namespace hs {

// Bidirectional jitter-absorber between two independently clocked audio
// streams. The producer pushes whole blocks; the consumer pulls a fixed number
// of frames every IO callback. A PI controller steers the resampler so the
// standing buffer stays at `targetFrames`, which absorbs crystal drift without
// ever dropping or duplicating audio.
class DriftBridge
{
public:
    DriftBridge();

    // `outputRate` is the rate of the consumer; `inputRate` the producer's.
    // When they differ the bridge also acts as a sample-rate converter.
    void reset(uint32_t outputRate, uint32_t bufferCapacity = 16384);
    void setInputRate(uint32_t inputRate);

    // Tune the standing buffer. Cheap; safe to call at any time.
    void setTarget(uint32_t frames, uint32_t toleranceFrames = 32);

    // Producer side. Thread-safe with respect to `pull` as long as only one
    // thread pushes and one thread pulls (the normal case).
    void push(const float* interleaved, uint32_t frames);

    // Consumer side.
    void pull(float* interleaved, uint32_t frames);

    // How many input frames the next `outFrames`-frame pull will consume at the
    // nominal rate ratio. Producers should push exactly this much; the ring then
    // only holds the difference the PI controller introduces.
    uint32_t inputFramesFor(uint32_t outFrames) const noexcept;

    // Current standing buffer, in frames.
    uint32_t buffered() const noexcept;

    uint32_t targetFrames() const noexcept { return target_; }
    uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    uint32_t overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }
    double   appliedRatioPPM() const noexcept { return ratioPpm_.load(std::memory_order_relaxed); }

    // Diagnostic: ratio currently commanded by the controller.
    double controlRatio() const noexcept { return lastStep_ > 0.0 ? 1.0 / lastStep_ : 1.0; }

private:
    void   updateController();
    void   fillSilence(float* interleaved, uint32_t frames);
    uint32_t readFrames(uint32_t frames);

    std::vector<float> buffer_;
    std::vector<float> scratchRead_;
    uint32_t capacity_ = 16384;
    uint32_t sampleRate_ = 48000;

    std::atomic<uint32_t> writeFrames_ { 0 };
    std::atomic<uint32_t> readFrames_ { 0 };
    std::atomic<uint32_t> underruns_ { 0 };
    std::atomic<uint32_t> overruns_ { 0 };
    std::atomic<int32_t>  ratioPpm_ { 0 };

    uint32_t target_ = 128;
    uint32_t tolerance_ = 32;
    double   baseStep_ = 1.0;   // input frames per output frame at nominal rates

    SincResampler resampler_;
    double lastStep_ = 1.0;
    double integral_ = 0.0;
    int    integralSamples_ = 0;
    uint64_t processedFrames_ = 0;
    uint64_t lastControllerFrames_ = 0;
    bool primed_ = false;
    uint32_t starveBlocks_ = 0;
};

} // namespace hs
