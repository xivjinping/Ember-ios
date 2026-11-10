#!/bin/bash
# Cross-compiles the libraries an x86 engine (xemu / QEMU) needs, for iOS devices.
#
#   Dependencies/build-ios-deps.sh            # builds everything into build-deps/prefix
#
# Requires macOS with Xcode, plus: brew install meson ninja pkgconf
# Output: static libraries, headers and .pc files under build-deps/prefix,
# which later stages point pkg-config at.
#
# Libraries so far: glib (+ libffi, pcre2, libintl stub), pixman, libsamplerate, SDL3.
# Each one is skipped if its pkg-config file is already in the prefix.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-deps"
PREFIX="$WORK/prefix"
SRC="$WORK/src"
MIN_IOS="15.0"
GLIB_VERSION="2.88.4"
PIXMAN_VERSION="0.44.2"
SAMPLERATE_VERSION="0.2.2"
# The SDL revision xemu itself pins (subprojects/sdl3.wrap at XEMU_COMMIT).
SDL_REVISION="829a65d769d935c4852f8159e964312c0957260a"

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

have() { [ -f "$PREFIX/lib/pkgconfig/$1.pc" ]; }

# --- glib ------------------------------------------------------------------------
if ! have glib-2.0; then
    [ -d "$SRC/glib" ] || git clone --depth 1 --branch "$GLIB_VERSION" https://github.com/GNOME/glib "$SRC/glib"
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
fi

# --- pixman (software pixel operations; QEMU's display core is built on it) ------
if ! have pixman-1; then
    [ -d "$SRC/pixman" ] || git clone --depth 1 --branch "pixman-$PIXMAN_VERSION" \
        https://gitlab.freedesktop.org/pixman/pixman.git "$SRC/pixman"
    rm -rf "$WORK/pixman-build"
    # The hand-written NEON assembly targets GNU as; Apple's assembler rejects
    # parts of it, so the C paths are used for now.
    meson setup "$WORK/pixman-build" "$SRC/pixman" \
        --cross-file "$CROSS" \
        --prefix "$PREFIX" \
        --buildtype release \
        -Ddefault_library=static \
        -Dtests=disabled \
        -Ddemos=disabled \
        -Dgtk=disabled \
        -Dlibpng=disabled \
        -Dopenmp=disabled \
        -Dneon=disabled \
        -Da64-neon=disabled \
        -Darm-simd=disabled
    ninja -C "$WORK/pixman-build"
    ninja -C "$WORK/pixman-build" install
fi

# --- libsamplerate (audio resampling; used by the console's audio processor) -----
if ! have samplerate; then
    [ -d "$SRC/libsamplerate" ] || git clone --depth 1 --branch "$SAMPLERATE_VERSION" \
        https://github.com/libsndfile/libsamplerate "$SRC/libsamplerate"
    rm -rf "$WORK/samplerate-build"
    cmake -S "$SRC/libsamplerate" -B "$WORK/samplerate-build" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DLIBSAMPLERATE_EXAMPLES=OFF \
        -DLIBSAMPLERATE_INSTALL=ON
    cmake --build "$WORK/samplerate-build"
    cmake --install "$WORK/samplerate-build"
fi

# --- SDL3 (xemu's audio output and controller types are written against it) ------
# SDL supports iOS directly. Ember keeps its own window and UI; SDL is used
# for the parts of the core that expect it (audio streams, gamepad types).
if ! have sdl3; then
    if [ ! -d "$SRC/SDL/.git" ]; then
        mkdir -p "$SRC/SDL"
        git -C "$SRC/SDL" init -q
        git -C "$SRC/SDL" remote add origin https://github.com/libsdl-org/SDL
    fi
    git -C "$SRC/SDL" fetch -q --depth 1 origin "$SDL_REVISION"
    git -C "$SRC/SDL" checkout -q -f FETCH_HEAD
    rm -rf "$WORK/sdl-build"
    cmake -S "$SRC/SDL" -B "$WORK/sdl-build" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF
    cmake --build "$WORK/sdl-build"
    cmake --install "$WORK/sdl-build"
fi

echo
echo "Built for iOS (arm64):"
find "$PREFIX/lib" -maxdepth 1 -name '*.a' -exec sh -c 'printf "  %-28s %s\n" "$(basename "$1")" "$(lipo -archs "$1" 2>/dev/null)"' _ {} \;
echo "pkg-config modules:"
ls "$PREFIX/lib/pkgconfig" | sed 's/^/  /'
