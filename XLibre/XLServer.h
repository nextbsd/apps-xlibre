//
//  XLServer.h
//  XLibre.app - the Xnextbsd child process
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//
//  Starts Xnextbsd from the app bundle (Resources/Xnextbsd) as
//
//    Xnextbsd -displayfd 3 -nolisten tcp -auth <server auth file>
//
//  with a fresh MIT-MAGIC-COOKIE-1 made with xauth. The server picks a free
//  display and writes its number to fd 3; the cookie then goes into the
//  user's Xauthority for that display, and X clients get DISPLAY and
//  XAUTHORITY. WindowServer credentials, when given, reach the server in
//  its environment (XNEXTBSD_JOIN), never on its command line, so it joins
//  XLibre.app's WindowServer app.
//

#import <Foundation/Foundation.h>
#import <sys/types.h>

// The running server's pid, for a signal handler to pass SIGTERM on to.
extern volatile pid_t XLServerChildPid;

// For a signal handler: stop the server and remove its auth file, with
// async-signal-safe calls only.
void XLServerEmergencyStop(void);

@interface XLServer : NSObject

// ":N" once the server is up.
@property (readonly, nonatomic) NSString *display;
// The Xauthority file that holds the display's cookie, for XAUTHORITY.
@property (readonly, nonatomic) NSString *authority;
@property (readonly, nonatomic) pid_t pid;

// credentials: @{ClientID, Token} from the nextbsd backend, or nil.
// On failure returns NO and sets *error to something to show the user.
- (BOOL)startWithCredentials:(NSDictionary *)credentials
                       error:(NSString **)error;

// Still running? Reaps the server if it exited.
- (BOOL)isRunning;

// Terminate the server (SIGTERM, then SIGKILL) and remove its cookie.
- (void)stop;

@end
