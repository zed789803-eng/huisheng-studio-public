#pragma once

// Runtime OS facts the audio code needs. The plug-ins are plain C++ with no
// Foundation, so this reads kern.osproductversion through sysctl instead of
// asking NSProcessInfo.

#include <cstdio>
#include <string>
#include <sys/sysctl.h>

namespace hs {

struct SystemVersion
{
    int major = 0;
    int minor = 0;
    bool known = false;
};

inline const SystemVersion& systemVersion()
{
    static const SystemVersion version = []
    {
        char text[64] = {};
        size_t size = sizeof(text);

        if (::sysctlbyname("kern.osproductversion", text, &size, nullptr, 0) != 0)
            return SystemVersion {};

        SystemVersion parsed;
        parsed.known = std::sscanf(text, "%d.%d", &parsed.major, &parsed.minor) == 2;
        return parsed;
    }();

    return version;
}

inline bool systemAtLeast(int major, int minor)
{
    const auto& version = systemVersion();

    return version.major > major || (version.major == major && version.minor >= minor);
}

// Process taps - the only public way to capture a single App - arrived in
// macOS 14.2. Older systems still get 虚拟通道 and the Return path, so the
// product stays usable there; only "应用" capture is missing.
inline bool systemSupportsAppCapture()
{
    // Unknown means sysctl did not answer (a sandboxed host, say). Fail open:
    // the tap itself reports a precise error, and wrongly hiding the feature on
    // a machine that does support it is the worse failure.
    if (! systemVersion().known)
        return true;

    return systemAtLeast(14, 2);
}

// "12.7" - enough to tell the user what they are running.
inline std::string systemVersionText()
{
    const auto& version = systemVersion();

    return std::to_string(version.major) + "." + std::to_string(version.minor);
}

} // namespace hs
