//
//  XLPasteboardBridge.h
//  XLibre.app - X selections <-> GNUstep pasteboards
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//
//  An xpbproxy-style bridge (libs-back's Tools/xpbs.m is the model) on its
//  own XCB connection to Xnextbsd. gpbs can't do this itself: it opens
//  DISPLAY once at login, before any X server exists.
//
//  Two selections, as xpbs bridges them: CLIPBOARD <-> the general
//  pasteboard, and PRIMARY <-> the "Selection" pasteboard.
//
//  - X to GNUstep: XFixes says when a client takes a selection; the bridge
//    asks for its TARGETS, then converts text (UTF8_STRING, else
//    text/plain;charset=utf-8, else STRING), RTF (text/rtf,
//    application/rtf or text/richtext) and an image (image/png, else
//    image/tiff), INCR included, and puts them on the pasteboard together.
//    Images go on as both TIFF and PNG. An owner without TARGETS gets
//    asked for UTF8_STRING, then STRING.
//  - GNUstep to X: when a pasteboard's change count moves, the bridge
//    takes the selection and answers TARGETS and the targets above (TEXT
//    as UTF8_STRING) from what the pasteboard held, INCR for large data.
//
//  Drag and drop is later work (apps-xlibre#7).
//

#import <Foundation/Foundation.h>

@interface XLPasteboardBridge : NSObject

// Connects to the X server named by DISPLAY and starts bridging.
- (BOOL)start;
- (void)stop;

@end
