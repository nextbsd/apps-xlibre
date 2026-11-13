//
//  XLibreController.h
//  XLibre.app - the application
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//
//  XLibre.app runs X11 programs on NextBSD's WindowServer, as XQuartz does
//  on macOS. At launch it starts Xnextbsd (XLServer), joined to its own
//  WindowServer app, runs the window manager (URSHybridEventHandler) and
//  the clipboard bridge (XLPasteboardBridge) in this process, and opens an
//  xterm. Its menu has Info (About, Preferences), Applications, Hide and
//  Quit.
//
//  Defaults:
//    XLibreAppToRun         run at launch (default /usr/X11/bin/xterm;
//                           empty for nothing), unless XLibre.app is
//                           opened with programs to run (openapp XLibre
//                           /usr/X11/bin/xclock, or Open With)
//    XLibreTerminal         the Applications menu's xterm
//    XLibreApplications     more Applications menu items: an array of
//                           commands, or of {Name, Command, Key} dictionaries
//    XLibreXResources       X resources loaded into the new server
//                           (default: XTerm*selectToClipboard: true, so
//                           xterm's selection and paste use CLIPBOARD)
//    XLibreLowResolution    on a HiDPI display, run X programs at 1x and
//                           let WindowServer scale them up, as XQuartz
//                           does (Xnextbsd -lowres; default NO: they
//                           draw at the display's scale, XLScaleSettings)
//    XLibreServerPath       an Xnextbsd other than Resources/Xnextbsd
//    XLibreServerArguments  extra arguments for Xnextbsd
//

#import <AppKit/AppKit.h>

@interface XLibreController : NSObject <NSApplicationDelegate>

- (void)runApplication:(id)sender;
- (void)showPreferences:(id)sender;

@end
