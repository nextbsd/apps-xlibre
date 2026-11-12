/*
 * Xnextbsd: DDX entry points, the WindowServer connection and the screen.
 *
 * Copyright (c) 2026, Joseph Maloney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Xnextbsd is a rootless X server: every top-level X window is a
 * WindowServer window, drawn by miext/rootless into a buffer shared with
 * WindowServer, and X input comes from WindowServer's events. It is
 * modelled on XQuartz's xpr layer (hw/xquartz/xpr), with
 * libWindowServerClient in the place of Apple's Xplugin.
 *
 * Everything runs on the server's main thread: WindowServer's connection
 * is one more file descriptor in the main loop (SetNotifyFd), and there is
 * no input thread.
 */

#include <dix-config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/screenint_priv.h"
#include "include/extinit.h"
#include "mi/mi_priv.h"
#include "os/cmdline.h"
#include "os/ddx_priv.h"
#include "os/osdep.h"

#include "scrnintstr.h"
#include "servermd.h"
#include "fb.h"
#include "micmap.h"
#include "damage.h"
#include "glx_extinit.h"

#include "nextbsd.h"

nbScreen nbScreenInfo;

/* The name WindowServer shows for us (Command-Tab, wsctl list). */
#define NB_APP_NAME "XLibre"

/* Pixmap formats: what RENDER and core clients need, at fb's sizes. */
static const PixmapFormatRec nbFormats[] = {
    { 1, 1, BITMAP_SCANLINE_PAD },
    { 4, 8, BITMAP_SCANLINE_PAD },
    { 8, 8, BITMAP_SCANLINE_PAD },
    { 15, 16, BITMAP_SCANLINE_PAD },
    { 16, 16, BITMAP_SCANLINE_PAD },
    { 24, 32, BITMAP_SCANLINE_PAD },
    { 32, 32, BITMAP_SCANLINE_PAD },
};

ws_connection_t *
nbConnection(void)
{
    return nbScreenInfo.conn;
}

/*
 * The connection to WindowServer broke (it quit or crashed). Without it
 * there is nowhere to show anything, so the server shuts down cleanly once
 * the current request is done, as it does on a KillServer.
 */
void
nbWindowServerLost(const char *what)
{
    static Bool reported;

    if (!reported) {
        ErrorF("Xnextbsd: lost the connection to WindowServer (%s: %s); "
               "shutting down\n", what, strerror(errno));
        reported = TRUE;
    }
    dispatchException |= DE_TERMINATE;
    isItTimeToYield = TRUE;
}

/*
 * XNEXTBSD_JOIN=<client id>:<token> joins the app of the connection that
 * started us (XLibre.app, apps-xlibre#5), so WindowServer treats the
 * two as one app. It is removed from the environment at once, so X
 * clients never see the token.
 */
static ws_connection_t *
nbConnectJoin(uint32_t flags)
{
    const char *join = getenv("XNEXTBSD_JOIN");
    unsigned long id;
    unsigned long long token;
    ws_connection_t *conn;
    char *end;

    if (join == NULL || *join == '\0')
        return NULL;
    errno = 0;
    id = strtoul(join, &end, 10);
    if (errno != 0 || *end != ':' || id == 0 || id > UINT32_MAX) {
        ErrorF("Xnextbsd: XNEXTBSD_JOIN is not <client id>:<token>\n");
        unsetenv("XNEXTBSD_JOIN");
        return NULL;
    }
    token = strtoull(end + 1, &end, 0);
    unsetenv("XNEXTBSD_JOIN");
    if (errno != 0 || *end != '\0') {
        ErrorF("Xnextbsd: XNEXTBSD_JOIN is not <client id>:<token>\n");
        return NULL;
    }
    conn = ws_connect_join(NB_APP_NAME, flags, (uint32_t) id, token);
    if (conn == NULL)
        ErrorF("Xnextbsd: can't join WindowServer app %lu (%s); "
               "connecting as an app of our own\n", id, strerror(errno));
    return conn;
}

static void
nbConnect(void)
{
    const ws_screen_info_t *si;

    if (nbScreenInfo.conn != NULL)
        return;

    /*
     * WS_CONNECT_XKB: WindowServer's keymap, XKB modifier state and held
     * keys (WindowServers with WS_SERVER_XKB). Should an older one refuse
     * the flag, connect without it.
     */
    nbScreenInfo.conn = nbConnectJoin(WS_CONNECT_XKB);
    if (nbScreenInfo.conn == NULL)
        nbScreenInfo.conn = ws_connect(NB_APP_NAME, WS_CONNECT_XKB);
    if (nbScreenInfo.conn == NULL)
        nbScreenInfo.conn = ws_connect(NB_APP_NAME, 0);
    if (nbScreenInfo.conn == NULL)
        FatalError("Xnextbsd: can't connect to WindowServer: %s\n"
                   "(is WindowServer running? WS_SERVICE_NAME picks "
                   "another one)\n", strerror(errno));

    si = ws_main_screen(nbScreenInfo.conn);
    if (si == NULL || si->width == 0 || si->height == 0)
        FatalError("Xnextbsd: WindowServer reported no screen\n");
    nbScreenInfo.width = si->width;
    nbScreenInfo.height = si->height;
    nbScreenInfo.mm_width = si->mm_width;
    nbScreenInfo.mm_height = si->mm_height;
    xorg_list_init(&nbScreenInfo.frames);
    nbScreenInfo.server_flags = ws_server_flags(nbScreenInfo.conn);

    LogMessageVerb(X_INFO, 0, "Xnextbsd: connected to WindowServer as "
                   "client %u (app %u), screen %ux%u, server flags 0x%x\n",
                   ws_client_id(nbScreenInfo.conn),
                   ws_app_id(nbScreenInfo.conn), si->width, si->height,
                   nbScreenInfo.server_flags);

    if (nbScreenInfo.server_flags & WS_SERVER_XKB)
        nbInputWaitForKeymap();
}

/* ---- DDX hooks the dix and os layers call ---- */

void
OsVendorInit(void)
{
}

void
OsVendorFatalError(const char *f, va_list args)
{
}

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

void
ddxGiveUp(enum ExitCode error)
{
    if (nbScreenInfo.conn != NULL) {
        ws_disconnect(nbScreenInfo.conn);
        nbScreenInfo.conn = NULL;
    }
}

void
ddxUseMsg(void)
{
    ErrorF("\nXnextbsd: a rootless X server on NextBSD's WindowServer.\n"
           "It connects to the WindowServer named by WS_SERVICE_NAME "
           "(default org.nextbsd.windowserver);\n"
           "the screen size comes from WindowServer, and the keyboard "
           "layout from WindowServer's\n"
           "KeyboardRules, KeyboardModel, KeyboardLayout, KeyboardVariant "
           "and KeyboardOptions settings.\n");
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    return 0;
}

/* ---- the main loop ---- */

/*
 * Requests that wait for WindowServer (creating a window, for one) can read
 * events off the socket and queue them inside libWindowServerClient, where
 * poll() on the descriptor won't see them. Handle those before sleeping,
 * and don't sleep if there were any.
 */
static void
nbBlockHandler(void *data, void *timeout)
{
    if (nbScreenInfo.conn == NULL)
        return;
    nbFocusCheck();
    if (ws_pending(nbScreenInfo.conn)) {
        nbInputDrain();
        AdjustWaitForDelay(timeout, 0);
    }
}

static void
nbWakeupHandler(void *data, int result)
{
}

/* ---- the screen ---- */

static Bool
nbScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    int dpix, dpiy;

    /*
     * One visual: 24-bit TrueColor in 32-bit pixels, the layout of
     * WindowServer's ARGB8888 buffers (composite adds a 32-bit ARGB visual
     * for translucent windows).
     */
    miClearVisualTypes();
    if (!miSetVisualTypesAndMasks(24, TrueColorMask, 8, TrueColor,
                                  0x00ff0000, 0x0000ff00, 0x000000ff))
        return FALSE;
    if (!miSetPixmapDepths())
        return FALSE;

    if (monitorResolution) {
        dpix = dpiy = monitorResolution;
    } else {
        dpix = dpiy = 96;
        if (nbScreenInfo.mm_width > 0)
            dpix = (int) (nbScreenInfo.width * 25.4 /
                          nbScreenInfo.mm_width + 0.5);
        if (nbScreenInfo.mm_height > 0)
            dpiy = (int) (nbScreenInfo.height * 25.4 /
                          nbScreenInfo.mm_height + 0.5);
    }

    /*
     * No framebuffer: every top-level window draws into its own
     * WindowServer buffer. With a zero pitch miCreateScreenResources
     * leaves the screen pixmap to rootless (RootlessUpdateScreenPixmap).
     */
    if (!fbScreenInit(pScreen, NULL, nbScreenInfo.width, nbScreenInfo.height,
                      dpix, dpiy, 0, 32))
        return FALSE;
    if (!fbPictureInit(pScreen, 0, 0))
        return FALSE;

    /* Damage has to wrap underneath rootless, so it goes first. */
    if (!DamageSetup(pScreen))
        return FALSE;
    if (!nbFrameInit(pScreen))
        return FALSE;
    if (!nbCursorInit(pScreen))
        return FALSE;
    nbInputScreenInit(pScreen);

    if (!fbCreateDefColormap(pScreen))
        return FALSE;

    pScreen->x = 0;
    pScreen->y = 0;

    return RegisterBlockAndWakeupHandlers(nbBlockHandler, nbWakeupHandler,
                                          NULL);
}

void
InitOutput(int argc, char **argv)
{
    int i;

    nbConnect();

    screenInfo.imageByteOrder = IMAGE_BYTE_ORDER;
    screenInfo.bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screenInfo.bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screenInfo.bitmapBitOrder = BITMAP_BIT_ORDER;
    screenInfo.numPixmapFormats = ARRAY_SIZE(nbFormats);
    for (i = 0; i < ARRAY_SIZE(nbFormats); i++)
        screenInfo.formats[i] = nbFormats[i];

    xorgGlxCreateVendor();

    if (AddScreen(nbScreenInit, argc, argv) == -1)
        FatalError("Xnextbsd: couldn't add the screen\n");
}
