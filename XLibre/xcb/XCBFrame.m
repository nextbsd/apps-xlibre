//
//  XCBFrame.m
//  XCBKit
//
//  Created by Alessandro Sangiuliano on 05/08/19.
//  Copyright (c) 2019 alex. All rights reserved.
//

#import "XCBFrame.h"
#import "Transformers.h"
#import "ICCCMService.h"
#import "URSDecorationMetrics.h"
#include <xcb/shape.h>
#import "XCBTypes.h"
#import <AppKit/NSScroller.h>
#import <GNUstepGUI/GSTheme.h>
#import "URSDecorationMetrics.h"

// Protocol for compositor manager to check if compositing is active
@protocol URSCompositingManaging <NSObject>
+ (instancetype)sharedManager;
- (BOOL)compositingActive;
@end

// Helper function to send synthetic ConfigureNotify to client during resize
// Per ICCCM 4.1.5, reparented windows need synthetic ConfigureNotify events.
// The x/y coordinates in ConfigureNotify are relative to the window's parent,
// so child windows must receive parent-relative positions, not root coordinates.
static void sendSyntheticConfigureNotify(xcb_connection_t *conn,
                                          XCBWindow *clientWindow,
                                          int16_t relX,
                                          int16_t relY,
                                          uint16_t width,
                                          uint16_t height)
{
    xcb_configure_notify_event_t event;
    memset(&event, 0, sizeof(event));
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.event = [clientWindow window];
    event.window = [clientWindow window];
    event.x = relX;
    event.y = relY;
    event.width = width;
    event.height = height;
    event.border_width = 0;
    event.above_sibling = XCB_NONE;
    event.override_redirect = 0;

    xcb_send_event(conn, 0, [clientWindow window],
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&event);
}

// Find 32-bit ARGB visual for alpha transparency support
// Returns visual ID and fills in visualType if found
static xcb_visualid_t findARGBVisual(xcb_screen_t *screen, xcb_visualtype_t **outVisualType) {
    if (!screen) return 0;

    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        if (depth_iter.data->depth != 32) continue;

        xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
            xcb_visualtype_t *visual = visual_iter.data;

            // Look for TrueColor with 8-bit alpha channel
            if (visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                if (outVisualType) *outVisualType = visual;
                return visual->visual_id;
            }
        }
    }

    return 0;
}

@implementation XCBFrame

@synthesize minWidthHint;
@synthesize minHeightHint;
@synthesize connection;
@synthesize rightBorderClicked;
@synthesize bottomBorderClicked;
@synthesize offset;
@synthesize leftBorderClicked;
@synthesize topBorderClicked;
@synthesize titleHeight;

- (id) initWithClientWindow:(XCBWindow *)aClientWindow withConnection:(XCBConnection *)aConnection
{
    return [self initWithClientWindow:aClientWindow
                       withConnection:aConnection
                        withXcbWindow:0
                             withRect:XCBInvalidRect];
}

- (id) initWithClientWindow:(XCBWindow *)aClientWindow
             withConnection:(XCBConnection *)aConnection
              withXcbWindow:(xcb_window_t)xcbWindow
                   withRect:(XCBRect)aRect
{
    self = [super initWithXCBWindow: xcbWindow andConnection:aConnection];
    [self setWindowRect:aRect];
    [self setOriginalRect:aRect];
    /*** checks normal hints for client window **/
    [connection setIsWindowsMapUpdated:NO];
    
    ICCCMService* icccmService = [ICCCMService sharedInstanceWithConnection:connection];
    xcb_size_hints_t *sizeHints = [icccmService wmNormalHintsForWindow:aClientWindow];

    [self setMinHeightHint:sizeHints->min_height];
    [self setMinWidthHint:sizeHints->min_width];

    // Enforce an absolute minimum client area so windows can never collapse
    // to just the titlebar height. Clients that don't set WM_NORMAL_HINTS
    // get minHeightHint=0 which previously caused uint32_t underflows
    // in the resize functions and allowed 0-height client areas.
    if (minHeightHint < WM_MIN_CLIENT_HEIGHT)
        minHeightHint = WM_MIN_CLIENT_HEIGHT;
    if (minWidthHint < WM_MIN_CLIENT_WIDTH)
        minWidthHint = WM_MIN_CLIENT_WIDTH;

    // Respect ICCCM WM_NORMAL_HINTS: if min == max for both dimensions, treat as non-resizable
    if ((sizeHints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
        (sizeHints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
        sizeHints->min_width == sizeHints->max_width &&
        sizeHints->min_height == sizeHints->max_height)
    {
        //NSLog(@"[XCBFrame] Detected fixed-size (non-resizable) client window %u (min==max)", [aClientWindow window]);
        // Disable resizing for the client window so WM won't offer resize handles etc.
        [aClientWindow setCanResize:NO];
    }

    // Decoration geometry comes from the theme and the client's style
    {
        BOOL edited = NO;
        uint16_t l, r, t, b;
        self.decorationStyleMask = [XCBFrame decorationStyleMaskForClient:aClientWindow
                                                               connection:aConnection
                                                           documentEdited:&edited];
        self.documentEdited = edited;
        [URSDecorationMetrics offsetsForStyleMask:self.decorationStyleMask
                                             left:&l right:&r top:&t bottom:&b];
        titleHeight = t;
        self.clientBorder = l;
        self.bottomBorder = b;
    }

    if (minWidthHint > [aClientWindow windowRect].size.width)
    {
        XCBRect rect = XCBMakeRect(XCBMakePoint(0,0), XCBMakeSize(minWidthHint, [aClientWindow windowRect].size.height));
        [aClientWindow setWindowRect:rect];
        [aClientWindow setOriginalRect:rect];
        rect.size.width = rect.size.width + 2 * self.clientBorder;
        [self setWindowRect: rect];
        [self setOriginalRect:rect];
        uint32_t values[] = {rect.size.width};
        xcb_configure_window([aConnection connection], window, XCB_CONFIG_WINDOW_WIDTH, values);
        values[0] = minWidthHint;
        xcb_configure_window([aConnection connection], [aClientWindow window], XCB_CONFIG_WINDOW_WIDTH, values);
    }

    if (minHeightHint > [aClientWindow windowRect].size.height)
    {
        XCBRect rect = XCBMakeRect(XCBMakePoint(0,0), XCBMakeSize([aClientWindow windowRect].size.width, minHeightHint));
        [aClientWindow setWindowRect:rect];
        [aClientWindow setOriginalRect:rect];
        rect.size.height = rect.size.height + titleHeight + self.bottomBorder;
        [self setWindowRect:rect];
        [self setOriginalRect:rect];
        uint32_t values[] = {rect.size.height};
        xcb_configure_window([aConnection connection], window, XCB_CONFIG_WINDOW_HEIGHT, values);
        values[0] = minHeightHint;
        xcb_configure_window([aConnection connection], [aClientWindow window], XCB_CONFIG_WINDOW_HEIGHT, values);
    }

    connection = aConnection;
    children = [[NSMutableDictionary alloc] init];
    NSNumber *key = [NSNumber numberWithInteger:ClientWindow];
    [children setObject:aClientWindow forKey: key];
    [connection registerWindow:self];

    [super setIsAbove:YES];
    free(sizeHints);
    icccmService = nil;
    key= nil;

    return self;
}

- (void) addChildWindow:(XCBWindow *)aChild withKey:(childrenMask) keyMask
{
    NSNumber* key = [NSNumber numberWithInteger:keyMask];
    [children setObject:aChild forKey: key];
    key = nil;
}

- (XCBWindow*) childWindowForKey:(childrenMask)key
{
    NSNumber* keyNumber = [NSNumber numberWithInteger:key];
    XCBWindow* child = [children objectForKey:keyNumber];
    keyNumber = nil;
    return child;
}

-(void)removeChild:(childrenMask)frameChild
{
    NSNumber* key = [NSNumber numberWithInteger:frameChild];
    [children removeObjectForKey:key];
    key = nil;
}

- (void) decorateClientWindow
{
    NSNumber* key = [NSNumber numberWithInteger:ClientWindow];
    XCBWindow *clientWindow = [children objectForKey:key];
    key = nil;

    XCBScreen *scr = [parentWindow screen];
    XCBVisual *rootVisual = [[XCBVisual alloc] initWithVisualId:[scr screen]->root_visual];
    [rootVisual setVisualTypeForScreen:scr];

    // Check if compositor is active for ARGB alpha transparency support
    Class compositorClass = NSClassFromString(@"URSCompositingManager");
    BOOL compositorActive = NO;
    if (compositorClass && [compositorClass respondsToSelector:@selector(sharedManager)]) {
        id manager = [compositorClass sharedManager];
        if ([manager respondsToSelector:@selector(compositingActive)]) {
            compositorActive = [manager compositingActive];
        }
    }


    uint32_t values[4];  // May need up to 4 values for ARGB (back_pixel, colormap, border_pixel, event_mask)
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint8_t depth = XCB_COPY_FROM_PARENT;
    XCBVisual *titlebarVisual = rootVisual;
    xcb_colormap_t argbColormap = XCB_NONE;

    values[0] = [scr screen]->white_pixel;
    values[1] = TITLE_MASK_VALUES;

    // If compositor is active, try to use 32-bit ARGB visual for alpha transparency
    if (compositorActive) {
        xcb_visualtype_t *argbVisualType = NULL;
        xcb_visualid_t argbVisualId = findARGBVisual([scr screen], &argbVisualType);

        if (argbVisualId != 0 && argbVisualType != NULL) {
            //NSLog(@"[XCBFrame] Creating titlebar with 32-bit ARGB visual (0x%x) for compositor alpha", argbVisualId);

            // Create colormap for ARGB visual (required for 32-bit windows)
            argbColormap = xcb_generate_id([connection connection]);
            xcb_create_colormap([connection connection],
                               XCB_COLORMAP_ALLOC_NONE,
                               argbColormap,
                               [scr screen]->root,
                               argbVisualId);

            // Set up ARGB visual
            titlebarVisual = [[XCBVisual alloc] initWithVisualId:argbVisualId];
            [titlebarVisual setVisualType:argbVisualType];
            depth = 32;

            // For 32-bit windows: back_pixel, border_pixel, event_mask, colormap
            // XCB_CW values must be in ascending bit order: 2, 8, 2048, 8192
            mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
            values[0] = 0;  // back_pixel = transparent black
            values[1] = 0;  // border_pixel = transparent
            values[2] = TITLE_MASK_VALUES;  // event_mask
            values[3] = argbColormap;  // colormap
        }
    }


    uint16_t height = titleHeight;

    XCBCreateWindowTypeRequest* request = [[XCBCreateWindowTypeRequest alloc] initForWindowType:XCBTitleBarRequest];
    [request setDepth:depth];
    [request setParentWindow:self];
    [request setXPosition:0];
    [request setYPosition:0];
    [request setWidth:[self windowRect].size.width];
    [request setHeight:height];
    [request setBorderWidth:0];
    [request setXcbClass:XCB_WINDOW_CLASS_INPUT_OUTPUT];
    [request setVisual:titlebarVisual];
    [request setValueMask:mask];
    [request setValueList:values];

    XCBWindowTypeResponse* response = [[super connection] createWindowForRequest:request registerWindow:YES];
    XCBTitleBar *titleBar = [response titleBar];

    // If using ARGB visual, configure titlebar for 32-bit pixmaps
    if (depth == 32 && argbColormap != XCB_NONE) {
        [titleBar setUse32BitDepth:YES];
        [titleBar setArgbVisualId:[titlebarVisual visualId]];
        //NSLog(@"[XCBFrame] Configured titlebar for 32-bit ARGB pixmaps");
    }

    [self addChildWindow:titleBar withKey:TitleBar];

    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];

    xcb_get_property_reply_t* reply = [ewmhService getProperty:[ewmhService EWMHWMName]
                              propertyType:XCB_GET_PROPERTY_TYPE_ANY
                                 forWindow:clientWindow
                                    delete:NO
                                    length:UINT32_MAX];

    NSString* windowTitle;
    if (reply)
    {
        char *value = xcb_get_property_value(reply);
        int len = xcb_get_property_value_length(reply);
            //NSLog(@"Window title: %s, len: %d", value, len);
        windowTitle = [NSString stringWithCString:value length:len];
    }

    // for now if it is nil just set an empty string

    if (windowTitle == nil)
    {
        ICCCMService* icccmService = [ICCCMService sharedInstanceWithConnection:connection];

        windowTitle = [icccmService getWmNameForWindow:clientWindow];

        if (windowTitle == nil)
            windowTitle = @"";

        icccmService = nil;
    }

    [titleBar onScreen];
    [titleBar updateAttributes];
    [titleBar setIsMapped:YES];
    
    // OPTIMIZATION: Only create pixmaps - skip legacy drawing if GSTheme will override
    // GSTheme integration in URSHybridEventHandler will render the titlebar contents
    // Creating pixmaps is still needed as GSTheme renders to them
    [titleBar createPixmap];
    
    // OPTIMIZATION: Skip button generation and legacy drawing when GSTheme is active
    // These operations are expensive and get completely overwritten by GSTheme
    
    [titleBar setIsAbove:YES];
    [clientWindow setDecorated:YES];
    [clientWindow setWindowBorderWidth:0];
    [connection mapWindow:titleBar];
    
    // Store title for later GSTheme rendering
    [titleBar setInternalTitle:windowTitle];

    // Position client window below titlebar, inset by the theme window border
    int cb = self.clientBorder;
    XCBPoint position = XCBMakePoint(cb, height);

    // When reparenting an already-mapped client the X server automatically unmaps then remaps
    // it.  This produces exactly two synthetic UnmapNotify events:
    //   1. event=root  (root's SubstructureNotify, from the implicit unmap)
    //   2. event=client (client's own StructureNotify, selected via CLIENT_SELECT_INPUT_EVENT_MASK)
    // Pre-arm the counter so handleUnMapNotify absorbs both without destroying the frame.
    if ([[clientWindow attributes] mapState] == XCB_MAP_STATE_VIEWABLE) {
        clientWindow.ignoreUnmapCount = 2;
    }

    [connection reparentWindow:clientWindow toWindow:self position:position];
    [connection mapWindow:clientWindow];
    uint32_t border[] = {0};
    // Ensure no borders on frame window
    xcb_configure_window([connection connection], window, XCB_CONFIG_WINDOW_BORDER_WIDTH, border);
    // Ensure no borders on client window
    xcb_configure_window([connection connection], [clientWindow window], XCB_CONFIG_WINDOW_BORDER_WIDTH, border);

    // Resize client to fill frame between titlebar and resize bar / border
    uint32_t clientSize[2] = {[self windowRect].size.width - 2 * (uint32_t)cb,
                              [self windowRect].size.height - height - (uint32_t)self.bottomBorder};
    xcb_configure_window([connection connection], [clientWindow window],
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, clientSize);
    
    // Flush to ensure reparent and map operations complete before continuing
    [connection flush];
    
    // Theme resize bar and its resize zones for resizable windows
    if ([URSDecorationMetrics hasResizeBarForStyleMask:self.decorationStyleMask]) {
        [self createResizeBarWithDepth:depth visual:titlebarVisual colormap:argbColormap];
        [self createResizeZonesFromTheme];
    }
    [self applyCornerShape];

    titleBar = nil;
    clientWindow = nil;
    ewmhService = nil;
    windowTitle = nil;
    scr = nil;
    rootVisual = nil;

    free(reply);
}

// Values from libs-back Headers/x11/XGServerWindow.h (GNUstepWMAttributes)
#define GS_WM_ATTR_WORDS        9
#define GS_WINDOW_STYLE_ATTR    (1 << 0)
#define GS_EXTRA_FLAGS_ATTR     (1 << 7)
#define GS_DOCUMENT_EDITED_FLAG (1 << 0)

+ (NSUInteger) decorationStyleMaskForClient:(XCBWindow*)clientWindow
                                 connection:(XCBConnection*)aConnection
                             documentEdited:(BOOL*)edited
{
    NSUInteger mask = 0;
    BOOL isEdited = NO;
    BOOL haveGNUstepStyle = NO;

    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:aConnection];
    xcb_atom_t attrAtom = [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService GNUStepWmAttr]];
    if (attrAtom != XCB_ATOM_NONE) {
        xcb_get_property_cookie_t cookie =
            xcb_get_property([aConnection connection], 0, [clientWindow window], attrAtom,
                             XCB_GET_PROPERTY_TYPE_ANY, 0, GS_WM_ATTR_WORDS);
        xcb_get_property_reply_t *reply =
            xcb_get_property_reply([aConnection connection], cookie, NULL);
        if (reply) {
            int words = (reply->format == 32) ? xcb_get_property_value_length(reply) / 4 : 0;
            uint32_t *attr = xcb_get_property_value(reply);
            if (words >= 2 && (attr[0] & GS_WINDOW_STYLE_ATTR)) {
                mask = attr[1] & URSDecorationStyleBits;
                haveGNUstepStyle = YES;
            }
            if (words >= GS_WM_ATTR_WORDS && (attr[0] & GS_EXTRA_FLAGS_ATTR))
                isEdited = (attr[8] & GS_DOCUMENT_EDITED_FLAG) != 0;
            free(reply);
        }
    }

    if (!haveGNUstepStyle) {
        // Plain X11 client: derive the style from ICCCM/EWMH hints
        mask = NSTitledWindowMask;
        ICCCMService *icccm = [ICCCMService sharedInstanceWithConnection:aConnection];
        if ([clientWindow canClose] &&
            [icccm hasProtocol:[icccm WMDeleteWindow] forWindow:clientWindow])
            mask |= NSClosableWindowMask;
        if ([clientWindow canMinimize])
            mask |= NSMiniaturizableWindowMask;

        BOOL fixedSize = NO;
        xcb_size_hints_t *hints = [icccm wmNormalHintsForWindow:clientWindow];
        if (hints) {
            fixedSize = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                        (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
                        hints->min_width == hints->max_width &&
                        hints->min_height == hints->max_height;
            free(hints);
        }
        if ([clientWindow canResize] && !fixedSize)
            mask |= NSResizableWindowMask;
    }

    if (!(mask & NSResizableWindowMask))
        [clientWindow setCanResize:NO];

    if (mask == 0)
        mask = NSTitledWindowMask;  // we only frame windows that want decorations

    if (edited)
        *edited = isEdited;
    return mask;
}

- (BOOL) updateDecorationStyleFromClient
{
    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    if (!clientWindow)
        return NO;

    BOOL edited = NO;
    NSUInteger mask = [XCBFrame decorationStyleMaskForClient:clientWindow
                                                  connection:connection
                                              documentEdited:&edited];

    // Geometry (resize bar, title bar) is fixed when the frame is created;
    // only the title bar contents follow later style changes.
    BOOL changed = (mask != self.decorationStyleMask) || (edited != self.documentEdited);
    self.decorationStyleMask = mask;
    self.documentEdited = edited;
    return changed;
}

/*** performance while resizing pixel by pixel is critical so we do everything we can to improve it also if the message signature looks bad ***/

- (void) resize:(xcb_motion_notify_event_t *)anEvent xcbConnection:(xcb_connection_t*)aXcbConnection
{
    int clientBorder = self.clientBorder;

    /*** width ***/

    if (rightBorderClicked && !bottomBorderClicked && !leftBorderClicked && !topBorderClicked)
    {
        resizeFromRightForEvent(anEvent, aXcbConnection, self, minWidthHint, clientBorder);
    }

    if (leftBorderClicked && !bottomBorderClicked && !rightBorderClicked && !topBorderClicked)
    {
        resizeFromLeftForEvent(anEvent, aXcbConnection, self, minWidthHint, clientBorder);
    }


    /** height **/

    if (bottomBorderClicked && !rightBorderClicked && !leftBorderClicked)
    {
        resizeFromBottomForEvent(anEvent, aXcbConnection, self, minHeightHint, titleHeight, clientBorder);
    }


    if (topBorderClicked && !rightBorderClicked && !leftBorderClicked && !bottomBorderClicked)
    {
        resizeFromTopForEvent(anEvent, aXcbConnection, self, minHeightHint, titleHeight, clientBorder);
    }


    /** width and height - corner resizes **/

    // SE corner (bottom-right)
    if (rightBorderClicked && bottomBorderClicked && !leftBorderClicked && !topBorderClicked)
    {
        resizeFromAngleForEvent(anEvent, aXcbConnection, self, minWidthHint, minHeightHint, titleHeight, clientBorder);
    }

    // NW corner (top-left) - combine top and left resizes
    if (topBorderClicked && leftBorderClicked && !rightBorderClicked && !bottomBorderClicked)
    {
        resizeFromTopForEvent(anEvent, aXcbConnection, self, minHeightHint, titleHeight, clientBorder);
        resizeFromLeftForEvent(anEvent, aXcbConnection, self, minWidthHint, clientBorder);
    }

    // NE corner (top-right) - combine top and right resizes
    if (topBorderClicked && rightBorderClicked && !leftBorderClicked && !bottomBorderClicked)
    {
        resizeFromTopForEvent(anEvent, aXcbConnection, self, minHeightHint, titleHeight, clientBorder);
        resizeFromRightForEvent(anEvent, aXcbConnection, self, minWidthHint, clientBorder);
    }

    // SW corner (bottom-left) - combine bottom and left resizes
    if (bottomBorderClicked && leftBorderClicked && !rightBorderClicked && !topBorderClicked)
    {
        resizeFromBottomForEvent(anEvent, aXcbConnection, self, minHeightHint, titleHeight, clientBorder);
        resizeFromLeftForEvent(anEvent, aXcbConnection, self, minWidthHint, clientBorder);
    }

    // Resize zones and shape mask are updated at button release (handleButtonRelease),
    // not on every motion event.  This keeps the hot path to the minimum 2-3 async
    // xcb_configure_window calls needed to move/resize the actual windows.

}

- (void)raiseResizeHandle
{
    childrenMask zones[] = {ResizeZoneSW, ResizeZoneS, ResizeZoneSE};
    for (int i = 0; i < sizeof(zones)/sizeof(zones[0]); i++) {
        XCBWindow *zone = [self childWindowForKey:zones[i]];
        if (zone) {
            [zone stackAbove];
        }
    }
}

#pragma mark - Resize Bar

- (void)createResizeBarWithDepth:(uint8_t)depth
                          visual:(XCBVisual *)visual
                        colormap:(xcb_colormap_t)colormap
{
    XCBRect frameRect = [self windowRect];
    uint16_t barHeight = (uint16_t)self.bottomBorder;
    xcb_window_t barWindow = xcb_generate_id([connection connection]);

    uint32_t mask = XCB_CW_BACK_PIXEL;
    uint32_t values[3];
    int n = 0;
    values[n++] = [URSDecorationMetrics borderPixel];
    if (depth == 32 && colormap != XCB_NONE) {
        mask |= XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP;
        values[n++] = 0;
        values[n++] = colormap;
    }

    xcb_create_window([connection connection],
                      depth,
                      barWindow,
                      window,
                      0, (int16_t)(frameRect.size.height - barHeight),
                      frameRect.size.width, barHeight,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      visual ? [visual visualId] : XCB_COPY_FROM_PARENT,
                      mask,
                      values);

    XCBWindow *bar = [[XCBWindow alloc] initWithXCBWindow:barWindow andConnection:connection];
    [bar setParentWindow:self];
    [bar setWindowRect:XCBMakeRect(XCBMakePoint(0, frameRect.size.height - barHeight),
                                   XCBMakeSize(frameRect.size.width, barHeight))];
    if (depth == 32 && visual) {
        [bar setUse32BitDepth:YES];
        [bar setArgbVisualId:[visual visualId]];
    }
    [self addChildWindow:bar withKey:ResizeBar];
    xcb_map_window([connection connection], barWindow);

    [self renderResizeBar];
}

- (void)renderResizeBar
{
    XCBWindow *bar = [self childWindowForKey:ResizeBar];
    if (!bar)
        return;

    Class themeIntegration = NSClassFromString(@"URSThemeIntegration");
    if ([themeIntegration respondsToSelector:@selector(renderResizeBarForFrame:)] &&
        [themeIntegration performSelector:@selector(renderResizeBarForFrame:) withObject:self]) {
        [bar putWindowBackgroundWithPixmap:[bar pixmap]];
        [bar drawArea:XCBMakeRect(XCBMakePoint(0, 0), [bar windowRect].size)];
    }
}

#pragma mark - Theme Relayout

- (BOOL)relayoutForCurrentTheme
{
    uint16_t l, r, t, b;
    [URSDecorationMetrics offsetsForStyleMask:self.decorationStyleMask
                                         left:&l right:&r top:&t bottom:&b];
    int oldL = self.clientBorder, oldT = titleHeight, oldB = self.bottomBorder;
    if (l == oldL && t == oldT && b == oldB)
        return NO;

    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    XCBTitleBar *titleBar = (XCBTitleBar *)[self childWindowForKey:TitleBar];
    int dW = 2 * ((int)l - oldL);
    int dH = ((int)t - oldT) + ((int)b - oldB);

    titleHeight = t;
    self.clientBorder = l;
    self.bottomBorder = b;

    // A saved pre-maximize rect restores to the same content size
    XCBRect saved = [self oldRect];
    if (FnCheckXCBRectIsValid(saved) && saved.size.width > 0 && saved.size.height > 0) {
        saved.size.width = (uint16_t)MAX(1, (int)saved.size.width + dW);
        saved.size.height = (uint16_t)MAX(1, (int)saved.size.height + dH);
        [self setOldRect:saved];
    }

    if (!clientWindow || !titleBar)
        return YES;

    // Fullscreen hides the decorations; the stored offsets are used on restore
    if ([self fullScreen] && ![self isMaximized])
        return YES;

    xcb_connection_t *conn = [connection connection];
    XCBRect frameRect = [self windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    if ([self isMaximized]) {
        // Keep the frame filling the work area; the content absorbs the change
        clientRect.size.width = (uint16_t)MAX(1, (int)frameRect.size.width - 2 * (int)l);
        clientRect.size.height = (uint16_t)MAX(1, (int)frameRect.size.height - (int)t - (int)b);
    } else {
        // Keep the content size; the frame grows or shrinks around it
        frameRect.size.width = (uint16_t)(clientRect.size.width + 2 * l);
        frameRect.size.height = (uint16_t)(clientRect.size.height + t + b);
        uint32_t frameValues[2] = {frameRect.size.width, frameRect.size.height};
        xcb_configure_window(conn, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             frameValues);
        [self setWindowRect:frameRect];
        [self setOriginalRect:frameRect];
    }

    XCBRect titleRect = XCBMakeRect(XCBMakePoint(0, 0), XCBMakeSize(frameRect.size.width, t));
    uint32_t titleValues[4] = {0, 0, titleRect.size.width, titleRect.size.height};
    xcb_configure_window(conn, [titleBar window],
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, titleValues);
    [titleBar setWindowRect:titleRect];
    [titleBar setOriginalRect:titleRect];

    clientRect.position = XCBMakePoint(l, t);
    uint32_t clientValues[4] = {l, t, clientRect.size.width, clientRect.size.height};
    xcb_configure_window(conn, [clientWindow window],
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, clientValues);
    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Tell the client where its content now is (root coordinates) and publish
    // the new frame extents
    sendSyntheticConfigureNotify(conn, clientWindow,
                                 frameRect.position.x + l, frameRect.position.y + t,
                                 clientRect.size.width, clientRect.size.height);
    [[EWMHService sharedInstanceWithConnection:connection] updateNetFrameExtentsForWindow:clientWindow];

    [self updateAllResizeZonePositions];
    return YES;
}

#pragma mark - Corner Shape

- (void)applyCornerShape
{
    xcb_connection_t *conn = [connection connection];
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_shape_id);
    if (!ext || !ext->present)
        return;

    Class compositorClass = NSClassFromString(@"URSCompositingManager");
    BOOL compositorActive = [[compositorClass sharedManager] compositingActive];

    int top = [URSDecorationMetrics topCornerRadiusForStyleMask:self.decorationStyleMask];
    int bottom = [URSDecorationMetrics bottomCornerRadiusForStyleMask:self.decorationStyleMask];
    XCBRect frameRect = [self windowRect];
    int w = frameRect.size.width, h = frameRect.size.height;

    if (compositorActive || (top == 0 && bottom == 0) || w <= 2 * MAX(top, bottom) ||
        h <= top + bottom) {
        // Rectangular: remove any mask left from a previous theme
        if (self.hasCornerShape) {
            xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, window, 0, 0, XCB_NONE);
            self.hasCornerShape = NO;
        }
        return;
    }

    xcb_pixmap_t mask = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 1, mask, window, w, h);
    xcb_gcontext_t clear = xcb_generate_id(conn);
    xcb_gcontext_t fill = xcb_generate_id(conn);
    xcb_create_gc(conn, clear, mask, XCB_GC_FOREGROUND, (uint32_t[]){0});
    xcb_create_gc(conn, fill, mask, XCB_GC_FOREGROUND, (uint32_t[]){1});

    xcb_rectangle_t all = {0, 0, w, h};
    xcb_poly_fill_rectangle(conn, mask, clear, 1, &all);

    xcb_rectangle_t body[] = {
        {0, top, w, h - top - bottom},          // between the corners
        {top, 0, w - 2 * top, top},             // top edge
        {bottom, h - bottom, w - 2 * bottom, bottom} // bottom edge
    };
    xcb_poly_fill_rectangle(conn, mask, fill, 3, body);

    xcb_arc_t arcs[4];
    int n = 0;
    if (top > 0) {
        arcs[n++] = (xcb_arc_t){0, 0, 2 * top, 2 * top, 0, 360 << 6};
        arcs[n++] = (xcb_arc_t){w - 2 * top, 0, 2 * top, 2 * top, 0, 360 << 6};
    }
    if (bottom > 0) {
        arcs[n++] = (xcb_arc_t){0, h - 2 * bottom, 2 * bottom, 2 * bottom, 0, 360 << 6};
        arcs[n++] = (xcb_arc_t){w - 2 * bottom, h - 2 * bottom, 2 * bottom, 2 * bottom, 0, 360 << 6};
    }
    if (n > 0)
        xcb_poly_fill_arc(conn, mask, fill, n, arcs);

    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, window, 0, 0, mask);
    self.hasCornerShape = YES;

    xcb_free_gc(conn, clear);
    xcb_free_gc(conn, fill);
    xcb_free_pixmap(conn, mask);
}

#pragma mark - Resize Zones

// Zones cover the resize bar the way GNUstep's resize bar handles clicks:
// the notched ends resize diagonally, the middle resizes vertically.
- (void)resizeZoneRects:(XCBRect *)rects
{
    XCBRect frameRect = [self windowRect];
    CGFloat w = frameRect.size.width;
    CGFloat h = frameRect.size.height;
    CGFloat barHeight = self.bottomBorder;
    CGFloat notch = [URSDecorationMetrics resizebarNotchWidth];
    if (notch * 2 > w)
        notch = floor(w / 2);

    rects[0] = XCBMakeRect(XCBMakePoint(0, h - barHeight), XCBMakeSize(notch, barHeight));
    rects[1] = XCBMakeRect(XCBMakePoint(notch, h - barHeight), XCBMakeSize(w - 2 * notch, barHeight));
    rects[2] = XCBMakeRect(XCBMakePoint(w - notch, h - barHeight), XCBMakeSize(notch, barHeight));
}

- (void)createResizeZonesFromTheme
{
    if (![URSDecorationMetrics hasResizeBarForStyleMask:self.decorationStyleMask])
        return;

    XCBRect rects[3];
    [self resizeZoneRects:rects];
    MousePosition positions[3] = {BottomLeftCorner, BottomBorder, BottomRightCorner};
    childrenMask keys[3] = {ResizeZoneSW, ResizeZoneS, ResizeZoneSE};

    for (int i = 0; i < 3; i++) {
        if ([self childWindowForKey:keys[i]])
            continue;
        [self createResizeZoneAtX:rects[i].position.x y:rects[i].position.y
                            width:rects[i].size.width height:rects[i].size.height
                         position:positions[i]
                              key:keys[i]];
    }

    [connection flush];
}

- (void)createResizeZoneAtX:(CGFloat)x y:(CGFloat)y
                      width:(CGFloat)width height:(CGFloat)height
                   position:(MousePosition)position
                        key:(childrenMask)key
{
    // Skip if zone would have invalid dimensions
    if (width <= 0 || height <= 0) {
        return;
    }

    xcb_window_t zoneWindow = xcb_generate_id([connection connection]);

    // Invisible INPUT_ONLY window - captures mouse events only
    uint32_t mask = XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
    uint32_t values[2];

    // Get appropriate resize cursor for this position without mutating selection state
    xcb_cursor_t resizeCursor = [[self cursor] cursorIdForPosition:position];

    values[0] = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;
    values[1] = resizeCursor;

    xcb_create_window([connection connection],
                      XCB_COPY_FROM_PARENT,
                      zoneWindow,
                      window, // Parent is the frame
                      (int16_t)x, (int16_t)y,
                      (uint16_t)width, (uint16_t)height,
                      0, // no border
                      XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT,
                      mask,
                      values);

    // Create XCBWindow wrapper and register it
    XCBWindow *resizeZone = [[XCBWindow alloc] initWithXCBWindow:zoneWindow andConnection:connection];
    [resizeZone setParentWindow:self];
    [self addChildWindow:resizeZone withKey:key];
    [connection registerWindow:resizeZone];

    // Map the resize zone
    xcb_map_window([connection connection], zoneWindow);

    // Raise above siblings
    [resizeZone stackAbove];
}

- (void)updateAllResizeZonePositions
{
    XCBWindow *bar = [self childWindowForKey:ResizeBar];
    if (bar) {
        XCBRect frameRect = [self windowRect];
        XCBRect barRect = XCBMakeRect(XCBMakePoint(0, frameRect.size.height - self.bottomBorder),
                                      XCBMakeSize(frameRect.size.width, self.bottomBorder));
        XCBRect oldRect = [bar windowRect];
        if (oldRect.position.y != barRect.position.y || oldRect.size.width != barRect.size.width ||
            oldRect.size.height != barRect.size.height) {
            uint32_t values[3] = {(uint32_t)barRect.position.y, barRect.size.width, barRect.size.height};
            xcb_configure_window([connection connection], [bar window],
                                 XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                                 values);
            [bar setWindowRect:barRect];
        }
        if ([bar pixmapSize].width != barRect.size.width ||
            [bar pixmapSize].height != barRect.size.height)
            [self renderResizeBar];
    }

    [self applyCornerShape];

    XCBRect rects[3];
    [self resizeZoneRects:rects];
    childrenMask keys[3] = {ResizeZoneSW, ResizeZoneS, ResizeZoneSE};
    for (int i = 0; i < 3; i++) {
        [self updateResizeZone:keys[i]
                           toX:rects[i].position.x y:rects[i].position.y
                         width:rects[i].size.width height:rects[i].size.height];
    }
}

- (void)updateResizeZone:(childrenMask)key toX:(CGFloat)x y:(CGFloat)y width:(CGFloat)width height:(CGFloat)height
{
    XCBWindow *zone = [self childWindowForKey:key];
    if (zone && width > 0 && height > 0) {
        uint32_t values[5] = {(uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height, XCB_STACK_MODE_ABOVE};
        xcb_configure_window([connection connection],
                           [zone window],
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                           XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                           XCB_CONFIG_WINDOW_STACK_MODE,
                           values);
    }
}

- (void)destroyResizeZones
{
    childrenMask zones[] = {ResizeZoneNW, ResizeZoneN, ResizeZoneNE, ResizeZoneE,
                           ResizeZoneSE, ResizeZoneS, ResizeZoneSW, ResizeZoneW,
                           ResizeZoneGrowBox, ResizeHandle};

    for (int i = 0; i < sizeof(zones)/sizeof(zones[0]); i++) {
        XCBWindow *zone = [self childWindowForKey:zones[i]];
        if (zone) {
            xcb_destroy_window([connection connection], [zone window]);
            [connection unregisterWindow:zone];
            [self removeChild:zones[i]];
        }
    }
}

void resizeFromRightForEvent(xcb_motion_notify_event_t *anEvent,
                             xcb_connection_t *connection,
                             XCBFrame* frame,
                             int minW,
                             int cb)
{
    XCBWindow* clientWindow = [frame childWindowForKey:ClientWindow];
    // Respect ICCCM: if client is non-resizable, ignore interactive resize
    if (clientWindow && ![clientWindow canResize]) {
        //NSDebugLog(@"Ignoring interactive right-edge resize for non-resizable client %u", [clientWindow window]);
        return;
    }
    XCBTitleBar* titleBar = (XCBTitleBar*)[frame childWindowForKey:TitleBar];
    //xcb_connection_t *connection = [[frame connection] connection];

    XCBRect frameRect = [frame windowRect];
    XCBRect titleBarRect = [titleBar windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    // Apply minimum visibility constraint when shrinking
    const int32_t MIN_VISIBLE_PIXELS = 16;
    XCBConnection *xcbConn = [frame connection];
    int32_t newWidth = anEvent->event_x;

    if ([xcbConn workareaValid]) {
        int32_t workareaX = [xcbConn cachedWorkareaX];
        // Ensure at least MIN_VISIBLE_PIXELS of right edge stays on screen
        // rightEdge = frameX + newWidth, must be >= workareaX + MIN_VISIBLE_PIXELS
        int32_t minWidth = workareaX + MIN_VISIBLE_PIXELS - frameRect.position.x;
        if (minWidth > minW) {
            minW = minWidth;
        }
    }

    // minW is a minimum *client* width; translate to minimum frame width
    int32_t minimumClientWidth = (minW < 1) ? 1 : minW;
    int32_t minimumFrameWidth = minimumClientWidth + 2 * cb;

    // Clamp to minimum width using signed arithmetic to prevent underflow
    if (newWidth < minimumFrameWidth)
        newWidth = minimumFrameWidth;

    int32_t newClientWidth = newWidth - 2 * cb;
    if (newClientWidth < minimumClientWidth)
        newClientWidth = minimumClientWidth;

    uint32_t values[1];

    values[0] = (uint32_t)newWidth;
    xcb_configure_window(connection, [frame window], XCB_CONFIG_WINDOW_WIDTH, values);
    xcb_configure_window(connection, [titleBar window], XCB_CONFIG_WINDOW_WIDTH, values);
    values[0] = (uint32_t)newClientWidth;
    xcb_configure_window(connection, [clientWindow window], XCB_CONFIG_WINDOW_WIDTH, values);

    frameRect.size.width = (uint16_t)newWidth;
    [frame setWindowRect:frameRect];
    [frame setOriginalRect:frameRect];

    titleBarRect.size.width = (uint16_t)newWidth;
    [titleBar setWindowRect:titleBarRect];
    [titleBar setOriginalRect:titleBarRect];

    clientRect.size.width = (uint16_t)newClientWidth;
    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify to client
    sendSyntheticConfigureNotify(connection, clientWindow,
                                  cb,
                                  [frame titleHeight],
                                  clientRect.size.width,
                                  clientRect.size.height);

    clientWindow = nil;
    titleBar = nil;
    connection = NULL;
}

void resizeFromLeftForEvent(xcb_motion_notify_event_t *anEvent,
                            xcb_connection_t *connection,
                            XCBFrame* frame,
                            int minW,
                            int cb)
{
    XCBWindow* clientWindow = [frame childWindowForKey:ClientWindow];
    // Respect ICCCM: if client is non-resizable, ignore interactive resize
    if (clientWindow && ![clientWindow canResize]) {
        //NSDebugLog(@"Ignoring interactive left-edge resize for non-resizable client %u", [clientWindow window]);
        return;
    }
    XCBTitleBar* titleBar = (XCBTitleBar*)[frame childWindowForKey:TitleBar];
    //xcb_connection_t *connection = [[frame connection] connection];

    XCBRect rect = [frame windowRect];
    XCBRect titleBarRect = [titleBar windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    // Apply minimum visibility constraint
    const int32_t MIN_VISIBLE_PIXELS = 16;
    XCBConnection *xcbConn = [frame connection];
    int16_t newX = anEvent->root_x;

    if ([xcbConn workareaValid]) {
        int32_t workareaX = [xcbConn cachedWorkareaX];
        int32_t workareaWidth = [xcbConn cachedWorkareaWidth];
        int32_t newWidth = rect.position.x - newX + rect.size.width;

        // Ensure at least MIN_VISIBLE_PIXELS of right edge stays on screen
        int32_t minX = workareaX + MIN_VISIBLE_PIXELS - newWidth;
        if (newX < minX) {
            newX = minX;
        }
        // Ensure at least MIN_VISIBLE_PIXELS of left edge stays on screen
        int32_t maxX = workareaX + workareaWidth - MIN_VISIBLE_PIXELS;
        if (newX > maxX) {
            newX = maxX;
        }
    }

    int xDelta = rect.position.x - newX;
    int32_t newFrameWidth = xDelta + (int32_t)rect.size.width;

    int32_t minimumClientWidth = (minW < 1) ? 1 : minW;
    int32_t minimumFrameWidth = minimumClientWidth + 2 * cb;

    // Clamp to minimum width using client-size hints translated to frame pixels
    if (newFrameWidth < minimumFrameWidth) {
        // Keep the right edge fixed; set left to preserve minimum size
        int32_t rightEdge = rect.position.x + rect.size.width;
        newX = rightEdge - minimumFrameWidth;
        newFrameWidth = minimumFrameWidth;
    }

    int32_t newClientWidth = newFrameWidth - 2 * cb;
    if (newClientWidth < minimumClientWidth)
        newClientWidth = minimumClientWidth;

    uint32_t values[2];

    values[0] = (uint32_t)newX;
    values[1] = (uint32_t)newFrameWidth;
    xcb_configure_window(connection, [frame window], XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);

    rect.position.x = newX;
    rect.size.width = (uint16_t)newFrameWidth;

    values[0] = 0;
    values[1] = (uint32_t)newFrameWidth;
    xcb_configure_window(connection, [titleBar window], XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);

    titleBarRect.position.x = 0;
    titleBarRect.size.width = (uint16_t)newFrameWidth;

    values[0] = cb;
    values[1] = (uint32_t)newClientWidth;
    xcb_configure_window(connection, [clientWindow window], XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);

    clientRect.position.x = cb;
    clientRect.size.width = (uint16_t)newClientWidth;

    [frame setWindowRect:rect];
    [frame setOriginalRect:rect];

    [titleBar setWindowRect:titleBarRect];
    [titleBar setOriginalRect:titleBarRect];

    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify to client
    sendSyntheticConfigureNotify(connection, clientWindow,
                                  rect.position.x + cb,
                                  rect.position.y + [frame titleHeight],
                                  clientRect.size.width,
                                  clientRect.size.height);

    clientWindow = nil;
    titleBar = nil;
    connection = NULL;

}

void resizeFromBottomForEvent(xcb_motion_notify_event_t *anEvent,
                              xcb_connection_t *connection,
                              XCBFrame* frame,
                              int minH,
                              uint16_t titleBarHeight,
                              int cb)
{
    XCBWindow* clientWindow = [frame childWindowForKey:ClientWindow];
    // Respect ICCCM: if client is non-resizable, ignore interactive resize
    if (clientWindow && ![clientWindow canResize]) {
        //NSDebugLog(@"Ignoring interactive bottom-edge resize for non-resizable client %u", [clientWindow window]);
        return;
    }
    //xcb_connection_t *connection = [[frame connection] connection];

    XCBRect rect = [frame windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    // Apply minimum visibility constraint when shrinking
    const int32_t MIN_VISIBLE_PIXELS = 16;
    XCBConnection *xcbConn = [frame connection];

    if ([xcbConn workareaValid]) {
        int32_t workareaY = [xcbConn cachedWorkareaY];
        // Ensure at least MIN_VISIBLE_PIXELS of bottom edge stays on screen
        // bottomEdge = frameY + newHeight, must be >= workareaY + MIN_VISIBLE_PIXELS
        int32_t minHeight = workareaY + MIN_VISIBLE_PIXELS - rect.position.y;
        if (minHeight > minH + titleBarHeight) {
            minH = minHeight - titleBarHeight;
        }
    }

    // Use signed arithmetic to prevent underflow when event_y is smaller than titlebar
    int32_t newFrameHeight = (int32_t)anEvent->event_y;
    int32_t minFrameHeight = minH + titleBarHeight + [frame bottomBorder];

    // Clamp to minimum
    if (newFrameHeight < minFrameHeight)
        newFrameHeight = minFrameHeight;

    int32_t newClientHeight = newFrameHeight - titleBarHeight - [frame bottomBorder];
    if (newClientHeight < minH)
        newClientHeight = minH;

    uint32_t values[1];

    values[0] = (uint32_t)newClientHeight;
    xcb_configure_window(connection, [clientWindow window], XCB_CONFIG_WINDOW_HEIGHT, values);
    clientRect.size.height = (uint16_t)newClientHeight;

    values[0] = (uint32_t)newFrameHeight;
    xcb_configure_window(connection, [frame window], XCB_CONFIG_WINDOW_HEIGHT, values);

    rect.size.height = (uint16_t)newFrameHeight;
    [frame setWindowRect:rect];
    [frame setOriginalRect:rect];

    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify to client
    sendSyntheticConfigureNotify(connection, clientWindow,
                                  rect.position.x + cb,
                                  rect.position.y + titleBarHeight,
                                  clientRect.size.width,
                                  clientRect.size.height);

    clientWindow = nil;
    connection = NULL;
}

void resizeFromTopForEvent(xcb_motion_notify_event_t *anEvent,
                           xcb_connection_t *connection,
                           XCBFrame* frame,
                           int minH,
                           uint16_t titleBarHeight,
                           int cb)
{
    XCBWindow* clientWindow = [frame childWindowForKey:ClientWindow];
    // Respect ICCCM: if client is non-resizable, ignore interactive resize
    if (clientWindow && ![clientWindow canResize]) {
        //NSDebugLog(@"Ignoring interactive top-edge resize for non-resizable client %u", [clientWindow window]);
        return;
    }
    XCBTitleBar* titleBar = (XCBTitleBar*)[frame childWindowForKey:TitleBar];

    XCBRect rect = [frame windowRect];
    XCBRect titleBarRect = [titleBar windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    // Apply minimum visibility constraint
    const int32_t MIN_VISIBLE_PIXELS = 16;
    XCBConnection *xcbConn = [frame connection];
    int16_t newY = anEvent->root_y;

    if ([xcbConn workareaValid]) {
        int32_t workareaY = [xcbConn cachedWorkareaY];
        int32_t workareaHeight = [xcbConn cachedWorkareaHeight];

        // Don't allow titlebar to go above workarea top
        if (newY < workareaY) {
            newY = workareaY;
        }
        // Ensure at least MIN_VISIBLE_PIXELS of top stays on screen
        int32_t maxY = workareaY + workareaHeight - MIN_VISIBLE_PIXELS;
        if (newY > maxY) {
            newY = maxY;
        }
    }

    int32_t yDelta = (int32_t)rect.position.y - (int32_t)newY;
    int32_t newFrameHeight = (int32_t)rect.size.height + yDelta;
    int32_t minFrameHeight = minH + titleBarHeight + [frame bottomBorder];

    // Clamp: if the new height is below minimum, fix the top position
    if (newFrameHeight < minFrameHeight) {
        // Keep the bottom edge fixed; set top to preserve minimum size
        int32_t bottomEdge = rect.position.y + rect.size.height;
        newY = bottomEdge - minFrameHeight;
        newFrameHeight = minFrameHeight;
    }

    int32_t newClientHeight = newFrameHeight - titleBarHeight - [frame bottomBorder];
    if (newClientHeight < minH)
        newClientHeight = minH;

    uint32_t values[2];

    // Configure frame (position + height)
    values[0] = (uint32_t)newY;
    values[1] = (uint32_t)newFrameHeight;
    xcb_configure_window(connection, [frame window], XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);

    rect.position.y = newY;
    rect.size.height = (uint16_t)newFrameHeight;

    // Titlebar Y is always 0 relative to frame
    values[0] = 0;
    xcb_configure_window(connection, [titleBar window], XCB_CONFIG_WINDOW_Y, values);
    titleBarRect.position.y = 0;

    // Configure client
    values[0] = titleBarHeight;
    values[1] = (uint32_t)newClientHeight;
    xcb_configure_window(connection, [clientWindow window], XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);
    clientRect.size.height = (uint16_t)newClientHeight;
    clientRect.position.y = titleBarHeight;

    [frame setWindowRect:rect];
    [frame setOriginalRect:rect];

    [titleBar setWindowRect:titleBarRect];
    [titleBar setOriginalRect:titleBarRect];

    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify to client
    sendSyntheticConfigureNotify(connection, clientWindow,
                                  rect.position.x + cb,
                                  rect.position.y + titleBarHeight,
                                  clientRect.size.width,
                                  clientRect.size.height);

    clientWindow = nil;
    titleBar = nil;
    connection = NULL;
}

void resizeFromAngleForEvent(xcb_motion_notify_event_t *anEvent,
                             xcb_connection_t *connection,
                             XCBFrame *frame,
                             int minW,
                             int minH,
                             uint16_t titleBarHeight,
                             int cb)
{
    XCBWindow* clientWindow = [frame childWindowForKey:ClientWindow];
    // Respect ICCCM: if client is non-resizable, ignore interactive resize
    if (clientWindow && ![clientWindow canResize]) {
        //NSDebugLog(@"Ignoring interactive corner resize for non-resizable client %u", [clientWindow window]);
        return;
    }
    XCBTitleBar* titleBar = (XCBTitleBar*)[frame childWindowForKey:TitleBar];
    //xcb_connection_t *connection = [[frame connection] connection];

    XCBRect rect = [frame windowRect];
    XCBRect titleBarRect = [titleBar windowRect];
    XCBRect clientRect = [clientWindow windowRect];

    // Use signed arithmetic to prevent underflow
    int32_t newFrameWidth = (int32_t)anEvent->event_x;
    int32_t newFrameHeight = (int32_t)anEvent->event_y;

    // Clamp to minimum dimensions
    int32_t minimumClientWidth = (minW < 1) ? 1 : minW;
    int32_t minimumFrameWidth = minimumClientWidth + 2 * cb;
    int32_t minFrameHeight = minH + titleBarHeight + [frame bottomBorder];
    if (newFrameWidth < minimumFrameWidth)
        newFrameWidth = minimumFrameWidth;
    if (newFrameHeight < minFrameHeight)
        newFrameHeight = minFrameHeight;

    int32_t newClientWidth = newFrameWidth - 2 * cb;
    int32_t newClientHeight = newFrameHeight - titleBarHeight - [frame bottomBorder];
    if (newClientWidth < minimumClientWidth) newClientWidth = minimumClientWidth;
    if (newClientHeight < 1) newClientHeight = 1;

    uint32_t values[2];

    values[0] = (uint32_t)newFrameWidth;
    values[1] = (uint32_t)newFrameHeight;
    xcb_configure_window(connection, [frame window], XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);

    values[0] = (uint32_t)newFrameWidth;
    xcb_configure_window(connection, [titleBar window], XCB_CONFIG_WINDOW_WIDTH, values);

    values[0] = (uint32_t)newClientWidth;
    values[1] = (uint32_t)newClientHeight;
    xcb_configure_window(connection, [clientWindow window], XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);

    rect.size.width = (uint16_t)newFrameWidth;
    rect.size.height = (uint16_t)newFrameHeight;
    [frame setWindowRect:rect];
    [frame setOriginalRect:rect];

    titleBarRect.size.width = (uint16_t)newFrameWidth;
    [titleBar setWindowRect:titleBarRect];
    [titleBar setOriginalRect:titleBarRect];

    clientRect.size.width = (uint16_t)newClientWidth;
    clientRect.size.height = (uint16_t)newClientHeight;
    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify to client
    sendSyntheticConfigureNotify(connection, clientWindow,
                                  rect.position.x + cb,
                                  rect.position.y + titleBarHeight,
                                  clientRect.size.width,
                                  clientRect.size.height);

    titleBar = nil;
    clientWindow = nil;
    connection = NULL;
}

- (void) moveTo:(XCBPoint)coordinates
{
    // Minimal implementation for maximum performance
    XCBPoint pos = XCBMakePoint(coordinates.x - offset.x, coordinates.y - offset.y);

    // Cast through int32_t first: (uint32_t)(negative double) is undefined
    // behavior in C, causing window jumps when the position goes negative
    // (e.g. left edge crossing the left screen border).
    uint32_t values[] = {(uint32_t)(int32_t)pos.x, (uint32_t)(int32_t)pos.y};
    xcb_configure_window([connection connection], window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    // PERFORMANCE FIX: Don't flush on every motion event - let the event loop batch flushes
    // xcb_flush([connection connection]);

    // Update internal state only - skip expensive rect operations during drag
    XCBRect rect = [super windowRect];
    rect.position = pos;
    [super setWindowRect:rect];
}

- (void) configureClient
{
    xcb_configure_notify_event_t event;
    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    // Use cached in-memory rects — no blocking xcb_get_geometry round-trip.
    XCBRect rect = [self windowRect];
    XCBRect clientRect = [clientWindow windowRect];
    uint16_t height = titleHeight;

    /*** synthetic event: coordinates must be in root space. ***/

    event.event = [clientWindow window];
    event.window = [clientWindow window];
    event.x = rect.position.x + self.clientBorder;
    event.y = rect.position.y + height;
    event.border_width = 0;
    event.width = clientRect.size.width;
    event.height = clientRect.size.height;
    event.override_redirect = 0;
    event.above_sibling = XCB_NONE;
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.sequence = 0;

    [connection sendEvent:(const char*) &event toClient:clientWindow propagate:NO];

    [clientWindow setWindowRect:clientRect];

    clientWindow = nil;
}

- (void)configureClientWithFramePosition:(XCBPoint)framePos
                              clientSize:(XCBSize)clientSize
{
    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    if (!clientWindow) return;

    uint16_t titleHgt = titleHeight;

    // Use the static helper with explicit dimensions (same as manual resize)
    sendSyntheticConfigureNotify([connection connection], clientWindow,
                                  framePos.x + self.clientBorder,
                                  framePos.y + titleHgt,
                                  clientSize.width,
                                  clientSize.height);

    clientWindow = nil;
}

- (void)programmaticResizeToRect:(XCBRect)targetRect
{
    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    XCBTitleBar *titleBar = (XCBTitleBar*)[self childWindowForKey:TitleBar];
    if (!clientWindow || !titleBar) return;

    // Enforce ICCCM / WM_NORMAL_HINTS: do not resize non-resizable (fixed-size) clients
    if (![clientWindow canResize]) {
        //NSLog(@"[XCBFrame] Refusing programmatic resize for non-resizable client %u", [clientWindow window]);
        return;
    }

    xcb_connection_t *conn = [connection connection];

    uint16_t titleHgt = titleHeight;

    // Calculate child window dimensions (same as manual resize functions)
    XCBRect titleBarRect = XCBMakeRect(XCBMakePoint(0, 0),
                                        XCBMakeSize(targetRect.size.width, titleHgt));
    // Client fills frame between titlebar and resize bar, inset by the border
    int cb = self.clientBorder;
    XCBRect clientRect = XCBMakeRect(XCBMakePoint(cb, titleHgt),
                                      XCBMakeSize(targetRect.size.width - 2 * cb,
                                                   targetRect.size.height - titleHgt - self.bottomBorder));

    // Configure frame window (position + size)
    uint32_t frameValues[4] = {(uint32_t)targetRect.position.x, (uint32_t)targetRect.position.y,
                               targetRect.size.width, targetRect.size.height};
    xcb_configure_window(conn, [self window],
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         frameValues);

    // Configure titlebar window (size only, position relative to frame)
    uint32_t titleValues[2] = {titleBarRect.size.width, titleBarRect.size.height};
    xcb_configure_window(conn, [titleBar window],
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         titleValues);

    // Configure client window (position + size relative to frame)
    uint32_t clientValues[4] = {(uint32_t)clientRect.position.x, (uint32_t)clientRect.position.y,
                                clientRect.size.width, clientRect.size.height};
    xcb_configure_window(conn, [clientWindow window],
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         clientValues);

    // Flush immediately (critical - this is what manual resize does)
    xcb_flush(conn);

    // Update both windowRect AND originalRect for all windows (like manual resize)
    [self setWindowRect:targetRect];
    [self setOriginalRect:targetRect];

    [titleBar setWindowRect:titleBarRect];
    [titleBar setOriginalRect:titleBarRect];

    [clientWindow setWindowRect:clientRect];
    [clientWindow setOriginalRect:clientRect];

    // Send synthetic ConfigureNotify with the calculated dimensions
    sendSyntheticConfigureNotify(conn, clientWindow,
                                  clientRect.position.x,
                                  clientRect.position.y,
                                  clientRect.size.width,
                                  clientRect.size.height);

}

- (MousePosition) mouseIsOnWindowBorderForEvent:(xcb_motion_notify_event_t *)anEvent
{
    int rightBorder = [super windowRect].size.width;
    int bottomBorder = [super windowRect].size.height;
    int leftBorder = [super windowRect].position.x;
    int topBorder = [super windowRect].position.y;
    MousePosition position = None;

    if (rightBorder == anEvent->event_x || (rightBorder - 3) < anEvent->event_x)
    {
        position = RightBorder;
    }


    if (bottomBorder == anEvent->event_y || (bottomBorder - 3) < anEvent->event_y)
    {
        position = BottomBorder;
    }

    if ((bottomBorder == anEvent->event_y || (bottomBorder - 3) < anEvent->event_y) &&
        (rightBorder == anEvent->event_x || (rightBorder - 3) < anEvent->event_x))
    {
        position = BottomRightCorner;
    }

    if (leftBorder == anEvent->root_x || (leftBorder + 3) > anEvent->root_x)
    {
        position = LeftBorder;
    }

    if (topBorder == anEvent->root_y || (topBorder + 3) > anEvent->root_y)
    {
        position = TopBorder;
    }

    // Top-left corner
    if ((topBorder == anEvent->root_y || (topBorder + 3) > anEvent->root_y) &&
        (leftBorder == anEvent->root_x || (leftBorder + 3) > anEvent->root_x))
    {
        position = TopLeftCorner;
    }

    // Top-right corner
    if ((topBorder == anEvent->root_y || (topBorder + 3) > anEvent->root_y) &&
        (rightBorder == anEvent->event_x || (rightBorder - 3) < anEvent->event_x))
    {
        position = TopRightCorner;
    }

    // Bottom-left corner
    if ((bottomBorder == anEvent->event_y || (bottomBorder - 3) < anEvent->event_y) &&
        (leftBorder == anEvent->root_x || (leftBorder + 3) > anEvent->root_x))
    {
        position = BottomLeftCorner;
    }

    return position;

}

- (void) restoreDimensionAndPosition
{
    XCBWindow *clientWindow = [self childWindowForKey:ClientWindow];
    XCBTitleBar *titleBar = (XCBTitleBar*)[self childWindowForKey:TitleBar];

    [super restoreDimensionAndPosition];
    [clientWindow restoreDimensionAndPosition];
    [titleBar restoreDimensionAndPosition];
    [titleBar drawTitleBarComponents];

    clientWindow = nil;
    titleBar = nil;
}


/********************************
 *                               *
 *            ACCESSORS          *
 *                               *
 ********************************/

- (void)setChildren:(NSMutableDictionary *)aChildrenSet
{
    children = aChildrenSet;
}

-(NSMutableDictionary*) getChildren
{
    return children;
}

- (void) dealloc
{
    [children removeAllObjects]; //not needed probably
    children = nil;
}


@end
