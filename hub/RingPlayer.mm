#include "RingPlayer.h"

#include "AudioDevices.h"
#include "Config.h"
#include "DriftBridge.h"

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

struct RingPlayer::Impl
{
    hs::RingBuffer* ring = nullptr;
    hs::DriftBridge bridge;
    AudioDeviceID device = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProc = nullptr;
    AudioStreamBasicDescription format {};
    std::string uid;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> framesPlayed { 0 };
    std::vector<float> scratch;

    void handleOutput(AudioBufferList* output) noexcept
    {
        if (output == nullptr || output->mNumberBuffers == 0
            || ! running.load(std::memory_order_relaxed))
        {
            return;
        }

        const bool nonInterleaved = (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        const uint32_t channels = std::max<uint32_t>(1, format.mChannelsPerFrame);

        uint32_t frames = 0;

        if (nonInterleaved)
            frames = output->mBuffers[0].mDataByteSize / static_cast<uint32_t>(sizeof(float));
        else
            frames = output->mBuffers[0].mDataByteSize
                     / static_cast<uint32_t>(sizeof(float) * channels);

        if (frames == 0)
            return;

        for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
            if (output->mBuffers[i].mData != nullptr)
                std::memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);

        if (static_cast<size_t>(frames) * 2 > scratch.size())
            return;

        // Pull from the ring into the bridge first so the standing buffer is
        // maintained independently of the IO block size.
        const uint32_t available = ring->available();
        const uint32_t needed = frames + 32;

        if (available > 0)
        {
            const uint32_t take = std::min(available, needed);
            ring->read(scratch.data(), take);
            bridge.push(scratch.data(), take);
        }

        if (static_cast<size_t>(frames) * 2 > scratch.size())
            return;

        bridge.pull(scratch.data(), frames);

        if (nonInterleaved)
        {
            auto* left = static_cast<float*>(output->mBuffers[0].mData);
            float* right = output->mNumberBuffers > 1
                               ? static_cast<float*>(output->mBuffers[1].mData)
                               : nullptr;

            for (uint32_t i = 0; i < frames; ++i)
            {
                left[i] = scratch[static_cast<size_t>(i) * 2];
                if (right != nullptr)
                    right[i] = scratch[static_cast<size_t>(i) * 2 + 1];
            }
        }
        else
        {
            auto* destination = static_cast<float*>(output->mBuffers[0].mData);

            if (channels == 2)
            {
                std::memcpy(destination, scratch.data(),
                            static_cast<size_t>(frames) * 2 * sizeof(float));
            }
            else
            {
                for (uint32_t i = 0; i < frames; ++i)
                {
                    destination[static_cast<size_t>(i) * channels] = scratch[static_cast<size_t>(i) * 2];
                    destination[static_cast<size_t>(i) * channels + 1] = scratch[static_cast<size_t>(i) * 2 + 1];
                }
            }
        }

        framesPlayed.fetch_add(frames, std::memory_order_relaxed);
    }
};

RingPlayer::RingPlayer() : impl(std::make_unique<Impl>()) {}

RingPlayer::~RingPlayer() { stop(); }

bool RingPlayer::start(const std::string& deviceUID, hs::RingBuffer& ring, std::string& error)
{
    stop();
    error.clear();

    AudioDeviceInfo info;
    if (! findDeviceByUID(deviceUID, info) || info.outputChannels < 2)
    {
        error = "Virtual output not found: " + deviceUID;
        return false;
    }

    if (! deviceHasStereoFloat(info.deviceID, true))
    {
        error = "The virtual output does not expose stereo float32";
        return false;
    }

    const uint32_t desiredRate = ring.sampleRate();

    if (desiredRate != 0)
    {
        const auto current = static_cast<uint32_t>(std::llround(deviceSampleRate(info.deviceID)));

        if (current != desiredRate && ! setDeviceSampleRate(info.deviceID, desiredRate))
        {
            error = "Could not set the virtual output to " + std::to_string(desiredRate) + " Hz";
            return false;
        }
    }

    impl->ring = &ring;
    impl->uid = deviceUID;
    impl->device = info.deviceID;

    AudioObjectPropertyAddress formatAddress {
        kAudioDevicePropertyStreamFormat,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 formatSize = sizeof(impl->format);

    if (AudioObjectGetPropertyData(impl->device, &formatAddress, 0, nullptr,
                                   &formatSize, &impl->format) != noErr)
    {
        error = "Could not read the virtual output format";
        return false;
    }

    const uint32_t deviceRate = static_cast<uint32_t>(std::llround(impl->format.mSampleRate));
    impl->bridge.reset(deviceRate);
    impl->bridge.setInputRate(desiredRate != 0 ? desiredRate : deviceRate);
    impl->bridge.setTarget(hs::kTargetFrames);

    impl->scratch.assign(16384 * 2, 0.0f);
    ring.setActive(true);

    Impl* pointer = impl.get();
    OSStatus status = AudioDeviceCreateIOProcIDWithBlock(
        &impl->ioProc, impl->device, nullptr,
        ^(const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
          AudioBufferList* output, const AudioTimeStamp*)
        {
            pointer->handleOutput(output);
        });

    if (status != noErr)
    {
        error = "AudioDeviceCreateIOProcIDWithBlock failed: " + std::to_string(status);
        stop();
        return false;
    }

    impl->running.store(true, std::memory_order_release);
    status = AudioDeviceStart(impl->device, impl->ioProc);

    if (status != noErr)
    {
        impl->running.store(false, std::memory_order_release);
        error = "AudioDeviceStart failed: " + std::to_string(status);
        stop();
        return false;
    }

    return true;
}

void RingPlayer::stop()
{
    impl->running.store(false, std::memory_order_release);

    if (impl->device != kAudioObjectUnknown && impl->ioProc != nullptr)
    {
        AudioDeviceStop(impl->device, impl->ioProc);
        AudioDeviceDestroyIOProcID(impl->device, impl->ioProc);
        impl->ioProc = nullptr;
    }

    impl->device = kAudioObjectUnknown;
    impl->uid.clear();
    impl->ring = nullptr;
}

bool RingPlayer::isRunning() const noexcept
{
    return impl->running.load(std::memory_order_acquire);
}

double RingPlayer::sampleRate() const noexcept
{
    return impl->format.mSampleRate;
}

std::string RingPlayer::deviceUID() const noexcept
{
    return impl->uid;
}

void RingPlayer::setTarget(uint32_t frames)
{
    impl->bridge.setTarget(frames);
}
