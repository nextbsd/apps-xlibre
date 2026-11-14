/*
 * drmetime.so: LD_PRELOAD shim for Linux GL programs under NextBSD's
 * Linuxulator. Linux DRM drivers report a wait that timed out with ETIME
 * (62); NextBSD's drm returns ETIMEDOUT, which the Linuxulator passes on
 * as Linux ETIMEDOUT (110), and Linux Mesa's v3d driver aborts on it
 * ("wait failed: -110") the first time it polls a busy buffer. This
 * ioctl() turns ETIMEDOUT from DRM ioctls back into ETIME and is
 * otherwise the plain syscall. Freestanding: no libc headers, the only
 * libc symbol it uses is __errno_location.
 * Build (on NextBSD): cc --target=aarch64-unknown-linux-gnu -O2 -fPIC
 *   -shared -nostdlib -ffreestanding -fuse-ld=lld -o drmetime.so drmetime.c
 */
extern int *__errno_location(void);

#define LINUX_ETIME     62
#define LINUX_ETIMEDOUT 110
#define DRM_IOCTL_TYPE  'd'

static long
sys_ioctl(long fd, unsigned long req, unsigned long arg)
{
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long) req;
	register long x2 __asm__("x2") = (long) arg;
	register long x8 __asm__("x8") = 29;	/* __NR_ioctl, aarch64 */

	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)
	    : "memory", "cc");
	return x0;
}

__attribute__((visibility("default"))) int
ioctl(int fd, unsigned long req, ...)
{
	__builtin_va_list ap;
	unsigned long arg;
	long r;

	__builtin_va_start(ap, req);
	arg = __builtin_va_arg(ap, unsigned long);
	__builtin_va_end(ap);
	r = sys_ioctl(fd, req, arg);
	if (r < 0 && r > -4096) {
		int e = (int) -r;

		if (e == LINUX_ETIMEDOUT && ((req >> 8) & 0xff) == DRM_IOCTL_TYPE)
			e = LINUX_ETIME;
		*__errno_location() = e;
		return -1;
	}
	return (int) r;
}
