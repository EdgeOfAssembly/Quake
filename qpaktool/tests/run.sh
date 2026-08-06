#!/usr/bin/env bash
# Contract tests for qpak
set -euo pipefail
QPAK="${1:?binary}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

# --- usage / version ---
"$QPAK" >/dev/null 2>&1 && fail "no-args should fail" || true
"$QPAK" -h | head -1 | grep -qi usage || fail "help"
"$QPAK" --version | grep -q 'qpak 0\.' || fail "version"
pass "help/version"

# --- synthetic PAK ---
python3 - <<'PY' "$TMP/test.pak"
import struct, sys
from pathlib import Path
# two files: a/hello.txt and progs/ogre.mdl (fake)
files = {
    b"a/hello.txt": b"hello-quake\n",
    b"progs/ogre.mdl": b"IDPO" + b"\0" * 100,
    b"sound/ogre/ogwake.wav": b"RIFF" + b"\0" * 20,
}
# layout: header, data..., dir
data_blobs = []
dir_ents = []
offset = 12
for name, blob in files.items():
    name56 = name[:56].ljust(56, b"\0")
    dir_ents.append(name56 + struct.pack("<II", offset, len(blob)))
    data_blobs.append(blob)
    offset += len(blob)
body = b"".join(data_blobs)
dirofs = 12 + len(body)
dirtab = b"".join(dir_ents)
header = b"PACK" + struct.pack("<II", dirofs, len(dirtab))
Path(sys.argv[1]).write_bytes(header + body + dirtab)
print("wrote", sys.argv[1], "entries", len(files))
PY

# list all
n=$("$QPAK" list "$TMP/test.pak" 2>/dev/null | wc -l)
[[ "$n" -eq 3 ]] || fail "list all count=$n"
pass "list all"

# exact
out=$("$QPAK" list "$TMP/test.pak" --exact progs/ogre.mdl 2>/dev/null)
echo "$out" | grep -q 'progs/ogre.mdl' || fail "exact"
[[ $(echo "$out" | wc -l) -eq 1 ]] || fail "exact count"
pass "exact"

# wildcard
out=$("$QPAK" list "$TMP/test.pak" --wildcard '*ogre*' 2>/dev/null)
[[ $(echo "$out" | wc -l) -eq 2 ]] || fail "wildcard count"
pass "wildcard"

# regex (search anywhere in path)
out=$("$QPAK" list "$TMP/test.pak" --regex 'ogre' 2>/dev/null)
[[ $(echo "$out" | wc -l) -eq 2 ]] || fail "regex count=$(echo "$out" | wc -l)"
out=$("$QPAK" list "$TMP/test.pak" --regex 'progs/.*\.mdl$' 2>/dev/null)
echo "$out" | grep -q 'progs/ogre.mdl' || fail "regex anchored"
pass "regex"

# extract one
"$QPAK" extract "$TMP/test.pak" "$TMP/out" --exact a/hello.txt >/dev/null
[[ -f "$TMP/out/a/hello.txt" ]] || fail "extract file missing"
grep -q 'hello-quake' "$TMP/out/a/hello.txt" || fail "extract content"
pass "extract exact"

# extract glob
"$QPAK" extract "$TMP/test.pak" "$TMP/out2" --wildcard 'progs/*' >/dev/null
[[ -f "$TMP/out2/progs/ogre.mdl" ]] || fail "extract glob"
pass "extract wildcard"

# real Quake PAK if present
PAK0=/tmp/QUAKE/id1/PAK0.PAK
if [[ -f "$PAK0" ]]; then
  "$QPAK" list "$PAK0" --exact progs/ogre.mdl 2>/dev/null | grep -q ogre.mdl || fail "real ogre list"
  "$QPAK" extract "$PAK0" "$TMP/quake" --wildcard '*ogre*' >/dev/null
  [[ -f "$TMP/quake/progs/ogre.mdl" ]] || fail "real ogre extract"
  pass "real PAK0 ogre"
else
  echo "SKIP: real PAK0 not at $PAK0"
fi

echo "ALL TESTS PASSED"
