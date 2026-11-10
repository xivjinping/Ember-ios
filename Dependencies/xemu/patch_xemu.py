#!/usr/bin/env python3
"""Applies Ember's iOS changes to a checkout of xemu.

    python3 patch_xemu.py /path/to/xemu

xemu is written for desktop systems: its build has no notion of iOS and
pulls in OpenGL, Cocoa, SDL and packet capture unconditionally. Each edit
below is a small, targeted replacement that either teaches the build about
iOS or makes a desktop-only piece optional. Every edit asserts that the text
it expects is present, so a change upstream makes this script fail loudly at
that edit instead of producing a silently wrong build.

The pinned upstream revision is in XEMU_COMMIT. xemu is GPL-2.0; so are
these modifications.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1])
applied = 0


def edit(relative, old, new, count=1):
    global applied
    path = root / relative
    text = path.read_text()
    found = text.count(old)
    if found < count:
        sys.exit(f"patch_xemu: {relative}: expected text not found ({found}/{count}):\n{old}")
    path.write_text(text.replace(old, new, count))
    applied += 1


# --- 1. Know when the target is iOS -------------------------------------------
# Meson reports iOS as 'darwin', the same as macOS, so ask the compiler.
edit("meson.build",
     "cc = meson.get_compiler('c')\n",
     "cc = meson.get_compiler('c')\n"
     "host_ios = host_os == 'darwin' and cc.compiles('''\n"
     "  #include <TargetConditionals.h>\n"
     "  #if !TARGET_OS_IPHONE\n"
     "  #error not an iOS target\n"
     "  #endif\n"
     "  int main(void) { return 0; }''', name: 'target is iOS')\n")

# --- 2. OpenGL is optional -------------------------------------------------------
# iOS has no desktop OpenGL and no libepoxy; the first iOS bring-up uses the
# null renderer.
edit("meson.build",
     "epoxy = dependency('epoxy', required: true)",
     "epoxy = dependency('epoxy', required: get_option('opengl').enabled())")
edit("meson.build",
     "if not get_option('opengl').auto() or have_system or have_vhost_user_gpu\n  # FIXME: Use meson's 'gl' dep",
     "if get_option('opengl').allowed() and (not get_option('opengl').auto() or have_system or have_vhost_user_gpu)\n"
     "  # FIXME: Use meson's 'gl' dep")

# --- 3. Packet capture is optional -----------------------------------------------
edit("meson.build",
     "libpcap = dependency('libpcap', method: 'pkg-config', required: true)",
     "libpcap = dependency('libpcap', method: 'pkg-config', required: not host_ios)")
edit("net/meson.build",
     "system_ss.add([libpcap, files('pcap.c')])",
     "if libpcap.found()\n  system_ss.add([libpcap, files('pcap.c')])\nendif")

# --- 4. No Cocoa on iOS ------------------------------------------------------------
edit("ui/meson.build",
     "if host_os == 'darwin'\n  xemu_cocoa = dependency('appleframeworks', modules: 'Cocoa')",
     "if host_os == 'darwin' and not host_ios\n  xemu_cocoa = dependency('appleframeworks', modules: 'Cocoa')")
edit("ui/meson.build",
     "elif host_os == 'darwin'\n  xemu_ss.add(files('xemu-os-utils-macos.m'))",
     "elif host_os == 'darwin' and not host_ios\n  xemu_ss.add(files('xemu-os-utils-macos.m'))")

print(f"patch_xemu: {applied} edits applied")
