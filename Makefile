# Top-level convenience Makefile for Linux Quake ports
# Builds WinQuake software X11 (quake.x11) and OpenGL (glquake).

.PHONY: all clean test tests verify install quake.x11 glquake

MAKE_WQ = $(MAKE) -f Makefile.linux -C WinQuake

all:
	$(MAKE_WQ) all

quake.x11:
	$(MAKE_WQ) quake.x11

glquake:
	$(MAKE_WQ) glquake

install:
	$(MAKE_WQ) install

test tests:
	$(MAKE_WQ) test

verify:
	$(MAKE_WQ) verify

clean:
	$(MAKE_WQ) clean
