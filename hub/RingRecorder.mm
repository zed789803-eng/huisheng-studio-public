#include "RingRecorder.h"

#include "AudioDevices.h"
#include "Config.h"

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

struct RingRecorder::Impl
{
    hs::RingBuffer* ring = nullptr;
    AudioDeviceID device = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProc = nullptr;
    AudioStreamBasicDescription format {};
    std::atomic<bool> running { false };
    std::atomic<uint64_t> framesCaptured { 0 };
    std::vector<float> scratch;

    void handleInput(const AudioBufferList* input) noexcept
    {
        if (input == nullptr || ring == nullptr || input->mNumberBuffers == 0
            || ! running.load(std::memory_order_relaxed))
        {
            return;
        }

        const AudioBuffer& first = input->mBuffers[0];

        if (first.mData == nullptr)
            return;

        const uint32_t channels = std::max<uint32_t>(1, first.mNumberChannels);
        const uint32_t frames = first.mDataByteSize / static_cast<uint32_t>(sizeof(float) * channels);

        if (frames == 0 || static_cast<size_t>(frames) * 2 > scratch.size())
            return;

        const auto* left = static_cast<const float*>(first.mData);

        if (input->mNumberBuffers >= 2 && input->mBuffers[1].mData != nullptr)
        {
            const auto* right = static_cast<const float*>(input->mBuffers[1].mData);

            for (uint32_t i = 0; i < frames; ++i)
            {
                scratch[static_cast<size_t>(i) * 2] = left[i];
                scratch[static_cast<size_t>(i) * 2 + 1] = right[i];
            }
        }
        else if (channels == 1)
        {
            for (uint32_t i = 0; i < frames; ++i)
            {
                scratch[static_cast<size_t>(i) * 2] = left[i];
                scratch[static_cast<size_t>(i) * 2 + 1] = left[i];
            }
        }
        else
        {
            for (uint32_t i = 0; i < frames; ++i)
            {
                scratch[static_cast<size_t>(i) * 2] = left[static_cast<size_t>(i) * channels];
                scratch[static_cast<size_t>(i) * 2 + 1] = left[static_cast<size_t>(i) * channels + 1];
            }
        }

        ring->write(scratch.data(), frames);
        framesCaptured.fetch_add(frames, std::memory_order_relaxed);
    }
};

RingRecorder::RingRecorder() : impl(std::make_unique<Impl>()) {}

RingRecorder::~RingRecorder() { stop(); }

bool RingRecorder::start(const std::string& deviceUID, hs::RingBuffer& ring,
                         const std::string& sourceName, std::string& error)
{
    stop();
    error.clear();

    AudioDeviceInfo info;
    if (! findDeviceByUID(deviceUID, info) || info.inputChannels < 2)
    {
        error = "Virtual input not found: " + deviceUID;
        return false;
    }

    if (! deviceHasStereoFloat(info.deviceID, false))
    {
        error = "The virtual input does not expose stereo float32";
        return false;
    }

    uint32_t desiredRate = ring.sampleRate();
    if (desiredRate == 0)
    {
        const uint32_t host = ring.header()->hostSampleRate.load(std::memory_order_relaxed);
        desiredRate = host != 0 ? host : hs::kDefaultRate;
    }

    const auto deviceRate = static_cast<uint32_t>(std::llround(deviceSampleRate(info.deviceID)));

    if (deviceRate != desiredRate && ! setDeviceSampleRate(info.deviceID, desiredRate))
    {
        error = "Could not set the virtual input to " + std::to_string(desiredRate) + " Hz";
        return false;
    }

    if (deviceRate != desiredRate)
    {
        // The HAL applies rate changes asynchronously; the agent retries.
        error = "Waiting for the virtual input to switch to "
                + std::to_string(desiredRate) + " Hz";
        return false;
    }

    impl->device = info.deviceID;
    impl->format.mSampleRate = static_cast<Float64>(deviceRate);
    // handleInput() drops every buffer when this is null, so without this line
    // the device opens and runs while the ring stays at "written = 0": the
    // plug-in then plays silence and reports nothing is wrong.
    impl->ring = &ring;
    ring.resetForProducer(deviceRate, sourceName);
    impl->scratch.assign(16384 * 2, 0.0f);

    Impl* pointer = impl.get();
    OSStatus status = AudioDeviceCreateIOProcIDWithBlock(
        &impl->ioProc, impl->device, nullptr,
        ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
          AudioBufferList*, const AudioTimeStamp*)
        {
            pointer->handleInput(input);
        });

    if (status != noErr)
    {
        error = "AudioDeviceCreateIOProcIDWithBlock failed: " + std::to_string(status);
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

    ring.setActive(true);
    return true;
}

void RingRecorder::stop()
{
    impl->running.store(false, std::memory_order_release);

    if (impl->device != kAudioObjectUnknown && impl->ioProc != nullptr)
    {
        AudioDeviceStop(impl->device, impl->ioProc);
        AudioDeviceDestroyIOProcID(impl->device, impl->ioProc);
        impl->ioProc = nullptr;
    }

    impl->device = kAudioObjectUnknown;
    impl->ring = nullptr;
}

bool RingRecorder::isRunning() const noexcept
{
    return impl->running.load(std::memory_order_acquire);
}

double RingRecorder::sampleRate() const noexcept
{
    return impl->format.mSampleRate;
}
