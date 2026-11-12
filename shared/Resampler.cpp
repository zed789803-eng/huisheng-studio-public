#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace hs {
namespace {

constexpr int kCent = SincResampler::kTaps / 2 - 1;

struct TableHolder
{
    std::once_flag once;
    float coeff[SincResampler::kPhases][SincResampler::kTaps] {};
};

TableHolder& table()
{
    static TableHolder holder;
    return holder;
}

double blackmanWindow(double u) // u in [0,1]
{
    const double pi2 = 2.0 * M_PI;
    return 0.42 - 0.5 * std::cos(pi2 * u) + 0.08 * std::cos(2.0 * pi2 * u);
}

double sinc(double x)
{
    if (std::abs(x) < 1e-12)
        return 1.0;
    return std::sin(M_PI * x) / (M_PI * x);
}

} // namespace

SincResampler::SincResampler()
{
    buffer_.assign(static_cast<size_t>(kTaps) * kChannels, 0.0f);
    count_ = 0;
}

void SincResampler::prepareTable()
{
    auto& t = table();
    std::call_once(t.once, [&t]
    {
        // Output n interpolates the input at position kCent + n*step, so the
        // window spans taps centred there.
        for (int p = 0; p < kPhases; ++p)
        {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0;

            for (int tap = 0; tap < kTaps; ++tap)
            {
                const double x = static_cast<double>(tap - kCent) - frac;
                const double u = (x + kTaps / 2.0) / kTaps;
                const double v = sinc(x) * blackmanWindow(u);

                t.coeff[p][tap] = static_cast<float>(v);
                sum += v;
            }

            // Unity DC gain per phase: keeps the interpolator transparent in
            // the passband instead of showing a phase-dependent ripple.
            if (std::abs(sum) > 1e-9)
            {
                const float scale = static_cast<float>(1.0 / sum);
                for (int tap = 0; tap < kTaps; ++tap)
                    t.coeff[p][tap] *= scale;
            }
        }
    });
}

void SincResampler::reserve(uint32_t frames)
{
    const size_t needed = (static_cast<size_t>(frames) + kTaps) * kChannels;
    if (buffer_.size() < needed)
        buffer_.resize(needed, 0.0f);

}

void SincResampler::reset()
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    pos_ = 0.0;
    count_ = 0;
}

void SincResampler::setStep(double step)
{
    // Wide enough for any crystal drift or rate conversion, narrow enough that
    // a bogus controller value cannot run the interpolator away.
    step_ = std::max(0.25, std::min(step, 4.0));
}

void SincResampler::push(const float* interleaved, uint32_t frames)
{
    if (interleaved == nullptr || frames == 0)
        return;

    const size_t needed = (count_ + frames) * kChannels;
    if (buffer_.size() < needed)
        buffer_.resize(needed + static_cast<size_t>(kTaps) * kChannels, 0.0f);

    std::memcpy(buffer_.data() + count_ * kChannels, interleaved,
                static_cast<size_t>(frames) * kChannels * sizeof(float));

    count_ += frames;
}

uint32_t SincResampler::process(float* out, uint32_t maxFrames)
{
    if (out == nullptr || maxFrames == 0 || count_ < static_cast<uint32_t>(kTaps))
        return 0;

    prepareTable();
    const float (*coeff)[kTaps] = table().coeff;

    uint32_t produced = 0;

    while (produced < maxFrames)
    {
        const size_t base = static_cast<size_t>(pos_);

        // Taps map to input [base-1 .. base-1+kTaps).
        if (base + static_cast<size_t>(kTaps) > count_ + 1)
            break;

        const int phase = static_cast<int>((pos_ - static_cast<double>(base)) * kPhases) & (kPhases - 1);
        const float* src = buffer_.data() + (base - 1) * kChannels;
        const float* c = coeff[phase];

        float left = 0.0f;
        float right = 0.0f;

        for (int tap = 0; tap < kTaps; ++tap)
        {
            left  += c[tap] * src[tap * kChannels];
            right += c[tap] * src[tap * kChannels + 1];
        }

        out[static_cast<size_t>(produced) * 2] = left;
        out[static_cast<size_t>(produced) * 2 + 1] = right;

        pos_ += step_;
        ++produced;
    }

    // Drop frames neither the filter nor the next block can need again.
    const size_t drop = pos_ > 1.0 ? static_cast<size_t>(pos_) - 1 : 0;
    if (drop > 0)
    {
        const size_t keep = count_ - drop;
        std::memmove(buffer_.data(), buffer_.data() + drop * kChannels,
                     keep * kChannels * sizeof(float));
        count_ = static_cast<uint32_t>(keep);
        pos_ -= static_cast<double>(drop);
    }

    return produced;
}

} // namespace hs
