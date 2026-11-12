#pragma once

#include <cstdint>
#include <string>

namespace hs {

inline constexpr const char* kProductName        = "绘声Studio";
inline constexpr const char* kProductNameShort   = "HsAudio";

inline constexpr const char* kAgentBundleID      = "com.huishengstudio.agent";
inline constexpr const char* kAgentName          = "绘声Studio";
inline constexpr const char* kAgentDaemonPath    = "/Applications/HuishengStudio Agent.app/Contents/MacOS/HuishengStudio Agent";

inline constexpr const char* kDriverBundleID     = "com.huishengstudio.hal";

// Four loopback pairs. The driver publishes each pair as two single-direction
// devices, on one sample clock, so Sound settings can name the outgoing and the
// incoming half differently: an output half that apps write into and an input
// half that hands the same audio back.
inline constexpr int kDevicePairs = 4;

// UIDs must match driver/HuishengStudioDriver.cpp exactly.
inline constexpr const char* kPairOutputUID[kDevicePairs] = {
    "com.huishengstudio.driver.sc12",
    "com.huishengstudio.driver.sc34",
    "com.huishengstudio.driver.sc56",
    "com.huishengstudio.driver.sc78",
};

inline constexpr const char* kPairInputUID[kDevicePairs] = {
    "com.huishengstudio.driver.sr12",
    "com.huishengstudio.driver.sr34",
    "com.huishengstudio.driver.sr56",
    "com.huishengstudio.driver.sr78",
};

// Device names stay ASCII: OBS, host sessions and command-line tools all quote
// them, and a non-ASCII name has to survive every one of those hops. SC is the
// half audio goes out through, SR the half it comes back on.
inline constexpr const char* kPairOutputName[kDevicePairs] = {
    "HsAudioSC 1/2",
    "HsAudioSC 3/4",
    "HsAudioSC 5/6",
    "HsAudioSC 7/8",
};

inline constexpr const char* kPairInputName[kDevicePairs] = {
    "HsAudioSR 1/2",
    "HsAudioSR 3/4",
    "HsAudioSR 5/6",
    "HsAudioSR 7/8",
};

// Pair 1/2 is the default DAW -> OBS / 抖音 / 微信 microphone bus.
inline constexpr int kDefaultReturnPair  = 0;
// Pair 3/4 is the default capture source for external players.
inline constexpr int kDefaultCapturePair = 1;

// Capture slots: one per Capture plug-in instance. A slot owns its own control
// fields and its own rings, so two instances can capture two different Apps
// (or two virtual channels) at the same time instead of overwriting each
// other's selection and sharing a single ring's read pointer.
inline constexpr int kCaptureSlots = 8;

// Apps one instance may capture at once. Several Apps go into one track when
// the user wants a group (all the music players on one fader), so a slot owns
// one tap ring per App index plus the single virtual channel ring.
inline constexpr int kSlotApps = 8;

// A slot whose plug-in stopped beating is treated as free after this long. The
// Agent releases its tap, and the next instance may claim the slot.
inline constexpr uint64_t kSlotLeaseNanos = 5'000'000'000ull;

inline constexpr uint32_t kRingChannels   = 2;
inline constexpr uint32_t kRingCapacity   = 192000; // 4 s at 48 kHz
inline constexpr uint32_t kDefaultRate    = 48000;

// Target standing buffer inside every cross-process/cross-device ring. The
// resampler holds this constant, so this is the only latency the bridge adds.
inline constexpr uint32_t kTargetFrames    = 128;
inline constexpr uint32_t kMaxTargetFrames = 2048;

// Human readable pair label, e.g. "HsAudioSC 3/4".
inline std::string pairLabel(int index)
{
    if (index < 0 || index >= kDevicePairs)
        return "—";
    return kPairOutputName[index];
}

// Either half of a pair maps back to the pair index: the system remembers
// whichever half the user picked in a host or in OBS.
inline int pairIndexForUID(const std::string& uid)
{
    for (int i = 0; i < kDevicePairs; ++i)
        if (uid == kPairOutputUID[i] || uid == kPairInputUID[i])
            return i;
    return -1;
}

} // namespace hs
