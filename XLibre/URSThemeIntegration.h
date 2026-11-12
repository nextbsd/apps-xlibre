//
//  URSThemeIntegration.h
//  uroswm - GSTheme Window Decoration for Titlebars
//
//  Renders window decorations with the active GSTheme so X11 and AppKit
//  windows look exactly like GNUstep's own window decorations.
//

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <GNUstepGUI/GSTheme.h>
#import <xcb/xcb.h>
#import "XCBTitleBar.h"
#import "XCBFrame.h"

@interface URSThemeIntegration : NSObject

// Singleton access
+ (instancetype)sharedInstance;

+ (GSTheme*)currentTheme;

// Render the frame's title bar with the active theme into its pixmaps
+ (BOOL)renderGSThemeToWindow:(XCBWindow*)window
                        frame:(XCBFrame*)frame
                        title:(NSString*)title
                       active:(BOOL)isActive;

// Render the frame's resize bar with the active theme into its pixmaps
+ (BOOL)renderResizeBarForFrame:(XCBFrame*)frame;

// Configuration
@property (assign, nonatomic) BOOL enabled;

// Fixed-size window tracking
+ (void)registerFixedSizeWindow:(xcb_window_t)windowId;
+ (void)unregisterFixedSizeWindow:(xcb_window_t)windowId;
+ (BOOL)isFixedSizeWindow:(xcb_window_t)windowId;

// Pressed title bar button tracking (button is an NSWindowButton, -1 = none).
// The pressed button is drawn highlighted while the pointer is over it.
+ (xcb_window_t)pressedTitlebarWindow;
+ (NSInteger)pressedButton;
+ (BOOL)pressedButtonHighlighted;
+ (void)setPressedTitlebar:(xcb_window_t)titlebarId
                    button:(NSInteger)button
               highlighted:(BOOL)highlighted;
+ (void)clearPressedState;

@end
