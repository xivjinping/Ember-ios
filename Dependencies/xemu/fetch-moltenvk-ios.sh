#!/bin/bash
# Fetches MoltenVK (Vulkan implemented on Metal) for iOS arm64 and prepares
# it for static linking into xemu.
#
# Result, in build-deps/moltenvk:
#   include/                 Vulkan headers
#   lib/libMoltenVK-ember.a  MoltenVK with every symbol hidden except
#                            ember_mvk_get_instance_proc_addr
#
# Why hide the symbols: xemu calls Vulkan through volk, which defines a
# global function pointer for each Vulkan function (vkCreateInstance, ...).
# MoltenVK's static library defines real functions with those same names, so
# linking both gives duplicate symbols. Here MoltenVK is linked into one
# object file in which only a renamed vkGetInstanceProcAddr stays visible;
# volk gets every other function from that one (see patch_xemu.py, section 11).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build-deps/moltenvk"
WORK="$ROOT/build-deps/moltenvk-download"
VERSION="v1.4.0"
MIN_IOS="15.0"

rm -rf "$OUT" "$WORK"
mkdir -p "$OUT/lib" "$WORK"

echo "Release assets of MoltenVK $VERSION:"
gh release view "$VERSION" --repo KhronosGroup/MoltenVK --json assets --jq '.assets[] | "  \(.name)  \(.size / 1048576 | floor) MB"'
gh release download "$VERSION" --repo KhronosGroup/MoltenVK --pattern 'MoltenVK-ios.tar' --dir "$WORK" \
    || gh release download "$VERSION" --repo KhronosGroup/MoltenVK --pattern 'MoltenVK-all.tar' --dir "$WORK"
tar -xf "$WORK"/MoltenVK-*.tar -C "$WORK"

LIB="$(find "$WORK" -path '*static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a' | head -1)"
HEADER="$(find "$WORK" -path '*/include/vulkan/vulkan.h' | head -1)"
if [ -z "$LIB" ] || [ -z "$HEADER" ]; then
    echo "error: static iOS library or headers not found in the package. Contents:" >&2
    find "$WORK" -maxdepth 6 | head -80 >&2
    exit 1
fi
cp -R "$(dirname "$(dirname "$HEADER")")" "$OUT/include"
lipo -info "$LIB"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
TARGET="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS"

cat > "$WORK/entry.c" <<'C'
/* The one way into MoltenVK that stays visible. */
typedef void (*ember_vk_function)(void);
extern ember_vk_function vkGetInstanceProcAddr(void *instance, const char *name);

ember_vk_function ember_mvk_get_instance_proc_addr(void *instance, const char *name)
{
    return vkGetInstanceProcAddr(instance, name);
}
C
# shellcheck disable=SC2086
clang $TARGET -O2 -c "$WORK/entry.c" -o "$WORK/entry.o"
# -r: link into one relocatable object. With -r, every global symbol not
# named by -exported_symbol becomes private to that object.
# shellcheck disable=SC2086
clang $TARGET -r -nostdlib -o "$WORK/MoltenVK-ember.o" "$WORK/entry.o" \
    -Wl,-force_load,"$LIB" -Wl,-exported_symbol,_ember_mvk_get_instance_proc_addr
libtool -static -o "$OUT/lib/libMoltenVK-ember.a" "$WORK/MoltenVK-ember.o"

VISIBLE="$(nm -m "$OUT/lib/libMoltenVK-ember.a" 2>/dev/null | grep -E '\) external _' | grep -v ' (undefined)' || true)"
echo "symbols still visible outside the object:"
echo "$VISIBLE" | sed 's/^/  /'
if echo "$VISIBLE" | grep -q ' _vk'; then
    echo "error: Vulkan symbols are still visible; they would clash with volk" >&2
    exit 1
fi
if ! echo "$VISIBLE" | grep -q '_ember_mvk_get_instance_proc_addr'; then
    echo "error: the entry point is missing" >&2
    exit 1
fi
# MoltenVK's own functions must still be in there, just not visible.
if ! nm -m "$OUT/lib/libMoltenVK-ember.a" 2>/dev/null | grep '_vkCreateInstance$' | head -3; then
    echo "error: vkCreateInstance is not in the object at all" >&2
    exit 1
fi

rm -rf "$WORK"
ls -la "$OUT" "$OUT/lib" "$OUT/include"
