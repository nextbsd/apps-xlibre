/*
 * kmsgrab OUT.ppm [scale]: as root, read the framebuffer the first active
 * CRTC on /dev/dri/card0 scans out (what is really on the screen, whatever
 * renderer made it), import it on the render node with its modifier, and
 * write it, downscaled by an integer, as PPM. WindowServer's snapshots
 * need its pixman renderer, and its captures and previews leave out GPU
 * buffers, so this is how Xnextbsd's GPU frames were checked on the Pi
 * (nextbsd/apps-xlibre#10). Reading a part of the screen fails on v3d
 * (Mesa can't map the imported buffer); crop the whole-screen PPM instead.
 * Build: cc -O2 -I/usr/include/libdrm -I/usr/X11/include -L/usr/X11/lib
 *   -Wl,-rpath,/usr/X11/lib -o kmsgrab kmsgrab.c -ldrm -lgbm -lEGL -lGLESv2
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: kmsgrab OUT.ppm [scale]\n"); return 2; }
	int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (card < 0) { perror("card0"); return 1; }
	drmModeResPtr res = drmModeGetResources(card);
	if (!res) { perror("GetResources"); return 1; }
	uint32_t fbid = 0;
	for (int i = 0; i < res->count_crtcs && !fbid; i++) {
		drmModeCrtcPtr c = drmModeGetCrtc(card, res->crtcs[i]);
		if (c && c->buffer_id) fbid = c->buffer_id;
		drmModeFreeCrtc(c);
	}
	if (!fbid) { fprintf(stderr, "no active crtc fb\n"); return 1; }
	drmModeFB2Ptr fb = drmModeGetFB2(card, fbid);
	if (!fb || !fb->handles[0]) { fprintf(stderr, "GetFB2: no handle (need root/master)\n"); return 1; }
	int dfd = -1;
	if (drmPrimeHandleToFD(card, fb->handles[0], DRM_CLOEXEC, &dfd)) { perror("PrimeHandleToFD"); return 1; }
	fprintf(stderr, "fb %u: %ux%u fourcc %.4s modifier 0x%llx pitch %u\n", fbid, fb->width, fb->height,
	    (char *)&fb->pixel_format, (unsigned long long)fb->modifier, fb->pitches[0]);
	int rn = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gbm_device *g = gbm_create_device(rn);
	PFNEGLGETPLATFORMDISPLAYEXTPROC gd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gd(EGL_PLATFORM_GBM_KHR, g, NULL);
	eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext x = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, x);
	EGLint ia[] = { EGL_WIDTH, fb->width, EGL_HEIGHT, fb->height, EGL_LINUX_DRM_FOURCC_EXT, fb->pixel_format,
		EGL_DMA_BUF_PLANE0_FD_EXT, dfd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, fb->offsets[0],
		EGL_DMA_BUF_PLANE0_PITCH_EXT, fb->pitches[0],
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(fb->modifier & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(fb->modifier >> 32), EGL_NONE };
	if (!(fb->flags & DRM_MODE_FB_MODIFIERS)) ia[14] = EGL_NONE;
	PFNEGLCREATEIMAGEKHRPROC ci = (void *)eglGetProcAddress("eglCreateImageKHR");
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC tt = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	EGLImageKHR img = ci(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
	if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "eglCreateImage failed 0x%x\n", eglGetError()); return 1; }
	GLuint t, f; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t); tt(GL_TEXTURE_2D, img);
	glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); return 1; }
	int X = 0, Y = 0, W = fb->width, H = fb->height, S = argc >= 3 ? atoi(argv[2]) : 1;
	if (S < 1) S = 1;
	unsigned char *p = malloc((size_t)W * H * 4);
	glReadPixels(X, Y, W, H, GL_RGBA, GL_UNSIGNED_BYTE, p);
	FILE *o = fopen(argv[1], "wb");
	fprintf(o, "P6\n%d %d\n255\n", W / S, H / S);
	for (int y = 0; y < H / S; y++) for (int xx = 0; xx < W / S; xx++) {
		unsigned char *q = p + ((size_t)(y * S) * W + xx * S) * 4;
		/* glReadPixels(GL_RGBA) gives logical R G B A whatever the fourcc */
		unsigned char rgb[3] = { q[0], q[1], q[2] };
		fwrite(rgb, 1, 3, o);
	}
	fclose(o);
	fprintf(stderr, "wrote %s %dx%d\n", argv[1], W / S, H / S);
	return 0;
}
