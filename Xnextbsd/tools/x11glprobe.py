# x11glprobe.py [egl|glx]: a Linux GL client on $DISPLAY (python ctypes,
# no compiler needed): EGL_PLATFORM_X11 (DRI3 + Present, 60 swaps) or GLX
# (glXCreateContextAttribsARB as ANGLE does), print the renderer and
# whether rendering is direct. Run under the Linuxulator:
#   DISPLAY=:0 /compat/linux/usr/bin/python3 x11glprobe.py egl
import ctypes as C, sys, os
mode = sys.argv[1] if len(sys.argv) > 1 else "egl"
x11 = C.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = C.c_void_p; x11.XOpenDisplay.argtypes = [C.c_char_p]
x11.XDefaultRootWindow.restype = C.c_ulong; x11.XDefaultRootWindow.argtypes = [C.c_void_p]
x11.XCreateSimpleWindow.restype = C.c_ulong
x11.XCreateSimpleWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]
x11.XMapWindow.argtypes = [C.c_void_p, C.c_ulong]; x11.XSync.argtypes = [C.c_void_p, C.c_int]
d = x11.XOpenDisplay(None)
if not d: print("no display"); sys.exit(1)
w = x11.XCreateSimpleWindow(d, x11.XDefaultRootWindow(d), 200, 200, 300, 200, 0, 0, 0)
x11.XMapWindow(d, w); x11.XSync(d, 0)
if mode == "egl":
    egl = C.CDLL("libEGL.so.1"); gles = C.CDLL("libGLESv2.so.2")
    egl.eglGetProcAddress.restype = C.c_void_p; egl.eglGetProcAddress.argtypes = [C.c_char_p]
    gpd = C.CFUNCTYPE(C.c_void_p, C.c_uint, C.c_void_p, C.c_void_p)(egl.eglGetProcAddress(b"eglGetPlatformDisplayEXT"))
    ed = gpd(0x31D5, d, None)
    egl.eglInitialize.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]; egl.eglGetError.restype = C.c_int
    if not egl.eglInitialize(ed, None, None): print("eglInitialize failed 0x%x" % egl.eglGetError()); sys.exit(1)
    egl.eglQueryString.restype = C.c_char_p; egl.eglQueryString.argtypes = [C.c_void_p, C.c_int]
    print("EGL vendor", egl.eglQueryString(ed, 0x3053))
    gdn = egl.eglGetProcAddress(b"eglGetDisplayDriverName")
    if gdn: print("EGL driver", C.CFUNCTYPE(C.c_char_p, C.c_void_p)(gdn)(ed))
    cfg = C.c_void_p(); n = C.c_int()
    egl.eglChooseConfig.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_int, C.c_void_p]
    egl.eglChooseConfig(ed, (C.c_int * 11)(0x3033, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3040, 4, 0x3038), C.byref(cfg), 1, C.byref(n))
    egl.eglBindAPI(0x30A0)
    egl.eglCreateContext.restype = C.c_void_p; egl.eglCreateContext.argtypes = [C.c_void_p] * 4
    ctx = egl.eglCreateContext(ed, cfg, None, (C.c_int * 3)(0x3098, 2, 0x3038))
    egl.eglCreateWindowSurface.restype = C.c_void_p; egl.eglCreateWindowSurface.argtypes = [C.c_void_p, C.c_void_p, C.c_ulong, C.c_void_p]
    s = egl.eglCreateWindowSurface(ed, cfg, w, None)
    egl.eglMakeCurrent.argtypes = [C.c_void_p] * 4
    egl.eglMakeCurrent(ed, s, s, ctx)
    gles.glGetString.restype = C.c_char_p
    print("GL_RENDERER", gles.glGetString(0x1F01), gles.glGetString(0x1F02))
    egl.eglSwapBuffers.argtypes = [C.c_void_p, C.c_void_p]
    for i in range(60):
        gles.glClearColor(C.c_float(0), C.c_float(i / 60.0), C.c_float(0), C.c_float(1)); gles.glClear(0x4000); egl.eglSwapBuffers(ed, s)
    print("60 swaps ok")
else:
    gl = C.CDLL("libGL.so.1")
    gl.glXGetProcAddressARB.restype = C.c_void_p; gl.glXGetProcAddressARB.argtypes = [C.c_char_p]
    gl.glXChooseFBConfig.restype = C.POINTER(C.c_void_p)
    gl.glXChooseFBConfig.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.POINTER(C.c_int)]
    n = C.c_int()
    # what ANGLE asks for: RGBA, window and pbuffer, X renderable, double-buffered
    for name, att in (("window|pbuffer", (0x8011, 1, 0x8010, 1 | 4, 0x8012, 1, 5, 1, 0)), ("window", (0x8011, 1, 0x8010, 1, 0x8012, 1, 5, 1, 0))):
        cf = gl.glXChooseFBConfig(d, 0, (C.c_int * len(att))(*att), C.byref(n))
        print("fbconfigs for", name, ":", n.value)
    cf = gl.glXChooseFBConfig(d, 0, (C.c_int * 9)(0x8011, 1, 0x8010, 1, 0x8012, 1, 5, 1, 0), C.byref(n))
    cca = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p, C.c_int, C.c_void_p)(gl.glXGetProcAddressARB(b"glXCreateContextAttribsARB"))
    for name, att in (("core 3.1", (0x2091, 3, 0x2092, 1, 0x9126, 1, 0)), ("es 3.0", (0x2091, 3, 0x2092, 0, 0x9126, 4, 0)), ("es 2.0", (0x2091, 2, 0x2092, 0, 0x9126, 4, 0)), ("default", (0,))):
        ctx = cca(d, cf[0], None, 1, (C.c_int * len(att))(*att))
        print("glXCreateContextAttribsARB", name, "->", "ok" if ctx else "FAILED")
        if ctx and name == "default":
            gl.glXMakeCurrent.argtypes = [C.c_void_p, C.c_ulong, C.c_void_p]
            gl.glXMakeCurrent(d, w, ctx)
            gl.glGetString.restype = C.c_char_p
            gl.glXIsDirect.argtypes = [C.c_void_p, C.c_void_p]
            print("GL_RENDERER", gl.glGetString(0x1F01), gl.glGetString(0x1F02), "direct", gl.glXIsDirect(d, ctx))
