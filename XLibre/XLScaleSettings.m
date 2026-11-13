//
//  XLScaleSettings.m
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

#import "XLScaleSettings.h"
#import "URSDecorationMetrics.h"

#import <AppKit/AppKit.h>
#import <xcb/xcb.h>
#import <stdlib.h>
#import <string.h>

NSString *const XLScaleDidChangeNotification = @"XLScaleDidChangeNotification";

// Set by Xnextbsd on the root window (NB_SCALE_ATOM in
// Xnextbsd/hw/nextbsd/nextbsd.h).
#define XL_SCALE_ATOM "_XNEXTBSD_SCALE"

// xterm's font when X draws at 2x or more: xterm uses the core "fixed"
// font (6x13) unless it is given an Xft face, and core fonts aren't
// scaled. An Xft face is, by Xft.dpi: a 10 point monospace face is as
// large as at 1x and drawn sharp at the scale.
#define XL_XTERM_FACE "monospace"
#define XL_XTERM_FACE_SIZE "10"

// XSETTINGS (freedesktop's XSETTINGS specification): setting types.
#define XL_XSETTINGS_INT 0

@interface XLScaleSettings () <RunLoopEvents>
@end

@implementation XLScaleSettings {
    xcb_connection_t *_c;
    xcb_window_t _root;
    xcb_window_t _win;           // the XSETTINGS owner window
    int _fd;
    BOOL _ownXSettings;
    uint32_t _serial;
    xcb_atom_t _SCALE, _XSETTINGS_S0, _XSETTINGS_SETTINGS, _MANAGER;
}

@synthesize scale = _scale;

- (instancetype)init
{
    if ((self = [super init]) != nil)
        _scale = 1;
    return self;
}

- (xcb_atom_t)atom:(const char *)name
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(_c,
        xcb_intern_atom(_c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r != NULL ? r->atom : XCB_ATOM_NONE;

    free(r);
    return a;
}

// Xnextbsd's _XNEXTBSD_SCALE, 1 without one (an older Xnextbsd).
- (NSUInteger)readScale
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(_c,
        xcb_get_property(_c, 0, _root, _SCALE, XCB_ATOM_CARDINAL, 0, 1),
        NULL);
    NSUInteger s = 1;

    if (r != NULL && r->format == 32 && xcb_get_property_value_length(r) >= 4) {
        uint32_t v = *(uint32_t *)xcb_get_property_value(r);

        if (v >= 1 && v <= 4)
            s = v;
    }
    free(r);
    return s;
}

#pragma mark - What X programs get

- (NSString *)resourceLines
{
    NSString *lines = [NSString stringWithFormat:
        @"Xft.dpi: %lu\nXcursor.size: %lu\n",
        (unsigned long)(96 * _scale), (unsigned long)(24 * _scale)];

    if (_scale > 1)
        lines = [lines stringByAppendingString:
                           @"XTerm*faceName: " XL_XTERM_FACE "\n"
                           @"XTerm*faceSize: " XL_XTERM_FACE_SIZE "\n"];
    return lines;
}

//  Each toolkit is told the scale once, so that nothing ends up twice
//  (4x) or not at all (1x) as large as on a 1x screen:
//
//  - GTK 3 (and Firefox, a GTK 3 program): GDK_SCALE=s fixes the window
//    scale, which also makes GTK ignore XSETTINGS' Gdk/UnscaledDPI and
//    take its font resolution from Xft/DPI, 96 * s, which would scale
//    text a second time. GDK_DPI_SCALE=1/s brings that back to 96 dpi.
//    (Without our XSETTINGS GTK's resolution is unset, 96, and it doesn't
//    apply GDK_DPI_SCALE to that, so it comes out the same.) Firefox takes
//    its device pixels per CSS pixel from the GDK scale (s) and its text
//    scale from GDK's resolution over 96 (1): exactly s.
//  - Qt 5.14 and later, Qt 6: they read Xft.dpi (96 * s) and would make
//    it a scale of s themselves; QT_SCREEN_SCALE_FACTORS=s sets the
//    screen's factor in its place (not on top of it, as QT_SCALE_FACTOR
//    would), and Qt then divides Xft.dpi by it for fonts: 96 dpi at a
//    scale of s. QT_ENABLE_HIGHDPI_SCALING=1 turns scaling on in Qt 5.14
//    and 5.15, where it is off by default (Qt 6 has it on).
//  - Xlib and Xt programs (xterm with Xft fonts, Xaw, Motif): Xft.dpi in
//    RESOURCE_MANAGER, 96 * s, so a 10 point font is s times as many
//    pixels. Core bitmap fonts aren't scaled: at 2x and more xterm gets
//    a 10 point monospace Xft face (XTerm*faceName, XTerm*faceSize)
//    instead of its core 6x13.
//  - Cursors: XCURSOR_SIZE (and Xcursor.size), 24 * s pixels, which
//    Xnextbsd shows at the scale (24 points). GTK and Qt load them at
//    that size without multiplying it again.
//
//  At 1x (an ordinary screen, or XLibreLowResolution) all of these say 1x,
//  so nothing inherited from elsewhere scales X programs that
//  WindowServer is scaling already.
- (NSDictionary *)environment
{
    NSUInteger s = _scale ?: 1;

    return @{
        @"GDK_SCALE": [NSString stringWithFormat:@"%lu", (unsigned long)s],
        @"GDK_DPI_SCALE": s == 1 ? @"1" :
            [NSString stringWithFormat:@"%.6g", 1.0 / (double)s],
        @"QT_SCREEN_SCALE_FACTORS": [NSString stringWithFormat:@"%lu",
                                        (unsigned long)s],
        @"QT_ENABLE_HIGHDPI_SCALING": @"1",
        @"XCURSOR_SIZE": [NSString stringWithFormat:@"%lu",
                             (unsigned long)(24 * s)],
    };
}

#pragma mark - RESOURCE_MANAGER

// Our two resources in RESOURCE_MANAGER at the current scale, the rest
// left as they are (xrdb -merge style).
- (void)mergeResources
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(_c,
        xcb_get_property(_c, 0, _root, XCB_ATOM_RESOURCE_MANAGER,
                         XCB_ATOM_STRING, 0, 1 << 20), NULL);
    NSMutableString *res = [NSMutableString string];
    NSString *old = nil;
    NSData *data;

    if (r != NULL && r->format == 8 && xcb_get_property_value_length(r) > 0)
        old = [[NSString alloc] initWithBytes:xcb_get_property_value(r)
                                       length:xcb_get_property_value_length(r)
                                     encoding:NSUTF8StringEncoding];
    free(r);
    for (NSString *line in [old componentsSeparatedByString:@"\n"]) {
        NSRange colon = [line rangeOfString:@":"];
        NSString *key = colon.location == NSNotFound ? nil :
            [[line substringToIndex:colon.location]
                stringByTrimmingCharactersInSet:
                    [NSCharacterSet whitespaceCharacterSet]];

        NSString *value = colon.location == NSNotFound ? nil :
            [[line substringFromIndex:colon.location + 1]
                stringByTrimmingCharactersInSet:
                    [NSCharacterSet whitespaceCharacterSet]];

        // Ours: Xft.dpi, Xcursor.size, and xterm's face when it is the
        // one we set (not someone else's); also the 10x20 core font
        // earlier versions set.
        if ([line length] == 0 || [key isEqualToString:@"Xft.dpi"] ||
            [key isEqualToString:@"Xcursor.size"] ||
            ([key isEqualToString:@"XTerm*faceName"] &&
             [value isEqualToString:@XL_XTERM_FACE]) ||
            ([key isEqualToString:@"XTerm*faceSize"] &&
             [value isEqualToString:@XL_XTERM_FACE_SIZE]) ||
            ([key isEqualToString:@"XTerm*font"] &&
             [value isEqualToString:@"10x20"]))
            continue;
        [res appendFormat:@"%@\n", line];
    }
    [res appendString:[self resourceLines]];
    data = [res dataUsingEncoding:NSUTF8StringEncoding];
    xcb_change_property(_c, XCB_PROP_MODE_REPLACE, _root,
                        XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8,
                        (uint32_t)[data length], [data bytes]);
}

#pragma mark - XSETTINGS

static void
appendInt(NSMutableData *d, const char *name, int32_t value, uint32_t serial)
{
    uint8_t head[4] = { XL_XSETTINGS_INT, 0, 0, 0 };
    uint16_t len = (uint16_t)strlen(name);
    static const uint8_t pad[4];

    memcpy(head + 2, &len, 2);
    [d appendBytes:head length:4];
    [d appendBytes:name length:len];
    [d appendBytes:pad length:(4 - len % 4) % 4];
    [d appendBytes:&serial length:4];
    [d appendBytes:&value length:4];
}

// _XSETTINGS_SETTINGS on the owner window: the scale for GTK.
- (void)writeXSettings
{
    NSMutableData *d = [NSMutableData data];
    uint32_t n = 3, one = 1;
    uint8_t head[4] = { 0, 0, 0, 0 };
    int32_t s = (int32_t)(_scale ?: 1);

    if (!_ownXSettings)
        return;
    _serial++;
    // Byte order of what follows: ours (LSBFirst 0, MSBFirst 1).
    head[0] = *(uint8_t *)&one == 1 ? 0 : 1;
    [d appendBytes:head length:4];
    [d appendBytes:&_serial length:4];
    [d appendBytes:&n length:4];
    appendInt(d, "Gdk/WindowScalingFactor", s, _serial);
    appendInt(d, "Gdk/UnscaledDPI", 96 * 1024, _serial);
    appendInt(d, "Xft/DPI", 96 * 1024 * s, _serial);
    xcb_change_property(_c, XCB_PROP_MODE_REPLACE, _win, _XSETTINGS_SETTINGS,
                        _XSETTINGS_SETTINGS, 8, (uint32_t)[d length],
                        [d bytes]);
}

// Take _XSETTINGS_S0 unless another settings manager has it, and say so
// with a MANAGER message on the root window.
- (void)acquireXSettings
{
    xcb_get_selection_owner_reply_t *o;
    xcb_client_message_event_t ev;

    o = xcb_get_selection_owner_reply(_c,
        xcb_get_selection_owner(_c, _XSETTINGS_S0), NULL);
    if (o != NULL && o->owner != XCB_WINDOW_NONE) {
        NSLog(@"[XLibre] XSETTINGS: another settings manager runs; "
              "the scale is in RESOURCE_MANAGER and the environment only");
        free(o);
        return;
    }
    free(o);
    xcb_set_selection_owner(_c, _win, _XSETTINGS_S0, XCB_CURRENT_TIME);
    o = xcb_get_selection_owner_reply(_c,
        xcb_get_selection_owner(_c, _XSETTINGS_S0), NULL);
    _ownXSettings = o != NULL && o->owner == _win;
    free(o);
    if (!_ownXSettings)
        return;
    [self writeXSettings];

    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = _root;
    ev.type = _MANAGER;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = _XSETTINGS_S0;
    ev.data.data32[2] = _win;
    xcb_send_event(_c, 0, _root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   (const char *)&ev);
}

#pragma mark - Starting

- (BOOL)start
{
    xcb_screen_t *screen;
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    NSRunLoop *loop = [NSRunLoop currentRunLoop];

    _c = xcb_connect(NULL, NULL);
    if (_c == NULL || xcb_connection_has_error(_c)) {
        NSLog(@"[XLibre] scale: can't connect to the X server");
        if (_c != NULL)
            xcb_disconnect(_c);
        _c = NULL;
        return NO;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(_c)).data;
    _root = screen->root;
    _SCALE = [self atom:XL_SCALE_ATOM];
    _XSETTINGS_S0 = [self atom:"_XSETTINGS_S0"];
    _XSETTINGS_SETTINGS = [self atom:"_XSETTINGS_SETTINGS"];
    _MANAGER = [self atom:"MANAGER"];

    // The root's property changes: _XNEXTBSD_SCALE.
    xcb_change_window_attributes(_c, _root, XCB_CW_EVENT_MASK, &mask);
    _win = xcb_generate_id(_c);
    xcb_create_window(_c, XCB_COPY_FROM_PARENT, _win, _root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, NULL);

    _scale = [self readScale];
    [URSDecorationMetrics setScale:_scale];
    [self mergeResources];
    [self acquireXSettings];
    xcb_flush(_c);
    NSLog(@"[XLibre] X programs draw at %lux", (unsigned long)_scale);

    _fd = xcb_get_file_descriptor(_c);
    for (NSString *mode in @[NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
                             NSModalPanelRunLoopMode])
        [loop addEvent:(void *)(uintptr_t)_fd type:ET_RDESC watcher:self
               forMode:mode];
    return YES;
}

- (void)stop
{
    if (_c == NULL)
        return;
    for (NSString *mode in @[NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
                             NSModalPanelRunLoopMode])
        [[NSRunLoop currentRunLoop] removeEvent:(void *)(uintptr_t)_fd
                                           type:ET_RDESC
                                        forMode:mode
                                            all:YES];
    xcb_disconnect(_c);
    _c = NULL;
}

- (void)dealloc
{
    [self stop];
}

#pragma mark - Events

- (void)scaleMayHaveChanged
{
    NSUInteger s = [self readScale];

    if (s == _scale)
        return;
    NSLog(@"[XLibre] X programs draw at %lux now (were %lux)",
          (unsigned long)s, (unsigned long)_scale);
    _scale = s;
    [URSDecorationMetrics setScale:s];
    [self mergeResources];
    [self writeXSettings];
    xcb_flush(_c);
    [[NSNotificationCenter defaultCenter]
        postNotificationName:XLScaleDidChangeNotification object:self];
}

- (void)receivedEvent:(void *)data
                 type:(RunLoopEventType)type
                extra:(void *)extra
              forMode:(NSString *)mode
{
    xcb_generic_event_t *e;
    BOOL changed = NO;

    if (_c == NULL)
        return;
    while ((e = xcb_poll_for_event(_c)) != NULL) {
        uint8_t t = e->response_type & ~0x80;

        if (t == XCB_PROPERTY_NOTIFY &&
            ((xcb_property_notify_event_t *)e)->window == _root &&
            ((xcb_property_notify_event_t *)e)->atom == _SCALE)
            changed = YES;
        else if (t == XCB_SELECTION_CLEAR &&
                 ((xcb_selection_clear_event_t *)e)->selection ==
                     _XSETTINGS_S0)
            _ownXSettings = NO;     // another settings manager took over
        free(e);
    }
    if (xcb_connection_has_error(_c)) {
        [self stop];
        return;
    }
    if (changed)
        [self scaleMayHaveChanged];
    xcb_flush(_c);
}

@end
