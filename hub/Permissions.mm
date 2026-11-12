#include "Permissions.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>
#include <utility>

namespace {

using PreflightFunction = int (*)(CFStringRef, CFDictionaryRef);
using RequestFunction = void (*)(CFStringRef, CFDictionaryRef, void (^)(bool));

struct TCCAPI
{
    PreflightFunction preflight = nullptr;
    RequestFunction request = nullptr;

    TCCAPI()
    {
        // The TCC API is private but stable; using it is the only way to get a
        // single prompt for "System Audio Recording" without a screen-capture
        // call, which would light up the purple indicator.
        void* handle = dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC",
                              RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
            handle = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC",
                            RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
            return;

        preflight = reinterpret_cast<PreflightFunction>(dlsym(handle, "TCCAccessPreflight"));
        request = reinterpret_cast<RequestFunction>(dlsym(handle, "TCCAccessRequest"));
    }
};

TCCAPI& tcc()
{
    static TCCAPI api;
    return api;
}

CFStringRef audioCaptureService() { return CFSTR("kTCCServiceAudioCapture"); }

void activateForPrompt()
{
    // An LSUIElement agent has no key window, so the sheet can be assigned to
    // the wrong application. Briefly become active while the prompt is shown.
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
}

} // namespace

int audioCapturePermissionRaw()
{
    if (tcc().preflight == nullptr)
        return -1;

    // TCCAccessPreflight answers 0 when the service is allowed, 1 when it is
    // denied and 2 when the user has not been asked. Everything else in this
    // project uses -1 / 0 / 1 / 2 (unknown / denied / undecided / granted), so
    // translate here instead of at every call site. Reading 0 as "denied" made
    // the plug-in show 需要系统权限 while the tap was running fine.
    switch (tcc().preflight(audioCaptureService(), nullptr))
    {
        case 0: return 2;
        case 1: return 0;
        case 2: return 1;
        default: return -1;
    }
}

int microphonePermissionRaw()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized: return 2;
        case AVAuthorizationStatusNotDetermined: return 1;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted: return 0;
        default: return -1;
    }
}

void requestAudioCapturePermission(std::function<void(bool)> callback)
{
    activateForPrompt();

    if (tcc().request == nullptr)
    {
        if (callback)
            callback(false);
        return;
    }

    auto completion = std::move(callback);

    // TCC builds the prompt text from NSAudioCaptureUsageDescription in the
    // bundle. That string used to be written empty by cmake/postprocess_agent.sh
    // (PlistBuddy without a value), in which case no prompt is ever shown and
    // the request just completes as denied.
    tcc().request(audioCaptureService(), nullptr, ^(bool granted)
    {
        if (completion)
            completion(granted);
    });
}

void requestMicrophonePermission(std::function<void(bool)> callback)
{
    activateForPrompt();

    auto completion = std::move(callback);

    // Microphone is a public AVFoundation permission: this is the supported
    // path, it reads NSMicrophoneUsageDescription, and it shows the standard
    // prompt exactly once.
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted)
    {
        if (completion)
            completion(granted == YES);
    }];
}
