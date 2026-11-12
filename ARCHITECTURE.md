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
