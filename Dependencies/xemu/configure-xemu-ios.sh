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

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
TARGET="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS"

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

# Only our prefix is visible to pkg-config, so nothing built for macOS leaks in.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --target-list=i386-softmmu \
    --cross-prefix= \
    --cc=clang --cxx=clang++ --objcc=clang \
    --extra-cflags="-DXBOX=1 $TARGET -I$PREFIX/include" \
    --extra-cxxflags="$TARGET -I$PREFIX/include" \
    --extra-objcflags="$TARGET -I$PREFIX/include" \
    --extra-ldflags="$TARGET -L$PREFIX/lib" \
    --enable-tcg \
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
    --disable-sdl \
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
