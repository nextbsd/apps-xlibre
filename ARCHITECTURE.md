# XLibre.app Architecture

## Overview

XLibre.app runs X11 programs on NextBSD's WindowServer, as XQuartz does on
macOS (plan: the [XLibre-on-WindowServer-Plan](https://github.com/nextbsd/apps-xlibre/wiki/XLibre-on-WindowServer-Plan)
wiki page). It is a GNUstep app on the `nextbsd` backend that starts
`Xnextbsd`, the rootless X server in `Xnextbsd/`, and runs the X window
manager and the clipboard bridge in its own process. It grew out of the
Gershwin WindowManager.app, which it replaces: the window manager, its
GSTheme title bars and the XCB layer are that app's, without its
compositor, window switcher, keyboard grabs, snapping and Gershwin menu.

```
 WindowServer ── composites ──┬── XLibre.app windows (menus, panels)
                              └── Xnextbsd windows (one per top-level X window)
   XLibre.app ── XCB ── Xnextbsd ── X11 ── xterm, ...
       ├── window manager: reparents X windows, draws NextBSD title bars
       └── clipboard bridge: CLIPBOARD <-> the general pasteboard,
                             PRIMARY <-> the "Selection" pasteboard (gpbs)
```

## Directory Layout

```
XLibre/
├── main.m                          Entry point, signal handling
├── XLibreController.h/m            App delegate: menus, launch, Hide, Quit
├── XLServer.h/m                    The Xnextbsd child: cookie, -displayfd, join
├── XLPasteboardBridge.h/m          X selections <-> GNUstep pasteboards
├── XLScaleSettings.h/m             HiDPI: Xft.dpi, XSETTINGS, toolkit env
├── XLibre.png, XLibreInfo.plist    Icon and Info.plist
├── GNUmakefile                     Builds and installs XLibre.app
│
├── xcb/                            XCB abstraction layer
│   ├── XCBConnection.h/m           X11 connection, event dispatch, window map
│   ├── XCBWindow.h/m               Base window abstraction
│   ├── XCBFrame.h/m                Reparenting frame (borders, resize zones)
│   ├── XCBTitleBar.h/m             Titlebar window (pixmap, buttons, drag)
│   ├── XCBScreen.h/m               Screen geometry and root window
│   ├── XCBVisual.h/m               Visual/depth selection
│   ├── XCBCursor.h/m               Cursor management
│   ├── XCBSelection.h/m            X11 selection ownership (WM_Sn)
│   ├── XCBRegion.h/m               XFixes region wrapper
│   ├── XCBShape.h/m                X11 Shape extension (rounded corners)
│   ├── XCBReply.h/m                Base class for XCB reply wrappers
│   ├── XCBAttributesReply.h/m      Window attributes reply
│   ├── XCBGeometryReply.h/m        Window geometry reply
│   ├── XCBQueryTreeReply.h/m       Query tree reply
│   ├── XCBTypes.h                  Geometry primitives (XCBPoint, XCBSize, XCBRect, XCBColor)
│   │
│   ├── enums/                      Enumeration constants
│   │   ├── EEwmh.h                 EWMH property identifiers
│   │   ├── EIcccm.h                ICCCM protocol constants
│   │   ├── EMousePosition.h        Mouse position on frame edges
│   │   ├── EResizeDirection.h      Resize direction flags
│   │   └── EXErrorMessages.h       X protocol error strings
│   │
│   ├── services/                   X11 protocol services
│   │   ├── EWMHService.h/m         Extended Window Manager Hints
│   │   ├── ICCCMService.h/m        Inter-Client Communication Conventions
│   │   └── XCBAtomService.h/m      Atom interning and lookup
│   │
│   └── utils/                      Utility classes and functions
│       ├── XCBCreateWindowTypeRequest.h/m  Window creation request builder
│       ├── XCBWindowTypeResponse.h/m       Window creation response
│       ├── Comparators.h/m         Color and struct comparison helpers
│       └── Transformers.h/m        Coordinate and struct transformation helpers
│
├── URSHybridEventHandler.h/m       Window manager: XCB events -> managers
├── URSFocusManager.h/m             Focus tracking and window resolution
├── URSWorkareaManager.h/m          EWMH strut tracking and workarea calculation
├── URSTitlebarController.h/m       Titlebar button hit-test, hover, resize
├── URSDecorationMetrics.h/m        Title bar and button geometry from the theme
└── URSThemeIntegration.h/m         GSTheme titlebar decoration bridge
```

## How the pieces talk

- **XLibre.app and WindowServer:** the `nextbsd` GNUstep backend. XLibre.app
  gets its WindowServer client id and token from
  `-[NextBSDServer windowServerAppCredentials]` (by selector) and hands them
  to `Xnextbsd` in its environment (`XNEXTBSD_JOIN`), so both connections are
  one WindowServer app: clicking an X window keeps XLibre's menu, and
  Command shortcuts typed in an X window reach XLibre's menu.
- **XLibre.app and Xnextbsd:** X11 over XCB, on two connections: the window
  manager's and the clipboard bridge's. The window manager's channel to
  the server is the root window property `_XNEXTBSD_HIDDEN` (1 when XLibre
  is hidden: Xnextbsd orders every frame out on WindowServer and leaves the
  X windows mapped). Focus follows WindowServer's key window inside
  Xnextbsd, and override-redirect windows get the popup level there.
  The other way, Xnextbsd puts WindowServer's usable area (the screen
  minus the Dock's strip) on the root window as `_XNEXTBSD_USABLE_AREA`,
  and the window manager keeps `_NET_WORKAREA`, and so new windows, inside
  it.
- **Minimizing:** the title bar's minimize button, or an X client's own
  iconify (`XIconifyWindow`, `WM_CHANGE_STATE` IconicState), minimizes the
  window through WindowServer, as GNUstep windows are: the window manager
  sets `WM_STATE` IconicState and `_NET_WM_STATE_HIDDEN` on the client and
  `_XNEXTBSD_MINIMIZED` on its frame (where: into the Dock when the
  user's `NBMinimizeStyle` is `dock`, the default, else WindowServer's
  bottom row), and Xnextbsd minimizes the frame with
  `ws_window_minimize_where()`. The X windows stay mapped, so the Dock's
  preview of the window is live; the Dock shows it with XLibre's icon,
  since Xnextbsd's connection is part of XLibre's app. Clicking the
  preview (or `WS_EVENT_WINDOW_RESTORE` from the overview) makes Xnextbsd
  show the window again, delete `_XNEXTBSD_MINIMIZED` and send the window
  manager a `_XNEXTBSD_RESTORE` ClientMessage, which puts `WM_STATE` back to
  NormalState, raises and focuses the window. `_NET_ACTIVE_WINDOW`, a
  `WM_CHANGE_STATE` NormalState or mapping the client restores it too, and
  makes XLibre active.
- **Window tiling:** when WindowServer tiles (`WindowMode` `tiling`), it
  gives X windows new frames, sizes included. Xnextbsd resizes the frame
  to them within the client's `WM_NORMAL_HINTS` (minimum, maximum, resize
  increments: an xterm fills its tile but for less than a character
  cell), counting the window manager's `_NET_FRAME_EXTENTS` (the title
  bar), and the window manager lays the frame out again around the
  client, which gets its ConfigureNotify and redraws. Xnextbsd tells
  WindowServer each window's size limits and flags: `WS_WINDOW_PANEL` for
  transient windows, dialogs, menus and override-redirect windows, and
  `WS_WINDOW_FIXED_SIZE` for windows that can't be resized, so those float
  above the tiles. WindowServer also gets each window's title (the
  client's name), which the Dock shows.
- **Screen changes:** when WindowServer's screen changes size, Xnextbsd
  resizes the root window and tells X clients through RandR (one output,
  the current mode; X clients can't set another). WindowServer moves the
  windows that no longer fit just onto the screen, and Xnextbsd moves the
  X windows with them; the window manager then moves those frames into
  the usable area.
- **Screen sharing:** X11 capture tools (Firefox's WebRTC, so Jitsi Meet
  sharing "Entire screen"; recorders; `xwd -root`) read the root window with
  `GetImage` or MIT-SHM's `ShmGetImage`, and may follow it with DAMAGE.
  Xnextbsd is rootless, so nothing paints the root, and GNUstep apps, the
  Dock and the desktop aren't in X at all. Xnextbsd (`nbCapture.c`) answers
  reads of the root from WindowServer's screen capture instead
  (apps-windowserver#49, "Screen capture" in `WSProtocol.h`): the real
  screen, every app on it. It asks for a stream on behalf of the X client
  that reads (`ws_capture_start_for` with the client's pid and command
  name), so WindowServer's Screen Recording prompt names that program
  ("firefox-esr"), not XLibre, and the user's answer is that program's.
  Until the user allows it, and after a refusal or a Stop, the client reads
  black. A stream runs only while a client reads the root (stopped after
  15 s without a read) or has a Damage object on it, and its frames'
  changed rectangles become DAMAGE events on the root. Frames leave the
  pointer out, as X11 tools draw it from XFixes. Captures of one X window
  (Composite) are untouched. MIT-SHM has no shared pixmaps in Xnextbsd, so
  WebRTC reads with `ShmGetImage` rather than copying the root.
- **HiDPI:** WindowServer lays the screen out in points, with a backing
  scale of 1, 2 or 3 pixels per point (`WS_SERVER_SCALE`, "HiDPI" in
  WindowServer's `WSProtocol.h`). Xnextbsd draws at that scale: its X
  screen is the screen's size in points times the scale (a 3840 x 2160
  panel that looks like 1920 x 1080 is a 3840 x 2160 X screen), each X
  window's WindowServer frame is its X geometry divided by the scale
  (rounded out to whole points; the buffer is padded with the window's
  last column and row), its buffers carry the scale, pointer positions are
  multiplied by it, and the usable area, RandR and the root window are in
  pixels. RENDER cursors (themed, at `Xcursor.size`) carry the scale too;
  core bitmap cursors are 1x and WindowServer scales them up. Xnextbsd puts
  the scale on the root window as `_XNEXTBSD_SCALE`, and XLibre.app
  (`XLScaleSettings`) tells X programs: `Xft.dpi` and `Xcursor.size` in
  `RESOURCE_MANAGER` (and an Xft face for xterm, `XTerm*faceName:
  monospace` at 10 points, at 2x and more), XSETTINGS (`Gdk/WindowScalingFactor`, `Gdk/UnscaledDPI`,
  `Xft/DPI`; XLibre.app owns `_XSETTINGS_S0`), and for the programs it
  starts `GDK_SCALE`, `GDK_DPI_SCALE`, `QT_SCREEN_SCALE_FACTORS`,
  `QT_ENABLE_HIGHDPI_SCALING` and `XCURSOR_SIZE`, chosen so that GTK,
  Firefox, Qt and Xft programs come out exactly the scale's size, never
  twice it (the reasoning is in `XLScaleSettings.m`). The window manager's
  title bars and borders are drawn at the scale too. When the user picks
  another "looks like" size the scale can change: X windows keep their
  pixels, get new frames and buffers, title bars are laid out again and
  the settings follow. The `XLibreLowResolution` default (Preferences:
  "Open in low resolution", off by default) starts Xnextbsd with
  `-lowres`: X draws at 1x, as XQuartz's programs do on a Retina display,
  and WindowServer scales X windows up; everything above then says 1x. In
  WindowServer's scaled "looks like" modes, WindowServer scales X windows
  to the panel with everything else, and the root window's screen capture
  is asked for at the X screen's size.
- **The GPU (nextbsd/apps-xlibre#10):** when WindowServer takes GPU
  buffers (`WS_SERVER_DMABUF`, "GPU buffers" in its `WSProtocol.h`) and EGL
  works on the render node it names (`/dev/dri/renderD128`, v3d, on a
  Raspberry Pi 5), Xnextbsd draws on the GPU (`nbGlamor.c`). Glamor, on an
  EGL context on GBM, does X rendering. Each top-level window's backing is
  a glamor pixmap, which rootless draws into through a `GetPixmap` frame
  proc (`patches/xlibre-xserver/0002`) instead of CPU memory. Its two
  WindowServer buffers are GBM buffer objects with a modifier WindowServer
  listed (Broadcom UIF on the Pi), handed over as dmabufs. A commit copies
  the damage into a free one on the GPU, flushes and attaches it, with the
  same `BUFFER_RELEASE`, frame pacing, resizes and HiDPI scale as shared
  memory, and no CPU copy.
  X clients get DRI3 (with modifiers), Present (copy mode, timed by a fake
  vblank) and GLX (glamor's EGL provider; clients render directly with
  DRI3), so GL and EGL programs render on the GPU. Linux Chromium's GPU
  process, for one, gets ANGLE on OpenGL ES on V3D, GPU compositing and
  WebGL.
  - **Chromium:** run it with `--use-angle=gles-egl`. ANGLE's default on
    X11 is GLX, which presents the whole window every frame, four
    full-window GPU passes in all, and is slower for video than software.
    With Mesa's EGL on X11, only what changed is presented, and 720p60
    video plays at 60 fps.
  - **GTK 3 and GL:** XLibre.app sets `GDK_GL=disable` for its X programs,
    so that GTK 3 doesn't load Mesa just to pick visuals. That made Linux
    Chromium's browser process crash at startup.
  - **Fallback:** without GPU buffers, a render node or a usable EGL, or
    with `-nogpu` or `XNEXTBSD_GPU=0` in Xnextbsd's environment, it is fb
    and shared memory as before, with no DRI3 or GLX.
  - **Not readable by WindowServer:** WindowServer can't read GPU buffers
    on the CPU, so X windows drawn in them are missing from its captures
    and the Dock's previews.
  - **Linux programs** need `Xnextbsd/tools/drmetime.c` preloaded until
    nextbsd/nextbsd-kernel#424 is fixed. The Linuxulator hands DRM wait
    timeouts to Linux Mesa as `ETIMEDOUT` rather than `ETIME`, and Linux
    Mesa's v3d driver aborts on it.
- **The clipboard bridge and gpbs:** NSPasteboard. The bridge owns
  CLIPBOARD for the general pasteboard and PRIMARY for the "Selection"
  pasteboard, as xpbs does, and copies new X owners' data to them: text
  (UTF8_STRING or STRING), RTF and PNG/TIFF images, with TARGETS and
  INCR. xterm's selections also go to CLIPBOARD (XTerm*selectToClipboard)
  unless XLibreSelectToClipboard is turned off.

## Layers

**Application** — `main.m`, `XLibreController`, `XLServer`
Starts the server with a fresh MIT-MAGIC-COOKIE-1 (xauth), sets `DISPLAY`
and `XAUTHORITY` for itself and the X programs it starts (not for anything
else: Terminal gets no `DISPLAY`), builds the menus, opens the
`XLibreAppToRun` program, and asks before quitting while X programs run.

**Window manager** — `URSHybridEventHandler` and the managers
Bridges the XCB file descriptor into the NSRunLoop, reparents X windows
into frames and dispatches events to the focus, workarea and title bar
managers. Each frame is one WindowServer window, so WindowServer
composites, stacks and moves the title bar with the window.

**Theme Layer** — `URSThemeIntegration`, `URSDecorationMetrics`
Draws title bars with the active GSTheme into a bitmap that is put into
the frame's title bar window. Nothing here needs the X11 GNUstep backend.

**XCB Abstraction Layer** — `xcb/`
Objective-C wrapper around libxcb. Provides object-oriented access to
X11 windows, screens, cursors, selections and EWMH/ICCCM services. No
window management policy, only mechanism.

## Build

```sh
gmake                                   # XLibre.app
gmake install XNEXTBSD=/path/Xnextbsd   # ~/Applications/XLibre.app, server inside
```

NextBSD has no meson or ninja, so Xnextbsd is built by CI
(`.github/workflows/xnextbsd.yml`) and published on every build of master to
the rolling `xnextbsd-latest` release, as `Xnextbsd-amd64` and
`Xnextbsd-arm64` (named by `uname -m`) with `.sha256` files and `SHA256SUMS`:

```sh
a=$(uname -m)
fetch https://github.com/nextbsd/apps-xlibre/releases/download/xnextbsd-latest/Xnextbsd-$a
```

## Key Design Decisions

1. **No separate framework.** XCBKit was merged into the application to
   eliminate the two-stage build, simplify deployment, and remove circular
   dependency potential. The XCB abstraction layer remains logically
   separated via the `xcb/` directory.

2. **Geometry types in XCBTypes.h.** The geometry primitives (`XCBPoint`,
   `XCBSize`, `XCBRect`, `XCBColor`) live in `xcb/XCBTypes.h` — distinct
   from `XCBShape.h/m` which wraps the X11 Shape extension class.

3. **Include paths over path prefixes.** The GNUmakefile adds `-Ixcb`,
   `-Ixcb/services`, `-Ixcb/enums`, `-Ixcb/utils` so all headers can be
   imported by name alone: `#import "XCBConnection.h"`,
   `#import "EWMHService.h"`. No `../` or `<XCBKit/...>` paths.

4. **Dead code removed.** The following unused files were deleted during
   the merge: `XCBKit.h/m` (empty class), `XCBEvent.h/m` (unused wrapper),
   `ERequests.h` (unused enum), `Client.h`/`Server.h` (unused protocols),
   `UROSTitleBar.h/m` (superseded), `UROSWindowDecorator.h/m` (superseded),
   `URSTitlebarTheming.h` (superseded), `URSCompositingManager.m.backup`.
