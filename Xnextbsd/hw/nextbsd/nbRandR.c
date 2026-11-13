/*
 * Xnextbsd: screen changes, RandR and the usable area.
 *
 * Copyright (c) 2026, Joseph Maloney
 * Modelled on hw/xquartz/quartz.c (QuartzUpdateScreens),
 * hw/xquartz/xpr/xprScreen.c (xprUpdateScreen) and hw/xquartz/quartzRandR.c:
 * Copyright (c) 2001-2004 Greg Parker and Torrey T. Lyons,
 * Copyright (c) 2002-2012 Apple Computer, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE ABOVE LISTED COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name(s) of the above copyright
 * holders shall not be used in advertising or otherwise to promote the sale,
 * use or other dealings in this Software without prior written authorization.
 */

/*
 * WindowServer has one screen, and X has one screen the same size. When
 * WindowServer's changes (WS_EVENT_SCREEN_CHANGED: a mode change, or
 * `wsctl screen size WxH` on a headless server), X follows it as XQuartz
 * follows a display change (QuartzUpdateScreens, then xprUpdateScreen):
 * the root window takes the new size, rootless moves every frame back to
 * where X has it and gives the screen pixmap the new width, and RandR
 * tells X clients (RRScreenChangeNotify, and the root's ConfigureNotify).
 *
 * RandR shows what quartzRandR.c shows in rootless mode: one output, named
 * after WindowServer's screen, driven by one CRTC in the current mode, and
 * nothing else. That mode is WindowServer's, so X clients can't set
 * another one: asking for anything but the current configuration fails
 * (RRSetConfigFailed). It is done with the RandR 1.2 calls rather than
 * quartzRandR.c's 1.0 ones, so that the output lists only the mode the
 * screen is in now, not every mode it has been in.
 *
 * The usable area, the part of the screen WindowServer keeps clear of the
 * Dock (WS_EVENT_USABLE_AREA), is published for the window manager on the
 * root window as NB_USABLE_AREA_ATOM, so it can place new windows inside
 * it and keep frames there after a screen change.
 *
 * HiDPI: WindowServer's screen is in points, X's in pixels, scale of them
 * per point (WindowServer's backing scale, or 1 with -lowres). A new
 * scale (the user picked another "looks like" size) changes X's screen
 * size like a mode change does, or not at all when the pixels stay the
 * same (a 3840 x 2160 panel at 1x, then looking like 1920 x 1080 at 2x),
 * and then every frame is shown at the new scale (nbFramesRescale), the
 * cursors are made again and NB_SCALE_ATOM tells XLibre.app. X windows
 * keep their size in pixels: their clients pick the new scale up from
 * XLibre.app's settings (XSETTINGS, Xft.dpi) as they can.
 */

#include <dix-config.h>

#include <stdio.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/input_priv.h"

#include "scrnintstr.h"
#include "windowstr.h"
#include "inputstr.h"
#include "propertyst.h"
#include "randrstr.h"
#include "rootless.h"

#include "nextbsd.h"

static void nbScaleChanged(void);

static RRCrtcPtr nbCrtc;
static RROutputPtr nbOutput;

/* ---- RandR ---- */

/* The output's mode: WindowServer's screen size at its refresh rate. */
static RRModePtr
nbRandRMode(int width, int height)
{
    xRRModeInfo info;
    char name[32];
    unsigned int mhz = nbScreenInfo.refresh_mhz ? nbScreenInfo.refresh_mhz
                                                : 60000;

    memset(&info, 0, sizeof(info));
    snprintf(name, sizeof(name), "%dx%d", width, height);
    info.width = width;
    info.height = height;
    info.hTotal = width;
    info.vTotal = height;
    /* RandR's refresh rate is dotClock / (hTotal * vTotal). */
    info.dotClock = (CARD32) ((uint64_t) width * height * mhz / 1000);
    info.nameLength = strlen(name);
    return RRModeGet(&info, name);
}

/*
 * Make the output's only mode the screen's current size, and show the CRTC
 * in it. RandR sends the events when it is next told something changed
 * (RRScreenSizeNotify or RRTellChanged).
 */
static Bool
nbRandRSetCurrent(ScreenPtr pScreen)
{
    RRModePtr mode;

    if (nbOutput == NULL || nbCrtc == NULL)
        return FALSE;
    mode = nbRandRMode(pScreen->width, pScreen->height);
    if (mode == NULL)
        return FALSE;
    /* The output takes this reference, RRCrtcNotify one of its own. */
    if (!RROutputSetModes(nbOutput, &mode, 1, 1))
        return FALSE;
    RROutputSetPhysicalSize(nbOutput, pScreen->mmWidth, pScreen->mmHeight);
    RRScreenSetSizeRange(pScreen, pScreen->width, pScreen->height,
                         pScreen->width, pScreen->height);
    return RRCrtcNotify(nbCrtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &nbOutput);
}

static Bool
nbRandRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    *rotations = RR_Rotate_0;
    return TRUE;
}

/*
 * X clients can't change WindowServer's mode (yet): only the configuration
 * the screen is already in is accepted. RRCrtcSet doesn't call this for a
 * request that changes nothing.
 */
static Bool
nbRandRCrtcSet(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x,
               int y, Rotation rotation, int numOutputs,
               RROutputPtr *outputs)
{
    if (crtc != nbCrtc || mode == NULL || x != 0 || y != 0 ||
        rotation != RR_Rotate_0 || numOutputs != 1 ||
        outputs[0] != nbOutput || mode->mode.width != pScreen->width ||
        mode->mode.height != pScreen->height)
        return FALSE;
    return RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutputs,
                        outputs);
}

static Bool
nbRandRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                     CARD32 mmWidth, CARD32 mmHeight)
{
    return width == pScreen->width && height == pScreen->height;
}

Bool
nbRandRInit(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv;
    const char *name;

    if (!RRScreenInit(pScreen))
        return FALSE;
    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = nbRandRGetInfo;
    pScrPriv->rrCrtcSet = nbRandRCrtcSet;
    pScrPriv->rrScreenSetSize = nbRandRScreenSetSize;

    nbCrtc = RRCrtcCreate(pScreen, NULL);
    if (nbCrtc == NULL)
        return FALSE;
    RRCrtcSetRotations(nbCrtc, RR_Rotate_0);
    /* Gamma isn't WindowServer's to change, but xrandr wants a size. */
    RRCrtcGammaSetSize(nbCrtc, 256);

    name = nbScreenInfo.screen_name[0] != '\0' ? nbScreenInfo.screen_name
                                               : "default";
    nbOutput = RROutputCreate(pScreen, name, strlen(name), NULL);
    if (nbOutput == NULL)
        return FALSE;
    if (!RROutputSetClones(nbOutput, NULL, 0) ||
        !RROutputSetCrtcs(nbOutput, &nbCrtc, 1) ||
        !RROutputSetConnection(nbOutput, RR_Connected))
        return FALSE;
    return nbRandRSetCurrent(pScreen);
}

/* ---- screen changes ---- */

/*
 * WindowServer's screen changed (WS_EVENT_SCREEN_CHANGED): take its new
 * size, as QuartzUpdateScreens and xprUpdateScreen do. WindowServer then
 * moves the windows that no longer fit back onto the screen and tells us
 * with WS_EVENT_WINDOW_FRAME (nbFrameMovedByWindowServer).
 */
void
nbScreenChanged(const ws_screen_info_t *si)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    WindowPtr pRoot;
    DeviceIntPtr pointer;
    BoxRec bounds;
    unsigned int scale = nbDrawScale(si), old_scale = nbScreenInfo.scale;
    int width = si->width * scale, height = si->height * scale;
    int mm_width, mm_height;

    if (si->refresh_mhz != 0)
        nbScreenInfo.refresh_mhz = si->refresh_mhz;
    if (pScreen == NULL || (pRoot = pScreen->root) == NULL ||
        width <= 0 || height <= 0 || width > MAXSHORT || height > MAXSHORT)
        return;
    nbScreenInfo.ws_scale = nbBackingScale(si);
    if (scale != old_scale) {
        LogMessageVerb(X_INFO, 0, "Xnextbsd: WindowServer's scale changed: "
                       "%u -> %u pixels per point\n", old_scale, scale);
        /* From here on frames, pointer events and the usable area are
           taken at the new scale. */
        nbScreenInfo.scale = scale;
        nbScreenInfo.scale_dirty = TRUE;
        nbScreenInfo.usable_dirty = TRUE;
    }
    /*
     * The physical size WindowServer reports, or, without one, the same
     * resolution as before (QuartzUpdateScreens), or 96 dpi per point
     * when the scale changed.
     */
    if (si->mm_width > 0 && si->mm_height > 0) {
        mm_width = si->mm_width;
        mm_height = si->mm_height;
    } else if (scale != old_scale) {
        mm_width = (int) (width * 25.4 / (96.0 * scale) + 0.5);
        mm_height = (int) (height * 25.4 / (96.0 * scale) + 0.5);
    } else {
        mm_width = pScreen->mmWidth * ((double) width / pScreen->width) + 0.5;
        mm_height = pScreen->mmHeight * ((double) height / pScreen->height) +
            0.5;
    }
    if (width == pScreen->width && height == pScreen->height &&
        mm_width == pScreen->mmWidth && mm_height == pScreen->mmHeight) {
        if (scale != old_scale)
            nbScaleChanged();
        return;
    }

    LogMessageVerb(X_INFO, 0, "Xnextbsd: WindowServer's screen changed: "
                   "%dx%d -> %dx%d pixels\n", pScreen->width,
                   pScreen->height, width, height);

    pScreen->mmWidth = mm_width;
    pScreen->mmHeight = mm_height;
    pScreen->width = width;
    pScreen->height = height;
    nbScreenInfo.width = width;
    nbScreenInfo.height = height;
    nbScreenInfo.mm_width = pScreen->mmWidth;
    nbScreenInfo.mm_height = pScreen->mmHeight;
    /* screenInfo's size scales the pointer's absolute positions. */
    update_desktop_dimensions();

    /* The root window: rootless resizes it in place. */
    pScreen->ResizeWindow(pRoot, 0, 0, width, height, NULL);

    /* Let the pointer reach the whole new screen, and no further. */
    bounds.x1 = 0;
    bounds.y1 = 0;
    bounds.x2 = width;
    bounds.y2 = height;
    pointer = inputInfo.pointer;
    if (pointer != NULL && pointer->spriteInfo != NULL &&
        pointer->spriteInfo->sprite != NULL) {
        pScreen->ConstrainCursor(pointer, pScreen, &bounds);
        pointer->spriteInfo->sprite->physLimits = bounds;
        pointer->spriteInfo->sprite->hotLimits = bounds;
    }

    /* xprUpdateScreen */
    RootlessRepositionWindows(pScreen);
    RootlessUpdateScreenPixmap(pScreen);

    /* After RootlessUpdateScreenPixmap, as QuartzUpdateScreens has it. */
    pScreen->PaintWindow(pRoot, &pRoot->borderClip, PW_BACKGROUND);

    /*
     * RandR: the new mode, then the root's ConfigureNotify,
     * RRScreenChangeNotify and the CRTC and output notifies, and the
     * connection setup new clients get.
     */
    nbRandRSetCurrent(pScreen);
    RRScreenSizeNotify(pScreen);
    RRTellChanged(pScreen);

    if (scale != old_scale)
        nbScaleChanged();
}

/*
 * The scale X draws at changed: the frames, the cursors and the root
 * window's NB_SCALE_ATOM and NB_USABLE_AREA_ATOM follow it.
 */
static void
nbScaleChanged(void)
{
    nbFramesRescale();
    nbCursorRescale();
    nbScalePublish();
    nbUsableAreaPublish();
}

/* ---- the usable area ---- */

/*
 * Put the usable area on the root window (NB_USABLE_AREA_ATOM, CARDINAL/32
 * x, y, width, height), once there is a root window and whenever it
 * changes.
 */
void
nbUsableAreaPublish(void)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    CARD32 value[4];
    Atom atom;

    if (!nbScreenInfo.usable_dirty || pScreen == NULL ||
        pScreen->root == NULL)
        return;
    atom = MakeAtom(NB_USABLE_AREA_ATOM, strlen(NB_USABLE_AREA_ATOM), TRUE);
    if (atom == None)
        return;
    /* WindowServer's points, in X's pixels */
    value[0] = nbScreenInfo.usable.x * nbScreenInfo.scale;
    value[1] = nbScreenInfo.usable.y * nbScreenInfo.scale;
    value[2] = nbScreenInfo.usable.width * nbScreenInfo.scale;
    value[3] = nbScreenInfo.usable.height * nbScreenInfo.scale;
    if (dixChangeWindowProperty(serverClient, pScreen->root, atom,
                                XA_CARDINAL, 32, PropModeReplace, 4, value,
                                TRUE) == Success)
        nbScreenInfo.usable_dirty = FALSE;
}

/*
 * WindowServer's usable area: from the welcome (nbConnect) and
 * WS_EVENT_USABLE_AREA. Without one (an older WindowServer, or an empty
 * area), the whole screen.
 */
void
nbUsableAreaChanged(const ws_rect_t *usable)
{
    ws_rect_t r = { 0, 0, nbScreenInfo.width / (int) nbScreenInfo.scale,
                    nbScreenInfo.height / (int) nbScreenInfo.scale };

    if (usable != NULL && usable->width > 0 && usable->height > 0)
        r = *usable;
    if (!nbScreenInfo.usable_dirty && r.x == nbScreenInfo.usable.x &&
        r.y == nbScreenInfo.usable.y && r.width == nbScreenInfo.usable.width &&
        r.height == nbScreenInfo.usable.height)
        return;
    nbScreenInfo.usable = r;
    nbScreenInfo.usable_dirty = TRUE;
    nbUsableAreaPublish();
}
