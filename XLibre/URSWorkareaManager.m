//
//  URSWorkareaManager.m
//  uroswm - ICCCM/EWMH Strut and Workarea Management
//
//  Tracks _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL properties from dock windows,
//  calculates the usable workarea, and updates _NET_WORKAREA on the root window.
//  The workarea is also kept inside WindowServer's usable area (the screen
//  minus the Dock's strip), which Xnextbsd puts on the root window as
//  _XNEXTBSD_USABLE_AREA.
//

#import "URSWorkareaManager.h"
#import "XCBScreen.h"
#import "XCBWindow.h"
#import "EWMHService.h"
#import "XCBAtomService.h"

@interface URSWorkareaManager ()
@property (strong, nonatomic) NSMutableDictionary *windowStruts;
@property (assign, nonatomic) int32_t cachedWorkareaX;
@property (assign, nonatomic) int32_t cachedWorkareaY;
@property (assign, nonatomic) uint32_t cachedWorkareaWidth;
@property (assign, nonatomic) uint32_t cachedWorkareaHeight;
@property (assign, nonatomic) xcb_atom_t usableAreaAtom;
@property (assign, nonatomic) BOOL hasUsableArea;
@property (assign, nonatomic) int32_t usableX;
@property (assign, nonatomic) int32_t usableY;
@property (assign, nonatomic) uint32_t usableWidth;
@property (assign, nonatomic) uint32_t usableHeight;
@end

@implementation URSWorkareaManager

- (instancetype)initWithConnection:(XCBConnection *)aConnection
{
    self = [super init];
    if (!self) return nil;

    _connection = aConnection;
    _windowStruts = [[NSMutableDictionary alloc] init];
    _cachedWorkareaX = INT32_MIN;
    _cachedWorkareaY = INT32_MIN;
    _cachedWorkareaWidth = UINT32_MAX;
    _cachedWorkareaHeight = UINT32_MAX;
    [self refreshUsableArea];

    return self;
}

#pragma mark - WindowServer's usable area

// _XNEXTBSD_USABLE_AREA (NB_USABLE_AREA_ATOM in Xnextbsd/hw/nextbsd/nextbsd.h).
#define XL_USABLE_AREA_ATOM "_XNEXTBSD_USABLE_AREA"

- (BOOL)isUsableAreaPropertyEvent:(xcb_property_notify_event_t *)event
{
    return event != NULL && self.usableAreaAtom != XCB_ATOM_NONE &&
           event->atom == self.usableAreaAtom;
}

- (BOOL)refreshUsableArea
{
    xcb_connection_t *conn = [self.connection connection];
    XCBScreen *screen = [[self.connection screens] firstObject];
    BOOL had = self.hasUsableArea;
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    BOOL has = NO;

    if (conn == NULL || screen == nil)
        return NO;
    if (self.usableAreaAtom == XCB_ATOM_NONE) {
        xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(conn,
            xcb_intern_atom(conn, 0, (uint16_t)strlen(XL_USABLE_AREA_ATOM),
                            XL_USABLE_AREA_ATOM), NULL);
        if (a == NULL)
            return NO;
        self.usableAreaAtom = a->atom;
        free(a);
    }

    xcb_get_property_reply_t *r = xcb_get_property_reply(conn,
        xcb_get_property(conn, 0, [screen screen]->root, self.usableAreaAtom,
                         XCB_ATOM_CARDINAL, 0, 4), NULL);
    if (r != NULL) {
        if (r->format == 32 && xcb_get_property_value_length(r) >= 16) {
            uint32_t *v = (uint32_t *)xcb_get_property_value(r);
            x = (int32_t)v[0];
            y = (int32_t)v[1];
            w = v[2];
            h = v[3];
            has = w > 0 && h > 0;
        }
        free(r);
    }

    if (has == had && (!has || (x == self.usableX && y == self.usableY &&
                                w == self.usableWidth && h == self.usableHeight)))
        return NO;
    self.hasUsableArea = has;
    self.usableX = x;
    self.usableY = y;
    self.usableWidth = w;
    self.usableHeight = h;
    return YES;
}

// The screen minus the struts, inside the usable area when there is one.
- (void)computeWorkareaX:(int32_t *)ox y:(int32_t *)oy
                   width:(uint32_t *)ow height:(uint32_t *)oh
{
    XCBScreen *screen = [[self.connection screens] objectAtIndex:0];
    int64_t screenWidth  = [screen screen]->width_in_pixels;
    int64_t screenHeight = [screen screen]->height_in_pixels;
    uint32_t maxLeft = 0, maxRight = 0, maxTop = 0, maxBottom = 0;

    for (NSNumber *windowKey in self.windowStruts) {
        NSDictionary *strutData = [self.windowStruts objectForKey:windowKey];

        uint32_t left   = [[strutData objectForKey:@"left"]   unsignedIntValue];
        uint32_t right  = [[strutData objectForKey:@"right"]  unsignedIntValue];
        uint32_t top    = [[strutData objectForKey:@"top"]    unsignedIntValue];
        uint32_t bottom = [[strutData objectForKey:@"bottom"] unsignedIntValue];

        if (left   > maxLeft)   maxLeft = left;
        if (right  > maxRight)  maxRight = right;
        if (top    > maxTop)    maxTop = top;
        if (bottom > maxBottom) maxBottom = bottom;
    }

    int64_t x1 = maxLeft, y1 = maxTop;
    int64_t x2 = screenWidth - maxRight, y2 = screenHeight - maxBottom;

    if (self.hasUsableArea) {
        x1 = MAX(x1, (int64_t)self.usableX);
        y1 = MAX(y1, (int64_t)self.usableY);
        x2 = MIN(x2, (int64_t)self.usableX + self.usableWidth);
        y2 = MIN(y2, (int64_t)self.usableY + self.usableHeight);
    }
    *ox = (int32_t)x1;
    *oy = (int32_t)y1;
    *ow = x2 > x1 ? (uint32_t)(x2 - x1) : 0;
    *oh = y2 > y1 ? (uint32_t)(y2 - y1) : 0;
}

#pragma mark - Strut Property Changes

- (void)handleStrutPropertyChange:(xcb_property_notify_event_t *)event
{
    if (!event) return;

    XCBAtomService *atomService =
        [XCBAtomService sharedInstanceWithConnection:self.connection];
    EWMHService *ewmhService =
        [EWMHService sharedInstanceWithConnection:self.connection];

    NSString *atomName = [atomService atomNameFromAtom:event->atom];

    if (![atomName isEqualToString:[ewmhService EWMHWMStrut]] &&
        ![atomName isEqualToString:[ewmhService EWMHWMStrutPartial]]) {
        return;
    }

    BOOL needsRecalc = NO;
    if (event->state == XCB_PROPERTY_DELETE) {
        needsRecalc = [self removeStrutForWindow:event->window];
    } else {
        needsRecalc = [self readAndRegisterStrutForWindow:event->window];
    }

    if (needsRecalc) {
        [self recalculateWorkarea];
    }
}

#pragma mark - Strut Registration

- (BOOL)readAndRegisterStrutForWindow:(xcb_window_t)windowId
{
    EWMHService *ewmhService =
        [EWMHService sharedInstanceWithConnection:self.connection];
    NSNumber *key = @(windowId);
    NSDictionary *existingStrut = [self.windowStruts objectForKey:key];

    XCBWindow *window = [[XCBWindow alloc] initWithXCBWindow:windowId
                                               andConnection:self.connection];
    if (!window) {
        NSLog(@"[ICCCM] Cannot create window object for %u", windowId);
        return NO;
    }

    // Try _NET_WM_STRUT_PARTIAL first (more precise)
    uint32_t strutPartial[12] = {0};
    if ([ewmhService readStrutPartialForWindow:window strut:strutPartial]) {
        NSMutableDictionary *strutData = [NSMutableDictionary dictionary];
        [strutData setObject:@(strutPartial[0])  forKey:@"left"];
        [strutData setObject:@(strutPartial[1])  forKey:@"right"];
        [strutData setObject:@(strutPartial[2])  forKey:@"top"];
        [strutData setObject:@(strutPartial[3])  forKey:@"bottom"];
        [strutData setObject:@(strutPartial[4])  forKey:@"left_start_y"];
        [strutData setObject:@(strutPartial[5])  forKey:@"left_end_y"];
        [strutData setObject:@(strutPartial[6])  forKey:@"right_start_y"];
        [strutData setObject:@(strutPartial[7])  forKey:@"right_end_y"];
        [strutData setObject:@(strutPartial[8])  forKey:@"top_start_x"];
        [strutData setObject:@(strutPartial[9])  forKey:@"top_end_x"];
        [strutData setObject:@(strutPartial[10]) forKey:@"bottom_start_x"];
        [strutData setObject:@(strutPartial[11]) forKey:@"bottom_end_x"];
        [strutData setObject:@(YES)              forKey:@"isPartial"];

        if ([existingStrut isEqualToDictionary:strutData]) {
            return NO;
        }

        [self.windowStruts setObject:strutData forKey:key];
        //NSLog(@"[ICCCM] Registered strut partial for window %u: left=%u, right=%u, top=%u, bottom=%u",
        //      windowId, strutPartial[0], strutPartial[1], strutPartial[2], strutPartial[3]);
        return YES;
    }

    // Fallback to _NET_WM_STRUT
    uint32_t strut[4] = {0};
    if ([ewmhService readStrutForWindow:window strut:strut]) {
        NSMutableDictionary *strutData = [NSMutableDictionary dictionary];
        [strutData setObject:@(strut[0]) forKey:@"left"];
        [strutData setObject:@(strut[1]) forKey:@"right"];
        [strutData setObject:@(strut[2]) forKey:@"top"];
        [strutData setObject:@(strut[3]) forKey:@"bottom"];
        [strutData setObject:@(NO)       forKey:@"isPartial"];

        if ([existingStrut isEqualToDictionary:strutData]) {
            return NO;
        }

        [self.windowStruts setObject:strutData forKey:key];
        //NSLog(@"[ICCCM] Registered strut for window %u: left=%u, right=%u, top=%u, bottom=%u",
        //      windowId, strut[0], strut[1], strut[2], strut[3]);
        return YES;
    }

    return NO;
}

- (BOOL)removeStrutForWindow:(xcb_window_t)windowId
{
    NSNumber *key = @(windowId);
    if ([self.windowStruts objectForKey:key]) {
        [self.windowStruts removeObjectForKey:key];
        //NSLog(@"[ICCCM] Removed strut for window %u", windowId);
        return YES;
    }
    return NO;
}

#pragma mark - Workarea Calculation

- (void)recalculateWorkarea
{
    @try {
        XCBScreen *screen = [[self.connection screens] objectAtIndex:0];
        XCBWindow *rootWindow = [screen rootWindow];
        int32_t workareaX, workareaY;
        uint32_t workareaWidth, workareaHeight;

        [self computeWorkareaX:&workareaX y:&workareaY
                         width:&workareaWidth height:&workareaHeight];

        if (workareaX     == self.cachedWorkareaX &&
            workareaY     == self.cachedWorkareaY &&
            workareaWidth == self.cachedWorkareaWidth &&
            workareaHeight == self.cachedWorkareaHeight) {
            return;
        }

        self.cachedWorkareaX      = workareaX;
        self.cachedWorkareaY      = workareaY;
        self.cachedWorkareaWidth  = workareaWidth;
        self.cachedWorkareaHeight = workareaHeight;

        EWMHService *ewmhService =
            [EWMHService sharedInstanceWithConnection:self.connection];
        [ewmhService updateWorkareaForRootWindow:rootWindow
                                               x:workareaX
                                               y:workareaY
                                           width:workareaWidth
                                          height:workareaHeight];
        // XCBConnection's copy of _NET_WORKAREA (placement, maximizing).
        self.connection.workareaValid = NO;

        [self.connection flush];

    } @catch (NSException *exception) {
        NSLog(@"[ICCCM] Exception recalculating workarea: %@", exception.reason);
    }
}

- (NSRect)currentWorkarea
{
    int32_t x, y;
    uint32_t w, h;

    [self computeWorkareaX:&x y:&y width:&w height:&h];
    return NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
}

@end
