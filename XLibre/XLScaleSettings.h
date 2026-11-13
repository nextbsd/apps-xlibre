//
//  XLScaleSettings.h
//  XLibre.app - the HiDPI scale for X programs
//
//  Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the
//  "Software"), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be included
//  in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//  On a HiDPI screen Xnextbsd draws at WindowServer's backing scale: its
//  X screen is the screen's size in points times the scale, and it says
//  the scale on the root window (_XNEXTBSD_SCALE, CARDINAL; 1 on ordinary
//  screens and when XLibreLowResolution makes it draw at 1x). This object
//  follows that property on its own XCB connection and tells X programs,
//  the way a desktop's settings daemon does:
//
//  - RESOURCE_MANAGER: Xft.dpi (96 times the scale) and Xcursor.size (24
//    times the scale), and at 2x and more a larger core font for xterm
//    (XTerm*font: 10x20), merged into what is there (xrdb's other
//    resources stay);
//  - XSETTINGS (it owns _XSETTINGS_S0): Gdk/WindowScalingFactor,
//    Gdk/UnscaledDPI and Xft/DPI, the serial bumped on every change, so
//    running GTK programs follow a new scale (those that have no GDK_SCALE
//    of their own: the ones XLibre.app starts keep theirs);
//  - the environment of the X programs XLibre.app starts (-environment):
//    GDK_SCALE, GDK_DPI_SCALE, QT_SCREEN_SCALE_FACTORS,
//    QT_ENABLE_HIGHDPI_SCALING and XCURSOR_SIZE; see -environment in the
//    .m for why these and not others;
//  - the window manager's title bars (URSDecorationMetrics), which are X
//    windows too, with XLScaleDidChangeNotification when it changes.
//

#import <Foundation/Foundation.h>

// Posted (on the main thread) when the scale changes after -start.
extern NSString *const XLScaleDidChangeNotification;

@interface XLScaleSettings : NSObject

// X pixels per WindowServer point: 1, 2 or 3.
@property (readonly, nonatomic) NSUInteger scale;

// Connects to the X server named by DISPLAY, reads the scale and
// publishes it. Call before X programs start.
- (BOOL)start;
- (void)stop;

// The resource lines for the current scale ("Xft.dpi: 192\n..."), for a
// RESOURCE_MANAGER written from scratch.
- (NSString *)resourceLines;

// Environment variables for X programs started at the current scale.
- (NSDictionary *)environment;

@end
