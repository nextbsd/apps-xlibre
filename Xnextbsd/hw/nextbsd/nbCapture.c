/*
 * Xnextbsd: a rootless X server on NextBSD's WindowServer.
 *
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
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
 * The root window, read through WindowServer's screen capture
 * (nextbsd/apps-xlibre#29, "Screen capture" in WindowServer's
 * WSProtocol.h).
 *
 * X11 screen sharing (Firefox's WebRTC, so Jitsi Meet; recorders;
 * xwd -root) reads the root window with GetImage or MIT-SHM's
 * ShmGetImage, and may follow its changes with DAMAGE. Xnextbsd is
 * rootless: nothing paints the root, and the windows that aren't X
 * (GNUstep apps, the Dock, the desktop) aren't in X at all. So a read
 * of the root is answered from a capture stream of WindowServer's
 * composited screen instead, the real screen with every app on it.
 *
 * Each X client that reads the root (or creates a Damage on it) gets a
 * stream asked for on its behalf (ws_capture_start_for: its pid, its
 * name), so WindowServer's Screen Recording prompt names the program
 * that wants the pixels, Firefox, and not XLibre. Until the user allows
 * it, and after a refusal, the client reads black, as before. Clients
 * of one process share its stream. A stream stops when its clients
 * haven't read the root for NB_CAPTURE_IDLE_MS and have no Damage on
 * it, or go away, so idle costs nothing. The frames' damage rectangles
 * become DAMAGE events on the root.
 *
 * Frames never show the pointer: on X11 a capture tool draws it from
 * XFixes' cursor image itself. Captures of one X window (Composite)
 * read that window as before.
 *
 * Against a WindowServer without WS_SERVER_CAPTURE, or an older
 * libWindowServerClient without the capture calls, the root reads
 * black, as it did.
 */

#include <dix-config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/extensions/damageproto.h>

#include "dix/dix_priv.h"
#include "os/client_priv.h"

#include "dixstruct.h"
#include "extnsionst.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "servermd.h"
#include "damage.h"

#include "nextbsd.h"

#ifdef WS_SERVER_CAPTURE

/* The capture calls of newer libWindowServerClients: NULL in older ones. */
#pragma weak ws_capture_start_for
#pragma weak ws_capture_stop
#pragma weak ws_capture_frame_data
#pragma weak ws_capture_release

/* A stream nobody read for this long stops. */
#define NB_CAPTURE_IDLE_MS      15000
/* Root frames at most this often: X capture tools poll at 30 or less. */
#define NB_CAPTURE_FPS          30

enum {
    NB_CAPTURE_NONE,            /* no stream (none yet, or stopped idle) */
    NB_CAPTURE_ASKING,          /* waiting for the user's answer */
    NB_CAPTURE_RUNNING,
    NB_CAPTURE_REFUSED,         /* refused, or the user stopped it: black
                                   until the process's clients are gone */
};

/* One process's capture of the screen. */
typedef struct nbCapture {
    struct xorg_list link;
    pid_t pid;
    int state;
    uint32_t stream;            /* WindowServer's stream, 0 = none */
    char name[WS_CAPTURE_NAME_MAX];
    int nclients;               /* of its clients we know */
    int damage_watchers;        /* Damage objects on the root */
    CARD32 last_read;           /* GetTimeInMillis() of the last read */
    Bool have_frame;            /* the frame we hold */
    ws_event_capture_frame_t frame;
    const char *data;
    uint32_t stride;
    uint32_t width, height;
} nbCapture;

/* A client of ours that read the root or watches it. */
typedef struct nbCaptureClient {
    struct xorg_list link;
    ClientPtr client;
    nbCapture *cap;
    XID damage[4];              /* its Damage objects on the root */
} nbCaptureClient;

static struct xorg_list nbCaptures;
static struct xorg_list nbCaptureClients;
static Bool nbCaptureListsReady;

/* The client whose request is being handled (GetImage, MIT-SHM). */
static ClientPtr nbCaptureCurrent;

static GetImageProcPtr nbOrigGetImage;
static int (*nbOrigProcGetImage) (ClientPtr);
static int (*nbOrigProcShm) (ClientPtr);
static int (*nbOrigSProcShm) (ClientPtr);
static int (*nbOrigProcDamage) (ClientPtr);
static int nbShmMajor = -1, nbDamageMajor = -1;
static x_server_generation_t nbWrappedGeneration;
static Bool nbCaptureLogged;

static Bool
nbCaptureAvailable(void)
{
    return nbConnection() != NULL &&
        (nbScreenInfo.server_flags & WS_SERVER_CAPTURE) &&
        ws_capture_start_for != NULL && ws_capture_frame_data != NULL &&
        ws_capture_release != NULL && ws_capture_stop != NULL;
}

static nbCapture *
nbCaptureByStream(uint32_t stream)
{
    nbCapture *cap;

    xorg_list_for_each_entry(cap, &nbCaptures, link)
        if (cap->stream == stream && stream != 0)
            return cap;
    return NULL;
}

static void
nbCaptureDropFrame(nbCapture *cap)
{
    if (cap->have_frame && nbConnection() != NULL)
        ws_capture_release(nbConnection(), &cap->frame);
    cap->have_frame = FALSE;
    cap->data = NULL;
}

static void
nbCaptureStop(nbCapture *cap, int state)
{
    nbCaptureDropFrame(cap);
    if (cap->stream != 0 && nbConnection() != NULL)
        ws_capture_stop(nbConnection(), cap->stream);
    cap->stream = 0;
    cap->state = state;
}

/* The program's name for the prompt: its command's, without the path. */
static void
nbCaptureName(ClientPtr client, char *buf, size_t len)
{
    const char *cmd = GetClientCmdName(client), *p;

    buf[0] = '\0';
    if (cmd == NULL)
        return;
    p = strrchr(cmd, '/');
    snprintf(buf, len, "%s", p != NULL ? p + 1 : cmd);
    /* "firefox-esr -contentproc ...": the command alone */
    if ((p = strchr(buf, ' ')) != NULL)
        buf[p - buf] = '\0';
}

/* The record of client's process, made if need be. */
static nbCaptureClient *
nbCaptureClientOf(ClientPtr client)
{
    nbCaptureClient *cc;
    nbCapture *cap;
    pid_t pid;

    xorg_list_for_each_entry(cc, &nbCaptureClients, link)
        if (cc->client == client)
            return cc;
    pid = GetClientPid(client);
    if (pid <= 0)
        return NULL;            /* not local: nobody to ask about */
    cc = calloc(1, sizeof(*cc));
    if (cc == NULL)
        return NULL;
    xorg_list_for_each_entry(cap, &nbCaptures, link)
        if (cap->pid == pid)
            break;
    if (&cap->link == &nbCaptures) {
        cap = calloc(1, sizeof(*cap));
        if (cap == NULL) {
            free(cc);
            return NULL;
        }
        cap->pid = pid;
        nbCaptureName(client, cap->name, sizeof(cap->name));
        xorg_list_add(&cap->link, &nbCaptures);
    }
    cap->nclients++;
    cc->client = client;
    cc->cap = cap;
    xorg_list_add(&cc->link, &nbCaptureClients);
    return cc;
}

/* Ask WindowServer for cap's stream, unless there is one or it's refused. */
static void
nbCaptureStart(nbCapture *cap)
{
    uint32_t flags = 0;

    if (cap->state != NB_CAPTURE_NONE || !nbCaptureAvailable())
        return;
    cap->stream = ws_capture_start_for(nbConnection(), 0, 0, 0, 0,
                                       NB_CAPTURE_FPS, (int32_t) cap->pid,
                                       cap->name, &flags);
    if (cap->stream == 0) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: screen capture for %s "
                       "(pid %d) refused: %s\n", cap->name, (int) cap->pid,
                       strerror(errno));
        cap->state = NB_CAPTURE_REFUSED;
        return;
    }
    cap->state = (flags & WS_CAPTURE_ASKING) ? NB_CAPTURE_ASKING :
        NB_CAPTURE_RUNNING;
    LogMessageVerb(X_INFO, 0, "Xnextbsd: capturing the screen for %s "
                   "(pid %d), stream %u%s\n", cap->name, (int) cap->pid,
                   cap->stream, (flags & WS_CAPTURE_ASKING) ?
                   ": asking the user" : "");
}

/* client reads the root now. */
static nbCapture *
nbCaptureRead(ClientPtr client)
{
    nbCaptureClient *cc;

    if (client == NULL || client == serverClient)
        return NULL;
    cc = nbCaptureClientOf(client);
    if (cc == NULL)
        return NULL;
    cc->cap->last_read = GetTimeInMillis();
    nbCaptureStart(cc->cap);
    return cc->cap;
}

/* ---- the root's pixels ---- */

/*
 * GetImage of the root (ZPixmap, 32 bits per pixel, as our only depth 24
 * visual): the capture's latest frame, or black.
 */
static void
nbCaptureFill(nbCapture *cap, int sx, int sy, int w, int h,
              unsigned long planeMask, char *dst)
{
    size_t dstride = PixmapBytePad(w, 24);
    int y, x, x0, x1;

    memset(dst, 0, dstride * (size_t) h);
    if (cap == NULL || cap->state != NB_CAPTURE_RUNNING ||
        !cap->have_frame || cap->data == NULL)
        return;
    x0 = sx < 0 ? -sx : 0;
    x1 = sx + w > (int) cap->width ? (int) cap->width - sx : w;
    if (x1 <= x0)
        return;
    for (y = 0; y < h; y++) {
        const uint32_t *src;
        uint32_t *out;

        if (sy + y < 0 || sy + y >= (int) cap->height)
            continue;
        /* the row of the frame, from the read's x0 on */
        src = (const uint32_t *) (const void *)
            (cap->data + (size_t) (sy + y) * cap->stride +
             (size_t) (sx + x0) * 4);
        out = (uint32_t *) (void *) (dst + (size_t) y * dstride) + x0;
        if ((planeMask & 0xffffff) == 0xffffff)
            memcpy(out, src, (size_t) (x1 - x0) * 4);
        else
            for (x = 0; x < x1 - x0; x++)
                out[x] = src[x] & (uint32_t) planeMask;
    }
}

static void
nbGetImage(DrawablePtr pDrawable, int sx, int sy, int w, int h,
           unsigned int format, unsigned long planeMask, char *pdstLine)
{
    ScreenPtr pScreen = pDrawable->pScreen;

    if (pDrawable->type == DRAWABLE_WINDOW &&
        ((WindowPtr) pDrawable)->parent == NULL && w > 0 && h > 0) {
        /* the root: coordinates are the screen's */
        if (format == ZPixmap && pDrawable->bitsPerPixel == 32) {
            nbCaptureFill(nbCaptureRead(nbCaptureCurrent),
                          pDrawable->x + sx, pDrawable->y + sy, w, h,
                          planeMask, pdstLine);
        } else {
            /* XYPixmap, a plane at a time: black */
            if (format == ZPixmap)
                memset(pdstLine, 0, PixmapBytePad(w, pDrawable->depth) *
                       (size_t) h);
            else
                memset(pdstLine, 0, BitmapBytePad(w) * (size_t) h);
        }
        return;
    }
    pScreen->GetImage = nbOrigGetImage;
    pScreen->GetImage(pDrawable, sx, sy, w, h, format, planeMask, pdstLine);
    pScreen->GetImage = nbGetImage;
}

/* ---- who asks: the requests that read the root ---- */

static int
nbProcGetImage(ClientPtr client)
{
    int rc;

    nbCaptureCurrent = client;
    rc = nbOrigProcGetImage(client);
    nbCaptureCurrent = NULL;
    return rc;
}

/* MIT-SHM: ShmGetImage reads through pScreen->GetImage too. */
static int
nbProcShm(ClientPtr client)
{
    int rc;

    nbCaptureCurrent = client;
    rc = nbOrigProcShm(client);
    nbCaptureCurrent = NULL;
    return rc;
}

static int
nbSProcShm(ClientPtr client)
{
    int rc;

    nbCaptureCurrent = client;
    rc = nbOrigSProcShm(client);
    nbCaptureCurrent = NULL;
    return rc;
}

/* DAMAGE: a Damage object on the root keeps the client's stream going. */
static int
nbProcDamage(ClientPtr client)
{
    xDamageCreateReq *create = client->requestBuffer;
    xDamageDestroyReq *destroy = client->requestBuffer;
    ScreenPtr pScreen = screenInfo.screens[0];
    nbCaptureClient *cc;
    XID damage = 0;
    int rc, i, minor = create->damageReqType;

    if (client->swapped || pScreen == NULL || pScreen->root == NULL)
        return nbOrigProcDamage(client);
    if (minor == X_DamageCreate &&
        client->req_len >= bytes_to_int32(sizeof(xDamageCreateReq)) &&
        create->drawable == pScreen->root->drawable.id)
        damage = create->damage;
    else if (minor == X_DamageDestroy &&
             client->req_len >= bytes_to_int32(sizeof(xDamageDestroyReq)))
        damage = destroy->damage;
    rc = nbOrigProcDamage(client);
    if (damage == 0 || rc != Success)
        return rc;
    if (minor == X_DamageCreate) {
        cc = nbCaptureClientOf(client);
        if (cc == NULL)
            return rc;
        for (i = 0; i < (int) ARRAY_SIZE(cc->damage); i++)
            if (cc->damage[i] == 0) {
                cc->damage[i] = damage;
                cc->cap->damage_watchers++;
                break;
            }
        cc->cap->last_read = GetTimeInMillis();
        nbCaptureStart(cc->cap);
    } else {
        xorg_list_for_each_entry(cc, &nbCaptureClients, link) {
            if (cc->client != client)
                continue;
            for (i = 0; i < (int) ARRAY_SIZE(cc->damage); i++)
                if (cc->damage[i] == damage) {
                    cc->damage[i] = 0;
                    cc->cap->damage_watchers--;
                }
        }
    }
    return rc;
}

static void
nbCaptureClientGone(CallbackListPtr *list, void *closure, void *data)
{
    NewClientInfoRec *info = data;
    nbCaptureClient *cc, *tmp;
    nbCapture *cap;
    int i;

    if (info->client->clientState != ClientStateGone)
        return;
    xorg_list_for_each_entry_safe(cc, tmp, &nbCaptureClients, link) {
        if (cc->client != info->client)
            continue;
        cap = cc->cap;
        for (i = 0; i < (int) ARRAY_SIZE(cc->damage); i++)
            if (cc->damage[i] != 0)
                cap->damage_watchers--;
        xorg_list_del(&cc->link);
        free(cc);
        if (--cap->nclients == 0) {
            nbCaptureStop(cap, NB_CAPTURE_NONE);
            xorg_list_del(&cap->link);
            free(cap);
        }
    }
}

/*
 * Once per server generation, when the extensions are there: wrap the
 * requests whose client we need to know, and follow clients going away.
 */
static void
nbCaptureWrap(void)
{
    ExtensionEntry *ext;

    if (nbWrappedGeneration == serverGeneration)
        return;
    nbWrappedGeneration = serverGeneration;
    if (ProcVector[X_GetImage] != nbProcGetImage) {
        nbOrigProcGetImage = ProcVector[X_GetImage];
        ProcVector[X_GetImage] = nbProcGetImage;
    }
    if ((ext = CheckExtension("MIT-SHM")) != NULL) {
        nbShmMajor = ext->base;
        if (ProcVector[nbShmMajor] != nbProcShm) {
            nbOrigProcShm = ProcVector[nbShmMajor];
            ProcVector[nbShmMajor] = nbProcShm;
            nbOrigSProcShm = SwappedProcVector[nbShmMajor];
            SwappedProcVector[nbShmMajor] = nbSProcShm;
        }
    }
    if ((ext = CheckExtension("DAMAGE")) != NULL) {
        nbDamageMajor = ext->base;
        if (ProcVector[nbDamageMajor] != nbProcDamage) {
            nbOrigProcDamage = ProcVector[nbDamageMajor];
            ProcVector[nbDamageMajor] = nbProcDamage;
        }
    }
    AddCallback(&ClientStateCallback, nbCaptureClientGone, NULL);
    if (!nbCaptureLogged) {
        nbCaptureLogged = TRUE;
        LogMessageVerb(X_INFO, 0, "Xnextbsd: the root window reads "
                       "WindowServer's screen capture%s\n",
                       nbCaptureAvailable() ? "" : " (not available: "
                       "WindowServer or its client library has none; "
                       "it reads black)");
    }
}

/* ---- WindowServer's events ---- */

/* The frame's changes, as damage on the root. */
static void
nbCaptureDamageRoot(nbCapture *cap, const ws_event_capture_frame_t *f)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    xRectangle rects[WS_CAPTURE_MAX_DAMAGE];
    RegionPtr region;
    uint32_t i, n = 0;

    if (cap->damage_watchers <= 0 || pScreen == NULL ||
        pScreen->root == NULL)
        return;
    for (i = 0; i < f->ndamage && i < WS_CAPTURE_MAX_DAMAGE; i++) {
        if (f->damage[i].width <= 0 || f->damage[i].height <= 0)
            continue;
        rects[n].x = (INT16) f->damage[i].x;
        rects[n].y = (INT16) f->damage[i].y;
        rects[n].width = (CARD16) f->damage[i].width;
        rects[n].height = (CARD16) f->damage[i].height;
        n++;
    }
    if (n == 0)
        return;
    region = RegionFromRects((int) n, rects, CT_UNSORTED);
    if (region == NULL)
        return;
    DamageDamageRegion(&pScreen->root->drawable, region);
    RegionDestroy(region);
}

void
nbCaptureEvent(const ws_event_t *ev)
{
    const ws_event_capture_frame_t *f;
    nbCapture *cap;
    const void *data;
    uint32_t stride;

    switch (ev->hdr.type) {
    case WS_EVENT_CAPTURE_STATE:
        cap = nbCaptureByStream(ev->u.capture_state.stream);
        if (cap == NULL)
            break;
        switch (ev->u.capture_state.state) {
        case WS_CAPTURE_RUNNING:
            cap->state = NB_CAPTURE_RUNNING;
            LogMessageVerb(X_INFO, 0, "Xnextbsd: the user allowed %s "
                           "(pid %d) to record the screen\n", cap->name,
                           (int) cap->pid);
            break;
        case WS_CAPTURE_DENIED:
        case WS_CAPTURE_STOPPED:
            /* refused, or stopped by the user: black from now on;
               ended by us: none */
            cap->stream = 0;
            nbCaptureDropFrame(cap);
            cap->state = ev->u.capture_state.error == 0 ?
                NB_CAPTURE_NONE : NB_CAPTURE_REFUSED;
            LogMessageVerb(X_INFO, 0, "Xnextbsd: screen capture for %s "
                           "(pid %d) %s: %s\n", cap->name, (int) cap->pid,
                           ev->u.capture_state.state == WS_CAPTURE_DENIED ?
                           "refused" : "ended",
                           strerror(ev->u.capture_state.error));
            break;
        }
        break;
    case WS_EVENT_CAPTURE_FRAME:
        f = &ev->u.capture_frame;
        cap = nbCaptureByStream(f->stream);
        data = ws_capture_frame_data(nbConnection(), f, &stride);
        if (cap == NULL || data == NULL) {
            ws_capture_release(nbConnection(), f);
            break;
        }
        /* keep the newest frame, give the one before back */
        nbCaptureDropFrame(cap);
        cap->frame = *f;
        cap->have_frame = TRUE;
        cap->data = data;
        cap->stride = stride;
        cap->state = NB_CAPTURE_RUNNING;
        nbCaptureDamageRoot(cap, f);
        break;
    case WS_EVENT_CAPTURE_BUFFER:
        cap = nbCaptureByStream(ev->u.capture_buffer.stream);
        if (cap != NULL) {
            cap->width = ev->u.capture_buffer.width;
            cap->height = ev->u.capture_buffer.height;
        }
        break;
    default:
        break;
    }
}

/* Before sleeping: wrap once the extensions are up; stop idle streams. */
void
nbCaptureBlockHandler(void)
{
    CARD32 now = GetTimeInMillis();
    nbCapture *cap;

    if (!nbCaptureListsReady)
        return;
    nbCaptureWrap();
    xorg_list_for_each_entry(cap, &nbCaptures, link) {
        if (cap->stream == 0 || cap->damage_watchers > 0 ||
            now - cap->last_read < NB_CAPTURE_IDLE_MS)
            continue;
        LogMessageVerb(X_INFO, 0, "Xnextbsd: %s (pid %d) stopped reading "
                       "the screen; capture stopped\n", cap->name,
                       (int) cap->pid);
        nbCaptureStop(cap, NB_CAPTURE_NONE);
    }
}

Bool
nbCaptureInit(ScreenPtr pScreen)
{
    if (!nbCaptureListsReady) {
        xorg_list_init(&nbCaptures);
        xorg_list_init(&nbCaptureClients);
        nbCaptureListsReady = TRUE;
    }
    /* above rootless, whose GetImage reads top-level windows */
    nbOrigGetImage = pScreen->GetImage;
    pScreen->GetImage = nbGetImage;
    return TRUE;
}

#else /* !WS_SERVER_CAPTURE: WindowServer headers without capture */

void
nbCaptureEvent(const ws_event_t *ev)
{
}

void
nbCaptureBlockHandler(void)
{
}

Bool
nbCaptureInit(ScreenPtr pScreen)
{
    return TRUE;
}

#endif
