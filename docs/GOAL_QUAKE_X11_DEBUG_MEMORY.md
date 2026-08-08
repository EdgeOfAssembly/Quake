# GOAL: quake.x11 — debug build, memory safety, portable integer types

**Target binary:** `quake.x11` (WinQuake software renderer + X11 / XShm)  
**Tree:** `/home/wizard/Quake` · sources `WinQuake/` · build `WinQuake/Makefile.linux`  
**Out of scope (this goal):** `glquake` feature work, dynlights polish, game-model / VL loop  
**In scope:** make **debug `quake.x11`** the quality bar for memory, asserts, sanitizers, types, and loop hygiene.

This document is the **campaign brief / agent goal prompt**. Paste sections into orchestrator or subagent prompts as needed.

---

## 1. Mission

Ship a **debug-first** Linux X11 Quake build that:

1. Builds with **full asserts**, **frame pointers**, **no optimise-away of checks**.
2. Is routinely run under **ASan / UBSan / (optional) MSan**, **Valgrind**, and **GDB**.
3. Is hunted by **multiple agents** for classic memory bugs.
4. Migrates host code toward **`stdint.h` / `stddef.h` types** (via existing `q_stdint.h` where appropriate).
5. Hardens **integer under/overflow** where risk is clear (widen types, checked math, or assert bounds).
6. Standardises **for-loop indexing** (`size_t` / `ssize_t`) and **hoists** invariant work out of loops.

Success is **evidence-based**: green sanitizer runs, Valgrind summaries, GDB repros fixed, and reviewable diffs — not “looks fine.”

---

## 2. Product / build targets

| Item | Spec |
|------|------|
| Binary | `quake.x11` (and install to repo root as today) |
| Debug binary | e.g. `quake.x11-dbg` or `build/*/bin/quake.x11` with `DEBUG=1` |
| Compiler | **gcc** only (project rule) |
| Standard | Prefer **gnu17** or **gnu23** if tree allows; do not silently break asm-free C fallbacks |
| Existing hook | `make -f Makefile.linux DEBUG=1` already uses `-O0 -g3 -fno-omit-frame-pointer` — **extend**, do not replace carelessly |

### Required Makefile / tooling work (implementers)

Add explicit, documented knobs (names may match project style):

```text
make -f Makefile.linux quake.x11 DEBUG=1
make -f Makefile.linux quake.x11 DEBUG=1 SANITIZE=address,undefined
make -f Makefile.linux quake.x11 DEBUG=1 SANITIZE=address,undefined,leak   # if supported
# optional later: SANITIZE=memory (MSan needs instrumented libs — document if skipped)
```

**Debug flags (minimum):**

- `-O0 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls`
- `-fno-strict-aliasing` (Quake type-puns; keep)
- **`-DDEBUG` / ensure `assert` active** — never `-DNDEBUG` on debug target
- Prefer **`-UNDEBUG`** and link with assertions in `zone`, `common`, host paths
- Harden warnings gradually: do **not** hide real bugs with new `-Wno-*` without justification

**Sanitizer flags (ASan + UBSan baseline):**

- `-fsanitize=address,undefined`
- `-fsanitize-address-use-after-scope` (if gcc supports in this version)
- `-fno-omit-frame-pointer`
- Suitable `-fstack-protector-strong` on debug
- Link with same `-fsanitize=...`

**Valgrind:**

- Document recipe:  
  `valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./quake.x11-dbg -basedir .. +map e1m1 +timedemo demo1`  
  (adjust demo/map to what exists under `id1/`)
- Short smoke: `+quit` after load; longer: one timedemo or fixed `host_framerate` session under Xmux

**GDB:**

- `gdb --args ./quake.x11-dbg ...`
- Break on `Sys_Error`, `assert` abort, ASan report hooks if useful
- Keep a short `docs/` or `scripts/` note: common breakpoints (`Z_Free`, `Hunk_AllocName`, `PR_ExecuteProgram`)

**Asserts:**

- Audit `#ifdef PARANOID`, `assert(`, custom `Sys_Error` checks
- Debug build: enable **maximum** existing paranoia (`PARANOID`, range checks in `zone.c`, `pr_exec.c`, etc.) without changing release defaults unless agreed
- New checks: prefer `assert(cond)` in debug + `Sys_Error` / graceful fail for unrecoverable host errors

---

## 3. Memory bug classes (multi-agent hunt)

Use **parallel explore/review agents** by subsystem; one implementer fixes; one reviewer verifies.

### 3.1 Bug classes (must search)

| Class | What to look for |
|-------|------------------|
| Uninitialized use | Stack locals, partial structs, `malloc` without init, conditional init |
| NULL deref | Unchecked returns (`malloc`, `COM_LoadHunkFile`, X11, sound), optional pointers |
| Use-after-free | `Z_Free` / hunk free / cache free then touch; entity remove; sound channels |
| Double-free | Two free paths; error paths that free then fall through |
| Buffer overflow | `strcpy`/`sprintf`/`strcat`, fixed `char[]`, `memcpy` length, surface/edge spans |
| Buffer underflow | Negative index into arrays; `size - n` without check |
| Stack smash | Large VLAs / on-stack buffers; unbounded recursion; `alloca` |
| Leaks | X11 resources, ALSA, files, zone/hunk lifetime vs level change |

### 3.2 Hot subsystems (priority order for agents)

1. **`zone.c` / `zone.h`** — Hunk, zone, cache (classic landmine)
2. **`common.c`** — file load, paths, `va()`, tokenisation
3. **`vid_x.c`** — X11 buffers, XShm, resize, 24/32 bpp paths (recent LP64 work)
4. **`sys_linux.c`**, **`snd_linux.c`**, **`cd_linux.c`**
5. **`model.c` / `r_*.c` / `d_*.c`** — software raster, surfaces, spans
6. **`pr_exec.c` / `pr_edict.c` / `pr_cmds.c`** — QC VM, strings, edicts
7. **`sv_*.c` / `world.c`** — server physics, area nodes
8. **`cl_*.c` / `net_*.c`** — parse, demos, packets
9. **`host.c` / `host_cmd.c`** — lifecycle, map change teardown

### 3.3 Agent roles (orchestrator pattern)

| Role | Job |
|------|-----|
| **[explore] memory-map** | Inventory alloc/free APIs, global buffers, `#pragma` / unchecked casts |
| **[explore] types-loops** | Find `int`/`long` indexes, `for (i=0;i<n;i++)` patterns, non-`stdint` widths |
| **[implement] debug-build** | Makefile `DEBUG`/`SANITIZE`, assert defines, `quake.x11-dbg` install name |
| **[implement] fix-N** | Fix one bug class or one file cluster; keep diffs tight |
| **[test] asan-valgrind** | Run recipes; capture logs under `docs/evidence/` or `/tmp` with paths in PR notes |
| **[review]** | Diff review for new UB, wrong `size_t` (unsigned underflow), behavior change |

Do **not** spawn agents that edit the same files without worktree isolation.

---

## 4. Integer / type policy

### 4.1 Prefer fixed-width and standard sizes

- Host code: **`stdint.h`** types (`int32_t`, `uint32_t`, `int16_t`, `uint8_t`, …) and **`stddef.h`** (`size_t`, `ptrdiff_t`).
- Use existing **`WinQuake/q_stdint.h`** (`qint32_t`, `qsize_t`, pointer helpers) for progs/network/file-format clarity.
- **Avoid** bare `long` for sizes and file offsets on LP64 (historical Quake bug class).
- **Progs / on-disk / network:** stay **32-bit** where the format requires it; do not widen wire structs casually.
- Pointers stored as integers: `intptr_t` / `uintptr_t` (or `qintptr_t`), never `int`.

### 4.2 Overflow / underflow

Where there is a **clear risk** (size calculations, `count * sizeof`, `end - start`, demo/net message lengths, surface extents):

1. Assert or check before multiply/add.
2. If a type is too narrow for real limits (e.g. 1080p buffers, large hunks), **widen** to `size_t` / `uint64_t` / `ptrdiff_t` as appropriate.
3. Prefer unsigned sizes for pure counts; beware **unsigned wrap** in `n - k` when `k > n`.
4. Document any intentional wrap (rare).

### 4.3 For-loop indexing (mandatory style for touched loops)

```c
/* Length fixed for this scope — compute once outside the loop */
const size_t count = (size_t)num_things;
for (size_t i = 0; i < count; i++)
{
	/* ... */
}
```

| Situation | Index type |
|-----------|------------|
| Index runs `0 .. length-1`, length non-negative | **`size_t`** |
| Length is fixed for the loop | **`const size_t len = ...` outside loop** |
| Index **can be negative** (scan backward, signed protocol, error codes as index) | **`ssize_t`** (signed) |
| Need difference of pointers | **`ptrdiff_t`** |

**Do not** casually change every loop in the tree in one PR — apply when touching a function, or in dedicated “loop hygiene” passes per file with tests.

### 4.4 Loop hoisting

As much as possible, **precompute outside the loop**:

- Invariant bounds, strides, pointers (`base + offset`)
- `sizeof` products, clip rectangles, colour tables
- Function calls that do not depend on the induction variable
- Early-out conditions that apply to the whole loop

Keep inner loops tight for software renderer paths (`d_scan.c`, `r_surf.c`, etc.) without changing algorithms unless required for safety.

---

## 5. Definition of done (this goal)

### Build / tooling

- [ ] `DEBUG=1` (or `make debug-x11`) produces a clearly named debug binary with asserts **on**
- [ ] `SANITIZE=address,undefined` build works for `quake.x11`
- [ ] Documented Valgrind one-liner and GDB one-liner in this doc or `docs/DEBUGGING_X11.md`
- [ ] Top-level `Makefile` forwards `DEBUG=` / `SANITIZE=` if useful

### Verification (evidence required)

- [ ] ASan+UBSan smoke: load map or `+quit` after init — **exit 0**, no sanitizer errors
- [ ] Valgrind smoke (may be slow): note still-reachable vs definite leaks; fix **definite** leaks in engine code we own
- [ ] At least one real bug found+fixed with sanitizer or assert evidence (or explicit “clean under recipe X”)
- [ ] `make test` / project test target still green (extend if missing X11-debug smoke)

### Types / loops

- [ ] Policy above recorded; new/edited code follows it
- [ ] No drive-by reformat of entire tree
- [ ] Clear overflow risks in touched code addressed or ticketed with file:line

### Process

- [ ] Multi-agent hunt covered zone + common + vid_x at minimum
- [ ] FEATURE/FIXUP commits, checkpoint, push (efficient-git)
- [ ] Formal: `make verify` if present; else `formal: not run` + reason

---

## 6. Constraints / non-goals

- **Do not** break shareware/`id1` load path or X11 input regressions (key repeat, resize).
- **Do not** enable sanitizers on default release `-O3` install without a separate target.
- **Do not** replace Quake’s hunk allocator with `malloc` everywhere in one go.
- **Do not** force `size_t` into progs VM opcode widths or network protocol fields.
- Prefer **minimal diffs**; memory fixes over style-only churn.
- gcc only; no clang-required flags unless optional.

---

## 7. Suggested execution order

1. **Tooling** — debug + sanitize Makefile targets; prove binary runs under Xmux/X11.
2. **Baseline** — ASan/UBSan + Valgrind logs on current tree (known issues list).
3. **Zone + common** — fix critical heap/hunk bugs first.
4. **vid_x + sys/snd** — X11/ALSA lifetime.
5. **Renderer / server** — as sanitizer points.
6. **Types/loops pass** — file-by-file, with overflow review on size math.
7. **Hardening pass** — asserts, hoisting in hottest loops touched for bugs.
8. **Docs + evidence** — update TODO, this goal checklist, commit.

---

## 8. Copy-paste orchestrator prompt

```text
You are the orchestrator for Quake Linux X11 (quake.x11) memory/debug hardening.

Repo: /home/wizard/Quake
Build: make -f WinQuake/Makefile.linux (or top-level Makefile) with DEBUG=1 and SANITIZE=address,undefined
Binary: quake.x11 / quake.x11-dbg — software renderer + X11 only (not glquake features).
Goal doc: docs/GOAL_QUAKE_X11_DEBUG_MEMORY.md — follow it.

Work:
1) Extend Makefile for full debug asserts + ASan/UBSan (+ document Valgrind/GDB).
2) Multi-agent hunt: uninit, NULL, UAF, double-free, buffer over/underflow, stack smash, leaks.
   Priority: zone.c, common.c, vid_x.c, sys_linux.c, snd_linux.c, then r_*/d_*/pr_*/sv_*.
3) Types: stdint/stddef via q_stdint.h where appropriate; widen on clear overflow risk.
4) Loops: size_t indexes; const size_t length outside loop; ssize_t if index can be negative;
   hoist invariants out of loops when touching code.
5) Evidence: sanitizer/valgrind/gdb logs; make test; efficient-git FEATURE/FIXUP + push.
6) No blind large refactors; no -DNDEBUG on debug builds.

Use explore/implement/review subagents; isolate parallel writers with worktrees.
```

---

## 9. Copy-paste subagent prompts (short)

### Explore — memory

```text
[explore] Map alloc/free and unsafe buffers in WinQuake for quake.x11.
List Z_*/Hunk_*/Cache_* APIs, global arrays, strcpy/sprintf sites, vid_x buffer ownership.
Output: ranked hit list with file:line and bug class hypothesis. Read-only.
```

### Explore — types/loops

```text
[explore] Find int/long size fields and for-loops that should use size_t/ssize_t in WinQuake
(X11 path sources). Note clear overflow risks (count*size, end-start). Read-only report.
```

### Implement — debug build

```text
[implement] WinQuake/Makefile.linux (+ top Makefile): DEBUG=1 asserts on, SANITIZE= for
address,undefined, produce quake.x11-dbg, document valgrind/gdb. gcc only. Prove build exit 0.
```

### Review

```text
[review] Review diff for quake.x11 debug/memory work: new UAF, unsigned underflow from size_t,
behavior changes, missing evidence. Do not implement.
```

---

## 10. Evidence log (fill as work proceeds)

| Date | Command | Result |
|------|---------|--------|
| | `make ... DEBUG=1 SANITIZE=...` | |
| | ASan smoke | |
| | Valgrind smoke | |
| | Notable fix | |

---

*Created for the post-ADOM return to Quake development. Keep this goal focused on **quake.x11** reliability before more gameplay/renderer features.*

## 10. Evidence log (session 2026-08-08)

| Date | Command | Result |
|------|---------|--------|
| 2026-08-08 | `make debug-x11` | exit 0 → `quake.x11-dbg` (`-DDEBUG -DPARANOID -UNDEBUG`) |
| 2026-08-08 | `make sanitize-x11` | exit 0 → `quake.x11-asan` (ASan+UBSan) |
| 2026-08-08 | `./quake.x11-asan -basedir . +quit` (before fix) | **ASan global-buffer-overflow** `COM_FileBase` common.c:868 |
| 2026-08-08 | same after fix | **exit 0**, VID_Shutdown clean |
| 2026-08-08 | explore reports | `docs/evidence/quake-x11-memory-explore.md`, `…-types-loops-explore.md` |

### Fixes landed this session
- Makefile: DEBUG/SANITIZE separate build trees, `debug-x11` / `sanitize-x11`, docs/DEBUGGING_X11.md
- `world.c` PARANOID: undeclared `sv_hullmodel` → `hull` (arg order fixed)
- `STRUCT_FROM_LINK`: `ptrdiff_t`/`offsetof` (LP64)
- `COM_FileBase`: no walk before string start; cap basename 31 chars
- `COM_Parse`: clamp to `com_token` size
- `va` + host printf helpers: `vsnprintf`


### Waves B–D (2026-08-08 autonomous)

| Wave | Work |
|------|------|
| **B vid_x** | `XDestroyImage` teardown; SHM detach order; checked width×height; `size_t` buffersize; NULL malloc check; `VID_Shutdown` frees video |
| **C QC** | Statement PC bounds; STOREP/LOAD/ADDRESS field+edict bounds; `PR_RunError` vsnprintf |
| **D zone/model** | `Q_checked_mul/add_size`, `Q_size_to_int`; zone/hunk/cache wrap guards; model lump/tex/alias/sprite checked allocs; `COM_LoadFile` len+1 |
| **extra** | `cl_parse` `1u<<j` UBSan shift |

| Command | Result |
|---------|--------|
| `./quake.x11-asan +quit` | exit 0 |
| `./quake.x11-asan +map e1m1 +quit` | exit 0 (player entered; VID_Shutdown) |

