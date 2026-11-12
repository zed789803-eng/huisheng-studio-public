#include "ProcessTap.h"

#include "ProcessList.h"
#include "Resampler.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

std::string describe(OSStatus status)
{
    const uint32_t value = static_cast<uint32_t>(status);
    const char chars[5] = {
        static_cast<char>((value >> 24) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>(value & 0xff),
        0
    };

    bool printable = true;
    for (int i = 0; i < 4; ++i)
        printable = printable && chars[i] >= 32 && chars[i] < 127;

    return printable ? std::string("'") + chars + "' (" + std::to_string(status) + ")"
                     : std::to_string(status);
}

std::string failure(const char* what, OSStatus status)
{
    return std::string(what) + " failed: " + describe(status);
}

NSString* key(const char* text)
{
    return [NSString stringWithUTF8String:text];
}

} // namespace

struct ProcessTap::Impl
{
    hs::RingBuffer* ring = nullptr;
    AudioObjectID tapID = kAudioObjectUnknown;
    AudioObjectID aggregateID = kAudioObjectUnknown;
    AudioObjectID processID = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProc = nullptr;
    AudioStreamBasicDescription format {};
    std::atomic<bool> running { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<uint64_t> framesCaptured { 0 };
    std::vector<float> scratch;
    std::vector<float> pulled;

    void updateGain(uint32_t pairs)
    {
        // A stereo-mixdown tap is scaled by 1/pairs when the App plays into a
        // device with several stereo pairs (an RME with 20 outputs loses
        // 20*log10(10) = 20 dB). Restore the original level; float32 end to end
        // means this stays exact.
        const float value = pairs > 1 ? static_cast<float>(pairs) : 1.0f;
        const float previous = gain.exchange(value, std::memory_order_relaxed);

        if (std::abs(previous - value) > 1e-4f)
        {
            NSLog(@"huishengstudio: tap headroom pairs=%u gain=+%.1f dB", pairs,
                  20.0f * std::log10(value));
        }
    }

    const AudioBuffer* pickTapBuffer(const AudioBufferList* input, uint32_t& frames) const
    {
        if (input == nullptr || input->mNumberBuffers == 0)
            return nullptr;

        const AudioBuffer* mono1 = nullptr;
        const AudioBuffer* mono2 = nullptr;

        for (UInt32 i = 0; i < input->mNumberBuffers; ++i)
        {
            const AudioBuffer& buffer = input->mBuffers[i];

            if (buffer.mData == nullptr || buffer.mDataByteSize == 0 || buffer.mNumberChannels == 0)
                continue;

            const uint32_t bufferFrames =
                buffer.mDataByteSize / (sizeof(float) * buffer.mNumberChannels);

            if (bufferFrames == 0)
                continue;

            if (buffer.mNumberChannels == 2)
            {
                frames = bufferFrames;
                return &buffer;
            }

            if (buffer.mNumberChannels == 1)
            {
                if (mono1 == nullptr)
                {
                    mono1 = &buffer;
                    frames = bufferFrames;
                }
                else if (mono2 == nullptr)
                {
                    mono2 = &buffer;
                }
            }
        }

        // A non-interleaved stereo tap arrives as two mono buffers.
        if (mono1 != nullptr && mono2 != nullptr)
            return mono1;

        return mono1;
    }

    void handleIO(const AudioBufferList* input) noexcept
    {
        if (input == nullptr || ring == nullptr || ! running.load(std::memory_order_relaxed))
            return;

        uint32_t frames = 0;
        const AudioBuffer* buffer = pickTapBuffer(input, frames);

        if (buffer == nullptr || frames == 0)
            return;

        const size_t needed = static_cast<size_t>(frames) * 2;

        if (needed > scratch.size() || needed > pulled.size())
            return;

        const auto* samples = static_cast<const float*>(buffer->mData);
        const uint32_t channels = std::max<uint32_t>(1, buffer->mNumberChannels);

        if (channels >= 2)
        {
            for (uint32_t i = 0; i < frames; ++i)
            {
                scratch[static_cast<size_t>(i) * 2] = samples[static_cast<size_t>(i) * channels];
                scratch[static_cast<size_t>(i) * 2 + 1] = samples[static_cast<size_t>(i) * channels + 1];
            }
        }
        else
        {
            // Second mono buffer, when present, holds the right channel.
            const AudioBuffer* partner = nullptr;

            for (UInt32 i = 0; i < input->mNumberBuffers; ++i)
            {
                const AudioBuffer& candidate = input->mBuffers[i];
                if (&candidate != buffer && candidate.mNumberChannels == 1 && candidate.mData != nullptr)
                {
                    partner = &candidate;
                    break;
                }
            }

            const auto* right = partner != nullptr ? static_cast<const float*>(partner->mData) : nullptr;

            for (uint32_t i = 0; i < frames; ++i)
            {
                scratch[static_cast<size_t>(i) * 2] = samples[i];
                scratch[static_cast<size_t>(i) * 2 + 1] = right != nullptr ? right[i] : samples[i];
            }
        }

        const float compensation = gain.load(std::memory_order_relaxed);

        if (compensation != 1.0f)
            for (size_t i = 0; i < needed; ++i)
                scratch[i] *= compensation;

        ring->write(scratch.data(), frames);
        framesCaptured.fetch_add(frames, std::memory_order_relaxed);
    }
};

ProcessTap::ProcessTap() : impl(std::make_unique<Impl>()) {}

ProcessTap::~ProcessTap() { stop(); }

bool ProcessTap::start(uint32_t processObjectID, hs::RingBuffer& ring,
                       const std::string& sourceName, std::string& error)
{
    stop();
    error.clear();
    impl->ring = &ring;

    if (processObjectID == 0)
    {
        error = "Invalid process object";
        return false;
    }

    // The tap API itself only exists from 14.2. The header is weak-linked, so
    // this branch - not the build - is what keeps one binary usable on macOS 12
    // and 13, where only 虚拟通道 capture works.
    if (@available(macOS 14.2, *))
    {
    }
    else
    {
        error = "process taps need macOS 14.2 or newer";
        return false;
    }

    @autoreleasepool
    {
        CATapDescription* description =
            [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(processObjectID) ]];

        description.UUID = [NSUUID UUID];
        description.name = @"HsAudio Capture";
        description.muteBehavior = CATapMutedWhenTapped;
        description.privateTap = YES;

        OSStatus status = AudioHardwareCreateProcessTap(description, &impl->tapID);

        if (status != noErr)
        {
            error = failure("AudioHardwareCreateProcessTap", status);
            stop();
            return false;
        }

        impl->processID = processObjectID;

        const uint32_t pairs = stereoPairsForProcess(processObjectID);
        impl->updateGain(pairs > 0 ? pairs : 1);

        NSString* aggregateUID = [[NSUUID UUID] UUIDString];

        // A tap-only private aggregate is clocked by the tap, which means the
        // tapped process's own device. Nothing else can leak into the buffers
        // and no drift compensation is needed.
        NSDictionary* subTap = @{
            key(kAudioSubTapUIDKey) : description.UUID.UUIDString,
            key(kAudioSubTapDriftCompensationKey) : @NO
        };

        NSDictionary* aggregate = @{
            key(kAudioAggregateDeviceNameKey) : @"huishengstudio Tap",
            key(kAudioAggregateDeviceUIDKey) : aggregateUID,
            key(kAudioAggregateDeviceIsPrivateKey) : @YES,
            key(kAudioAggregateDeviceIsStackedKey) : @NO,
            key(kAudioAggregateDeviceTapAutoStartKey) : @YES,
            key(kAudioAggregateDeviceTapListKey) : @[ subTap ]
        };

        status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef) aggregate,
                                                    &impl->aggregateID);

        if (status != noErr)
        {
            error = failure("AudioHardwareCreateAggregateDevice", status);
            stop();
            return false;
        }

        AudioObjectPropertyAddress formatAddress {
            kAudioTapPropertyFormat,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        UInt32 formatSize = sizeof(impl->format);
        status = AudioObjectGetPropertyData(impl->tapID, &formatAddress, 0, nullptr,
                                            &formatSize, &impl->format);

        if (status != noErr)
        {
            error = failure("Reading tap format", status);
            stop();
            return false;
        }

        if (impl->format.mFormatID != kAudioFormatLinearPCM
            || (impl->format.mFormatFlags & kAudioFormatFlagIsFloat) == 0
            || impl->format.mBitsPerChannel != 32
            || impl->format.mChannelsPerFrame != 2
            || impl->format.mSampleRate <= 0.0)
        {
            error = "The tap returned an unsupported audio format";
            stop();
            return false;
        }

        impl->scratch.assign(16384 * 2, 0.0f);
        impl->pulled.assign(16384 * 2, 0.0f);
        impl->framesCaptured.store(0, std::memory_order_relaxed);

        const uint32_t tapRate = static_cast<uint32_t>(std::llround(impl->format.mSampleRate));
        ring.resetForProducer(tapRate, sourceName);

        Impl* pointer = impl.get();
        status = AudioDeviceCreateIOProcIDWithBlock(
            &impl->ioProc, impl->aggregateID, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
              AudioBufferList*, const AudioTimeStamp*)
            {
                pointer->handleIO(input);
            });

        if (status != noErr)
        {
            error = failure("AudioDeviceCreateIOProcIDWithBlock", status);
            stop();
            return false;
        }

        impl->running.store(true, std::memory_order_release);

        status = AudioDeviceStart(impl->aggregateID, impl->ioProc);

        if (status != noErr)
        {
            impl->running.store(false, std::memory_order_release);
            error = failure("AudioDeviceStart", status);
            stop();
            return false;
        }

        ring.setActive(true);
    }

    return true;
}

void ProcessTap::stop()
{
    impl->running.store(false, std::memory_order_release);

    if (impl->aggregateID != kAudioObjectUnknown)
    {
        if (impl->ioProc != nullptr)
        {
            AudioDeviceStop(impl->aggregateID, impl->ioProc);
            AudioDeviceDestroyIOProcID(impl->aggregateID, impl->ioProc);
            impl->ioProc = nullptr;
        }

        AudioHardwareDestroyAggregateDevice(impl->aggregateID);
        impl->aggregateID = kAudioObjectUnknown;
    }

    if (impl->tapID != kAudioObjectUnknown)
    {
        if (@available(macOS 14.2, *))
            AudioHardwareDestroyProcessTap(impl->tapID);

        impl->tapID = kAudioObjectUnknown;
    }

    if (impl->ring != nullptr)
        impl->ring->setActive(false);

    impl->ring = nullptr;
    impl->processID = kAudioObjectUnknown;
}

bool ProcessTap::isRunning() const noexcept
{
    return impl->running.load(std::memory_order_acquire);
}

double ProcessTap::sampleRate() const noexcept
{
    return impl->format.mSampleRate;
}

uint32_t ProcessTap::tapChannels() const noexcept
{
    return impl->format.mChannelsPerFrame;
}

void ProcessTap::refreshGain()
{
    if (impl->processID != kAudioObjectUnknown)
    {
        const uint32_t pairs = stereoPairsForProcess(impl->processID);
        if (pairs > 0)
            impl->updateGain(pairs);
    }
}

float ProcessTap::gain() const noexcept
{
    return impl->gain.load(std::memory_order_relaxed);
}
