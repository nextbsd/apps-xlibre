/* xmover: a window that draws a pattern, then moves, resizes (bigger, smaller,
   odd sizes) and scrolls its contents (XCopyArea within the window), checking
   with GetImage after each step. Prints PASS/FAIL per step. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>
static Display *d; static Window w; static GC gc;
static void paint(int W, int H) {
	XSetForeground(d, gc, 0x2040c0); XFillRectangle(d, w, gc, 0, 0, W, H);
	XSetForeground(d, gc, 0xffff00); XFillRectangle(d, w, gc, 10, 10, 40, 40);
	XSetForeground(d, gc, 0xff0000); XFillRectangle(d, w, gc, W - 30, H - 30, 20, 20);
	XSync(d, False);
}
static int check(const char *what, int x, int y, unsigned long want) {
	XImage *im = XGetImage(d, w, x, y, 1, 1, AllPlanes, ZPixmap);
	unsigned long p = XGetPixel(im, 0, 0) & 0xffffff; XDestroyImage(im);
	printf("%-28s (%d,%d)=%06lx want %06lx %s\n", what, x, y, p, want, p == want ? "PASS" : "FAIL");
	return p == want;
}
static void waitexpose(void) { XEvent e; usleep(150000); while (XPending(d)) XNextEvent(d, &e); }
int main(void) {
	d = XOpenDisplay(NULL); if (!d) return 1;
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 2400, 300, 400, 300, 0, 0, 0);
	XStoreName(d, w, "xmover"); XSelectInput(d, w, ExposureMask | StructureNotifyMask);
	XMapWindow(d, w); XEvent e; do XNextEvent(d, &e); while (e.type != Expose);
	gc = XCreateGC(d, w, 0, NULL);
	int ok = 1;
	paint(400, 300); ok &= check("initial yellow", 20, 20, 0xffff00); ok &= check("initial red", 380, 280, 0xff0000);
	XMoveWindow(d, w, 2200, 600); XSync(d, False); waitexpose();
	ok &= check("after move", 20, 20, 0xffff00);
	int sizes[][2] = { {801, 603}, {257, 131}, {1203, 777}, {400, 300} };
	for (int i = 0; i < 4; i++) {
		XResizeWindow(d, w, sizes[i][0], sizes[i][1]); XSync(d, False); waitexpose();
		paint(sizes[i][0], sizes[i][1]);
		char b[64]; snprintf(b, sizeof b, "resize %dx%d", sizes[i][0], sizes[i][1]);
		ok &= check(b, 20, 20, 0xffff00);
		ok &= check(b, sizes[i][0] - 20, sizes[i][1] - 20, 0xff0000);
	}
	/* scroll: copy the yellow square 100 px right within the window */
	XCopyArea(d, w, w, gc, 0, 0, 200, 150, 100, 0); XSync(d, False);
	ok &= check("scroll (CopyArea in window)", 120, 20, 0xffff00);
	printf("%s\n", ok ? "ALL PASS" : "SOME FAIL"); fflush(stdout);
	sleep(25);
	return 0;
}
