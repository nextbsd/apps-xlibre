/*
 * Xnextbsd: X cursors as WindowServer custom cursors.
 *
 * Copyright (c) 2026, Joseph Maloney
 * The cursor image conversion follows hw/xquartz/xpr/xprCursor.c:
 * Copyright (c) 2001-2012 Apple Inc. All rights reserved.
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
 * WindowServer draws the cursor; X only says which image. Each X cursor is
 * converted once to premultiplied ARGB8888 in a WindowServer buffer, kept
 * until X frees the cursor. There is no software cursor in the X server.
 *
 * With a WindowServer that has a cursor per window
 * (WS_SERVER_WINDOW_CURSOR), the cursor X shows goes to the frame the
 * pointer is in, so WindowServer shows it the moment the pointer enters
 * that frame again, before X has seen the motion; over no frame of ours
 * (during a grab) it is our connection's cursor. With an older one, it
 * is always the connection's cursor, sent again whenever it changes.
 *
 * Warps (XWarpPointer) move WindowServer's pointer too, which it allows
 * while we are the active app and the pointer is over one of our frames
 * or grabbed by us (WS_SERVER_POINTER_WARP).
 *
 * HiDPI: hot spots are points on WindowServer. RENDER (ARGB) cursors are
 * the ones X programs load from cursor themes at Xcursor.size, which
 * XLibre.app makes 24 times the scale, so their buffers carry the scale
 * X draws at and show pixel for pixel. Their image is moved right and
 * down by up to scale - 1 pixels so that the hot spot is on a whole
 * point. Core (bitmap) cursors are made for 1x screens: their buffers
 * are 1x, and WindowServer scales them up to the size they have on one.
 */

#include <dix-config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dix/cursor_priv.h"
#include "mi/mipointer_priv.h"

#include "scrnintstr.h"
#include "cursorstr.h"
#include "inputstr.h"
#include "fb.h"
#include "mipointer.h"
#include "servermd.h"

#include "nextbsd.h"

/* An X cursor's image in a WindowServer buffer. */
typedef struct nbCursorBuf {
    struct xorg_list link;
    CursorPtr cursor;
    ws_buffer_t *buffer;
    int hot_x, hot_y;           /* in points */
} nbCursorBuf;

static struct xorg_list nbCursorImages;
static Bool nbCursorImagesInit;
static CursorPtr nbCursorShown;         /* our connection's cursor */
static Bool nbCursorHidden;

/* Premultiply a non-premultiplied colour by alpha 0xff or 0x00. */
static uint32_t
nbCursorPixel(unsigned short r, unsigned short g, unsigned short b)
{
    return 0xff000000u | (uint32_t) (r >> 8) << 16 |
        (uint32_t) (g >> 8) << 8 | (uint32_t) (b >> 8);
}

/* Fill a WindowServer buffer with the cursor's image, at dx, dy. */
static void
nbCursorImage(CursorPtr pCursor, ws_buffer_t *buffer, int dx, int dy)
{
    CursorBitsPtr bits = pCursor->bits;
    uint32_t stride = ws_buffer_stride(buffer) / 4;
    uint32_t *dst = (uint32_t *) ws_buffer_data(buffer) + dy * stride + dx;
    uint32_t fg, bg;
    int x, y;

    if (bits->argb != NULL) {
        /* RENDER cursors are premultiplied ARGB already. */
        for (y = 0; y < bits->height; y++)
            memcpy(dst + y * stride, bits->argb + y * bits->width,
                   bits->width * 4);
        return;
    }

    /* A core cursor: source and mask bitmaps, two colours. */
    fg = nbCursorPixel(pCursor->foreRed, pCursor->foreGreen,
                       pCursor->foreBlue);
    bg = nbCursorPixel(pCursor->backRed, pCursor->backGreen,
                       pCursor->backBlue);
    for (y = 0; y < bits->height; y++) {
        const unsigned char *s = bits->source + y * BitmapBytePad(bits->width);
        const unsigned char *m = bits->mask + y * BitmapBytePad(bits->width);

        for (x = 0; x < bits->width; x++) {
#if BITMAP_BIT_ORDER == MSBFirst
            int bit = 0x80 >> (x & 7);
#else
            int bit = 1 << (x & 7);
#endif
            if (bits->emptyMask || !(m[x / 8] & bit))
                dst[y * stride + x] = 0;
            else
                dst[y * stride + x] = (s[x / 8] & bit) ? fg : bg;
        }
    }
}

/*
 * The buffer holding pCursor's image, made the first time it is shown,
 * and its hot spot in points.
 */
static nbCursorBuf *
nbCursorBuffer(CursorPtr pCursor)
{
    CursorBitsPtr bits = pCursor->bits;
    nbCursorBuf *ci;
    int s, dx, dy, w, h;

    xorg_list_for_each_entry(ci, &nbCursorImages, link)
        if (ci->cursor == pCursor)
            return ci;
    ci = calloc(1, sizeof(*ci));
    if (ci == NULL)
        return NULL;
    s = bits->argb != NULL ? (int) nbScreenInfo.scale : 1;
    /* the hot spot on a whole point, the size whole points */
    dx = (s - bits->xhot % s) % s;
    dy = (s - bits->yhot % s) % s;
    w = (bits->width + dx + s - 1) / s * s;
    h = (bits->height + dy + s - 1) / s * s;
    ci->buffer = ws_buffer_create(nbConnection(), w ? w : s, h ? h : s, s);
    if (ci->buffer == NULL) {
        ErrorF("Xnextbsd: can't make a cursor buffer: %s\n",
               strerror(errno));
        free(ci);
        return NULL;
    }
    nbCursorImage(pCursor, ci->buffer, dx, dy);
    ci->hot_x = (bits->xhot + dx) / s;
    ci->hot_y = (bits->yhot + dy) / s;
    ci->cursor = pCursor;
    xorg_list_add(&ci->link, &nbCursorImages);
    return ci;
}

/* Drop a cursor's buffer (WindowServer keeps its own references). */
static void
nbCursorForget(nbCursorBuf *ci)
{
    xorg_list_del(&ci->link);
    if (nbConnection() != NULL)
        ws_buffer_destroy(nbConnection(), ci->buffer);
    free(ci);
}

/* The frame showing the window the sprite is in, or NULL (the root). */
static nbFrame *
nbCursorFrame(DeviceIntPtr pDev)
{
    SpritePtr sprite;

    if (pDev == NULL || pDev->spriteInfo == NULL ||
        (sprite = pDev->spriteInfo->sprite) == NULL)
        return NULL;
    return nbFrameOfWindow(sprite->win);
}

static void
nbCursorShow(DeviceIntPtr pDev, CursorPtr pCursor)
{
    nbCursorBuf *ci;
    nbFrame *frame = NULL;
    int r;

    if (nbConnection() == NULL)
        return;

    if (pCursor == NULL) {
        if (!nbCursorHidden) {
            ws_set_cursor_visible(nbConnection(), 0);
            nbCursorHidden = TRUE;
        }
        return;
    }
    if (nbCursorHidden) {
        ws_set_cursor_visible(nbConnection(), 1);
        nbCursorHidden = FALSE;
    }
    if (nbScreenInfo.server_flags & WS_SERVER_WINDOW_CURSOR)
        frame = nbCursorFrame(pDev);
    if (frame != NULL ? frame->cursor == pCursor : pCursor == nbCursorShown)
        return;

    ci = nbCursorBuffer(pCursor);
    if (ci == NULL)
        return;
    if (frame != NULL)
        r = ws_window_set_cursor(nbConnection(), frame->wid,
                                 WS_CURSOR_CUSTOM, ci->buffer,
                                 ci->hot_x, ci->hot_y);
    else
        r = ws_set_cursor(nbConnection(), WS_CURSOR_CUSTOM, ci->buffer,
                          ci->hot_x, ci->hot_y);
    if (r < 0) {
        nbWindowServerLost("cursor");
        return;
    }
    if (frame != NULL)
        frame->cursor = pCursor;
    else
        nbCursorShown = pCursor;
}


/* ---- sprite functions ---- */

static Bool
nbRealizeCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor)
{
    return TRUE;
}

static Bool
nbUnrealizeCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor)
{
    nbCursorBuf *ci, *tmp;
    nbFrame *frame;

    /* The address may be reused by the next cursor. */
    if (pCursor == nbCursorShown)
        nbCursorShown = NULL;
    xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link)
        if (frame->cursor == pCursor)
            frame->cursor = NULL;
    xorg_list_for_each_entry_safe(ci, tmp, &nbCursorImages, link)
        if (ci->cursor == pCursor)
            nbCursorForget(ci);
    return TRUE;
}

static void
nbSetCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor,
            int x, int y)
{
    nbCursorShow(pDev, pCursor);
}

static void
nbMoveCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    /* WindowServer moves the cursor. */
}

static Bool
nbDeviceCursorInitialize(DeviceIntPtr pDev, ScreenPtr pScreen)
{
    return TRUE;
}

static void
nbDeviceCursorCleanup(DeviceIntPtr pDev, ScreenPtr pScreen)
{
}

static miPointerSpriteFuncRec nbSpriteFuncs = {
    .RealizeCursor = nbRealizeCursor,
    .UnrealizeCursor = nbUnrealizeCursor,
    .SetCursor = nbSetCursor,
    .MoveCursor = nbMoveCursor,
    .DeviceCursorInitialize = nbDeviceCursorInitialize,
    .DeviceCursorCleanup = nbDeviceCursorCleanup,
};

/* ---- screen functions ---- */

static Bool
nbCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    return FALSE;
}

static void
nbCrossScreen(ScreenPtr pScreen, Bool entering)
{
}

/*
 * XWarpPointer: X's idea of the position moves, and WindowServer's pointer
 * with it when it allows (see the top of the file): only asked when it
 * would, so X's own warps at startup don't end in errors. Otherwise the
 * next motion puts X back where the pointer really is.
 */
static void
nbWarpCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    miPointerWarpCursor(pDev, pScreen, x, y);
    if (nbConnection() == NULL ||
        !(nbScreenInfo.server_flags & WS_SERVER_POINTER_WARP) ||
        !nbScreenInfo.active ||
        (nbScreenInfo.pointer_frame == NULL && !nbScreenInfo.ws_grab))
        return;
    /* X's pixels, in WindowServer's points */
    if (ws_warp_pointer(nbConnection(), 0, (double) x / nbScreenInfo.scale,
                        (double) y / nbScreenInfo.scale) < 0)
        nbWindowServerLost("warp");
}

static miPointerScreenFuncRec nbPointerScreenFuncs = {
    .CursorOffScreen = nbCursorOffScreen,
    .CrossScreen = nbCrossScreen,
    .WarpCursor = nbWarpCursor,
};

static void
nbCursorQueryBestSize(int class, unsigned short *width,
                      unsigned short *height, ScreenPtr pScreen)
{
    if (class == CursorShape) {
        *width = 64 * nbScreenInfo.scale;
        *height = 64 * nbScreenInfo.scale;
    } else {
        fbQueryBestSize(class, width, height, pScreen);
    }
}

/*
 * The scale X draws at changed: the cursors' buffers are made again at
 * it when next shown, and the one shown now is sent again.
 */
void
nbCursorRescale(void)
{
    nbCursorBuf *ci, *tmp;
    nbFrame *frame;
    DeviceIntPtr pDev = inputInfo.pointer;
    CursorPtr shown = nbCursorShown;

    xorg_list_for_each_entry_safe(ci, tmp, &nbCursorImages, link)
        nbCursorForget(ci);
    nbCursorShown = NULL;
    xorg_list_for_each_entry(frame, &nbScreenInfo.frames, link)
        frame->cursor = NULL;
    if (pDev != NULL && pDev->spriteInfo != NULL &&
        pDev->spriteInfo->sprite != NULL &&
        pDev->spriteInfo->sprite->current != NULL)
        shown = pDev->spriteInfo->sprite->current;
    if (shown != NULL && !nbCursorHidden)
        nbCursorShow(pDev, shown);
}

Bool
nbCursorInit(ScreenPtr pScreen)
{
    nbCursorBuf *ci, *tmp;

    /* A new server generation: the old cursors' buffers aren't needed. */
    if (!nbCursorImagesInit) {
        xorg_list_init(&nbCursorImages);
        nbCursorImagesInit = TRUE;
    }
    xorg_list_for_each_entry_safe(ci, tmp, &nbCursorImages, link)
        nbCursorForget(ci);
    nbCursorShown = NULL;
    nbCursorHidden = FALSE;
    pScreen->QueryBestSize = nbCursorQueryBestSize;
    return miPointerInitialize(pScreen, &nbSpriteFuncs, &nbPointerScreenFuncs,
                               FALSE);
}
