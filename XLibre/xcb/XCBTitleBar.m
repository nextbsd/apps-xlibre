//
//  XCBTitleBar.m
//  XCBKit
//
//  Created by Alessandro Sangiuliano on 06/08/19.
//  Copyright (c) 2019 alex. All rights reserved.
//

#import "XCBTitleBar.h"

@implementation XCBTitleBar

@synthesize ewmhService;
@synthesize titleIsSet;


- (id) initWithFrame:(XCBFrame *)aFrame withConnection:(XCBConnection *)aConnection
{
    self = [super init];

    if (self == nil)
        return nil;

    windowMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    
    [super setConnection:aConnection];

    ewmhService = [EWMHService sharedInstanceWithConnection:[super connection]];
    titleIsSet = NO;
    
    return self;
}

- (void)drawTitleBarComponents
{
    // Contents come from the GSTheme-rendered pixmap
    [super drawArea:[super windowRect]];
}

- (void) setWindowTitle:(NSString *) title
{
    if (titleIsSet && windowTitle && [windowTitle isEqualToString:title])
        return;

    windowTitle = title;

    if ([title length] == 0)
        return;

    // GSTheme handles title text rendering — legacy path removed.
    titleIsSet = YES;
}

// OPTIMIZATION: Set internal title without legacy rendering
// Used when GSTheme will handle the actual titlebar rendering
- (void) setInternalTitle:(NSString *) title
{
    windowTitle = title;
    // Don't set titleIsSet here - allows setWindowTitle to work if needed later
}

- (NSString*) windowTitle
{
    return windowTitle;
}

- (void) dealloc
{
    ewmhService = nil;
}


@end
