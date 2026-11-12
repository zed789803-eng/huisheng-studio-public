// Checks that GetZeroTimeStamp never makes the HAL chase a stale clock.
//
// The driver answers timestamps from a fixed anchor. If it only advances the
// anchor by one period per call, a timestamp that is seconds behind leaves the
// HAL calling it in a tight loop until the clock catches up with mach time:
// coreaudiod pegs a core and every host that is opening the device waits.
// A host restart with the virtual device already open was exactly that case.
#include "../driver/HuishengStudioDriver.cpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool condition, const char* what, double value)
{
    std::printf("  %s %s (%.3f)\n", condition ? "ok  " : "FAIL", what, value);

    if (! condition)
        ++failures;
}

double behindSeconds(UInt64 hostTime, UInt64 now)
{
    return static_cast<double>(now - hostTime) / gHostTicksPerSecond;
}

} // namespace

int main()
{
    if (hs_Initialize(gDriverRef, NULL) != kAudioHardwareNoError)
    {
        std::printf("initialize failed\n");
        return 1;
    }

    const AudioObjectID device = kObjectID_Device0;
    Float64 sampleTime = 0.0;
    UInt64 hostTime = 0;
    UInt64 seed = 0;

    // No handshake at all: the HAL asks for a timestamp long after the driver
    // initialized (coreaudiod idles out and comes back).
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    UInt64 now = mach_absolute_time();
    check(hs_GetZeroTimeStamp(gDriverRef, device, 0, &sampleTime, &hostTime, &seed) == kAudioHardwareNoError,
          "timestamp without IO succeeds", 0.0);
    check(behindSeconds(hostTime, now) < 0.100, "answers within one period in one call",
          behindSeconds(hostTime, now));

    // The same again, but with IO running and the anchor aged.
    if (hs_StartIO(gDriverRef, device, 0) != kAudioHardwareNoError)
    {
        std::printf("start io failed\n");
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    now = mach_absolute_time();
    const Float64 firstSample = sampleTime;
    check(hs_GetZeroTimeStamp(gDriverRef, device, 0, &sampleTime, &hostTime, &seed) == kAudioHardwareNoError,
          "timestamp after a gap succeeds", 0.0);
    check(behindSeconds(hostTime, now) < 0.100, "catches up in one call", behindSeconds(hostTime, now));
    check(sampleTime >= firstSample, "sample time does not go backwards", sampleTime - firstSample);

    // A burst must stay monotonic and must not walk forward one period at a
    // time (that slow walk is what pegs the CPU).
    double previous = sampleTime;
    bool monotonic = true;
    int steps = 0;

    const UInt64 burstStart = mach_absolute_time();

    while (behindSeconds(mach_absolute_time(), burstStart) < 0.020 && steps < 10000)
    {
        hs_GetZeroTimeStamp(gDriverRef, device, 0, &sampleTime, &hostTime, &seed);
        monotonic = monotonic && sampleTime >= previous;

        if (sampleTime != previous)
            ++steps;

        previous = sampleTime;
    }

    check(monotonic, "burst stays monotonic", steps);
    check(steps <= 4, "burst does not step once per call", static_cast<double>(steps));

    // The two halves of a pair are one engine: one anchor, one seed, one clock.
    // If they disagreed, the input half would drift away from the output half and
    // the loopback would slowly starve.
    Float64 inputSample = 0.0;
    UInt64 inputHost = 0;
    UInt64 inputSeed = 0;
    const AudioObjectID inputHalf = kObjectID_Device0 + HS_PAIR_COUNT;

    check(hs_GetZeroTimeStamp(gDriverRef, inputHalf, 0, &inputSample, &inputHost, &inputSeed)
              == kAudioHardwareNoError, "the input half answers too", 0.0);
    check(inputSeed == seed, "both halves share one timeline seed", static_cast<double>(inputSeed));
    check(std::fabs(inputSample - sampleTime) <= 2.0 * kZeroPeriod,
          "both halves answer on the same clock", inputSample - sampleTime);

    hs_StopIO(gDriverRef, device, 0);

    // A second stop must not be an error: the HAL retries a failing stop.
    check(hs_StopIO(gDriverRef, device, 0) == kAudioHardwareNoError, "stop is idempotent", 0.0);

    if (failures == 0)
        std::printf("all timestamp checks passed\n");

    return failures == 0 ? 0 : 1;
}
