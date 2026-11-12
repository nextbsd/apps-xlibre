//
//  URSTitlebarController.m
//  uroswm - Titlebar Interaction Controller
//
//  Handles titlebar button hit-testing, hover state, button press actions
//  (close/minimize/maximize), and resize-during-motion rendering updates.
//

#import "URSTitlebarController.h"
#import "URSProfiler.h"
#import "URSThemeIntegration.h"
#import "URSFocusManager.h"
#import "URSDecorationMetrics.h"

@implementation URSTitlebarController

- (instancetype)initWithConnection:(XCBConnection *)aConnection
{
    self = [super init];
    if (!self) return nil;

    _connection = aConnection;

    return self;
}

#pragma mark - Button Hit Detection

- (NSInteger)buttonAtPoint:(NSPoint)point
               forTitlebar:(XCBTitleBar *)titlebar
{
    if (![[titlebar parentWindow] isKindOfClass:[XCBFrame class]]) {
        return -1;
    }
    XCBFrame *frame = (XCBFrame *)[titlebar parentWindow];
    XCBRect titlebarRect = [titlebar windowRect];
    NSSize size = NSMakeSize(titlebarRect.size.width, titlebarRect.size.height);

    return [URSDecorationMetrics buttonAtX11Point:point
                                     titleBarSize:size
                                        styleMask:[frame decorationStyleMask]];
}

// Pointer position relative to a title bar, for events delivered to another window
- (NSPoint)point:(NSPoint)eventPoint
      fromWindow:(xcb_window_t)eventWindow
      toTitlebar:(xcb_window_t)titlebarId
{
    if (eventWindow == titlebarId) {
        return eventPoint;
    }
    xcb_translate_coordinates_reply_t *reply =
        xcb_translate_coordinates_reply([self.connection connection],
            xcb_translate_coordinates([self.connection connection], eventWindow, titlebarId,
                                      (int16_t)eventPoint.x, (int16_t)eventPoint.y),
            NULL);
    if (!reply) {
        return NSMakePoint(-1, -1);
    }
    NSPoint p = NSMakePoint(reply->dst_x, reply->dst_y);
    free(reply);
    return p;
}

#pragma mark - Button Press Handling

- (BOOL)handleTitlebarButtonPress:(xcb_button_press_event_t *)pressEvent
{
    @try {
        XCBWindow *window = [self.connection windowForXCBId:pressEvent->event];
        if (!window || ![window isKindOfClass:[XCBTitleBar class]]) {
            return NO;
        }

        XCBTitleBar *titlebar = (XCBTitleBar *)window;

        // Only the primary button operates title bar buttons; right-click is
        // handled by the tiling menu controller.
        if (pressEvent->detail != 1) {
            return NO;
        }

        NSPoint clickPoint = NSMakePoint(pressEvent->event_x, pressEvent->event_y);
        NSInteger button = [self buttonAtPoint:clickPoint forTitlebar:titlebar];
        if (button < 0) {
            return NO;
        }

        // Release the implicit grab freeze; the release is still delivered to us
        xcb_allow_events([self.connection connection],
                         XCB_ALLOW_ASYNC_POINTER, pressEvent->time);

        [URSThemeIntegration setPressedTitlebar:[titlebar window]
                                         button:button
                                    highlighted:YES];
        [self redrawTitlebar:titlebar inFrame:(XCBFrame *)[titlebar parentWindow]];

        self.connection.dragState = NO;
        self.connection.resizeState = NO;
        [self.connection flush];
        return YES;

    } @catch (NSException *exception) {
        NSLog(@"Exception handling titlebar button press: %@", exception.reason);
        return NO;
    }
}

- (BOOL)handleTitlebarButtonRelease:(xcb_button_release_event_t *)releaseEvent
{
    xcb_window_t titlebarId = [URSThemeIntegration pressedTitlebarWindow];
    if (titlebarId == 0) {
        return NO;
    }
    NSInteger button = [URSThemeIntegration pressedButton];
    [URSThemeIntegration clearPressedState];

    @try {
        XCBWindow *window = [self.connection windowForXCBId:titlebarId];
        if (!window || ![window isKindOfClass:[XCBTitleBar class]]) {
            return YES;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)window;
        XCBFrame *frame = (XCBFrame *)[titlebar parentWindow];
        if (!frame || ![frame isKindOfClass:[XCBFrame class]]) {
            return YES;
        }

        NSPoint p = [self point:NSMakePoint(releaseEvent->event_x, releaseEvent->event_y)
                     fromWindow:releaseEvent->event
                     toTitlebar:titlebarId];
        BOOL inside = ([self buttonAtPoint:p forTitlebar:titlebar] == button);

        // Remove the highlight before acting (the window may go away)
        [self redrawTitlebar:titlebar inFrame:frame];

        if (!inside) {
            return YES;
        }

        XCBWindow *clientWindow = [frame childWindowForKey:ClientWindow];
        switch (button) {
            case NSWindowCloseButton:
                if (clientWindow) {
                    [clientWindow close];
                    [frame setNeedDestroy:YES];
                }
                break;

            case NSWindowMiniaturizeButton:
                [frame minimize];
                break;

            case NSWindowZoomButton:
                [self handleZoomForFrame:frame
                                titlebar:titlebar
                            clientWindow:clientWindow];
                break;

            default:
                break;
        }

        [self.connection flush];
    } @catch (NSException *exception) {
        NSLog(@"Exception handling titlebar button release: %@", exception.reason);
    }
    return YES;
}

- (void)handleZoomForFrame:(XCBFrame *)frame
                  titlebar:(XCBTitleBar *)titlebar
              clientWindow:(XCBWindow *)clientWindow
{
    if ([frame isMaximized]) {
        XCBRect startRect = [frame windowRect];
        XCBRect restoredRect = [frame oldRect];

        [frame programmaticResizeToRect:restoredRect];
        [frame setFullScreen:NO];
        [titlebar setFullScreen:NO];
        if (clientWindow) {
            [clientWindow setFullScreen:NO];
        }
        [frame setIsMaximized:NO];

        [titlebar destroyPixmap];
        [titlebar createPixmap];

        BOOL restoreIsActive = [self titlebarIsActiveForFrame:frame
                                                  clientWindow:clientWindow];
        [URSThemeIntegration renderGSThemeToWindow:frame
                                             frame:frame
                                             title:[titlebar windowTitle]
                                            active:restoreIsActive];

        [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
        [titlebar drawArea:[titlebar windowRect]];

        [frame updateAllResizeZonePositions];

        [self animateTransition:frame
                       fromRect:startRect
                         toRect:[frame windowRect]];
    } else {
        XCBRect startRect = [frame windowRect];

        [frame setOldRect:startRect];
        [titlebar setOldRect:[titlebar windowRect]];
        if (clientWindow) {
            [clientWindow setOldRect:[clientWindow windowRect]];
        }

        NSRect workarea = [self.workareaManager currentWorkarea];
        XCBRect targetRect = XCBMakeRect(
            XCBMakePoint((int32_t)workarea.origin.x,
                         (int32_t)workarea.origin.y),
            XCBMakeSize((uint32_t)workarea.size.width,
                        (uint32_t)workarea.size.height));

        [frame programmaticResizeToRect:targetRect];
        [frame setFullScreen:YES];
        [frame setIsMaximized:YES];
        [titlebar setFullScreen:YES];
        if (clientWindow) {
            [clientWindow setFullScreen:YES];
        }

        [titlebar destroyPixmap];
        [titlebar createPixmap];

        BOOL maximizeIsActive = [self titlebarIsActiveForFrame:frame
                                                  clientWindow:clientWindow];
        [URSThemeIntegration renderGSThemeToWindow:frame
                                             frame:frame
                                             title:[titlebar windowTitle]
                                            active:maximizeIsActive];

        [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
        [titlebar drawArea:[titlebar windowRect]];

        [frame updateAllResizeZonePositions];

        [self animateTransition:frame
                       fromRect:startRect
                         toRect:[frame windowRect]];
    }
}

- (BOOL)titlebarIsActiveForFrame:(XCBFrame *)frame clientWindow:(XCBWindow *)clientWindow
{
    return self.focusManager != nil && [self.focusManager isFrameFocused:frame];
}

- (void)animateTransition:(XCBFrame *)frame
                 fromRect:(XCBRect)startRect
                   toRect:(XCBRect)endRect
{
}

#pragma mark - Pressed Button Tracking

- (void)handleHoverDuringMotion:(xcb_motion_notify_event_t *)motionEvent
{
    URS_PROFILE_BEGIN(titlebarHover);
    @try {
        xcb_window_t titlebarId = [URSThemeIntegration pressedTitlebarWindow];
        if (titlebarId == 0) {
            return;
        }

        XCBWindow *window = [self.connection windowForXCBId:titlebarId];
        if (!window || ![window isKindOfClass:[XCBTitleBar class]]) {
            [URSThemeIntegration clearPressedState];
            return;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)window;

        NSPoint p = [self point:NSMakePoint(motionEvent->event_x, motionEvent->event_y)
                     fromWindow:motionEvent->event
                     toTitlebar:titlebarId];
        NSInteger button = [URSThemeIntegration pressedButton];
        BOOL highlighted = ([self buttonAtPoint:p forTitlebar:titlebar] == button);

        if (highlighted != [URSThemeIntegration pressedButtonHighlighted]) {
            [URSThemeIntegration setPressedTitlebar:titlebarId
                                             button:button
                                        highlighted:highlighted];
            [self redrawTitlebar:titlebar inFrame:(XCBFrame *)[titlebar parentWindow]];
        }

    } @catch (NSException *exception) {
        // Silently ignore exceptions during motion handling
    }
    URS_PROFILE_END(titlebarHover);
}

- (void)handleTitlebarLeave:(xcb_leave_notify_event_t *)leaveEvent
{
    @try {
        xcb_window_t titlebarId = [URSThemeIntegration pressedTitlebarWindow];
        if (titlebarId != 0 && leaveEvent->event == titlebarId &&
            [URSThemeIntegration pressedButtonHighlighted]) {
            [URSThemeIntegration setPressedTitlebar:titlebarId
                                             button:[URSThemeIntegration pressedButton]
                                        highlighted:NO];
            [self redrawTitlebarById:titlebarId];
        }
    } @catch (NSException *exception) {
        // Silently ignore
    }
}

#pragma mark - Titlebar Redraw

- (void)redrawTitlebar:(XCBTitleBar *)titlebar inFrame:(XCBFrame *)frame
{
    if (!titlebar || !frame) return;

    XCBWindow *clientWindow = [frame childWindowForKey:ClientWindow];
    NSString *title = [titlebar windowTitle];

    // Determine active state from the focus manager, not from stacking order.
    // Stacking order (isAbove) can differ from keyboard focus; the titlebar
    // MUST always reflect which window actually has keyboard input.
    BOOL isActive = self.focusManager != nil && [self.focusManager isFrameFocused:frame];
    // Fallback to isAbove if no focus manager is available (should not happen)
    if (!self.focusManager) {
        isActive = [titlebar isAbove];
    }

    [URSThemeIntegration renderGSThemeToWindow:clientWindow
                                         frame:frame
                                         title:title
                                        active:isActive];

    XCBRect rect = [titlebar windowRect];
    [titlebar drawArea:rect];
    [self.connection flush];

}

- (void)redrawTitlebarById:(xcb_window_t)titlebarId
{
    @try {
        XCBWindow *window = [self.connection windowForXCBId:titlebarId];
        if (!window || ![window isKindOfClass:[XCBTitleBar class]]) return;
        XCBTitleBar *titlebar = (XCBTitleBar *)window;
        XCBFrame *frame = (XCBFrame *)[titlebar parentWindow];
        [self redrawTitlebar:titlebar inFrame:frame];
    } @catch (NSException *exception) {
        // Silently ignore
    }
}

- (void)rerenderTitlebarForFrame:(XCBFrame *)frame active:(BOOL)isActive
{
    if (!frame) return;

    @try {
        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (!titlebarWindow ||
            ![titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
            return;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)titlebarWindow;

        [URSThemeIntegration renderGSThemeToWindow:frame
                                             frame:frame
                                             title:[titlebar windowTitle]
                                            active:isActive];

        [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
        [titlebar drawArea:[titlebar windowRect]];
        [self.connection flush];


    } @catch (NSException *exception) {
        NSLog(@"Exception in rerenderTitlebarForFrame: %@", exception.reason);
    }
}

#pragma mark - Resize Rendering

- (void)handleResizeDuringMotion:(xcb_motion_notify_event_t *)motionEvent
{
    URS_PROFILE_BEGIN(titlebarResize);
    @try {
        XCBWindow *window = [self.connection windowForXCBId:motionEvent->event];
        if (!window) return;

        XCBFrame *frame = nil;
        if ([window isKindOfClass:[XCBFrame class]]) {
            frame = (XCBFrame *)window;
        }
        if (!frame) return;

        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (!titlebarWindow ||
            ![titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
            return;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)titlebarWindow;

        XCBRect titlebarRect = [titlebar windowRect];
        XCBSize pixmapSize = [titlebar pixmapSize];

        if (pixmapSize.width != titlebarRect.size.width) {
            xcb_pixmap_t oldPixmap = [titlebar pixmap];
            xcb_pixmap_t oldDPixmap = [titlebar dPixmap];

            [titlebar createPixmap];

            XCBWindow *resizeClient = [frame childWindowForKey:ClientWindow];
            BOOL resizeIsActive = [self titlebarIsActiveForFrame:frame
                                                    clientWindow:resizeClient];
            [URSThemeIntegration renderGSThemeToWindow:frame
                                                 frame:frame
                                                 title:[titlebar windowTitle]
                                                active:resizeIsActive];

            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];

            if (oldPixmap != 0) {
                xcb_free_pixmap([self.connection connection], oldPixmap);
            }
            if (oldDPixmap != 0) {
                xcb_free_pixmap([self.connection connection], oldDPixmap);
            }

            [titlebar drawArea:titlebarRect];

        } else {
            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
            [titlebar drawArea:titlebarRect];
        }
    } @catch (NSException *exception) {
        // Silently ignore exceptions during resize motion
    }
    URS_PROFILE_END(titlebarResize);
}

- (void)handleResizeComplete:(xcb_button_release_event_t *)releaseEvent
{
    @try {
        XCBWindow *window = [self.connection windowForXCBId:releaseEvent->event];
        if (!window) return;

        XCBFrame *frame = nil;
        if ([window isKindOfClass:[XCBFrame class]]) {
            frame = (XCBFrame *)window;
        } else if ([window parentWindow] &&
                   [[window parentWindow] isKindOfClass:[XCBFrame class]]) {
            frame = (XCBFrame *)[window parentWindow];
        }
        if (!frame) return;

        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (!titlebarWindow ||
            ![titlebarWindow isKindOfClass:[XCBTitleBar class]]) {
            return;
        }
        XCBTitleBar *titlebar = (XCBTitleBar *)titlebarWindow;

        XCBRect titlebarRect = [titlebar windowRect];
        XCBSize pixmapSize = [titlebar pixmapSize];

        if (pixmapSize.width != titlebarRect.size.width ||
            pixmapSize.height != titlebarRect.size.height) {
            [titlebar destroyPixmap];
            [titlebar createPixmap];

            XCBWindow *resizeClient = [frame childWindowForKey:ClientWindow];
            BOOL resizeIsActive = [self titlebarIsActiveForFrame:frame
                                                    clientWindow:resizeClient];
            [URSThemeIntegration renderGSThemeToWindow:frame
                                                 frame:frame
                                                 title:[titlebar windowTitle]
                                                active:resizeIsActive];

            [titlebar putWindowBackgroundWithPixmap:[titlebar pixmap]];
            [titlebar drawArea:[titlebar windowRect]];
            [self.connection flush];

        }
    } @catch (NSException *exception) {
        NSLog(@"Exception in handleResizeComplete: %@", exception.reason);
    }
}

@end
