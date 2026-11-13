/*
 * Xnextbsd: rootless frames as WindowServer windows.
 *
 * Copyright (c) 2026, Joseph Maloney
 * Modelled on hw/xquartz/xpr/xprFrame.c:
 * Copyright (c) 2002-2012 Apple Computer, Inc. All rights reserved.
 * Copyright (c) 2003 Torrey T. Lyons. All Rights Reserved.
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
 * Each top-level X window is one WindowServer window. Rootless draws into
 * a private backing (StartDrawing hands it the pixels and stride), reports
 * what it drew with DamageRects, and asks for it to be shown with
 * StopDrawing(flush) or UpdateRegion, at most every
 * ROOTLESS_REDISPLAY_DELAY ms.
 *
 * WindowServer composites straight from a window's attached buffer, so X
 * never draws into that one. Showing the damage (nbFrameCommit) takes a
 * buffer WindowServer doesn't hold, copies into it from the backing what
 * changed since that buffer was last brought up to date (its stale
 * region), attaches it and commits. WindowServer hands the previous
 * buffer back (WS_EVENT_BUFFER_RELEASE), and the next commit uses that
 * one: two buffers per window, as Xwayland's xwayland-window-buffers.c
 * does.
 *
 * Commits are paced by WindowServer's frames: each asks for
 * WS_EVENT_FRAME_DONE, and until it comes, damage just accumulates and is
 * committed in one go when it does (nbFrameDone). A window drawing faster
 * than the screen refreshes costs one copy per frame shown, not one per
 * redisplay.
 *
 * With a WindowServer that has them, a resize carries the new frame in
 * the commit of the first buffer at the new size
 * (WS_SERVER_COMMIT_FRAME), so the window never shows its new size with
 * old contents; frames are created without a round trip each
 * (WS_SERVER_WINDOW_IDS); and shaped windows (X Shape) take pointer
 * events only inside their bounding and input shapes
 * (WS_SERVER_INPUT_REGION), while their buffers are premultiplied
 * ARGB8888 with nothing outside the bounding shape, so they look shaped
 * whatever their depth.
 *
 * Window tiling (WS_SERVER_TILING): WindowServer gives windows new frames,
 * sizes included (WS_EVENT_WINDOW_FRAME). X follows: the top-level window
 * is moved and resized, within the client's WM_NORMAL_HINTS, and the
 * frame WindowServer has is put right when X's differs. Each frame tells
 * WindowServer its size limits from those hints and whether it is a panel
 * (a transient or dialog window, or an override-redirect one) or can't be
 * resized, so that those float above the tiles.
 *
 * Minimizing (NB_MINIMIZED_ATOM): the window manager asks for it with a
 * property on its frame, the frame is minimized on WindowServer while the
 * X window stays mapped (so the Dock's preview of it is live), and
 * WindowServer's WS_EVENT_WINDOW_RESTORE goes back to the window manager.
 *
 * HiDPI (WS_SERVER_SCALE): X draws at nbScreenInfo.scale pixels per
 * WindowServer point. Frames, damage, input regions and size limits go
 * to WindowServer in points (nbPixelFrameToPoints in nextbsd.h), and the
 * buffers carry the scale, so WindowServer shows them pixel for pixel.
 * When the scale changes, every frame gets new buffers and its frame in
 * the new points (nbFramesRescale).
 */

#include <dix-config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <X11/X.h>

#include "dix/dix_priv.h"
#include "dix/property_priv.h"
#include "dix/resource_priv.h"
#include "dix/window_priv.h"
#include "inputstr.h"
#include "propertyst.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "rootlessCommon.h"

#include "nextbsd.h"

/* At most this many damage rectangles per commit; more become their
   bounding box. */
#define NB_MAX_DAMAGE_RECTS 64

static ws_rect_t
nbRect(int x, int y, unsigned int w, unsigned int h)
{
    ws_rect_t r = { x, y, (int32_t) w, (int32_t) h };

    return r;
}

nbFrame *
nbFrameFromWID(uint32_t wid)
{
    nbFrame *frame;

    if (wid == 0)
        return NULL;
    xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link)
        if (frame->wid == wid)
            return frame;
    return NULL;
}

WindowPtr
nbFrameWindow(nbFrame *frame)
{
    return frame != NULL && frame->rwin != NULL ? frame->rwin->win : NULL;
}

/* The frame in WindowServer's points. */
static ws_rect_t
nbFramePoints(int x, int y, unsigned int w, unsigned int h)
{
    return nbPixelFrameToPoints(x, y, w, h);
}

/*
 * A buffer for a frame of width x height X pixels: at least one point,
 * padded to whole points (WindowServer refuses empty ones), with the
 * scale X draws at.
 */
static ws_buffer_t *
nbBufferCreate(nbBuffer *buf, unsigned int width, unsigned int height,
               uint32_t format)
{
    unsigned int s = nbScreenInfo.scale;
    unsigned int w = nbPixelsToPointsUp((int) (width ? width : 1)) * s;
    unsigned int h = nbPixelsToPointsUp((int) (height ? height : 1)) * s;
    ws_buffer_t *buffer;

    buffer = ws_buffer_create_format(nbConnection(), w, h, s, format);
    if (buffer == NULL)
        ErrorF("Xnextbsd: can't make a %ux%u window buffer: %s\n",
               w, h, strerror(errno));
    buf->width = w;
    buf->height = h;
    return buffer;
}

/* The backing, at least 1x1 like the buffers. */
static Bool
nbBackingAlloc(nbFrame *frame, unsigned int width, unsigned int height)
{
    unsigned int stride = (width ? width : 1) * 4;

    frame->bits = calloc(height ? height : 1, stride);
    if (frame->bits == NULL) {
        ErrorF("Xnextbsd: can't allocate a %ux%u window backing\n",
               width, height);
        return FALSE;
    }
    frame->stride = stride;
    return TRUE;
}

/* Drop the WindowServer buffers (it keeps its own reference to the one
   it shows until another is attached). */
static void
nbBuffersDestroy(nbFrame *frame)
{
    int i;

    for (i = 0; i < NB_NBUFFERS; i++) {
        if (frame->buffers[i].ws != NULL && nbConnection() != NULL)
            ws_buffer_destroy(nbConnection(), frame->buffers[i].ws);
        frame->buffers[i].ws = NULL;
        frame->buffers[i].busy = FALSE;
        RegionEmpty(&frame->buffers[i].stale);
    }
}

/*
 * Copy a frame-local region of the backing into a buffer. In a shaped
 * frame only what is inside the bounding shape is copied, opaque if the
 * window has no alpha of its own, and the rest is made transparent.
 */
static void
nbBufferUpdate(nbFrame *frame, nbBuffer *buf, RegionPtr region)
{
    char *dst = ws_buffer_data(buf->ws);
    unsigned int dstride = ws_buffer_stride(buf->ws);
    WindowPtr pWin = nbFrameWindow(frame);
    Bool opaque = pWin == NULL || pWin->drawable.depth != 32;
    BoxRec all = { 0, 0, frame->width, frame->height };
    RegionRec in, out;
    const BoxRec *box;
    int i, n, x, y;
    size_t len;

    RegionInit(&in, &all, 1);
    RegionIntersect(&in, &in, region);
    RegionNull(&out);
    if (frame->shaped) {
        RegionSubtract(&out, &in, &frame->shape);
        RegionIntersect(&in, &in, &frame->shape);
    }

    box = RegionRects(&in);
    n = RegionNumRects(&in);
    for (i = 0; i < n; i++) {
        len = (size_t) (box[i].x2 - box[i].x1) * 4;
        for (y = box[i].y1; y < box[i].y2; y++) {
            char *d = dst + (size_t) y * dstride + box[i].x1 * 4;

            memcpy(d, frame->bits + (size_t) y * frame->stride +
                   box[i].x1 * 4, len);
            if (frame->shaped && opaque)
                for (x = 0; x < box[i].x2 - box[i].x1; x++)
                    ((uint32_t *) d)[x] |= 0xff000000u;
        }
    }
    box = RegionRects(&out);
    n = RegionNumRects(&out);
    for (i = 0; i < n; i++) {
        len = (size_t) (box[i].x2 - box[i].x1) * 4;
        for (y = box[i].y1; y < box[i].y2; y++)
            memset(dst + (size_t) y * dstride + box[i].x1 * 4, 0, len);
    }

    /*
     * A frame of an odd size on a 2x screen: the buffer has whole points,
     * up to scale - 1 pixels more than the frame. Repeat the last column
     * and row into them, so the window's edge shows there.
     */
    if (buf->width > frame->width || buf->height > frame->height) {
        RegionUnion(&in, &in, &out);
        box = RegionExtents(&in);
        if (RegionNotEmpty(&in) && frame->width > 0 && frame->height > 0) {
            if (buf->width > frame->width && box->x2 >= (int) frame->width)
                for (y = box->y1; y < box->y2; y++) {
                    uint32_t *row = (uint32_t *) (dst + (size_t) y * dstride);

                    for (x = frame->width; x < (int) buf->width; x++)
                        row[x] = row[frame->width - 1];
                }
            if (buf->height > frame->height &&
                box->y2 >= (int) frame->height)
                for (y = frame->height; y < (int) buf->height; y++)
                    memcpy(dst + (size_t) y * dstride,
                           dst + (size_t) (frame->height - 1) * dstride,
                           (size_t) buf->width * 4);
        }
    }
    RegionUninit(&in);
    RegionUninit(&out);
}

/*
 * A buffer WindowServer doesn't hold, made the first time it is needed
 * (all stale then), or NULL if both are held.
 */
static nbBuffer *
nbFrameFreeBuffer(nbFrame *frame)
{
    BoxRec all = { 0, 0, frame->width, frame->height };
    nbBuffer *buf;
    int i;

    for (i = 0; i < NB_NBUFFERS; i++) {
        buf = &frame->buffers[i];
        if (buf->busy)
            continue;
        if (buf->ws == NULL) {
            buf->ws = nbBufferCreate(buf, frame->width, frame->height,
                                     frame->format);
            if (buf->ws == NULL)
                continue;
            RegionReset(&buf->stale, &all);
        }
        return buf;
    }
    return NULL;
}

/*
 * Show the frame's damage: bring a free buffer up to date, attach it,
 * send the damage and commit, asking for WS_EVENT_FRAME_DONE. While the
 * last commit's frame isn't done, or WindowServer holds both buffers,
 * the damage waits (commit_wanted) and goes with the next commit.
 * With nothing damaged there is nothing to do.
 *
 * With `rect`, the window also takes that frame: in the same commit
 * (WS_SERVER_COMMIT_FRAME; the library falls back to setting it first),
 * or at once if the commit has to wait.
 */
static void
nbFrameCommitWithFrame(nbFrame *frame, const ws_rect_t *rect)
{
    ws_rect_t rects[NB_MAX_DAMAGE_RECTS];
    const BoxRec *box;
    nbBuffer *buf;
    int i, n;

    if (frame->wid == 0 || !RegionNotEmpty(&frame->damage) ||
        frame->frame_pending || (buf = nbFrameFreeBuffer(frame)) == NULL) {
        if (frame->wid != 0 && RegionNotEmpty(&frame->damage))
            frame->commit_wanted = TRUE;
        if (rect != NULL && frame->wid != 0 &&
            ws_window_set_frame(nbConnection(), frame->wid, rect) < 0)
            nbWindowServerLost("resize");
        return;
    }
    frame->commit_wanted = FALSE;

    /* Every buffer now lags the backing where it was drawn. */
    for (i = 0; i < NB_NBUFFERS; i++)
        if (frame->buffers[i].ws != NULL)
            RegionUnion(&frame->buffers[i].stale, &frame->buffers[i].stale,
                        &frame->damage);
    nbBufferUpdate(frame, buf, &buf->stale);
    RegionEmpty(&buf->stale);

    n = RegionNumRects(&frame->damage);
    box = RegionRects(&frame->damage);
    if (n > NB_MAX_DAMAGE_RECTS) {
        box = RegionExtents(&frame->damage);
        n = 1;
    }
    /* in points, rounded out */
    for (i = 0; i < n; i++) {
        int x1 = nbPixelsToPointsDown(box[i].x1);
        int y1 = nbPixelsToPointsDown(box[i].y1);

        rects[i] = nbRect(x1, y1, nbPixelsToPointsUp(box[i].x2) - x1,
                          nbPixelsToPointsUp(box[i].y2) - y1);
    }
    RegionEmpty(&frame->damage);

    if (ws_window_attach(nbConnection(), frame->wid, buf->ws) < 0 ||
        ws_window_damage(nbConnection(), frame->wid, rects, n) < 0 ||
        (rect != NULL ?
         ws_window_commit_frame(nbConnection(), frame->wid,
                                WS_COMMIT_FRAME_DONE, rect) :
         ws_window_commit(nbConnection(), frame->wid,
                          WS_COMMIT_FRAME_DONE)) < 0) {
        nbWindowServerLost("commit");
        return;
    }
    /* The other buffer, if it was attached, comes back with
       WS_EVENT_BUFFER_RELEASE. */
    buf->busy = TRUE;
    frame->frame_pending = TRUE;
}

static void
nbFrameCommit(nbFrame *frame)
{
    nbFrameCommitWithFrame(frame, NULL);
}

/* WindowServer showed the last commit: commit what accumulated since. */
void
nbFrameDone(nbFrame *frame)
{
    frame->frame_pending = FALSE;
    if (frame->commit_wanted)
        nbFrameCommit(frame);
}

/* WindowServer no longer reads this buffer: X may draw into it again. */
void
nbFrameBufferReleased(nbFrame *frame, uint32_t buffer_id)
{
    int i;

    for (i = 0; i < NB_NBUFFERS; i++) {
        if (frame->buffers[i].ws != NULL &&
            ws_buffer_id(frame->buffers[i].ws) == buffer_id) {
            frame->buffers[i].busy = FALSE;
            if (frame->commit_wanted && !frame->frame_pending)
                nbFrameCommit(frame);
            return;
        }
    }
}

static void
nbFrameDamageAll(nbFrame *frame)
{
    BoxRec box = { 0, 0, frame->width, frame->height };

    RegionReset(&frame->damage, &box);
}

/* ---- window manager properties ---- */

/* A property of a window, NULL if it has none (or isn't format `format`). */
static PropertyPtr
nbProperty(WindowPtr pWin, const char *name, int format)
{
    Atom atom = MakeAtom(name, strlen(name), FALSE);
    PropertyPtr prop;

    if (atom == None || pWin == NULL ||
        dixLookupProperty(&prop, pWin, atom, serverClient,
                          DixReadAccess) != Success ||
        prop->format != format)
        return NULL;
    return prop;
}

/*
 * The client window a top-level window shows: the first window below it,
 * breadth first, with WM_STATE (a window manager's frame holds it), or
 * the top-level window itself.
 */
static WindowPtr
nbFrameClient(WindowPtr top)
{
    WindowPtr queue[64];
    int head = 0, tail = 0;
    WindowPtr w, c;

    for (c = top->firstChild; c != NULL && tail < ARRAY_SIZE(queue);
         c = c->nextSib)
        queue[tail++] = c;
    while (head < tail) {
        w = queue[head++];
        if (nbProperty(w, "WM_STATE", 32) != NULL)
            return w;
        for (c = w->firstChild; c != NULL && tail < ARRAY_SIZE(queue);
             c = c->nextSib)
            queue[tail++] = c;
    }
    return top;
}

/* WM_NORMAL_HINTS (ICCCM 4.1.2.3): what the client's size may be. */
typedef struct nbSizeHints {
    int min_w, min_h;           /* at least 1 */
    int max_w, max_h;           /* 0: no maximum */
    int base_w, base_h;         /* sizes are base + n * inc */
    int inc_w, inc_h;           /* at least 1 */
    Bool has_min;               /* PMinSize or PBaseSize */
} nbSizeHints;

#define NB_PMinSize     (1L << 4)
#define NB_PMaxSize     (1L << 5)
#define NB_PResizeInc   (1L << 6)
#define NB_PBaseSize    (1L << 8)

static void
nbReadSizeHints(WindowPtr client, nbSizeHints *h)
{
    PropertyPtr prop = nbProperty(client, "WM_NORMAL_HINTS", 32);
    const CARD32 *v = prop != NULL ? prop->data : NULL;
    long flags = v != NULL && prop->size >= 15 ? (long) v[0] : 0;

    memset(h, 0, sizeof(*h));
    h->min_w = h->min_h = 1;
    h->inc_w = h->inc_h = 1;
    if ((flags & NB_PBaseSize) && prop->size >= 17) {
        h->base_w = max((INT32) v[15], 0);
        h->base_h = max((INT32) v[16], 0);
    }
    if (flags & NB_PMinSize) {
        h->min_w = max((INT32) v[5], 1);
        h->min_h = max((INT32) v[6], 1);
        if (!(flags & NB_PBaseSize)) {
            h->base_w = h->min_w;
            h->base_h = h->min_h;
        }
    } else if ((flags & NB_PBaseSize) && prop->size >= 17) {
        h->min_w = max(h->base_w, 1);
        h->min_h = max(h->base_h, 1);
    }
    h->has_min = (flags & (NB_PMinSize | NB_PBaseSize)) != 0;
    if (flags & NB_PMaxSize) {
        h->max_w = max((INT32) v[7], 0);
        h->max_h = max((INT32) v[8], 0);
        if (h->max_w > 0)
            h->max_w = max(h->max_w, h->min_w);
        if (h->max_h > 0)
            h->max_h = max(h->max_h, h->min_h);
    }
    if (flags & NB_PResizeInc) {
        h->inc_w = max((INT32) v[9], 1);
        h->inc_h = max((INT32) v[10], 1);
    }
}

/* The largest size the hints allow up to `size` (or the minimum). */
static int
nbConstrain(int size, int min, int max, int base, int inc)
{
    if (max > 0 && size > max)
        size = max;
    if (size < min)
        return min;
    if (inc > 1 && size > base) {
        size = base + (size - base) / inc * inc;
        if (size < min)
            size += inc;
    }
    return size;
}

/*
 * What a top-level window has around its client (a window manager's
 * title bar and borders), which resizing the frame leaves as it is: the
 * window manager's _NET_FRAME_EXTENTS on the client, or else the frame's
 * size minus the client's, borders included (which is off while the
 * window manager hasn't laid out a frame we just resized).
 */
typedef struct nbDecoration {
    WindowPtr client;
    int w, h;                   /* frame minus client, borders included */
    int bw;                     /* the client's border width */
} nbDecoration;

static void
nbFrameDecoration(WindowPtr top, nbDecoration *d)
{
    PropertyPtr prop;
    const CARD32 *v;

    d->client = nbFrameClient(top);
    d->bw = wBorderWidth(d->client);
    if (d->client == top) {
        d->w = d->h = 0;
        return;
    }
    prop = nbProperty(d->client, "_NET_FRAME_EXTENTS", 32);
    if (prop != NULL && prop->size >= 4) {
        v = prop->data;
        d->w = (int) min(v[0] + v[1], 4096u) + 2 * wBorderWidth(top);
        d->h = (int) min(v[2] + v[3], 4096u) + 2 * wBorderWidth(top);
        return;
    }
    d->w = max((int) top->drawable.width + 2 * wBorderWidth(top) -
               ((int) d->client->drawable.width + 2 * d->bw), 0);
    d->h = max((int) top->drawable.height + 2 * wBorderWidth(top) -
               ((int) d->client->drawable.height + 2 * d->bw), 0);
}

/*
 * The panel kinds of _NET_WM_WINDOW_TYPE: windows that float above the
 * tiles, as a GNUstep app's panels and menus do.
 */
static Bool
nbIsPanelType(Atom type)
{
    static const char *const panels[] = {
        "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
        "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_SPLASH",
        "_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
        "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP",
        "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_COMBO",
        "_NET_WM_WINDOW_TYPE_DND",
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(panels); i++)
        if (type == MakeAtom(panels[i], strlen(panels[i]), FALSE))
            return TRUE;
    return FALSE;
}

/*
 * What window tiling needs to know of a top-level window (servers with
 * WS_SERVER_TILING): WS_WINDOW_PANEL for override-redirect windows,
 * transient windows (WM_TRANSIENT_FOR) and dialogs, menus and the like
 * (_NET_WM_WINDOW_TYPE), and WS_WINDOW_FIXED_SIZE when the client's
 * minimum and maximum size are the same.
 */
static uint32_t
nbTilingFlags(WindowPtr pWin)
{
#ifdef WS_SERVER_TILING
    WindowPtr client;
    PropertyPtr prop;
    nbSizeHints h;

    if (!(nbScreenInfo.server_flags & WS_SERVER_TILING))
        return 0;
    if (pWin->overrideRedirect)
        return WS_WINDOW_PANEL;
    client = nbFrameClient(pWin);
    if ((prop = nbProperty(client, "WM_TRANSIENT_FOR", 32)) != NULL &&
        prop->size >= 1 && ((const CARD32 *) prop->data)[0] != None)
        return WS_WINDOW_PANEL;
    if ((prop = nbProperty(client, "_NET_WM_WINDOW_TYPE", 32)) != NULL &&
        prop->size >= 1 && nbIsPanelType(((const CARD32 *) prop->data)[0]))
        return WS_WINDOW_PANEL;
    nbReadSizeHints(client, &h);
    if (h.max_w > 0 && h.max_h > 0 && h.max_w == h.min_w &&
        h.max_h == h.min_h)
        return WS_WINDOW_FIXED_SIZE;
#endif
    return 0;
}

/*
 * Window levels, flags and pixel format. Override-redirect windows
 * (menus, tooltips, popups) float above normal windows, never become key
 * and don't activate us when clicked, like an NSMenu's window. Depth-24
 * windows have no alpha channel: their buffers are XRGB8888, which
 * WindowServer composites opaque, and depth-32 (ARGB visual) windows'
 * are premultiplied ARGB8888, as RENDER draws them.
 */
static void
nbFrameStyle(WindowPtr pWin, Bool shaped, int32_t *level, uint32_t *flags,
             uint32_t *format)
{
    *flags = 0;
    if (pWin->overrideRedirect) {
        *level = WS_LEVEL_POPUP_MENU;
        *flags |= WS_WINDOW_NO_KEY | WS_WINDOW_NO_ACTIVATE;
    } else {
        *level = WS_LEVEL_NORMAL;
    }
    if (pWin->drawable.depth == 32 || shaped) {
        /* A shaped window is transparent outside its bounding shape. */
        *format = WS_FORMAT_ARGB8888;
    } else if (nbScreenInfo.server_flags & WS_SERVER_XRGB) {
        *format = WS_FORMAT_XRGB8888;
    } else {
        /*
         * A WindowServer without XRGB8888 buffers: ARGB8888 with the
         * opaque flag, ROOTLESS_SAFEALPHA keeping the alpha bytes at 0xff.
         */
        *format = WS_FORMAT_ARGB8888;
        *flags |= WS_WINDOW_OPAQUE;
    }
    *flags |= nbTilingFlags(pWin);
}

/*
 * The frame's shape, from rootless's bounding shape (frame-local, NULL if
 * the window isn't shaped) and the window's input shape: the buffer shows
 * only the bounding shape, and WindowServer's input region is where both
 * are. Returns TRUE if WindowServer's input region needs sending.
 */
static Bool
nbFrameSetShape(nbFrame *frame, WindowPtr pWin, RegionPtr pShape)
{
    RegionPtr input = pWin != NULL ? wInputShape(pWin) : NULL;
    Bool was = frame->shaped;

    frame->shaped = pShape != NULL;
    if (pShape != NULL)
        RegionCopy(&frame->shape, pShape);
    else
        RegionEmpty(&frame->shape);
    return was || frame->shaped || input != NULL;
}

/* Tell WindowServer where the frame takes pointer events. */
static void
nbFrameSendInputRegion(nbFrame *frame, WindowPtr pWin)
{
    RegionPtr input = pWin != NULL ? wInputShape(pWin) : NULL;
    ws_rect_t *rects;
    RegionRec r;
    const BoxRec *box;
    int i, n;

    if (!(nbScreenInfo.server_flags & WS_SERVER_INPUT_REGION) ||
        frame->wid == 0)
        return;
    if (!frame->shaped && input == NULL) {
        if (ws_window_set_input_region(nbConnection(), frame->wid, NULL,
                                       0) < 0)
            nbWindowServerLost("input region");
        return;
    }
    RegionNull(&r);
    if (input != NULL) {
        /* Relative to the window's inside, like the bounding shape. */
        RegionCopy(&r, input);
        RegionTranslate(&r, pWin->borderWidth, pWin->borderWidth);
        if (frame->shaped)
            RegionIntersect(&r, &r, &frame->shape);
    } else {
        RegionCopy(&r, &frame->shape);
    }
    n = RegionNumRects(&r);
    box = RegionRects(&r);
    rects = calloc(n > 0 ? n : 1, sizeof(*rects));
    if (rects != NULL) {
        /* in points: where any pixel of a point takes events, all of it
           does */
        for (i = 0; i < n; i++) {
            int x1 = nbPixelsToPointsDown(box[i].x1);
            int y1 = nbPixelsToPointsDown(box[i].y1);

            rects[i] = nbRect(x1, y1, nbPixelsToPointsUp(box[i].x2) - x1,
                              nbPixelsToPointsUp(box[i].y2) - y1);
        }
        if (ws_window_set_input_region(nbConnection(), frame->wid, rects,
                                       n) < 0)
            nbWindowServerLost("input region");
        free(rects);
    }
    RegionUninit(&r);
}

static Bool
nbCreateFrame(RootlessWindowPtr pFrame, ScreenPtr pScreen, int newX,
              int newY, RegionPtr pShape)
{
    WindowPtr pWin = pFrame->win;
    nbFrame *frame;
    ws_rect_t r;
    char title[32];

    frame = calloc(1, sizeof(*frame));
    if (frame == NULL)
        return FALSE;
    frame->x = newX;
    frame->y = newY;
    frame->width = pFrame->width;
    frame->height = pFrame->height;
    frame->rwin = pFrame;
    RegionNull(&frame->damage);
    RegionNull(&frame->shape);
    for (int i = 0; i < NB_NBUFFERS; i++)
        RegionNull(&frame->buffers[i].stale);
    nbFrameSetShape(frame, pWin, pShape);
    nbFrameStyle(pWin, frame->shaped, &frame->level, &frame->flags,
                 &frame->format);

    r = nbFramePoints(newX, newY, frame->width, frame->height);
    snprintf(title, sizeof(title), "X11 0x%x", (unsigned) pWin->drawable.id);
    /* No round trip on a WindowServer with WS_SERVER_WINDOW_IDS. */
    frame->wid = ws_window_create_nowait(nbConnection(), &r, frame->level,
                                         frame->flags, title);
    if (frame->wid == 0) {
        ErrorF("Xnextbsd: can't create a WindowServer window: %s\n",
               strerror(errno));
        RegionUninit(&frame->damage);
        RegionUninit(&frame->shape);
        free(frame);
        return FALSE;
    }
    if (frame->shaped || wInputShape(pWin) != NULL)
        nbFrameSendInputRegion(frame, pWin);

    /* The first buffer is made and attached by the first commit. */
    if (!nbBackingAlloc(frame, frame->width, frame->height)) {
        ws_window_destroy(nbConnection(), frame->wid);
        RegionUninit(&frame->damage);
        RegionUninit(&frame->shape);
        free(frame);
        return FALSE;
    }

    xorg_list_add(&frame->link, &nbScreenInfo.frames);
    pFrame->wid = frame;
    /* Its size limits go to WindowServer before the server sleeps. */
    nbScreenInfo.frames_dirty = TRUE;
    return TRUE;
}

static void
nbForgetFrame(nbFrame *frame)
{
    if (nbScreenInfo.pointer_frame == frame)
        nbScreenInfo.pointer_frame = NULL;
    if (nbScreenInfo.key_frame == frame)
        nbScreenInfo.key_frame = NULL;
    if (nbScreenInfo.x_focus_frame == frame)
        nbScreenInfo.x_focus_frame = NULL;
}

static void
nbDestroyFrame(RootlessFrameID wid)
{
    nbFrame *frame = wid;

    nbForgetFrame(frame);
    xorg_list_del(&frame->link);
    if (frame->wid != 0 && nbConnection() != NULL)
        ws_window_destroy(nbConnection(), frame->wid);
    nbBuffersDestroy(frame);
    for (int i = 0; i < NB_NBUFFERS; i++)
        RegionUninit(&frame->buffers[i].stale);
    RegionUninit(&frame->damage);
    RegionUninit(&frame->shape);
    free(frame->bits);
    free(frame->title);
    free(frame);
}

static void
nbMoveFrame(RootlessFrameID wid, ScreenPtr pScreen, int newX, int newY)
{
    nbFrame *frame = wid;
    ws_rect_t r;

    /* Already there: X following a move WindowServer made, for one. */
    if (frame->x == newX && frame->y == newY)
        return;
    frame->x = newX;
    frame->y = newY;
    r = nbFramePoints(newX, newY, frame->width, frame->height);
    if (ws_window_set_frame(nbConnection(), frame->wid, &r) < 0)
        nbWindowServerLost("move");
}

/*
 * WindowServer gave a frame a new place or size itself
 * (WS_EVENT_WINDOW_FRAME): window tiling lays it out, or a screen that
 * shrank puts it back on the screen. X follows, so that its window is
 * where WindowServer shows it: the top-level window is moved and resized
 * as if its owner did it (the window manager, for a frame of its own), so
 * X clients and the window manager see the ConfigureNotify, and a window
 * manager then lays its frame out again around the client.
 *
 * The client's WM_NORMAL_HINTS have the last word on its size: its
 * minimum and maximum, and its resize increments (an xterm's character
 * cells), with what the frame has around the client added. When that
 * leaves X with another frame than WindowServer's (smaller than a tile,
 * or a move another client redirected), WindowServer gets X's: tiling
 * keeps a smaller window at its tile's top-left corner.
 *
 * WindowServer's frame is in points: X takes it times the scale, unless
 * it is the frame X's own already makes (an odd X size or place rounds
 * to it, nbPixelFrameToPoints), so that X's pixels stay as they are.
 */
void
nbFrameSetByWindowServer(nbFrame *frame, const ws_rect_t *pr)
{
    WindowPtr pWin = nbFrameWindow(frame);
    ws_rect_t had = nbFramePoints(frame->x, frame->y, frame->width,
                                  frame->height);
    ws_rect_t rr, *r = &rr;
    int s = (int) nbScreenInfo.scale;
    Bool moved, sized;
    ClientPtr owner;
    nbDecoration d;
    nbSizeHints h;
    XID vlist[4];
    Mask mask = CWX | CWY;
    int bw, cw, ch, w, hgt, x, y;
    ws_rect_t now;

    moved = pr->x != had.x || pr->y != had.y;
    sized = pr->width != had.width || pr->height != had.height;
    if (!moved && !sized)
        return;
    /* In X pixels; what didn't change in points stays as X has it. */
    rr.x = moved ? pr->x * s : frame->x;
    rr.y = moved ? pr->y * s : frame->y;
    rr.width = sized ? pr->width * s : (int) frame->width;
    rr.height = sized ? pr->height * s : (int) frame->height;
    frame->x = r->x;
    frame->y = r->y;
    if (pWin == NULL || pWin->parent == NULL)
        return;

    bw = wBorderWidth(pWin);
    vlist[0] = (XID) (INT16) (r->x - pWin->parent->drawable.x);
    vlist[1] = (XID) (INT16) (r->y - pWin->parent->drawable.y);
    if (sized && ((unsigned) r->width != frame->width ||
                  (unsigned) r->height != frame->height)) {
        nbFrameDecoration(pWin, &d);
        nbReadSizeHints(d.client, &h);
        cw = nbConstrain(r->width - d.w - 2 * d.bw, h.min_w, h.max_w,
                         h.base_w, h.inc_w);
        ch = nbConstrain(r->height - d.h - 2 * d.bw, h.min_h, h.max_h,
                         h.base_h, h.inc_h);
        w = cw + 2 * d.bw + d.w - 2 * bw;
        hgt = ch + 2 * d.bw + d.h - 2 * bw;
        if (w > 0 && hgt > 0 && (w != pWin->drawable.width ||
                                 hgt != pWin->drawable.height)) {
            vlist[2] = (XID) (CARD16) w;
            vlist[3] = (XID) (CARD16) hgt;
            mask |= CWWidth | CWHeight;
        }
    }
    owner = dixClientForWindow(pWin);
    ConfigureWindow(pWin, mask, vlist, owner ? owner : serverClient);

    /* Rootless resized the frame (nbResizeFrame) if X did; WindowServer
       gets X's frame if it still differs. */
    x = pWin->drawable.x - wBorderWidth(pWin);
    y = pWin->drawable.y - wBorderWidth(pWin);
    w = pWin->drawable.width + 2 * wBorderWidth(pWin);
    hgt = pWin->drawable.height + 2 * wBorderWidth(pWin);
    frame->x = x;
    frame->y = y;
    now = nbFramePoints(x, y, w, hgt);
    if (now.x != pr->x || now.y != pr->y || now.width != pr->width ||
        now.height != pr->height) {
        if (ws_window_set_frame(nbConnection(), frame->wid, &now) < 0)
            nbWindowServerLost("frame");
    }
}

/*
 * Copy what the old backing can keep into the new one, anchored as the
 * window's bit gravity says, so the window doesn't flash empty while X
 * repaints the rest.
 */
static void
nbCopyByGravity(const char *from, unsigned int fstride, unsigned int ow,
                unsigned int oh, char *to, unsigned int tstride,
                unsigned int nw, unsigned int nh, unsigned int gravity)
{
    unsigned int w = min(ow, nw), h = min(oh, nh), row;
    unsigned int sx = 0, sy = 0, dx = 0, dy = 0;
    const char *src;
    char *dst;

    switch (gravity) {
    case RL_GRAVITY_NORTH_WEST:
        break;
    case RL_GRAVITY_NORTH_EAST:
        sx = ow - w;
        dx = nw - w;
        break;
    case RL_GRAVITY_SOUTH_EAST:
        sx = ow - w;
        dx = nw - w;
        sy = oh - h;
        dy = nh - h;
        break;
    case RL_GRAVITY_SOUTH_WEST:
        sy = oh - h;
        dy = nh - h;
        break;
    default:
        return;
    }
    src = from + sy * fstride + sx * 4;
    dst = to + dy * tstride + dx * 4;
    for (row = 0; row < h; row++) {
        memcpy(dst, src, w * 4);
        src += fstride;
        dst += tstride;
    }
}

static void
nbResizeFrame(RootlessFrameID wid, ScreenPtr pScreen, int newX, int newY,
              unsigned int newW, unsigned int newH, unsigned int gravity)
{
    nbFrame *frame = wid;
    char *old = frame->bits;
    unsigned int ostride = frame->stride;
    ws_rect_t r;

    if (newW != frame->width || newH != frame->height) {
        if (!nbBackingAlloc(frame, newW, newH)) {
            frame->bits = old;  /* keep the old size; X redraws into it */
            return;
        }
        nbCopyByGravity(old, ostride, frame->width, frame->height,
                        frame->bits, frame->stride, newW, newH, gravity);
        free(old);
        /*
         * New buffers at the new size, made by the commit below.
         * WindowServer keeps showing the old one until then. The commit
         * goes now, whatever the frame pacing: the window must not show
         * its new size with old contents longer than it has to.
         */
        nbBuffersDestroy(frame);
        frame->frame_pending = FALSE;
    }

    frame->x = newX;
    frame->y = newY;
    frame->width = newW;
    frame->height = newH;
    /* The new frame goes with the first buffer at the new size. */
    r = nbFramePoints(newX, newY, newW, newH);
    /* What's around the client may have changed with it. */
    nbScreenInfo.frames_dirty = TRUE;
    RegionEmpty(&frame->damage);
    nbFrameDamageAll(frame);
    nbFrameCommitWithFrame(frame, &r);
}

/*
 * Restack: nextWid is the frame now directly above this one, or NULL if
 * this one is on top. Ordering also maps a frame that isn't shown yet.
 *
 * WindowServer keeps levels apart, and orders relative to a window only
 * within a level, so a frame whose X neighbour is on another level (a
 * normal window under a menu) goes to the top of its own level instead.
 *
 * WindowServer also raises a window itself when it is clicked; it says so
 * with WS_EVENT_WINDOW_ORDERED, and X follows (nbFrameOrdered). Pointer
 * events go through the frame WindowServer reports either way
 * (nbXYToWindow), so input is right even with a WindowServer that
 * doesn't send the notice.
 */
static void
nbRestackFrame(RootlessFrameID wid, RootlessFrameID nextWid)
{
    nbFrame *frame = wid, *next = nextWid;
    int ok;

    if (nbScreenInfo.hidden || frame->minimized) {
        /* Shown again, in X's stacking order, when XLibre.app unhides or
           the window is restored. */
        if (!frame->ordered_in) {
            frame->ordered_in = TRUE;
            nbFrameDamageAll(frame);
            nbFrameCommit(frame);
            nbScreenInfo.frames_dirty = TRUE;
        }
        return;
    }
    if (next != NULL && next->ordered_in && next->level == frame->level)
        ok = ws_window_order(nbConnection(), frame->wid, WS_ORDER_BELOW,
                             next->wid);
    else
        ok = ws_window_order(nbConnection(), frame->wid, WS_ORDER_ABOVE, 0);
    if (ok < 0) {
        nbWindowServerLost("order");
        return;
    }
    if (!frame->ordered_in) {
        frame->ordered_in = TRUE;
        /* First time on screen: show the whole buffer. It may have to
           be minimized right away (nbFramesUpdate). */
        nbFrameDamageAll(frame);
        nbFrameCommit(frame);
        nbScreenInfo.frames_dirty = TRUE;
    }
}

/*
 * X Shape: the bounding shape (pShape, frame-local, NULL if none) or the
 * input shape changed. The buffers become ARGB8888 if they weren't (a
 * shaped depth-24 window), WindowServer gets the new input region, and
 * the whole frame is shown again with the new shape.
 *
 * Rootless reshapes before it resizes: when the X window has a new size
 * the frame doesn't have yet, the resize's commit shows the new shape,
 * so nothing is committed here at the old size.
 */
static void
nbReshapeFrame(RootlessFrameID wid, RegionPtr pShape)
{
    nbFrame *frame = wid;
    WindowPtr pWin = nbFrameWindow(frame);
    int32_t level;
    uint32_t flags, format;
    Bool resizing;

    if (pWin == NULL)
        return;
    if (!nbFrameSetShape(frame, pWin, pShape))
        return;
    nbFrameStyle(pWin, frame->shaped, &level, &flags, &format);
    if (format != frame->format) {
        /* New buffers in the new format, made by the next commit. */
        nbBuffersDestroy(frame);
        frame->frame_pending = FALSE;
        frame->format = format;
    }
    if (flags != frame->flags) {
        frame->flags = flags;
        if (ws_window_set_flags(nbConnection(), frame->wid, flags) < 0)
            nbWindowServerLost("flags");
    }
    nbFrameSendInputRegion(frame, pWin);
    nbFrameDamageAll(frame);
    resizing = frame->width != pWin->drawable.width + 2 * wBorderWidth(pWin) ||
        frame->height != pWin->drawable.height + 2 * wBorderWidth(pWin);
    if (!resizing)
        nbFrameCommit(frame);
}

static void
nbUnmapFrame(RootlessFrameID wid)
{
    nbFrame *frame = wid;

    if (!frame->ordered_in)
        return;
    frame->ordered_in = FALSE;
    /* Ordering out also takes a minimized window out of the Dock. */
    frame->minimized = FALSE;
    if (ws_window_order(nbConnection(), frame->wid, WS_ORDER_OUT, 0) < 0)
        nbWindowServerLost("order out");
    if (nbScreenInfo.pointer_frame == frame)
        nbScreenInfo.pointer_frame = NULL;
}

static void
nbStartDrawing(RootlessFrameID wid, char **pixelData, int *bytesPerRow)
{
    nbFrame *frame = wid;

    *pixelData = frame->bits;
    *bytesPerRow = frame->stride;
}

static void
nbStopDrawing(RootlessFrameID wid, Bool flush)
{
    if (flush)
        nbFrameCommit(wid);
}

static void
nbUpdateRegion(RootlessFrameID wid, RegionPtr pDamage)
{
    nbFrame *frame = wid;

    if (pDamage != NULL)
        RegionUnion(&frame->damage, &frame->damage, pDamage);
    nbFrameCommit(frame);
}

static void
nbDamageRects(RootlessFrameID wid, int nrects, const BoxRec *rects,
              int shift_x, int shift_y)
{
    nbFrame *frame = wid;
    BoxRec clip = { 0, 0, frame->width, frame->height };
    RegionRec r;
    int i;

    for (i = 0; i < nrects; i++) {
        BoxRec b = {
            max(rects[i].x1 + shift_x, clip.x1),
            max(rects[i].y1 + shift_y, clip.y1),
            min(rects[i].x2 + shift_x, clip.x2),
            min(rects[i].y2 + shift_y, clip.y2),
        };

        if (b.x1 >= b.x2 || b.y1 >= b.y2)
            continue;
        RegionInit(&r, &b, 1);
        RegionUnion(&frame->damage, &frame->damage, &r);
        RegionUninit(&r);
    }
}

/*
 * The X window a frame shows changed (a window manager reparented the
 * client into its own frame window): the WindowServer window stays.
 * Its style may differ, so update it.
 */
static void
nbSwitchWindow(RootlessWindowPtr pFrame, WindowPtr oldWin)
{
    nbFrame *frame = pFrame->wid;
    int32_t level;
    uint32_t flags, format;

    frame->rwin = pFrame;
    /* The format stays: the buffer is the same. */
    nbFrameStyle(pFrame->win, frame->shaped, &level, &flags, &format);
    if (level != frame->level) {
        frame->level = level;
        ws_window_set_level(nbConnection(), frame->wid, level);
    }
    if (flags != frame->flags) {
        frame->flags = flags;
        ws_window_set_flags(nbConnection(), frame->wid, flags);
    }
    nbScreenInfo.frames_dirty = TRUE;
}

/*
 * WindowServer moved one of our frames in its stack by itself (it raises
 * a window when it is clicked): put the X window in the same place
 * relative to its X neighbours, so X's stacking order stays WindowServer's.
 * The window manager, if there is one, is asked like for any other
 * client's restack (it gets a ConfigureRequest); without one the X window
 * just moves. Rootless then orders the frame where it already is.
 */
void
nbFrameOrdered(nbFrame *frame, const ws_event_ordered_t *ordered)
{
    WindowPtr pWin = nbFrameWindow(frame), sib;
    nbFrame *other;
    XID vlist[2];

    if (pWin == NULL)
        return;
    if (ordered->op == WS_ORDER_OUT) {
        /* TODO: WindowServer hid it (nothing does yet); X keeps it mapped. */
        frame->ordered_in = FALSE;
        return;
    }
    if ((other = nbFrameFromWID(ordered->below)) != NULL &&
        (sib = nbFrameWindow(other)) != NULL && sib->parent == pWin->parent) {
        vlist[1] = Above;
    } else if ((other = nbFrameFromWID(ordered->above)) != NULL &&
               (sib = nbFrameWindow(other)) != NULL &&
               sib->parent == pWin->parent) {
        vlist[1] = Below;
    } else {
        return;
    }
    vlist[0] = sib->drawable.id;
    ConfigureWindow(pWin, CWSibling | CWStackMode, vlist, serverClient);
}

static void
nbHideWindow(RootlessFrameID wid)
{
    nbUnmapFrame(wid);
}

/*
 * Hide and show: XLibre.app was hidden (its Hide menu item, Command-H) or
 * shown again. Hidden, every frame is ordered out on WindowServer while
 * its X window stays mapped, so X clients see no change; frame->ordered_in
 * keeps saying which frames X has on screen. Shown, those go back in X's
 * stacking order, bottom first, each on top of its level. Minimized
 * windows stay in the Dock either way.
 */
static void
nbFrameSetHidden(Bool hide)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    WindowPtr pWin;
    RootlessWindowRec *winRec;
    nbFrame *frame;

    if (hide == nbScreenInfo.hidden)
        return;
    nbScreenInfo.hidden = hide;
    if (hide) {
        xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link)
            if (frame->ordered_in && !frame->minimized &&
                ws_window_order(nbConnection(), frame->wid, WS_ORDER_OUT,
                                0) < 0) {
                nbWindowServerLost("hide");
                return;
            }
        nbScreenInfo.pointer_frame = NULL;
        return;
    }
    if (pScreen == NULL || pScreen->root == NULL)
        return;
    for (pWin = pScreen->root->lastChild; pWin != NULL; pWin = pWin->prevSib) {
        winRec = WINREC(pWin);
        if (winRec == NULL || (frame = winRec->wid) == NULL ||
            !frame->ordered_in || frame->minimized)
            continue;
        if (ws_window_order(nbConnection(), frame->wid, WS_ORDER_ABOVE,
                            0) < 0) {
            nbWindowServerLost("show");
            return;
        }
        nbFrameDamageAll(frame);
        nbFrameCommit(frame);
    }
    /* Windows minimized while we were hidden go to the Dock now. */
    nbScreenInfo.frames_dirty = TRUE;
}

/* ---- minimizing and size limits ---- */

/*
 * Minimize or show the frame as the window manager's NB_MINIMIZED_ATOM on
 * its top-level window says. Only frames X has on screen are minimized:
 * WindowServer minimizes visible windows. On a WindowServer that can't
 * minimize, the window is just ordered out until it is restored.
 */
static void
nbFrameFollowMinimized(nbFrame *frame, WindowPtr pWin)
{
    PropertyPtr prop = nbProperty(pWin, NB_MINIMIZED_ATOM, 32);
    uint32_t where = prop != NULL && prop->size >= 1 ?
        ((const CARD32 *) prop->data)[0] : WS_MINIMIZE_DEFAULT;
    Bool want = prop != NULL;
    int ok;

    if (want == frame->minimized || !frame->ordered_in)
        return;
    if (want) {
        if (nbScreenInfo.hidden)
            return;             /* when XLibre.app is shown again */
        frame->minimized = TRUE;
        if (nbScreenInfo.server_flags & WS_SERVER_MINIMIZE)
            ok = ws_window_minimize_where(nbConnection(), frame->wid, where);
        else
            ok = ws_window_order(nbConnection(), frame->wid, WS_ORDER_OUT, 0);
        if (nbScreenInfo.pointer_frame == frame)
            nbScreenInfo.pointer_frame = NULL;
    } else {
        /* Ordering it in takes it out of the Dock. X's focus may still
           be in it: make it key again (nbFocusCheck). */
        frame->minimized = FALSE;
        if (nbScreenInfo.x_focus_frame == frame)
            nbScreenInfo.x_focus_frame = NULL;
        ok = ws_window_order(nbConnection(), frame->wid,
                             nbScreenInfo.hidden ? WS_ORDER_OUT :
                             WS_ORDER_ABOVE, 0);
        if (ok >= 0 && !nbScreenInfo.hidden) {
            nbFrameDamageAll(frame);
            nbFrameCommit(frame);
        }
    }
    if (ok < 0)
        nbWindowServerLost("minimize");
}

/*
 * The window's title on WindowServer, which the Dock shows for minimized
 * windows and in its window lists: the client's _NET_WM_NAME (UTF-8), or
 * its WM_NAME when that is ASCII.
 */
static void
nbFrameSendTitle(nbFrame *frame, WindowPtr pWin)
{
    WindowPtr client = nbFrameClient(pWin);
    PropertyPtr prop = nbProperty(client, "_NET_WM_NAME", 8);
    char title[256];            /* WindowServer's WS_MAX_TITLE */
    size_t len, i;

    if (prop == NULL) {
        prop = nbProperty(client, "WM_NAME", 8);
        for (i = 0; prop != NULL && i < (size_t) prop->size; i++)
            if (((const unsigned char *) prop->data)[i] >= 0x80)
                prop = NULL;
    }
    if (prop == NULL || prop->size == 0)
        return;
    len = min((size_t) prop->size, sizeof(title) - 1);
    memcpy(title, prop->data, len);
    title[len] = '\0';
    if (frame->title != NULL && strcmp(frame->title, title) == 0)
        return;
    free(frame->title);
    frame->title = strdup(title);
    if (ws_window_set_title(nbConnection(), frame->wid, title) < 0)
        nbWindowServerLost("title");
}

/*
 * Tell window tiling the smallest and largest frame the window takes:
 * the client's WM_NORMAL_HINTS with what is around it in the frame (the
 * window manager's title bar and borders).
 */
static void
nbFrameSendSizeLimits(nbFrame *frame, WindowPtr pWin)
{
#ifdef WS_SERVER_TILING
    nbDecoration d;
    nbSizeHints h;
    uint32_t limits[4];
    int extra_w, extra_h;

    if (!(nbScreenInfo.server_flags & WS_SERVER_TILING))
        return;
    nbFrameDecoration(pWin, &d);
    nbReadSizeHints(d.client, &h);
    extra_w = d.w + 2 * d.bw;
    extra_h = d.h + 2 * d.bw;
    /* in points, rounded up: X keeps the client within its hints */
    limits[0] = h.has_min ?
        (uint32_t) nbPixelsToPointsUp(h.min_w + extra_w) : 0;
    limits[1] = h.has_min ?
        (uint32_t) nbPixelsToPointsUp(h.min_h + extra_h) : 0;
    limits[2] = h.max_w > 0 ?
        (uint32_t) nbPixelsToPointsUp(h.max_w + extra_w) : 0;
    limits[3] = h.max_h > 0 ?
        (uint32_t) nbPixelsToPointsUp(h.max_h + extra_h) : 0;
    if (memcmp(limits, frame->limits, sizeof(limits)) == 0)
        return;
    memcpy(frame->limits, limits, sizeof(limits));
    if (ws_window_set_size_limits(nbConnection(), frame->wid, limits[0],
                                  limits[1], limits[2], limits[3]) < 0 &&
        errno != ENOTSUP)
        nbWindowServerLost("size limits");
#endif
}

/*
 * Before the server sleeps, after a window manager property changed (or
 * a frame came, went on screen, or changed size): bring every frame's
 * flags, title, size limits and minimized state up to date.
 */
void
nbFramesUpdate(void)
{
    nbFrame *frame;
    WindowPtr pWin;
    int32_t level;
    uint32_t flags, format;

    if (!nbScreenInfo.frames_dirty || nbConnection() == NULL)
        return;
    nbScreenInfo.frames_dirty = FALSE;
    xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link) {
        if ((pWin = nbFrameWindow(frame)) == NULL)
            continue;
        nbFrameStyle(pWin, frame->shaped, &level, &flags, &format);
        if (flags != frame->flags) {
            frame->flags = flags;
            if (ws_window_set_flags(nbConnection(), frame->wid, flags) < 0) {
                nbWindowServerLost("flags");
                return;
            }
        }
        nbFrameSendTitle(frame, pWin);
        nbFrameSendSizeLimits(frame, pWin);
        nbFrameFollowMinimized(frame, pWin);
        if (nbConnection() == NULL)
            return;
    }
}

/*
 * The scale X draws at changed (WS_EVENT_SCREEN_CHANGED with a new
 * backing scale, nbScreenChanged). X's windows keep their pixels; every
 * frame gets buffers at the new scale and its frame in the new points,
 * together in one commit, and its input region and size limits again.
 */
void
nbFramesRescale(void)
{
    nbFrame *frame;
    ws_rect_t r;

    if (nbConnection() == NULL)
        return;
    xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link) {
        if (frame->wid == 0)
            continue;
        nbBuffersDestroy(frame);
        frame->frame_pending = FALSE;
        memset(frame->limits, 0xff, sizeof(frame->limits));
        if (frame->shaped || (nbFrameWindow(frame) != NULL &&
                              wInputShape(nbFrameWindow(frame)) != NULL))
            nbFrameSendInputRegion(frame, nbFrameWindow(frame));
        r = nbFramePoints(frame->x, frame->y, frame->width, frame->height);
        nbFrameDamageAll(frame);
        nbFrameCommitWithFrame(frame, &r);
        if (nbConnection() == NULL)
            return;
    }
    nbScreenInfo.frames_dirty = TRUE;
}

/*
 * The user restored a minimized window from WindowServer (the Dock, its
 * tile, the overview; WS_EVENT_WINDOW_RESTORE), which has activated us.
 * Show it, drop NB_MINIMIZED_ATOM, and tell the window manager, which
 * puts WM_STATE back to NormalState, raises the window and focuses it.
 */
void
nbFrameRestoreRequested(nbFrame *frame)
{
    WindowPtr pWin = nbFrameWindow(frame);
    ScreenPtr pScreen = screenInfo.screens[0];
    Atom minimized = MakeAtom(NB_MINIMIZED_ATOM, strlen(NB_MINIMIZED_ATOM),
                              FALSE);
    xEvent ev;

    if (pWin == NULL || !frame->minimized)
        return;
    if (minimized != None)
        DeleteProperty(serverClient, pWin, minimized);
    nbFrameFollowMinimized(frame, pWin);
    if (pScreen == NULL || pScreen->root == NULL)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.u.u.type = ClientMessage;
    ev.u.u.detail = 32;
    ev.u.clientMessage.window = pWin->drawable.id;
    ev.u.clientMessage.u.l.type = MakeAtom(NB_RESTORE_ATOM,
                                           strlen(NB_RESTORE_ATOM), TRUE);
    ev.u.clientMessage.u.l.longs0 = pWin->drawable.id;
    DeliverEventsToWindow(inputInfo.pointer, pScreen->root, &ev, 1,
                          SubstructureRedirectMask, NullGrab);
}

/*
 * Watch ChangeProperty and DeleteProperty of NB_HIDDEN_ATOM on the root
 * window, which is how XLibre.app's window manager says hide and show,
 * and of the properties that make a frame's style, size limits and
 * minimized state, which are looked at again before the server sleeps
 * (the filter runs before the change). The request itself goes ahead,
 * so the property shows the state.
 */
static void
nbPropertyFilter(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    static const char *const watched[] = {
        NB_MINIMIZED_ATOM, "WM_NORMAL_HINTS", "WM_TRANSIENT_FOR",
        "_NET_WM_WINDOW_TYPE", "WM_STATE", "WM_NAME", "_NET_WM_NAME",
        "_NET_FRAME_EXTENTS",
    };
    PropertyFilterParam *p = calldata;
    ScreenPtr pScreen = screenInfo.screens[0];
    Atom hidden;
    size_t i;

    if (p->skip || pScreen == NULL || pScreen->root == NULL ||
        p->atoms != NULL ||
        (p->access_mode != DixWriteAccess &&
         p->access_mode != DixRemoveAccess))
        return;
    if (p->window != pScreen->root->drawable.id) {
        for (i = 0; i < ARRAY_SIZE(watched); i++)
            if (p->property == MakeAtom(watched[i], strlen(watched[i]),
                                        FALSE))
                nbScreenInfo.frames_dirty = TRUE;
        return;
    }
    hidden = MakeAtom(NB_HIDDEN_ATOM, strlen(NB_HIDDEN_ATOM), FALSE);
    if (hidden == None || p->property != hidden)
        return;
    if (p->access_mode == DixRemoveAccess) {
        nbFrameSetHidden(FALSE);
    } else if (p->format == 32 && p->len >= 1 && p->value != NULL &&
               p->mode == PropModeReplace) {
        nbFrameSetHidden(((const CARD32 *) p->value)[0] != 0);
    }
}

static RootlessFrameProcsRec nbRootlessProcs = {
    .CreateFrame = nbCreateFrame,
    .DestroyFrame = nbDestroyFrame,
    .MoveFrame = nbMoveFrame,
    .ResizeFrame = nbResizeFrame,
    .RestackFrame = nbRestackFrame,
    .ReshapeFrame = nbReshapeFrame,
    .UnmapFrame = nbUnmapFrame,
    .StartDrawing = nbStartDrawing,
    .StopDrawing = nbStopDrawing,
    .UpdateRegion = nbUpdateRegion,
    .DamageRects = nbDamageRects,
    .SwitchWindow = nbSwitchWindow,
    .HideWindow = nbHideWindow,
};

Bool
nbFrameInit(ScreenPtr pScreen)
{
    rootlessGlobalOffsetX = 0;
    rootlessGlobalOffsetY = 0;
    nbScreenInfo.hidden = FALSE;
    if (!AddCallback(&PropertyFilterCallback, nbPropertyFilter, NULL))
        return FALSE;
    return RootlessInit(pScreen, &nbRootlessProcs);
}
