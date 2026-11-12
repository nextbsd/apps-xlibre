//
//  main.m
//  XLibre.app
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//
//  XLibre.app keeps its Dock tile (it never sets GSSuppressAppIcon for
//  itself); X programs run inside it share that one tile.
//

#define _GNU_SOURCE 1   /* setenv, kill on glibc with -std=c99 */

#import <AppKit/AppKit.h>
#import <signal.h>
#import <unistd.h>
#import "XLibreController.h"
#import "XLServer.h"

// Logout or kill: take the X server (and so every X client) with us.
static void
quitSignal(int sig)
{
    XLServerEmergencyStop();
    _exit(0);
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        XLibreController *controller;

        // Writing to a closed stderr (started without a terminal) mustn't
        // kill us.
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTERM, quitSignal);
        signal(SIGHUP, quitSignal);
        signal(SIGINT, quitSignal);

        [NSApplication sharedApplication];
        controller = [[XLibreController alloc] init];
        [NSApp setDelegate:controller];
        [NSApp run];
    }
    return 0;
}
