#include "ProcessList.h"

#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <libproc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

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

std::string toString(NSString* value)
{
    if (value == nil || value.UTF8String == nullptr)
        return {};

    return std::string(value.UTF8String);
}

std::string readBundleID(uint32_t object)
{
    AudioObjectPropertyAddress address {
        kAudioProcessPropertyBundleID,
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

std::vector<uint8_t> iconForProcess(pid_t pid)
{
    NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];

    if (application == nil || application.icon == nil)
        return {};

    const NSInteger size = 32;
    NSImage* resized = [[NSImage alloc] initWithSize:NSMakeSize(size, size)];
    [resized lockFocus];
    [application.icon drawInRect:NSMakeRect(0, 0, size, size)
                        fromRect:NSZeroRect
                       operation:NSCompositingOperationCopy
                        fraction:1.0];
    [resized unlockFocus];

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithData:[resized TIFFRepresentation]];

    if (rep == nil)
        return {};

    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];

    if (png == nil || png.length == 0)
        return {};

    std::vector<uint8_t> bytes(static_cast<size_t>(png.length));
    std::memcpy(bytes.data(), png.bytes, bytes.size());
    return bytes;
}

bool readUInt32(AudioObjectID object, AudioObjectPropertySelector selector, UInt32& value)
{
    AudioObjectPropertyAddress address {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr;
}

std::vector<AudioObjectID> processObjects()
{
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyProcessObjectList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr
        || size == 0)
    {
        return {};
    }

    std::vector<AudioObjectID> objects(size / sizeof(AudioObjectID));

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   objects.data()) != noErr)
    {
        return {};
    }

    return objects;
}

} // namespace

std::vector<AudioProcessInfo> listAudioProcesses(bool includeIcons)
{
    std::vector<AudioProcessInfo> result;
    const pid_t ownPID = getpid();
    const pid_t agentPID = []() -> pid_t {
        NSArray<NSRunningApplication*>* apps =
            [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.huishengstudio.agent"];

        for (NSRunningApplication* app in apps)
            return app.processIdentifier;

        return -1;
    }();

    @autoreleasepool
    {
        for (AudioObjectID object : processObjects())
        {
            AudioObjectPropertyAddress pidAddress {
                kAudioProcessPropertyPID,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };

            pid_t pid = -1;
            UInt32 pidSize = sizeof(pid);

            if (AudioObjectGetPropertyData(object, &pidAddress, 0, nullptr, &pidSize, &pid) != noErr)
                continue;

            if (pid <= 0 || pid == ownPID || pid == agentPID)
                continue;

            AudioProcessInfo info;
            info.objectID = static_cast<uint32_t>(object);
            info.pid = pid;
            info.bundleID = readBundleID(object);

            UInt32 running = 0;
            if (readUInt32(object, kAudioProcessPropertyIsRunning, running))
                info.audioRunning = running != 0;

            bool regular = false;
            NSRunningApplication* application =
                [NSRunningApplication runningApplicationWithProcessIdentifier:pid];

            if (application != nil)
            {
                info.name = toString(application.localizedName);

                if (includeIcons)
                    info.iconPNG = iconForProcess(pid);

                regular = application.activationPolicy == NSApplicationActivationPolicyRegular;
            }

            // Regular apps always belong in the picker; helpers only while they
            // are producing audio, otherwise the list fills with noise.
            if (! regular && ! info.audioRunning)
                continue;

            if (info.name.empty())
            {
                char processName[512] = {};
                if (proc_name(pid, processName, sizeof(processName)) > 0)
                    info.name = processName;
            }

            if (info.name.empty())
                info.name = info.bundleID.empty() ? ("PID " + std::to_string(pid)) : info.bundleID;

            result.push_back(std::move(info));
        }
    }

    std::sort(result.begin(), result.end(), [](const AudioProcessInfo& a, const AudioProcessInfo& b)
    {
        if (a.audioRunning != b.audioRunning)
            return a.audioRunning;

        return a.name < b.name;
    });

    return result;
}

uint32_t processObjectForPID(int32_t pid)
{
    if (pid <= 0)
        return 0;

    for (AudioObjectID object : processObjects())
    {
        AudioObjectPropertyAddress pidAddress {
            kAudioProcessPropertyPID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        pid_t objectPID = -1;
        UInt32 size = sizeof(objectPID);

        if (AudioObjectGetPropertyData(object, &pidAddress, 0, nullptr, &size, &objectPID) == noErr
            && objectPID == pid)
        {
            return static_cast<uint32_t>(object);
        }
    }

    return 0;
}

uint32_t stereoPairsForProcess(uint32_t objectID)
{
    AudioObjectPropertyAddress devicesAddress {
        kAudioProcessPropertyDevices,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize(objectID, &devicesAddress, 0, nullptr, &size) != noErr
        || size < sizeof(AudioDeviceID))
    {
        return 0;
    }

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));

    if (AudioObjectGetPropertyData(objectID, &devicesAddress, 0, nullptr, &size,
                                   devices.data()) != noErr)
    {
        return 0;
    }

    uint32_t pairs = 0;

    for (AudioDeviceID device : devices)
    {
        AudioObjectPropertyAddress configAddress {
            kAudioDevicePropertyStreamConfiguration,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };

        UInt32 configSize = 0;
        if (AudioObjectGetPropertyDataSize(device, &configAddress, 0, nullptr, &configSize) != noErr
            || configSize == 0)
        {
            continue;
        }

        std::vector<uint8_t> storage(configSize);
        auto* list = reinterpret_cast<AudioBufferList*>(storage.data());

        if (AudioObjectGetPropertyData(device, &configAddress, 0, nullptr, &configSize, list) != noErr)
            continue;

        uint32_t channels = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            channels += list->mBuffers[i].mNumberChannels;

        pairs = std::max(pairs, channels / 2);
    }

    return pairs;
}
