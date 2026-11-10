#!/bin/bash
# Builds xemu's DSP56300 emulator (a Rust static library) for iOS arm64.
#
# xemu's subprojects/dsp56300/meson.build downloads a prebuilt library from
# the project's GitHub releases. Releases exist for macOS, Linux and Windows
# only, so a macOS library ends up in the iOS link and the linker refuses it.
# This script builds the same release from source for aarch64-apple-ios and
# packs it in the tarball layout meson.build expects. configure-xemu-ios.sh
# drops that tarball next to meson.build so the download is skipped.
#
# The version must match `project('dsp56300', version: ...)` in xemu's
# subprojects/dsp56300/meson.build; configure-xemu-ios.sh checks that.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$ROOT/build-deps/dsp56300"
VERSION="0.1.3"
RUST_TARGET="aarch64-apple-ios"
MIN_IOS="15.0"

# meson.build names the package after the *host OS* it detects, which is
# "darwin" for iOS too, so the tarball carries the macOS name.
PKG="dsp56300-$VERSION-aarch64-apple-darwin"

rm -rf "$WORK"
mkdir -p "$WORK/src"
cd "$WORK/src"
git init -q
git remote add origin https://github.com/mborgerson/dsp56300
git fetch -q --depth 1 origin "refs/tags/v$VERSION"
git checkout -q FETCH_HEAD

# rust-toolchain.toml pins the compiler; add the iOS target to that one.
TOOLCHAIN="$(sed -n 's/^channel *= *"\(.*\)"/\1/p' rust-toolchain.toml)"
rustup toolchain install "$TOOLCHAIN" --profile minimal --target "$RUST_TARGET"

export IPHONEOS_DEPLOYMENT_TARGET="$MIN_IOS"
cargo +"$TOOLCHAIN" build --release --target "$RUST_TARGET" -p dsp56300-emu-ffi

mkdir -p "$WORK/$PKG/lib" "$WORK/$PKG/include"
cp "target/$RUST_TARGET/release/libdsp56300_emu_ffi.a" "$WORK/$PKG/lib/"
cp crates/emu-ffi/include/dsp56300.h "$WORK/$PKG/include/"
tar czf "$WORK/$PKG.tar.gz" -C "$WORK" "$PKG"

# Show that the library really is for iOS (platform 2), not macOS (platform 1).
vtool -show-build "$WORK/$PKG/lib/libdsp56300_emu_ffi.a" 2>/dev/null | grep -E 'platform|minos' | sort | uniq -c || true
ls -la "$WORK/$PKG.tar.gz"
