//
//  URSThemeIntegration.m
//  uroswm - GSTheme Window Decoration for Titlebars
//
//  All decoration appearance comes from the active GSTheme: title bar
//  drawing, button cells and images, colours, fonts and layout. The window
//  manager only provides the drawing surface.
//

#import "URSThemeIntegration.h"
#import "URSDecorationMetrics.h"
#import "URSProfiler.h"
#import "XCBConnection.h"
#import "XCBFrame.h"
#import "XCBScreen.h"

// Implemented by GSTheme (GSThemeDrawing.m) but not declared in its header.
// TODO(libs-gui): declare these publicly.
@interface GSTheme (URSPrivateMethods)
- (void)drawTitleBarRect:(NSRect)titleBarRect
            forStyleMask:(unsigned int)styleMask
                   state:(int)inputState
                andTitle:(NSString*)title;
- (void)drawResizeBarRect:(NSRect)resizeBarRect;
@end

@implementation URSThemeIntegration

static URSThemeIntegration *sharedInstance = nil;
static NSMutableSet *fixedSizeWindows = nil;

static xcb_window_t pressedTitlebarWindow = 0;
static NSInteger pressedButton = -1;
static BOOL pressedButtonHighlighted = NO;

+ (void)initialize {
    if (self == [URSThemeIntegration class]) {
        fixedSizeWindows = [[NSMutableSet alloc] init];
    }
}

#pragma mark - ARGB Visual Support for Compositor Alpha

+ (xcb_visualid_t)findARGBVisualForScreen:(XCBScreen *)screen {
    xcb_screen_t *xcbScreen = [screen screen];
    if (!xcbScreen) return 0;

    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(xcbScreen);
    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        if (depth_iter.data->depth != 32) continue;
        xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
            if (visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return visual_iter.data->visual_id;
        }
    }
    return 0;
}

#pragma mark - Fixed-size window tracking

+ (void)registerFixedSizeWindow:(xcb_window_t)windowId {
    @synchronized(fixedSizeWindows) {
        [fixedSizeWindows addObject:@(windowId)];
    }
}

+ (void)unregisterFixedSizeWindow:(xcb_window_t)windowId {
    @synchronized(fixedSizeWindows) {
        [fixedSizeWindows removeObject:@(windowId)];
    }
}

+ (BOOL)isFixedSizeWindow:(xcb_window_t)windowId {
    @synchronized(fixedSizeWindows) {
        return [fixedSizeWindows containsObject:@(windowId)];
    }
}

#pragma mark - Pressed Button Tracking

+ (xcb_window_t)pressedTitlebarWindow {
    return pressedTitlebarWindow;
}

+ (NSInteger)pressedButton {
    return pressedButton;
}

+ (BOOL)pressedButtonHighlighted {
    return pressedButtonHighlighted;
}

+ (void)setPressedTitlebar:(xcb_window_t)titlebarId
                    button:(NSInteger)button
               highlighted:(BOOL)highlighted {
    pressedTitlebarWindow = titlebarId;
    pressedButton = button;
    pressedButtonHighlighted = highlighted;
}

+ (void)clearPressedState {
    pressedTitlebarWindow = 0;
    pressedButton = -1;
    pressedButtonHighlighted = NO;
}

#pragma mark - Singleton Management

+ (instancetype)sharedInstance {
    if (sharedInstance == nil) {
        sharedInstance = [[self alloc] init];
    }
    return sharedInstance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _enabled = YES;
    }
    return self;
}

+ (GSTheme*)currentTheme {
    return [GSTheme theme];
}

#pragma mark - Drawing

+ (NSBitmapImageRep *)newBitmapWithSize:(NSSize)size {
    int w = (int)size.width, h = (int)size.height;
    return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                   pixelsWide:w
                                                   pixelsHigh:h
                                                bitsPerSample:8
                                              samplesPerPixel:4
                                                     hasAlpha:YES
                                                     isPlanar:NO
                                               colorSpaceName:NSDeviceRGBColorSpace
                                                  bytesPerRow:w * 4
                                                 bitsPerPixel:32];
}

// Draw a 1px outline on the given edges, as -[GSTheme drawWindowBorder:...] does
+ (void)strokeBorderForRect:(NSRect)r top:(BOOL)top bottom:(BOOL)bottom {
    [[URSDecorationMetrics borderColor] set];
    NSRectFill(NSMakeRect(0, 0, 1, r.size.height));
    NSRectFill(NSMakeRect(r.size.width - 1, 0, 1, r.size.height));
    if (top)
        NSRectFill(NSMakeRect(0, r.size.height - 1, r.size.width, 1));
    if (bottom)
        NSRectFill(NSMakeRect(0, 0, r.size.width, 1));
}

// Outline the rounded corners of a decoration strip in the border colour.
+ (void)strokeRoundedCornersOfRect:(NSRect)r radius:(CGFloat)radius top:(BOOL)top
{
    if (radius <= 0)
        return;

    // Extend past the far edge so only the requested corners curve
    NSRect shape = top ? NSMakeRect(NSMinX(r), NSMinY(r) - radius, NSWidth(r), NSHeight(r) + radius)
                       : NSMakeRect(NSMinX(r), NSMinY(r), NSWidth(r), NSHeight(r) + radius);
    NSBezierPath *stroke = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(shape, 0.5, 0.5)
                                                           xRadius:radius - 0.5
                                                           yRadius:radius - 0.5];
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(r);
    [[URSDecorationMetrics borderColor] set];
    [stroke setLineWidth:1.0];
    [stroke stroke];
    [NSGraphicsContext restoreGraphicsState];
}

// Make the pixels outside rounded corners transparent (antialiased), directly
// in the rendered bitmap. With a compositor they show what is behind the
// window; without one the frame's shape mask hides them anyway.
// Rows are top-down in the bitmap data.
+ (void)clearCornersOfBitmap:(NSBitmapImageRep *)bitmap radius:(int)radius top:(BOOL)top
{
    int width = (int)[bitmap pixelsWide];
    int height = (int)[bitmap pixelsHigh];
    if (radius <= 0 || radius > height || 2 * radius > width)
        return;

    BOOL premultiplied = ([bitmap bitmapFormat] & NSAlphaNonpremultipliedBitmapFormat) == 0;
    int alphaIndex = ([bitmap bitmapFormat] & NSAlphaFirstBitmapFormat) ? 0 : 3;
    unsigned char *data = [bitmap bitmapData];
    int bytesPerRow = (int)[bitmap bytesPerRow];
    const int samples = 4;

    for (int dy = 0; dy < radius; dy++) {
        int row = top ? dy : height - 1 - dy;
        for (int dx = 0; dx < radius; dx++) {
            // Coverage of pixel (dx, dy) by the circle centred at (radius, radius)
            int inside = 0;
            for (int sy = 0; sy < samples; sy++) {
                for (int sx = 0; sx < samples; sx++) {
                    double px = dx + (sx + 0.5) / samples - radius;
                    double py = dy + (sy + 0.5) / samples - radius;
                    if (px * px + py * py <= (double)radius * radius)
                        inside++;
                }
            }
            if (inside == samples * samples)
                continue;

            int cols[2] = {dx, width - 1 - dx};
            for (int c = 0; c < 2; c++) {
                uint8_t *px = data + row * bytesPerRow + cols[c] * 4;
                if (premultiplied) {
                    for (int k = 0; k < 4; k++)
                        px[k] = (uint8_t)(px[k] * inside / (samples * samples));
                } else {
                    px[alphaIndex] = (uint8_t)(px[alphaIndex] * inside / (samples * samples));
                }
            }
        }
    }
}

+ (void)drawTitleBarInRect:(NSRect)rect
                 styleMask:(NSUInteger)styleMask
                     state:(int)state
                     title:(NSString *)title
            documentEdited:(BOOL)edited
               titlebarId:(xcb_window_t)titlebarId {
    GSTheme *theme = [GSTheme theme];

    // The theme's title bar drawing leaves the outer 1px to the window
    // border, so start from the border colour.
    [[URSDecorationMetrics borderColor] set];
    NSRectFill(rect);

    if ([theme respondsToSelector:@selector(drawTitleBarRect:forStyleMask:state:andTitle:)]) {
        [theme drawTitleBarRect:rect
                   forStyleMask:(unsigned int)styleMask
                          state:state
                       andTitle:title ?: @""];
    } else {
        [theme drawWindowBorder:rect
                      withFrame:rect
                   forStyleMask:(unsigned int)(styleMask & ~NSResizableWindowMask)
                          state:state
                       andTitle:title ?: @""];
    }
    [self strokeBorderForRect:rect top:YES bottom:NO];

    for (NSNumber *b in [URSDecorationMetrics buttonsForStyleMask:styleMask]) {
        NSWindowButton button = (NSWindowButton)[b integerValue];
        BOOL highlighted = (titlebarId != 0 &&
                            titlebarId == pressedTitlebarWindow &&
                            button == pressedButton &&
                            pressedButtonHighlighted);
        NSRect frame = [URSDecorationMetrics frameForButton:button
                                               titleBarSize:rect.size
                                                  styleMask:styleMask];
        NSButton *cellButton = [URSDecorationMetrics themeButton:button
                                                       styleMask:styleMask
                                                     highlighted:highlighted
                                                  documentEdited:edited];
        [cellButton setFrame:frame];
        [[cellButton cell] drawWithFrame:frame inView:cellButton];
    }

    [self strokeRoundedCornersOfRect:rect
                              radius:[URSDecorationMetrics topCornerRadiusForStyleMask:styleMask]
                                 top:YES];
}

#pragma mark - Pixmap Upload

// Upload a rendered bitmap into a window's pixmap and dPixmap.
+ (BOOL)uploadBitmap:(NSBitmapImageRep *)bitmap toWindow:(XCBWindow *)target {
    unsigned char *data = [bitmap bitmapData];
    if (!data || [bitmap bitsPerPixel] != 32) {
        NSLog(@"URSThemeIntegration: unexpected bitmap format for decoration upload");
        return NO;
    }

    int width = (int)[bitmap pixelsWide];
    int height = (int)[bitmap pixelsHigh];
    int bytesPerRow = (int)[bitmap bytesPerRow];
    BOOL alphaFirst = ([bitmap bitmapFormat] & NSAlphaFirstBitmapFormat) != 0;
    BOOL premultiplied = ([bitmap bitmapFormat] & NSAlphaNonpremultipliedBitmapFormat) == 0;

    // Convert to premultiplied BGRA (B | G<<8 | R<<16 | A<<24, little-endian)
    for (int y = 0; y < height; y++) {
        uint8_t *px = data + y * bytesPerRow;
        for (int x = 0; x < width; x++, px += 4) {
            uint32_t r, g, b, a;
            if (alphaFirst) {
                a = px[0]; r = px[1]; g = px[2]; b = px[3];
            } else {
                r = px[0]; g = px[1]; b = px[2]; a = px[3];
            }
            if (!premultiplied && a < 255) {
                r = (r * a) / 255;
                g = (g * a) / 255;
                b = (b * a) / 255;
            }
            uint32_t out = b | (g << 8) | (r << 16) | (a << 24);
            memcpy(px, &out, 4);
        }
    }

    xcb_connection_t *conn = [[target connection] connection];
    uint8_t depth = 32;
    if (![target use32BitDepth]) {
        XCBScreen *screen = [target onScreen] ?: [target screen];
        if (screen) depth = [screen screen]->root_depth;
    }

    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, [target pixmap], 0, NULL);
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, [target pixmap], gc,
                  (uint16_t)width, (uint16_t)height, 0, 0, 0, depth,
                  (uint32_t)((size_t)height * (size_t)bytesPerRow), data);
    if ([target dPixmap] != 0)
        xcb_copy_area(conn, [target pixmap], [target dPixmap], gc,
                      0, 0, 0, 0, (uint16_t)width, (uint16_t)height);
    xcb_free_gc(conn, gc);
    [[target connection] flush];
    return YES;
}

#pragma mark - Title Bar Rendering

+ (BOOL)renderGSThemeToWindow:(XCBWindow*)window
                        frame:(XCBFrame*)frame
                        title:(NSString*)title
                       active:(BOOL)isActive {
    URS_PROFILE_BEGIN(themeRender);
    BOOL success = NO;

    @try {
        if (![[URSThemeIntegration sharedInstance] enabled] || !frame)
            return NO;

        XCBWindow *titlebarWindow = [frame childWindowForKey:TitleBar];
        if (![titlebarWindow isKindOfClass:[XCBTitleBar class]])
            return NO;
        XCBTitleBar *titlebar = (XCBTitleBar *)titlebarWindow;

        // The title bar spans the full frame width
        XCBRect titlebarRect = [titlebar windowRect];
        XCBRect frameRect = [frame windowRect];
        uint16_t width = frameRect.size.width;
        uint16_t height = titlebarRect.size.height;
        if (width == 0 || height == 0)
            return NO;

        if (titlebarRect.position.x != 0 || titlebarRect.size.width != width) {
            uint32_t values[2] = {0, width};
            xcb_configure_window([[frame connection] connection], [titlebar window],
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);
            titlebarRect.position.x = 0;
            titlebarRect.size.width = width;
            [titlebar setWindowRect:titlebarRect];
        }

        XCBSize pixmapSize = [titlebar pixmapSize];
        if ([titlebar pixmap] == 0 || [titlebar dPixmap] == 0 ||
            pixmapSize.width != width || pixmapSize.height != height) {
            if ([titlebar pixmap] != 0 || [titlebar dPixmap] != 0)
                [titlebar destroyPixmap];
            [titlebar createPixmap];
        }

        // libs-back prefixes edited titles with "*" for window managers other
        // than WindowMaker; the theme shows edited state on the close button.
        if ([frame documentEdited] && [title hasPrefix:@"*"])
            title = [title substringFromIndex:1];

        NSRect rect = NSMakeRect(0, 0, width, height);
        NSBitmapImageRep *bitmap = [self newBitmapWithSize:rect.size];
        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];

        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        [self drawTitleBarInRect:rect
                       styleMask:[frame decorationStyleMask]
                           state:isActive ? GSTitleBarKey : GSTitleBarNormal
                           title:title
                  documentEdited:[frame documentEdited]
                      titlebarId:[titlebar window]];
        [ctx flushGraphics];
        [NSGraphicsContext restoreGraphicsState];
        [self clearCornersOfBitmap:bitmap
                            radius:[URSDecorationMetrics topCornerRadiusForStyleMask:[frame decorationStyleMask]]
                               top:YES];

        success = [self uploadBitmap:bitmap toWindow:titlebar];
        if (!success)
            NSLog(@"Failed to upload title bar for: %@", title);
    } @catch (NSException *exception) {
        NSLog(@"Title bar rendering failed: %@", exception.reason);
        success = NO;
    }

    URS_PROFILE_END(themeRender);
    return success;
}

#pragma mark - Resize Bar Rendering

+ (BOOL)renderResizeBarForFrame:(XCBFrame*)frame {
    @try {
        if (![[URSThemeIntegration sharedInstance] enabled] || !frame)
            return NO;

        XCBWindow *bar = [frame childWindowForKey:ResizeBar];
        XCBRect barRect = [bar windowRect];
        if (!bar || barRect.size.width == 0 || barRect.size.height == 0)
            return NO;

        XCBSize pixmapSize = [bar pixmapSize];
        if ([bar pixmap] == 0 || pixmapSize.width != barRect.size.width ||
            pixmapSize.height != barRect.size.height) {
            if ([bar pixmap] != 0)
                [bar destroyPixmap];
            [bar createPixmap];
        }

        GSTheme *theme = [GSTheme theme];
        NSRect rect = NSMakeRect(0, 0, barRect.size.width, barRect.size.height);
        NSBitmapImageRep *bitmap = [self newBitmapWithSize:rect.size];
        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];

        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        [[URSDecorationMetrics borderColor] set];
        NSRectFill(rect);
        if ([theme respondsToSelector:@selector(drawResizeBarRect:)]) {
            [theme drawResizeBarRect:rect];
        } else {
            [theme drawWindowBorder:rect
                          withFrame:rect
                       forStyleMask:NSResizableWindowMask
                              state:GSTitleBarNormal
                           andTitle:@""];
        }
        [self strokeBorderForRect:rect top:NO bottom:YES];
        [self strokeRoundedCornersOfRect:rect
                                  radius:[URSDecorationMetrics bottomCornerRadiusForStyleMask:[frame decorationStyleMask]]
                                     top:NO];
        [ctx flushGraphics];
        [NSGraphicsContext restoreGraphicsState];
        [self clearCornersOfBitmap:bitmap
                            radius:[URSDecorationMetrics bottomCornerRadiusForStyleMask:[frame decorationStyleMask]]
                               top:NO];

        return [self uploadBitmap:bitmap toWindow:bar];
    } @catch (NSException *exception) {
        NSLog(@"Resize bar rendering failed: %@", exception.reason);
        return NO;
    }
}

@end
