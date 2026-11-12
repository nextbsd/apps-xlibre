//
//  URSTitlebarController.h
//  uroswm - Titlebar Interaction Controller
//
//  Handles titlebar button hit-testing, hover state, button press actions
//  (close/minimize/maximize), and resize-during-motion rendering updates.
//

#import <Foundation/Foundation.h>
#import "XCBConnection.h"
#import "XCBFrame.h"
#import "XCBTitleBar.h"
#import "URSWorkareaManager.h"

@class URSFocusManager;

@interface URSTitlebarController : NSObject

@property (weak, nonatomic) XCBConnection *connection;
@property (weak, nonatomic) URSFocusManager *focusManager;
@property (weak, nonatomic) URSWorkareaManager *workareaManager;

- (instancetype)initWithConnection:(XCBConnection *)connection;

// Button press/release handling (return YES if the event was consumed).
// A title bar button highlights on press and acts on release, like NSButton.
- (BOOL)handleTitlebarButtonPress:(xcb_button_press_event_t *)pressEvent;
- (BOOL)handleTitlebarButtonRelease:(xcb_button_release_event_t *)releaseEvent;

// Button hit detection (X11 coordinates relative to the title bar).
// Returns an NSWindowButton, or -1 for none.
- (NSInteger)buttonAtPoint:(NSPoint)point
               forTitlebar:(XCBTitleBar *)titlebar;

// Pressed-button highlight tracking during motion
- (void)handleHoverDuringMotion:(xcb_motion_notify_event_t *)motionEvent;
- (void)handleTitlebarLeave:(xcb_leave_notify_event_t *)leaveEvent;

// Resize rendering
- (void)handleResizeDuringMotion:(xcb_motion_notify_event_t *)motionEvent;
- (void)handleResizeComplete:(xcb_button_release_event_t *)releaseEvent;

// Focus-change rendering
- (void)rerenderTitlebarForFrame:(XCBFrame *)frame active:(BOOL)isActive;

@end
