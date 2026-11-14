/*
 * glprobe [egl|glx] [frames] [hold]: on $DISPLAY, make a window (W x H,
 * default 500 x 400), render with EGL (EGL_PLATFORM_X11: DRI3 + Present)
 * or GLX, print the renderer and whether rendering is direct, swap
 * `frames` frames of changing colours, report the frame rate, and leave
 * the last frame (green) up for `hold` seconds (default 20).
 * Build: cc -O2 -I/usr/X11/include -L/usr/X11/lib -Wl,-rpath,/usr/X11/lib
 *   -o glprobe glprobe.c -lX11 -lEGL -lGL
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glx.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "egl";
	int frames = argc > 2 ? atoi(argv[2]) : 300;
	int hold = argc > 3 ? atoi(argv[3]) : 20;
	Display *d = XOpenDisplay(NULL);
	if (!d) { printf("no display\n"); return 1; }
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 80, 80,
	    getenv("W") ? atoi(getenv("W")) : 500, getenv("H") ? atoi(getenv("H")) : 400, 0, 0, 0);
	XStoreName(d, w, mode);
	XMapWindow(d, w); XSync(d, False);
	double t0;
	if (strcmp(mode, "glx") == 0) {
		int att[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
		XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), att);
		if (!vi) { printf("glXChooseVisual failed\n"); return 1; }
		GLXContext c = glXCreateContext(d, vi, NULL, True);
		if (!c) { printf("glXCreateContext failed\n"); return 1; }
		glXMakeCurrent(d, w, c);
		printf("GLX vendor: %s\nGL_RENDERER: %s\nGL_VERSION: %s\ndirect: %s\n", glXGetClientString(d, GLX_VENDOR),
		    glGetString(GL_RENDERER), glGetString(GL_VERSION), glXIsDirect(d, c) ? "yes" : "no");
		fflush(stdout);
		t0 = now();
		for (int i = 0; i < frames; i++) { glClearColor((i % 60) / 60.0, 0, 1 - (i % 60) / 60.0, 1); glClear(GL_COLOR_BUFFER_BIT); glXSwapBuffers(d, w); }
		glFinish();
		printf("%d frames in %.2f s: %.1f fps\n", frames, now() - t0, frames / (now() - t0));
		glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glXSwapBuffers(d, w); glFinish();
	} else {
		PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
		EGLDisplay ed = gpd(EGL_PLATFORM_X11_KHR, d, NULL);
		EGLint maj, min;
		if (!eglInitialize(ed, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
		printf("EGL %d.%d vendor %s\n", maj, min, eglQueryString(ed, EGL_VENDOR));
		const char *drv = NULL;
		PFNEGLGETDISPLAYDRIVERNAMEPROC gdn = (void *)eglGetProcAddress("eglGetDisplayDriverName");
		if (gdn) drv = gdn(ed);
		printf("EGL driver: %s\n", drv ? drv : "?");
		EGLint a[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
		EGLConfig cfg; EGLint n;
		eglChooseConfig(ed, a, &cfg, 1, &n);
		eglBindAPI(EGL_OPENGL_ES_API);
		EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		EGLContext c = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ca);
		EGLSurface s = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)w, NULL);
		if (s == EGL_NO_SURFACE) { printf("eglCreateWindowSurface failed 0x%x\n", eglGetError()); return 1; }
		eglMakeCurrent(ed, s, s, c);
		printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
		fflush(stdout);
		t0 = now();
		for (int i = 0; i < frames; i++) { glClearColor((i % 60) / 60.0, 0, 1 - (i % 60) / 60.0, 1); glClear(GL_COLOR_BUFFER_BIT); eglSwapBuffers(ed, s); }
		glFinish();
		printf("%d frames in %.2f s: %.1f fps\n", frames, now() - t0, frames / (now() - t0));
		glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); eglSwapBuffers(ed, s);
	}
	fflush(stdout);
	sleep(hold);
	return 0;
}
