# qpak — Quake PAK tool (g++ only, C++23)
CXX      ?= g++
CXXSTD   := -std=gnu++23
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2
INCLUDES := -Iinclude
SRC      := src/main.cpp src/pak.cpp

BUILD    ?= debug
PREFIX   ?= /usr/local

ifeq ($(BUILD),release)
  CXXFLAGS ?= $(CXXSTD) $(WARN) -O3 -DNDEBUG $(INCLUDES)
else ifeq ($(BUILD),profile)
  CXXFLAGS ?= $(CXXSTD) $(WARN) -O2 -g -fno-omit-frame-pointer $(INCLUDES)
else
  BUILD := debug
  CXXFLAGS ?= $(CXXSTD) $(WARN) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(INCLUDES)
  LDFLAGS  ?= -fsanitize=address,undefined
endif

BUILDDIR := build/$(BUILD)
BIN      := $(BUILDDIR)/qpak

.PHONY: all debug release profile test tests verify clean install

all: $(BIN)

debug:
	$(MAKE) BUILD=debug

release:
	$(MAKE) BUILD=release

profile:
	$(MAKE) BUILD=profile

$(BIN): $(SRC) include/qpak/pak.hpp include/qpak/version.hpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDFLAGS)

test tests: $(BIN)
	@bash tests/run.sh "$(BIN)"

verify: test
	@echo "formal: not run (no CBMC harness yet)"

install: release
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 build/release/qpak $(DESTDIR)$(PREFIX)/bin/qpak

clean:
	rm -rf build
