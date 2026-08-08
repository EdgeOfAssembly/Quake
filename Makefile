# Top-level convenience Makefile for Linux Quake ports
# Builds WinQuake software X11 (quake.x11) and OpenGL (glquake).

.PHONY: all clean test tests verify install quake.x11 glquake \
        debug-x11 sanitize-x11 test-debug

# Forward DEBUG= and SANITIZE= from the environment / command line.
MAKE_WQ = $(MAKE) -f Makefile.linux -C WinQuake \
	$(if $(DEBUG),DEBUG=$(DEBUG)) \
	$(if $(SANITIZE),SANITIZE=$(SANITIZE))

all:
	$(MAKE_WQ) all

quake.x11:
	$(MAKE_WQ) quake.x11

glquake:
	$(MAKE_WQ) glquake

install:
	$(MAKE_WQ) install

debug-x11:
	$(MAKE_WQ) debug-x11

sanitize-x11:
	$(MAKE_WQ) sanitize-x11

test tests:
	$(MAKE_WQ) test

test-debug:
	$(MAKE_WQ) test-debug

verify:
	$(MAKE_WQ) verify

clean:
	$(MAKE_WQ) clean
