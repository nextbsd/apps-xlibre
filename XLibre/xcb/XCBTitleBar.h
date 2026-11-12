//
//  XCBTitleBar.h
//  XCBKit
//
//  Created by Alessandro Sangiuliano on 06/08/19.
//  Copyright (c) 2019 alex. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "XCBWindow.h"
#import "XCBFrame.h"
#import "EWMHService.h"
#import "XCBTypes.h"


#ifndef TITLE_MASK

#define TITLE_MASK_VALUES XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | \
XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_POINTER_MOTION | \
XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | \
XCB_EVENT_MASK_KEY_PRESS

#endif


@interface XCBTitleBar : XCBWindow
{
    NSString *windowTitle;
}

@property (strong, nonatomic) EWMHService *ewmhService;
@property (nonatomic, assign) BOOL titleIsSet;

- (id) initWithFrame:(XCBFrame*) aFrame withConnection:(XCBConnection*) aConnection;

// Copy the GSTheme-rendered pixmap into the window
- (void) drawTitleBarComponents;

/****************
 *    ACCESORS  *
 ***************/

- (void) setWindowTitle:(NSString*) title;
- (NSString*) windowTitle;

// Set the title without redrawing; the GSTheme integration renders it
- (void) setInternalTitle:(NSString*) title;

@end
