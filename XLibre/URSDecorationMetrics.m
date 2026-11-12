//
//  URSDecorationMetrics.m
//  uroswm - GSTheme-derived window decoration layout
//

#import "URSDecorationMetrics.h"
#import <GNUstepGUI/GSTheme.h>
#import <math.h>

// Optional theme hooks not (yet) in libs-gui
@interface GSTheme (URSOptionalDecorationMethods)
- (NSRect)zoomButtonFrameForBounds:(NSRect)bounds;
- (CGFloat)titlebarCornerRadius;
- (CGFloat)windowBottomCornerRadius;
@end

@implementation URSDecorationMetrics

+ (uint16_t)titlebarHeight
{
    return (uint16_t)lround([[GSTheme theme] titlebarHeight]);
}

+ (uint16_t)resizebarHeight
{
    return (uint16_t)lround([[GSTheme theme] resizebarHeight]);
}

+ (uint16_t)resizebarNotchWidth
{
    return (uint16_t)lround([[GSTheme theme] resizebarNotchWidth]);
}

+ (uint16_t)borderWidth
{
    return 1;
}

+ (BOOL)hasTitleBarForStyleMask:(NSUInteger)styleMask
{
    return (styleMask & (NSTitledWindowMask | NSClosableWindowMask |
                         NSMiniaturizableWindowMask)) != 0;
}

+ (BOOL)hasResizeBarForStyleMask:(NSUInteger)styleMask
{
    return (styleMask & NSResizableWindowMask) != 0;
}

+ (void)offsetsForStyleMask:(NSUInteger)styleMask
                       left:(uint16_t *)l right:(uint16_t *)r
                        top:(uint16_t *)t bottom:(uint16_t *)b
{
    uint16_t border = (styleMask & URSDecorationStyleBits) ? [self borderWidth] : 0;
    uint16_t left = border, right = border, top = border, bottom = border;

    if ([self hasTitleBarForStyleMask:styleMask])
        top = [self titlebarHeight];
    if ([self hasResizeBarForStyleMask:styleMask])
        bottom = [self resizebarHeight];

    if (l) *l = left;
    if (r) *r = right;
    if (t) *t = top;
    if (b) *b = bottom;
}

+ (BOOL)image:(NSString *)name inDirectory:(NSString *)dir ofBundle:(NSBundle *)bundle
{
    if (!bundle)
        return NO;
    NSString *ext = [name pathExtension];
    NSString *base = [name stringByDeletingPathExtension];
    if ([ext length] > 0)
        return [bundle pathForResource:base ofType:ext inDirectory:dir] != nil;
    for (NSString *type in [NSImage imageFileTypes]) {
        if ([bundle pathForResource:base ofType:type inDirectory:dir] != nil)
            return YES;
    }
    return NO;
}

+ (BOOL)themeProvidesImageNamed:(NSString *)name
{
    GSTheme *theme = [GSTheme theme];
    NSBundle *bundle = [theme bundle];
    if (bundle) {
        // Same lookup order as -[NSImage _pathForThemeImageNamed:ofType:]
        NSString *mapped = [[[theme infoDictionary] objectForKey:@"GSThemeImages"] objectForKey:name];
        if (mapped && [self image:mapped inDirectory:@"ThemeImages" ofBundle:bundle])
            return YES;
        if ([self image:name inDirectory:@"ThemeImages" ofBundle:bundle])
            return YES;
    }
    // Stock AppKit images (Library/Images in the GNUstep domains)
    for (NSString *type in [NSImage imageFileTypes]) {
        if ([NSBundle pathForLibraryResource:name ofType:type inDirectory:@"Images"] != nil)
            return YES;
    }
    return NO;
}

+ (NSArray *)buttonsForStyleMask:(NSUInteger)styleMask
{
    NSMutableArray *buttons = [NSMutableArray array];
    if (styleMask & NSMiniaturizableWindowMask)
        [buttons addObject:@(NSWindowMiniaturizeButton)];
    if ((styleMask & NSResizableWindowMask) &&
        [self themeProvidesImageNamed:@"common_Zoom"])
        [buttons addObject:@(NSWindowZoomButton)];
    if (styleMask & NSClosableWindowMask)
        [buttons addObject:@(NSWindowCloseButton)];
    return buttons;
}

+ (NSRect)frameForButton:(NSWindowButton)button
            titleBarSize:(NSSize)size
               styleMask:(NSUInteger)styleMask
{
    GSTheme *theme = [GSTheme theme];
    NSRect bounds = NSMakeRect(0, 0, size.width, size.height);

    switch (button) {
        case NSWindowCloseButton:
            return [theme closeButtonFrameForBounds:bounds];
        case NSWindowMiniaturizeButton:
            return [theme miniaturizeButtonFrameForBounds:bounds];
        case NSWindowZoomButton: {
            if ([theme respondsToSelector:@selector(zoomButtonFrameForBounds:)])
                return [theme zoomButtonFrameForBounds:bounds];
            // TODO(libs-gui): no zoom placement in GSTheme; sit left of close.
            NSRect frame = [theme closeButtonFrameForBounds:bounds];
            if (styleMask & NSClosableWindowMask)
                frame.origin.x -= [theme titlebarButtonSize] + [theme titlebarPaddingRight];
            return frame;
        }
        default:
            return NSZeroRect;
    }
}

+ (NSInteger)buttonAtX11Point:(NSPoint)point
                 titleBarSize:(NSSize)size
                    styleMask:(NSUInteger)styleMask
{
    NSPoint p = NSMakePoint(point.x, size.height - point.y);
    for (NSNumber *b in [self buttonsForStyleMask:styleMask]) {
        NSWindowButton button = (NSWindowButton)[b integerValue];
        NSRect frame = [self frameForButton:button titleBarSize:size styleMask:styleMask];
        if (NSMouseInRect(p, frame, NO))
            return button;
    }
    return -1;
}

+ (NSButton *)themeButton:(NSWindowButton)button
                styleMask:(NSUInteger)styleMask
              highlighted:(BOOL)highlighted
           documentEdited:(BOOL)edited
{
    NSButton *b = [[GSTheme theme] standardWindowButton:button forStyleMask:styleMask];

    // Same image swap GSStandardWindowDecorationView does for edited documents
    if (button == NSWindowCloseButton && edited) {
        [b setImage:[NSImage imageNamed:@"common_CloseBroken"]];
        [b setAlternateImage:[NSImage imageNamed:@"common_CloseBrokenH"]];
    }
    // TODO(libs-gui): -standardWindowButton: sets no zoom image yet.
    if (button == NSWindowZoomButton && [b image] == nil) {
        [b setImage:[NSImage imageNamed:@"common_Zoom"]];
        [b setAlternateImage:[NSImage imageNamed:@"common_ZoomH"]];
    }

    [[b cell] setHighlighted:highlighted];
    return b;
}

+ (uint16_t)topCornerRadiusForStyleMask:(NSUInteger)styleMask
{
    GSTheme *theme = [GSTheme theme];
    if (![self hasTitleBarForStyleMask:styleMask] ||
        ![theme respondsToSelector:@selector(titlebarCornerRadius)])
        return 0;
    long radius = lround([theme titlebarCornerRadius]);
    return (uint16_t)MAX(0, MIN(radius, (long)[self titlebarHeight]));
}

+ (uint16_t)bottomCornerRadiusForStyleMask:(NSUInteger)styleMask
{
    GSTheme *theme = [GSTheme theme];
    if (![self hasResizeBarForStyleMask:styleMask] ||
        ![theme respondsToSelector:@selector(windowBottomCornerRadius)])
        return 0;
    long radius = lround([theme windowBottomCornerRadius]);
    return (uint16_t)MAX(0, MIN(radius, (long)[self resizebarHeight]));
}

+ (NSColor *)borderColor
{
    NSColor *color = [[GSTheme theme] colorNamed:@"windowBorderColor"
                                           state:GSThemeNormalState];
    return color ? color : [NSColor blackColor];
}

+ (uint32_t)borderPixel
{
    NSColor *c = [[self borderColor] colorUsingColorSpaceName:NSDeviceRGBColorSpace];
    if (!c)
        return 0xFF000000;
    CGFloat r, g, b, a;
    [c getRed:&r green:&g blue:&b alpha:&a];
    return (0xFFu << 24) |
           ((uint32_t)lround(r * 255) << 16) |
           ((uint32_t)lround(g * 255) << 8) |
           (uint32_t)lround(b * 255);
}

@end
