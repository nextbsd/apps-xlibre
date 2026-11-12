//
//  XCBFrame.h
//  XCBKit
//
//  Created by Alessandro Sangiuliano on 05/08/19.
//  Copyright (c) 2019 alex. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "XCBWindow.h"
#import "XCBConnection.h"
#import "EMousePosition.h"
#import "EResizeDirection.h"


#define WM_MIN_WINDOW_HEIGHT 431
#define WM_MIN_WINDOW_WIDTH 496

// Absolute minimum client area — prevents windows from collapsing to just the titlebar.
// These are enforced even when the client doesn't set WM_NORMAL_HINTS.
#define WM_MIN_CLIENT_HEIGHT 100
#define WM_MIN_CLIENT_WIDTH  100

typedef NS_ENUM(NSInteger, childrenMask)
{
    TitleBar = 0,
    ClientWindow = 1,
    ResizeHandle = 2,    // Legacy (keep for backwards compatibility)
    ResizeBar = 3,       // Theme-drawn resize bar along the bottom edge
    ResizeZoneNW = 10,
    ResizeZoneN = 11,
    ResizeZoneNE = 12,
    ResizeZoneE = 13,
    ResizeZoneSE = 14,
    ResizeZoneS = 15,
    ResizeZoneSW = 16,
    ResizeZoneW = 17,
    ResizeZoneGrowBox = 18  // Theme-defined grow box overlay
};

@interface XCBFrame : XCBWindow
{
    NSMutableDictionary *children;
}

@property (nonatomic, assign) int minHeightHint;
@property (nonatomic, assign) int minWidthHint;
@property (nonatomic, assign) uint16_t titleHeight;
@property (strong, nonatomic) XCBConnection *connection;
// clientBorder: left/right inset of the client area (theme window border)
@property (nonatomic, assign) int clientBorder;
// bottomBorder: bottom inset of the client area (theme resize bar, or border)
@property (nonatomic, assign) int bottomBorder;
@property (nonatomic, assign) BOOL rightBorderClicked;
@property (nonatomic, assign) BOOL bottomBorderClicked;
@property (nonatomic, assign) BOOL leftBorderClicked;
@property (nonatomic, assign) BOOL topBorderClicked;
@property (nonatomic, assign) XCBPoint offset;
// NSWindow style mask used for decorations (titled/closable/miniaturizable/resizable)
@property (nonatomic, assign) NSUInteger decorationStyleMask;
// Client reports unsaved changes (_GNUSTEP_WM_ATTR GSDocumentEditedFlag)
@property (nonatomic, assign) BOOL documentEdited;
// A corner-rounding shape mask is currently set on the frame
@property (nonatomic, assign) BOOL hasCornerShape;

- (id) initWithClientWindow:(XCBWindow*) aClientWindow withConnection:(XCBConnection*) aConnection;
- (id) initWithClientWindow:(XCBWindow*) aClientWindow
             withConnection:(XCBConnection*) aConnection
              withXcbWindow:(xcb_window_t) xcbWindow
                   withRect:(XCBRect)aRect;

- (void) addChildWindow:(XCBWindow*) aChild withKey:(childrenMask) keyMask;
- (XCBWindow*) childWindowForKey:(childrenMask) key;
- (void) removeChild:(childrenMask) frameChild;
- (void) resize:(xcb_motion_notify_event_t *)anEvent xcbConnection:(xcb_connection_t*)aXcbConnection;
- (void) moveTo:(XCBPoint)coordinates;
- (void) configureClient;
- (void) configureClientWithFramePosition:(XCBPoint)framePos clientSize:(XCBSize)clientSize;
- (MousePosition) mouseIsOnWindowBorderForEvent:(xcb_motion_notify_event_t *)anEvent;
- (void) restoreDimensionAndPosition;
- (void) raiseResizeHandle;
- (void) programmaticResizeToRect:(XCBRect)targetRect;

// Resize bar and its resize zones (GSTheme resize bar)
- (void) createResizeZonesFromTheme;
// Lay out the resize bar and zones for the current frame size (redraws the bar if its width changed)
- (void) updateAllResizeZonePositions;
- (void) destroyResizeZones;
// Redraw the resize bar with the active theme
- (void) renderResizeBar;
// Re-apply the active theme's decoration offsets (title bar, border, resize
// bar) to an existing frame after a theme switch. Returns YES if the
// geometry changed.
- (BOOL) relayoutForCurrentTheme;
// Round the frame's corners (theme corner radii) with a shape mask when no
// compositor is running; with a compositor the decorations use transparency.
- (void) applyCornerShape;


 /********************************
 *                               *
 *            ACCESSORS          *
 *                               *
 ********************************/

- (void) setChildren:(NSMutableDictionary*) aChildrenSet;
- (NSMutableDictionary*) getChildren;
- (void) decorateClientWindow;

// Re-read the client's decoration style (_GNUSTEP_WM_ATTR for GNUstep apps,
// ICCCM/EWMH hints otherwise). Returns YES if the style or edited state changed.
- (BOOL) updateDecorationStyleFromClient;

// Decoration style for a client that may not be framed yet
+ (NSUInteger) decorationStyleMaskForClient:(XCBWindow*)aClient
                                 connection:(XCBConnection*)aConnection
                             documentEdited:(BOOL*)edited;

@end
