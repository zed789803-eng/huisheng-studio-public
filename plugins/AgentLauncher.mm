#include "AgentLauncher.h"

#include "Config.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace hs {

bool agentRunning()
{
    NSArray<NSRunningApplication*>* apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@(kAgentBundleID)];

    return apps.count > 0;
}

bool launchAgentIfNeeded()
{
    if (agentRunning())
        return true;

    NSFileManager* manager = [NSFileManager defaultManager];

    NSArray<NSString*>* candidates = @[
        @"/Applications/HuishengStudio Agent.app",
        [@"~/Applications/HuishengStudio Agent.app" stringByExpandingTildeInPath],
        @"/Library/Application Support/huishengstudio/HuishengStudio Agent.app"
    ];

    for (NSString* path in candidates)
    {
        if (! [manager fileExistsAtPath:path])
            continue;

        NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
        configuration.activates = NO;
        configuration.addsToRecentItems = NO;
        configuration.createsNewApplicationInstance = NO;

        [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:path]
                                             configuration:configuration
                                         completionHandler:nil];
        return true;
    }

    return false;
}

bool restartAgent()
{
    NSArray<NSRunningApplication*>* apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@(kAgentBundleID)];

    if (apps.count == 0)
        return false;

    for (NSRunningApplication* app in apps)
        [app terminate];

    // Give the service a moment to exit on its own before forcing it: it holds
    // an audio device open, and a forced kill while a host is starting playback
    // is the one case that can click.
    for (int i = 0; i < 20; ++i)
    {
        [NSThread sleepForTimeInterval:0.05];

        NSArray<NSRunningApplication*>* still =
            [NSRunningApplication runningApplicationsWithBundleIdentifier:@(kAgentBundleID)];

        if (still.count == 0)
            return true;
    }

    NSArray<NSRunningApplication*>* stubborn =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@(kAgentBundleID)];

    for (NSRunningApplication* app in stubborn)
        [app forceTerminate];

    return true;
}

} // namespace hs
