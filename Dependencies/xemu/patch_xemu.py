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

# --- 5. No desktop UI toolkit on iOS -------------------------------------------------
# xemu's menus and overlays are drawn with Dear ImGui on SDL and OpenGL.
# Ember's SwiftUI front end replaces all of it, so the toolkit is not built.
edit("meson.build",
     "imgui_proj = subproject('imgui', required: true,",
     "if host_ios\n  imgui = not_found\n  implot = not_found\nelse\n"
     "imgui_proj = subproject('imgui', required: true,")
edit("meson.build",
     "implot = implot_proj.get_variable('implot_dep')\n",
     "implot = implot_proj.get_variable('implot_dep')\nendif\n")
edit("ui/meson.build",
     "subdir('xui')\n",
     "if not host_ios\n  subdir('xui')\nendif\n")
# The desktop front end (window, menus, SDL input, settings UI) stays out of
# the iOS build. What the core still needs from it is supplied by Ember.
# The parts with no window or OpenGL in them are kept as they are: settings
# (the core reads its machine configuration from them), networking setup,
# the monitor console and the widescreen flag.
edit("ui/meson.build",
     "system_ss.add_all(xemu_ss)",
     "if host_ios\n"
     "  system_ss.add(files('xemu-settings.cc', 'xemu-net.c', 'xemu-monitor.c',\n"
     "                      'xemu-widescreen.c'),\n"
     "                sdl, tomlplusplus, genconfig)\n"
     "else\n  system_ss.add_all(xemu_ss)\nendif")

# --- 6. Let Meson find CMake when cross-compiling --------------------------------------
# One of xemu's components (the vertex-shader CPU emulator) is a CMake
# project. In a cross build Meson only uses tools named in the cross file,
# and configure does not list cmake there.
edit("configure",
     '  echo "pkgconfig = [$(meson_quote $pkg_config)]" >> $cross\n',
     '  echo "pkgconfig = [$(meson_quote $pkg_config)]" >> $cross\n'
     '  echo "cmake = [\'cmake\']" >> $cross\n')

# --- 7. No OpenGL renderer on iOS ----------------------------------------------------------
# The offscreen-GL helper and the OpenGL renderer (with its GLSL generator)
# need desktop OpenGL through SDL. The Vulkan renderer is already optional.
edit("hw/xbox/nv2a/pgraph/thirdparty/meson.build",
     "libgloffscreen = static_library(",
     "if host_ios\n  gloffscreen = not_found\nelse\nlibgloffscreen = static_library(")
edit("hw/xbox/nv2a/pgraph/thirdparty/meson.build",
     "include_directories: ['gloffscreen'])\n",
     "include_directories: ['gloffscreen'])\nendif\n")
edit("hw/xbox/nv2a/pgraph/meson.build",
     "subdir('gl')\nsubdir('glsl')\n",
     "if not host_ios\n  subdir('gl')\n  subdir('glsl')\nendif\n")

# --- 8. JIT write protection ----------------------------------------------------------------
# On macOS, QEMU flips a per-thread switch between writing and executing JIT
# pages. That call does not exist on iOS. Ember gives the code generator two
# views of its buffer instead (one writable, one executable), so on iOS the
# switch has nothing to do.
edit("include/qemu/osdep.h",
     "#ifdef __APPLE__\nstatic inline void qemu_thread_jit_execute(void)",
     "#if defined(__APPLE__) && !defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)\n"
     "static inline void qemu_thread_jit_execute(void)")

# --- 9. statfs on iOS ---------------------------------------------------------------------------
# The header that declares it was only included together with the macOS
# CD/DVD device headers, which iOS does not have.
edit("block/file-posix.c",
     "#if defined(__APPLE__) && (__MACH__)\n",
     "#if defined(__APPLE__) && (__MACH__)\n#include <sys/param.h>\n#include <sys/mount.h>\n")

# --- 10. Null renderer without OpenGL ------------------------------------------------------------
# Even the renderer that draws nothing creates an offscreen OpenGL context
# and hands the display a 1x1 black OpenGL texture. Without OpenGL it keeps
# no context and reports "no framebuffer texture" (0).
edit("hw/xbox/nv2a/pgraph/null/renderer.c",
     '#include "gloffscreen.h"\n\n'
     'static GloContext *g_null_gl_context;\n\n'
     'static void early_context_init(void)\n{\n'
     '    g_null_gl_context = glo_context_create();\n}\n',
     '#ifdef CONFIG_OPENGL\n'
     '#include "gloffscreen.h"\n\n'
     'static GloContext *g_null_gl_context;\n#endif\n\n'
     'static void early_context_init(void)\n{\n'
     '#ifdef CONFIG_OPENGL\n'
     '    g_null_gl_context = glo_context_create();\n#endif\n}\n')
edit("hw/xbox/nv2a/pgraph/null/renderer.c",
     '        if (r->blank_texture) {\n'
     '            glo_set_current(g_null_gl_context);\n'
     '            glDeleteTextures(1, &r->blank_texture);\n'
     '            glo_set_current(NULL);\n'
     '        }\n',
     '#ifdef CONFIG_OPENGL\n'
     '        if (r->blank_texture) {\n'
     '            glo_set_current(g_null_gl_context);\n'
     '            glDeleteTextures(1, &r->blank_texture);\n'
     '            glo_set_current(NULL);\n'
     '        }\n'
     '#endif\n')
edit("hw/xbox/nv2a/pgraph/null/renderer.c",
     '    if (r) {\n        if (!r->blank_texture) {\n            static const uint8_t black_pixel',
     '#ifdef CONFIG_OPENGL\n    if (r) {\n        if (!r->blank_texture) {\n            static const uint8_t black_pixel')
edit("hw/xbox/nv2a/pgraph/null/renderer.c",
     '        return r->blank_texture;\n    }\n\n    return 0;',
     '        return r->blank_texture;\n    }\n#else\n    (void)r;\n#endif\n\n    return 0;')

# --- 11. zlib from the iOS SDK ---------------------------------------------------------------------
# With CMake visible (edit 6), Meson asks CMake for zlib, and CMake answers
# with the macOS SDK's copy, which cannot be linked into an iOS binary. The
# "system" method links plain -lz, which resolves inside the iOS SDK.
edit("meson.build",
     "zlib = dependency('zlib', required: true)",
     "zlib = dependency('zlib', required: true, method: host_ios ? 'system' : 'auto')")

print(f"patch_xemu: {applied} edits applied")
