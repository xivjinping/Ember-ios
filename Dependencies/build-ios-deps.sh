#!/bin/bash
# Cross-compiles the libraries an x86 engine (xemu / QEMU) needs, for iOS devices.
#
#   Dependencies/build-ios-deps.sh            # builds everything into build-deps/prefix
#
# Requires macOS with Xcode, plus: brew install meson ninja pkgconf
# Output: static libraries, headers and .pc files under build-deps/prefix,
# which later stages point pkg-config at.
#
# Stage 2, step 1 of the port: glib only. More libraries are added here as
# each one is proven to build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-deps"
PREFIX="$WORK/prefix"
SRC="$WORK/src"
MIN_IOS="15.0"
GLIB_VERSION="2.88.4"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$PREFIX/lib/pkgconfig" "$SRC"

# --- Meson cross file ------------------------------------------------------------
# pkg_config_libdir restricts lookups to our prefix, so nothing built for
# macOS (Homebrew's libffi, pcre2, ...) can leak into an iOS build; missing
# libraries fall back to the source "wraps" each project ships.
CROSS="$WORK/ios-arm64.ini"
cat > "$CROSS" <<INI
[constants]
target = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$MIN_IOS']

[binaries]
c = ['clang'] + target
cpp = ['clang++'] + target
objc = ['clang'] + target
objcpp = ['clang++'] + target
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PREFIX/lib/pkgconfig'

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
INI

# --- glib ------------------------------------------------------------------------
if [ ! -d "$SRC/glib" ]; then
    git clone --depth 1 --branch "$GLIB_VERSION" https://github.com/GNOME/glib "$SRC/glib"
fi
rm -rf "$WORK/glib-build"
meson setup "$WORK/glib-build" "$SRC/glib" \
    --cross-file "$CROSS" \
    --prefix "$PREFIX" \
    --buildtype release \
    -Ddefault_library=static \
    -Dtests=false \
    -Dnls=disabled \
    -Dintrospection=disabled \
    -Dglib_debug=disabled \
    -Dselinux=disabled \
    -Dxattr=false \
    -Dlibmount=disabled \
    -Dlibelf=disabled \
    -Dsysprof=disabled \
    -Ddtrace=disabled \
    -Dsystemtap=disabled \
    -Dman-pages=disabled
ninja -C "$WORK/glib-build"
ninja -C "$WORK/glib-build" install

echo
echo "Built for iOS (arm64):"
find "$PREFIX/lib" -maxdepth 1 -name '*.a' -exec sh -c 'printf "  %-28s %s\n" "$(basename "$1")" "$(lipo -archs "$1" 2>/dev/null)"' _ {} \;
