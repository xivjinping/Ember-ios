#!/bin/bash
# Fetches the pinned xemu revision, applies Ember's iOS changes and runs
# xemu's configure for an iOS arm64 device against the libraries built by
# Dependencies/build-ios-deps.sh.
#
# Stage 3, step 1 of the port: get *configure* to pass. Compiling comes next.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$ROOT/build-deps"
PREFIX="$WORK/prefix"
SRC="$WORK/src/xemu"
BUILD="$WORK/xemu-build"
MIN_IOS="15.0"
COMMIT="$(tr -d '[:space:]' < "$HERE/XEMU_COMMIT")"
# jit (default): TCG writes native code into Ember's JIT region.
# nojit: TCG's bytecode interpreter (TCI), for devices without JIT. Much slower.
VARIANT="${EMBER_XEMU_VARIANT:-jit}"
TCI_FLAG=""
if [ "$VARIANT" = "nojit" ]; then TCI_FLAG="--enable-tcg-interpreter"; fi
echo "xemu variant: $VARIANT"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
TARGET="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS"
# Stand-ins for desktop-only headers (OpenGL types); see shims/.
SHIMS="-I$HERE/shims"
# Vulkan headers and library (Dependencies/xemu/fetch-moltenvk-ios.sh). The
# headers go on every compile line because xemu's bundled Vulkan helper
# libraries (volk and friends) expect to find them like system headers.
MOLTENVK="$WORK/moltenvk"
if [ ! -f "$MOLTENVK/lib/libMoltenVK-ember.a" ]; then
    echo "error: $MOLTENVK not prepared; run Dependencies/xemu/fetch-moltenvk-ios.sh" >&2
    exit 1
fi
SHIMS="$SHIMS -I$MOLTENVK/include"

# A pinned, pristine checkout every time: patches are applied to known text.
if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" remote add origin https://github.com/xemu-project/xemu
fi
git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
git -C "$SRC" checkout -q -f FETCH_HEAD
git -C "$SRC" clean -q -fdx
python3 "$HERE/patch_xemu.py" "$SRC"

# xemu would download a macOS build of the DSP library. Provide the iOS build
# (Dependencies/xemu/build-dsp56300-ios.sh) as the tarball it looks for first.
DSP_VERSION="$(sed -n "s/^project('dsp56300', version: '\(.*\)')/\1/p" "$SRC/subprojects/dsp56300/meson.build")"
DSP_TARBALL="$ROOT/build-deps/dsp56300/dsp56300-$DSP_VERSION-aarch64-apple-darwin.tar.gz"
if [ ! -f "$DSP_TARBALL" ]; then
    echo "error: $DSP_TARBALL not found; run Dependencies/xemu/build-dsp56300-ios.sh" >&2
    echo "       (xemu wants dsp56300 $DSP_VERSION)" >&2
    exit 1
fi
cp "$DSP_TARBALL" "$SRC/subprojects/dsp56300/"

# Only our prefix is visible to pkg-config, so nothing built for macOS leaks in.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --target-list=i386-softmmu \
    --cross-prefix= \
    --cc=clang --cxx=clang++ --objcc=clang \
    --extra-cflags="-DXBOX=1 $TARGET -I$PREFIX/include $SHIMS" \
    --extra-cxxflags="$TARGET -I$PREFIX/include $SHIMS" \
    --extra-objcflags="$TARGET -I$PREFIX/include $SHIMS" \
    --extra-ldflags="$TARGET -L$PREFIX/lib" \
    --enable-tcg $TCI_FLAG \
    --enable-pixman \
    --enable-slirp \
    --disable-werror \
    --disable-docs \
    --disable-tools \
    --disable-guest-agent \
    --disable-plugins \
    --disable-modules \
    --disable-rust \
    --disable-kvm \
    --disable-hvf \
    --disable-cocoa \
    --disable-coreaudio \
    --enable-sdl \
    --disable-gtk \
    --disable-vnc \
    --disable-curses \
    --disable-opengl \
    --disable-renderdoc \
    --disable-libusb \
    --disable-curl \
    --disable-capstone

# xemu generates its settings header with a Python script that reads YAML.
# configure builds its own Python environment, which does not include PyYAML.
"$BUILD/pyvenv/bin/python3" -m pip install --quiet pyyaml \
    || python3 -m pip install --quiet --break-system-packages pyyaml
"$BUILD/pyvenv/bin/python3" -c "import yaml; print('PyYAML', yaml.__version__)"

echo
echo "configure passed."
