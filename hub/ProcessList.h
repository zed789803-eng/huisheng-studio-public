#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AudioProcessInfo
{
    uint32_t objectID = 0;
    int32_t pid = -1;
    std::string bundleID;
    std::string name;
    bool audioRunning = false;
    std::vector<uint8_t> iconPNG;
};

// Core Audio process objects that can be tapped right now. Background helpers
// are only listed while they are actually doing IO.
std::vector<AudioProcessInfo> listAudioProcesses(bool includeIcons);

// Current Core Audio process object for a pid. Object IDs are not stable across
// audio sessions, so always resolve before creating a tap.
uint32_t processObjectForPID(int32_t pid);

// Number of stereo pairs on the process's output devices. macOS attenuates a
// stereo-mixdown tap by exactly this factor; the reciprocal compensates for it.
uint32_t stereoPairsForProcess(uint32_t objectID);
