# Debugging quake.x11 (Linux software + X11)

See also: [`GOAL_QUAKE_X11_DEBUG_MEMORY.md`](GOAL_QUAKE_X11_DEBUG_MEMORY.md)

## Build

```bash
# From repo root
make debug-x11                          # → quake.x11-dbg  (-O0 -g3 -DDEBUG -DPARANOID -UNDEBUG)
make sanitize-x11                       # → quake.x11-asan (ASan + UBSan + debug flags)
make DEBUG=1 quake.x11                  # debug objects under WinQuake/build/*-dbg/
make DEBUG=1 SANITIZE=address,undefined quake.x11
```

Debug/sanitize builds use a **separate** `WinQuake/build/<arch>-dbg[-san]/` tree so they never mix with release `-O3` objects.

## Asserts

Debug builds define `-DDEBUG -DPARANOID -UNDEBUG` so:

- C `assert()` is active
- Quake `#ifdef PARANOID` checks run (zone heap, math, etc.)

Release builds must **not** pass these; do not add `-DNDEBUG` to `DEBUG=1`.

## GDB

```bash
gdb --args ./quake.x11-dbg -basedir . +map e1m1
# useful breakpoints:
#   break Sys_Error
#   break Z_Free
#   break Hunk_AllocName
#   break PR_ExecuteProgram
```

## Valgrind

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  ./quake.x11-dbg -basedir . +map e1m1 +quit
```

Prefer a short `+map`/`+quit` or timedemo; full play is slow under Valgrind.  
Use a real X display (Xmux/Xvfb) if the client requires it.

## ASan / UBSan

```bash
./quake.x11-asan -basedir . +map e1m1 +quit
# or:
ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
  ./quake.x11-asan -basedir . +map e1m1
```

## Smoke tests

```bash
make DEBUG=1 -C WinQuake -f Makefile.linux test-debug
make test          # release both binaries
```

## Visual testing (spectator)

Prefer **Xmux** so the human can watch:

```bash
xmux start quake-x11-test --geometry 1024x768 --gl mesa --no-attach
# SPECTATOR: xmux attach quake-x11-test --no-reconnect
eval $(xmux env quake-x11-test)
./quake.x11-dbg -basedir . -window -width 800 -height 600 +map e1m1
```

Screenshots during play:

```bash
xmux screenshot quake-x11-test -o /tmp/quake-shots/xmux-N.png
# or full session root via GNOME (uses X11 fallback on Xvfb):
DISPLAY=:10 gnome-screenshot -f /tmp/quake-shots/gnome-N.png
```

Evidence shots: `docs/evidence/shots/`.

## Valgrind

Requires glibc debuginfo for the dynamic linker (`ld-linux-x86-64.so.2` must export `memcmp` for redirection).

On this Gentoo host (glibc-2.38), Valgrind 3.26 may fail at startup with:

```text
a must-be-redirected function ... memcmp ... ld-linux-x86-64.so.2 was not found
```

**Fix (host):** install/splitdebug glibc so `/usr/lib/debug/.../ld-*.so` is present, then:

```bash
valgrind --leak-check=full --track-origins=yes ./quake.x11-dbg -basedir . +quit
```

Until then, prefer **ASan** (`make sanitize-x11`) for memory errors.
