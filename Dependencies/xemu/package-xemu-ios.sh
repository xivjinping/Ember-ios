#!/bin/bash
# Turns the compiled xemu tree into static libraries the app can link.
#
# Run after compile-xemu-ios.sh has linked the qemu-system-i386 executable.
# The executable itself is not used: this script takes its link command
# apart and repacks the same inputs as
#
#   build-deps/xemu-lib/libember-xemu-core.a   every object file of the link,
#                                              plus archives it force-loaded.
#                                              The app must -force_load this:
#                                              QEMU registers devices from
#                                              constructors nothing refers to.
#   build-deps/xemu-lib/libember-xemu-deps.a   the libraries it linked
#                                              (GLib, pixman, SDL3, ...)
#   build-deps/xemu-lib/ember-xemu.xcconfig    linker flags for the app
#                                              (included by Config/Backend.xcconfig)
#
# It then links a small test program against the two libraries, so a
# library that cannot be linked fails here and not in the app build.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build-deps/xemu-build"
OUT="$ROOT/build-deps/xemu-lib"
MIN_IOS="15.0"

rm -rf "$OUT"
mkdir -p "$OUT"

ninja -C "$BUILD" -t commands qemu-system-i386-unsigned | tail -1 > "$OUT/link-command.txt"

python3 - "$BUILD" "$OUT" <<'PY'
import os, shlex, sys
build, out = sys.argv[1], sys.argv[2]
tokens = shlex.split(open(os.path.join(out, "link-command.txt")).read())
# Meson wraps commands that need a working directory or a shell; keep the
# compiler invocation only.
start = next(i for i, t in enumerate(tokens) if os.path.basename(t) in ("clang", "clang++", "cc", "c++"))
tokens = tokens[start + 1:]
for stop in ("&&", ";"):
    if stop in tokens:
        tokens = tokens[:tokens.index(stop)]

def absolute(path):
    return path if os.path.isabs(path) else os.path.normpath(os.path.join(build, path))

core, deps, flags, ignored, libdirs = [], [], [], [], []
for t in tokens:
    if t.startswith("-L"):
        libdirs.append(absolute(t[2:]))

i = 0
while i < len(tokens):
    t = tokens[i]
    nxt = tokens[i + 1] if i + 1 < len(tokens) else None
    if t == "-o":
        i += 1
    elif t in ("-Wl,-force_load", "-force_load"):
        core.append(absolute(nxt)); i += 1
    elif t.startswith("-Wl,-force_load,"):
        core.append(absolute(t.split(",", 2)[2]))
    elif t.endswith(".o"):
        if "ember-exe-main" not in t:
            core.append(absolute(t))
    elif t.endswith((".a", ".fa")) and not t.startswith("-"):
        deps.append(absolute(t))
    elif t in ("-framework", "-weak_framework"):
        flags += [t, nxt]; i += 1
    elif t.startswith(("-Wl,-framework,", "-Wl,-weak_framework,")):
        kind, name = t.split(",")[1:3]
        flags += [kind, name]
    elif t.startswith("-l"):
        name = t[2:]
        found = next((os.path.join(d, f"lib{name}.a") for d in libdirs
                      if os.path.exists(os.path.join(d, f"lib{name}.a"))), None)
        if found:
            deps.append(found)
        else:
            flags.append(t)
    elif t in ("-arch", "-isysroot"):
        i += 1
    elif t.startswith(("-L", "-I", "-D", "-O", "-g", "-m", "-std", "-f", "-W", "-pthread")):
        if t.startswith("-Wl,") and not any(k in t for k in ("dead_strip", "headerpad", "rpath", "search_paths")):
            ignored.append(t)
    else:
        ignored.append(t)
    i += 1

def unique(items):
    seen, result = set(), []
    for item in items:
        if item not in seen:
            seen.add(item); result.append(item)
    return result

core, deps = unique(core), [d for d in unique(deps) if d not in core]
# Flags come in pairs (-framework X) or alone (-lz); drop repeats of either.
paired, j = [], 0
while j < len(flags):
    if flags[j] in ("-framework", "-weak_framework"):
        paired.append(flags[j] + " " + flags[j + 1]); j += 2
    else:
        paired.append(flags[j]); j += 1
flags = unique(paired)

missing = [p for p in core + deps if not os.path.exists(p)]
if missing:
    sys.exit("package-xemu-ios: link inputs not found:\n  " + "\n  ".join(missing))

open(os.path.join(out, "core.filelist"), "w").write("\n".join(core) + "\n")
open(os.path.join(out, "deps.filelist"), "w").write("\n".join(deps) + "\n")
open(os.path.join(out, "system-flags.txt"), "w").write(" ".join(flags) + "\n")
print(f"core: {len(core)} objects and force-loaded archives")
print("libraries:", " ".join(os.path.basename(d) for d in deps))
print("system flags:", " ".join(flags))
print("ignored link arguments:", " ".join(ignored) or "(none)")
PY

# libtool warns about every member without symbols and every repeated member
# name; neither matters here.
libtool -static -o "$OUT/libember-xemu-core.a" -filelist "$OUT/core.filelist" 2>&1 | grep -v 'has no symbols\|same member name\|^$' | head -20 || true
libtool -static -o "$OUT/libember-xemu-deps.a" -filelist "$OUT/deps.filelist" 2>&1 | grep -v 'has no symbols\|same member name\|^$' | head -20 || true
# Debug information triples the size and the app cannot use it.
strip -S "$OUT/libember-xemu-core.a" "$OUT/libember-xemu-deps.a" 2>&1 | grep -v 'has no symbols\|^$' | head -5 || true

FLAGS="$(cat "$OUT/system-flags.txt")"
cat > "$OUT/ember-xemu.xcconfig" <<XC
// Generated by Dependencies/xemu/package-xemu-ios.sh. Switches the app from
// the stub backend to xemu. Included, when present, by Config/Backend.xcconfig.
EMBER_XEMU_LDFLAGS = -force_load \$(SRCROOT)/build-deps/xemu-lib/libember-xemu-core.a \$(SRCROOT)/build-deps/xemu-lib/libember-xemu-deps.a $FLAGS
EMBER_XEMU_DEFS = EMBER_BACKEND_XEMU=1
XC

# Test link: the same way the app links it, with a main() of its own.
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
TARGET="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS"
clang $TARGET -c "$HERE/ember/ember-exe-main.c" -o "$OUT/test-main.o"
# shellcheck disable=SC2086
clang++ $TARGET -o "$OUT/link-test" "$OUT/test-main.o" \
    -Wl,-force_load,"$OUT/libember-xemu-core.a" "$OUT/libember-xemu-deps.a" $FLAGS \
    > "$OUT/link-test.log" 2>&1 || {
        echo "==== test link FAILED"
        grep -c 'referenced from' "$OUT/link-test.log" | sed 's/^/undefined symbols: /'
        awk '/", referenced from:/ { sym=$1; getline; sub(/^ +/, ""); print sym "  <-  " $0 }' "$OUT/link-test.log" | sort -u | cut -c1-200 | head -200
        grep -E '^ld: |duplicate symbol|clang(\+\+)?: error' "$OUT/link-test.log" | sort | uniq -c | cut -c1-300 | head -40
        exit 1
    }
echo "==== test link passed"
nm -g "$OUT/link-test" | grep -c ' T ' | sed 's/^/exported functions in the test program: /'
rm -f "$OUT/link-test" "$OUT/test-main.o" "$OUT/core.filelist" "$OUT/deps.filelist"
ls -la "$OUT"
cat "$OUT/ember-xemu.xcconfig"
