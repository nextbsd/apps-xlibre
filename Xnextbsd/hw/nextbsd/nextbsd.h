/*
 * Xnextbsd: a rootless X server on NextBSD's WindowServer.
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

#ifndef XNEXTBSD_H
#define XNEXTBSD_H

#include <dix-config.h>

#include <stdint.h>

#include <WindowServer/WSClient.h>

#include "list.h"
#include "regionstr.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "rootless.h"
#include "cursor.h"

/*
 * A WindowServer buffer of a frame. WindowServer reads it while it is
 * attached (and until it hands it back), so X only writes to one it
 * doesn't hold.
 */
typedef struct nbBuffer {
    ws_buffer_t *ws;            /* NULL until first needed */
    Bool busy;                  /* attached, or not handed back yet */
    RegionRec stale;            /* frame-local: where it lags the backing */
} nbBuffer;

#define NB_NBUFFERS 2

/*
 * One WindowServer window per top-level X window: rootless calls it a
 * frame. The RootlessFrameID handed to rootless is a pointer to this.
 */
typedef struct nbFrame {
    struct xorg_list link;      /* nbScreen.frames */
    uint32_t wid;               /* WindowServer window id */
    char *bits;                 /* the backing rootless draws into */
    unsigned int stride;        /* bytes per row of bits */
    nbBuffer buffers[NB_NBUFFERS];
    Bool frame_pending;         /* committed, WS_EVENT_FRAME_DONE not in */
    Bool commit_wanted;         /* damage waits for FRAME_DONE or a buffer */
    int x, y;                   /* screen position, border included */
    unsigned int width, height;
    int32_t level;              /* WS_LEVEL_* */
    uint32_t flags;             /* WS_WINDOW_* */
    uint32_t format;            /* WS_FORMAT_* of the buffer */
    Bool ordered_in;            /* in WindowServer's visible stack */
    RegionRec damage;           /* frame-local, not committed yet */
    Bool shaped;                /* X Shape: the bounding shape below */
    RegionRec shape;            /* frame-local; outside it the buffer is
                                   transparent */
    CursorPtr cursor;           /* the X cursor WindowServer shows over
                                   this frame (WS_SERVER_WINDOW_CURSOR) */
    Bool minimized;             /* minimized on WindowServer (in the Dock or
                                   a tile) while X keeps it mapped */
    uint32_t limits[4];         /* the size limits WindowServer last got:
                                   min width, height, max width, height */
    char *title;                /* the title WindowServer last got (the
                                   client's name), NULL for the first one */
    RootlessWindowPtr rwin;
} nbFrame;

typedef struct nbScreen {
    ws_connection_t *conn;
    uint32_t server_flags;      /* WS_SERVER_*: what WindowServer can do */
    char *keymap;               /* WindowServer's compiled XKB keymap */
    size_t keymap_len;
    int repeat_delay;           /* from the keymap event, ms */
    int repeat_interval;        /* 0: no key repeat */
    int width, height;
    int mm_width, mm_height;
    unsigned int refresh_mhz;   /* WindowServer's refresh rate, 0 if unknown */
    char screen_name[32];       /* WindowServer's name for the screen */
    ws_rect_t usable;           /* WindowServer's usable area */
    Bool usable_dirty;          /* not on the root window yet */
    struct xorg_list frames;
    Bool active;                /* we are WindowServer's active app */
    nbFrame *pointer_frame;     /* the frame WindowServer says the pointer
                                   is over (or grabbed by), or NULL */
    nbFrame *key_frame;         /* WindowServer's key window, or NULL */
    nbFrame *x_focus_frame;     /* frame of X's input focus last time we
                                   looked, to tell WindowServer once */
    Bool hidden;                /* XLibre.app hid us: every frame is ordered
                                   out on WindowServer, X keeps them mapped */
    Bool ws_grab;               /* we hold WindowServer's pointer grab, for
                                   an X client's active grab */
    Bool frames_dirty;          /* a window manager property changed: look
                                   at every frame's style, size limits and
                                   minimized state before sleeping */
    XYToWindowProcPtr XYToWindow;
} nbScreen;

extern nbScreen nbScreenInfo;

/* nbInit.c */
ws_connection_t *nbConnection(void);
void nbWindowServerLost(const char *what);

/* nbFrame.c */
Bool nbFrameInit(ScreenPtr pScreen);
nbFrame *nbFrameFromWID(uint32_t wid);
WindowPtr nbFrameWindow(nbFrame *frame);
void nbFrameOrdered(nbFrame *frame, const ws_event_ordered_t *ordered);
void nbFrameDone(nbFrame *frame);
void nbFrameBufferReleased(nbFrame *frame, uint32_t buffer_id);
void nbFrameSetByWindowServer(nbFrame *frame, const ws_rect_t *r);
void nbFrameRestoreRequested(nbFrame *frame);
void nbFramesUpdate(void);
nbFrame *nbFrameOfWindow(WindowPtr pWin);

/*
 * The window manager's channel to the server, a property on the root
 * window: _XNEXTBSD_HIDDEN (CARDINAL/32), set to 1 by XLibre.app when it
 * is hidden and to 0 (or deleted) when it is shown again.
 */
#define NB_HIDDEN_ATOM "_XNEXTBSD_HIDDEN"

/*
 * The other way, set by the server: _XNEXTBSD_USABLE_AREA (CARDINAL/32
 * x, y, width, height) on the root window is WindowServer's usable area,
 * the screen minus the Dock's strip, for the window manager to place
 * windows in.
 */
#define NB_USABLE_AREA_ATOM "_XNEXTBSD_USABLE_AREA"

/*
 * Minimizing, set by the window manager on a top-level window (its frame):
 * _XNEXTBSD_MINIMIZED (CARDINAL/32, one of WS_MINIMIZE_DEFAULT,
 * _BOTTOM_ROW, _INTO_DOCK) asks the server to minimize that window on
 * WindowServer, there; deleting it shows the window again. The X window
 * stays mapped meanwhile, so WindowServer's preview of it stays live.
 *
 * When the user restores the window from WindowServer (the Dock, a
 * tile, the overview), the server shows it again, deletes the property
 * and sends the window manager a _XNEXTBSD_RESTORE ClientMessage (format
 * 32, window and data.l[0] the top-level window) on the root window with
 * SubstructureRedirectMask, so that it sets WM_STATE back to NormalState,
 * raises the window and gives it the focus.
 */
#define NB_MINIMIZED_ATOM "_XNEXTBSD_MINIMIZED"
#define NB_RESTORE_ATOM "_XNEXTBSD_RESTORE"

/* nbRandR.c */
Bool nbRandRInit(ScreenPtr pScreen);
void nbScreenChanged(const ws_screen_info_t *si);
void nbUsableAreaChanged(const ws_rect_t *usable);
void nbUsableAreaPublish(void);

/* nbInput.c */
void nbInputDispatch(int fd, int ready, void *data);
void nbInputDrain(void);
void nbInputScreenInit(ScreenPtr pScreen);
void nbFocusCheck(void);
void nbInputWaitForKeymap(void);

/* nbCursor.c */
Bool nbCursorInit(ScreenPtr pScreen);

/* nbConfig.c: the keyboard settings WindowServer itself uses */
typedef struct nbKeyboardConfig {
    char rules[64];
    char model[64];
    char layout[64];
    char variant[64];
    char options[256];
    int repeat_delay;           /* ms */
    int repeat_interval;        /* ms */
} nbKeyboardConfig;

void nbReadKeyboardConfig(nbKeyboardConfig *cfg);

#endif /* XNEXTBSD_H */
