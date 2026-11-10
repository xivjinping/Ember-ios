#!/bin/bash
# Compiles the configured xemu tree for iOS and summarises what fails.
#
# Stage 3, step 2 of the port. The first runs are expected to fail: the point
# is to see every error at once (ninja -k 0 keeps going after a failure)
# instead of fixing them one build at a time. The full log is kept as
# build-deps/xemu-compile.log; the summary printed here groups identical
# errors so the most common causes come first.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build-deps/xemu-build"
LOG="$ROOT/build-deps/xemu-compile.log"

ninja -C "$BUILD" -k 0 qemu-system-i386 > "$LOG" 2>&1
status=$?

strip_paths() { sed -e "s#$ROOT/build-deps/src/xemu/##g" -e "s#$ROOT/##g" -e 's#^\.\./src/xemu/##'; }

echo "==== result: ninja exit $status"
echo "==== steps: $(grep -c '^\[[0-9]*/[0-9]*\]' "$LOG") run, $(grep -c '^FAILED' "$LOG") failed"
echo
echo "==== first failures, in full"
# Not every failure is a compiler error (generators, scripts), so show the
# raw output that follows the first few FAILED lines.
awk '/^FAILED/ { n++; left = 30 } n <= 4 && left > 0 { print; left-- }' "$LOG" | strip_paths | cut -c1-300
echo
echo "==== files that failed"
grep '^FAILED' "$LOG" | strip_paths | sed 's/^FAILED: //' | cut -c1-140 | sort | head -200
echo
echo "==== distinct errors, most frequent first"
grep -E ' (fatal )?error: ' "$LOG" | strip_paths | sed -E 's/^[^ ]*:[0-9]+:[0-9]+: //' | sort | uniq -c | sort -rn | cut -c1-220 | head -120
echo
echo "==== first occurrence of each error, with its location"
grep -E ' (fatal )?error: ' "$LOG" | strip_paths | awk '{ key=$0; sub(/^[^ ]*:[0-9]+:[0-9]+: /, "", key); if (!(key in seen)) { seen[key]=1; print } }' | cut -c1-260 | head -120
echo
echo "==== where each error occurs (up to 12 places per error)"
grep -E ' (fatal )?error: ' "$LOG" | strip_paths | awk '{ key=$0; sub(/^[^ ]*:[0-9]+:[0-9]+: /, "", key); n[key]++; if (n[key] <= 12) print key "  <-  " $1 }' | sort | cut -c1-260 | head -200
echo
echo "==== linker output (if it got that far)"
grep -E 'Undefined symbols|ld: |"_[A-Za-z0-9_]+", referenced from' "$LOG" | sort | uniq -c | sort -rn | cut -c1-200 | head -80

exit $status
