// Checks that the driver's reported property sizes match what it writes.
//
// coreaudiod allocates exactly GetPropertyDataSize() bytes and the driver
// writes into that buffer. A size that is too small is a heap overflow inside
// coreaudiod: it shows up as
//
//     HALS_UCRemotePlugIn::ObjectGetPropertyDataSize: failed: ..., Error: 2003332927
//     HALS_PlugIn.cpp:1518  Caught exception trying to add device
//
// and the devices never appear. Reporting sizeof(UInt32) for the CFString
// properties (device UID, model UID) did exactly that, so every property the
// host is known to query is checked here with a guard region after the buffer.
#include "../driver/HuishengStudioDriver.cpp"
#include "Config.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(AudioObjectID object, UInt32 selector, UInt32 scope, const char* name)
{
    AudioObjectPropertyAddress address { selector, static_cast<AudioObjectPropertyScope>(scope),
                                         kAudioObjectPropertyElementMain };

    if (! hs_HasProperty(gDriverRef, object, 0, &address))
        return;

    UInt32 size = 0;
    const OSStatus sizeStatus = hs_GetPropertyDataSize(gDriverRef, object, 0, &address, 0, nullptr, &size);

    if (sizeStatus != kAudioHardwareNoError)
    {
        std::printf("  FAIL  %-26s object %2u size status %d\n", name, object, (int) sizeStatus);
        ++failures;
        return;
    }

    if (size == 0)
        return;

    constexpr UInt32 guard = 64;
    std::vector<unsigned char> buffer(size + guard, 0xAB);

    UInt32 written = size;
    const OSStatus readStatus = hs_GetPropertyData(gDriverRef, object, 0, &address, 0, nullptr,
                                                    size, &written, buffer.data());

    bool overflowed = false;
    for (UInt32 i = size; i < size + guard; ++i)
        overflowed = overflowed || buffer[i] != 0xAB;

    if (readStatus != kAudioHardwareNoError || overflowed || written != size)
    {
        std::printf("  FAIL  %-26s object %2u size %u written %u status %d%s\n",
                    name, object, size, written, (int) readStatus,
                    overflowed ? " overflow" : "");
        ++failures;
    }
}

} // namespace

namespace {

// The topology is a contract: shared/Config.h hands these names, UIDs and
// directions to the agent, the plug-ins and the command-line tools, so a drift
// between the driver and the config is caught here, with no Mac involved.
void fail(const char* name, const char* detail)
{
    std::printf("  FAIL  %-34s %s\n", name, detail);
    ++failures;
}

void expectNumber(AudioObjectID object, UInt32 selector, UInt32 scope, UInt32 want, const char* name)
{
    AudioObjectPropertyAddress address { selector, static_cast<AudioObjectPropertyScope>(scope),
                                         kAudioObjectPropertyElementMain };
    UInt32 value = 0xFFFFFFFF, size = sizeof(value);

    if (hs_GetPropertyData(gDriverRef, object, 0, &address, 0, nullptr, size, &size, &value)
            != kAudioHardwareNoError || value != want)
    {
        char detail[96] = {};
        std::snprintf(detail, sizeof(detail), "want %u, got %u", (unsigned) want, (unsigned) value);
        fail(name, detail);
        return;
    }

    std::printf("  ok    %-34s %u\n", name, (unsigned) value);
}

void expectString(AudioObjectID object, UInt32 selector, const char* want, const char* name)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (hs_GetPropertyDataSize(gDriverRef, object, 0, &address, 0, nullptr, &size)
            != kAudioHardwareNoError || size != sizeof(CFStringRef))
    {
        fail(name, "no CFStringRef property");
        return;
    }

    CFStringRef value = nullptr;

    if (hs_GetPropertyData(gDriverRef, object, 0, &address, 0, nullptr, size, &size, &value)
            != kAudioHardwareNoError || value == nullptr)
    {
        fail(name, "unreadable CFStringRef property");
        return;
    }

    char buffer[256] = {};
    CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(value);

    if (std::strcmp(buffer, want) != 0)
    {
        std::string detail = std::string("want \"") + want + "\", got \"" + buffer + "\"";
        fail(name, detail.c_str());
        return;
    }

    std::printf("  ok    %-34s %s\n", name, buffer);
}

void expectStreams(AudioObjectID device, UInt32 scope, AudioObjectID want, const char* name)
{
    AudioObjectPropertyAddress address { kAudioDevicePropertyStreams,
                                         static_cast<AudioObjectPropertyScope>(scope),
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (hs_GetPropertyDataSize(gDriverRef, device, 0, &address, 0, nullptr, &size) != kAudioHardwareNoError)
    {
        fail(name, "streams size failed");
        return;
    }

    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID), 0);

    if (size > 0 && hs_GetPropertyData(gDriverRef, device, 0, &address, 0, nullptr, size, &size, ids.data())
                        != kAudioHardwareNoError)
    {
        fail(name, "streams read failed");
        return;
    }

    if (want == 0)
    {
        if (! ids.empty())
        {
            fail(name, "expected no stream in this scope");
            return;
        }
    }
    else if (ids.size() != 1 || ids[0] != want)
    {
        char detail[96] = {};
        std::snprintf(detail, sizeof(detail), "want stream %u, got %u stream(s)",
                      (unsigned) want, (unsigned) ids.size());
        fail(name, detail);
        return;
    }

    std::printf("  ok    %-34s %u stream(s)\n", name, (unsigned) ids.size());
}

void expectBuffers(AudioObjectID device, bool outputHalf, const char* name)
{
    struct Case { UInt32 scope; UInt32 want; };

    const Case cases[] = {
        { kAudioObjectPropertyScopeGlobal, 1 },
        { kAudioObjectPropertyScopeOutput, outputHalf ? 1u : 0u },
        { kAudioObjectPropertyScopeInput,  outputHalf ? 0u : 1u },
    };

    for (const auto& c : cases)
    {
        AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                             static_cast<AudioObjectPropertyScope>(c.scope),
                                             kAudioObjectPropertyElementMain };
        UInt32 size = 0;

        if (hs_GetPropertyDataSize(gDriverRef, device, 0, &address, 0, nullptr, &size) != kAudioHardwareNoError
            || size < sizeof(AudioBufferList))
        {
            fail(name, "stream configuration size failed");
            return;
        }

        std::vector<unsigned char> buffer(size + 64, 0xAB);
        UInt32 written = size;

        if (hs_GetPropertyData(gDriverRef, device, 0, &address, 0, nullptr, size, &written, buffer.data())
                != kAudioHardwareNoError)
        {
            fail(name, "stream configuration read failed");
            return;
        }

        const auto* list = reinterpret_cast<const AudioBufferList*>(buffer.data());

        // A single-direction device reports its own stereo buffer in the scope it
        // owns (and in the global scope) and zero buffers in the other one.
        if (list->mNumberBuffers != c.want
            || (c.want > 0 && list->mBuffers[0].mNumberChannels != 2))
        {
            char detail[96] = {};
            std::snprintf(detail, sizeof(detail), "scope %u: want %u buffer(s), got %u",
                          (unsigned) c.scope, (unsigned) c.want, (unsigned) list->mNumberBuffers);
            fail(name, detail);
            return;
        }
    }

    std::printf("  ok    %-34s %s\n", name, outputHalf ? "output half: 1 buffer out, 0 in"
                                                       : "input half: 1 buffer in, 0 out");
}

} // namespace

int main()
{
    hs_Initialize(gDriverRef, nullptr);

    struct Query { UInt32 selector; UInt32 scope; const char* name; };

    const Query pluginQueries[] = {
        { kAudioObjectPropertyBaseClass,       kAudioObjectPropertyScopeGlobal, "plug-in base class" },
        { kAudioObjectPropertyClass,           kAudioObjectPropertyScopeGlobal, "plug-in class" },
        { kAudioObjectPropertyManufacturer,    kAudioObjectPropertyScopeGlobal, "plug-in manufacturer" },
        { kAudioObjectPropertyOwnedObjects,    kAudioObjectPropertyScopeGlobal, "plug-in owned objects" },
        { kAudioPlugInPropertyDeviceList,      kAudioObjectPropertyScopeGlobal, "plug-in device list" },
        { kAudioPlugInPropertyBundleID,        kAudioObjectPropertyScopeGlobal, "plug-in bundle id" },
        { kAudioPlugInPropertyResourceBundle,  kAudioObjectPropertyScopeGlobal, "plug-in resource bundle" },
    };

    const Query deviceQueries[] = {
        { kAudioObjectPropertyBaseClass,                    kAudioObjectPropertyScopeGlobal, "device base class" },
        { kAudioObjectPropertyClass,                        kAudioObjectPropertyScopeGlobal, "device class" },
        { kAudioObjectPropertyOwner,                        kAudioObjectPropertyScopeGlobal, "device owner" },
        { kAudioObjectPropertyName,                         kAudioObjectPropertyScopeGlobal, "device name" },
        { kAudioObjectPropertyManufacturer,                 kAudioObjectPropertyScopeGlobal, "device manufacturer" },
        { kAudioObjectPropertyOwnedObjects,                 kAudioObjectPropertyScopeGlobal, "device owned objects" },
        { kAudioObjectPropertyControlList,                  kAudioObjectPropertyScopeGlobal, "device control list" },
        { kAudioDevicePropertyDeviceUID,                    kAudioObjectPropertyScopeGlobal, "device uid" },
        { kAudioDevicePropertyModelUID,                     kAudioObjectPropertyScopeGlobal, "device model uid" },
        { kAudioDevicePropertyTransportType,                kAudioObjectPropertyScopeGlobal, "device transport" },
        { kAudioDevicePropertyRelatedDevices,               kAudioObjectPropertyScopeGlobal, "device related" },
        { kAudioDevicePropertyClockDomain,                  kAudioObjectPropertyScopeGlobal, "device clock domain" },
        { kAudioDevicePropertyDeviceIsAlive,                kAudioObjectPropertyScopeGlobal, "device is alive" },
        { kAudioDevicePropertyDeviceIsRunning,              kAudioObjectPropertyScopeGlobal, "device is running" },
        { kAudioDevicePropertyDeviceIsRunningSomewhere,     kAudioObjectPropertyScopeGlobal, "device running anywhere" },
        { kAudioDevicePropertyDeviceCanBeDefaultDevice,     kAudioObjectPropertyScopeGlobal, "device can be default" },
        { kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeGlobal, "device can be system" },
        { kAudioDevicePropertyHogMode,                      kAudioObjectPropertyScopeGlobal, "device hog mode" },
        { kAudioDevicePropertyLatency,                      kAudioObjectPropertyScopeGlobal, "device latency" },
        { kAudioDevicePropertySafetyOffset,                 kAudioObjectPropertyScopeGlobal, "device safety offset" },
        { kAudioDevicePropertyNominalSampleRate,            kAudioObjectPropertyScopeGlobal, "device sample rate" },
        { kAudioDevicePropertyAvailableNominalSampleRates,  kAudioObjectPropertyScopeGlobal, "device sample rates" },
        { kAudioDevicePropertyIsHidden,                     kAudioObjectPropertyScopeGlobal, "device is hidden" },
        { kAudioDevicePropertyPreferredChannelsForStereo,   kAudioObjectPropertyScopeGlobal, "device stereo channels" },
        { kAudioDevicePropertyPreferredChannelLayout,       kAudioObjectPropertyScopeGlobal, "device channel layout" },
        { kAudioDevicePropertyZeroTimeStampPeriod,          kAudioObjectPropertyScopeGlobal, "device zero period" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeGlobal, "device streams" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeInput,  "device input streams" },
        { kAudioDevicePropertyStreams,                      kAudioObjectPropertyScopeOutput, "device output streams" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeGlobal, "device stream config" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeInput,  "device input config" },
        { kAudioDevicePropertyStreamConfiguration,          kAudioObjectPropertyScopeOutput, "device output config" },
    };

    const Query streamQueries[] = {
        { kAudioObjectPropertyBaseClass,                   kAudioObjectPropertyScopeGlobal, "stream base class" },
        { kAudioObjectPropertyClass,                       kAudioObjectPropertyScopeGlobal, "stream class" },
        { kAudioObjectPropertyOwner,                       kAudioObjectPropertyScopeGlobal, "stream owner" },
        { kAudioStreamPropertyIsActive,                    kAudioObjectPropertyScopeGlobal, "stream is active" },
        { kAudioStreamPropertyDirection,                   kAudioObjectPropertyScopeGlobal, "stream direction" },
        { kAudioStreamPropertyTerminalType,                kAudioObjectPropertyScopeGlobal, "stream terminal type" },
        { kAudioStreamPropertyStartingChannel,             kAudioObjectPropertyScopeGlobal, "stream start channel" },
        { kAudioStreamPropertyLatency,                     kAudioObjectPropertyScopeGlobal, "stream latency" },
        { kAudioStreamPropertyVirtualFormat,               kAudioObjectPropertyScopeGlobal, "stream virtual format" },
        { kAudioStreamPropertyPhysicalFormat,              kAudioObjectPropertyScopeGlobal, "stream physical format" },
        { kAudioStreamPropertyAvailableVirtualFormats,     kAudioObjectPropertyScopeGlobal, "stream virtual formats" },
        { kAudioStreamPropertyAvailablePhysicalFormats,    kAudioObjectPropertyScopeGlobal, "stream physical formats" },
    };

    for (const auto& query : pluginQueries)
        check(kObjectID_PlugIn, query.selector, query.scope, query.name);

    for (int i = 0; i < HS_DEVICE_COUNT; ++i)
    {
        const AudioObjectID device = kObjectID_Device0 + i;

        for (const auto& query : deviceQueries)
            check(device, query.selector, query.scope, query.name);
    }

    // Streams exist once per pair, not once per device: the pair's output stream
    // belongs to the output half and its input stream to the input half.
    for (int pair = 0; pair < HS_PAIR_COUNT; ++pair)
    {
        for (const auto& query : streamQueries)
        {
            check(kObjectID_StreamInputBase + pair, query.selector, query.scope, query.name);
            check(kObjectID_StreamOutputBase + pair, query.selector, query.scope, query.name);
        }
    }

    std::printf("\ntopology\n");

    for (int pair = 0; pair < HS_PAIR_COUNT; ++pair)
    {
        const AudioObjectID outputHalf = kObjectID_Device0 + pair;
        const AudioObjectID inputHalf  = kObjectID_Device0 + pair + HS_PAIR_COUNT;

        expectString(outputHalf, kAudioObjectPropertyName, hs::kPairOutputName[pair], "output half name");
        expectString(outputHalf, kAudioDevicePropertyDeviceUID, hs::kPairOutputUID[pair], "output half uid");
        expectString(inputHalf, kAudioObjectPropertyName, hs::kPairInputName[pair], "input half name");
        expectString(inputHalf, kAudioDevicePropertyDeviceUID, hs::kPairInputUID[pair], "input half uid");

        expectNumber(outputHalf, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeGlobal,
                     0, "output half latency");
        expectNumber(inputHalf, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeGlobal,
                     kPairReadLagFrames, "input half latency");

        expectStreams(outputHalf, kAudioObjectPropertyScopeGlobal,
                      kObjectID_StreamOutputBase + pair, "output half owns its stream");
        expectStreams(outputHalf, kAudioObjectPropertyScopeInput, 0, "output half has no input stream");
        expectStreams(inputHalf, kAudioObjectPropertyScopeGlobal,
                      kObjectID_StreamInputBase + pair, "input half owns its stream");
        expectStreams(inputHalf, kAudioObjectPropertyScopeOutput, 0, "input half has no output stream");

        expectNumber(outputHalf, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput,
                     1, "output half can be the default out");
        expectNumber(outputHalf, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput,
                     0, "output half cannot be the default in");
        expectNumber(inputHalf, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput,
                     0, "input half cannot be the default out");
        expectNumber(inputHalf, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput,
                     1, "input half can be the default in");

        expectBuffers(outputHalf, true, "output half buffers");
        expectBuffers(inputHalf, false, "input half buffers");
    }

    std::printf("\nproperty size/data mismatches: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
