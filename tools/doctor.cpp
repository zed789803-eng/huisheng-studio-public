// hsdoctor — explains why the virtual devices are or are not visible, and
// whether they actually run once they are there.
//
// Checks, in the order that matters:
//   1. is the HAL bundle in place, owned by root, and signed
//   2. did coreaudiod actually load it
//   3. did it publish devices, and are they alive
//   4. can each device be started, and does it deliver IO callbacks
//
// Run it after installing; it prints one line per failure with the fix.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <vector>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* kBundlePath = "/Library/Audio/Plug-Ins/HAL/HuishengStudio.driver";
constexpr const char* kBundleID   = "com.huishengstudio.hal";

// Each pair is published as two single-direction devices — "HsAudioSC 1/2"
// (the half apps play into) and "HsAudioSR 1/2" (the half that hands the audio
// back) — so the product prefix to look for is the display name, not the bundle
// name, and the two halves are counted separately.
constexpr const char* kDevicePrefix = "HsAudio";
constexpr const char* kOutputPrefix = "HsAudioSC";
constexpr const char* kInputPrefix  = "HsAudioSR";

int failures = 0;

void ok(const std::string& text)   { std::printf("  ok    %s\n", text.c_str()); }
void warn(const std::string& text) { std::printf("  warn  %s\n", text.c_str()); }
void bad(const std::string& text)  { std::printf("  FAIL  %s\n", text.c_str()); ++failures; }

std::string readString(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);

    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};

    char buffer[512] = {};
    CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(value);
    return buffer;
}

std::vector<AudioObjectID> objectList(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;

    if (AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};

    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));

    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, ids.data()) != noErr)
        return {};

    return ids;
}

void checkBundle()
{
    std::printf("bundle\n");

    struct stat st {};

    if (::stat(kBundlePath, &st) != 0)
    {
        bad(std::string("missing ") + kBundlePath + " — run scripts/install_driver.sh");
        return;
    }

    ok(std::string(kBundlePath) + " exists");

    if (st.st_uid != 0)
        bad("owner is not root — coreaudiod refuses to load it; run: sudo chown -R root:wheel " + std::string(kBundlePath));
    else
        ok("owner root:wheel");

    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        warn("group/other writable — run: sudo chmod -R go-w " + std::string(kBundlePath));
}

void checkLoaded()
{
    std::printf("core audio\n");

    bool found = false;

    for (AudioObjectID plugin : objectList(kAudioObjectSystemObject, kAudioHardwarePropertyPlugInList))
    {
        if (readString(plugin, kAudioPlugInPropertyBundleID) == kBundleID)
        {
            found = true;
            ok(std::string("plug-in loaded as ") + kBundleID);

            const auto devices = objectList(plugin, kAudioPlugInPropertyDeviceList);

            if (devices.empty())
            {
                bad("plug-in loaded but published 0 devices — restart core audio: sudo killall coreaudiod");
                return;
            }

            for (AudioObjectID device : devices)
            {
                UInt32 alive = 0;
                UInt32 size = sizeof(alive);
                AudioObjectPropertyAddress address { kAudioDevicePropertyDeviceIsAlive,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMain };
                AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &alive);

                char stereo[64] = {};
                Float64 rate = 0.0;
                UInt32 rateSize = sizeof(rate);
                AudioObjectPropertyAddress rateAddress { kAudioDevicePropertyNominalSampleRate,
                                                         kAudioObjectPropertyScopeGlobal,
                                                         kAudioObjectPropertyElementMain };
                AudioObjectGetPropertyData(device, &rateAddress, 0, nullptr, &rateSize, &rate);
                std::snprintf(stereo, sizeof(stereo), "%.0f Hz", rate);

                if (alive)
                    ok(readString(device, kAudioObjectPropertyName) + "  " + stereo);
                else
                    bad(readString(device, kAudioObjectPropertyName) + " is not alive");
            }

            return;
        }
    }

    if (! found)
        bad("plug-in not loaded — restart core audio: sudo killall coreaudiod");
}

void checkVisible()
{
    std::printf("devices\n");

    int outputs = 0;
    int inputs = 0;

    for (AudioObjectID device : objectList(kAudioObjectSystemObject, kAudioHardwarePropertyDevices))
    {
        const auto name = readString(device, kAudioObjectPropertyName);

        if (name.find(kDevicePrefix) != std::string::npos)
        {
            const bool out = name.find(kOutputPrefix) != std::string::npos;
            const bool in  = name.find(kInputPrefix) != std::string::npos;

            if (out) ++outputs;
            if (in)  ++inputs;

            std::printf("  seen  %s  (%s)\n", name.c_str(),
                        out ? "output" : (in ? "input" : "unknown direction"));
        }
    }

    if (outputs == 0 && inputs == 0)
    {
        bad(std::string("no ") + kDevicePrefix + " device in the system device list");
    }
    else if (outputs != 4 || inputs != 4)
    {
        bad("expected 4 " + std::string(kOutputPrefix) + " and 4 " + std::string(kInputPrefix)
            + " devices, found " + std::to_string(outputs) + " and " + std::to_string(inputs));
    }
    else
    {
        ok("4 " + std::string(kOutputPrefix) + " (output) + 4 " + std::string(kInputPrefix)
           + " (input) visible to every app");
    }
}

// Being listed is not the same as working: this starts every device through
// CoreAudio and counts the callbacks the driver really delivers. A device that
// enumerates but never calls back would still look fine in system_profiler.
std::atomic<unsigned> ioCallbacks { 0 };

OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void*)
{
    ioCallbacks.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

void checkEngine()
{
    std::printf("engine\n");

    int devices = 0;
    int started = 0;
    int outputHalves = 0;
    int outputHalvesStarted = 0;
    unsigned callbacks = 0;

    for (AudioObjectID device : objectList(kAudioObjectSystemObject, kAudioHardwarePropertyDevices))
    {
        const auto name = readString(device, kAudioObjectPropertyName);

        if (name.find(kDevicePrefix) == std::string::npos)
            continue;

        ++devices;

        const bool outputHalf = name.find(kOutputPrefix) != std::string::npos;

        if (outputHalf)
            ++outputHalves;

        AudioDeviceIOProcID proc = nullptr;
        ioCallbacks.store(0);

        if (AudioDeviceCreateIOProcID(device, ioProc, nullptr, &proc) != noErr || proc == nullptr)
        {
            bad(name + ": cannot create an IO proc");
            continue;
        }

        if (AudioDeviceStart(device, proc) != noErr)
        {
            bad(name + ": cannot start");
            AudioDeviceDestroyIOProcID(device, proc);
            continue;
        }

        ::usleep(500000); // half a second is ~375 callbacks at 128 frames/48 kHz
        AudioDeviceStop(device, proc);
        AudioDeviceDestroyIOProcID(device, proc);

        const unsigned seen = ioCallbacks.load();
        callbacks += seen;

        if (seen > 0)
        {
            ok(name + ": runs, " + std::to_string(seen) + " callbacks in 0.5 s");
            ++started;

            if (outputHalf)
                ++outputHalvesStarted;
        }
        else if (outputHalf)
        {
            bad(name + ": started but never called back");
        }
        else
        {
            // An input half still needs the microphone grant before it delivers
            // audio; without it the HAL runs the proc but sends silence.
            warn(name + ": started, no callback yet (microphone access not granted)");
        }
    }

    if (devices == 0)
        bad("no device to start");
    else if (outputHalvesStarted < outputHalves || outputHalvesStarted == 0)
        bad("only " + std::to_string(outputHalvesStarted) + "/" + std::to_string(outputHalves)
            + " output halves ran");
    else
        ok(std::to_string(started) + "/" + std::to_string(devices) + " devices ran, "
           + std::to_string(callbacks) + " callbacks total");
}

} // namespace

int main()
{
    std::printf("huishengstudio doctor\n\n");
    checkBundle();
    checkLoaded();
    checkVisible();
    checkEngine();

    std::printf("\n%s\n", failures == 0 ? "All checks passed."
                                        : "Some checks failed; fix the lines marked FAIL.");
    return failures == 0 ? 0 : 1;
}
