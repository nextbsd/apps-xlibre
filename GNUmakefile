# Top-level GNUmakefile for apps-xlibre
#
# all / install / clean: the window manager (WindowManager/), as before.
# xnextbsd / install-xnextbsd / clean-xnextbsd: Xnextbsd, the rootless X
# server on WindowServer (Xnextbsd/GNUmakefile).

.PHONY: all clean install xnextbsd install-xnextbsd clean-xnextbsd

all:
	$(MAKE) -C WindowManager -f GNUmakefile

install:
	$(MAKE) -C WindowManager -f GNUmakefile install

clean:
	$(MAKE) -C WindowManager -f GNUmakefile clean

xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile

install-xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile install

clean-xnextbsd:
	$(MAKE) -C Xnextbsd -f GNUmakefile clean
