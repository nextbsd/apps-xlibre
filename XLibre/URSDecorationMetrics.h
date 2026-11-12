//
//  URSDecorationMetrics.h
//  uroswm - GSTheme-derived window decoration layout
//
//  Single source of truth for decoration geometry. Every value comes from
//  the active GSTheme; nothing here encodes a look of its own.
//
//  Some rules below mirror libs-gui code that is not yet exposed as GSTheme
//  API (GSStandardWindowDecorationView). They are marked TODO(libs-gui) and
//  should be replaced once GSTheme grows the matching methods.
//

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

// Decoration style bits we care about (subset of NSWindow style masks)
#define URSDecorationStyleBits (NSTitledWindowMask | NSClosableWindowMask | \
                                NSMiniaturizableWindowMask | NSResizableWindowMask)

@interface URSDecorationMetrics : NSObject

// Vertical metrics, in pixels
+ (uint16_t)titlebarHeight;
+ (uint16_t)resizebarHeight;
+ (uint16_t)resizebarNotchWidth;

// Width of the outline drawn around decorated windows.
// TODO(libs-gui): GSStandardWindowDecorationView hardcodes 1px; no GSTheme API yet.
+ (uint16_t)borderWidth;

+ (BOOL)hasTitleBarForStyleMask:(NSUInteger)styleMask;
+ (BOOL)hasResizeBarForStyleMask:(NSUInteger)styleMask;

// Window frame offsets (left, right, top, bottom) for a style mask, matching
// +[GSStandardWindowDecorationView offsets::::forStyleMask:].
+ (void)offsetsForStyleMask:(NSUInteger)styleMask
                       left:(uint16_t *)l right:(uint16_t *)r
                        top:(uint16_t *)t bottom:(uint16_t *)b;

// YES if the active theme (or, without a theme bundle, stock AppKit) ships an
// image with this name. Unlike +[NSImage imageNamed:], this is not affected by
// images still registered from a previously active theme.
+ (BOOL)themeProvidesImageNamed:(NSString *)name;

// Title bar buttons (NSNumber-wrapped NSWindowButton) the theme shows for a
// style mask. Close/miniaturize follow the style mask; zoom appears only when
// the window is resizable and the theme provides a zoom image (common_Zoom).
// TODO(libs-gui): replace with -[GSTheme titleBarHasButton:forStyleMask:].
+ (NSArray *)buttonsForStyleMask:(NSUInteger)styleMask;

// Button frame inside a title bar of the given size, in unflipped
// (AppKit, origin bottom-left) coordinates.
+ (NSRect)frameForButton:(NSWindowButton)button
            titleBarSize:(NSSize)size
               styleMask:(NSUInteger)styleMask;

// Hit test using X11 (origin top-left) coordinates. Returns -1 for none.
+ (NSInteger)buttonAtX11Point:(NSPoint)point
                 titleBarSize:(NSSize)size
                    styleMask:(NSUInteger)styleMask;

// Theme button cell for drawing, configured for the requested state.
+ (NSButton *)themeButton:(NSWindowButton)button
                styleMask:(NSUInteger)styleMask
              highlighted:(BOOL)highlighted
           documentEdited:(BOOL)edited;

// Corner radii for decorated windows. Top corners round the title bar; bottom
// corners round the resize bar, so they are 0 for windows without one.
// TODO(libs-gui): themes provide these through the informal
// -titlebarCornerRadius / -windowBottomCornerRadius methods for now.
+ (uint16_t)topCornerRadiusForStyleMask:(NSUInteger)styleMask;
+ (uint16_t)bottomCornerRadiusForStyleMask:(NSUInteger)styleMask;

// Window outline colour (theme "windowBorderColor", black fallback as in GSTheme)
+ (NSColor *)borderColor;

// Same colour as a 0xAARRGGBB pixel value
+ (uint32_t)borderPixel;

@end
