#pragma once

#include <cstdint>
#include <vector>

namespace hs {

// Variable-rate windowed-sinc interpolator used for two jobs:
//
//   * bridging two independent clocks (ppm-level drift correction), and
//   * converting a source that runs at another sample rate.
//
// The phase accumulator is a double, so the ratio can change every block
// without a discontinuity. At an exact 1.0 ratio with zero frac the class takes
// a memcpy path, which makes it a true bit-exact passthrough.
class SincResampler
{
public:
    static constexpr int kTaps        = 64;  // even, 31 frames of look-back
    static constexpr int kPhases      = 1024;
    static constexpr int kChannels    = 2;
    static constexpr int kLookBack    = kTaps / 2 - 1;
    static constexpr int kLookAhead   = kTaps - kLookBack - 1;

    static constexpr uint32_t latencyFrames() { return static_cast<uint32_t>(kLookBack); }

    SincResampler();

    // Builds the shared filter table once per process.
    static void prepareTable();

    void reset();

    // Pre-allocates so push() never allocates on the audio thread.
    void reserve(uint32_t frames);

    // step = input frames consumed per output frame produced.
    void setStep(double step);
    double step() const noexcept { return step_; }

    // Number of input frames the interpolator is holding.
    uint32_t buffered() const noexcept { return count_; }

    // Appends `frames` interleaved frames. Allocates on first use only.
    void push(const float* interleaved, uint32_t frames);

    // Produces up to `maxFrames`; returns how many were written.
    uint32_t process(float* out, uint32_t maxFrames);

private:
    std::vector<float> buffer_;
    double   pos_   = 0.0;   // next output position, in frames from buffer start
    double   step_  = 1.0;
    uint32_t count_ = 0;     // valid frames in buffer_
};

} // namespace hs
