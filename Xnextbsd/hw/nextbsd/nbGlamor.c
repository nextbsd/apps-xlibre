/*
 * Xnextbsd: X rendering on the GPU (glamor), GPU buffers for WindowServer,
 * and DRI3, Present and GLX for X clients.
 *
 * Copyright (c) 2026, Joseph Maloney
 * The EGL, GBM and DRI3 parts follow glamor/glamor_egl.c:
 * Copyright © 2010 Intel Corporation.
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
 * When WindowServer takes GPU buffers (WS_SERVER_DMABUF, "GPU buffers" in
 * its WSProtocol.h) and the render node it names gives us an EGL context,
 * Xnextbsd draws on the GPU:
 *
 *   - glamor does X rendering, on an EGL context on GBM on WindowServer's
 *     render node (/dev/dri/renderD128, v3d, on a Raspberry Pi 5);
 *   - each frame (top-level window) is a glamor pixmap rootless draws into
 *     (its GetPixmap proc, patches/0002), instead of CPU memory;
 *   - a frame's two WindowServer buffers are GBM buffer objects with a
 *     modifier WindowServer listed (Broadcom UIF first on the Pi), handed
 *     over as dmabufs: a commit copies the damage from the frame's pixmap
 *     into a free one on the GPU (no CPU copy), flushes, and attaches it;
 *   - X clients get DRI3 (buffers on the same render node, with
 *     modifiers), Present (copy mode, timed by a fake vblank) and GLX
 *     (glamor's EGL provider; clients render directly with DRI3), so GL
 *     and EGL programs, Chromium's GPU process among them, render on the
 *     GPU.
 *
 * Anything missing (an old WindowServer, no render node, no EGL, an EGL
 * without the extensions glamor needs, -nogpu or XNEXTBSD_GPU=0) leaves
 * the server as it was: fb and pixman in CPU memory, shared-memory
 * buffers, no DRI3 or GLX.
 */

#include <dix-config.h>

#ifdef GLAMOR

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include "dix/screen_hooks_priv.h"
#include "glamor/glamor_priv.h"
#include "glamor_egl.h"
#include "glamor_glx_provider.h"
#include "os/bug_priv.h"
#include "dri3.h"
#include "present.h"
#include "picturestr.h"
#include "mi.h"

#include "nextbsd.h"

#ifndef GBM_MAX_PLANES
#define GBM_MAX_PLANES 4
#endif

typedef struct nbGlamor {
    Bool enabled;               /* glamor is drawing, frames are GPU */
    int fd;                     /* the render node */
    char *device_path;          /* its path, for DRI3 clients */
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    Bool dmabuf_capable;        /* dmabuf import and export with modifiers */
    Bool has_image_dma_buf_export;
#ifdef WS_SERVER_DMABUF
    ws_dmabuf_formats_t ws_formats; /* what WindowServer composites */
#endif
    unsigned long buffers;      /* GPU buffers made, for the log */
} nbGlamor;

static nbGlamor nbGL = { .fd = -1, .display = EGL_NO_DISPLAY,
                         .context = EGL_NO_CONTEXT };

Bool
nbGlamorEnabled(void)
{
    return nbGL.enabled;
}

/* ---- what glamor asks of its EGL layer (glamor_egl.c's, without xf86) ---- */

static void
nbGlamorMakeCurrent(struct glamor_context *glamor_ctx)
{
    /* As glamor_egl.c: drop the context first, so that EGL's fast path
       for "already current" doesn't skip a switch back from GLX. */
    eglMakeCurrent(glamor_ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (!eglMakeCurrent(glamor_ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        glamor_ctx->ctx))
        FatalError("Xnextbsd: can't make the EGL context current\n");
}

static void
nbGlamorSetPixmapImage(PixmapPtr pixmap, EGLImageKHR image,
                       Bool used_modifiers)
{
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);

    BUG_RETURN(!pixmap_priv);
    if (pixmap_priv->image)
        eglDestroyImageKHR(nbGL.display, pixmap_priv->image);
    pixmap_priv->image = image;
    pixmap_priv->used_modifiers = used_modifiers;
}

/* A pixmap of a GBM buffer object: an EGLImage of it as the texture. */
static Bool
nbGlamorPixmapFromBO(PixmapPtr pixmap, struct gbm_bo *bo, Bool used_modifiers)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    EGLImageKHR image;
    GLuint texture;
    EGLint attrs[64];
    int fds[GBM_MAX_PLANES];
    int planes = gbm_bo_get_plane_count(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    int i, n = 0;

    static const EGLint plane_attrs[GBM_MAX_PLANES][5] = {
        { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
          EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
          EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
          EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };

    if (planes < 1 || planes > GBM_MAX_PLANES)
        return FALSE;
    glamor_make_current(glamor_priv);

    attrs[n++] = EGL_WIDTH;
    attrs[n++] = gbm_bo_get_width(bo);
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = gbm_bo_get_height(bo);
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = gbm_bo_get_format(bo);
    for (i = 0; i < planes; i++) {
        fds[i] = gbm_bo_get_fd_for_plane(bo, i);
        attrs[n++] = plane_attrs[i][0];
        attrs[n++] = fds[i];
        attrs[n++] = plane_attrs[i][1];
        attrs[n++] = gbm_bo_get_offset(bo, i);
        attrs[n++] = plane_attrs[i][2];
        attrs[n++] = gbm_bo_get_stride_for_plane(bo, i);
        if (nbGL.dmabuf_capable && modifier != DRM_FORMAT_MOD_INVALID) {
            attrs[n++] = plane_attrs[i][3];
            attrs[n++] = (EGLint) (modifier & 0xffffffffu);
            attrs[n++] = plane_attrs[i][4];
            attrs[n++] = (EGLint) (modifier >> 32);
        }
    }
    attrs[n++] = EGL_NONE;

    image = eglCreateImageKHR(nbGL.display, EGL_NO_CONTEXT,
                              EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    for (i = 0; i < planes; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    if (image == EGL_NO_IMAGE_KHR) {
        glamor_set_pixmap_type(pixmap, GLAMOR_DRM_ONLY);
        return FALSE;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    glamor_set_pixmap_texture(pixmap, texture);
    nbGlamorSetPixmapImage(pixmap, image, used_modifiers);
    return TRUE;
}

static uint32_t
nbGbmFormatForDepth(int depth)
{
    switch (depth) {
    case 15:
        return GBM_FORMAT_ARGB1555;
    case 16:
        return GBM_FORMAT_RGB565;
    case 24:
        return GBM_FORMAT_XRGB8888;
    case 30:
        return GBM_FORMAT_ARGB2101010;
    case 32:
        return GBM_FORMAT_ARGB8888;
    case 8:
        return GBM_FORMAT_R8;
    default:
        return 0;
    }
}

Bool
glamor_get_formats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    EGLint num;

    *num_formats = 0;
    *formats = NULL;
    if (!nbGL.dmabuf_capable)
        return TRUE;
    if (!eglQueryDmaBufFormatsEXT(nbGL.display, 0, NULL, &num))
        return FALSE;
    if (num == 0)
        return TRUE;
    *formats = calloc(num, sizeof(CARD32));
    if (*formats == NULL)
        return FALSE;
    if (!eglQueryDmaBufFormatsEXT(nbGL.display, num, (EGLint *) *formats,
                                  &num)) {
        free(*formats);
        *formats = NULL;
        return FALSE;
    }
    *num_formats = num;
    return TRUE;
}

Bool
glamor_get_modifiers(ScreenPtr screen, uint32_t format,
                     uint32_t *num_modifiers, uint64_t **modifiers)
{
    EGLint num;

    *num_modifiers = 0;
    *modifiers = NULL;
    if (!nbGL.dmabuf_capable)
        return FALSE;
    if (!eglQueryDmaBufModifiersEXT(nbGL.display, format, 0, NULL, NULL,
                                    &num))
        return FALSE;
    if (num == 0)
        return TRUE;
    *modifiers = calloc(num, sizeof(uint64_t));
    if (*modifiers == NULL)
        return FALSE;
    if (!eglQueryDmaBufModifiersEXT(nbGL.display, format, num,
                                    (EGLuint64KHR *) *modifiers, NULL,
                                    &num)) {
        free(*modifiers);
        *modifiers = NULL;
        return FALSE;
    }
    *num_modifiers = num;
    return TRUE;
}

void
glamor_egl_exchange_buffers(PixmapPtr front, PixmapPtr back)
{
    struct glamor_pixmap_private *front_priv =
        glamor_get_pixmap_private(front);
    struct glamor_pixmap_private *back_priv =
        glamor_get_pixmap_private(back);
    EGLImageKHR image;
    Bool mods;

    BUG_RETURN(!front_priv || !back_priv);
    glamor_pixmap_exchange_fbos(front, back);
    image = back_priv->image;
    mods = back_priv->used_modifiers;
    back_priv->image = front_priv->image;
    back_priv->used_modifiers = front_priv->used_modifiers;
    front_priv->image = image;
    front_priv->used_modifiers = mods;
    glamor_set_pixmap_type(front, GLAMOR_TEXTURE_DRM);
    glamor_set_pixmap_type(back, GLAMOR_TEXTURE_DRM);
}

/*
 * Give a pixmap a GBM buffer object (so that it can be exported) if it
 * hasn't one: a new one, with the modifiers EGL can import if allowed,
 * the pixmap's contents copied in and the two swapped.
 */
static Bool
nbGlamorMakeExportable(PixmapPtr pixmap, Bool modifiers_ok)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);
    unsigned int width = pixmap->drawable.width;
    unsigned int height = pixmap->drawable.height;
    uint32_t format = nbGbmFormatForDepth(pixmap->drawable.depth);
    struct gbm_bo *bo = NULL;
    Bool used_modifiers = FALSE;
    PixmapPtr exported;
    GCPtr gc;

    BUG_RETURN_VAL(!pixmap_priv, FALSE);
    if (pixmap_priv->image &&
        (modifiers_ok || !pixmap_priv->used_modifiers))
        return TRUE;
    /* 24 and 32 are ARGB8888, as glamor_egl.c makes them */
    if (pixmap->drawable.depth == 24)
        format = GBM_FORMAT_ARGB8888;
    if (format == 0)
        return FALSE;

    if (modifiers_ok && nbGL.dmabuf_capable) {
        uint32_t num = 0;
        uint64_t *mods = NULL;

        glamor_get_modifiers(screen, format, &num, &mods);
        if (num > 0)
            bo = gbm_bo_create_with_modifiers2(nbGL.gbm, width, height,
                                               format, mods, num,
                                               GBM_BO_USE_RENDERING);
        if (bo != NULL)
            used_modifiers = TRUE;
        free(mods);
    }
    if (bo == NULL)
        bo = gbm_bo_create(nbGL.gbm, width, height, format,
                           (pixmap->usage_hint == CREATE_PIXMAP_USAGE_SHARED ?
                            GBM_BO_USE_LINEAR : 0) | GBM_BO_USE_RENDERING);
    if (bo == NULL) {
        ErrorF("Xnextbsd: can't make a %ux%u GBM buffer to export\n",
               width, height);
        return FALSE;
    }

    exported = screen->CreatePixmap(screen, 0, 0, pixmap->drawable.depth, 0);
    if (exported == NULL) {
        gbm_bo_destroy(bo);
        return FALSE;
    }
    screen->ModifyPixmapHeader(exported, width, height, 0, 0,
                               gbm_bo_get_stride(bo), NULL);
    if (!nbGlamorPixmapFromBO(exported, bo, used_modifiers)) {
        dixDestroyPixmap(exported, 0);
        gbm_bo_destroy(bo);
        return FALSE;
    }
    gbm_bo_destroy(bo);

    gc = GetScratchGC(pixmap->drawable.depth, screen);
    ValidateGC(&exported->drawable, gc);
    (void) gc->ops->CopyArea(&pixmap->drawable, &exported->drawable, gc,
                             0, 0, width, height, 0, 0);
    FreeScratchGC(gc);

    glamor_egl_exchange_buffers(pixmap, exported);
    screen->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, exported->devKind, NULL);
    dixDestroyPixmap(exported, 0);
    return TRUE;
}

static struct gbm_bo *
nbGlamorBOFromPixmap(PixmapPtr pixmap)
{
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);

    if (pixmap_priv == NULL || !pixmap_priv->image)
        return NULL;
    return gbm_bo_import(nbGL.gbm, GBM_BO_IMPORT_EGL_IMAGE,
                         pixmap_priv->image, 0);
}

struct gbm_bo *
glamor_gbm_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap)
{
    if (!nbGlamorMakeExportable(pixmap, TRUE))
        return NULL;
    return nbGlamorBOFromPixmap(pixmap);
}

int
glamor_egl_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *strides, uint32_t *offsets,
                           uint64_t *modifier)
{
    struct gbm_bo *bo;
    int num, i;

    if (!nbGlamorMakeExportable(pixmap, TRUE) ||
        (bo = nbGlamorBOFromPixmap(pixmap)) == NULL)
        return 0;
    num = gbm_bo_get_plane_count(bo);
    for (i = 0; i < num; i++) {
        fds[i] = gbm_bo_get_fd_for_plane(bo, i);
        if (fds[i] < 0) {
            while (--i >= 0)
                close(fds[i]);
            gbm_bo_destroy(bo);
            return 0;
        }
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
    }
    *modifier = gbm_bo_get_modifier(bo);
    gbm_bo_destroy(bo);
    return num;
}

int
glamor_egl_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                          CARD16 *stride, CARD32 *size)
{
    struct gbm_bo *bo;
    int fd;

    if (!nbGlamorMakeExportable(pixmap, FALSE) ||
        (bo = nbGlamorBOFromPixmap(pixmap)) == NULL)
        return -1;
    fd = gbm_bo_get_fd(bo);
    *stride = gbm_bo_get_stride(bo);
    *size = *stride * gbm_bo_get_height(bo);
    gbm_bo_destroy(bo);
    return fd;
}

/* DRI2-style flink names: not on a render node. */
int
glamor_egl_fd_name_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size)
{
    return -1;
}

Bool
glamor_back_pixmap_from_fd(PixmapPtr pixmap, int fd, CARD16 width,
                           CARD16 height, CARD16 stride, CARD8 depth,
                           CARD8 bpp)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct gbm_import_fd_data data = { 0 };
    struct gbm_bo *bo;
    Bool ret;

    data.format = nbGbmFormatForDepth(depth);
    if (data.format == 0 || width == 0 || height == 0)
        return FALSE;
    data.fd = fd;
    data.width = width;
    data.height = height;
    data.stride = stride;
    bo = gbm_bo_import(nbGL.gbm, GBM_BO_IMPORT_FD, &data,
                       GBM_BO_USE_RENDERING);
    if (bo == NULL)
        return FALSE;
    screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, stride, NULL);
    ret = nbGlamorPixmapFromBO(pixmap, bo, FALSE);
    gbm_bo_destroy(bo);
    return ret;
}

PixmapPtr
glamor_pixmap_from_fds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                       CARD16 width, CARD16 height, const CARD32 *strides,
                       const CARD32 *offsets, CARD8 depth, CARD8 bpp,
                       uint64_t modifier)
{
    PixmapPtr pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    Bool ret = FALSE;
    int i;

    if (pixmap == NULL)
        return NULL;
    if (nbGL.dmabuf_capable && modifier != DRM_FORMAT_MOD_INVALID) {
        struct gbm_import_fd_modifier_data data = { 0 };
        struct gbm_bo *bo;

        data.format = nbGbmFormatForDepth(depth);
        if (data.format != 0 && width != 0 && height != 0 &&
            num_fds <= GBM_MAX_PLANES) {
            data.width = width;
            data.height = height;
            data.num_fds = num_fds;
            data.modifier = modifier;
            for (i = 0; i < num_fds; i++) {
                data.fds[i] = fds[i];
                data.strides[i] = strides[i];
                data.offsets[i] = offsets[i];
            }
            bo = gbm_bo_import(nbGL.gbm, GBM_BO_IMPORT_FD_MODIFIER, &data,
                               GBM_BO_USE_RENDERING);
            if (bo != NULL) {
                screen->ModifyPixmapHeader(pixmap, width, height, 0, 0,
                                           strides[0], NULL);
                ret = nbGlamorPixmapFromBO(pixmap, bo, TRUE);
                gbm_bo_destroy(bo);
            }
        }
    } else if (num_fds == 1) {
        ret = glamor_back_pixmap_from_fd(pixmap, fds[0], width, height,
                                         strides[0], depth, bpp);
    }
    if (!ret) {
        dixDestroyPixmap(pixmap, 0);
        return NULL;
    }
    return pixmap;
}

PixmapPtr
glamor_pixmap_from_fd(ScreenPtr screen, int fd, CARD16 width, CARD16 height,
                      CARD16 stride, CARD8 depth, CARD8 bpp)
{
    PixmapPtr pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);

    if (pixmap == NULL)
        return NULL;
    if (!glamor_back_pixmap_from_fd(pixmap, fd, width, height, stride,
                                    depth, bpp)) {
        dixDestroyPixmap(pixmap, 0);
        return NULL;
    }
    return pixmap;
}

struct gbm_device *
glamor_egl_get_gbm_device(ScreenPtr screen)
{
    return nbGL.gbm;
}

static void
nbGlamorPixmapDestroy(CallbackListPtr *pcbl, ScreenPtr pScreen,
                      PixmapPtr pixmap)
{
    struct glamor_pixmap_private *pixmap_priv =
        glamor_get_pixmap_private(pixmap);

    if (pixmap_priv != NULL && pixmap_priv->image) {
        eglDestroyImageKHR(nbGL.display, pixmap_priv->image);
        pixmap_priv->image = NULL;
    }
}

#ifdef DRI3
/*
 * DRI3 clients get the render node: no DRM authentication on one, and
 * they render with their own address space on the GPU.
 */
static int
nbDri3OpenClient(ClientPtr client, ScreenPtr screen, RRProviderPtr provider,
                 int *fdp)
{
    int fd = open(nbGL.device_path, O_RDWR | O_CLOEXEC);
    drm_magic_t magic;

    if (fd < 0)
        return BadAlloc;
    if (drmGetMagic(fd, &magic) < 0 && errno != EACCES) {
        close(fd);
        return BadMatch;
    }
    *fdp = fd;
    return Success;
}

static const dri3_screen_info_rec nbDri3Info = {
    .version = 2,
    .open_client = nbDri3OpenClient,
    .pixmap_from_fds = glamor_pixmap_from_fds,
    .fd_from_pixmap = glamor_egl_fd_from_pixmap,
    .fds_from_pixmap = glamor_egl_fds_from_pixmap,
    .get_formats = glamor_get_formats,
    .get_modifiers = glamor_get_modifiers,
    .get_drawable_modifiers = glamor_get_drawable_modifiers,
};
#endif

/* glamor_init (GLAMOR_USE_EGL_SCREEN) calls this. */
void
glamor_egl_screen_init(ScreenPtr screen, struct glamor_context *glamor_ctx)
{
    dixScreenHookPixmapDestroy(screen, nbGlamorPixmapDestroy);
    glamor_ctx->ctx = nbGL.context;
    glamor_ctx->display = nbGL.display;
    glamor_ctx->make_current = nbGlamorMakeCurrent;

#ifdef DRI3
    glamor_enable_dri3(screen);
    if (!dri3_screen_init(screen, &nbDri3Info))
        ErrorF("Xnextbsd: can't set up DRI3\n");
#endif
#ifdef GLXEXT
    {
        static Bool pushed;

        if (!pushed) {
            GlxPushProvider(&glamor_provider);
            pushed = TRUE;
        }
    }
#endif
}

/* ---- bringing it up ---- */

static void
nbGlamorCleanup(void)
{
    if (nbGL.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(nbGL.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        lastGLContext = NULL;
        eglTerminate(nbGL.display);
        nbGL.display = EGL_NO_DISPLAY;
    }
    nbGL.context = EGL_NO_CONTEXT;
    if (nbGL.gbm != NULL)
        gbm_device_destroy(nbGL.gbm);
    nbGL.gbm = NULL;
    if (nbGL.fd >= 0)
        close(nbGL.fd);
    nbGL.fd = -1;
    free(nbGL.device_path);
    nbGL.device_path = NULL;
}

/* An EGL context: desktop GL 2.1 or later if there is one, else GLES 2. */
static Bool
nbGlamorCreateContext(void)
{
    static const EGLint core[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, GLAMOR_GL_CORE_VER_MAJOR,
        EGL_CONTEXT_MINOR_VERSION_KHR, GLAMOR_GL_CORE_VER_MINOR,
        EGL_NONE
    };
    static const EGLint none[] = { EGL_NONE };
    static const EGLint gles[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const char *api = getenv("XNEXTBSD_GL_API");   /* "gl" or "es" */

    if ((api == NULL || strcmp(api, "es") != 0) &&
        eglBindAPI(EGL_OPENGL_API)) {
        nbGL.context = eglCreateContext(nbGL.display, EGL_NO_CONFIG_KHR,
                                        EGL_NO_CONTEXT, core);
        if (nbGL.context == EGL_NO_CONTEXT)
            nbGL.context = eglCreateContext(nbGL.display, EGL_NO_CONFIG_KHR,
                                            EGL_NO_CONTEXT, none);
        if (nbGL.context != EGL_NO_CONTEXT) {
            if (!eglMakeCurrent(nbGL.display, EGL_NO_SURFACE,
                                EGL_NO_SURFACE, nbGL.context))
                return FALSE;
            if (epoxy_gl_version() < 21) {
                eglMakeCurrent(nbGL.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
                eglDestroyContext(nbGL.display, nbGL.context);
                nbGL.context = EGL_NO_CONTEXT;
            }
        }
    }
    if (nbGL.context == EGL_NO_CONTEXT && (api == NULL ||
                                           strcmp(api, "gl") != 0)) {
        if (!eglBindAPI(EGL_OPENGL_ES_API))
            return FALSE;
        nbGL.context = eglCreateContext(nbGL.display, EGL_NO_CONFIG_KHR,
                                        EGL_NO_CONTEXT, gles);
        if (nbGL.context != EGL_NO_CONTEXT &&
            !eglMakeCurrent(nbGL.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            nbGL.context))
            return FALSE;
    }
    return nbGL.context != EGL_NO_CONTEXT;
}

/* WindowServer's render node, or the first one there is. */
static char *
nbGlamorRenderNode(void)
{
#ifdef WS_SERVER_DMABUF
    if (nbGL.ws_formats.render_node[0] != '\0')
        return strdup(nbGL.ws_formats.render_node);
#endif
    {
        drmDevicePtr devices[16];
        int i, n = drmGetDevices2(0, devices, 16);
        char *path = NULL;

        for (i = 0; i < n; i++)
            if (path == NULL &&
                (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
                path = strdup(devices[i]->nodes[DRM_NODE_RENDER]);
        if (n > 0)
            drmFreeDevices(devices, n);
        return path;
    }
}

Bool
nbGlamorInit(ScreenPtr pScreen)
{
    const char *gpu = getenv("XNEXTBSD_GPU");
    const char *mods = getenv("XNEXTBSD_DMABUF_MODIFIERS");
    const char *renderer;
    GLint max_texture = 0;

    if (nbScreenInfo.nogpu || (gpu != NULL && strcmp(gpu, "0") == 0)) {
        LogMessageVerb(X_INFO, 0, "Xnextbsd: GPU off (-nogpu or "
                       "XNEXTBSD_GPU=0): drawing in CPU memory\n");
        return FALSE;
    }
#ifndef WS_SERVER_DMABUF
    LogMessageVerb(X_INFO, 0, "Xnextbsd: built without WindowServer's GPU "
                   "buffers: drawing in CPU memory\n");
    return FALSE;
#else
    if (!(nbScreenInfo.server_flags & WS_SERVER_DMABUF)) {
        LogMessageVerb(X_INFO, 0, "Xnextbsd: WindowServer takes no GPU "
                       "buffers: drawing in CPU memory\n");
        return FALSE;
    }
    if (ws_dmabuf_formats(nbConnection(), &nbGL.ws_formats) < 0 ||
        nbGL.ws_formats.count == 0) {
        LogMessageVerb(X_INFO, 0, "Xnextbsd: WindowServer lists no GPU "
                       "buffer formats: drawing in CPU memory\n");
        return FALSE;
    }

    nbGL.device_path = nbGlamorRenderNode();
    if (nbGL.device_path == NULL ||
        (nbGL.fd = open(nbGL.device_path, O_RDWR | O_CLOEXEC)) < 0) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: can't open the render node "
                       "%s (%s): drawing in CPU memory\n",
                       nbGL.device_path ? nbGL.device_path : "(none)",
                       strerror(errno));
        goto fail;
    }
    nbGL.gbm = gbm_create_device(nbGL.fd);
    if (nbGL.gbm == NULL)
        goto fail;
    nbGL.display = glamor_egl_get_display(EGL_PLATFORM_GBM_MESA, nbGL.gbm);
    if (nbGL.display == EGL_NO_DISPLAY ||
        !eglInitialize(nbGL.display, NULL, NULL)) {
        nbGL.display = EGL_NO_DISPLAY;
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: no EGL on %s: drawing in "
                       "CPU memory\n", nbGL.device_path);
        goto fail;
    }
    if (!epoxy_has_egl_extension(nbGL.display, "EGL_KHR_surfaceless_context") ||
        !epoxy_has_egl_extension(nbGL.display, "EGL_KHR_no_config_context") ||
        !epoxy_has_egl_extension(nbGL.display, "EGL_EXT_image_dma_buf_import")) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: EGL on %s lacks surfaceless "
                       "contexts or dmabuf import: drawing in CPU memory\n",
                       nbGL.device_path);
        goto fail;
    }
    nbGL.has_image_dma_buf_export =
        epoxy_has_egl_extension(nbGL.display, "EGL_MESA_image_dma_buf_export");
    if (!nbGlamorCreateContext()) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: no GL or GLES 2 context on "
                       "%s: drawing in CPU memory\n", nbGL.device_path);
        goto fail;
    }
    renderer = (const char *) glGetString(GL_RENDERER);
    if (renderer == NULL || strstr(renderer, "softpipe") ||
        strncmp(renderer, "llvmpipe", 8) == 0) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: %s renders in software: "
                       "drawing in CPU memory\n",
                       renderer ? renderer : "the GL");
        goto fail;
    }
    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: %s has no GL_OES_EGL_image: "
                       "drawing in CPU memory\n", renderer);
        goto fail;
    }
    /* DRI3 1.2 modifiers: tiled buffers between clients and us. */
    nbGL.dmabuf_capable =
        epoxy_has_egl_extension(nbGL.display,
                                "EGL_EXT_image_dma_buf_import_modifiers") &&
        !(mods != NULL && strcmp(mods, "0") == 0);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    lastGLContext = NULL;

    if (!glamor_init(pScreen, GLAMOR_USE_EGL_SCREEN)) {
        LogMessageVerb(X_WARNING, 0, "Xnextbsd: glamor didn't start on %s: "
                       "drawing in CPU memory\n", renderer);
        goto fail;
    }
    nbGL.enabled = TRUE;

    /* Present for X clients: copy mode, with a fake vblank. */
    if (!present_screen_init(pScreen, NULL))
        ErrorF("Xnextbsd: can't set up Present\n");

    LogMessageVerb(X_INFO, 0, "Xnextbsd: glamor on %s (%s, GL %d.%d%s, "
                   "textures up to %d), %u GPU buffer formats from "
                   "WindowServer, DRI3%s\n", renderer, nbGL.device_path,
                   epoxy_gl_version() / 10, epoxy_gl_version() % 10,
                   epoxy_is_desktop_gl() ? "" : " ES", max_texture,
                   nbGL.ws_formats.count,
                   nbGL.dmabuf_capable ? " with modifiers" : "");
    return TRUE;

 fail:
    nbGlamorCleanup();
    return FALSE;
#endif
}

/* ---- frames: glamor pixmaps and GPU buffers ---- */

/* A frame's pixmap: GPU memory, cleared. */
PixmapPtr
nbGlamorCreateFramePixmap(ScreenPtr pScreen, int width, int height, int depth)
{
    PixmapPtr pixmap;

    pixmap = glamor_create_pixmap(pScreen, width > 0 ? width : 1,
                                  height > 0 ? height : 1, depth,
                                  GLAMOR_CREATE_NO_LARGE);
    if (pixmap == NULL)
        return NULL;
    glamor_clear_pixmap(pixmap);
    return pixmap;
}

void
nbGlamorDestroyPixmap(PixmapPtr pixmap)
{
    if (pixmap != NULL)
        dixDestroyPixmap(pixmap, 0);
}

#ifdef WS_SERVER_DMABUF
/*
 * A GPU buffer for WindowServer: a GBM buffer object of width x height
 * pixels in WindowServer's format `format`, with the modifiers
 * WindowServer composites (fastest first; Mesa picks among them), as a
 * glamor pixmap (*pixmap) to copy into, handed to WindowServer as a
 * dmabuf.
 */
ws_buffer_t *
nbGlamorBufferCreate(PixmapPtr *pixmap, unsigned int width,
                     unsigned int height, unsigned int scale, uint32_t format)
{
    ScreenPtr pScreen = screenInfo.screens[0];
    uint32_t gbm_format = format == WS_FORMAT_XRGB8888 ?
        GBM_FORMAT_XRGB8888 : GBM_FORMAT_ARGB8888;
    int depth = format == WS_FORMAT_XRGB8888 ? 24 : 32;
    uint64_t mods[WS_MAX_DMABUF_FORMATS];
    unsigned int i, n = 0;
    struct gbm_bo *bo = NULL;
    ws_buffer_t *buffer;
    PixmapPtr pix;
    int fd;

    *pixmap = NULL;
    for (i = 0; i < nbGL.ws_formats.count; i++)
        if (nbGL.ws_formats.formats[i].format == format)
            mods[n++] = nbGL.ws_formats.formats[i].modifier;
    if (n > 0)
        bo = gbm_bo_create_with_modifiers2(nbGL.gbm, width, height,
                                           gbm_format, mods, n,
                                           GBM_BO_USE_RENDERING);
    if (bo == NULL) {
        ErrorF("Xnextbsd: can't make a %ux%u GPU buffer: %s\n", width,
               height, strerror(errno));
        return NULL;
    }
    pix = pScreen->CreatePixmap(pScreen, 0, 0, depth, 0);
    if (pix == NULL) {
        gbm_bo_destroy(bo);
        return NULL;
    }
    pScreen->ModifyPixmapHeader(pix, width, height, 0, 0,
                                gbm_bo_get_stride(bo), NULL);
    if (!nbGlamorPixmapFromBO(pix, bo, TRUE)) {
        ErrorF("Xnextbsd: can't draw into a %ux%u GPU buffer\n", width,
               height);
        dixDestroyPixmap(pix, 0);
        gbm_bo_destroy(bo);
        return NULL;
    }
    fd = gbm_bo_get_fd(bo);
    buffer = fd < 0 ? NULL :
        ws_buffer_create_dmabuf(nbConnection(), width, height, scale, format,
                                fd, gbm_bo_get_offset(bo, 0),
                                gbm_bo_get_stride(bo),
                                gbm_bo_get_modifier(bo));
    if (buffer == NULL) {
        ErrorF("Xnextbsd: WindowServer didn't take a %ux%u GPU buffer: %s\n",
               width, height, strerror(errno));
        dixDestroyPixmap(pix, 0);
    } else {
        *pixmap = pix;
        if (nbGL.buffers++ == 0)
            LogMessageVerb(X_INFO, 0, "Xnextbsd: first GPU buffer for "
                           "WindowServer: %ux%u, format 0x%x, modifier "
                           "0x%016llx\n", width, height, gbm_format,
                           (unsigned long long) gbm_bo_get_modifier(bo));
    }
    if (fd >= 0)
        close(fd);
    gbm_bo_destroy(bo);
    return buffer;
}
#else
ws_buffer_t *
nbGlamorBufferCreate(PixmapPtr *pixmap, unsigned int width,
                     unsigned int height, unsigned int scale, uint32_t format)
{
    *pixmap = NULL;
    return NULL;
}
#endif

static PicturePtr
nbGlamorPicture(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    int depth = pixmap->drawable.depth;
    PictFormatPtr format;
    int error;

    format = PictureMatchFormat(pScreen, depth, depth == 32 ? PICT_a8r8g8b8 :
                                depth == 24 ? PICT_x8r8g8b8 : 0);
    if (format == NULL)
        return NULL;
    return CreatePicture(0, &pixmap->drawable, format, 0, NULL, serverClient,
                         &error);
}

/*
 * Bring a buffer up to date from a frame's pixmap on the GPU: `copy`
 * (frame-local, may reach into the buffer's padding past the frame's
 * edge, which takes the edge's pixels) is copied, opaque where the
 * frame has no alpha, and `clear` is made transparent. What lies inside
 * the frame, between pixmaps of one depth, is a plain glamor copy (one
 * draw for all its boxes); the padding, and a depth-24 frame going into
 * an ARGB buffer (a shaped window), go through RENDER, whose source
 * repeats its edge and gives depth 24 an opaque alpha.
 */
void
nbGlamorUpdateBuffer(PixmapPtr from, PixmapPtr to, RegionPtr copy,
                     RegionPtr clear)
{
    BoxRec inside = { 0, 0, from->drawable.width, from->drawable.height };
    PicturePtr src = NULL, dst = NULL;
    RegionRec plain, rest;
    const BoxRec *box;
    int i, n;

    RegionInit(&plain, &inside, 1);
    RegionIntersect(&plain, &plain, copy);
    RegionNull(&rest);
    RegionSubtract(&rest, copy, &plain);
    if (from->drawable.depth != to->drawable.depth) {
        RegionUnion(&rest, &rest, &plain);
        RegionEmpty(&plain);
    }
    if (RegionNotEmpty(&plain))
        miCopyRegion(&from->drawable, &to->drawable, NULL, &plain, 0, 0,
                     glamor_copy, 0, NULL);

    if (RegionNotEmpty(&rest) || RegionNotEmpty(clear)) {
        src = nbGlamorPicture(from);
        dst = nbGlamorPicture(to);
        if (src == NULL || dst == NULL)
            goto out;
    }
    if (RegionNotEmpty(&rest)) {
        src->repeat = TRUE;
        src->repeatType = RepeatPad;
        box = RegionRects(&rest);
        n = RegionNumRects(&rest);
        for (i = 0; i < n; i++)
            CompositePicture(PictOpSrc, src, NULL, dst, box[i].x1, box[i].y1,
                             0, 0, box[i].x1, box[i].y1,
                             box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
    }
    n = RegionNumRects(clear);
    if (n > 0) {
        xRenderColor transparent = { 0, 0, 0, 0 };
        xRectangle *rects = calloc(n, sizeof(*rects));

        box = RegionRects(clear);
        for (i = 0; rects != NULL && i < n; i++) {
            rects[i].x = box[i].x1;
            rects[i].y = box[i].y1;
            rects[i].width = box[i].x2 - box[i].x1;
            rects[i].height = box[i].y2 - box[i].y1;
        }
        if (rects != NULL)
            CompositeRects(PictOpSrc, dst, &transparent, n, rects);
        free(rects);
    }
 out:
    RegionUninit(&plain);
    RegionUninit(&rest);
    if (src != NULL)
        FreePicture(src, 0);
    if (dst != NULL)
        FreePicture(dst, 0);
}

/* Copy a w x h rectangle between two pixmaps of the same depth. */
void
nbGlamorCopyArea(PixmapPtr from, PixmapPtr to, int sx, int sy, int w, int h,
                 int dx, int dy)
{
    GCPtr gc;

    if (w <= 0 || h <= 0)
        return;
    gc = GetScratchGC(to->drawable.depth, to->drawable.pScreen);
    if (gc == NULL)
        return;
    ValidateGC(&to->drawable, gc);
    (void) gc->ops->CopyArea(&from->drawable, &to->drawable, gc, sx, sy, w, h,
                             dx, dy);
    FreeScratchGC(gc);
}

/*
 * Rootless's CopyWindow within a frame's pixmap: the boxes (frame-local)
 * take what is at (dx, dy) from them.
 */
void
nbGlamorCopyWithin(PixmapPtr pixmap, int nbox, const BoxRec *boxes, int dx,
                   int dy)
{
    RegionRec region;

    if (nbox <= 0)
        return;
    RegionInit(&region, NullBox, 0);
    for (int i = 0; i < nbox; i++) {
        RegionRec b;

        RegionInit(&b, (BoxPtr) &boxes[i], 1);
        RegionUnion(&region, &region, &b);
        RegionUninit(&b);
    }
    miCopyRegion(&pixmap->drawable, &pixmap->drawable, NULL, &region, dx, dy,
                 glamor_copy, 0, NULL);
    RegionUninit(&region);
}

/* Hand what was drawn to the GPU before WindowServer reads a buffer. */
void
nbGlamorFlush(void)
{
    ScreenPtr pScreen = screenInfo.screens[0];

    if (nbGL.enabled && pScreen != NULL)
        glamor_block_handler(pScreen);
}

#else /* !GLAMOR: built without glamor, fb only */

#include "nextbsd.h"

Bool nbGlamorInit(ScreenPtr pScreen) { return FALSE; }
Bool nbGlamorEnabled(void) { return FALSE; }
PixmapPtr nbGlamorCreateFramePixmap(ScreenPtr pScreen, int width, int height,
                                    int depth) { return NULL; }
void nbGlamorDestroyPixmap(PixmapPtr pixmap) { }
ws_buffer_t *nbGlamorBufferCreate(PixmapPtr *pixmap, unsigned int width,
                                  unsigned int height, unsigned int scale,
                                  uint32_t format) { *pixmap = NULL; return NULL; }
void nbGlamorUpdateBuffer(PixmapPtr from, PixmapPtr to, RegionPtr copy,
                          RegionPtr clear) { }
void nbGlamorCopyArea(PixmapPtr from, PixmapPtr to, int sx, int sy, int w,
                      int h, int dx, int dy) { }
void nbGlamorCopyWithin(PixmapPtr pixmap, int nbox, const BoxRec *boxes,
                        int dx, int dy) { }
void nbGlamorFlush(void) { }

#endif /* GLAMOR */
