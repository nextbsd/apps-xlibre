# Xnextbsd: the rootless X server on NextBSD's WindowServer
# (nextbsd/apps-xlibre#2, #3). See build-xnextbsd.sh.
#
#   gmake               build Xnextbsd into .build/stage (needs meson and
#                       ninja; NextBSD has neither yet, so CI builds it)
#   gmake install       put it in the XLibre.app bundle:
#                       $(DESTDIR)$(XLIBRE_APP)/Resources/Xnextbsd
#   gmake install XNEXTBSD=/path/to/Xnextbsd
#                       the same with a binary built elsewhere (the CI
#                       artifact), without building
#   gmake clean
#
# XLibre.app installs as the user, in whichever GNUstep domain they choose
# (~/Applications by default), and runs Xnextbsd from inside its bundle:
# nothing goes in a system path, nothing needs root, and Xnextbsd is not
# set-uid. Until XLibre.app exists (apps-xlibre#5) this makes the
# bundle's Resources directory with Xnextbsd in it and nothing else.

GNUSTEP_USER_APPS ?= $(shell gnustep-config --variable=GNUSTEP_USER_APPS 2>/dev/null || echo $(HOME)/Applications)
XLIBRE_APP	?= $(GNUSTEP_USER_APPS)/XLibre.app
INSTALL		?= install
DESTDIR		?=

STAGE		= .build/stage
XNEXTBSD	?= $(STAGE)/Xnextbsd

.PHONY: all install clean

all: $(STAGE)/Xnextbsd

$(STAGE)/Xnextbsd:
	DESTDIR="$(CURDIR)/$(STAGE)" ./build-xnextbsd.sh

ifeq ($(XNEXTBSD),$(STAGE)/Xnextbsd)
install: $(STAGE)/Xnextbsd
else
install:
endif
	$(INSTALL) -d "$(DESTDIR)$(XLIBRE_APP)/Resources"
	$(INSTALL) -m 0755 "$(XNEXTBSD)" "$(DESTDIR)$(XLIBRE_APP)/Resources/Xnextbsd"

clean:
	rm -rf .build
