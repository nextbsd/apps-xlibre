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
 * WindowServer draws the cursor; X only says which image. Each time X
 * changes the cursor, its image is converted to premultiplied ARGB8888 in
 * a new WindowServer buffer and set as our app's custom cursor, which
 * WindowServer shows while the pointer is over one of our windows. There
 * is no software cursor in the X server.
 *
 * TODO: a cursor per window (a later WindowServer item), so the cursor
 * is right the moment the pointer enters a window, before X has seen the
 * motion.
 */

#include <dix-config.h>

#include <errno.h>
#include <string.h>

#include "dix/cursor_priv.h"
#include "mi/mipointer_priv.h"

#include "scrnintstr.h"
#include "cursorstr.h"
#include "fb.h"
#include "mipointer.h"
#include "servermd.h"

#include "nextbsd.h"

static ws_buffer_t *nbCursorBuffer;     /* the cursor WindowServer shows */
static CursorPtr nbCursorShown;         /* and the X cursor it came from */
static Bool nbCursorHidden;

/* Premultiply a non-premultiplied colour by alpha 0xff or 0x00. */
static uint32_t
nbCursorPixel(unsigned short r, unsigned short g, unsigned short b)
{
    return 0xff000000u | (uint32_t) (r >> 8) << 16 |
        (uint32_t) (g >> 8) << 8 | (uint32_t) (b >> 8);
}

/* Fill a WindowServer buffer with the cursor's image. */
static void
nbCursorImage(CursorPtr pCursor, ws_buffer_t *buffer)
{
    CursorBitsPtr bits = pCursor->bits;
    uint32_t *dst = ws_buffer_data(buffer);
    uint32_t stride = ws_buffer_stride(buffer) / 4;
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

static void
nbCursorShow(CursorPtr pCursor)
{
    ws_buffer_t *buffer;

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
    if (pCursor == nbCursorShown)
        return;

    buffer = ws_buffer_create(nbConnection(), pCursor->bits->width,
                              pCursor->bits->height, 1);
    if (buffer == NULL) {
        ErrorF("Xnextbsd: can't make a cursor buffer: %s\n",
               strerror(errno));
        return;
    }
    nbCursorImage(pCursor, buffer);
    if (ws_set_cursor(nbConnection(), WS_CURSOR_CUSTOM, buffer,
                      pCursor->bits->xhot, pCursor->bits->yhot) < 0) {
        ws_buffer_destroy(nbConnection(), buffer);
        nbWindowServerLost("cursor");
        return;
    }
    /* WindowServer keeps its own reference to the buffer it shows. */
    if (nbCursorBuffer != NULL)
        ws_buffer_destroy(nbConnection(), nbCursorBuffer);
    nbCursorBuffer = buffer;
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
    /* The address may be reused by the next cursor. */
    if (pCursor == nbCursorShown)
        nbCursorShown = NULL;
    return TRUE;
}

static void
nbSetCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCursor,
            int x, int y)
{
    nbCursorShow(pCursor);
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
 * TODO: pointer warps (XWarpPointer) need WindowServer's pointer warp,
 * limited to the active app (a later WindowServer item). Until then only
 * X's idea of the position moves.
 */
static void
nbWarpCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    miPointerWarpCursor(pDev, pScreen, x, y);
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
        *width = 64;
        *height = 64;
    } else {
        fbQueryBestSize(class, width, height, pScreen);
    }
}

Bool
nbCursorInit(ScreenPtr pScreen)
{
    /* A new server generation: the old cursor buffer isn't needed. */
    if (nbCursorBuffer != NULL && nbConnection() != NULL)
        ws_buffer_destroy(nbConnection(), nbCursorBuffer);
    nbCursorBuffer = NULL;
    nbCursorShown = NULL;
    nbCursorHidden = FALSE;
    pScreen->QueryBestSize = nbCursorQueryBestSize;
    return miPointerInitialize(pScreen, &nbSpriteFuncs, &nbPointerScreenFuncs,
                               FALSE);
}
