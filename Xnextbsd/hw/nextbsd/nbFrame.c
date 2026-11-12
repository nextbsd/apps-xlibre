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
 */

#include <dix-config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <X11/X.h>

#include "dix/dix_priv.h"
#include "dix/property_priv.h"
#include "dix/resource_priv.h"
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

/* A buffer of at least 1x1 (WindowServer refuses empty ones). */
static ws_buffer_t *
nbBufferCreate(unsigned int width, unsigned int height, uint32_t format)
{
    ws_buffer_t *buffer;

    buffer = ws_buffer_create_format(nbConnection(), width ? width : 1,
                                     height ? height : 1, 1, format);
    if (buffer == NULL)
        ErrorF("Xnextbsd: can't make a %ux%u window buffer: %s\n",
               width, height, strerror(errno));
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

/* Copy a frame-local region of the backing into a buffer. */
static void
nbBufferUpdate(nbFrame *frame, nbBuffer *buf, RegionPtr region)
{
    char *dst = ws_buffer_data(buf->ws);
    unsigned int dstride = ws_buffer_stride(buf->ws);
    const BoxRec *box = RegionRects(region);
    int i, n = RegionNumRects(region), y;
    size_t len;

    for (i = 0; i < n; i++) {
        BoxRec b = {
            max(box[i].x1, 0), max(box[i].y1, 0),
            min(box[i].x2, (int) frame->width),
            min(box[i].y2, (int) frame->height),
        };

        if (b.x1 >= b.x2 || b.y1 >= b.y2)
            continue;
        len = (size_t) (b.x2 - b.x1) * 4;
        for (y = b.y1; y < b.y2; y++)
            memcpy(dst + (size_t) y * dstride + b.x1 * 4,
                   frame->bits + (size_t) y * frame->stride + b.x1 * 4, len);
    }
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
            buf->ws = nbBufferCreate(frame->width, frame->height,
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
 */
static void
nbFrameCommit(nbFrame *frame)
{
    ws_rect_t rects[NB_MAX_DAMAGE_RECTS];
    const BoxRec *box;
    nbBuffer *buf;
    int i, n;

    if (frame->wid == 0 || !RegionNotEmpty(&frame->damage))
        return;
    if (frame->frame_pending || (buf = nbFrameFreeBuffer(frame)) == NULL) {
        frame->commit_wanted = TRUE;
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
    for (i = 0; i < n; i++)
        rects[i] = nbRect(box[i].x1, box[i].y1, box[i].x2 - box[i].x1,
                          box[i].y2 - box[i].y1);
    RegionEmpty(&frame->damage);

    if (ws_window_attach(nbConnection(), frame->wid, buf->ws) < 0 ||
        ws_window_damage(nbConnection(), frame->wid, rects, n) < 0 ||
        ws_window_commit(nbConnection(), frame->wid,
                         WS_COMMIT_FRAME_DONE) < 0) {
        nbWindowServerLost("commit");
        return;
    }
    /* The other buffer, if it was attached, comes back with
       WS_EVENT_BUFFER_RELEASE. */
    buf->busy = TRUE;
    frame->frame_pending = TRUE;
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

/*
 * Window levels, flags and pixel format. Override-redirect windows
 * (menus, tooltips, popups) float above normal windows, never become key
 * and don't activate us when clicked, like an NSMenu's window. Depth-24
 * windows have no alpha channel: their buffers are XRGB8888, which
 * WindowServer composites opaque, and depth-32 (ARGB visual) windows'
 * are premultiplied ARGB8888, as RENDER draws them.
 */
static void
nbFrameStyle(WindowPtr pWin, int32_t *level, uint32_t *flags,
             uint32_t *format)
{
    *flags = 0;
    if (pWin->overrideRedirect) {
        *level = WS_LEVEL_POPUP_MENU;
        *flags |= WS_WINDOW_NO_KEY | WS_WINDOW_NO_ACTIVATE;
    } else {
        *level = WS_LEVEL_NORMAL;
    }
    if (pWin->drawable.depth == 32) {
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
    for (int i = 0; i < NB_NBUFFERS; i++)
        RegionNull(&frame->buffers[i].stale);
    nbFrameStyle(pWin, &frame->level, &frame->flags, &frame->format);

    /* TODO: shaped windows (pShape) need input regions in WindowServer. */
    r = nbRect(newX, newY, frame->width, frame->height);
    snprintf(title, sizeof(title), "X11 0x%x", (unsigned) pWin->drawable.id);
    frame->wid = ws_window_create(nbConnection(), &r, frame->level,
                                  frame->flags, title);
    if (frame->wid == 0) {
        ErrorF("Xnextbsd: can't create a WindowServer window: %s\n",
               strerror(errno));
        RegionUninit(&frame->damage);
        free(frame);
        return FALSE;
    }

    /* The first buffer is made and attached by the first commit. */
    if (!nbBackingAlloc(frame, frame->width, frame->height)) {
        ws_window_destroy(nbConnection(), frame->wid);
        RegionUninit(&frame->damage);
        free(frame);
        return FALSE;
    }

    xorg_list_add(&frame->link, &nbScreenInfo.frames);
    pFrame->wid = frame;
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
    free(frame->bits);
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
    r = nbRect(newX, newY, frame->width, frame->height);
    if (ws_window_set_frame(nbConnection(), frame->wid, &r) < 0)
        nbWindowServerLost("move");
}

/*
 * WindowServer moved a frame itself (WS_EVENT_WINDOW_FRAME): after a
 * screen change, it puts the windows that no longer fit back on the
 * screen. X follows, so that its window is where WindowServer shows it:
 * the top-level window moves as if its owner had moved it (the window
 * manager, for a frame of its own), and X clients and the window manager
 * see the ConfigureNotify. Should another client redirect the move (a
 * window manager, for a window that isn't its own), WindowServer is put
 * back where X still has the window until that client decides.
 */
void
nbFrameMovedByWindowServer(nbFrame *frame, const ws_rect_t *r)
{
    WindowPtr pWin = nbFrameWindow(frame);
    ClientPtr owner;
    XID vlist[2];
    int x, y;

    if (r->x == frame->x && r->y == frame->y)
        return;
    frame->x = r->x;
    frame->y = r->y;
    if (pWin == NULL || pWin->parent == NULL)
        return;

    vlist[0] = (XID) (INT16) (r->x - pWin->parent->drawable.x);
    vlist[1] = (XID) (INT16) (r->y - pWin->parent->drawable.y);
    owner = dixClientForWindow(pWin);
    ConfigureWindow(pWin, CWX | CWY, vlist, owner ? owner : serverClient);

    x = pWin->drawable.x - wBorderWidth(pWin);
    y = pWin->drawable.y - wBorderWidth(pWin);
    if (x != r->x || y != r->y)
        nbMoveFrame(frame, pWin->drawable.pScreen, x, y);
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
    /*
     * TODO: frame and buffer in one commit, so a resize can't show the new
     * frame with the old buffer for a moment (a later WindowServer item).
     */
    r = nbRect(newX, newY, newW, newH);
    if (ws_window_set_frame(nbConnection(), frame->wid, &r) < 0)
        nbWindowServerLost("resize");
    RegionEmpty(&frame->damage);
    nbFrameDamageAll(frame);
    nbFrameCommit(frame);
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

    if (nbScreenInfo.hidden) {
        /* Shown again, in X's stacking order, when XLibre.app unhides. */
        if (!frame->ordered_in) {
            frame->ordered_in = TRUE;
            nbFrameDamageAll(frame);
            nbFrameCommit(frame);
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
        /* First time on screen: show the whole buffer. */
        nbFrameDamageAll(frame);
        nbFrameCommit(frame);
    }
}

static void
nbReshapeFrame(RootlessFrameID wid, RegionPtr pShape)
{
    /*
     * TODO: shaped windows. WindowServer has no input regions yet; the
     * frame stays rectangular, and rootless leaves the area outside the
     * shape transparent in the buffer only for depth-32 windows.
     */
}

static void
nbUnmapFrame(RootlessFrameID wid)
{
    nbFrame *frame = wid;

    if (!frame->ordered_in)
        return;
    frame->ordered_in = FALSE;
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
    nbFrameStyle(pFrame->win, &level, &flags, &format);
    if (level != frame->level) {
        frame->level = level;
        ws_window_set_level(nbConnection(), frame->wid, level);
    }
    if (flags != frame->flags) {
        frame->flags = flags;
        ws_window_set_flags(nbConnection(), frame->wid, flags);
    }
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
 * stacking order, bottom first, each on top of its level.
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
            if (frame->ordered_in &&
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
            !frame->ordered_in)
            continue;
        if (ws_window_order(nbConnection(), frame->wid, WS_ORDER_ABOVE,
                            0) < 0) {
            nbWindowServerLost("show");
            return;
        }
        nbFrameDamageAll(frame);
        nbFrameCommit(frame);
    }
}

/*
 * Watch ChangeProperty and DeleteProperty of NB_HIDDEN_ATOM on the root
 * window, which is how XLibre.app's window manager says hide and show.
 * The request itself goes ahead, so the property shows the state.
 */
static void
nbPropertyFilter(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    PropertyFilterParam *p = calldata;
    ScreenPtr pScreen = screenInfo.screens[0];
    Atom hidden;

    if (p->skip || pScreen == NULL || pScreen->root == NULL ||
        p->window != pScreen->root->drawable.id || p->atoms != NULL)
        return;
    hidden = MakeAtom(NB_HIDDEN_ATOM, strlen(NB_HIDDEN_ATOM), FALSE);
    if (hidden == None || p->property != hidden)
        return;
    if (p->access_mode == DixRemoveAccess) {
        nbFrameSetHidden(FALSE);
    } else if (p->access_mode == DixWriteAccess && p->format == 32 &&
               p->len >= 1 && p->value != NULL &&
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
