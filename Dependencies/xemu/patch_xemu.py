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

# --- 7. Ember's replacements for the excluded desktop front end -----------------------
# ember-frontend.c: controller state, notifications, snapshot hooks and the
#   packet-capture client, which the core still calls.
# ember-core.c: starts the machine on its own thread and provides the
#   "xemu" display that vl.c asks for, without a window.
# ember-xemu.h: the C interface the app uses.
# ember-exe-main.c: main() for the executable only (see that file).
import shutil
for name in ("ember-frontend.c", "ember-core.c", "ember-xemu.h", "ember-exe-main.c"):
    shutil.copy(Path(__file__).parent / "ember" / name, root / "ui" / name)
shutil.copy(Path(__file__).parent / "ember" / "coroutine-ember.c", root / "util" / "coroutine-ember.c")
edit("ui/meson.build",
     "                      'xemu-widescreen.c'),\n",
     "                      'xemu-widescreen.c', 'ember-frontend.c', 'ember-core.c'),\n")
edit("meson.build",
     "      'sources': files('system/main.c'),\n      'dependencies': [sdl]\n    }]",
     "      'sources': files('system/main.c') + (host_ios ? files('ui/ember-exe-main.c') : []),\n"
     "      'dependencies': [sdl]\n    }]")

# --- 8. Static slirp -----------------------------------------------------------------------
# The bundled user-mode network library is built as a .dylib by default; the
# app links everything statically.
edit("meson.build",
     "  slirp = dependency('slirp', required: get_option('slirp'),\n",
     "  slirp = dependency('slirp', required: get_option('slirp'),\n"
     "                     default_options: ['default_library=static'],\n")

# --- 9. Coroutines on iOS ------------------------------------------------------------------
# sigaltstack() fails silently on iOS and there is no ucontext, so neither
# stock backend works. See ember/coroutine-ember.c.
edit("util/meson.build",
     "  util_ss.add(files(f'coroutine-@coroutine_backend@.c'))",
     "  if host_ios\n"
     "    util_ss.add(files('coroutine-ember.c'))\n"
     "  else\n"
     "    util_ss.add(files(f'coroutine-@coroutine_backend@.c'))\n"
     "  endif")

# --- 10. The code generator writes into Ember's JIT region ------------------------------------
# iOS gives a sideloaded app executable memory only through the debugger
# protocol in docs/JIT.md, and only before the debugger detaches. The app
# allocates one region that way (an executable view and a writable view of
# the same pages) and passes it in; TCG then uses it as a split-wx buffer
# of exactly that size instead of mapping its own.
edit("tcg/region.c",
     "static int alloc_code_gen_buffer(size_t size, int splitwx, Error **errp)\n"
     "{\n"
     "    ERRP_GUARD();\n"
     "    int prot, flags;\n",
     "static void *ember_code_rx, *ember_code_rw;\n"
     "static size_t ember_code_size;\n"
     "\n"
     "void tcg_ember_set_code_buffer(void *rx, void *rw, size_t size);\n"
     "void tcg_ember_set_code_buffer(void *rx, void *rw, size_t size)\n"
     "{\n"
     "    ember_code_rx = rx;\n"
     "    ember_code_rw = rw;\n"
     "    ember_code_size = size;\n"
     "}\n"
     "\n"
     "static int alloc_code_gen_buffer(size_t size, int splitwx, Error **errp)\n"
     "{\n"
     "    ERRP_GUARD();\n"
     "    int prot, flags;\n"
     "\n"
     "    if (ember_code_size) {\n"
     "        region.start_aligned = ember_code_rw;\n"
     "        region.total_size = ember_code_size;\n"
     "        tcg_splitwx_diff = (char *)ember_code_rx - (char *)ember_code_rw;\n"
     "        return PROT_READ | PROT_WRITE;\n"
     "    }\n")
edit("tcg/region.c",
     "    /* Size the buffer.  */\n    if (tb_size == 0) {",
     "    /* Ember: the buffer is the size of the region the app prepared. */\n"
     "    if (ember_code_size) {\n"
     "        tb_size = ember_code_size;\n"
     "    }\n"
     "\n"
     "    /* Size the buffer.  */\n    if (tb_size == 0) {")
# Guard pages would change the protection of pages inside the prepared
# region; they are optional, so leave the region as it was handed over.
edit("tcg/region.c",
     "        if (have_prot != 0) {\n"
     "            /* Guard pages are nice for bug detection but are not essential. */",
     "        if (have_prot != 0 && !ember_code_size) {\n"
     "            /* Guard pages are nice for bug detection but are not essential. */")

print(f"patch_xemu: {applied} edits applied")
