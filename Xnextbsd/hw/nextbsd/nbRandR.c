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
 * X has one screen, the size of WindowServer's: with several displays,
 * the rectangle around them all, as WindowServer's ws_screen_info_t is.
 * When WindowServer's changes (WS_EVENT_SCREEN_CHANGED: a mode change, a
 * display plugged in or out, or `wsctl screen size WxH` on a headless
 * server), X follows it as XQuartz follows a display change
 * (QuartzUpdateScreens, then xprUpdateScreen): the root window takes the
 * new size, rootless moves every frame back to where X has it and gives
 * the screen pixmap the new width, and RandR tells X clients
 * (RRScreenChangeNotify, and the root's ConfigureNotify).
 *
 * RandR shows WindowServer's displays (WS_SERVER_DISPLAYS, "Displays" in
 * its WSProtocol.h): an output per display, named after its connector
 * ("eDP-1", "DP-1"), each driven by a CRTC of its own at the display's
 * place in X's screen, in one mode, the display's size in X pixels, named
 * after its "looks like" size. The main display's output is the primary
 * one. A display that mirrors another is a clone: its output shares the
 * other's CRTC. RandR 1.5 makes a monitor of each CRTC, and its Xinerama
 * emulation (rrxinerama.c, on when the PanoramiX extension is off, as it
 * is by default) answers Xinerama from those, so clients that know
 * either one see each display, and put a full-screen window on one. When
 * the displays change (WS_EVENT_DISPLAYS_CHANGED) they are listed again
 * and X clients hear RRScreenChangeNotify and RRNotify for the outputs
 * and CRTCs that changed; an output whose display went away stays,
 * disconnected, as Xorg's do. With a WindowServer without displays (or
 * Xnextbsd built against older headers) it is what quartzRandR.c shows in
 * rootless mode: one output, named after WindowServer's screen, driven by
 * one CRTC in the current mode, and nothing else.
 *
 * These are WindowServer's to set (Displays.app, `wsctl`), so X clients
 * can't set them: asking for anything but the configuration the screen
 * is already in fails (RRSetConfigFailed). It is done with the RandR 1.2
 * calls rather than quartzRandR.c's 1.0 ones, so that an output lists
 * only the mode its display is in now, not every mode it has been in.
 * WindowServer places the pointer, so RandR doesn't keep it inside the
 * CRTCs (ConstrainCursorHarder).
 *
 * The usable area, the part of the screen WindowServer keeps clear of the
 * Dock and the menu bar on the main display (WS_EVENT_USABLE_AREA), is
 * published for the window manager on the root window as
 * NB_USABLE_AREA_ATOM, so it can place new windows inside it and keep
 * frames there after a screen change.
 *
 * HiDPI: WindowServer's screen is in points, X's in pixels, scale of them
 * per point (WindowServer's backing scale, or 1 with -lowres). A new
 * scale (the user picked another "looks like" size) changes X's screen
 * size like a mode change does, or not at all when the pixels stay the
 * same (a 3840 x 2160 panel at 1x, then looking like 1920 x 1080 at 2x),
 * and then every frame is shown at the new scale (nbFramesRescale), the
 * cursors are made again and NB_SCALE_ATOM tells XLibre.app. X windows
 * keep their size in pixels: their clients pick the new scale up from
 * XLibre.app's settings (XSETTINGS, Xft.dpi) as they can. Every display
 * is shown at the one scale (WindowServer draws the whole desktop at it
 * and scales each display to its panel), so X draws at 1x or that scale
 * as before, whichever display a window is on.
 */

#include <dix-config.h>

#include <errno.h>
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

/*
 * A display as RandR shows it. WindowServer's ws_display_info_t when it
 * has displays; otherwise one made up of its screen.
 */
typedef struct nbDisplay {
    uint32_t id;                /* WindowServer's display id; 0: the screen */
    Bool main, mirror;
    uint32_t mirror_of;         /* mirror: the display it shows */
    ws_rect_t frame;            /* points, in WindowServer's desktop */
    unsigned int refresh_mhz;   /* 0 if unknown */
    int mm_width, mm_height;    /* 0 if unknown */
    char name[32];              /* the output's: the display's connector */
} nbDisplay;

#define NB_MAX_DISPLAYS 8       /* WS_MAX_DISPLAYS */
#define NB_MAX_OUTPUTS 16       /* displays that have been connected */

static nbDisplay nbDisplays[NB_MAX_DISPLAYS];
static int nbNumDisplays;       /* 0: none listed, RandR shows the screen */
static Bool nbDisplaysDirty;    /* listed again, not shown by RandR yet */

static RRCrtcPtr nbCrtcs[NB_MAX_DISPLAYS];
static uint32_t nbCrtcDisplay[NB_MAX_DISPLAYS]; /* the display id each
                                                   CRTC last showed */
static int nbNumCrtcs;
static RROutputPtr nbOutputs[NB_MAX_OUTPUTS];
static int nbNumOutputs;

/* ---- WindowServer's displays ---- */

/*
 * The point at X's 0, 0: the top-left corner of the rectangle around
 * the displays, which WindowServer's screen is. 0, 0 on WindowServers
 * without displays, as Xnextbsd has always had it.
 */
void
nbScreenOrigin(const ws_screen_info_t *si, int *x, int *y)
{
    *x = *y = 0;
#if NB_DISPLAYS
    if ((nbScreenInfo.server_flags & WS_SERVER_DISPLAYS) && si != NULL) {
        *x = si->x;
        *y = si->y;
    }
#endif
}

#if NB_DISPLAYS
typedef struct nbDisplayList {
    nbDisplay d[NB_MAX_DISPLAYS];
    int n;
} nbDisplayList;

static void
nbDisplayListed(const ws_display_info_t *di, void *ctx)
{
    nbDisplayList *l = ctx;
    nbDisplay *d;

    if (l->n >= NB_MAX_DISPLAYS || di->frame.width <= 0 ||
        di->frame.height <= 0)
        return;
    d = &l->d[l->n++];
    memset(d, 0, sizeof(*d));
    d->id = di->display_id;
    d->main = (di->flags & WS_DISPLAY_MAIN) != 0;
    d->mirror = (di->flags & WS_DISPLAY_MIRROR) != 0;
    d->mirror_of = di->mirror_of;
    d->frame = di->frame;
    d->refresh_mhz = di->refresh_mhz;
    d->mm_width = (int) di->mm_width;
    d->mm_height = (int) di->mm_height;
    if (di->connector[0] != '\0')
        snprintf(d->name, sizeof(d->name), "%.*s",
                 (int) sizeof(di->connector), di->connector);
    else
        snprintf(d->name, sizeof(d->name), "display-%u", di->display_id);
}
#endif

/*
 * Ask WindowServer for its displays (main first), into nbDisplays. On a
 * WindowServer without them, or should it fail, there are none, and
 * RandR shows the screen.
 */
static void
nbDisplaysList(void)
{
#if NB_DISPLAYS
    nbDisplayList l;

    l.n = 0;
    if (!(nbScreenInfo.server_flags & WS_SERVER_DISPLAYS) ||
        nbConnection() == NULL)
        return;
    if (ws_list_displays(nbConnection(), nbDisplayListed, &l) < 0) {
        ErrorF("Xnextbsd: can't list WindowServer's displays: %s\n",
               strerror(errno));
        l.n = 0;
    }
    memcpy(nbDisplays, l.d, sizeof(nbDisplays));
    nbNumDisplays = l.n;
    nbDisplaysDirty = TRUE;
    for (int i = 0; i < l.n; i++)
        LogMessageVerb(X_INFO, 1, "Xnextbsd: display %u %s: %dx%d+%d+%d "
                       "points%s%s\n", l.d[i].id, l.d[i].name,
                       l.d[i].frame.width, l.d[i].frame.height,
                       l.d[i].frame.x, l.d[i].frame.y,
                       l.d[i].main ? ", main" : "",
                       l.d[i].mirror ? ", mirror" : "");
#endif
}

/*
 * WS_EVENT_DISPLAYS_CHANGED: list them again. WS_EVENT_SCREEN_CHANGED
 * follows when the screen changed with them, and shows them
 * (nbScreenChanged); otherwise the block handler does (nbDisplaysPublish),
 * once the events that came with this one are in.
 */
void
nbDisplaysChanged(void)
{
    nbDisplaysList();
}

/* ---- RandR ---- */

/*
 * A mode of width x height X pixels at refresh_mhz (60 Hz if unknown),
 * named name_w x name_h.
 */
static RRModePtr
nbRandRMode(int width, int height, int name_w, int name_h,
            unsigned int refresh_mhz)
{
    xRRModeInfo info;
    char name[32];
    unsigned int mhz = refresh_mhz ? refresh_mhz : 60000;

    memset(&info, 0, sizeof(info));
    snprintf(name, sizeof(name), "%dx%d", name_w, name_h);
    info.width = width;
    info.height = height;
    info.hTotal = width;
    info.vTotal = height;
    /* RandR's refresh rate is dotClock / (hTotal * vTotal). */
    info.dotClock = (CARD32) ((uint64_t) width * height * mhz / 1000);
    info.nameLength = strlen(name);
    return RRModeGet(&info, name);
}

/* The output named name, made the first time. */
static RROutputPtr
nbRandROutput(ScreenPtr pScreen, const char *name)
{
    RROutputPtr output;
    int i;

    for (i = 0; i < nbNumOutputs; i++)
        if (strcmp(nbOutputs[i]->name, name) == 0)
            return nbOutputs[i];
    /* Full: an output of a display long gone makes room. */
    if (nbNumOutputs == NB_MAX_OUTPUTS) {
        for (i = 0; i < nbNumOutputs; i++)
            if (nbOutputs[i]->crtc == NULL &&
                nbOutputs[i]->connection != RR_Connected)
                break;
        if (i == nbNumOutputs)
            return NULL;
        RROutputDestroy(nbOutputs[i]);
        memmove(&nbOutputs[i], &nbOutputs[i + 1],
                (nbNumOutputs - i - 1) * sizeof(nbOutputs[0]));
        nbNumOutputs--;
    }
    output = RROutputCreate(pScreen, name, strlen(name), NULL);
    if (output == NULL)
        return NULL;
    if (!RROutputSetClones(output, NULL, 0) ||
        !RROutputSetConnection(output, RR_Disconnected)) {
        RROutputDestroy(output);
        return NULL;
    }
    nbOutputs[nbNumOutputs++] = output;
    return output;
}

static RRCrtcPtr
nbRandRCrtcCreate(ScreenPtr pScreen)
{
    RRCrtcPtr crtc = RRCrtcCreate(pScreen, NULL);

    if (crtc == NULL)
        return NULL;
    RRCrtcSetRotations(crtc, RR_Rotate_0);
    /* Gamma isn't WindowServer's to change, but xrandr wants a size. */
    RRCrtcGammaSetSize(crtc, 256);
    nbCrtcDisplay[nbNumCrtcs] = 0;
    nbCrtcs[nbNumCrtcs++] = crtc;
    return crtc;
}

/* The display at index i, its source's index for a mirror, or -1. */
static int
nbDisplayShown(const nbDisplay *d, int n, int i)
{
    if (!d[i].mirror)
        return i;
    for (int j = 0; j < n; j++)
        if (j != i && d[j].id == d[i].mirror_of && !d[j].mirror)
            return j;
    /* The display it mirrors isn't listed: it shows its frame alone. */
    return i;
}

/*
 * Show the displays, or the screen when there are none: every output's
 * connection, modes and CRTC, every CRTC's mode and place, the primary
 * output and the screen's size range. RandR sends the events when it is
 * next told something changed (RRScreenSizeNotify or RRTellChanged).
 */
static Bool
nbRandRSetCurrent(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv = rrGetScrPriv(pScreen);
    const nbDisplay *d = nbDisplays;
    int n = nbNumDisplays, s = (int) nbScreenInfo.scale;
    nbDisplay screen;
    RROutputPtr out[NB_MAX_DISPLAYS], group[NB_MAX_DISPLAYS];
    RRCrtcPtr crtc[NB_MAX_DISPLAYS];
    Bool crtc_used[NB_MAX_DISPLAYS] = { FALSE };
    Bool output_used[NB_MAX_OUTPUTS] = { FALSE };
    RROutputPtr primary = NULL;
    Bool ok = TRUE;
    int i, j, k, ngroup;

    if (pScrPriv == NULL)
        return FALSE;
    nbDisplaysDirty = FALSE;

    /* Without displays, one of the screen, as before them. */
    if (n == 0) {
        memset(&screen, 0, sizeof(screen));
        screen.main = TRUE;
        screen.frame.x = nbScreenInfo.origin_x;
        screen.frame.y = nbScreenInfo.origin_y;
        screen.frame.width = pScreen->width / s;
        screen.frame.height = pScreen->height / s;
        screen.refresh_mhz = nbScreenInfo.refresh_mhz;
        screen.mm_width = pScreen->mmWidth;
        screen.mm_height = pScreen->mmHeight;
        snprintf(screen.name, sizeof(screen.name), "%s",
                 nbScreenInfo.screen_name[0] != '\0' ?
                 nbScreenInfo.screen_name : "default");
        d = &screen;
        n = 1;
    }

    /* The outputs, made as displays first come. */
    for (i = 0; i < n; i++) {
        out[i] = nbRandROutput(pScreen, d[i].name);
        crtc[i] = NULL;
        if (out[i] == NULL)
            ok = FALSE;
        /* Two displays on one connector: the first has it. */
        for (j = 0; j < i; j++)
            if (out[j] == out[i])
                out[i] = NULL;
    }

    /*
     * A CRTC per display that isn't a mirror, the one it had before
     * when there was one, so CRTCs don't change hands as displays come
     * and go.
     */
    for (i = 0; i < n; i++) {
        if (out[i] == NULL || nbDisplayShown(d, n, i) != i)
            continue;
        for (k = 0; k < nbNumCrtcs; k++)
            if (!crtc_used[k] && nbCrtcDisplay[k] == d[i].id)
                break;
        if (k == nbNumCrtcs)
            for (k = 0; k < nbNumCrtcs; k++)
                if (!crtc_used[k])
                    break;
        if (k == nbNumCrtcs && nbRandRCrtcCreate(pScreen) == NULL) {
            ok = FALSE;
            continue;
        }
        crtc_used[k] = TRUE;
        nbCrtcDisplay[k] = d[i].id;
        crtc[i] = nbCrtcs[k];
    }

    /* Every output can be shown by any CRTC. */
    for (j = 0; j < nbNumOutputs; j++)
        if (!RROutputSetCrtcs(nbOutputs[j], nbCrtcs, nbNumCrtcs))
            ok = FALSE;

    /*
     * Each CRTC: its display's place and size, in X pixels, with that
     * display's output and those of the displays that mirror it.
     */
    for (i = 0; i < n; i++) {
        int x, y, w, h;
        RRModePtr mode;

        if (crtc[i] == NULL)
            continue;
        ngroup = 0;
        for (j = 0; j < n; j++)
            if (out[j] != NULL && crtc[nbDisplayShown(d, n, j)] == crtc[i])
                group[ngroup++] = out[j];
        x = (d[i].frame.x - nbScreenInfo.origin_x) * s;
        y = (d[i].frame.y - nbScreenInfo.origin_y) * s;
        w = d[i].frame.width * s;
        h = d[i].frame.height * s;
        for (j = 0; j < n; j++) {
            RROutputPtr clones[NB_MAX_DISPLAYS];
            int nclones = 0;

            if (out[j] == NULL || crtc[nbDisplayShown(d, n, j)] != crtc[i])
                continue;
            /*
             * The mode is named after the "looks like" size (the frame,
             * in points); without displays, after the screen's pixels,
             * as it always was.
             */
            mode = nbRandRMode(w, h,
                               d == &screen ? w : d[i].frame.width,
                               d == &screen ? h : d[i].frame.height,
                               d[i].refresh_mhz);
            /* The output takes this reference, RRCrtcNotify one of its
               own. */
            if (mode == NULL || !RROutputSetModes(out[j], &mode, 1, 1))
                ok = FALSE;
            for (k = 0; k < ngroup; k++)
                if (group[k] != out[j])
                    clones[nclones++] = group[k];
            if (!RROutputSetClones(out[j], clones, nclones) ||
                !RROutputSetConnection(out[j], RR_Connected))
                ok = FALSE;
            RROutputSetPhysicalSize(out[j], d[j].mm_width, d[j].mm_height);
            for (k = 0; k < nbNumOutputs; k++)
                if (nbOutputs[k] == out[j])
                    output_used[k] = TRUE;
            if (d[j].main && primary == NULL)
                primary = out[j];
        }
        mode = nbRandRMode(w, h, d == &screen ? w : d[i].frame.width,
                           d == &screen ? h : d[i].frame.height,
                           d[i].refresh_mhz);
        if (mode == NULL) {
            ok = FALSE;
            continue;
        }
        if (!RRCrtcNotify(crtc[i], mode, x, y, RR_Rotate_0, NULL, ngroup,
                          group))
            ok = FALSE;
        /* RRCrtcNotify took a reference of its own. */
        RRModeDestroy(mode);
    }

    /* CRTCs no display needs now are off. */
    for (k = 0; k < nbNumCrtcs; k++)
        if (!crtc_used[k] &&
            !RRCrtcNotify(nbCrtcs[k], NULL, 0, 0, RR_Rotate_0, NULL, 0, NULL))
            ok = FALSE;

    /* Outputs whose display went away are disconnected. */
    for (k = 0; k < nbNumOutputs; k++) {
        if (output_used[k])
            continue;
        RROutputSetModes(nbOutputs[k], NULL, 0, 0);
        RROutputSetClones(nbOutputs[k], NULL, 0);
        RROutputSetConnection(nbOutputs[k], RR_Disconnected);
    }

    /* The primary output is the main display's (RRSetPrimaryOutput). */
    if (primary == NULL)
        primary = out[0];
    if (pScrPriv->primaryOutput != primary) {
        if (pScrPriv->primaryOutput != NULL)
            RROutputChanged(pScrPriv->primaryOutput, FALSE);
        pScrPriv->primaryOutput = primary;
        if (primary != NULL)
            RROutputChanged(primary, FALSE);
        pScrPriv->layoutChanged = TRUE;
    }

    RRScreenSetSizeRange(pScreen, pScreen->width, pScreen->height,
                         pScreen->width, pScreen->height);
    return ok;
}

/*
 * The displays were listed again and nothing showed them yet: before
 * sleeping, RandR shows them and tells X clients.
 */
void
nbDisplaysPublish(void)
{
    ScreenPtr pScreen = screenInfo.screens[0];

    if (!nbDisplaysDirty || pScreen == NULL || pScreen->root == NULL)
        return;
    nbRandRSetCurrent(pScreen);
    RRTellChanged(pScreen);
}

static Bool
nbRandRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    *rotations = RR_Rotate_0;
    return TRUE;
}

/*
 * X clients can't change WindowServer's displays: only the configuration
 * a CRTC is already in is accepted (the same size, place and outputs; a
 * mode of the same size counts). RRCrtcSet doesn't call this for a
 * request that changes nothing.
 */
static Bool
nbRandRCrtcSet(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x,
               int y, Rotation rotation, int numOutputs,
               RROutputPtr *outputs)
{
    int i, j;

    if (mode == NULL || crtc->mode == NULL || x != crtc->x ||
        y != crtc->y || rotation != RR_Rotate_0 ||
        numOutputs != crtc->numOutputs ||
        mode->mode.width != crtc->mode->mode.width ||
        mode->mode.height != crtc->mode->mode.height)
        return FALSE;
    for (i = 0; i < numOutputs; i++) {
        for (j = 0; j < crtc->numOutputs; j++)
            if (outputs[i] == crtc->outputs[j])
                break;
        if (j == crtc->numOutputs)
            return FALSE;
    }
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

    if (!RRScreenInit(pScreen))
        return FALSE;
    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = nbRandRGetInfo;
    pScrPriv->rrCrtcSet = nbRandRCrtcSet;
    pScrPriv->rrScreenSetSize = nbRandRScreenSetSize;
    /*
     * The pointer is where WindowServer says, in a gap between displays
     * or not: RandR doesn't keep it inside the CRTCs.
     */
    pScreen->ConstrainCursorHarder = NULL;

    nbDisplaysList();
    if (nbRandRCrtcCreate(pScreen) == NULL)
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
    int old_x = nbScreenInfo.origin_x, old_y = nbScreenInfo.origin_y;
    int origin_x, origin_y;
    Bool moved;

    if (si->refresh_mhz != 0)
        nbScreenInfo.refresh_mhz = si->refresh_mhz;
    if (pScreen == NULL || (pRoot = pScreen->root) == NULL ||
        width <= 0 || height <= 0 || width > MAXSHORT || height > MAXSHORT)
        return;
    nbScreenInfo.ws_scale = nbBackingScale(si);
    /*
     * The desktop's top-left corner, X's 0, 0, moves when a display goes
     * left of or above the others: from here on positions are taken from
     * the new one, and the frames move in X to stay where they are on
     * WindowServer (nbFramesOriginChanged, once the root has its size).
     */
    nbScreenOrigin(si, &origin_x, &origin_y);
    moved = origin_x != old_x || origin_y != old_y;
    if (moved) {
        LogMessageVerb(X_INFO, 0, "Xnextbsd: WindowServer's desktop starts "
                       "at %d,%d points\n", origin_x, origin_y);
        nbScreenInfo.origin_x = origin_x;
        nbScreenInfo.origin_y = origin_y;
        nbScreenInfo.usable_dirty = TRUE;
    }
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
        if (moved)
            nbFramesOriginChanged(old_x, old_y);
        if (scale != old_scale)
            nbScaleChanged();
        /* The displays may have moved or changed scale inside it. */
        nbRandRSetCurrent(pScreen);
        RRTellChanged(pScreen);
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

    if (moved)
        nbFramesOriginChanged(old_x, old_y);

    /*
     * RandR: the displays' new modes and places, then the root's
     * ConfigureNotify, RRScreenChangeNotify and the CRTC and output
     * notifies, and the connection setup new clients get.
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
    value[0] = (nbScreenInfo.usable.x - nbScreenInfo.origin_x) *
        nbScreenInfo.scale;
    value[1] = (nbScreenInfo.usable.y - nbScreenInfo.origin_y) *
        nbScreenInfo.scale;
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
    ws_rect_t r = { nbScreenInfo.origin_x, nbScreenInfo.origin_y,
                    nbScreenInfo.width / (int) nbScreenInfo.scale,
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
