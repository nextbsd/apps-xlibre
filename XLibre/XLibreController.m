//
//  XLibreController.m
//  XLibre.app - the application
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//

#define _GNU_SOURCE 1   /* setenv, kill on glibc with -std=c99 */

#import "XLibreController.h"
#import "XLServer.h"
#import "XLPasteboardBridge.h"
#import "URSHybridEventHandler.h"

#import <GNUstepGUI/GSDisplayServer.h>
#import <GNUstepGUI/GSTheme.h>
#import <xcb/xcb.h>
#import <stdlib.h>
#import <string.h>

// The root window property Xnextbsd watches for hide and show
// (NB_HIDDEN_ATOM in Xnextbsd/hw/nextbsd/nextbsd.h).
#define XL_HIDDEN_ATOM "_XNEXTBSD_HIDDEN"

NSString *const URSXConnectionLostNotification = @"URSXConnectionLostNotification";

@implementation XLibreController {
    XLServer *_server;
    URSHybridEventHandler *_wm;
    XLPasteboardBridge *_clipboard;
    NSTimer *_watchdog;
    BOOL _serverGone;
    NSPanel *_prefs;
    NSTextField *_prefsAppToRun;
    NSMutableArray *_openedPrograms;
}

+ (void)initialize
{
    if (self != [XLibreController class])
        return;
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        @"XLibreAppToRun": @"/usr/X11/bin/xterm",
        @"XLibreTerminal": @"/usr/X11/bin/xterm",
        @"XLibreApplications": @[],
        @"XLibreXResources": @"XTerm*selectToClipboard: true\n",
    }];
}

#pragma mark - Menus

- (NSMenuItem *)addItem:(NSString *)title action:(SEL)action
                    key:(NSString *)key to:(NSMenu *)menu
{
    NSMenuItem *item = (NSMenuItem *)[menu addItemWithTitle:title action:action
                                              keyEquivalent:key ?: @""];

    if (action != NULL && sel_isEqual(action, @selector(runApplication:)))
        [item setTarget:self];
    else if (action != NULL &&
             sel_isEqual(action, @selector(showPreferences:)))
        [item setTarget:self];
    return item;
}

// An Applications menu entry: a command string, or {Name, Command, Key}.
- (void)addApplication:(id)entry to:(NSMenu *)menu
{
    NSString *name = nil, *command = nil, *key = nil;
    NSMenuItem *item;

    if ([entry isKindOfClass:[NSString class]]) {
        command = entry;
    } else if ([entry isKindOfClass:[NSDictionary class]]) {
        name = [entry objectForKey:@"Name"];
        command = [entry objectForKey:@"Command"];
        key = [entry objectForKey:@"Key"];
    }
    if (![command isKindOfClass:[NSString class]] || [command length] == 0)
        return;
    if (![name isKindOfClass:[NSString class]] || [name length] == 0) {
        NSArray *words = [command componentsSeparatedByString:@" "];
        name = [[words objectAtIndex:0] lastPathComponent];
    }
    if (![key isKindOfClass:[NSString class]])
        key = nil;
    item = [self addItem:name action:@selector(runApplication:) key:key to:menu];
    [item setRepresentedObject:command];
}

- (void)buildMenus
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSMenu *main = [[NSMenu alloc] initWithTitle:@"XLibre"];
    NSMenu *info = [[NSMenu alloc] initWithTitle:@"Info"];
    NSMenu *apps = [[NSMenu alloc] initWithTitle:@"Applications"];
    NSArray *more = [d arrayForKey:@"XLibreApplications"];
    NSMenuItem *item;

    [self addItem:@"About XLibre" action:@selector(orderFrontStandardInfoPanel:)
              key:nil to:info];
    [self addItem:@"Preferences..." action:@selector(showPreferences:)
              key:@"," to:info];
    item = [self addItem:@"Info" action:NULL key:nil to:main];
    [main setSubmenu:info forItem:item];

    item = [self addItem:@"xterm" action:@selector(runApplication:)
                     key:@"n" to:apps];
    [item setRepresentedObject:[d stringForKey:@"XLibreTerminal"]];
    if ([more count] > 0)
        [apps addItem:[NSMenuItem separatorItem]];
    for (id entry in more)
        [self addApplication:entry to:apps];
    item = [self addItem:@"Applications" action:NULL key:nil to:main];
    [main setSubmenu:apps forItem:item];

    [self addItem:@"Hide" action:@selector(hide:) key:@"h" to:main];
    [self addItem:@"Quit" action:@selector(terminate:) key:@"q" to:main];
    [NSApp setMainMenu:main];
}

#pragma mark - Launching

- (void)applicationWillFinishLaunching:(NSNotification *)notification
{
    [self buildMenus];
}

// The nextbsd backend's WindowServer client id and token, so Xnextbsd
// joins this app (nil on another backend). Its header isn't installed;
// call it by selector.
- (NSDictionary *)windowServerCredentials
{
    id server = GSCurrentServer();
    SEL sel = NSSelectorFromString(@"windowServerAppCredentials");

    if (server == nil || ![server respondsToSelector:sel])
        return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    return [server performSelector:sel];
#pragma clang diagnostic pop
}

- (void)fail:(NSString *)message
{
    NSLog(@"[XLibre] %@", message);
    [self runAlert:@"XLibre" message:message default:@"Quit" alternate:nil];
    _serverGone = YES;
    [NSApp terminate:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    NSDictionary *creds = [self windowServerCredentials];
    NSString *error = nil;

    if (creds == nil)
        NSLog(@"[XLibre] not on the nextbsd backend: Xnextbsd will be an "
              "app of its own on WindowServer");
    _server = [[XLServer alloc] init];
    if (![_server startWithCredentials:creds error:&error]) {
        [self fail:error];
        return;
    }
    // For the window manager, the clipboard bridge and every X client we
    // start. Terminal and other non-X shells don't get DISPLAY.
    setenv("DISPLAY", [[_server display] UTF8String], 1);
    setenv("XAUTHORITY", [[_server authority] fileSystemRepresentation], 1);

    // Load the theme now: title bars drawn before it activates would use
    // GNUstep's default one until the next redraw.
    [GSTheme theme];
    _wm = [[URSHybridEventHandler alloc] init];
    if (_wm == nil || ![_wm start]) {
        [self fail:@"The window manager couldn't start on the X server."];
        return;
    }
    [self loadResources];
    [self placeBesideMenu];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(xConnectionLost:)
               name:URSXConnectionLostNotification object:nil];

    _clipboard = [[XLPasteboardBridge alloc] init];
    if (![_clipboard start])
        _clipboard = nil;

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(appWillHide:)
               name:NSApplicationWillHideNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(appDidUnhide:)
               name:NSApplicationDidUnhideNotification object:nil];

    _watchdog = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(watchServer:)
                                               userInfo:nil repeats:YES];
    [self runStartupProgram];
}

// New X windows go to the right of the main menu (NeXT style, at the
// top left), not under it.
- (void)placeBesideMenu
{
    NSWindow *menuWindow = [[NSApp mainMenu] window];
    NSRect f = menuWindow != nil ? [menuWindow frame] : NSZeroRect;

    if (NSMaxX(f) > 0 && NSMaxX(f) < 400)
        [[_wm connection] setPlacementX:(int16_t)(NSMaxX(f) + 22)];
}

// X resources for the new server's RESOURCE_MANAGER, as xrdb would load.
- (void)loadResources
{
    NSString *res = [[NSUserDefaults standardUserDefaults]
                        stringForKey:@"XLibreXResources"];
    NSData *data = [res dataUsingEncoding:NSUTF8StringEncoding];
    xcb_connection_t *c = [[_wm connection] connection];
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    if ([data length] == 0)
        return;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, screen->root,
                        XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 8,
                        (uint32_t)[data length], [data bytes]);
    xcb_flush(c);
}

// Opened with programs to run (openapp XLibre /usr/X11/bin/xclock, or a
// Workspace "open with"): those run instead of XLibreAppToRun. Options
// for them go in XLibreAppToRun itself: -XLibreAppToRun "xclock -digital".
- (BOOL)application:(NSApplication *)app openFile:(NSString *)path
{
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:path])
        return NO;
    if (_server != nil && [_server display] != nil) {
        [self launch:path arguments:@[]];
    } else {
        if (_openedPrograms == nil)
            _openedPrograms = [NSMutableArray array];
        [_openedPrograms addObject:path];
    }
    return YES;
}

- (void)runStartupProgram
{
    NSString *command;

    if ([_openedPrograms count] > 0) {
        for (NSString *path in _openedPrograms)
            [self launch:path arguments:@[]];
        _openedPrograms = nil;
        return;
    }
    command = [[NSUserDefaults standardUserDefaults]
                  stringForKey:@"XLibreAppToRun"];
    if ([command length] > 0)
        [self runCommand:command];
}

#pragma mark - X programs

// Our environment, which NSProcessInfo took at startup, plus DISPLAY and
// XAUTHORITY.
- (NSDictionary *)clientEnvironment
{
    NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment]
                                   mutableCopy];

    [env setObject:[_server display] forKey:@"DISPLAY"];
    [env setObject:[_server authority] forKey:@"XAUTHORITY"];
    [env removeObjectForKey:@"XNEXTBSD_JOIN"];
    return env;
}

- (void)launch:(NSString *)path arguments:(NSArray *)arguments
{
    NSTask *task = [[NSTask alloc] init];

    [task setLaunchPath:path];
    [task setArguments:arguments];
    [task setEnvironment:[self clientEnvironment]];
    [task setCurrentDirectoryPath:NSHomeDirectory()];
    @try {
        [task launch];
    } @catch (NSException *e) {
        NSRunAlertPanel(@"XLibre", @"Can't run %@: %@", @"OK", nil, nil,
                        path, [e reason]);
    }
}

// A command line from the menu or the defaults, run by the shell.
- (void)runCommand:(NSString *)command
{
    if (_server == nil || [_server display] == nil)
        return;
    [self launch:@"/bin/sh"
       arguments:@[@"-c", [@"exec " stringByAppendingString:command]]];
}

- (void)runApplication:(id)sender
{
    NSString *command = [sender representedObject];

    if ([command length] > 0)
        [self runCommand:command];
}

#pragma mark - Hide

// Xnextbsd orders every X frame out (or back in) on WindowServer and
// leaves the X windows mapped.
- (void)setXHidden:(BOOL)hidden
{
    xcb_connection_t *c = [[_wm connection] connection];
    xcb_intern_atom_reply_t *r;
    xcb_screen_t *screen;
    uint32_t value = hidden ? 1 : 0;

    if (c == NULL || xcb_connection_has_error(c))
        return;
    r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0,
        (uint16_t)strlen(XL_HIDDEN_ATOM), XL_HIDDEN_ATOM), NULL);
    if (r == NULL)
        return;
    screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, screen->root, r->atom,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
    free(r);
    xcb_flush(c);
}

- (void)appWillHide:(NSNotification *)n
{
    [self setXHidden:YES];
}

- (void)appDidUnhide:(NSNotification *)n
{
    [self setXHidden:NO];
}

#pragma mark - Preferences

- (void)showPreferences:(id)sender
{
    if (_prefs == nil) {
        NSTextField *label, *note;
        NSView *view;

        _prefs = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 420, 120)
                      styleMask:NSTitledWindowMask | NSClosableWindowMask
                        backing:NSBackingStoreBuffered
                          defer:YES];
        [_prefs setTitle:@"XLibre Preferences"];
        [_prefs setReleasedWhenClosed:NO];
        [_prefs setDelegate:(id)self];
        view = [_prefs contentView];

        label = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 78, 388, 20)];
        [label setStringValue:@"Run when XLibre starts:"];
        [label setEditable:NO];
        [label setSelectable:NO];
        [label setBezeled:NO];
        [label setDrawsBackground:NO];
        [view addSubview:label];

        _prefsAppToRun = [[NSTextField alloc]
                             initWithFrame:NSMakeRect(16, 50, 388, 24)];
        [_prefsAppToRun setTarget:self];
        [_prefsAppToRun setAction:@selector(appToRunChanged:)];
        [view addSubview:_prefsAppToRun];

        note = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 12, 388, 32)];
        [note setStringValue:@"Leave empty to start nothing. More programs "
              "for the Applications menu: the XLibreApplications default."];
        [note setEditable:NO];
        [note setSelectable:NO];
        [note setBezeled:NO];
        [note setDrawsBackground:NO];
        [note setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
        [view addSubview:note];
        [_prefs center];
    }
    [_prefsAppToRun setStringValue:[[NSUserDefaults standardUserDefaults]
                                       stringForKey:@"XLibreAppToRun"] ?: @""];
    [_prefs makeKeyAndOrderFront:nil];
    [_prefs makeFirstResponder:_prefsAppToRun];
}

- (void)appToRunChanged:(id)sender
{
    [[NSUserDefaults standardUserDefaults]
        setObject:[_prefsAppToRun stringValue] forKey:@"XLibreAppToRun"];
    [[NSUserDefaults standardUserDefaults] synchronize];
}

- (void)windowWillClose:(NSNotification *)n
{
    if ([n object] == _prefs)
        [self appToRunChanged:nil];
}

#pragma mark - Quitting

- (void)serverGone
{
    if (_serverGone)
        return;
    _serverGone = YES;
    NSLog(@"[XLibre] the X server went away; quitting");
    [NSApp terminate:nil];
}

- (void)watchServer:(NSTimer *)timer
{
    if (![_server isRunning])
        [self serverGone];
}

- (void)xConnectionLost:(NSNotification *)n
{
    [self serverGone];
}

// An alert panel that takes the key window, so Return and Escape answer
// it even when an X window was key. -runModalForWindow: makes a panel key
// only if it is already on screen, which NSRunAlertPanel's isn't.
- (NSInteger)runAlert:(NSString *)title message:(NSString *)message
              default:(NSString *)def alternate:(NSString *)alt
{
    NSPanel *panel = NSGetAlertPanel(title, @"%@", def, alt, nil, message);
    NSInteger answer;

    [panel center];
    [panel orderFrontRegardless];
    answer = [NSApp runModalForWindow:panel];
    [panel orderOut:nil];
    NSReleaseAlertPanel(panel);
    return answer;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    NSInteger answer;

    if (_serverGone || _wm == nil || [_wm managedWindowCount] == 0)
        return NSTerminateNow;
    answer = [self runAlert:@"Quit XLibre?"
                    message:@"All X11 applications will close."
                    default:@"Quit" alternate:@"Cancel"];
    return answer == NSAlertDefaultReturn ? NSTerminateNow : NSTerminateCancel;
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
    _serverGone = YES;          // stopping it now; nothing to report
    [_watchdog invalidate];
    _watchdog = nil;
    [_clipboard stop];
    _clipboard = nil;
    [_server stop];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return NO;
}

@end
