#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include "Config.h"
#include "Core.h"
#include "PermissionGuide.h"
#include "RingBuffer.h"

#include <cstring>
#include <memory>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

constexpr double kTickInterval = 0.1;

// Never raise the guide more than once a minute on our own: it is a helper, not
// a nag, and the user may deliberately be working without the grant.
constexpr uint64_t kGuideRetryInterval = 60'000'000'000ull;

// The installer, a plug-in and a login item can all ask for the agent within the
// same second. Two workers would fight over the same rings and taps, so only the
// first one gets to run; the others forward their request and exit.
constexpr char kShowGuideNotification[] = "com.huishengstudio.agent.show-guide";

// Returns the lock fd while this process owns the agent role, 0 when another
// agent already does, and -1 when locking is impossible (then we run anyway).
int acquireInstanceLock()
{
    NSString* dir = [@"~/Library/Application Support/huishengstudio" stringByExpandingTildeInPath];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];

    NSString* path = [dir stringByAppendingPathComponent:@"agent.lock"];
    const int fd = ::open([path fileSystemRepresentation], O_CREAT | O_RDWR, 0644);

    if (fd < 0)
        return -1;

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        ::close(fd);
        return 0;
    }

    return fd;
}

} // namespace

@interface AgentDelegate : NSObject
@end

@implementation AgentDelegate
{
    Core* core;
    uint64_t lastGuideTick;
}

- (instancetype)init
{
    self = [super init];

    if (self != nil)
        core = new Core();

    return self;
}

- (void)dealloc
{
    delete core;
    core = nullptr;
}

- (void)start
{
    if (! core->open())
        NSLog(@"HuishengStudio Agent: could not open the shared rings");

    // A second launch (the installer runs the agent while a plug-in has already
    // started it) forwards its request here instead of starting another worker.
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(showGuideOnRequest:)
               name:@(kShowGuideNotification)
             object:nil];

    [NSTimer scheduledTimerWithTimeInterval:kTickInterval
                                     target:self
                                   selector:@selector(tick:)
                                   userInfo:nil
                                    repeats:YES];

    // Rebuild everything after coreaudiod restarts: devices, taps and our own
    // sample-rate assumptions are all invalid at that point.
    AudioObjectPropertyAddress address {
        kAudioHardwarePropertyServiceRestarted,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &address,
                                        dispatch_get_main_queue(),
                                        ^(UInt32, const AudioObjectPropertyAddress*) {
        NSLog(@"HuishengStudio Agent: core audio restarted");
        core->open();
    });
}

- (void)requestPermissions
{
    // The system prompt is the first attempt; if macOS does not show one (an
    // empty usage description, a previous refusal, or a grant that lives on the
    // wrong identity) the drag-and-drop guide is what actually gets the user
    // through it.
    core->requestInstallPermissions();
    hsShowPermissionGuide();
}

- (void)showGuideOnRequest:(NSNotification*)notification
{
    (void) notification;
    [self requestPermissions];
}

- (void)tick:(NSTimer*)timer
{
    (void) timer;
    core->tick();

    const uint64_t now = hs::nowNs();

    if (now - lastGuideTick > kGuideRetryInterval)
    {
        lastGuideTick = now;
        hsShowPermissionGuideIfNeeded();
    }
}

@end

int main(int argc, const char* argv[])
{
    const bool requestPermissions = argc > 1 && std::strcmp(argv[1], "--request-permissions") == 0;
    const bool showGuide = argc > 1 && std::strcmp(argv[1], "--permissions") == 0;
    const bool snapshotGuide = argc > 2 && std::strcmp(argv[1], "--snapshot-permissions") == 0;

    @autoreleasepool
    {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // Render-only mode: no rings, no taps, no second worker - it only paints
        // the guide so a test run can inspect it.
        if (snapshotGuide)
        {
            NSString* path = [NSString stringWithUTF8String:argv[2]];

            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                const bool written = hsSnapshotPermissionGuide([path UTF8String]);
                NSLog(@"huishengstudio: permission guide snapshot %@ -> %@",
                      written ? @"written" : @"failed", path);
                [NSApp terminate:nil];
            });

            [application run];
            return 0;
        }

        const int instanceLock = acquireInstanceLock();

        if (instanceLock == 0)
        {
            if (requestPermissions || showGuide)
            {
                NSLog(@"HuishengStudio Agent: another instance is running; handing over the guide");
                [[NSDistributedNotificationCenter defaultCenter]
                    postNotificationName:@(kShowGuideNotification)
                                  object:nil
                                userInfo:nil
                        deliverImmediately:YES];
            }
            else
            {
                NSLog(@"HuishengStudio Agent: another instance is running; exiting");
            }

            return 0;
        }

        AgentDelegate* delegate = [[AgentDelegate alloc] init];
        [delegate start];

        if (requestPermissions)
        {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                [delegate requestPermissions];
            });
        }

        if (showGuide)
        {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                hsShowPermissionGuide();
            });
        }

        [application run];
    }

    return 0;
}
