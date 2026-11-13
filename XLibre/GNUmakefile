# XLibre.app: X11 programs on NextBSD's WindowServer (nextbsd/nextbsd#625).
#
#   gmake                      build XLibre.app
#   gmake install              install it into the GNUstep domain named by
#                              GNUSTEP_INSTALLATION_DOMAIN (USER, so
#                              ~/Applications, unless set), with Xnextbsd
#                              in its Resources when there is one: the
#                              XNEXTBSD= binary (a CI artifact), else
#                              ../Xnextbsd/.build/stage/Xnextbsd if built
#   gmake install XNEXTBSD=/path/to/Xnextbsd
#
# Nothing goes in a system path and nothing needs root.

GNUSTEP_INSTALLATION_DOMAIN ?= USER

PACKAGE_NAME = XLibre

include $(GNUSTEP_MAKEFILES)/common.make

VERSION = 0.2.0

APP_NAME = XLibre
$(APP_NAME)_APPLICATION_ICON = XLibre.png
$(APP_NAME)_RESOURCE_FILES = XLibre.png
export APP_NAME

# Include paths for the xcb abstraction layer
$(APP_NAME)_INCLUDE_DIRS = -Ixcb -Ixcb/services -Ixcb/enums -Ixcb/utils

$(APP_NAME)_OBJC_FILES = \
		main.m \
		XLibreController.m \
		XLServer.m \
		XLPasteboardBridge.m \
		XLScaleSettings.m \
		URSHybridEventHandler.m \
		URSFocusManager.m \
		URSWorkareaManager.m \
		URSTitlebarController.m \
		URSThemeIntegration.m \
		URSProfiler.m \
		URSDecorationMetrics.m \
		xcb/XCBConnection.m \
		xcb/XCBWindow.m \
		xcb/XCBFrame.m \
		xcb/XCBTitleBar.m \
		xcb/XCBScreen.m \
		xcb/XCBVisual.m \
		xcb/XCBCursor.m \
		xcb/XCBSelection.m \
		xcb/XCBRegion.m \
		xcb/XCBShape.m \
		xcb/XCBReply.m \
		xcb/XCBAttributesReply.m \
		xcb/XCBGeometryReply.m \
		xcb/XCBQueryTreeReply.m \
		xcb/services/EWMHService.m \
		xcb/services/ICCCMService.m \
		xcb/services/XCBAtomService.m \
		xcb/utils/XCBCreateWindowTypeRequest.m \
		xcb/utils/XCBWindowTypeResponse.m \
		xcb/utils/Comparators.m \
		xcb/utils/Transformers.m

$(APP_NAME)_GUI_LIBS = \
		$(shell pkg-config --libs xcb xcb-icccm xcb-xfixes xcb-aux xcb-cursor xcb-shape xcb-randr) \
		-lxcb-util -ldispatch

### To avoid a -ldispatch path issue with clang on Debian
ifeq ($(shell uname -s),Linux)
ifeq ($(shell lsb_release -is 2>/dev/null),Debian)
	CPPFLAGS += -I/usr/local/include
	LDFLAGS  += -L/usr/local/lib
	LDLIBS   += -ldispatch
endif
endif

ADDITIONAL_OBJCFLAGS = -std=c99 -g -O0 -fobjc-arc -fblocks -Wall -Wno-typedef-redefinition

# Profiling: build with  make PROFILE=1  to enable CPU instrumentation.
# When enabled, a summary is printed every 10s and on SIGUSR1.
ifeq ($(PROFILE),1)
ADDITIONAL_OBJCFLAGS += -DURS_PROFILING=1
endif

include $(GNUSTEP_MAKEFILES)/application.make

# Xnextbsd goes inside the installed bundle: Resources/Xnextbsd.
XNEXTBSD_STAGE = ../Xnextbsd/.build/stage/Xnextbsd
ifeq ($(XNEXTBSD),)
XNEXTBSD_BIN = $(wildcard $(XNEXTBSD_STAGE))
else
XNEXTBSD_BIN = $(XNEXTBSD)
endif

after-install::
ifneq ($(XNEXTBSD_BIN),)
	$(MAKE) -C ../Xnextbsd -f GNUmakefile install \
	    XNEXTBSD="$(abspath $(XNEXTBSD_BIN))" \
	    XLIBRE_APP="$(GNUSTEP_APPS)/$(APP_NAME).app" DESTDIR="$(DESTDIR)"
else
	@echo "XLibre.app: no Xnextbsd to install; build it (gmake xnextbsd) or"
	@echo "pass XNEXTBSD=/path/to/Xnextbsd (the CI artifact)."
endif
