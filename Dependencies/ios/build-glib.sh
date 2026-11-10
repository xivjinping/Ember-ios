#!/bin/bash
# Cross-compiles GLib (and the libraries it needs: libffi, pcre2, libintl
# stub, zlib) as static libraries for iOS arm64.
#
# GLib is the first and largest dependency of the QEMU core that xemu is
# built on. Run on macOS with Xcode, meson, ninja and pkg-config installed:
#   Dependencies/ios/build-glib.sh
# Output goes to Dependencies/ios/prefix.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$ROOT/work}"
PREFIX="${PREFIX:-$ROOT/prefix}"
GLIB_VERSION="${GLIB_VERSION:-2.84.0}"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "iOS SDK: $SDK"
echo "meson $(meson --version), ninja $(ninja --version)"

mkdir -p "$WORK" "$PREFIX/lib/pkgconfig"
CROSS="$WORK/cross-ios-arm64.ini"
sed -e "s|@SDK@|$SDK|g" -e "s|@PREFIX@|$PREFIX|g" "$ROOT/cross-ios-arm64.ini.in" > "$CROSS"

SRC="$WORK/glib-$GLIB_VERSION"
if [ ! -d "$SRC" ]; then
    curl -fL --retry 3 -o "$WORK/glib.tar.xz" \
        "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz"
    tar -xf "$WORK/glib.tar.xz" -C "$WORK"
fi

BUILD="$WORK/glib-build"
rm -rf "$BUILD"
meson setup "$BUILD" "$SRC" \
    --cross-file "$CROSS" \
    --prefix "$PREFIX" \
    --force-fallback-for=libffi,pcre2,proxy-libintl,zlib \
    -Dtests=false \
    -Dnls=disabled \
    -Ddtrace=disabled \
    -Dsystemtap=disabled \
    -Dsysprof=disabled \
    -Dintrospection=disabled \
    -Dman-pages=disabled \
    -Ddocumentation=false \
    -Dselinux=disabled \
    -Dxattr=false \
    -Dlibmount=disabled \
    -Dlibelf=disabled \
    || { echo "---- meson-log.txt (tail) ----"; tail -n 120 "$BUILD/meson-logs/meson-log.txt" || true; exit 1; }

meson compile -C "$BUILD"
meson install -C "$BUILD" --no-rebuild > "$WORK/glib-install.log"

echo "---- installed libraries ----"
ls -la "$PREFIX/lib"/*.a
echo "---- target check (must say platform 2 = iOS) ----"
otool -l "$PREFIX/lib/libglib-2.0.a" | grep -A4 LC_BUILD_VERSION | head -6
lipo -info "$PREFIX/lib/libglib-2.0.a"
