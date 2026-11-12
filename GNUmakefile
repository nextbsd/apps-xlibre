# Top-level GNUmakefile for apps-xlibre
#
# all / install / clean: XLibre.app (XLibre/). "gmake install" puts it in
# the GNUstep domain GNUSTEP_INSTALLATION_DOMAIN names (USER unless set,
# so ~/Applications), with Xnextbsd inside it when there is one: pass
# XNEXTBSD=/path/to/Xnextbsd (the CI artifact), or build it first.
# xnextbsd / install-xnextbsd / clean-xnextbsd: Xnextbsd, the rootless X
# server on WindowServer (Xnextbsd/GNUmakefile).

.PHONY: all clean install xnextbsd install-xnextbsd clean-xnextbsd

all:
	$(MAKE) -C XLibre -f GNUmakefile

install:
	$(MAKE) -C XLibre -f GNUmakefile install

clean:
	$(MAKE) -C XLibre -f GNUmakefile clean

xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile

install-xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile install

clean-xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile clean
