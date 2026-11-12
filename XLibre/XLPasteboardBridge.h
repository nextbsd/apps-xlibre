//
//  XLPasteboardBridge.h
//  XLibre.app - X CLIPBOARD <-> GNUstep general pasteboard
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//
//  An xpbproxy-style bridge (libs-back's Tools/xpbs.m is the model) on its
//  own XCB connection to Xnextbsd. gpbs can't do this itself: it opens
//  DISPLAY once at login, before any X server exists.
//
//  - X to GNUstep: XFixes says when a client takes CLIPBOARD; the bridge
//    converts it to UTF8_STRING (STRING if the owner can't), INCR
//    included, and puts the text on the general pasteboard.
//  - GNUstep to X: when the general pasteboard's change count moves and it
//    holds a string, the bridge takes CLIPBOARD and answers TARGETS,
//    UTF8_STRING, TEXT and STRING requests, with INCR for large text.
//
//  PRIMARY, images, RTF and drag and drop are later work (apps-xlibre#7).
//

#import <Foundation/Foundation.h>

@interface XLPasteboardBridge : NSObject

// Connects to the X server named by DISPLAY and starts bridging.
- (BOOL)start;
- (void)stop;

@end
