//
//  XLPasteboardBridge.m
//  XLibre.app - X selections <-> GNUstep pasteboards
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

// How often to look at the pasteboards' change counts. gpbs has no change
// notification, so the bridge polls, as xpbproxy does.
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

// One X selection and the pasteboard it is bridged to: CLIPBOARD and the
// general pasteboard, PRIMARY and the "Selection" pasteboard.
@interface XLSelection : NSObject
@property (assign, nonatomic) xcb_atom_t atom;
@property (assign, nonatomic) xcb_atom_t property;  // ours, for conversions
@property (strong, nonatomic) NSPasteboard *pasteboard;
@property (assign, nonatomic) NSInteger lastChangeCount;

// GNUstep -> X: while we own the selection, target atom -> its bytes
// (and type atom, for TEXT).
@property (strong, nonatomic) NSMutableDictionary *owned;
@property (assign, nonatomic) BOOL ownSelection;

// X -> GNUstep: the targets still to convert, the one in flight and what
// came back (target atom -> data).
@property (strong, nonatomic) NSMutableArray *queue;
@property (assign, nonatomic) xcb_atom_t wantTarget;
@property (assign, nonatomic) xcb_timestamp_t time;
@property (strong, nonatomic) NSMutableDictionary *received;
@property (assign, nonatomic) BOOL incoming;        // an INCR transfer to us
@property (strong, nonatomic) NSMutableData *incomingData;
@end

@implementation XLSelection
@end

@interface XLPasteboardBridge () <RunLoopEvents>
@end

@implementation XLPasteboardBridge {
    xcb_connection_t *_c;
    xcb_window_t _win;
    int _fd;
    uint8_t _xfixesEvent;
    NSUInteger _chunk;           // INCR above this many bytes

    xcb_atom_t _UTF8_STRING, _STRING, _TEXT, _TARGETS, _INCR,
               _TEXT_PLAIN_UTF8, _TEXT_RTF, _APPLICATION_RTF, _TEXT_RICHTEXT,
               _IMAGE_PNG, _IMAGE_TIFF;

    NSArray *_selections;        // XLSelection
    NSTimer *_timer;
    NSMutableArray *_transfers;
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

- (XLSelection *)selection:(const char *)name property:(const char *)prop
                pasteboard:(NSPasteboard *)pb
{
    XLSelection *s = [[XLSelection alloc] init];

    s.atom = [self atom:name];
    s.property = [self atom:prop];
    s.pasteboard = pb;
    s.lastChangeCount = -1;      // export what is on the pasteboard now
    xcb_xfixes_select_selection_input(_c, _win, s.atom,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
    return s;
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

    _UTF8_STRING = [self atom:"UTF8_STRING"];
    _STRING = XCB_ATOM_STRING;
    _TEXT = [self atom:"TEXT"];
    _TARGETS = [self atom:"TARGETS"];
    _INCR = [self atom:"INCR"];
    _TEXT_PLAIN_UTF8 = [self atom:"text/plain;charset=utf-8"];
    _TEXT_RTF = [self atom:"text/rtf"];
    _APPLICATION_RTF = [self atom:"application/rtf"];
    _TEXT_RICHTEXT = [self atom:"text/richtext"];
    _IMAGE_PNG = [self atom:"image/png"];
    _IMAGE_TIFF = [self atom:"image/tiff"];

    screen = xcb_setup_roots_iterator(xcb_get_setup(_c)).data;
    _win = xcb_generate_id(_c);
    xcb_create_window(_c, XCB_COPY_FROM_PARENT, _win, screen->root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    _selections = @[
        [self selection:"CLIPBOARD" property:"_XLIBRE_CLIPBOARD"
             pasteboard:[NSPasteboard generalPasteboard]],
        [self selection:"PRIMARY" property:"_XLIBRE_PRIMARY"
             pasteboard:[NSPasteboard pasteboardWithName:@"Selection"]],
    ];
    xcb_flush(_c);

    // Leave room for the ChangeProperty request header.
    maxreq = xcb_get_maximum_request_length(_c) * 4;
    _chunk = maxreq > 65536 ? 65536 : (maxreq > 1024 ? maxreq - 256 : 768);

    _transfers = [NSMutableArray array];

    _fd = xcb_get_file_descriptor(_c);
    for (NSString *mode in @[NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
                             NSModalPanelRunLoopMode])
        [loop addEvent:(void *)(uintptr_t)_fd type:ET_RDESC watcher:self
               forMode:mode];
    _timer = [NSTimer scheduledTimerWithTimeInterval:XL_PASTEBOARD_POLL
                                              target:self
                                            selector:@selector(pollPasteboards:)
                                            userInfo:nil
                                             repeats:YES];
    [self pollPasteboards:nil];
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

- (XLSelection *)selectionForAtom:(xcb_atom_t)atom
{
    for (XLSelection *s in _selections)
        if (s.atom == atom)
            return s;
    return nil;
}

#pragma mark - Images

static NSData *
convertImage(NSData *data, NSBitmapImageFileType to)
{
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:data];

    if (rep == nil)
        return nil;
    if (to == NSTIFFFileType)
        return [rep TIFFRepresentation];
    return [rep representationUsingType:to properties:@{}];
}

#pragma mark - GNUstep -> X

// What a pasteboard holds, as X targets: text as UTF8_STRING, TEXT,
// text/plain;charset=utf-8 and STRING; RTF as text/rtf, application/rtf
// and text/richtext; TIFF and PNG images as image/tiff and image/png, each
// converted to the other.
- (NSMutableDictionary *)targetsFromPasteboard:(NSPasteboard *)pb
{
    NSMutableDictionary *t = [NSMutableDictionary dictionary];
    NSMutableArray *types = [NSMutableArray array];
    NSString *s = nil;
    NSData *d, *png = nil, *tiff = nil;

    // Asked one by one: see putOnPasteboard: about arrays and gpbs.
    for (NSString *type in @[NSStringPboardType, NSRTFPboardType,
                             NSPasteboardTypePNG, NSTIFFPboardType])
        if ([pb availableTypeFromArray:@[type]] != nil)
            [types addObject:type];
    if ([types containsObject:NSStringPboardType])
        s = [pb stringForType:NSStringPboardType];
    if (s != nil) {
        d = [s dataUsingEncoding:NSUTF8StringEncoding];
        t[@(_UTF8_STRING)] = d;
        t[@(_TEXT_PLAIN_UTF8)] = d;
        t[@(_STRING)] = [s dataUsingEncoding:NSISOLatin1StringEncoding
                        allowLossyConversion:YES];
    }
    if ([types containsObject:NSRTFPboardType] &&
        (d = [pb dataForType:NSRTFPboardType]) != nil) {
        // RTF alone: its plain text too, for clients that only take text.
        if (s == nil) {
            NSAttributedString *as = [[NSAttributedString alloc]
                                         initWithRTF:d
                                  documentAttributes:NULL];
            NSString *plain = [as string];

            if (plain != nil) {
                t[@(_UTF8_STRING)] =
                    [plain dataUsingEncoding:NSUTF8StringEncoding];
                t[@(_TEXT_PLAIN_UTF8)] = t[@(_UTF8_STRING)];
                t[@(_STRING)] =
                    [plain dataUsingEncoding:NSISOLatin1StringEncoding
                        allowLossyConversion:YES];
            }
        }
        t[@(_TEXT_RTF)] = d;
        t[@(_APPLICATION_RTF)] = d;
        t[@(_TEXT_RICHTEXT)] = d;
    }
    if ([types containsObject:NSPasteboardTypePNG])
        png = [pb dataForType:NSPasteboardTypePNG];
    if ([types containsObject:NSTIFFPboardType])
        tiff = [pb dataForType:NSTIFFPboardType];
    if (png == nil && tiff != nil)
        png = convertImage(tiff, NSPNGFileType);
    if (tiff == nil && png != nil)
        tiff = convertImage(png, NSTIFFFileType);
    if (png != nil)
        t[@(_IMAGE_PNG)] = png;
    if (tiff != nil)
        t[@(_IMAGE_TIFF)] = tiff;
    return t;
}

- (void)pollPasteboards:(NSTimer *)timer
{
    NSMutableDictionary *targets;
    NSInteger count;

    if (_c == NULL)
        return;
    for (XLSelection *sel in _selections) {
        count = [sel.pasteboard changeCount];
        if (count == sel.lastChangeCount)
            continue;
        sel.lastChangeCount = count;
        targets = [self targetsFromPasteboard:sel.pasteboard];
        if ([targets count] == 0)
            continue;
        sel.owned = targets;
        sel.ownSelection = YES;
        xcb_set_selection_owner(_c, _win, sel.atom, XCB_CURRENT_TIME);
    }
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

- (void)selectionRequest:(xcb_selection_request_event_t *)req
{
    XLSelection *sel = [self selectionForAtom:req->selection];
    xcb_atom_t prop = req->property != XCB_NONE ? req->property : req->target;
    xcb_atom_t type = req->target;
    NSData *data;

    if (sel == nil || !sel.ownSelection || sel.owned == nil) {
        [self notify:req property:XCB_NONE];
        return;
    }
    if (req->target == _TARGETS) {
        NSArray *keys = [sel.owned allKeys];
        xcb_atom_t targets[[keys count] + 2];
        uint32_t n = 0;

        targets[n++] = _TARGETS;
        for (NSNumber *k in keys)
            targets[n++] = (xcb_atom_t)[k unsignedIntValue];
        if (sel.owned[@(_UTF8_STRING)] != nil)
            targets[n++] = _TEXT;
        xcb_change_property(_c, XCB_PROP_MODE_REPLACE, req->requestor, prop,
                            XCB_ATOM_ATOM, 32, n, targets);
        [self notify:req property:prop];
        return;
    }
    if (req->target == _TEXT) {
        data = sel.owned[@(_UTF8_STRING)];
        type = _UTF8_STRING;
    } else {
        data = sel.owned[@(req->target)];
    }
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

// Ask the owner for the next target in the queue, or publish what came
// back when there is none left.
- (void)convertNext:(XLSelection *)sel
{
    if ([sel.queue count] == 0) {
        sel.wantTarget = XCB_NONE;
        [self putOnPasteboard:sel];
        return;
    }
    sel.wantTarget = (xcb_atom_t)[sel.queue[0] unsignedIntValue];
    [sel.queue removeObjectAtIndex:0];
    sel.incoming = NO;
    sel.incomingData = nil;
    xcb_convert_selection(_c, _win, sel.atom, sel.wantTarget, sel.property,
                          sel.time);
}

- (void)ownerChanged:(xcb_xfixes_selection_notify_event_t *)ev
{
    XLSelection *sel = [self selectionForAtom:ev->selection];

    if (sel == nil || ev->owner == _win)
        return;
    sel.ownSelection = NO;
    sel.owned = nil;
    sel.queue = nil;
    sel.wantTarget = XCB_NONE;
    sel.incoming = NO;
    sel.incomingData = nil;
    if (ev->owner == XCB_NONE)
        return;                 // the owner went away; keep the pasteboard
    // TARGETS first, to know what else to ask for.
    sel.time = ev->selection_timestamp;
    sel.received = [NSMutableDictionary dictionary];
    sel.queue = [NSMutableArray arrayWithObject:@(_TARGETS)];
    [self convertNext:sel];
}

// The owner's TARGETS (or none): queue text, RTF and one image, each in
// the form we like best.
- (void)queueFromTargets:(NSData *)data selection:(XLSelection *)sel
{
    const xcb_atom_t *a = [data bytes];
    NSUInteger i, n = data != nil ? [data length] / sizeof(xcb_atom_t) : 0;
    NSMutableSet *have = [NSMutableSet set];
    NSArray *groups = @[
        @[@(_UTF8_STRING), @(_TEXT_PLAIN_UTF8), @(_STRING)],
        @[@(_TEXT_RTF), @(_APPLICATION_RTF), @(_TEXT_RICHTEXT)],
        @[@(_IMAGE_PNG), @(_IMAGE_TIFF)],
    ];

    sel.queue = [NSMutableArray array];
    if (n == 0) {
        // No TARGETS: try text, as before.
        [sel.queue addObject:@(_UTF8_STRING)];
        [sel.queue addObject:@(_STRING)];
        return;
    }
    for (i = 0; i < n; i++)
        [have addObject:@(a[i])];
    for (NSArray *g in groups)
        for (NSNumber *t in g)
            if ([have containsObject:t]) {
                [sel.queue addObject:t];
                break;
            }
}

- (NSString *)stringFromData:(NSData *)data type:(xcb_atom_t)type
{
    NSString *s = [[NSString alloc] initWithData:data
                                        encoding:type == _STRING
                                                 ? NSISOLatin1StringEncoding
                                                 : NSUTF8StringEncoding];

    if (s == nil)
        s = [[NSString alloc] initWithData:data
                                  encoding:NSISOLatin1StringEncoding];
    return s;
}

// Everything the owner gave us goes on the pasteboard at once.
- (void)putOnPasteboard:(XLSelection *)sel
{
    NSPasteboard *pb = sel.pasteboard;
    NSMutableArray *types = [NSMutableArray array];
    NSString *string = nil;
    NSData *rtf = nil, *png = nil, *tiff = nil, *d;

    for (NSNumber *t in @[@(_UTF8_STRING), @(_TEXT_PLAIN_UTF8), @(_STRING)])
        if (string == nil && (d = sel.received[t]) != nil)
            string = [self stringFromData:d type:[t unsignedIntValue]];
    for (NSNumber *t in @[@(_TEXT_RTF), @(_APPLICATION_RTF),
                          @(_TEXT_RICHTEXT)])
        if (rtf == nil)
            rtf = sel.received[t];
    png = sel.received[@(_IMAGE_PNG)];
    tiff = sel.received[@(_IMAGE_TIFF)];
    if (png != nil && tiff == nil)
        tiff = convertImage(png, NSTIFFFileType);
    if (tiff != nil && png == nil)
        png = convertImage(tiff, NSPNGFileType);
    sel.received = nil;

    if (string != nil)
        [types addObject:NSStringPboardType];
    if (rtf != nil)
        [types addObject:NSRTFPboardType];
    if (tiff != nil)
        [types addObject:NSTIFFPboardType];
    if (png != nil)
        [types addObject:NSPasteboardTypePNG];
    if ([types count] == 0)
        return;
    // One type per message: gpbs gets only the first object of an array
    // by copy and every other one as a proxy into our process, which it
    // then crashes on (NextBSD's libs-base). So declare the first type
    // and add the others one by one.
    [pb declareTypes:@[types[0]] owner:nil];
    for (NSUInteger i = 1; i < [types count]; i++)
        [pb addTypes:@[types[i]] owner:nil];
    if (string != nil)
        [pb setString:string forType:NSStringPboardType];
    if (rtf != nil)
        [pb setData:rtf forType:NSRTFPboardType];
    if (tiff != nil)
        [pb setData:tiff forType:NSTIFFPboardType];
    if (png != nil)
        [pb setData:png forType:NSPasteboardTypePNG];
    sel.lastChangeCount = [pb changeCount];
}

// Read and delete one of our properties: its type and bytes.
- (NSData *)takeProperty:(xcb_atom_t)prop type:(xcb_atom_t *)type
{
    xcb_get_property_reply_t *r;
    NSData *data;

    r = xcb_get_property_reply(_c,
        xcb_get_property(_c, 1, _win, prop, XCB_GET_PROPERTY_TYPE_ANY,
                         0, UINT32_MAX / 4), NULL);
    if (r == NULL)
        return nil;
    *type = r->type;
    data = [NSData dataWithBytes:xcb_get_property_value(r)
                          length:(NSUInteger)xcb_get_property_value_length(r)];
    free(r);
    return data;
}

// One conversion is done (data nil: refused).
- (void)received:(NSData *)data selection:(XLSelection *)sel
{
    xcb_atom_t target = sel.wantTarget;

    if (target == _TARGETS) {
        [self queueFromTargets:data selection:sel];
    } else if (data != nil) {
        sel.received[@(target)] = data;
        // Got the text: the fallbacks for it aren't needed.
        if (target == _UTF8_STRING || target == _TEXT_PLAIN_UTF8)
            [sel.queue removeObject:@(_STRING)];
    }
    [self convertNext:sel];
}

- (void)selectionNotify:(xcb_selection_notify_event_t *)ev
{
    XLSelection *sel = [self selectionForAtom:ev->selection];
    xcb_atom_t type = XCB_NONE;
    NSData *data;

    if (sel == nil || ev->requestor != _win || sel.wantTarget == XCB_NONE ||
        ev->target != sel.wantTarget)
        return;
    if (ev->property == XCB_NONE) {
        [self received:nil selection:sel];
        return;
    }
    data = [self takeProperty:sel.property type:&type];
    if (data != nil && type == _INCR) {
        // Deleting the property (done) asks the owner for the chunks.
        sel.incoming = YES;
        sel.incomingData = [NSMutableData data];
        return;
    }
    [self received:data selection:sel];
}

- (void)propertyNotify:(xcb_property_notify_event_t *)ev
{
    xcb_atom_t type = XCB_NONE;
    XLSelection *sel = nil;
    NSData *data;

    if (ev->state == XCB_PROPERTY_DELETE) {
        if ([self incrPropertyDeleted:ev])
            xcb_flush(_c);
        return;
    }
    if (ev->window != _win || ev->state != XCB_PROPERTY_NEW_VALUE)
        return;
    for (XLSelection *s in _selections)
        if (s.property == ev->atom && s.incoming)
            sel = s;
    if (sel == nil)
        return;
    data = [self takeProperty:sel.property type:&type];
    if (data == nil)
        return;
    if ([data length] > 0) {
        [sel.incomingData appendData:data];
        return;
    }
    sel.incoming = NO;
    data = sel.incomingData;
    sel.incomingData = nil;
    [self received:data selection:sel];
}

#pragma mark - Events

- (void)receivedEvent:(void *)data
                 type:(RunLoopEventType)type
                extra:(void *)extra
              forMode:(NSString *)mode
{
    xcb_generic_event_t *e;
    XLSelection *sel;

    if (_c == NULL)
        return;
    while ((e = xcb_poll_for_event(_c)) != NULL) {
        uint8_t t = e->response_type & ~0x80;

        if (t == XCB_SELECTION_REQUEST)
            [self selectionRequest:(xcb_selection_request_event_t *)e];
        else if (t == XCB_SELECTION_NOTIFY)
            [self selectionNotify:(xcb_selection_notify_event_t *)e];
        else if (t == XCB_SELECTION_CLEAR) {
            sel = [self selectionForAtom:
                            ((xcb_selection_clear_event_t *)e)->selection];
            if (sel != nil) {
                sel.ownSelection = NO;
                sel.owned = nil;
            }
        } else if (t == XCB_PROPERTY_NOTIFY)
            [self propertyNotify:(xcb_property_notify_event_t *)e];
        else if (t == _xfixesEvent + XCB_XFIXES_SELECTION_NOTIFY)
            [self ownerChanged:(xcb_xfixes_selection_notify_event_t *)e];
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
