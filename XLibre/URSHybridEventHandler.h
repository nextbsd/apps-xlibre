//
//  URSHybridEventHandler.h
//  XLibre.app - the window manager
//
//  Created by Alessandro Sangiuliano on 22/06/20.
//  Copyright (c) 2020 Alessandro Sangiuliano. All rights reserved.
//
//  Owns the XCB event loop on the NSRunLoop and dispatches to the focus,
//  workarea and title bar managers.
//

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import "XCBConnection.h"
#import "XCBWindow.h"
#import "XCBTitleBar.h"
#import "URSThemeIntegration.h"
#import "URSFocusManager.h"
#import "URSWorkareaManager.h"
#import "URSTitlebarController.h"

@interface URSHybridEventHandler : NSObject <RunLoopEvents>

// XCB Integration
@property (strong, nonatomic) XCBConnection *connection;
@property (strong, nonatomic) XCBWindow *selectionManagerWindow;

// RANDR extension event base (for xrandr event routing)
@property (assign, nonatomic) uint8_t randrEventBase;

// Event loop bookkeeping
@property (assign, nonatomic) BOOL xcbEventsIntegrated;
@property (assign, nonatomic) NSUInteger eventCount;

// --- Single-responsibility managers ---
@property (strong, nonatomic) URSFocusManager *focusManager;
@property (strong, nonatomic) URSWorkareaManager *workareaManager;
@property (strong, nonatomic) URSTitlebarController *titlebarController;

// Connects to the X server named by DISPLAY; nil if it can't.
- (id)init;

// Become the window manager, adopt existing windows and put the XCB
// connection on the NSRunLoop. NO if another window manager won't let go.
- (BOOL)start;

// Top-level client windows the window manager has framed.
- (NSUInteger)managedWindowCount;

// Window manager lifecycle
- (BOOL)registerAsWindowManager;
- (void)decorateExistingWindowsOnStartup;

// NSRunLoop integration
- (void)setupXCBEventIntegration;
- (void)processXCBEvent:(xcb_generic_event_t *)event;

// GSTheme integration
- (void)refreshAllManagedWindows;

// Cleanup
- (void)cleanupBeforeExit;

// ICCCM Manager Selection Protocol
- (void)handleSelectionClear:(xcb_selection_clear_event_t *)event;

// XLibre's own GNUstep windows (About, Preferences, the Quit alert): while
// one is key, no X window has the X focus or an active title bar; when
// none is, the X window that had it gets it back.
- (void)appKitWindowBecameKey;
- (void)appKitWindowsResignedKey;

@end
