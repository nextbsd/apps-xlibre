//
//  URSHybridEventHandler.m
//  XLibre.app - the window manager
//
//  Created by Alessandro Sangiuliano on 22/06/20.
//  Copyright (c) 2020 Alessandro Sangiuliano. All rights reserved.
//
//  Owns the XCB event loop on the NSRunLoop and dispatches to the focus,
//  workarea and title bar managers. In XLibre.app it manages the windows
//  of Xnextbsd, whose frames are WindowServer windows: WindowServer
//  composites them, and Command-Tab and the Dock belong to WindowServer,
//  so there is no compositor, window switcher, keyboard grab or snapping
//  here.
//

#import "URSHybridEventHandler.h"
#import "URSProfiler.h"
#import "XCBScreen.h"
#import "XCBQueryTreeReply.h"
#import "XCBAttributesReply.h"
#import <xcb/xcb.h>
#import <xcb/xcb_icccm.h>
#import <xcb/xcb_aux.h>
#import <xcb/randr.h>
#import <xcb/xproto.h>
#import "EWMHService.h"
#import "XCBAtomService.h"
#import "ICCCMService.h"
#import "XCBFrame.h"
#import "URSThemeIntegration.h"
#import "URSDecorationMetrics.h"
#import "XLScaleSettings.h"

@implementation URSHybridEventHandler

@synthesize connection;
@synthesize selectionManagerWindow;
@synthesize xcbEventsIntegrated;
@synthesize eventCount;
@synthesize focusManager;
@synthesize workareaManager;
@synthesize titlebarController;
@synthesize randrEventBase = _randrEventBase;

#pragma mark - Initialization

- (id)init
{
    self = [super init];

    if (self == nil) {
        NSLog(@"Unable to init URSHybridEventHandler...");
        return nil;
    }

    self.xcbEventsIntegrated = NO;
    self.eventCount = 0;
    _randrEventBase = 0;

    // The X server named by DISPLAY: XLibre.app sets it once Xnextbsd is up.
    connection = [XCBConnection sharedConnectionAsWindowManager:YES];
    if (connection == nil || [connection connection] == NULL ||
        xcb_connection_has_error([connection connection])) {
        NSLog(@"[XLibre] can't connect to the X server");
        return nil;
    }

    self.focusManager = [[URSFocusManager alloc] initWithConnection:connection
                                                    selectionWindow:nil]; // set after registerAsWindowManager
    self.workareaManager = [[URSWorkareaManager alloc] initWithConnection:connection];
    self.titlebarController = [[URSTitlebarController alloc] initWithConnection:connection];
    self.titlebarController.workareaManager = self.workareaManager;
    self.titlebarController.focusManager = self.focusManager;

    return self;
}

#pragma mark - Starting

- (BOOL)start
{
    if (![self registerAsWindowManager]) {
        NSLog(@"[XLibre] can't become the X window manager");
        return NO;
    }

    // Redraw all decorations when the user switches GSTheme. The Themes
    // preference pane announces changes with a distributed notification.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(themeDidActivate:)
                                                 name:GSThemeDidActivateNotification
                                               object:nil];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self
                                                        selector:@selector(themePreferenceDidChange:)
                                                            name:@"GSThemePreferenceDidChangeNotification"
                                                          object:nil];

    // HiDPI: a new scale makes title bars and borders another size in X
    // pixels (URSDecorationMetrics); lay every frame out again, as for a
    // new theme.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(themeDidActivate:)
                                                 name:XLScaleDidChangeNotification
                                               object:nil];

    // Wire selectionManagerWindow into the focus manager now that it exists
    self.focusManager.selectionManagerWindow = self.selectionManagerWindow;

    [self decorateExistingWindowsOnStartup];
    [self setupXCBEventIntegration];
    [self setupRANDR];
    return YES;
}

#pragma mark - Managed windows

- (NSUInteger)managedWindowCount
{
    NSUInteger n = 0;
    for (NSString *key in [connection windowsMap]) {
        XCBWindow *w = [[connection windowsMap] objectForKey:key];
        if ([w isKindOfClass:[XCBFrame class]] &&
            [(XCBFrame *)w childWindowForKey:ClientWindow] != nil)
            n++;
    }
    return n;
}

#pragma mark - Original URSEventHandler Methods (Preserved)

- (BOOL)registerAsWindowManager
{
    XCBScreen *screen = [[connection screens] objectAtIndex:0];
    XCBVisual *visual = [[XCBVisual alloc] initWithVisualId:[screen screen]->root_visual];
    [visual setVisualTypeForScreen:screen];

    selectionManagerWindow = [connection createWindowWithDepth:[screen screen]->root_depth
                                                 withParentWindow:[screen rootWindow]
                                                    withXPosition:-1
                                                    withYPosition:-1
                                                        withWidth:1
                                                       withHeight:1
                                                 withBorrderWidth:0
                                                     withXCBClass:XCB_COPY_FROM_PARENT
                                                     withVisualId:visual
                                                    withValueMask:0
                                                    withValueList:NULL
                                                  registerWindow:YES];

    [selectionManagerWindow setSkipTaskBar:YES];
    [selectionManagerWindow setSkipPager:YES];
    [selectionManagerWindow setDecorated:NO];

    //NSLog(@"[WindowManager] Attempting to become WM (replace existing if needed)...");
    BOOL registered = [connection registerAsWindowManager:YES screenId:0 selectionWindow:selectionManagerWindow];

    if (!registered) {
        //NSLog(@"[WindowManager] Existing WM detected; trying to replace it");
        registered = [connection registerAsWindowManager:NO screenId:0 selectionWindow:selectionManagerWindow];
    }

    if (!registered) {
        NSLog(@"[WindowManager] Could not acquire WM ownership even after replace attempt");
        return NO;
    }

    //NSLog(@"[WindowManager] Successfully registered as window manager");

    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];
    [ewmhService updateNetWmState:selectionManagerWindow];
    [ewmhService putPropertiesForRootWindow:[screen rootWindow] andWmWindow:selectionManagerWindow];
    
    // Set initial workarea to full screen (no struts yet)
    [ewmhService updateWorkareaForRootWindow:[screen rootWindow] 
                                           x:0 
                                           y:0 
                                       width:[screen screen]->width_in_pixels 
                                      height:[screen screen]->height_in_pixels];
    
    [connection flush];

    // ARC handles cleanup automatically
    return YES;
}

#pragma mark - Existing Windows Decoration

- (void)decorateExistingWindowsOnStartup {
    @try {
        XCBScreen *screen = [[connection screens] objectAtIndex:0];
        XCBWindow *rootWindow = [screen rootWindow];
        EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];

        XCBQueryTreeReply *tree = [rootWindow queryTree];
        xcb_window_t *children = [tree queryTreeAsArray];
        uint32_t childCount = tree.childrenLen;

        //NSLog(@"[WindowManager] Decorating %u pre-existing windows", childCount);

        connection.adoptingExistingWindows = YES;
        for (uint32_t i = 0; i < childCount; i++) {
            xcb_window_t winId = children[i];

            // Skip our own helper/selection window and root
            if (winId == [rootWindow window] || winId == [self.selectionManagerWindow window]) {
                continue;
            }

            XCBWindow *win = [[XCBWindow alloc] initWithXCBWindow:winId andConnection:connection];
            [win updateAttributes];
            XCBAttributesReply *attrs = [win attributes];

            if (!attrs) {
                //NSLog(@"[WindowManager] Skipping window %u (no attributes)", winId);
                continue;
            }

            // Ignore override-redirect windows for decoration
            if (attrs.overrideRedirect) {
                //NSLog(@"[WindowManager] Skipping window %u (override-redirect)", winId);
                continue;
            }

            if (attrs.mapState != XCB_MAP_STATE_VIEWABLE) {
                //NSLog(@"[WindowManager] Skipping window %u (mapState %u)", winId, attrs.mapState);
                continue;
            }
            
            // Check if this is a dock window with struts - scan for struts even if already managed
            if ([ewmhService isWindowTypeDock:win]) {
                //NSLog(@"[WindowManager] Found dock window %u at startup - checking for struts", winId);
                [self.workareaManager readAndRegisterStrutForWindow:winId];
            }

            // Skip already-managed windows
            if ([connection windowForXCBId:winId]) {
                //NSLog(@"[WindowManager] Window %u already managed; skipping", winId);
                continue;
            }

            //NSLog(@"[WindowManager] Adopting existing window %u", winId);

            // Synthesize a map request so normal decoration flow runs
            xcb_map_request_event_t mapEvent = {0};
            mapEvent.response_type = XCB_MAP_REQUEST;
            mapEvent.parent = [rootWindow window];
            mapEvent.window = winId;

            [connection handleMapRequest:&mapEvent];

            // Mirror the XCB_MAP_REQUEST handler's post-processing for startup-adopted windows.
            // Without this, pre-existing windows miss compositor registration and fixed-size
            // border adjustment that the normal map-request flow provides.
            XCBWindow *mappedClient = [connection windowForXCBId:winId];
            if (mappedClient && [[mappedClient parentWindow] isKindOfClass:[XCBFrame class]]) {
                [self adjustBorderForFixedSizeWindow:winId];
            }

            // Apply GSTheme rendering to the titlebar immediately after decoration.
            // The normal XCB_MAP_REQUEST path calls this; we must replicate it for
            // startup-adopted windows or they keep the unstyled placeholder titlebar.
            [self applyGSThemeToRecentlyMappedWindow:[NSNumber numberWithUnsignedInt:winId]];
        }
        connection.adoptingExistingWindows = NO;

        [connection flush];
        
        // Recalculate workarea after scanning all existing windows for struts,
        // inside WindowServer's usable area
        [self.workareaManager refreshUsableArea];
        [self.workareaManager recalculateWorkarea];
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception while decorating existing windows: %@", exception.reason);
    }
}

#pragma mark - NSRunLoop Integration (New for Phase 1)

- (void)setupXCBEventIntegration
{

    // Get XCB file descriptor for monitoring
    int xcbFD = xcb_get_file_descriptor([connection connection]);
    if (xcbFD < 0) {
        NSLog(@"ERROR Phase 1: Failed to get XCB file descriptor");
        return;
    }

    // Follow libs-back pattern for NSRunLoop file descriptor monitoring
    NSRunLoop *currentRunLoop = [NSRunLoop currentRunLoop];

    // Add XCB file descriptor to NSRunLoop for read events
    [currentRunLoop addEvent:(void*)(uintptr_t)xcbFD
                        type:ET_RDESC
                     watcher:self
                     forMode:NSDefaultRunLoopMode];

    // Menu tracking loops run in NSEventTrackingRunLoopMode — process XCB events
    // there too so the WM can handle MapRequest for popup menu windows
    [currentRunLoop addEvent:(void*)(uintptr_t)xcbFD
                        type:ET_RDESC
                     watcher:self
                     forMode:NSEventTrackingRunLoopMode];

    // NSAlert's -runModal runs its event loop in NSModalPanelRunLoopMode.
    // Without this, the MapRequest for the alert's own window is never
    // processed and the X server waits forever — deadlock.
    [currentRunLoop addEvent:(void*)(uintptr_t)xcbFD
                        type:ET_RDESC
                     watcher:self
                     forMode:NSModalPanelRunLoopMode];

    self.xcbEventsIntegrated = YES;

    // Start monitoring for XCB events immediately
    [self performSelector:@selector(processAvailableXCBEvents)
               withObject:nil
               afterDelay:0.1];
}

#pragma mark - RunLoopEvents Protocol Implementation

- (void)receivedEvent:(void*)data
                 type:(RunLoopEventType)type
                extra:(void*)extra
              forMode:(NSString*)mode
{
    if (type == ET_RDESC) {
        // Process available XCB events (non-blocking)
        [self processAvailableXCBEvents];
    }
}

- (void)processAvailableXCBEvents
{
    URS_PROFILE_BEGIN(eventLoop);
    xcb_generic_event_t *e;
    xcb_motion_notify_event_t *lastMotionEvent = NULL;
    BOOL needFlush = NO;
    NSUInteger eventsProcessed = 0;
    const NSUInteger maxEventsPerCall = 50; // Limit to prevent CPU hogging
    BOOL moreEventsAvailable = NO;

    // Use xcb_poll_for_event (non-blocking) instead of xcb_wait_for_event (blocking)
    while ((e = xcb_poll_for_event([connection connection])) &&
           eventsProcessed < maxEventsPerCall) {
        eventsProcessed++;

        // Motion event compression: accumulate the latest motion event
        // but don't process it until we see a non-motion event or the queue empties.
        if ((e->response_type & ~0x80) == XCB_MOTION_NOTIFY) {
            if (lastMotionEvent) {
                free(lastMotionEvent);
            }
            lastMotionEvent = malloc(sizeof(xcb_motion_notify_event_t));
            memcpy(lastMotionEvent, e, sizeof(xcb_motion_notify_event_t));
            free(e);
            continue;
        }

        // Flush pending compressed motion only before events that depend
        // on an up-to-date window position (button press/release).
        // Flushing before every non-motion event (e.g. DAMAGE) would
        // defeat compression and make resize unbearably slow.
        if (lastMotionEvent) {
            uint8_t nextType = e->response_type & ~0x80;
            if (nextType == XCB_BUTTON_RELEASE || nextType == XCB_BUTTON_PRESS) {
                [connection handleMotionNotify:lastMotionEvent];
                [self.titlebarController handleResizeDuringMotion:lastMotionEvent];
                [self.titlebarController handleHoverDuringMotion:lastMotionEvent];
                needFlush = YES;
                free(lastMotionEvent);
                lastMotionEvent = NULL;
            }
        }

        [self processXCBEvent:e];

        // Check if we need to flush after this event
        if ([self eventNeedsFlush:e]) {
            needFlush = YES;
        }

        free(e);
    }

    // Process any remaining compressed motion event (e.g. motion was last in queue)
    if (lastMotionEvent) {
        [connection handleMotionNotify:lastMotionEvent];
        [self.titlebarController handleResizeDuringMotion:lastMotionEvent];
        [self.titlebarController handleHoverDuringMotion:lastMotionEvent];
        needFlush = YES;
        free(lastMotionEvent);
        lastMotionEvent = NULL;
    }

    // The X server went away: stop watching its fd (it would stay
    // readable) and let XLibre.app quit.
    if (xcb_connection_has_error([connection connection])) {
        if (self.xcbEventsIntegrated) {
            int fd = xcb_get_file_descriptor([connection connection]);
            for (NSString *mode in @[NSDefaultRunLoopMode, NSEventTrackingRunLoopMode,
                                     NSModalPanelRunLoopMode])
                [[NSRunLoop currentRunLoop] removeEvent:(void*)(uintptr_t)fd
                                                   type:ET_RDESC
                                                forMode:mode
                                                    all:YES];
            self.xcbEventsIntegrated = NO;
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"URSXConnectionLostNotification" object:self];
        }
        URS_PROFILE_END(eventLoop);
        return;
    }

    // Batched flush: only flush when needed
    if (needFlush) {
        [connection flush];
        [connection setNeedFlush:NO];
    }
    

    // If we hit the event limit, assume more events may be available
    // Don't poll again here as both xcb_poll_for_event and xcb_poll_for_queued_event
    // remove events from the queue, which would cause lost events
    if (eventsProcessed >= maxEventsPerCall) {
        moreEventsAvailable = YES;
    }

    // Update event statistics
    self.eventCount += eventsProcessed;

    // If we hit the limit and there are more events, reschedule processing
    // This prevents CPU hogging while maintaining responsiveness
    if (eventsProcessed >= maxEventsPerCall && moreEventsAvailable) {
        [self performSelector:@selector(processAvailableXCBEvents)
                   withObject:nil
                   afterDelay:0.001]; // Very short delay to yield CPU
    }

    URS_PROFILE_END(eventLoop);
}

- (void)processXCBEvent:(xcb_generic_event_t*)event
{
    URS_PROFILE_BEGIN(eventDispatch);
    // Process individual XCB event (same logic as original startEventHandlerLoop)
    switch (event->response_type & ~0x80) {
        case XCB_VISIBILITY_NOTIFY: {
            xcb_visibility_notify_event_t *visibilityEvent = (xcb_visibility_notify_event_t *)event;
            [connection handleVisibilityEvent:visibilityEvent];
            break;
        }
        case XCB_EXPOSE: {
            xcb_expose_event_t *exposeEvent = (xcb_expose_event_t *)event;
            [connection handleExpose:exposeEvent];

            // Re-apply GSTheme if this is a titlebar expose event
            [self handleTitlebarExpose:exposeEvent];

            break;
        }
        case XCB_ENTER_NOTIFY: {
            xcb_enter_notify_event_t *enterEvent = (xcb_enter_notify_event_t *)event;
            [connection handleEnterNotify:enterEvent];
            break;
        }
        case XCB_LEAVE_NOTIFY: {
            xcb_leave_notify_event_t *leaveEvent = (xcb_leave_notify_event_t *)event;
            [connection handleLeaveNotify:leaveEvent];
            // Clear hover state if leaving the hovered titlebar
            [self.titlebarController handleTitlebarLeave:leaveEvent];
            break;
        }
        case XCB_FOCUS_IN: {
            xcb_focus_in_event_t *focusInEvent = (xcb_focus_in_event_t *)event;
            [connection handleFocusIn:focusInEvent];
            [self handleFocusChange:focusInEvent->event isActive:YES];
            [self xFocusInWhileAppKitWindowKey:focusInEvent];
            break;
        }
        case XCB_FOCUS_OUT: {
            xcb_focus_out_event_t *focusOutEvent = (xcb_focus_out_event_t *)event;
            [connection handleFocusOut:focusOutEvent];
            // Titlebar active/inactive state is driven entirely by FocusIn.
            // Processing FocusOut here would cause a momentary inactive flash
            // when focus moves between child windows of the same application
            // (e.g. Chrome swapping rendering surfaces during page load).
            // The previously-focused window's titlebar is marked inactive by
            // handleFocusChange:isActive:YES when the next FocusIn arrives.
            break;
        }
        case XCB_BUTTON_PRESS: {
            xcb_button_press_event_t *pressEvent = (xcb_button_press_event_t *)event;

            // Check if this is a button click on a GSThemeTitleBar
            BOOL wasButtonPress = [self.titlebarController handleTitlebarButtonPress:pressEvent];
            if (!wasButtonPress) {
                // Not a titlebar button: the XCBKit activation path.
                // 1. Focus the client window (WM_TAKE_FOCUS, _NET_ACTIVE_WINDOW, ungrab keyboard)
                // 2. Raise the frame
                // 3. Update titlebar states (active/inactive for all windows)
                [connection handleButtonPress:pressEvent];
            }
            
            break;
        }
        case XCB_BUTTON_RELEASE: {
            xcb_button_release_event_t *releaseEvent = (xcb_button_release_event_t *)event;

            // Title bar buttons act on release
            if ([self.titlebarController handleTitlebarButtonRelease:releaseEvent]) {
                break;
            }

            // Let xcbkit handle the release first
            [connection handleButtonRelease:releaseEvent];
            // After resize completes, update the titlebar with GSTheme
            [self.titlebarController handleResizeComplete:releaseEvent];

            break;
        }
        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t *notifyEvent = (xcb_map_notify_event_t *)event;
            [connection handleMapNotify:notifyEvent];
            
            break;
        }
        case XCB_MAP_REQUEST: {
            xcb_map_request_event_t *mapRequestEvent = (xcb_map_request_event_t *)event;

            // Check if this is a dock window with struts
            EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];
            XCBWindow *tempWindow = [[XCBWindow alloc] initWithXCBWindow:mapRequestEvent->window andConnection:connection];
            if ([ewmhService isWindowTypeDock:tempWindow]) {
                //NSLog(@"[WindowManager] Dock window %u being mapped - checking for struts", mapRequestEvent->window);
                [self.workareaManager readAndRegisterStrutForWindow:mapRequestEvent->window];
                [self.workareaManager recalculateWorkarea];
            }
            tempWindow = nil;
            ewmhService = nil;

            // Resize window to 70% of screen size before mapping
            [self resizeWindowTo70Percent:mapRequestEvent->window];

            // Let XCBConnection handle the map request (creates frame for managed windows)
            [connection handleMapRequest:mapRequestEvent];

            XCBWindow *mappedClient = [connection windowForXCBId:mapRequestEvent->window];

            // Check if handleMapRequest created a frame for this window.
            // Unframed windows (menus, popups, tooltips, transients) only need
            // compositor registration — skip theme, focus, and border processing.
            if (!mappedClient || ![[mappedClient parentWindow] isKindOfClass:[XCBFrame class]]) {
                break;
            }

            // --- Framed windows only below this point ---


            // Hide borders for windows with fixed sizes (like info panels and logout)
            [self adjustBorderForFixedSizeWindow:mapRequestEvent->window];

            // Apply GSTheme immediately with no delay
            [self applyGSThemeToRecentlyMappedWindow:[NSNumber numberWithUnsignedInt:mapRequestEvent->window]];

            // If the window has _NET_WM_STATE_FULLSCREEN set in its properties
            // (e.g. browser video fullscreen), immediately enter fullscreen mode.
            {
                EWMHService *ewmh = [EWMHService sharedInstanceWithConnection:connection];
                void *fullReply = [ewmh getProperty:[ewmh EWMHWMState]
                                      propertyType:XCB_ATOM_ATOM
                                         forWindow:mappedClient
                                            delete:NO
                                            length:UINT32_MAX];
                BOOL wantsFullscreen = NO;
                if (fullReply)
                {
                    xcb_atom_t *atoms = (xcb_atom_t *)xcb_get_property_value(fullReply);
                    uint32_t len = xcb_get_property_value_length(fullReply) / sizeof(xcb_atom_t);
                    xcb_atom_t fsAtom = [[ewmh atomService] atomFromCachedAtomsWithKey:[ewmh EWMHWMStateFullscreen]];
                    for (uint32_t i = 0; i < len; i++)
                    {
                        if (atoms[i] == fsAtom)
                        {
                            wantsFullscreen = YES;
                            break;
                        }
                    }
                    free(fullReply);
                }
                if (wantsFullscreen)
                {
                    [ewmh toggleFullscreenForWindow:mappedClient];
                }
            }

            // Try to focus the client window if it's focusable
            // This ensures dialogs, alerts, sheets and other special windows get focused too
            if ([self.focusManager isWindowFocusable:mappedClient allowDesktop:NO]) {
                // Schedule focus after a brief delay to ensure the window is fully set up
                // Use focusNewlyMappedWindow to ensure new windows always get focus
                [self performSelector:@selector(focusNewlyMappedWindow:)
                           withObject:mappedClient
                           afterDelay:0.1];
            }
            break;
        }
        case XCB_UNMAP_NOTIFY: {
            xcb_unmap_notify_event_t *unmapNotifyEvent = (xcb_unmap_notify_event_t *)event;
            xcb_window_t removedClientId = [self.focusManager clientWindowIdForWindowId:unmapNotifyEvent->window];
            [connection handleUnMapNotify:unmapNotifyEvent];


            [self.focusManager ensureFocusAfterWindowRemoval:removedClientId];
            break;
        }
        case XCB_DESTROY_NOTIFY: {
            xcb_destroy_notify_event_t *destroyNotify = (xcb_destroy_notify_event_t *)event;
            xcb_window_t removedClientId = [self.focusManager clientWindowIdForWindowId:destroyNotify->window];
            
            
            // Remove any struts for this window
            if ([self.workareaManager removeStrutForWindow:destroyNotify->window]) {
                [self.workareaManager recalculateWorkarea];
            }
            
            [connection handleDestroyNotify:destroyNotify];
            [self.focusManager ensureFocusAfterWindowRemoval:removedClientId];
            break;
        }
        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *clientMessageEvent = (xcb_client_message_event_t *)event;
            [connection handleClientMessage:clientMessageEvent];
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            xcb_configure_request_event_t *configRequest = (xcb_configure_request_event_t *)event;
            [connection handleConfigureWindowRequest:configRequest];
            break;
        }
        case XCB_CREATE_NOTIFY: {
            xcb_create_notify_event_t *createNotify = (xcb_create_notify_event_t *)event;
            [connection handleCreateNotify:createNotify];
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *configureNotify = (xcb_configure_notify_event_t *)event;
            [connection handleConfigureNotify:configureNotify];
            
            break;
        }
        case XCB_REPARENT_NOTIFY: {
            xcb_reparent_notify_event_t *reparentNotify = (xcb_reparent_notify_event_t *)event;
            [connection handleReparentNotify:reparentNotify];

            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            xcb_property_notify_event_t *propEvent = (xcb_property_notify_event_t *)event;
            // The root window's properties: only WindowServer's usable area
            // (_XNEXTBSD_USABLE_AREA, set by Xnextbsd) is of interest.
            XCBScreen *propScreen = [[connection screens] firstObject];
            if (propScreen && propEvent->window == [propScreen screen]->root) {
                if ([self.workareaManager isUsableAreaPropertyEvent:propEvent] &&
                    [self.workareaManager refreshUsableArea])
                    [self.workareaManager recalculateWorkarea];
                break;
            }
            // Check if this is a strut property change
            [self.workareaManager handleStrutPropertyChange:propEvent];
            [self handleWindowTitlePropertyChange:propEvent];
            [connection handlePropertyNotify:propEvent];
            break;
        }
        case XCB_SELECTION_CLEAR: {
            xcb_selection_clear_event_t *selectionClearEvent = (xcb_selection_clear_event_t *)event;
            [self handleSelectionClear:selectionClearEvent];
            break;
        }
        default: {
            [self handleExtensionEvent:event];
            break;
        }
    }
    URS_PROFILE_END(eventDispatch);
}

- (BOOL)eventNeedsFlush:(xcb_generic_event_t*)event
{
    // Determine if event requires immediate flush (same logic as original)
    switch (event->response_type & ~0x80) {
        case XCB_EXPOSE:
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
        case XCB_MAP_REQUEST:
        case XCB_DESTROY_NOTIFY:
        case XCB_CLIENT_MESSAGE:
        case XCB_CONFIGURE_REQUEST:
        case XCB_SELECTION_CLEAR:
        case XCB_ENTER_NOTIFY:
        case XCB_LEAVE_NOTIFY:
            return YES;
        default:
            return NO;
    }
}

- (void)setupRANDR
{
    xcb_connection_t *conn = [connection connection];
    if (!conn) return;

    const xcb_query_extension_reply_t *randr_ext =
        xcb_get_extension_data(conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        _randrEventBase = randr_ext->first_event;

        XCBScreen *screen = [[connection screens] firstObject];
        if (screen) {
            xcb_window_t rootWindow = [screen screen]->root;
            xcb_randr_select_input(conn, rootWindow,
                XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE);
            [connection flush];
        }
    }
}

- (void)handleRandrGeometryChange:(uint16_t)newW height:(uint16_t)newH
{
    XCBScreen *screen = [[connection screens] firstObject];
    if (!screen) return;

    if ([screen width] == newW && [screen height] == newH)
        return;

    NSLog(@"[WindowManager] Screen size changed: %ux%u -> %ux%u",
          [screen width], [screen height], newW, newH);

    [screen setWidth:newW];
    [screen setHeight:newH];
    // Most of the window manager reads the size from the connection setup's
    // screen, so that has to follow too.
    [screen screen]->width_in_pixels = newW;
    [screen screen]->height_in_pixels = newH;

    xcb_connection_t *conn = [connection connection];

    // Update _NET_DESKTOP_GEOMETRY
    {
        EWMHService *ewmh = [EWMHService sharedInstanceWithConnection:connection];
        XCBWindow *scrRootWin = [screen rootWindow];
        uint32_t geom[2] = {newW, newH};
        [ewmh changePropertiesForWindow:scrRootWin
                               withMode:XCB_PROP_MODE_REPLACE
                           withProperty:[ewmh EWMHDesktopGeometry]
                               withType:XCB_ATOM_CARDINAL
                             withFormat:32
                         withDataLength:2
                               withData:geom];
    }

    // The workarea: the new screen, and WindowServer's usable area on it
    // (Xnextbsd has it on the root window by the time this event comes).
    [self.workareaManager refreshUsableArea];
    [self.workareaManager recalculateWorkarea];
    connection.workareaValid = NO;

    // Frames that no longer fit on the screen move back into the usable
    // area, as NSWindow's constrainFrameRect:toScreen: keeps windows on a
    // screen that shrank; frames still on the screen stay where they are.
    // WindowServer has already moved the ones that didn't fit just onto
    // the new screen (Xnextbsd moves the X window to match, and that
    // ConfigureNotify comes after this event), so a frame is moved when
    // either where it was (the cached rect) or where it is now (asked for
    // again) doesn't fit, and it is moved from where it is now.
    NSRect area = [self.workareaManager currentWorkarea];
    int areaX = (int)area.origin.x, areaY = (int)area.origin.y;
    int areaR = areaX + (int)area.size.width, areaB = areaY + (int)area.size.height;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn,
        xcb_query_tree(conn, [screen screen]->root), NULL);
    if (tree) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        int n = xcb_query_tree_children_length(tree);
        for (int i = 0; i < n; i++) {
            XCBWindow *w = [connection windowForXCBId:children[i]];
            if (!w || !([w isKindOfClass:[XCBFrame class]] || ![w decorated]))
                continue;
            // Frames, shown or not; other windows of ours only when they are
            // shown and managed (not the window manager's own helpers).
            if (![w isKindOfClass:[XCBFrame class]]) {
                xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(
                    conn, xcb_get_window_attributes(conn, children[i]), NULL);
                BOOL shown = a && a->map_state == XCB_MAP_STATE_VIEWABLE &&
                             !a->override_redirect;
                free(a);
                if (!shown) continue;
            }
            xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn,
                xcb_get_geometry(conn, children[i]), NULL);
            if (!g) continue;
            int x = g->x, y = g->y;
            int width = g->width + 2 * g->border_width;
            int height = g->height + 2 * g->border_width;
            free(g);
            XCBRect was = [w windowRect];
            BOOL fitsNow = x >= 0 && y >= 0 && x + width <= newW && y + height <= newH;
            BOOL fitted = was.position.x >= 0 && was.position.y >= 0 &&
                          was.position.x + (int)was.size.width <= newW &&
                          was.position.y + (int)was.size.height <= newH;
            if (fitsNow && fitted)
                continue;
            int nx = x, ny = y;
            if (nx + width > areaR) nx = areaR - width;
            if (nx < areaX) nx = areaX;
            if (ny + height > areaB) ny = areaB - height;
            if (ny < areaY) ny = areaY;
            if (nx == x && ny == y)
                continue;
            uint32_t vals[2] = {(uint32_t)(int32_t)nx, (uint32_t)(int32_t)ny};
            xcb_configure_window(conn, children[i],
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
            XCBRect r = [w windowRect];
            r.position.x = (int16_t)nx;
            r.position.y = (int16_t)ny;
            [w setWindowRect:r];
        }
        free(tree);
    }

    [connection flush];
}

- (void)handleExtensionEvent:(xcb_generic_event_t*)event
{
    uint8_t responseType = event->response_type & ~0x80;

    // RANDR events are handled regardless of compositing state
    if (_randrEventBase > 0) {
        if (responseType == _randrEventBase + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            xcb_randr_screen_change_notify_event_t *rEvent =
                (xcb_randr_screen_change_notify_event_t *)event;
            [self handleRandrGeometryChange:rEvent->width height:rEvent->height];
            return;
        }
        if (responseType == _randrEventBase + XCB_RANDR_NOTIFY) {
            xcb_connection_t *conn = [connection connection];
            XCBScreen *screen = [[connection screens] firstObject];
            if (screen && conn) {
                xcb_window_t root = [screen screen]->root;
                xcb_get_geometry_cookie_t gc =
                    xcb_get_geometry(conn, root);
                xcb_get_geometry_reply_t *geom =
                    xcb_get_geometry_reply(conn, gc, NULL);
                if (geom) {
                    [self handleRandrGeometryChange:geom->width
                                             height:geom->height];
                    free(geom);
                }
            }
            return;
        }
    }
}

#pragma mark - GSTheme Integration (NEW)

- (void)handleFocusChange:(xcb_window_t)windowId isActive:(BOOL)isActive {
    @try {
        // When any window gains focus, IMMEDIATELY mark the previously-focused
        // window's titlebar as inactive — before we try to resolve the incoming
        // window.  If the incoming FocusIn targets a window the WM doesn't track
        // (e.g. an undecorated popup), the resolution below fails and we'd return
        // early, leaving the old window stuck with active decorations forever.
        if (isActive) {
            xcb_window_t prevFocusedId = self.focusManager.lastFocusedWindowId;
            if (prevFocusedId != XCB_NONE && prevFocusedId != windowId) {
                [self handleFocusChange:prevFocusedId isActive:NO];
            }
        }

        // Find the window that received focus change
        XCBWindow *window = [connection windowForXCBId:windowId];
        if (!window) {
            // The focus event might be for a client window - search all frames
            NSDictionary *windowsMap = [connection windowsMap];
            for (NSString *mapWindowId in windowsMap) {
                XCBWindow *mapWindow = [windowsMap objectForKey:mapWindowId];
                if (mapWindow && [mapWindow isKindOfClass:[XCBFrame class]]) {
                    XCBFrame *testFrame = (XCBFrame*)mapWindow;
                    XCBWindow *clientWindow = [testFrame childWindowForKey:ClientWindow];
                    if (clientWindow && [clientWindow window] == windowId) {
                        window = testFrame;
                        break;
                    }
                }
            }
            // Second pass: check if windowId matches any frame directly
            if (!window) {
                for (NSString *mapWindowId in windowsMap) {
                    XCBWindow *mapWindow = [windowsMap objectForKey:mapWindowId];
                    if (mapWindow && [mapWindow isKindOfClass:[XCBFrame class]] &&
                        [mapWindow window] == windowId) {
                        window = mapWindow;
                        break;
                    }
                }
            }
            if (!window) {
                // Last resort: try to resolve via the focus manager which
                // maintains client→frame mappings
                xcb_window_t resolvedFrameId = XCB_NONE;
                XCBWindow *clientWin = [self.focusManager windowForClientWindowId:windowId];
                if (clientWin) {
                    if ([[clientWin parentWindow] isKindOfClass:[XCBFrame class]]) {
                        resolvedFrameId = [[clientWin parentWindow] window];
                    }
                    if (resolvedFrameId != XCB_NONE) {
                        window = [connection windowForXCBId:resolvedFrameId];
                    }
                }
            }
            if (!window) {
                NSLog(@"handleFocusChange: Could not resolve window %u for focus event", windowId);

                // If we deactivated the previous window above (isActive == YES)
                // but can't find the new one, clear lastFocusedWindowId so stale
                // focus doesn't linger.
                if (isActive) {
                    self.focusManager.lastFocusedWindowId = XCB_NONE;
                }
                return;
            }
        }

        // Find the frame and titlebar
        XCBFrame *frame = nil;
        XCBTitleBar *titlebar = nil;

        if ([window isKindOfClass:[XCBFrame class]]) {
            frame = (XCBFrame*)window;
        } else if ([window isKindOfClass:[XCBTitleBar class]]) {
            titlebar = (XCBTitleBar*)window;
            frame = (XCBFrame*)[titlebar parentWindow];
        } else if ([window parentWindow] && [[window parentWindow] isKindOfClass:[XCBFrame class]]) {
            frame = (XCBFrame*)[window parentWindow];
        }

        if (frame) {
            XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
            if (titlebarWindow && [titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
                titlebar = (XCBTitleBar*)titlebarWindow;
            }
        }

        if (!titlebar) {
            //NSLog(@"handleFocusChange: No titlebar found for window %u", windowId);
            // If we deactivated the previous window above (isActive == YES)
            // but the current window has no titlebar, still clear stale focus.
            if (isActive) {
                self.focusManager.lastFocusedWindowId = XCB_NONE;
            }
            return;
        }

        //NSLog(@"GSTheme: Focus %@ for window %@", isActive ? @"gained" : @"lost", titlebar.windowTitle);

        if (isActive) {
            XCBWindow *clientWindow = [self.focusManager clientWindowForWindow:window fallbackFrame:frame];
            if (clientWindow) {
                xcb_window_t clientId = [clientWindow window];
                [self.focusManager trackFocusGain:clientId];
            }
        }

        // Re-render titlebar with GSTheme using the correct active/inactive state
        [URSThemeIntegration renderGSThemeToWindow:frame
                                             frame:frame
                                             title:[titlebar windowTitle]
                                            active:isActive];

        // Update background pixmap and redraw
        [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
        [titlebar drawArea:[titlebar windowRect]];
        [connection flush];
        

    } @catch (NSException *exception) {
        NSLog(@"Exception in handleFocusChange: %@", exception.reason);
    }
}

- (void)themePreferenceDidChange:(NSNotification *)notification {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    [defaults synchronize];

    NSString *name = [defaults stringForKey:@"GSTheme"];
    if ([name length] == 0) {
        name = @"GNUstep";
    } else if ([[name pathExtension] isEqualToString:@"theme"]) {
        name = [name stringByDeletingPathExtension];
    }
    NSLog(@"[WindowManager] Theme preference changed to '%@' (active: '%@')",
          name, [[GSTheme theme] name]);

    // Same rule as +[GSTheme defaultsDidChange:]; setTheme: posts
    // GSThemeDidActivateNotification, which redraws all decorations.
    if (![[name lastPathComponent] isEqualToString:[[GSTheme theme] name]]) {
        [GSTheme setTheme:[GSTheme loadThemeNamed:name]];
    }
}

- (void)themeDidActivate:(NSNotification *)notification {
    NSLog(@"[WindowManager] GSTheme '%@' activated; redrawing decorations (zoom image: %d, stock close image: %d)",
          [[GSTheme theme] name],
          [URSDecorationMetrics themeProvidesImageNamed:@"common_Zoom"],
          [URSDecorationMetrics themeProvidesImageNamed:@"common_Close"]);
    // Redraw on the next run loop pass so NSColor and friends have processed
    // the same notification and report the new theme's values.
    [self performSelector:@selector(refreshAllManagedWindows) withObject:nil afterDelay:0];
}

- (void)refreshAllManagedWindows {
    NSDictionary *windowsMap = [connection windowsMap];
    for (NSString *mapWindowId in windowsMap) {
        XCBWindow *mapWindow = [windowsMap objectForKey:mapWindowId];
        if (![mapWindow isKindOfClass:[XCBFrame class]]) {
            continue;
        }
        XCBFrame *frame = (XCBFrame *)mapWindow;
        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (![titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
            continue;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)titlebarWindow;
        // Title bar / resize bar sizes may differ in the new theme
        [frame relayoutForCurrentTheme];

        BOOL isActive = [self.focusManager isFrameFocused:frame];
        uint32_t borderPixel = [URSDecorationMetrics borderPixel];
        if (![frame use32BitDepth])
            borderPixel &= 0x00FFFFFF;
        xcb_change_window_attributes([connection connection], [frame window],
                                     XCB_CW_BACK_PIXEL, &borderPixel);
        xcb_clear_area([connection connection], 0, [frame window], 0, 0, 0, 0);

        if ([URSThemeIntegration renderGSThemeToWindow:frame
                                                 frame:frame
                                                 title:[titlebar windowTitle]
                                                active:isActive]) {
            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
            [titlebar drawArea:[titlebar windowRect]];
        }
        [frame renderResizeBar];
        [frame applyCornerShape];
    }
    [connection flush];
}

- (void)handleTitlebarExpose:(xcb_expose_event_t*)exposeEvent {
    @try {
        URSThemeIntegration *integration = [URSThemeIntegration sharedInstance];
        if (!integration.enabled) {
            return;
        }

        xcb_window_t exposedWindow = exposeEvent->window;

        XCBWindow *exposed = [connection windowForXCBId:exposedWindow];
        if (![exposed isKindOfClass:[XCBTitleBar class]] ||
            ![[exposed parentWindow] isKindOfClass:[XCBFrame class]]) {
            return;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)exposed;
        XCBFrame *frame = (XCBFrame *)[titlebar parentWindow];

        BOOL exposeIsActive = [self.focusManager isFrameFocused:frame];

        // Draw the pixmap into the window backing store so the compositor
        // captures themed content on its next paint.
        if ([URSThemeIntegration renderGSThemeToWindow:frame
                                                 frame:frame
                                                 title:titlebar.windowTitle
                                                active:exposeIsActive]) {
            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
            [titlebar drawArea:[titlebar windowRect]];
        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in titlebar expose handler: %@", exception.reason);
    }
}

- (void)adjustBorderForFixedSizeWindow:(xcb_window_t)clientWindowId {
    @try {
        // Check if window has fixed size (min == max in WM_NORMAL_HINTS)
        xcb_size_hints_t sizeHints;
        if (xcb_icccm_get_wm_normal_hints_reply([connection connection],
                                                 xcb_icccm_get_wm_normal_hints([connection connection], clientWindowId),
                                                 &sizeHints,
                                                 NULL)) {
            if ((sizeHints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                (sizeHints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
                sizeHints.min_width == sizeHints.max_width &&
                sizeHints.min_height == sizeHints.max_height) {

                //NSLog(@"Fixed-size window %u detected - removing border and extra buttons", clientWindowId);

                // Register as fixed-size window (for button hiding in GSTheme rendering)
                [URSThemeIntegration registerFixedSizeWindow:clientWindowId];

                // Also mark client window as non-resizable so WM won't offer resize or attempt programmatic resizes
                XCBWindow *clientW = [connection windowForXCBId:clientWindowId];
                if (clientW) {
                    [clientW setCanResize:NO];
                    //NSLog(@"Marked client window %u as non-resizable (canResize=NO)", clientWindowId);
                }

                // Find the frame for this client window and set its border to 0
                NSDictionary *windowsMap = [connection windowsMap];
                for (NSString *mapWindowId in windowsMap) {
                    XCBWindow *window = [windowsMap objectForKey:mapWindowId];

                    if (window && [window isKindOfClass:[XCBFrame class]]) {
                        XCBFrame *frame = (XCBFrame*)window;
                        XCBWindow *clientWindow = [frame childWindowForKey:ClientWindow];

                        if (clientWindow && [clientWindow window] == clientWindowId) {
                            // Set the frame's border width to 0
                            uint32_t borderWidth[] = {0};
                            xcb_configure_window([connection connection],
                                                 [frame window],
                                                 XCB_CONFIG_WINDOW_BORDER_WIDTH,
                                                 borderWidth);
                            [connection flush];
                            //NSLog(@"Removed border from frame %u for fixed-size window %u", [frame window], clientWindowId);
                            return;
                        }
                    }
                }
            }
        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in adjustBorderForFixedSizeWindow: %@", exception.reason);
    }
}

- (void)resizeWindowTo70Percent:(xcb_window_t)clientWindowId {
    @try {
        // If the window is already managed by us (already decorated or currently minimized),
        // we must respect its existing geometry and state. Restoration from minimized state
        // is handled precisely by XCBConnection's handleMapRequest during the map sequence.
        XCBWindow *existingWindow = [connection windowForXCBId:clientWindowId];
        if (existingWindow && ([existingWindow decorated] || [existingWindow isMinimized])) {
            //NSLog(@"[WindowManager] Skipping automatic resize for already-managed window %u (decorated=%d, minimized=%d)", 
                  //clientWindowId, [existingWindow decorated], [existingWindow isMinimized]);
            return;
        }

        // Get the screen dimensions
        XCBScreen *screen = [[connection screens] objectAtIndex:0];
        uint16_t screenWidth = [screen width];
        uint16_t screenHeight = [screen height];
        
        // Get the current workarea (respects struts from dock windows like menu bar)
        NSRect workarea = [self.workareaManager currentWorkarea];
        
        // Golden ratio positioning (0.618) within the workarea
        // Position window at (1 - φ) ≈ 0.382 to lean left and top
        uint16_t goldenPosX = (uint16_t)(workarea.origin.x + workarea.size.width * 0.382);
        uint16_t goldenPosY = (uint16_t)(workarea.origin.y + workarea.size.height * 0.382);
        
        // Get current geometry to check if resizing is needed
        xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry([connection connection], clientWindowId);
        xcb_get_geometry_reply_t *geom_reply = xcb_get_geometry_reply([connection connection], geom_cookie, NULL);
        
        if (geom_reply) {
            // Respect ICCCM WM_NORMAL_HINTS: if the client is fixed-size, do not apply WM defaults
            xcb_size_hints_t sizeHints;
            if (xcb_icccm_get_wm_normal_hints_reply([connection connection],
                                                    xcb_icccm_get_wm_normal_hints([connection connection], clientWindowId),
                                                    &sizeHints,
                                                    NULL)) {
                if ((sizeHints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
                    (sizeHints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) &&
                    sizeHints.min_width == sizeHints.max_width &&
                    sizeHints.min_height == sizeHints.max_height) {
                    //NSLog(@"resizeWindowTo70Percent: client %u is fixed-size; skipping WM defaults", clientWindowId);
                    free(geom_reply);
                    return;
                }
            }

            
            // Check window type
            EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];
            XCBWindow *queryWindow = [[XCBWindow alloc] initWithXCBWindow:clientWindowId andConnection:connection];

            void *windowTypeReply = [ewmhService getProperty:[ewmhService EWMHWMWindowType]
                                                propertyType:XCB_ATOM_ATOM
                                                   forWindow:queryWindow
                                                      delete:NO
                                                      length:1];
            
            BOOL isDesktopWindow = NO;
            BOOL isDialogWindow = NO;
            if (windowTypeReply) {
                xcb_atom_t *atom = (xcb_atom_t *) xcb_get_property_value(windowTypeReply);
                if (atom) {
                    xcb_atom_t desktopAtom = [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHWMWindowTypeDesktop]];
                    xcb_atom_t dialogAtom = [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHWMWindowTypeDialog]];
                    if (*atom == desktopAtom) {
                        isDesktopWindow = YES;
                    } else if (*atom == dialogAtom) {
                        isDialogWindow = YES;
                    }
                }
                free(windowTypeReply);
            }
            
            // Check if window has fullscreen state
            BOOL isFullscreenState = NO;
            void *stateReply = [ewmhService getProperty:[ewmhService EWMHWMState]
                                           propertyType:XCB_ATOM_ATOM
                                              forWindow:queryWindow
                                                 delete:NO
                                                 length:UINT32_MAX];
            
            if (stateReply) {
                xcb_atom_t *atoms = (xcb_atom_t *) xcb_get_property_value(stateReply);
                uint32_t length = xcb_get_property_value_length(stateReply);
                xcb_atom_t fullscreenAtom = [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHWMStateFullscreen]];
                
                for (uint32_t i = 0; i < length; i++) {
                    if (atoms[i] == fullscreenAtom) {
                        isFullscreenState = YES;
                        break;
                    }
                }
                free(stateReply);
            }
            
            queryWindow = nil;
            // Clamp overly large windows before mapping.
            // Rule: if either dimension exceeds 90% of screen, resize both dimensions to 80%
            BOOL exceedsNinetyPercent =
                ((uint32_t)geom_reply->width * 100 > (uint32_t)screenWidth * 90) ||
                ((uint32_t)geom_reply->height * 100 > (uint32_t)screenHeight * 90);

            if (!isDesktopWindow && !isFullscreenState && exceedsNinetyPercent) {
                uint16_t clampedWidth = (uint16_t)(screenWidth * 0.8);
                uint16_t clampedHeight = (uint16_t)(screenHeight * 0.8);

                // Per HIG: place resized windows toward top-left so desktop status affordances
                // (such as volume icons) remain visible and unobstructed.
                uint16_t defaultX = isDialogWindow ? goldenPosX : 22;
                uint16_t defaultY = isDialogWindow ? goldenPosY : 44;
                uint32_t sizeValues[] = {defaultX, defaultY, clampedWidth, clampedHeight};
                xcb_configure_window([connection connection],
                                     clientWindowId,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                                     sizeValues);
                [connection flush];
                //NSLog(@"Window %u exceeds 90%% of screen (%ux%u). Clamped to 80%% (%ux%u) and placed at (%u,%u) before map.",
                      //clientWindowId,
                      //geom_reply->width,
                      //geom_reply->height,
                      //clampedWidth,
                    //clampedHeight,
                    //defaultX,
                    //defaultY);
            }

            // Only apply WM default placement if:
            // 1. Window is positioned at (0,0) - indicates no app positioning
            // 2. AND window is not a desktop window
            // 3. AND window is not explicitly requesting fullscreen
            BOOL isAtOrigin = (geom_reply->x == 0 && geom_reply->y == 0);
            BOOL isFullScreenSize = (geom_reply->width >= screenWidth && geom_reply->height >= screenHeight);
            
            if (isAtOrigin && (geom_reply->width < screenWidth) && !isDesktopWindow && !isFullscreenState) {
                // Window starts at (0,0) but is NOT full-width. This is usually a fallback position
                // for apps that don't specify geometry. Move it to a suitable default position:
                // dialogs get centered (golden ratio), other windows get 22,44 offset.
                uint16_t defaultX = isDialogWindow ? goldenPosX : 22;
                uint16_t defaultY = isDialogWindow ? goldenPosY : 44;
                //NSLog(@"Window %u starts at origin (0,0) but is not full-width (%u). Applying default placement (%u,%u) to avoid x=0 default.",
                      //clientWindowId, geom_reply->width, defaultX, defaultY);
                
                uint32_t configValues[] = {defaultX, defaultY};
                xcb_configure_window([connection connection],
                                     clientWindowId,
                                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                                     configValues);
                [connection flush];
            } else if (isDesktopWindow || isFullscreenState) {
                //NSLog(@"Window %u is desktop or fullscreen window. Skipping WM defaults (isDesktop=%d, isFullscreen=%d)",
                      //clientWindowId, isDesktopWindow, isFullscreenState);
            } else if (isAtOrigin && isFullScreenSize) {
                //NSLog(@"Window %u is exactly full screen size at origin; skipping >90%% clamp per 100%% exception.",
                      //clientWindowId);
            } else {
                //NSLog(@"Window %u has app-determined geometry (%ux%u at %d,%d). Respecting app preferences",
                      //clientWindowId, geom_reply->width, geom_reply->height, geom_reply->x, geom_reply->y);
            }
            free(geom_reply);
        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in resizeWindowTo70Percent: %@", exception.reason);
    }
}

- (void)applyGSThemeToRecentlyMappedWindow:(NSNumber*)windowIdNumber {
    @try {
        xcb_window_t windowId = [windowIdNumber unsignedIntValue];

        //NSLog(@"Applying GSTheme to recently mapped window: %u", windowId);

        // Find the frame for this client window
        NSDictionary *windowsMap = [self.connection windowsMap];

        for (NSString *mapWindowId in windowsMap) {
            XCBWindow *window = [windowsMap objectForKey:mapWindowId];

            if (window && [window isKindOfClass:[XCBFrame class]]) {
                XCBFrame *frame = (XCBFrame*)window;
                XCBWindow *clientWindow = [frame childWindowForKey:ClientWindow];

                // Check if this frame contains our client window
                if (clientWindow && [clientWindow window] == windowId) {
                    XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];

                    if (titlebarWindow && [titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
                        XCBTitleBar *titlebar = (XCBTitleBar*)titlebarWindow;

                        //NSLog(@"Found frame for client window %u, applying GSTheme to titlebar", windowId);

                        // Apply GSTheme rendering (this will override XCBKit's decoration).
                        // Newly mapped windows almost always get focus, so default active.
                        BOOL success = [URSThemeIntegration renderGSThemeToWindow:window
                                                                             frame:frame
                                                                             title:titlebar.windowTitle
                                                                            active:YES];

                        if (success) {

                            //NSLog(@"Successfully applied GSTheme to titlebar for window %u: %@",
                                  //windowId, titlebar.windowTitle ?: @"(untitled)");

                            // Paint the GSTheme content into the titlebar's backing store NOW,
                            // before the compositor takes its first NameWindowPixmap snapshot.
                            // Without this, the compositor may capture the blank initial state
                            // (no drawArea has been called yet) and show a flash of undecorated
                            // content before the first Expose-driven redraw arrives.
                            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
                            [titlebar drawArea:[titlebar windowRect]];
                            [self.connection flush];


                            // Auto-focus the client window - the frame and titlebar are now fully set up
                            // Focus after a small delay to ensure the window is properly rendered and ready
                            [self performSelector:@selector(focusWindowAfterThemeApplied:)
                                       withObject:clientWindow
                                       afterDelay:0.1];
                        } else {
                            NSLog(@"Failed to apply GSTheme to titlebar for window %u", windowId);
                        }

                        return; // Found and processed
                    }
                }
            }
        }

        // If we couldn't find a frame, the window may be undecorated (dialogs, alerts, sheets, docks).
        // Undecorated windows have no titlebars, so GSTheme is not applicable — skip silently.
        XCBWindow *directWindow = [self.connection windowForXCBId:windowId];
        if (directWindow) {
            if (![directWindow decorated]) return;

            // Attempt a direct focus on the client window as a fallback.
            if ([self.focusManager isWindowFocusable:directWindow allowDesktop:NO]) {
                [self performSelector:@selector(focusWindowAfterThemeApplied:)
                           withObject:directWindow
                           afterDelay:0.1];
                return;
            }
        }

        NSLog(@"Could not find frame for client window %u", windowId);

    } @catch (NSException *exception) {
        NSLog(@"Exception applying GSTheme to recently mapped window: %@", exception.reason);
    }
}

- (void)reapplyGSThemeToTitlebar:(XCBTitleBar*)titlebar {
    @try {
        if (!titlebar) return;

        //NSLog(@"Reapplying GSTheme to titlebar: %@", titlebar.windowTitle);

        // Find the frame containing this titlebar
        NSDictionary *windowsMap = [self.connection windowsMap];

        for (NSString *windowId in windowsMap) {
            XCBWindow *window = [windowsMap objectForKey:windowId];

            if (window && [window isKindOfClass:[XCBFrame class]]) {
                XCBFrame *frame = (XCBFrame*)window;
                XCBWindow *frameTitle = [frame childWindowForKey:TitleBar];

                if (frameTitle && frameTitle == titlebar) {
                    // Determine whether this window actually has keyboard focus
                    BOOL reapplyIsActive = [self.focusManager isFrameFocused:frame];

                    // Reapply GSTheme rendering
                    [URSThemeIntegration renderGSThemeToWindow:window
                                                         frame:frame
                                                         title:titlebar.windowTitle
                                                        active:reapplyIsActive];
                    //NSLog(@"GSTheme reapplied to titlebar: %@", titlebar.windowTitle);
                    
                    return;
                }
            }
        }

        NSLog(@"Could not find frame for titlebar reapplication");

    } @catch (NSException *exception) {
        NSLog(@"Exception in GSTheme reapplication: %@", exception.reason);
    }
}

#pragma mark - Spatial Path Popup (modifier+click on titlebar)

/* Read a UTF-8 string property from an X11 window */

/* Handle a modifier+click on a titlebar that has _GW_SPATIAL_PATH set.
 * Returns YES if the event was consumed (popup shown). */

/* Called when the user selects a path from the spatial path popup */

#pragma mark - Cleanup

- (void)cleanupRootWindowEventMask {
    //NSLog(@"[WindowManager] Cleaning up root window event mask");
    
    @try {
        XCBScreen *screen = [[connection screens] objectAtIndex:0];
        XCBWindow *rootWindow = [[XCBWindow alloc] initWithXCBWindow:[[screen rootWindow] window] 
                                                        andConnection:connection];
        
        uint32_t values[1];
        values[0] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        
        BOOL success = [rootWindow changeAttributes:values 
                                           withMask:XCB_CW_EVENT_MASK 
                                            checked:NO];
        
        if (success) {
            //NSLog(@"[WindowManager] Successfully restored root window event mask");
        } else {
            NSLog(@"[WindowManager] Warning: Failed to restore root window event mask");
        }
        
        [connection flush];
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception in cleanupRootWindowEventMask: %@", exception.reason);
    }
}

- (void)cleanupBeforeExit
{
    //NSLog(@"[WindowManager] ========== Starting comprehensive cleanup ==========");
    
    @try {
        
        
        // Step 2: Undecorate and restore all client windows
        //NSLog(@"[WindowManager] Step 2: Restoring all client windows");
        [self undecoratAllWindows];
        
        // Step 3: Clear EWMH properties
        //NSLog(@"[WindowManager] Step 3: Clearing EWMH properties");
        [self clearEWMHProperties];
        
        // Step 4: Release window manager selection ownership
        //NSLog(@"[WindowManager] Step 4: Releasing WM selection ownership");
        [self releaseWMSelection];
        
        // Step 5: Restore root window event mask
        //NSLog(@"[WindowManager] Step 5: Restoring root window event mask");
        [self cleanupRootWindowEventMask];
        
        // Step 6: Flush all changes to X server
        //NSLog(@"[WindowManager] Step 6: Flushing changes to X server");
        [connection flush];
        xcb_aux_sync([connection connection]);
        
        //NSLog(@"[WindowManager] ========== Cleanup completed successfully ==========");
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception during cleanup: %@", exception.reason);
    }
}

- (void)undecoratAllWindows
{
    @try {
        if (!connection) {
            //NSLog(@"[WindowManager] No connection available for window cleanup");
            return;
        }
        
        NSDictionary *windowsMap = [connection windowsMap];
        if (!windowsMap || [windowsMap count] == 0) {
            //NSLog(@"[WindowManager] No windows to clean up");
            return;
        }
        
        //NSLog(@"[WindowManager] Cleaning up %lu managed windows", (unsigned long)[windowsMap count]);
        
        XCBScreen *screen = [[connection screens] objectAtIndex:0];
        XCBWindow *rootWindow = [screen rootWindow];
        
        // Collect all frames first to avoid modifying dictionary while iterating
        NSMutableArray *framesToCleanup = [NSMutableArray array];
        
        for (NSString *windowId in windowsMap) {
            XCBWindow *window = [windowsMap objectForKey:windowId];
            if (window && [window isKindOfClass:[XCBFrame class]]) {
                [framesToCleanup addObject:window];
            }
        }
        
        //NSLog(@"[WindowManager] Found %lu frames to clean up", (unsigned long)[framesToCleanup count]);
        
        // Preserve original stacking order before undecorating.
        // Query the current X tree to get bottom-to-top order of managed windows.
        NSMutableArray *clientStackOrder = [NSMutableArray array];
        {
            xcb_query_tree_cookie_t treeCookie = xcb_query_tree([connection connection], [rootWindow window]);
            xcb_query_tree_reply_t *treeReply = xcb_query_tree_reply([connection connection], treeCookie, NULL);
            if (treeReply) {
                xcb_window_t *children = xcb_query_tree_children(treeReply);
                int numChildren = xcb_query_tree_children_length(treeReply);
                for (int i = 0; i < numChildren; i++) {
                    XCBWindow *win = [connection windowForXCBId:children[i]];
                    if ([win isKindOfClass:[XCBFrame class]]) {
                        XCBWindow *client = [(XCBFrame *)win childWindowForKey:ClientWindow];
                        if (client)
                            [clientStackOrder addObject:client];
                    } else if (win) {
                        [clientStackOrder addObject:win];
                    }
                }
                free(treeReply);
            }
        }
        
        // Clean up each frame
        for (XCBFrame *frame in framesToCleanup) {
            @try {
                XCBWindow *clientWindow = [frame childWindowForKey:ClientWindow];
                
                if (clientWindow) {
                    //NSLog(@"[WindowManager] Restoring client window %u", [clientWindow window]);

                    // Translate client coordinates to root-space before reparenting.
                    // clientWindow.windowRect is relative to the frame and causes
                    // incorrect placement if used directly for root reparent.
                    int16_t rootX = 0;
                    int16_t rootY = 0;
                    xcb_translate_coordinates_reply_t *translated =
                        xcb_translate_coordinates_reply([connection connection],
                                                       xcb_translate_coordinates([connection connection],
                                                                                 [clientWindow window],
                                                                                 [rootWindow window],
                                                                                 0,
                                                                                 0),
                                                       NULL);

                    if (translated) {
                        rootX = translated->dst_x;
                        rootY = translated->dst_y;
                        free(translated);
                    } else {
                        XCBRect frameRect = [frame windowRect];
                        XCBRect clientRect = [clientWindow windowRect];
                        rootX = frameRect.position.x + clientRect.position.x;
                        rootY = frameRect.position.y + clientRect.position.y;
                    }

                    // Reparent client back to root window
                    xcb_reparent_window([connection connection],
                                      [clientWindow window],
                                      [rootWindow window],
                                      rootX,
                                      rootY);
                    
                    // Unmap the frame (this hides the decorations)
                    xcb_unmap_window([connection connection], [frame window]);
                    
                    // Mark client as not decorated
                    [clientWindow setDecorated:NO];
                    
                    //NSLog(@"[WindowManager] Client window %u restored to root at %d,%d", [clientWindow window], rootX, rootY);
                }
                
                // Destroy the frame window (this will also clean up titlebar and buttons)
                xcb_destroy_window([connection connection], [frame window]);
                
            } @catch (NSException *exception) {
                NSLog(@"[WindowManager] Exception cleaning up frame %u: %@", [frame window], exception.reason);
            }
        }
        
        // Restore original stacking order for client windows.
        // After reparenting, each window ends up on top, reversing their order.
        // Apply XCB_STACK_MODE_BELOW from bottom to top to restore the original layout.
        for (XCBWindow *client in clientStackOrder) {
            uint32_t values[] = {XCB_STACK_MODE_BELOW};
            xcb_configure_window([connection connection], [client window],
                                 XCB_CONFIG_WINDOW_STACK_MODE, values);
        }
        
        [connection flush];
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception in undecoratAllWindows: %@", exception.reason);
    }
}

- (void)clearEWMHProperties
{
    @try {
        if (!connection) {
            //NSLog(@"[WindowManager] No connection available for EWMH cleanup");
            return;
        }
        
        XCBScreen *screen = [[connection screens] objectAtIndex:0];
        XCBWindow *rootWindow = [screen rootWindow];
        EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];
        
        //NSLog(@"[WindowManager] Clearing EWMH properties from root window");
        
        // Clear _NET_SUPPORTING_WM_CHECK
        xcb_delete_property([connection connection],
                          [rootWindow window],
                          [[ewmhService atomService] atomFromCachedAtomsWithKey:@"_NET_SUPPORTING_WM_CHECK"]);
        
        // Clear _NET_ACTIVE_WINDOW
        xcb_delete_property([connection connection],
                          [rootWindow window],
                          [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHActiveWindow]]);
        
        // Clear _NET_CLIENT_LIST
        xcb_delete_property([connection connection],
                          [rootWindow window],
                          [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHClientList]]);
        
        // Clear _NET_CLIENT_LIST_STACKING
        xcb_delete_property([connection connection],
                          [rootWindow window],
                          [[ewmhService atomService] atomFromCachedAtomsWithKey:[ewmhService EWMHClientListStacking]]);
        
        [connection flush];
        //NSLog(@"[WindowManager] EWMH properties cleared");
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception clearing EWMH properties: %@", exception.reason);
    }
}

- (void)releaseWMSelection
{
    @try {
        if (!connection) {
            //NSLog(@"[WindowManager] No connection available for selection release");
            return;
        }
        
        //NSLog(@"[WindowManager] Releasing WM_S0 selection ownership");
        
        XCBAtomService *atomService = [XCBAtomService sharedInstanceWithConnection:connection];
        xcb_atom_t wmS0Atom = [atomService cacheAtom:@"WM_S0"];
        
        // Set selection owner to None (releases ownership)
        xcb_set_selection_owner([connection connection],
                               XCB_NONE,
                               wmS0Atom,
                               XCB_CURRENT_TIME);
        
        [connection flush];
        
    } @catch (NSException *exception) {
        NSLog(@"[WindowManager] Exception releasing WM selection: %@", exception.reason);
    }
}

- (void)handleSelectionClear:(xcb_selection_clear_event_t *)event
{
    XCBAtomService *atomService = [XCBAtomService sharedInstanceWithConnection:connection];
    xcb_atom_t wmS0Atom = [atomService cacheAtom:@"WM_S0"];
    
    // Check if this is the WM_S0 selection being cleared (we're being replaced)
    if (event->selection == wmS0Atom) {
        //NSLog(@"[WindowManager] WM_S0 selection cleared - another WM is taking over");
        //NSLog(@"[WindowManager] Timestamp: %u, Owner: %u", event->time, event->owner);
        
        // Initiate clean shutdown
        [self cleanupBeforeExit];
        
        // Destroy our selection window if we have one
        if (selectionManagerWindow) {
            xcb_destroy_window([connection connection], [selectionManagerWindow window]);
            [connection flush];
            //NSLog(@"[WindowManager] Selection manager window destroyed");
        }
        
        // Terminate the application gracefully
        //NSLog(@"[WindowManager] Terminating to allow new WM to take over");
        [NSApp terminate:nil];
    } else {
        //NSString *selectionName = [atomService atomNameFromAtom:event->selection];
        ////NSLog(@"[WindowManager] SelectionClear for non-WM selection: %@", selectionName);
    }
}

#pragma mark - Window Title Updates

- (NSString *)readUTF8Property:(NSString *)propertyName forWindow:(XCBWindow *)window
{
    if (!propertyName || !window) {
        return nil;
    }

    XCBAtomService *atomService = [XCBAtomService sharedInstanceWithConnection:connection];
    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];

    xcb_atom_t propertyAtom = [atomService atomFromCachedAtomsWithKey:propertyName];
    if (propertyAtom == XCB_ATOM_NONE) {
        propertyAtom = [atomService cacheAtom:propertyName];
    }

    xcb_atom_t utf8Atom = [atomService atomFromCachedAtomsWithKey:[ewmhService UTF8_STRING]];
    if (utf8Atom == XCB_ATOM_NONE) {
        utf8Atom = [atomService cacheAtom:[ewmhService UTF8_STRING]];
    }

    xcb_get_property_cookie_t cookie = xcb_get_property([connection connection],
                                                         0,
                                                         [window window],
                                                         propertyAtom,
                                                         utf8Atom,
                                                         0,
                                                         1024);
    xcb_generic_error_t *propError = NULL;
    xcb_get_property_reply_t *reply = xcb_get_property_reply([connection connection], cookie, &propError);
    if (propError)
    {
        free(propError);
        return nil;
    }
    if (!reply) {
        return nil;
    }

    int length = xcb_get_property_value_length(reply);
    if (length <= 0) {
        free(reply);
        return nil;
    }

    const char *bytes = (const char *)xcb_get_property_value(reply);
    NSString *value = [[NSString alloc] initWithBytes:bytes length:(NSUInteger)length encoding:NSUTF8StringEncoding];
    free(reply);
    return value;
}

- (NSString *)titleForClientWindow:(XCBWindow *)clientWindow
{
    if (!clientWindow) {
        return @"";
    }

    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];

    NSString *title = [self readUTF8Property:[ewmhService EWMHWMVisibleName] forWindow:clientWindow];
    if (!title || [title length] == 0) {
        title = [self readUTF8Property:[ewmhService EWMHWMName] forWindow:clientWindow];
    }

    if (!title || [title length] == 0) {
        ICCCMService *icccmService = [ICCCMService sharedInstanceWithConnection:connection];
        title = [icccmService getWmNameForWindow:clientWindow];
    }

    if (!title) {
        title = @"";
    }

    return title;
}

- (void)handleWindowTitlePropertyChange:(xcb_property_notify_event_t*)event
{
    if (!event) {
        return;
    }

    XCBAtomService *atomService = [XCBAtomService sharedInstanceWithConnection:connection];
    EWMHService *ewmhService = [EWMHService sharedInstanceWithConnection:connection];
    ICCCMService *icccmService = [ICCCMService sharedInstanceWithConnection:connection];

    NSString *atomName = [atomService atomNameFromAtom:event->atom];
    if (!atomName) {
        return;
    }

    BOOL isWmName = [atomName isEqualToString:[icccmService WMName]];
    BOOL isNetWmName = [atomName isEqualToString:[ewmhService EWMHWMName]];
    BOOL isNetWmVisibleName = [atomName isEqualToString:[ewmhService EWMHWMVisibleName]];
    BOOL isGNUstepAttr = [atomName isEqualToString:[ewmhService GNUStepWmAttr]];

    if (!isWmName && !isNetWmName && !isNetWmVisibleName && !isGNUstepAttr) {
        return;
    }

    XCBWindow *eventWindow = [connection windowForXCBId:event->window];
    if (!eventWindow) {
        return;
    }

    XCBFrame *frame = nil;
    XCBTitleBar *titlebar = nil;
    XCBWindow *clientWindow = nil;

    if ([eventWindow isKindOfClass:[XCBFrame class]]) {
        frame = (XCBFrame *)eventWindow;
        clientWindow = [frame childWindowForKey:ClientWindow];
    } else if ([eventWindow isKindOfClass:[XCBTitleBar class]]) {
        titlebar = (XCBTitleBar *)eventWindow;
        frame = (XCBFrame *)[titlebar parentWindow];
        if (frame) {
            clientWindow = [frame childWindowForKey:ClientWindow];
        }
    } else if ([eventWindow parentWindow] && [[eventWindow parentWindow] isKindOfClass:[XCBFrame class]]) {
        frame = (XCBFrame *)[eventWindow parentWindow];
        clientWindow = [frame childWindowForKey:ClientWindow];
    } else {
        NSDictionary *windowsMap = [connection windowsMap];
        for (NSString *mapWindowId in windowsMap) {
            XCBWindow *mapWindow = [windowsMap objectForKey:mapWindowId];
            if (mapWindow && [mapWindow isKindOfClass:[XCBFrame class]]) {
                XCBFrame *testFrame = (XCBFrame *)mapWindow;
                XCBWindow *testClient = [testFrame childWindowForKey:ClientWindow];
                if (testClient && [testClient window] == event->window) {
                    frame = testFrame;
                    clientWindow = testClient;
                    break;
                }
            }
        }
    }

    if (frame && !titlebar) {
        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (titlebarWindow && [titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
            titlebar = (XCBTitleBar *)titlebarWindow;
        }
    }

    if (!titlebar) {
        return;
    }

    if (isGNUstepAttr) {
        // Style mask or document-edited state changed: redraw only if it matters
        if (![frame updateDecorationStyleFromClient]) {
            return;
        }
    }

    NSString *newTitle = isGNUstepAttr
        ? [titlebar windowTitle]
        : [self titleForClientWindow:(clientWindow ? clientWindow : eventWindow)];

    [titlebar setInternalTitle:newTitle];

    if ([[URSThemeIntegration sharedInstance] enabled]) {
        // Use the focus manager to determine active state — frame.isFocused
        // is never actually set anywhere, so it's always NO.
        BOOL isActive = [self.focusManager isFrameFocused:frame];
        [URSThemeIntegration renderGSThemeToWindow:frame
                                             frame:frame
                                             title:newTitle
                                            active:isActive];
        [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
        [titlebar drawArea:[titlebar windowRect]];
        [connection flush];
    } else {
        [titlebar setWindowTitle:newTitle];
        [titlebar drawArea:[titlebar windowRect]];
        [connection flush];
    }
}


#pragma mark - Cleanup

- (void)dealloc
{
    // Remove from run loop if integrated - must match all modes added in setupXCBEventIntegration
    if (self.xcbEventsIntegrated && connection) {
        int xcbFD = xcb_get_file_descriptor([connection connection]);
        if (xcbFD >= 0) {
            NSRunLoop *currentRunLoop = [NSRunLoop currentRunLoop];
            [currentRunLoop removeEvent:(void*)(uintptr_t)xcbFD
                                   type:ET_RDESC
                                forMode:NSDefaultRunLoopMode
                                   all:YES];
            [currentRunLoop removeEvent:(void*)(uintptr_t)xcbFD
                                   type:ET_RDESC
                                forMode:NSEventTrackingRunLoopMode
                                   all:YES];
            [currentRunLoop removeEvent:(void*)(uintptr_t)xcbFD
                                   type:ET_RDESC
                                forMode:NSModalPanelRunLoopMode
                                   all:YES];
        }
    }

    // Remove notification center observers
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    // ARC handles memory management automatically
}

- (void)focusWindowAfterThemeApplied:(XCBWindow *)clientWindow
{
    [self.focusManager focusWindowAfterThemeApplied:clientWindow];
    [self redrawFocusedTitlebar];
}

- (void)focusNewlyMappedWindow:(XCBWindow *)clientWindow
{
    [self.focusManager focusNewlyMappedWindow:clientWindow];
    [self redrawFocusedTitlebar];
}

// A new window got the focus without a FocusIn reaching us (its title bar
// was drawn, and maybe redrawn by an expose or a title change, before it
// had the focus): draw every title bar in its current state.
- (void)redrawFocusedTitlebar
{
    NSDictionary *windowsMap = [connection windowsMap];

    for (NSString *key in windowsMap) {
        XCBWindow *w = [windowsMap objectForKey:key];
        XCBTitleBar *titlebar;

        if (![w isKindOfClass:[XCBFrame class]])
            continue;
        titlebar = (XCBTitleBar *)[(XCBFrame *)w childWindowForKey:TitleBar];
        if (![titlebar isKindOfClass:[XCBTitleBar class]])
            continue;
        if ([URSThemeIntegration renderGSThemeToWindow:w
                                                 frame:(XCBFrame *)w
                                                 title:[titlebar windowTitle]
                                                active:[self.focusManager isFrameFocused:(XCBFrame *)w]]) {
            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
            [titlebar drawArea:[titlebar windowRect]];
        }
    }
    [connection flush];
}

- (void)removeWindowFromRecentlyFocused:(NSNumber *)windowIdNum
{
    [self.focusManager removeWindowFromRecentlyFocused:windowIdNum];
}

#pragma mark - XLibre's own windows

// The X window that had the focus when one of XLibre's GNUstep windows
// became key, or XCB_NONE.
static xcb_window_t appKitSavedFocus = XCB_NONE;
static BOOL appKitWindowKey = NO;

static xcb_window_t
currentInputFocus(xcb_connection_t *c)
{
    xcb_get_input_focus_reply_t *r =
        xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL);
    xcb_window_t w = r != NULL ? r->focus : XCB_NONE;

    free(r);
    return w;
}

- (xcb_window_t)rootWindowId
{
    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup([connection connection])).data;

    return screen != NULL ? screen->root : XCB_NONE;
}

// A GNUstep window of ours is key: WindowServer sends it the keys, so
// take X's focus off the X window (to the root window, which is in no
// frame, so Xnextbsd doesn't make a frame key for it) and draw that
// window's title bar inactive, as for any window that isn't key.
- (void)appKitWindowBecameKey
{
    xcb_connection_t *c = [connection connection];
    xcb_window_t root = [self rootWindowId], focused;

    if (c == NULL || root == XCB_NONE)
        return;
    appKitWindowKey = YES;
    focused = self.focusManager.lastFocusedWindowId;
    if (focused != XCB_NONE)
        appKitSavedFocus = focused;
    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, root,
                        XCB_CURRENT_TIME);
    if (focused != XCB_NONE)
        [self handleFocusChange:focused isActive:NO];
    self.focusManager.lastFocusedWindowId = XCB_NONE;
    [connection flush];
}

// An X window got the focus while one of our GNUstep windows was key: the
// user clicked it, WindowServer made its frame key and Xnextbsd focused
// it. WindowServer doesn't tell the GNUstep window it lost the key to
// another connection of our app, so it would stay key (and drawn key) in
// AppKit: resign it here.
- (void)xFocusInWhileAppKitWindowKey:(xcb_focus_in_event_t *)event
{
    if (!appKitWindowKey || event->event == [self rootWindowId] ||
        event->detail == XCB_NOTIFY_DETAIL_POINTER ||
        event->detail == XCB_NOTIFY_DETAIL_POINTER_ROOT ||
        event->detail == XCB_NOTIFY_DETAIL_NONE)
        return;
    appKitWindowKey = NO;
    appKitSavedFocus = XCB_NONE;
    [[NSApp keyWindow] resignKeyWindow];
}

// None of our GNUstep windows is key any more. If the X focus is still
// where we left it (the user didn't click another X window, which
// Xnextbsd then focuses itself) and XLibre is still the active app, give
// it back to the window that had it; its FocusIn redraws the title bar
// active, and Xnextbsd makes its frame key again.
- (void)appKitWindowsResignedKey
{
    xcb_connection_t *c = [connection connection];
    xcb_window_t saved = appKitSavedFocus;
    XCBWindow *client;

    if (!appKitWindowKey)
        return;
    appKitWindowKey = NO;
    appKitSavedFocus = XCB_NONE;
    if (c == NULL || saved == XCB_NONE || ![NSApp isActive] ||
        currentInputFocus(c) != [self rootWindowId])
        return;
    client = [self.focusManager windowForClientWindowId:saved];
    if (client == nil)
        client = [connection windowForXCBId:saved];
    if (client == nil || ![self.focusManager isWindowFocusable:client
                                                  allowDesktop:NO])
        return;
    [client focus];
    [self handleFocusChange:saved isActive:YES];
    [connection flush];
}

@end
