// Exercises the HAL driver's IO path directly (no install needed).
// Builds the driver source directly so its IO path can be exercised without
// installing the HAL bundle. Link with: clang++ -I ../shared driver/HuishengStudioDriver.cpp ...
//
// Topology under test: each pair is two single-direction devices. Audio written
// to the output half ("HsAudioSC n/n") comes back from the input half
// ("HsAudioSR n/n") exactly kPairReadLagFrames later, so a reader that lands in
// an earlier IO callback than the writer can never outrun it.
#include "../driver/HuishengStudioDriver.cpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what)
{
    std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", what);
    if (! condition)
        ++failures;
}

void writeCycle(AudioObjectID device, int pair, double sampleTime, uint32_t frames, const float* toWrite)
{
    AudioServerPlugInIOCycleInfo info{};
    info.mNominalIOBufferFrameSize = frames;
    info.mOutputTime.mSampleTime = sampleTime;
    info.mInputTime.mSampleTime = sampleTime;

    hs_DoIOOperation(gDriverRef, device, kObjectID_StreamOutputBase + pair, 0,
                     kAudioServerPlugInIOOperationWriteMix, frames, &info,
                     const_cast<float*>(toWrite), nullptr);
}

// The input half reads that far behind its own clock, so a caller that wants the
// window starting at `sampleTime` reports a clock kPairReadLagFrames ahead.
void readCycle(AudioObjectID device, int pair, double sampleTime, uint32_t frames, float* readBack)
{
    AudioServerPlugInIOCycleInfo info{};
    info.mNominalIOBufferFrameSize = frames;
    const double clock = sampleTime + static_cast<double>(kPairReadLagFrames);
    info.mInputTime.mSampleTime = clock;
    info.mOutputTime.mSampleTime = clock;

    hs_DoIOOperation(gDriverRef, device, kObjectID_StreamInputBase + pair, 0,
                     kAudioServerPlugInIOOperationReadInput, frames, &info,
                     readBack, nullptr);
}

float sampleValue(int cycle, uint32_t i)
{
    return (cycle == 8 && i == 0) ? 0.75f : static_cast<float>(cycle * 1000 + i) * 1.0e-4f;
}

} // namespace

int main()
{
    hs_Initialize(gDriverRef, nullptr);

    const int pair = 1;                                                      // pair 3/4
    const AudioObjectID writer = kObjectID_Device0 + pair;                    // HsAudioSC 3/4
    const AudioObjectID reader = kObjectID_Device0 + pair + HS_PAIR_COUNT;    // HsAudioSR 3/4

    hs_StartIO(gDriverRef, writer, 0);

    const uint32_t frames = 128;
    std::vector<float> out(frames * 2), in(frames * 2);

    // 1) The write comes out of the other half, bit exact, and only after the lag.
    int firstHeard = -1;
    int heardTooEarly = -1;
    uint32_t mismatches = 0;

    for (int c = 0; c < 64; ++c)
    {
        const double t = static_cast<double>(c) * frames;

        for (uint32_t i = 0; i < frames; ++i)
        {
            out[i * 2]     = sampleValue(c, i);
            out[i * 2 + 1] = -sampleValue(c, i);
        }

        writeCycle(writer, pair, t, frames, out.data());

        if (c >= 1)
        {
            std::fill(in.begin(), in.end(), 0.0f);
            readCycle(reader, pair, t - frames, frames, in.data());

            if (in[0] == 0.75f && firstHeard < 0)
                firstHeard = c - 1;

            for (uint32_t i = 0; i < frames; ++i)
                if (in[i * 2] != sampleValue(c - 1, i) || in[i * 2 + 1] != -sampleValue(c - 1, i))
                    ++mismatches;
        }

        // The cycle being written right now must not be readable yet.
        std::fill(in.begin(), in.end(), 1.0f);
        readCycle(reader, pair, t + frames, frames, in.data());

        for (float v : in)
            if (v != 0.0f && heardTooEarly < 0)
                heardTooEarly = c;
    }

    expect(mismatches == 0, "every sample comes back unchanged");
    expect(firstHeard == 8, "impulse written in cycle 8 is heard one cycle later");
    expect(heardTooEarly < 0, "the cycle being written is not readable yet");

    // 2) Realistic timing: the host presents output ahead of input, so the
    //    callback that reads time t can run before the one that writes t.
    std::vector<float> a(frames * 2, 0.5f), b(frames * 2, 0.25f), seen(frames * 2, 0.0f);
    writeCycle(writer, pair, 9000.0, frames, b.data());        // written first, must win
    writeCycle(writer, pair, 9000.0, frames, a.data());        // same window, later write
    readCycle(reader, pair, 9000.0, frames, seen.data());

    uint32_t wrong = 0;
    for (float v : seen)
        if (v != 0.5f)
            ++wrong;

    expect(wrong == 0, "a window written twice reads back the latest write");

    // 3) A pair that has never been written reads as silence.
    const int coldPair = 3;
    const AudioObjectID coldReader = kObjectID_Device0 + coldPair + HS_PAIR_COUNT;
    hs_StartIO(gDriverRef, kObjectID_Device0 + coldPair, 0);

    std::vector<float> cold(frames * 2, 9.0f);
    readCycle(coldReader, coldPair, 777.0, frames, cold.data());

    float maxCold = 0.0f;
    for (float v : cold)
        maxCold = std::fmax(maxCold, std::fabs(v));

    expect(maxCold == 0.0f, "an unwritten pair reads back silence");

    // 4) Reported numbers: the output half is honest about adding no latency, the
    //    input half admits the read lag, and the safety offset stays put.
    auto number = [](AudioObjectID device, AudioObjectPropertySelector selector) -> UInt32
    {
        AudioObjectPropertyAddress address{ selector, kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain };
        UInt32 value = 0, size = sizeof(value);
        hs_GetPropertyData(gDriverRef, device, 0, &address, 0, nullptr, size, &size, &value);
        return value;
    };

    const UInt32 outLatency = number(writer, kAudioDevicePropertyLatency);
    const UInt32 inLatency  = number(reader, kAudioDevicePropertyLatency);
    const UInt32 safety     = number(writer, kAudioDevicePropertySafetyOffset);
    const UInt32 period     = number(writer, kAudioDevicePropertyZeroTimeStampPeriod);

    std::printf("latency out=%u in=%u safety=%u zeroPeriod=%u lag=%u\n",
                outLatency, inLatency, safety, period, (unsigned) kPairReadLagFrames);

    expect(outLatency == 0, "the output half reports no latency");
    expect(inLatency == kPairReadLagFrames, "the input half reports the read lag");
    expect(safety == 64, "safety offset is unchanged");
    expect(period == kZeroPeriod, "zero timestamp period is unchanged");

    hs_StopIO(gDriverRef, kObjectID_Device0 + coldPair, 0);
    hs_StopIO(gDriverRef, writer, 0);

    std::printf("\nloopback failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
