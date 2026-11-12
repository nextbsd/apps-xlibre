//
//  XLPasteboardBridge.m
//  XLibre.app - X CLIPBOARD <-> GNUstep general pasteboard
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//

#import "XLPasteboardBridge.h"

#import <AppKit/AppKit.h>
#import <xcb/xcb.h>
#import <xcb/xfixes.h>
#import <stdlib.h>
#import <string.h>

// How often to look at the general pasteboard's change count. gpbs has
// no change notification, so the bridge polls, as xpbproxy does.
#define XL_PASTEBOARD_POLL 0.5

// An outgoing INCR transfer: one requestor property being fed in chunks.
@interface XLIncrTransfer : NSObject
@property (assign, nonatomic) xcb_window_t requestor;
@property (assign, nonatomic) xcb_atom_t property;
@property (assign, nonatomic) xcb_atom_t type;
@property (strong, nonatomic) NSData *data;
@property (assign, nonatomic) NSUInteger offset;
@end

@implementation XLIncrTransfer
@end

@interface XLPasteboardBridge () <RunLoopEvents>
@end

@implementation XLPasteboardBridge {
    xcb_connection_t *_c;
    xcb_window_t _win;
    int _fd;
    uint8_t _xfixesEvent;
    NSUInteger _chunk;           // INCR above this many bytes

    xcb_atom_t _CLIPBOARD, _UTF8_STRING, _STRING, _TEXT, _TARGETS, _INCR,
               _MULTIPLE, _TIMESTAMP, _PROP;

    // GNUstep -> X: the text we serve while we own CLIPBOARD.
    NSString *_owned;
    BOOL _ownClipboard;
    NSInteger _lastChangeCount;
    NSTimer *_timer;
    NSMutableArray *_transfers;

    // X -> GNUstep: the conversion in flight.
    xcb_atom_t _wantTarget;      // UTF8_STRING, then STRING
    BOOL _incoming;              // an INCR transfer to us is running
    NSMutableData *_incomingData;
}

#pragma mark - Setup

- (xcb_atom_t)atom:(const char *)name
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(_c,
        xcb_intern_atom(_c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r != NULL ? r->atom : XCB_NONE;

    free(r);
    return a;
}

- (BOOL)start
{
    const xcb_query_extension_reply_t *ext;
    xcb_xfixes_query_version_reply_t *ver;
    xcb_screen_t *screen;
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    NSRunLoop *loop = [NSRunLoop currentRunLoop];
    uint32_t maxreq;

    _c = xcb_connect(NULL, NULL);
    if (_c == NULL || xcb_connection_has_error(_c)) {
        NSLog(@"[XLibre] clipboard: can't connect to the X server");
        if (_c != NULL)
            xcb_disconnect(_c);
        _c = NULL;
        return NO;
    }
    ext = xcb_get_extension_data(_c, &xcb_xfixes_id);
    if (ext == NULL || !ext->present) {
        NSLog(@"[XLibre] clipboard: the X server has no XFIXES");
        [self stop];
        return NO;
    }
    _xfixesEvent = ext->first_event;
    ver = xcb_xfixes_query_version_reply(_c,
        xcb_xfixes_query_version(_c, 5, 0), NULL);
    free(ver);

    _CLIPBOARD = [self atom:"CLIPBOARD"];
    _UTF8_STRING = [self atom:"UTF8_STRING"];
    _STRING = XCB_ATOM_STRING;
    _TEXT = [self atom:"TEXT"];
    _TARGETS = [self atom:"TARGETS"];
    _INCR = [self atom:"INCR"];
    _MULTIPLE = [self atom:"MULTIPLE"];
    _TIMESTAMP = [self atom:"TIMESTAMP"];
    _PROP = [self atom:"_XLIBRE_SELECTION"];

    screen = xcb_setup_roots_iterator(xcb_get_setup(_c)).data;
    _win = xcb_generate_id(_c);
    xcb_create_window(_c, XCB_COPY_FROM_PARENT, _win, screen->root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    xcb_xfixes_select_selection_input(_c, _win, _CLIPBOARD,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
    xcb_flush(_c);

    // Leave room for the ChangeProperty request header.
    maxreq = xcb_get_maximum_request_length(_c) * 4;
    _chunk = maxreq > 65536 ? 65536 : (maxreq > 1024 ? maxreq - 256 : 768);

    _transfers = [NSMutableArray array];
    _lastChangeCount = -1;      // export what is on the pasteboard now

    _fd = xcb_get_file_descriptor(_c);
    for (NSString *mode in @[NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
                             NSModalPanelRunLoopMode])
        [loop addEvent:(void *)(uintptr_t)_fd type:ET_RDESC watcher:self
               forMode:mode];
    _timer = [NSTimer scheduledTimerWithTimeInterval:XL_PASTEBOARD_POLL
                                              target:self
                                            selector:@selector(pollPasteboard:)
                                            userInfo:nil
                                             repeats:YES];
    [self pollPasteboard:nil];
    return YES;
}

- (void)stop
{
    [_timer invalidate];
    _timer = nil;
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

#pragma mark - GNUstep -> X

- (void)pollPasteboard:(NSTimer *)timer
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSInteger count = [pb changeCount];
    NSString *s;

    if (_c == NULL || count == _lastChangeCount)
        return;
    _lastChangeCount = count;
    if ([pb availableTypeFromArray:@[NSStringPboardType]] == nil)
        return;
    s = [pb stringForType:NSStringPboardType];
    if (s == nil)
        return;
    _owned = [s copy];
    _ownClipboard = YES;
    xcb_set_selection_owner(_c, _win, _CLIPBOARD, XCB_CURRENT_TIME);
    xcb_flush(_c);
}

- (void)notify:(xcb_selection_request_event_t *)req property:(xcb_atom_t)prop
{
    xcb_selection_notify_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time = req->time;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.property = prop;
    xcb_send_event(_c, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT,
                   (const char *)&ev);
}

- (NSData *)dataForTarget:(xcb_atom_t)target type:(xcb_atom_t *)type
{
    if (target == _UTF8_STRING || target == _TEXT) {
        *type = _UTF8_STRING;
        return [_owned dataUsingEncoding:NSUTF8StringEncoding];
    }
    if (target == _STRING) {
        *type = _STRING;
        return [_owned dataUsingEncoding:NSISOLatin1StringEncoding
                    allowLossyConversion:YES];
    }
    return nil;
}

- (void)selectionRequest:(xcb_selection_request_event_t *)req
{
    xcb_atom_t prop = req->property != XCB_NONE ? req->property : req->target;
    xcb_atom_t type;
    NSData *data;

    if (req->selection != _CLIPBOARD || !_ownClipboard || _owned == nil) {
        [self notify:req property:XCB_NONE];
        return;
    }
    if (req->target == _TARGETS) {
        xcb_atom_t targets[] = { _TARGETS, _UTF8_STRING, _TEXT, _STRING };

        xcb_change_property(_c, XCB_PROP_MODE_REPLACE, req->requestor, prop,
                            XCB_ATOM_ATOM, 32, 4, targets);
        [self notify:req property:prop];
        return;
    }
    data = [self dataForTarget:req->target type:&type];
    if (data == nil) {
        // MULTIPLE, TIMESTAMP and anything else: refused.
        [self notify:req property:XCB_NONE];
        return;
    }
    if ([data length] > _chunk) {
        XLIncrTransfer *t = [[XLIncrTransfer alloc] init];
        uint32_t size = (uint32_t)[data length];
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

        t.requestor = req->requestor;
        t.property = prop;
        t.type = type;
        t.data = data;
        t.offset = 0;
        [_transfers addObject:t];
        // The requestor deletes the property to ask for each next chunk.
        xcb_change_window_attributes(_c, req->requestor, XCB_CW_EVENT_MASK,
                                     &mask);
        xcb_change_property(_c, XCB_PROP_MODE_REPLACE, req->requestor, prop,
                            _INCR, 32, 1, &size);
    } else {
        xcb_change_property(_c, XCB_PROP_MODE_REPLACE, req->requestor, prop,
                            type, 8, (uint32_t)[data length], [data bytes]);
    }
    [self notify:req property:prop];
}

// The requestor deleted an INCR property: send the next chunk, and an
// empty one at the end.
- (BOOL)incrPropertyDeleted:(xcb_property_notify_event_t *)ev
{
    XLIncrTransfer *found = nil;
    NSUInteger n;

    for (XLIncrTransfer *t in _transfers)
        if (t.requestor == ev->window && t.property == ev->atom) {
            found = t;
            break;
        }
    if (found == nil)
        return NO;
    n = MIN(_chunk, [found.data length] - found.offset);
    xcb_change_property(_c, XCB_PROP_MODE_REPLACE, found.requestor,
                        found.property, found.type, 8, (uint32_t)n,
                        (const uint8_t *)[found.data bytes] + found.offset);
    found.offset += n;
    if (n == 0)
        [_transfers removeObject:found];
    return YES;
}

#pragma mark - X -> GNUstep

- (void)convert:(xcb_atom_t)target time:(xcb_timestamp_t)time
{
    _wantTarget = target;
    xcb_convert_selection(_c, _win, _CLIPBOARD, target, _PROP, time);
}

- (void)clipboardOwnerChanged:(xcb_xfixes_selection_notify_event_t *)ev
{
    if (ev->selection != _CLIPBOARD || ev->owner == _win)
        return;
    _ownClipboard = NO;
    _owned = nil;
    if (ev->owner == XCB_NONE)
        return;                 // the owner went away; keep the pasteboard
    _incoming = NO;
    _incomingData = nil;
    [self convert:_UTF8_STRING time:ev->selection_timestamp];
}

- (void)putOnPasteboard:(NSData *)data type:(xcb_atom_t)type
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;

    s = [[NSString alloc] initWithData:data
                              encoding:type == _STRING
                                       ? NSISOLatin1StringEncoding
                                       : NSUTF8StringEncoding];
    if (s == nil)
        s = [[NSString alloc] initWithData:data
                                  encoding:NSISOLatin1StringEncoding];
    if (s == nil)
        return;
    [pb declareTypes:@[NSStringPboardType] owner:nil];
    [pb setString:s forType:NSStringPboardType];
    _lastChangeCount = [pb changeCount];
}

// Read and delete our property: its type and bytes.
- (NSData *)takeProperty:(xcb_atom_t *)type
{
    xcb_get_property_reply_t *r;
    NSData *data;

    r = xcb_get_property_reply(_c,
        xcb_get_property(_c, 1, _win, _PROP, XCB_GET_PROPERTY_TYPE_ANY,
                         0, UINT32_MAX / 4), NULL);
    if (r == NULL)
        return nil;
    *type = r->type;
    data = [NSData dataWithBytes:xcb_get_property_value(r)
                          length:(NSUInteger)xcb_get_property_value_length(r)];
    free(r);
    return data;
}

- (void)selectionNotify:(xcb_selection_notify_event_t *)ev
{
    xcb_atom_t type = XCB_NONE;
    NSData *data;

    if (ev->requestor != _win || ev->selection != _CLIPBOARD)
        return;
    if (ev->property == XCB_NONE) {
        if (ev->target == _UTF8_STRING)
            [self convert:_STRING time:ev->time];
        return;
    }
    data = [self takeProperty:&type];
    if (data == nil)
        return;
    if (type == _INCR) {
        // Deleting the property (done) asks the owner for the chunks.
        _incoming = YES;
        _incomingData = [NSMutableData data];
        return;
    }
    [self putOnPasteboard:data type:type];
}

- (void)propertyNotify:(xcb_property_notify_event_t *)ev
{
    xcb_atom_t type = XCB_NONE;
    NSData *data;

    if (ev->state == XCB_PROPERTY_DELETE) {
        if ([self incrPropertyDeleted:ev])
            xcb_flush(_c);
        return;
    }
    if (!_incoming || ev->window != _win || ev->atom != _PROP ||
        ev->state != XCB_PROPERTY_NEW_VALUE)
        return;
    data = [self takeProperty:&type];
    if (data == nil)
        return;
    if ([data length] > 0) {
        [_incomingData appendData:data];
        return;
    }
    _incoming = NO;
    [self putOnPasteboard:_incomingData
                     type:_wantTarget == _STRING ? _STRING : _UTF8_STRING];
    _incomingData = nil;
}

#pragma mark - Events

- (void)receivedEvent:(void *)data
                 type:(RunLoopEventType)type
                extra:(void *)extra
              forMode:(NSString *)mode
{
    xcb_generic_event_t *e;

    if (_c == NULL)
        return;
    while ((e = xcb_poll_for_event(_c)) != NULL) {
        uint8_t t = e->response_type & ~0x80;

        if (t == XCB_SELECTION_REQUEST)
            [self selectionRequest:(xcb_selection_request_event_t *)e];
        else if (t == XCB_SELECTION_NOTIFY)
            [self selectionNotify:(xcb_selection_notify_event_t *)e];
        else if (t == XCB_SELECTION_CLEAR) {
            if (((xcb_selection_clear_event_t *)e)->selection == _CLIPBOARD) {
                _ownClipboard = NO;
                _owned = nil;
            }
        } else if (t == XCB_PROPERTY_NOTIFY)
            [self propertyNotify:(xcb_property_notify_event_t *)e];
        else if (t == _xfixesEvent + XCB_XFIXES_SELECTION_NOTIFY)
            [self clipboardOwnerChanged:(xcb_xfixes_selection_notify_event_t *)e];
        free(e);
    }
    if (xcb_connection_has_error(_c)) {
        NSLog(@"[XLibre] clipboard: lost the X server");
        [self stop];
        return;
    }
    xcb_flush(_c);
}

@end
