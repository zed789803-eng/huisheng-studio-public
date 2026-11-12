#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "PermissionGuide.h"
#include "Permissions.h"

#include "Config.h"
#include "Control.h"
#include "SystemCompat.h"

#include <string>

// -----------------------------------------------------------------------------
// Product facts the window needs
// -----------------------------------------------------------------------------

namespace {

constexpr CGFloat kWindowWidth = 520.0;
constexpr CGFloat kWindowHeight = 496.0;

enum class Grant
{
    Unknown = -1,
    Denied = 0,
    Undecided = 1,
    Granted = 2,
};

Grant toGrant(int raw)
{
    switch (raw)
    {
        case 0: return Grant::Denied;
        case 1: return Grant::Undecided;
        case 2: return Grant::Granted;
        default: return Grant::Unknown;
    }
}

NSString* statusText(Grant grant)
{
    switch (grant)
    {
        case Grant::Granted: return @"已授权";
        case Grant::Denied: return @"已拒绝";
        case Grant::Undecided: return @"未授权";
        default: return @"未知";
    }
}

// The two grants this product needs.
enum class Service { Microphone = 0, AudioCapture = 1 };

NSString* serviceTitle(Service service)
{
    return service == Service::Microphone ? @"麦克风" : @"录屏与系统录音";
}

NSString* serviceReason(Service service)
{
    return service == Service::Microphone
               ? @"读取 huishengstudio 虚拟输入通道（选择「虚拟通道」时需要）"
               : @"采集你选择的 App 的声音（选择「应用」时需要）";
}

// System Settings panes. Privacy_ScreenCapture is where macOS puts 系统录音:
// both live in the same "录屏与系统录音" list.
NSString* serviceSettingsURL(Service service)
{
    return service == Service::Microphone
               ? @"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"
               : @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
}

// Fixed palette from ui/Theme.h. System "label" colours resolve against the
// process appearance, which is Aqua for a background agent, so text set from
// [NSColor labelColor] would draw black on the dark plate.
static NSColor* const kTextPrimary   = [NSColor colorWithSRGBRed:0.949 green:0.953 blue:0.961 alpha:1.0];
static NSColor* const kTextSecondary = [NSColor colorWithSRGBRed:0.545 green:0.569 blue:0.596 alpha:1.0];
static NSColor* const kTextTertiary  = [NSColor colorWithSRGBRed:0.365 green:0.388 blue:0.412 alpha:1.0];
static NSColor* const kPositive      = [NSColor colorWithSRGBRed:0.298 green:0.851 blue:0.392 alpha:1.0];
static NSColor* const kWarning       = [NSColor colorWithSRGBRed:1.000 green:0.690 blue:0.125 alpha:1.0];
static NSColor* const kDanger        = [NSColor colorWithSRGBRed:1.000 green:0.353 blue:0.322 alpha:1.0];

static NSColor* const kSurface       = [NSColor colorWithSRGBRed:0.090 green:0.098 blue:0.110 alpha:1.0];

Grant currentGrant(Service service)
{
    return service == Service::Microphone ? toGrant(microphonePermissionRaw())
                                          : toGrant(audioCapturePermissionRaw());
}

// Only complain about a grant the mode in use actually needs.
bool serviceNeeded(Service service, hs::CaptureMode mode)
{
    const bool app = mode == hs::CaptureMode::App || mode == hs::CaptureMode::Both;
    const bool channel = mode == hs::CaptureMode::Channel || mode == hs::CaptureMode::Both;

    // No tap service exists before 14.2, so nothing can be missing there.
    if (service == Service::AudioCapture && ! hs::systemSupportsAppCapture())
        return false;

    return service == Service::Microphone ? channel : app;
}

hs::CaptureMode currentMode()
{
    hs::ControlMapping mapping;

    if (! mapping.open())
        return hs::CaptureMode::Off;

    // 一路实例一个槽位：只要还有活着的槽位在采 App 或在采虚拟通道，引导页就要提这件事。
    const uint64_t now = hs::nowNs();
    bool app = false;
    bool channel = false;

    for (int i = 0; i < hs::kCaptureSlots; ++i)
    {
        const auto* slot = &mapping.block()->slots[i];
        const uint32_t owner = slot->owner.load(std::memory_order_relaxed);
        const uint64_t beat = slot->heartbeat.load(std::memory_order_relaxed);
        const uint32_t raw = slot->mode.load(std::memory_order_relaxed);

        if (owner == 0 || beat == 0 || now - beat > hs::kSlotLeaseNanos || raw > 3)
            continue;

        const auto mode = static_cast<hs::CaptureMode>(raw);
        app = app || mode == hs::CaptureMode::App || mode == hs::CaptureMode::Both;
        channel = channel || mode == hs::CaptureMode::Channel || mode == hs::CaptureMode::Both;
    }

    if (app && channel)
        return hs::CaptureMode::Both;

    if (app)
        return hs::CaptureMode::App;

    if (channel)
        return hs::CaptureMode::Channel;

    return hs::CaptureMode::Off;
}

NSImage* appIcon()
{
    NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:[[NSBundle mainBundle] bundlePath]];
    icon.size = NSMakeSize(96, 96);
    return icon;
}

NSColor* statusColour(Grant grant, BOOL needed)
{
    if (! needed)
        return kTextTertiary;

    switch (grant)
    {
        case Grant::Granted: return kPositive;
        case Grant::Undecided: return kWarning;
        case Grant::Denied: return kDanger;
        default: return kTextSecondary;
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Drag source. The whole point: pick the icon up and drop it into the System
// Settings privacy list, which is the only reliable way to add an app that
// macOS has not listed on its own.
// -----------------------------------------------------------------------------

@interface HSDragIconView : NSView <NSDraggingSource>
@end

@implementation HSDragIconView
{
    NSImage* icon_;
    BOOL enabled_;
    BOOL hovering_;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];

    if (self != nil)
    {
        icon_ = appIcon();
        enabled_ = YES;
        [self setWantsLayer:YES];
    }

    return self;
}

- (void)setEnabled:(BOOL)enabled
{
    enabled_ = enabled;
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];

    for (NSTrackingArea* area in [self trackingAreas])
        [self removeTrackingArea:area];

    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:[self bounds]
             options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow)
               owner:self
            userInfo:nil];
    [self addTrackingArea:area];
}

- (void)mouseEntered:(NSEvent*)event
{
    (void) event;
    hovering_ = YES;
    [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent*)event
{
    (void) event;
    hovering_ = NO;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void) dirtyRect;

    const NSRect bounds = [self bounds];
    NSBezierPath* plate = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:14 yRadius:14];

    const CGFloat alpha = enabled_ ? (hovering_ ? 0.14 : 0.08) : 0.04;
    [[NSColor colorWithWhite:1.0 alpha:alpha] setFill];
    [plate fill];

    [[NSColor colorWithWhite:1.0 alpha:enabled_ ? 0.18 : 0.08] setStroke];
    [plate setLineWidth:1.0];
    [plate stroke];

    if (icon_ == nil)
        return;

    const CGFloat side = MIN(NSWidth(bounds), NSHeight(bounds)) - 26.0;
    const NSRect iconRect = NSMakeRect(NSMidX(bounds) - side / 2.0,
                                       NSMidY(bounds) - side / 2.0 + 10.0,
                                       side, side);

    [icon_ drawInRect:iconRect
             fromRect:NSZeroRect
            operation:NSCompositingOperationSourceOver
             fraction:enabled_ ? 1.0 : 0.4
       respectFlipped:YES
                hints:nil];

    // A grab handle under the icon says "pick me up" without words.
    NSString* handle = @"⋮⋮";
    NSDictionary* attributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor colorWithWhite:1.0 alpha:enabled_ ? 0.45 : 0.2]
    };
    const NSSize size = [handle sizeWithAttributes:attributes];
    [handle drawAtPoint:NSMakePoint(NSMidX(bounds) - size.width / 2.0, 4.0)
         withAttributes:attributes];
}

- (void)mouseDragged:(NSEvent*)event
{
    if (! enabled_)
        return;

    NSString* path = [[NSBundle mainBundle] bundlePath];

    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    [item setString:[@"file://" stringByAppendingString:path]
            forType:NSPasteboardTypeFileURL];

    NSDraggingItem* dragging = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
    const CGFloat side = 64.0;
    const NSRect frame = NSMakeRect(NSMidX([self bounds]) - side / 2.0,
                                    NSMidY([self bounds]) - side / 2.0,
                                    side, side);
    [dragging setDraggingFrame:frame contents:icon_];

    NSDraggingSession* session = [self beginDraggingSessionWithItems:@[ dragging ]
                                                               event:event
                                                              source:self];
    session.animatesToStartingPositionsOnCancelOrFail = YES;
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void) session;
    (void) context;
    return NSDragOperationCopy;
}

@end

// -----------------------------------------------------------------------------
// Rounded card used for the drag target and for each grant row.
// -----------------------------------------------------------------------------

@interface HSCardView : NSView
@property(nonatomic, strong) NSColor* borderColour;
@property(nonatomic, assign) CGFloat fillAlpha;
@end

@implementation HSCardView

- (void)drawRect:(NSRect)dirtyRect
{
    (void) dirtyRect;

    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:[self bounds]
                                                         xRadius:12
                                                         yRadius:12];
    [[NSColor colorWithWhite:1.0 alpha:self.fillAlpha] setFill];
    [path fill];

    [(self.borderColour ?: [NSColor colorWithWhite:1.0 alpha:0.12]) setStroke];
    [path setLineWidth:1.0];
    [path stroke];
}

@end

// -----------------------------------------------------------------------------
// Window controller
// -----------------------------------------------------------------------------

@interface HSGrantRow : NSObject
@property(nonatomic, strong) HSCardView* container;
@property(nonatomic, strong) NSTextField* title;
@property(nonatomic, strong) NSTextField* reason;
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSView* statusDot;
@property(nonatomic, strong) NSButton* openButton;
@property(nonatomic, assign) Service service;
@end

@implementation HSGrantRow
@end

@interface HSPermissionWindowController : NSWindowController <NSWindowDelegate>
- (void)show;
- (void)refresh;
@end

@implementation HSPermissionWindowController
{
    NSWindow* window_;
    HSDragIconView* dragIcon_;
    HSCardView* dragCard_;

    HSGrantRow* micRow_;
    HSGrantRow* captureRow_;

    NSTextField* headline_;
    NSTextField* subtitle_;
    NSTextField* hint_;

    NSTimer* timer_;
    NSDate* allGrantedSince_;
    BOOL closing_;
}

// Places a direct child of the content view using top-down coordinates.
- (void)setFrame:(NSView*)view
              top:(CGFloat)top
             left:(CGFloat)left
            width:(CGFloat)width
           height:(CGFloat)height
{
    const CGFloat contentHeight = NSHeight(window_.contentView.bounds);
    view.frame = NSMakeRect(left, contentHeight - top - height, width, height);
}

// Places a child inside a card. The top-down origin must be measured against
// the card's own height, not the window's, or every row lands outside the
// card's bounds and gets clipped away.
- (void)setFrame:(NSView*)view
          inCard:(NSView*)card
              top:(CGFloat)top
             left:(CGFloat)left
            width:(CGFloat)width
           height:(CGFloat)height
{
    view.frame = NSMakeRect(left, NSHeight(card.bounds) - top - height, width, height);
}

- (instancetype)init
{
    NSRect frame = NSMakeRect(0, 0, kWindowWidth, kWindowHeight);

    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:(NSWindowStyleMaskTitled
                                                     | NSWindowStyleMaskClosable)
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    window_.title = @"绘声Studio · 权限引导";
    window_.releasedWhenClosed = NO;
    window_.delegate = self;
    window_.level = NSFloatingWindowLevel;
    window_.titlebarAppearsTransparent = YES;

    // The product is dark only. Without pinning the appearance, a window
    // created from an accessory app can resolve dynamic colours against the
    // wrong appearance and end up white text on a white background.
    window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [window_ center];

    self = [super initWithWindow:window_];

    if (self == nil)
        return nil;

    NSView* content = window_.contentView;
    window_.backgroundColor = [NSColor colorWithSRGBRed:0.055
                                                  green:0.059
                                                   blue:0.067
                                                  alpha:1.0];
    content.wantsLayer = YES;
    content.layer.backgroundColor = window_.backgroundColor.CGColor;

    // ---- header -----------------------------------------------------------
    headline_ = [self label:@"还需要授权" size:20.0 weight:NSFontWeightSemibold
                      colour:kTextPrimary];
    [self setFrame:headline_ top:26 left:28 width:440 height:26];
    [content addSubview:headline_];

    subtitle_ = [self label:@"把左边的图标拖进系统设置右侧的列表，再打开开关。授权后窗口会自动关闭。"
                       size:12.0 weight:NSFontWeightRegular
                     colour:kTextSecondary];
    [self setFrame:subtitle_ top:56 left:28 width:464 height:36];
    subtitle_.maximumNumberOfLines = 2;
    [content addSubview:subtitle_];

    // ---- drag card --------------------------------------------------------
    dragCard_ = [[HSCardView alloc] initWithFrame:NSZeroRect];
    dragCard_.fillAlpha = 0.05;
    [self setFrame:dragCard_ top:108 left:28 width:464 height:136];
    [content addSubview:dragCard_];

    dragIcon_ = [[HSDragIconView alloc] initWithFrame:NSZeroRect];
    [self setFrame:dragIcon_ inCard:dragCard_ top:16 left:16 width:104 height:104];
    [dragCard_ addSubview:dragIcon_];

    NSTextField* appName = [self label:@"绘声Studio" size:14.0 weight:NSFontWeightMedium
                                colour:kTextPrimary];
    [self setFrame:appName inCard:dragCard_ top:20 left:140 width:308 height:22];
    [dragCard_ addSubview:appName];

    NSTextField* appPath = [self label:[[NSBundle mainBundle] bundlePath]
                                  size:10.5 weight:NSFontWeightRegular
                                colour:kTextTertiary];
    appPath.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self setFrame:appPath inCard:dragCard_ top:44 left:140 width:308 height:16];
    [dragCard_ addSubview:appPath];

    hint_ = [self label:@"选中图标后按住拖动。拖不动就用下面的「打开系统设置」，再点列表下方的 + 号选择这个 app。"
                    size:11.0 weight:NSFontWeightRegular
                  colour:kTextSecondary];
    hint_.maximumNumberOfLines = 3;
    [self setFrame:hint_ inCard:dragCard_ top:66 left:140 width:308 height:52];
    [dragCard_ addSubview:hint_];

    // ---- grant rows -------------------------------------------------------
    micRow_ = [self makeRow:Service::Microphone top:256];
    captureRow_ = [self makeRow:Service::AudioCapture top:348];
    [content addSubview:micRow_.container];
    [content addSubview:captureRow_.container];

    // ---- footer -----------------------------------------------------------
    NSButton* recheck = [NSButton buttonWithTitle:@"重新检查"
                                           target:self
                                           action:@selector(recheck:)];
    recheck.bezelStyle = NSBezelStyleRounded;
    [self setFrame:recheck top:446 left:28 width:100 height:28];
    [content addSubview:recheck];

    NSTextField* footer = [self label:@"也可以在终端运行：HuishengStudio Agent --permissions"
                                 size:10.5 weight:NSFontWeightRegular
                               colour:kTextTertiary];
    [self setFrame:footer top:452 left:140 width:352 height:18];
    [content addSubview:footer];

    return self;
}

- (NSTextField*)label:(NSString*)text
                 size:(CGFloat)size
               weight:(NSFontWeight)weight
               colour:(NSColor*)colour
{
    NSTextField* label = [NSTextField labelWithString:(text ?: @"")];
    label.font = [NSFont systemFontOfSize:size weight:weight];
    label.textColor = colour;
    label.maximumNumberOfLines = 0;
    label.lineBreakMode = NSLineBreakByWordWrapping;
    return label;
}

- (HSGrantRow*)makeRow:(Service)service top:(CGFloat)top
{
    HSCardView* container = [[HSCardView alloc] initWithFrame:NSZeroRect];
    container.fillAlpha = 0.04;
    [self setFrame:container top:top left:28 width:464 height:80];

    HSGrantRow* row = [[HSGrantRow alloc] init];
    row.service = service;
    row.container = container;

    NSView* dot = [[NSView alloc] initWithFrame:NSZeroRect];
    dot.wantsLayer = YES;
    dot.layer.cornerRadius = 4.0;
    [self setFrame:dot inCard:container top:20 left:18 width:8 height:8];
    [container addSubview:dot];
    row.statusDot = dot;

    NSTextField* title = [self label:serviceTitle(service) size:13.5
                              weight:NSFontWeightSemibold
                              colour:kTextPrimary];
    [self setFrame:title inCard:container top:13 left:36 width:250 height:20];
    [container addSubview:title];
    row.title = title;

    NSTextField* reason = [self label:serviceReason(service) size:11.0
                               weight:NSFontWeightRegular
                               colour:kTextSecondary];
    reason.lineBreakMode = NSLineBreakByTruncatingTail;
    [self setFrame:reason inCard:container top:35 left:36 width:250 height:18];
    [container addSubview:reason];
    row.reason = reason;

    NSTextField* status = [self label:@"…" size:12.0
                               weight:NSFontWeightMedium
                               colour:kTextSecondary];
    status.alignment = NSTextAlignmentRight;
    [self setFrame:status inCard:container top:14 left:300 width:148 height:20];
    [container addSubview:status];
    row.status = status;

    NSButton* open = [NSButton buttonWithTitle:@"打开系统设置"
                                        target:self
                                        action:@selector(openSettings:)];
    open.bezelStyle = NSBezelStyleRounded;
    open.controlSize = NSControlSizeSmall;
    open.font = [NSFont systemFontOfSize:11.0];
    open.tag = static_cast<NSInteger>(service);
    [self setFrame:open inCard:container top:40 left:316 width:132 height:24];
    [container addSubview:open];
    row.openButton = open;

    return row;
}

- (void)openSettings:(NSButton*)sender
{
    const auto service = static_cast<Service>(sender.tag);
    NSURL* url = [NSURL URLWithString:serviceSettingsURL(service)];

    if (url != nil)
        [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)recheck:(id)sender
{
    (void) sender;
    [self refresh];
}

- (void)show
{
    [NSApp activateIgnoringOtherApps:YES];
    [window_ makeKeyAndOrderFront:nil];

    closing_ = NO;
    allGrantedSince_ = nil;

    [self refresh];

    if (timer_ == nil)
    {
        timer_ = [NSTimer scheduledTimerWithTimeInterval:0.6
                                                  target:self
                                                selector:@selector(tick:)
                                                userInfo:nil
                                                 repeats:YES];
    }
}

- (void)hide
{
    [timer_ invalidate];
    timer_ = nil;
    [window_ orderOut:nil];
}

- (void)windowWillClose:(NSNotification*)notification
{
    (void) notification;
    [timer_ invalidate];
    timer_ = nil;
}

- (void)refresh
{
    const auto mode = currentMode();

    BOOL everyNeededGrantPresent = YES;

    for (HSGrantRow* row in @[ micRow_, captureRow_ ])
    {
        // 系统录音 is a process-tap grant; macOS 12 and 13 have no such service
        // at all, so the row explains the version instead of asking for a
        // permission that can never be given.
        const bool supported = row.service != Service::AudioCapture
                               || hs::systemSupportsAppCapture();
        const auto grant = currentGrant(row.service);
        const BOOL needed = serviceNeeded(row.service, mode) != false;

        if (needed && grant != Grant::Granted)
            everyNeededGrantPresent = NO;

        if (! supported)
        {
            row.status.stringValue = [NSString stringWithFormat:@"需要 macOS 14.2+（当前 %s）",
                                                                hs::systemVersionText().c_str()];
            row.status.textColor = kTextTertiary;
            row.statusDot.layer.backgroundColor =
                [kTextTertiary colorWithAlphaComponent:0.5].CGColor;
            row.statusDot.hidden = NO;
            row.container.alphaValue = 0.55;
            row.openButton.enabled = NO;
            row.container.borderColour = [NSColor colorWithWhite:1.0 alpha:0.12];
            [row.container setNeedsDisplay:YES];
            continue;
        }

        NSString* suffix = needed ? @"" : @" · 当前不需要";
        row.status.stringValue =
            [NSString stringWithFormat:@"%@%@", statusText(grant), suffix];
        row.status.textColor = statusColour(grant, needed);

        row.statusDot.layer.backgroundColor =
            [statusColour(grant, needed) colorWithAlphaComponent:needed ? 1.0 : 0.5].CGColor;
        row.statusDot.hidden = NO;

        row.container.alphaValue = needed ? 1.0 : 0.55;
        row.openButton.enabled = needed;
        row.container.borderColour =
            (needed && grant == Grant::Granted)
                ? [kPositive colorWithAlphaComponent:0.35]
                : [NSColor colorWithWhite:1.0 alpha:0.12];
        [row.container setNeedsDisplay:YES];
    }

    if (everyNeededGrantPresent)
    {
        headline_.stringValue = @"权限已就绪";
        headline_.textColor = kPositive;
        subtitle_.stringValue = @"可以关闭这个窗口，回到 DAW 继续使用了。";
        hint_.stringValue = @"已获得授权。如果刚才在系统设置里改了开关，"
                            @"macOS 可能要求重新打开一次 DAW 才生效。";
        [dragIcon_ setEnabled:NO];

        if (allGrantedSince_ == nil)
            allGrantedSince_ = [NSDate date];

        if ([[NSDate date] timeIntervalSinceDate:allGrantedSince_] > 1.2 && ! closing_)
        {
            closing_ = YES;
            [self hide];
        }
    }
    else
    {
        allGrantedSince_ = nil;
        headline_.stringValue = @"还需要授权";
        headline_.textColor = kTextPrimary;
        subtitle_.stringValue =
            @"把左边的图标拖进系统设置右侧的列表，再打开开关。授权后窗口会自动关闭。";
        hint_.stringValue =
            @"选中图标后按住拖动。拖不动就用下面的「打开系统设置」，"
            @"再点列表下方的 + 号选择这个 app。";
        [dragIcon_ setEnabled:YES];
    }
}

- (void)tick:(NSTimer*)timer
{
    (void) timer;
    [self refresh];
}

@end

// -----------------------------------------------------------------------------
// C entry points
// -----------------------------------------------------------------------------

namespace {

HSPermissionWindowController* gController = nil;

} // namespace

void hsShowPermissionGuide(void)
{
    NSLog(@"huishengstudio: permission guide requested");

    if (gController == nil)
        gController = [[HSPermissionWindowController alloc] init];

    [gController show];
}

bool hsSnapshotPermissionGuide(const char* path)
{
    if (path == nullptr || *path == '\0')
        return false;

    if (gController == nil)
        gController = [[HSPermissionWindowController alloc] init];

    [gController show];

    NSWindow* window = [gController window];
    NSView* view = [window contentView];

    // The window is on screen but the run loop has not drawn it yet; force one
    // layout+display pass so the lazy layer tree exists before we snapshot it.
    [view layoutSubtreeIfNeeded];
    [view displayIfNeeded];

    const NSRect bounds = [view bounds];
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:bounds];

    if (rep == nil)
        return false;

    [view cacheDisplayInRect:bounds toBitmapImageRep:rep];

    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];

    return png != nil && [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
}

void hsShowPermissionGuideIfNeeded(void)
{
    const auto mode = currentMode();

    if (mode == hs::CaptureMode::Off)
        return;

    const bool micMissing = serviceNeeded(Service::Microphone, mode)
                            && currentGrant(Service::Microphone) != Grant::Granted;
    const bool captureMissing = serviceNeeded(Service::AudioCapture, mode)
                                && currentGrant(Service::AudioCapture) != Grant::Granted;

    if (micMissing || captureMissing)
        hsShowPermissionGuide();
}
