/*
   绘声Studio — huishengstudio virtual audio driver

   One HAL plug-in bundle that publishes eight single-direction 2-channel
   devices: four output halves ("HsAudioSC 1/2" … "HsAudioSC 7/8") and four
   input halves ("HsAudioSR 1/2" … "HsAudioSR 7/8"). The output half and the
   input half sharing a number are one loopback pair: they run on the same
   sample clock and the same sample-time addressed ring, so audio written to
   "HsAudioSC 3/4" is read back from "HsAudioSR 3/4". Sound settings keeps one
   name per device and one direction per device, and a single duplex device
   cannot show 输出 and 输入 differently. No shared file, no second bundle, no
   clock-domain crossing, and no resampling.

   Derived in structure from Apple's AudioServerPlugIn sample code and
   SoundPusher (see LICENSE). The loopback and timing code is new.
*/

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <os/lock.h>
#include <os/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <atomic>

//==================================================================================================
#pragma mark Configuration
//==================================================================================================

#ifndef HS_BUNDLE_ID
#define HS_BUNDLE_ID  "com.huishengstudio.hal"
#endif

// Per-bundle suffix so two copies of the same plug-in can coexist: macOS keys
// HAL plug-ins by CFBundleIdentifier and silently ignores duplicates.
#ifndef HS_BUNDLE_SUFFIX
#define HS_BUNDLE_SUFFIX ""
#endif
#define HS_DRIVER_NAME "HsAudio"
#define HS_MANUFACTURER "HsAudio"
// Four loopback pairs, published as eight single-direction devices:
// device index 0..3 = "HsAudioSC n/n" (output half, writes the pair's ring),
// device index 4..7 = "HsAudioSR n/n" (input half, reads the pair's ring).
#define HS_PAIR_COUNT 4
#define HS_DEVICE_COUNT (HS_PAIR_COUNT * 2)

// Loopback storage. 16384 frames (341 ms at 48 kHz) is address space, not
// delay: audio is addressed by sample time, so the loop only costs one IO
// cycle plus the safety offset.
static constexpr uint32_t kRingFrames = 16384;

// How often GetZeroTimeStamp advances. Small enough for fine timing at 32-64
// frame IO sizes, large enough that the HAL is not calling us constantly.
static constexpr uint32_t kZeroPeriod = 1024;

// The writer and the reader are two separate device objects now, so the HAL is
// free to run the reader's cycle before the writer's cycle for the same period.
// The output side is always ahead of the input side by a buffer, so one zero
// period (1024 frames, ~21 ms) of lag already leaves several IO periods of
// slack at a 128-frame buffer, while costing a fraction of what two periods
// would add to the loopback.
static constexpr uint32_t kPairReadLagFrames = kZeroPeriod;

static constexpr Float64 kSampleRates[] = { 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
static constexpr UInt32  kSampleRateCount = sizeof(kSampleRates) / sizeof(kSampleRates[0]);

static const char* const kDeviceUIDBase[HS_DEVICE_COUNT] = {
    "com.huishengstudio.driver.sc12",
    "com.huishengstudio.driver.sc34",
    "com.huishengstudio.driver.sc56",
    "com.huishengstudio.driver.sc78",
    "com.huishengstudio.driver.sr12",
    "com.huishengstudio.driver.sr34",
    "com.huishengstudio.driver.sr56",
    "com.huishengstudio.driver.sr78",
};

static std::string deviceUID(int index)
{
    return std::string(kDeviceUIDBase[index]) + HS_BUNDLE_SUFFIX;
}

static const char* const kDeviceNameBase[HS_DEVICE_COUNT] = {
    HS_DRIVER_NAME "SC 1/2",
    HS_DRIVER_NAME "SC 3/4",
    HS_DRIVER_NAME "SC 5/6",
    HS_DRIVER_NAME "SC 7/8",
    HS_DRIVER_NAME "SR 1/2",
    HS_DRIVER_NAME "SR 3/4",
    HS_DRIVER_NAME "SR 5/6",
    HS_DRIVER_NAME "SR 7/8",
};

#ifndef HS_NAME_SUFFIX
#define HS_NAME_SUFFIX ""
#endif

static std::string deviceName(int index)
{
    return std::string(kDeviceNameBase[index]) + HS_NAME_SUFFIX;
}

//==================================================================================================
#pragma mark Object IDs
//==================================================================================================

enum : AudioObjectID
{
    kObjectID_PlugIn          = kAudioObjectPlugInObject,   // 1
    kObjectID_Device0         = 2,                          // devices 2..9
    kObjectID_DeviceLast      = kObjectID_Device0 + HS_DEVICE_COUNT - 1,
    kObjectID_StreamInputBase = 10,                         // 10..13, the pairs' input halves
    kObjectID_StreamOutputBase = 20                          // 20..23, the pairs' output halves
};

static inline bool isDevice(AudioObjectID id) { return id >= kObjectID_Device0 && id <= kObjectID_DeviceLast; }
static inline bool isInputStream(AudioObjectID id)  { return id >= kObjectID_StreamInputBase  && id < kObjectID_StreamInputBase  + HS_PAIR_COUNT; }
static inline bool isOutputStream(AudioObjectID id) { return id >= kObjectID_StreamOutputBase && id < kObjectID_StreamOutputBase + HS_PAIR_COUNT; }
static inline int deviceIndex(AudioObjectID id) { return static_cast<int>(id) - kObjectID_Device0; }

// Device index → loopback pair: 0..3 are the output halves, 4..7 the input
// halves, so the pair is the index modulo the pair count.
static inline int pairOfDevice(int index) { return index % HS_PAIR_COUNT; }
static inline bool deviceHasOutput(int index) { return index < HS_PAIR_COUNT; }
static inline bool deviceHasInput(int index) { return index >= HS_PAIR_COUNT; }
static inline AudioObjectID deviceForDirection(int pair, bool input)
{
    return static_cast<AudioObjectID>(kObjectID_Device0 + (input ? pair + HS_PAIR_COUNT : pair));
}

// Stream objects are per pair and direction: 10..13 belong to the input halves,
// 20..23 to the output halves of the same pair.
static inline int inputStreamPair(AudioObjectID id)  { return static_cast<int>(id) - kObjectID_StreamInputBase; }
static inline int outputStreamPair(AudioObjectID id) { return static_cast<int>(id) - kObjectID_StreamOutputBase; }

enum : UInt64 { kChangeRequest_SampleRate = 1 };

//==================================================================================================
#pragma mark Driver state
//==================================================================================================

// Sample-time addressed loopback buffer. One producer (WriteMix) and one
// consumer (ReadInput) touch it from the same HAL IO thread, so no locks are
// needed beyond the atomics used to publish how far the writer has reached.
struct LoopRing
{
    float*   storage = nullptr;                 // kRingFrames * 2, zeroed
    uint64_t anchorSampleTime = 0;
    uint64_t anchorHostTime = 0;
    uint64_t timelineSeed = 1;

    std::atomic<int64_t> writtenHead { 0 };     // absolute sample index written so far
    std::atomic<int64_t> writtenTail { 0 };     // oldest absolute sample still valid
    uint32_t safetyFrames = 0;

    bool allocate()
    {
        if (storage != nullptr)
            return true;

        storage = static_cast<float*>(calloc(static_cast<size_t>(kRingFrames) * 2, sizeof(float)));
        return storage != nullptr;
    }

    void clear()
    {
        if (storage != nullptr)
            memset(storage, 0, static_cast<size_t>(kRingFrames) * 2 * sizeof(float));

        writtenHead.store(0, std::memory_order_relaxed);
        writtenTail.store(0, std::memory_order_relaxed);
    }

    void write(const float* src, uint32_t frames, int64_t sampleTime)
    {
        if (storage == nullptr || src == nullptr || frames == 0)
            return;

        const uint32_t start = static_cast<uint32_t>(sampleTime % kRingFrames);
        const uint32_t first = std::min(frames, kRingFrames - start);

        memcpy(storage + static_cast<size_t>(start) * 2, src,
               static_cast<size_t>(first) * 2 * sizeof(float));

        if (frames > first)
            memcpy(storage, src + static_cast<size_t>(first) * 2,
                   static_cast<size_t>(frames - first) * 2 * sizeof(float));

        const int64_t end = sampleTime + static_cast<int64_t>(frames);
        writtenHead.store(end, std::memory_order_release);
        writtenTail.store(std::max<int64_t>(writtenTail.load(std::memory_order_relaxed),
                                            end - static_cast<int64_t>(kRingFrames)),
                          std::memory_order_release);
    }

    // Copies `frames` starting at `sampleTime`. Anything not yet written (first
    // cycles, or after a timeline reset) comes back as silence.
    void read(float* dst, uint32_t frames, int64_t sampleTime) const
    {
        if (storage == nullptr || dst == nullptr || frames == 0)
            return;

        const int64_t tail = writtenTail.load(std::memory_order_acquire);
        const int64_t head = writtenHead.load(std::memory_order_acquire);

        const int64_t from = std::max<int64_t>(sampleTime, tail);
        const int64_t to = std::min<int64_t>(sampleTime + static_cast<int64_t>(frames), head);

        memset(dst, 0, static_cast<size_t>(frames) * 2 * sizeof(float));

        if (to <= from)
            return;

        const int64_t count = to - from;
        const uint32_t start = static_cast<uint32_t>(from % kRingFrames);
        const uint32_t first = static_cast<uint32_t>(std::min<int64_t>(count, kRingFrames - start));

        memcpy(dst + static_cast<size_t>(from - sampleTime) * 2,
               storage + static_cast<size_t>(start) * 2,
               static_cast<size_t>(first) * 2 * sizeof(float));

        if (count > first)
            memcpy(dst + static_cast<size_t>(from - sampleTime + first) * 2, storage,
                   static_cast<size_t>(count - first) * 2 * sizeof(float));
    }
};

struct DeviceState
{
    AudioStreamBasicDescription format {};
    AudioChannelLayout channelLayout {};
    Float64 hostTicksPerFrame = 0.0;

    uint32_t ioIsRunning = 0;           // clients with IO running on either half
    uint32_t inputActive = 1;           // reported by the pair's input half
    uint32_t outputActive = 1;          // reported by the pair's output half
    uint32_t mute = 0;

    uint64_t numberTimeStamps = 0;
    Float64  anchorTicks = 0.0;         // ticks elapsed at the current zero time stamp
    uint64_t anchorHostTime = 0;
    uint64_t timelineSeed = 1;

    LoopRing ring;
    float* scratch = nullptr;           // 16384 * 2, used to interleave into a copy
};

static UInt32 gPlugInRefCount = 0;
static AudioServerPlugInHostRef gHost = NULL;
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static os_unfair_lock gIOLock = OS_UNFAIR_LOCK_INIT;
static DeviceState gPairs[HS_PAIR_COUNT];

// The HAL addresses eight device objects; the engine behind each one is its pair.
static inline DeviceState& engineState(int index) { return gPairs[pairOfDevice(index)]; }

static Float64 gHostTicksPerSecond = 0.0;

static void initDeviceFormat(DeviceState& d)
{
    d.format.mSampleRate = 48000.0;
    d.format.mFormatID = kAudioFormatLinearPCM;
    d.format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    d.format.mBytesPerPacket = sizeof(float) * 2;
    d.format.mFramesPerPacket = 1;
    d.format.mBytesPerFrame = sizeof(float) * 2;
    d.format.mChannelsPerFrame = 2;
    d.format.mBitsPerChannel = sizeof(float) * 8;
    d.format.mReserved = 0;

    d.channelLayout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
    d.channelLayout.mChannelBitmap = 0;
    d.channelLayout.mNumberChannelDescriptions = 0;
}

static void updateTicksPerFrame(DeviceState& d)
{
    d.hostTicksPerFrame = d.format.mSampleRate > 0.0
                              ? gHostTicksPerSecond / d.format.mSampleRate
                              : 0.0;
}

static void resetTimeline(DeviceState& d)
{
    d.numberTimeStamps = 0;
    d.anchorTicks = 0.0;
    d.anchorHostTime = mach_absolute_time();
    d.timelineSeed += 1;
    d.ring.clear();
}

//==================================================================================================
#pragma mark Helpers
//==================================================================================================

static void notifyChanged(AudioObjectID object, AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope, AudioObjectPropertyElement element)
{
    if (gHost == NULL)
        return;

    AudioObjectPropertyAddress address { selector, scope, element };
    gHost->PropertiesChanged(gHost, object, 1, &address);
}

//==================================================================================================
#pragma mark Interface
//==================================================================================================


static HRESULT  hs_QueryInterface(void*, REFIID, LPVOID*);
static ULONG    hs_AddRef(void*);
static ULONG    hs_Release(void*);
static OSStatus hs_Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus hs_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*);
static OSStatus hs_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus hs_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus hs_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*);
static OSStatus hs_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static OSStatus hs_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);
static Boolean  hs_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
static OSStatus hs_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
static OSStatus hs_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
static OSStatus hs_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
static OSStatus hs_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
static OSStatus hs_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus hs_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus hs_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);

static OSStatus hs_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
static OSStatus hs_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);
static OSStatus hs_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
static OSStatus hs_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface gInterface =
{
    NULL,
    hs_QueryInterface,
    hs_AddRef,
    hs_Release,
    hs_Initialize,
    hs_CreateDevice,
    hs_DestroyDevice,
    hs_AddDeviceClient,
    hs_RemoveDeviceClient,
    hs_PerformDeviceConfigurationChange,
    hs_AbortDeviceConfigurationChange,
    hs_HasProperty,
    hs_IsPropertySettable,
    hs_GetPropertyDataSize,
    hs_GetPropertyData,
    hs_SetPropertyData,
    hs_StartIO,
    hs_StopIO,
    hs_GetZeroTimeStamp,
    hs_WillDoIOOperation,
    hs_BeginIOOperation,
    hs_DoIOOperation,
    hs_EndIOOperation
};

static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

//==================================================================================================
#pragma mark Factory / lifetime
//==================================================================================================

extern "C" void* HuishengStudioDriver_Create(CFAllocatorRef, CFUUIDRef);

void* HuishengStudioDriver_Create(CFAllocatorRef, CFUUIDRef requestedTypeUUID)
{
    if (CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID))
        return gDriverRef;

    return NULL;
}

static HRESULT hs_QueryInterface(void* driver, REFIID uuid, LPVOID* outInterface)
{
    if (driver != gDriverRef || outInterface == NULL)
        return kAudioHardwareBadObjectError;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT result = E_NOINTERFACE;

    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gStateMutex);
        ++gPlugInRefCount;
        pthread_mutex_unlock(&gStateMutex);

        *outInterface = gDriverRef;
        result = S_OK;
    }

    CFRelease(requested);
    return result;
}

static ULONG hs_AddRef(void* driver)
{
    if (driver != gDriverRef)
        return 0;

    pthread_mutex_lock(&gStateMutex);
    const ULONG result = ++gPlugInRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return result;
}

static ULONG hs_Release(void* driver)
{
    if (driver != gDriverRef)
        return 0;

    pthread_mutex_lock(&gStateMutex);
    if (gPlugInRefCount > 0)
        --gPlugInRefCount;
    const ULONG result = gPlugInRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return result;
}

static OSStatus hs_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    if (driver != gDriverRef)
        return kAudioHardwareBadObjectError;

    gHost = host;

    struct mach_timebase_info timeBase;
    mach_timebase_info(&timeBase);
    gHostTicksPerSecond = (static_cast<Float64>(timeBase.denom) / static_cast<Float64>(timeBase.numer)) * 1.0e9;

    // Every pair publishes both halves, so every device carries exactly one
    // direction: the output halves show up under 输出, the input halves under
    // 输入. macOS only makes a default device out of one that really has a
    // stream, which each half has.
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT,
                     "huishengstudio HAL: Initialize (bundle %{public}s)", HS_BUNDLE_ID);

    for (int i = 0; i < HS_PAIR_COUNT; ++i)
    {
        initDeviceFormat(gPairs[i]);
        updateTicksPerFrame(gPairs[i]);
        resetTimeline(gPairs[i]);
    }

    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT,
                     "huishengstudio HAL: %d devices published (%d out + %d in on %d loopback pairs)",
                     HS_DEVICE_COUNT, HS_PAIR_COUNT, HS_PAIR_COUNT, HS_PAIR_COUNT);

    return kAudioHardwareNoError;
}

static OSStatus hs_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*)
{
    // The eight devices are static; the HAL never needs to create more.
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus hs_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus hs_AddDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                    const AudioServerPlugInClientInfo*)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    return kAudioHardwareNoError;
}

static OSStatus hs_RemoveDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                       const AudioServerPlugInClientInfo*)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    return kAudioHardwareNoError;
}

static OSStatus hs_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                                      UInt64 action, void* info)
{
    if (driver != gDriverRef || ! isDevice(device) || info == NULL)
        return kAudioHardwareBadObjectError;

    if (action != kChangeRequest_SampleRate)
        return kAudioHardwareBadObjectError;

    auto* newFormat = static_cast<AudioStreamBasicDescription*>(info);
    const int changed = deviceIndex(device);
    auto& state = engineState(changed);

    pthread_mutex_lock(&gStateMutex);
    state.format = *newFormat;
    updateTicksPerFrame(state);
    resetTimeline(state);
    pthread_mutex_unlock(&gStateMutex);

    // Tell the clients the change actually landed. Without this a host that
    // asked for a rate keeps polling for it (and can poll a second client into
    // asking again) instead of settling.
    // Both halves of a pair share one engine, so the rate lands on both. Every
    // object that reports the rate has to hear about it, or a host polling the
    // other half keeps asking for a rate it already has.
    const int pair = pairOfDevice(changed);
    const AudioObjectID halves[2] = { deviceForDirection(pair, false),
                                      deviceForDirection(pair, true) };

    for (AudioObjectID half : halves)
    {
        notifyChanged(half, kAudioDevicePropertyNominalSampleRate,
                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
        notifyChanged(half, kAudioDevicePropertyActualSampleRate,
                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    }

    notifyChanged(static_cast<AudioObjectID>(kObjectID_StreamInputBase + pair),
                  kAudioStreamPropertyVirtualFormat,
                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    notifyChanged(static_cast<AudioObjectID>(kObjectID_StreamOutputBase + pair),
                  kAudioStreamPropertyVirtualFormat,
                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);

    free(newFormat);
    return kAudioHardwareNoError;
}

static OSStatus hs_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                                    UInt64, void*)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    return kAudioHardwareNoError;
}

//==================================================================================================
#pragma mark Property: HasProperty
//==================================================================================================

static Boolean hs_HasProperty(AudioServerPlugInDriverRef driver, AudioObjectID object,
                               pid_t, const AudioObjectPropertyAddress* address)
{
    if (driver != gDriverRef || address == NULL)
        return false;

    const AudioObjectPropertyScope scope = address->mScope;

    if (object == kObjectID_PlugIn)
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyBundleID:
            case kAudioPlugInPropertyResourceBundle:
                return scope == kAudioObjectPropertyScopeGlobal;
            default:
                return false;
        }
    }

    if (isDevice(object))
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyHogMode:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyActualSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyStreamConfiguration:
            case kAudioObjectPropertyControlList:
                return scope == kAudioObjectPropertyScopeGlobal
                       || scope == kAudioObjectPropertyScopeInput
                       || scope == kAudioObjectPropertyScopeOutput;

            default:
                return false;
        }
    }

    if (isInputStream(object) || isOutputStream(object))
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return scope == kAudioObjectPropertyScopeGlobal;
            default:
                return false;
        }
    }

    return false;
}

static OSStatus hs_IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                       pid_t, const AudioObjectPropertyAddress* address, Boolean* outSettable)
{
    if (driver != gDriverRef || address == NULL || outSettable == NULL)
        return kAudioHardwareIllegalOperationError;

    *outSettable = false;

    if (isDevice(object) && address->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        *outSettable = true;
        return kAudioHardwareNoError;
    }

    if (isInputStream(object) || isOutputStream(object))
    {
        if (address->mSelector == kAudioStreamPropertyIsActive
            || address->mSelector == kAudioStreamPropertyVirtualFormat
            || address->mSelector == kAudioStreamPropertyPhysicalFormat)
        {
            *outSettable = true;
            return kAudioHardwareNoError;
        }
    }

    return kAudioHardwareNoError;
}

//==================================================================================================
#pragma mark Property: data size
//==================================================================================================

static OSStatus hs_GetPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                        pid_t, const AudioObjectPropertyAddress* address,
                                        UInt32, const void*, UInt32* outSize)
{
    if (driver != gDriverRef || address == NULL || outSize == NULL)
        return kAudioHardwareIllegalOperationError;

    const AudioObjectPropertyScope scope = address->mScope;
    *outSize = 0;

    if (object == kObjectID_PlugIn)
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
                *outSize = sizeof(AudioClassID);
                break;
            case kAudioPlugInPropertyDeviceList:
                *outSize = HS_DEVICE_COUNT * sizeof(AudioObjectID);
                break;
            case kAudioObjectPropertyOwnedObjects:
                *outSize = HS_DEVICE_COUNT * sizeof(AudioObjectID);
                break;
            case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyResourceBundle:
                *outSize = sizeof(CFStringRef);
                break;
            case kAudioPlugInPropertyBundleID:
                *outSize = sizeof(CFStringRef);
                break;
            case kAudioPlugInPropertyTranslateUIDToDevice:
                *outSize = sizeof(AudioObjectID);
                break;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
        return kAudioHardwareNoError;
    }

    if (isDevice(object))
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyZeroTimeStampPeriod:
                *outSize = sizeof(UInt32);
                break;
            case kAudioDevicePropertyHogMode:
                *outSize = sizeof(pid_t);
                break;
            case kAudioObjectPropertyControlList:
                *outSize = 0;
                break;
            // These return a CFStringRef. Reporting sizeof(UInt32) here makes
            // the host allocate four bytes and the driver write eight, which
            // corrupts coreaudiod's heap and aborts device activation.
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                *outSize = sizeof(CFStringRef);
                break;
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyActualSampleRate:
                *outSize = sizeof(Float64);
                break;
            case kAudioDevicePropertyAvailableNominalSampleRates:
                *outSize = kSampleRateCount * sizeof(AudioValueRange);
                break;
            case kAudioDevicePropertyRelatedDevices:
                *outSize = HS_DEVICE_COUNT * sizeof(AudioObjectID);
                break;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                *outSize = 2 * sizeof(UInt32);
                break;
            case kAudioDevicePropertyPreferredChannelLayout:
                *outSize = sizeof(AudioChannelLayout);
                break;
            case kAudioDevicePropertyStreams:
            {
                const int index = deviceIndex(object);
                UInt32 streams = 0;
                if (scope == kAudioObjectPropertyScopeGlobal)
                    streams = 1;
                else if (scope == kAudioObjectPropertyScopeInput && deviceHasInput(index))
                    streams = 1;
                else if (scope == kAudioObjectPropertyScopeOutput && deviceHasOutput(index))
                    streams = 1;

                *outSize = streams * sizeof(AudioObjectID);
                break;
            }
            case kAudioObjectPropertyOwnedObjects:
            {
                const int index = deviceIndex(object);
                UInt32 owned = 0;
                if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
                    owned += deviceHasInput(index) ? 1u : 0u;
                if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
                    owned += deviceHasOutput(index) ? 1u : 0u;

                *outSize = owned * sizeof(AudioObjectID);
                break;
            }
            case kAudioDevicePropertyStreamConfiguration:
            {
                // The size has to match what the data query writes: one stereo
                // buffer in the scope this device owns (and in the global scope),
                // no buffer in the direction it does not carry.
                const int index = deviceIndex(object);
                const bool outputHalf = deviceHasOutput(index);
                const bool inScope = scope == kAudioObjectPropertyScopeGlobal
                                     || (scope == kAudioObjectPropertyScopeOutput && outputHalf)
                                     || (scope == kAudioObjectPropertyScopeInput && ! outputHalf);
                *outSize = sizeof(AudioBufferList) + (inScope ? sizeof(AudioBuffer) : 0);
                break;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }

        if (address->mSelector == kAudioDevicePropertyStreams
            && ((scope == kAudioObjectPropertyScopeInput && deviceHasInput(deviceIndex(object)))
                || (scope == kAudioObjectPropertyScopeOutput && deviceHasOutput(deviceIndex(object)))))
        {
            *outSize = sizeof(AudioObjectID);
        }

        return kAudioHardwareNoError;
    }

    if (isInputStream(object) || isOutputStream(object))
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
                *outSize = sizeof(UInt32);
                break;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *outSize = sizeof(AudioStreamBasicDescription);
                break;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                *outSize = kSampleRateCount * sizeof(AudioStreamRangedDescription);
                break;
            case kAudioObjectPropertyOwnedObjects:
                *outSize = 0;
                break;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareBadObjectError;
}
//==================================================================================================
#pragma mark Property: data
//==================================================================================================

static CFStringRef cfString(const char* text)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
}

static void describeFormat(AudioStreamBasicDescription& format, Float64 rate)
{
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = sizeof(float) * 2;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * 2;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = sizeof(float) * 8;
    format.mReserved = 0;
}

static OSStatus hs_GetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                    pid_t, const AudioObjectPropertyAddress* address,
                                    UInt32 qualifierSize, const void* qualifier,
                                    UInt32 dataSize, UInt32* outSize, void* outData)
{
    if (driver != gDriverRef || address == NULL || outSize == NULL || outData == NULL)
        return kAudioHardwareIllegalOperationError;

    const AudioObjectPropertyScope scope = address->mScope;
    *outSize = 0;

    if (object == kObjectID_PlugIn)
    {
        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioPlugInClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = cfString(HS_MANUFACTURER);
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioPlugInPropertyResourceBundle:
                *(CFStringRef*)outData = CFSTR("");
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioPlugInPropertyDeviceList:
            case kAudioObjectPropertyOwnedObjects:
            {
                const UInt32 wanted = std::min<UInt32>(dataSize / sizeof(AudioObjectID), HS_DEVICE_COUNT);
                auto* ids = static_cast<AudioObjectID*>(outData);
                for (UInt32 i = 0; i < wanted; ++i)
                    ids[i] = kObjectID_Device0 + i;
                *outSize = wanted * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioPlugInPropertyBundleID:
                *(CFStringRef*)outData = cfString(HS_BUNDLE_ID);
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioPlugInPropertyTranslateUIDToDevice:
            {
                if (qualifier == NULL || qualifierSize != sizeof(CFStringRef))
                    return kAudioHardwareIllegalOperationError;

                const auto uid = static_cast<CFStringRef>(qualifier);
                AudioObjectID result = kAudioObjectUnknown;

                for (int i = 0; i < HS_DEVICE_COUNT; ++i)
                {
                    CFStringRef known = cfString(deviceUID(i).c_str());
                    const bool match = CFStringCompare(known, uid, 0) == kCFCompareEqualTo;
                    CFRelease(known);

                    if (match)
                    {
                        result = kObjectID_Device0 + i;
                        break;
                    }
                }

                *(AudioObjectID*)outData = result;
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (isDevice(object))
    {
        const int index = deviceIndex(object);
        DeviceState& state = engineState(index);

        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioDeviceClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kObjectID_PlugIn;
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = cfString(deviceName(index).c_str());
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = cfString(HS_MANUFACTURER);
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyDeviceUID:
                *(CFStringRef*)outData = cfString(deviceUID(index).c_str());
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyModelUID:
                *(CFStringRef*)outData = cfString(HS_BUNDLE_ID);
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyOwnedObjects:
            {
                AudioObjectID ids[2] = { kObjectID_StreamInputBase + index, kObjectID_StreamOutputBase + index };
                UInt32 count = 0;

                if (scope == kAudioObjectPropertyScopeGlobal)
                {
                    count = std::min<UInt32>(dataSize / sizeof(AudioObjectID), 2);
                    for (UInt32 i = 0; i < count; ++i)
                        static_cast<AudioObjectID*>(outData)[i] = ids[i];
                }
                else if (scope == kAudioObjectPropertyScopeInput && dataSize >= sizeof(AudioObjectID))
                {
                    *(AudioObjectID*)outData = ids[0];
                    count = 1;
                }
                else if (scope == kAudioObjectPropertyScopeOutput && dataSize >= sizeof(AudioObjectID))
                {
                    *(AudioObjectID*)outData = ids[1];
                    count = 1;
                }

                *outSize = count * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyTransportType:
                *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyRelatedDevices:
            {
                const UInt32 wanted = std::min<UInt32>(dataSize / sizeof(AudioObjectID), HS_DEVICE_COUNT);
                auto* ids = static_cast<AudioObjectID*>(outData);
                for (UInt32 i = 0; i < wanted; ++i)
                    ids[i] = kObjectID_Device0 + i;
                *outSize = wanted * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyClockDomain:
                // Each device runs its own mach-time timeline; declaring a shared
                // non-zero domain would tell the HAL they are hardware locked.
                *(UInt32*)outData = 0;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyDeviceIsAlive:
                *(UInt32*)outData = 1;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere:
            {
                pthread_mutex_lock(&gStateMutex);
                *(UInt32*)outData = state.ioIsRunning > 0 ? 1 : 0;
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyHogMode:
                *(pid_t*)outData = -1;
                *outSize = sizeof(pid_t);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyControlList:
                *outSize = 0;
                return kAudioHardwareNoError;

            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                // A single-direction device may only be the default for the
                // direction it actually carries.
                if (scope == kAudioObjectPropertyScopeInput)
                    *(UInt32*)outData = deviceHasInput(index) ? 1u : 0u;
                else if (scope == kAudioObjectPropertyScopeOutput)
                    *(UInt32*)outData = deviceHasOutput(index) ? 1u : 0u;
                else
                    *(UInt32*)outData = 1;

                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyLatency:
                // The output half presents immediately. The input half reads a
                // fixed kPairReadLagFrames behind the shared timeline, because
                // the writer's cycle for the period the reader is in has not
                // necessarily run yet. Reporting that honestly lets the host
                // compensate instead of guessing.
                *(UInt32*)outData = deviceHasInput(index) ? kPairReadLagFrames : 0u;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertySafetyOffset:
                // The host must present output slightly ahead of input or there
                // would be nothing to read. 64 frames = 1.3 ms at 48 kHz.
                *(UInt32*)outData = 64;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyIsHidden:
                *(UInt32*)outData = 0;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyZeroTimeStampPeriod:
                *(UInt32*)outData = kZeroPeriod;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyActualSampleRate:
                pthread_mutex_lock(&gStateMutex);
                *(Float64*)outData = state.format.mSampleRate;
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(Float64);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyAvailableNominalSampleRates:
            {
                const UInt32 wanted = std::min<UInt32>(dataSize / sizeof(AudioValueRange), kSampleRateCount);
                auto* ranges = static_cast<AudioValueRange*>(outData);
                for (UInt32 i = 0; i < wanted; ++i)
                {
                    ranges[i].mMinimum = kSampleRates[i];
                    ranges[i].mMaximum = kSampleRates[i];
                }
                *outSize = wanted * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyPreferredChannelsForStereo:
            {
                auto* channels = static_cast<UInt32*>(outData);
                channels[0] = 1;
                channels[1] = 2;
                *outSize = 2 * sizeof(UInt32);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyPreferredChannelLayout:
                memcpy(outData, &state.channelLayout, sizeof(AudioChannelLayout));
                *outSize = sizeof(AudioChannelLayout);
                return kAudioHardwareNoError;

            case kAudioDevicePropertyStreams:
            {
                const bool outputHalf = deviceHasOutput(index);
                const AudioObjectID own =
                    static_cast<AudioObjectID>((outputHalf ? kObjectID_StreamOutputBase
                                                           : kObjectID_StreamInputBase) + pairOfDevice(index));
                UInt32 count = 0;

                if ((scope == kAudioObjectPropertyScopeGlobal
                     || (scope == kAudioObjectPropertyScopeOutput && outputHalf)
                     || (scope == kAudioObjectPropertyScopeInput && ! outputHalf))
                    && dataSize >= sizeof(AudioObjectID))
                {
                    *(AudioObjectID*)outData = own;
                    count = 1;
                }

                *outSize = count * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyStreamConfiguration:
            {
                // One device, one direction, one buffer — in either scope.
                auto* list = static_cast<AudioBufferList*>(outData);
                list->mNumberBuffers = 0;

                const bool outputHalf = deviceHasOutput(index);
                const bool inScope = scope == kAudioObjectPropertyScopeGlobal
                                     || (scope == kAudioObjectPropertyScopeOutput && outputHalf)
                                     || (scope == kAudioObjectPropertyScopeInput && ! outputHalf);

                if (inScope && dataSize >= sizeof(AudioBufferList) + sizeof(AudioBuffer))
                {
                    list->mNumberBuffers = 1;
                    list->mBuffers[0].mNumberChannels = 2;
                    list->mBuffers[0].mDataByteSize = 0;
                    list->mBuffers[0].mData = NULL;
                }

                *outSize = sizeof(AudioBufferList) + (list->mNumberBuffers > 0 ? sizeof(AudioBuffer) : 0);
                return kAudioHardwareNoError;
            }

            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (isInputStream(object) || isOutputStream(object))
    {
        const bool input = isInputStream(object);
        const int pair = input ? inputStreamPair(object) : outputStreamPair(object);
        DeviceState& state = gPairs[pair];

        switch (address->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioStreamClassID;
                *outSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = deviceForDirection(pair, input);
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;

            case kAudioObjectPropertyOwnedObjects:
                *outSize = 0;
                return kAudioHardwareNoError;

            case kAudioStreamPropertyIsActive:
                pthread_mutex_lock(&gStateMutex);
                *(UInt32*)outData = input ? state.inputActive : state.outputActive;
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyDirection:
                *(UInt32*)outData = input ? 1 : 0;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyTerminalType:
                *(UInt32*)outData = input ? kAudioStreamTerminalTypeMicrophone
                                          : kAudioStreamTerminalTypeSpeaker;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyStartingChannel:
                *(UInt32*)outData = 1;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyLatency:
                *(UInt32*)outData = 0;
                *outSize = sizeof(UInt32);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                pthread_mutex_lock(&gStateMutex);
                memcpy(outData, &state.format, sizeof(AudioStreamBasicDescription));
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;

            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
            {
                const UInt32 wanted = std::min<UInt32>(dataSize / sizeof(AudioStreamRangedDescription), kSampleRateCount);
                auto* descriptions = static_cast<AudioStreamRangedDescription*>(outData);

                for (UInt32 i = 0; i < wanted; ++i)
                {
                    describeFormat(descriptions[i].mFormat, kSampleRates[i]);
                    descriptions[i].mSampleRateRange.mMinimum = kSampleRates[i];
                    descriptions[i].mSampleRateRange.mMaximum = kSampleRates[i];
                }

                *outSize = wanted * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }

            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    return kAudioHardwareBadObjectError;
}

//==================================================================================================
#pragma mark Property: setters
//==================================================================================================

static OSStatus hs_SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID object,
                                    pid_t, const AudioObjectPropertyAddress* address,
                                    UInt32, const void*, UInt32 dataSize, const void* inData)
{
    if (driver != gDriverRef || address == NULL || inData == NULL)
        return kAudioHardwareIllegalOperationError;

    if (isDevice(object) && address->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        if (dataSize != sizeof(Float64))
            return kAudioHardwareBadPropertySizeError;

        const Float64 requested = *(const Float64*)inData;
        bool supported = false;

        for (UInt32 i = 0; i < kSampleRateCount; ++i)
            supported = supported || std::abs(kSampleRates[i] - requested) < 0.5;

        if (! supported)
            return kAudioDeviceUnsupportedFormatError;

        // The HAL requires the change to go through this asynchronous path so
        // IO is stopped and restarted around it.
        auto* format = static_cast<AudioStreamBasicDescription*>(malloc(sizeof(AudioStreamBasicDescription)));

        if (format == NULL)
            return kAudioHardwareUnspecifiedError;

        memcpy(format, &engineState(deviceIndex(object)).format, sizeof(AudioStreamBasicDescription));
        format->mSampleRate = requested;

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            if (gHost != NULL)
                gHost->RequestDeviceConfigurationChange(gHost, object, kChangeRequest_SampleRate, format);
        });

        return kAudioHardwareNoError;
    }

    if (isInputStream(object) || isOutputStream(object))
    {
        const bool input = isInputStream(object);
        const int pair = input ? inputStreamPair(object) : outputStreamPair(object);

        if (address->mSelector == kAudioStreamPropertyIsActive)
        {
            if (dataSize != sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;

            pthread_mutex_lock(&gStateMutex);
            if (input)
                gPairs[pair].inputActive = *(const UInt32*)inData;
            else
                gPairs[pair].outputActive = *(const UInt32*)inData;
            pthread_mutex_unlock(&gStateMutex);

            notifyChanged(object, kAudioStreamPropertyIsActive,
                          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
            return kAudioHardwareNoError;
        }

        if (address->mSelector == kAudioStreamPropertyVirtualFormat
            || address->mSelector == kAudioStreamPropertyPhysicalFormat)
        {
            if (dataSize != sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;

            const auto* requested = static_cast<const AudioStreamBasicDescription*>(inData);

            if (requested->mFormatID != kAudioFormatLinearPCM
                || (requested->mFormatFlags & kAudioFormatFlagIsFloat) == 0
                || requested->mBitsPerChannel != 32
                || requested->mChannelsPerFrame != 2)
            {
                return kAudioDeviceUnsupportedFormatError;
            }

            bool supported = false;
            for (UInt32 i = 0; i < kSampleRateCount; ++i)
                supported = supported || std::abs(kSampleRates[i] - requested->mSampleRate) < 0.5;

            if (! supported)
                return kAudioDeviceUnsupportedFormatError;

            // Forward as a device rate change. Passing the stream address on
            // made the device branch reject it, so the HAL saw "unsupported"
            // for a format it had just been told was legal.
            const AudioObjectPropertyAddress rateAddress {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };

            const Float64 rate = requested->mSampleRate;

            return hs_SetPropertyData(driver, deviceForDirection(pair, input),
                                       0, &rateAddress, 0, nullptr,
                                       sizeof(Float64), &rate);
        }
    }

    return kAudioHardwareUnknownPropertyError;
}

//==================================================================================================
#pragma mark IO
//==================================================================================================

static OSStatus hs_StartIO(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    DeviceState& state = engineState(deviceIndex(device));

    pthread_mutex_lock(&gStateMutex);

    if (state.ioIsRunning == UINT32_MAX)
    {
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareIllegalOperationError;
    }

    if (state.ioIsRunning == 0)
    {
        if (! state.ring.allocate())
        {
            pthread_mutex_unlock(&gStateMutex);
            return kAudioHardwareUnspecifiedError;
        }

        if (state.scratch == nullptr)
            state.scratch = static_cast<float*>(calloc(static_cast<size_t>(kRingFrames) * 2, sizeof(float)));

        // The first writer may land anywhere in the timeline; start reading
        // from zero so early reads are silence rather than stale memory.
        state.ring.clear();
        state.numberTimeStamps = 0;
        state.anchorTicks = 0.0;
        state.anchorHostTime = mach_absolute_time();
        state.timelineSeed += 1;

        state.ioIsRunning = 1;
    }
    else
    {
        ++state.ioIsRunning;
    }

    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus hs_StopIO(AudioServerPlugInDriverRef driver, AudioObjectID device, UInt32)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    DeviceState& state = engineState(deviceIndex(device));

    pthread_mutex_lock(&gStateMutex);

    // Idempotent on purpose: the HAL retries a failing stop, and an error here
    // leaves the device marked as running for every later client.
    if (state.ioIsRunning == 0)
    {
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNoError;
    }

    --state.ioIsRunning;

    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus hs_GetZeroTimeStamp(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                     UInt32, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    if (driver != gDriverRef || ! isDevice(device) || outSampleTime == NULL
        || outHostTime == NULL || outSeed == NULL)
    {
        return kAudioHardwareBadObjectError;
    }

    DeviceState& state = engineState(deviceIndex(device));

    os_unfair_lock_lock(&gIOLock);

    const UInt64 now = mach_absolute_time();

    // The sample rate can arrive after the first timestamp request.
    if (state.hostTicksPerFrame <= 0.0)
        updateTicksPerFrame(state);

    const Float64 ticksPerPeriod = state.hostTicksPerFrame * static_cast<Float64>(kZeroPeriod);

    if (! (ticksPerPeriod > 0.0))
    {
        // Never return an error here: the HAL treats a failing timestamp as
        // "try again" and calls this in a tight loop.
        *outSampleTime = 0.0;
        *outHostTime = now;
        *outSeed = state.timelineSeed;

        os_unfair_lock_unlock(&gIOLock);
        return kAudioHardwareNoError;
    }

    // Jump to the period that contains "now" in one step. Advancing one period
    // per call is what turns a stale anchor (host away, device reconfigured)
    // into a spin: the HAL keeps asking until the timestamp reaches the wall
    // clock, and each answer only moves it one period.
    if (now > state.anchorHostTime)
    {
        const UInt64 periods = static_cast<UInt64>(
            static_cast<Float64>(now - state.anchorHostTime) / ticksPerPeriod);

        if (periods > state.numberTimeStamps)
            state.numberTimeStamps = periods;
    }

    state.anchorTicks = static_cast<Float64>(state.numberTimeStamps) * ticksPerPeriod;

    *outSampleTime = static_cast<Float64>(state.numberTimeStamps) * static_cast<Float64>(kZeroPeriod);
    *outHostTime = state.anchorHostTime + static_cast<UInt64>(state.anchorTicks);
    *outSeed = state.timelineSeed;

    os_unfair_lock_unlock(&gIOLock);
    return kAudioHardwareNoError;
}

static OSStatus hs_WillDoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                      UInt32, UInt32 operation, Boolean* outWillDo, Boolean* outInPlace)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    const int index = deviceIndex(device);
    bool willDo = false;

    switch (operation)
    {
        case kAudioServerPlugInIOOperationReadInput:
            willDo = deviceHasInput(index);
            break;
        case kAudioServerPlugInIOOperationWriteMix:
            willDo = deviceHasOutput(index);
            break;
        default:
            break;
    }

    if (outWillDo != NULL)
        *outWillDo = willDo;

    if (outInPlace != NULL)
        *outInPlace = true;

    return kAudioHardwareNoError;
}

static OSStatus hs_BeginIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                     UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    return kAudioHardwareNoError;
}

static OSStatus hs_DoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                  AudioObjectID stream, UInt32, UInt32 operation,
                                  UInt32 frames, const AudioServerPlugInIOCycleInfo* cycle,
                                  void* ioMainBuffer, void*)
{
    if (driver != gDriverRef || ! isDevice(device) || cycle == NULL || ioMainBuffer == NULL)
        return kAudioHardwareBadObjectError;

    const int index = deviceIndex(device);
    const int pair = pairOfDevice(index);
    DeviceState& state = gPairs[pair];

    if (frames > kRingFrames)
        return kAudioHardwareUnspecifiedError;

    if (operation == kAudioServerPlugInIOOperationWriteMix)
    {
        // Only the output half of a pair writes; the input half of the same
        // pair reads what lands here.
        if (! deviceHasOutput(index)
            || stream != static_cast<AudioObjectID>(kObjectID_StreamOutputBase + pair)
            || state.ring.storage == nullptr)
            return kAudioHardwareNoError;

        const int64_t sampleTime = static_cast<int64_t>(cycle->mOutputTime.mSampleTime);
        state.ring.write(static_cast<const float*>(ioMainBuffer), frames, sampleTime);
        return kAudioHardwareNoError;
    }

    if (operation == kAudioServerPlugInIOOperationReadInput)
    {
        if (! deviceHasInput(index)
            || stream != static_cast<AudioObjectID>(kObjectID_StreamInputBase + pair))
            return kAudioHardwareNoError;

        const int64_t sampleTime = static_cast<int64_t>(cycle->mInputTime.mSampleTime)
                                   - static_cast<int64_t>(kPairReadLagFrames);
        state.ring.read(static_cast<float*>(ioMainBuffer), frames, sampleTime);
        return kAudioHardwareNoError;
    }

    return kAudioHardwareNoError;
}

static OSStatus hs_EndIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID device,
                                   UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*)
{
    if (driver != gDriverRef || ! isDevice(device))
        return kAudioHardwareBadObjectError;

    return kAudioHardwareNoError;
}
