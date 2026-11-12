#include "AudioDevices.h"

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

std::string toString(CFStringRef value)
{
    if (value == nullptr)
        return {};

    char buffer[1024] = {};
    if (! CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8))
        return {};

    return buffer;
}

std::string readString(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);

    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr)
        return {};

    std::string result = toString(value);

    if (value != nullptr)
        CFRelease(value);

    return result;
}

uint32_t channelCount(AudioDeviceID device, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    std::vector<uint8_t> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());

    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr)
        return 0;

    uint32_t total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        total += list->mBuffers[i].mNumberChannels;

    return total;
}

std::vector<AudioObjectID> allDevices()
{
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr
        || size == 0)
    {
        return {};
    }

    std::vector<AudioObjectID> devices(size / sizeof(AudioDeviceID));

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr)
    {
        return {};
    }

    return devices;
}

} // namespace

std::vector<AudioDeviceInfo> listDevices()
{
    std::vector<AudioDeviceInfo> result;

    for (AudioObjectID device : allDevices())
    {
        AudioDeviceInfo info;
        info.deviceID = static_cast<uint32_t>(device);
        info.uid = readString(device, kAudioDevicePropertyDeviceUID);
        info.name = readString(device, kAudioObjectPropertyName);
        info.inputChannels = channelCount(device, kAudioObjectPropertyScopeInput);
        info.outputChannels = channelCount(device, kAudioObjectPropertyScopeOutput);

        if (info.name.empty())
            info.name = info.uid.empty() ? ("Device " + std::to_string(device)) : info.uid;

        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b)
    {
        return a.name < b.name;
    });

    return result;
}

std::vector<AudioDeviceInfo> listOutputDevices()
{
    std::vector<AudioDeviceInfo> result;

    for (auto& device : listDevices())
        if (device.outputChannels > 0)
            result.push_back(std::move(device));

    return result;
}

std::vector<AudioDeviceInfo> listInputDevices()
{
    std::vector<AudioDeviceInfo> result;

    for (auto& device : listDevices())
        if (device.inputChannels > 0)
            result.push_back(std::move(device));

    return result;
}

bool findDeviceByUID(const std::string& uid, AudioDeviceInfo& out)
{
    for (auto& device : listDevices())
    {
        if (device.uid == uid)
        {
            out = std::move(device);
            return true;
        }
    }

    return false;
}

double deviceSampleRate(uint32_t deviceID)
{
    AudioObjectPropertyAddress address {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);

    if (AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, &rate) != noErr)
        return 0.0;

    return rate;
}

bool setDeviceSampleRate(uint32_t deviceID, double rate)
{
    if (deviceID == 0 || rate <= 0.0)
        return false;

    const double current = deviceSampleRate(deviceID);
    if (current > 0.0 && std::abs(current - rate) < 0.5)
        return true;

    AudioObjectPropertyAddress address {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    Float64 value = rate;
    return AudioObjectSetPropertyData(deviceID, &address, 0, nullptr, sizeof(value), &value) == noErr;
}

bool deviceHasStereoFloat(uint32_t deviceID, bool output)
{
    const AudioObjectPropertyScope scope = output ? kAudioObjectPropertyScopeOutput
                                                  : kAudioObjectPropertyScopeInput;

    AudioObjectPropertyAddress address {
        kAudioDevicePropertyStreams,
        scope,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(deviceID, &address, 0, nullptr, &size) != noErr
        || size < sizeof(AudioStreamID))
    {
        return false;
    }

    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));

    if (AudioObjectGetPropertyData(deviceID, &address, 0, nullptr, &size, streams.data()) != noErr)
        return false;

    AudioObjectPropertyAddress formatAddress {
        kAudioStreamPropertyVirtualFormat,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioStreamBasicDescription format {};
    UInt32 formatSize = sizeof(format);

    if (AudioObjectGetPropertyData(streams.front(), &formatAddress, 0, nullptr, &formatSize,
                                   &format) != noErr)
    {
        return false;
    }

    return format.mFormatID == kAudioFormatLinearPCM
           && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0
           && format.mBitsPerChannel == 32
           && format.mChannelsPerFrame >= 2;
}
