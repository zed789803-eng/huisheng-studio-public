#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AudioDeviceInfo
{
    uint32_t deviceID = 0;
    std::string uid;
    std::string name;
    uint32_t inputChannels = 0;
    uint32_t outputChannels = 0;
};

std::vector<AudioDeviceInfo> listDevices();
std::vector<AudioDeviceInfo> listOutputDevices();
std::vector<AudioDeviceInfo> listInputDevices();

bool findDeviceByUID(const std::string& uid, AudioDeviceInfo& out);

double deviceSampleRate(uint32_t deviceID);
bool setDeviceSampleRate(uint32_t deviceID, double rate);

// True when the device exposes at least two float32 channels in that scope and
// its current stream format is interleaved stereo float.
bool deviceHasStereoFloat(uint32_t deviceID, bool output);
