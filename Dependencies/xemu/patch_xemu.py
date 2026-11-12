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


def edit_span(relative, start, end, new):
    """Replaces everything from `start` through `end` (both included)."""
    global applied
    path = root / relative
    text = path.read_text()
    a = text.find(start)
    b = text.find(end, a) if a >= 0 else -1
    if a < 0 or b < 0:
        sys.exit(f"patch_xemu: {relative}: span not found:\n{start}\n...\n{end}")
    path.write_text(text[:a] + new + text[b + len(end):])
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
# The offscreen-GL helper and the OpenGL renderer need desktop OpenGL through
# SDL. The GLSL generator is kept: the Vulkan renderer uses it (section 11).
edit("hw/xbox/nv2a/pgraph/thirdparty/meson.build",
     "libgloffscreen = static_library(",
     "if host_ios\n  gloffscreen = not_found\nelse\nlibgloffscreen = static_library(")
edit("hw/xbox/nv2a/pgraph/thirdparty/meson.build",
     "include_directories: ['gloffscreen'])\n",
     "include_directories: ['gloffscreen'])\nendif\n")
edit("hw/xbox/nv2a/pgraph/meson.build",
     "subdir('gl')\nsubdir('glsl')\n",
     "if not host_ios\n  subdir('gl')\nendif\nsubdir('glsl')\n")

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

# The interpreter (no-JIT) build allocates its bytecode buffer as ordinary
# memory; iOS refuses MAP_JIT to an app without JIT.
edit("tcg/region.c",
     "    flags = MAP_PRIVATE | MAP_ANONYMOUS;\n#ifdef CONFIG_DARWIN\n",
     "    flags = MAP_PRIVATE | MAP_ANONYMOUS;\n"
     "#if defined(CONFIG_DARWIN) && !defined(CONFIG_TCG_INTERPRETER)\n")

# This xemu revision handles every op through the TCGOutOp tables; the
# native backends keep tcg_out_op() as an unreachable stub, which the
# interpreter backend lost. Without it the TCI build does not link.
edit("tcg/tci/tcg-target.c.inc",
     "static bool tcg_out_qemu_st_slow_path(TCGContext *s, TCGLabelQemuLdst *l)\n"
     "{\n    g_assert_not_reached();\n}\n",
     "static bool tcg_out_qemu_st_slow_path(TCGContext *s, TCGLabelQemuLdst *l)\n"
     "{\n    g_assert_not_reached();\n}\n"
     "\n"
     "static inline void tcg_out_op(TCGContext *s, TCGOpcode opc, TCGType type,\n"
     "                              const TCGArg args[TCG_MAX_OP_ARGS],\n"
     "                              const int const_args[TCG_MAX_OP_ARGS])\n"
     "{\n    g_assert_not_reached();\n}\n")

# --- 11. The Vulkan renderer on MoltenVK ------------------------------------------------------
# iOS has no Vulkan; MoltenVK implements it on Metal. fetch-moltenvk-ios.sh
# puts its headers and a static library (with every Vulkan symbol hidden but
# one entry point, so they cannot clash with volk's) in build-deps/moltenvk.
# The renderer then needs four kinds of change:
#   a. build: where Vulkan comes from on iOS;
#   b. no OpenGL interop: the picture is read back, not shared with GL;
#   c. no geometry shaders (Metal has none): depth comes from interpolated
#      values, and quads/fans are expanded on the CPU (ember/vk-draw-ember.h);
#   d. device requirements that MoltenVK cannot meet are not demanded.
MVK = Path(__file__).resolve().parent.parent.parent / "build-deps" / "moltenvk"
mvk_link = ", ".join(f"'{a}'" for a in (
    [str(MVK / "lib" / "libMoltenVK-ember.a")] +
    [f"-Wl,-framework,{f}" for f in ("Metal", "Foundation", "QuartzCore", "CoreGraphics", "IOSurface", "UIKit")]))

# a. build
edit("meson.build",
     "elif host_os == 'linux'\n  vulkan = dependency('vulkan')\nendif\n",
     "elif host_os == 'linux'\n  vulkan = dependency('vulkan')\n"
     "elif host_ios\n"
     f"  vulkan = declare_dependency(compile_args: ['-I{MVK / 'include'}'],\n"
     f"                              link_args: [{mvk_link}])\n"
     "endif\n")
shutil.copy(Path(__file__).parent / "ember" / "vk-draw-ember.h",
            root / "hw" / "xbox" / "nv2a" / "pgraph" / "vk" / "vk-draw-ember.h")

# b. no OpenGL interop
edit("hw/xbox/nv2a/pgraph/vk/renderer.h",
     "#define HAVE_EXTERNAL_MEMORY 1",
     "#include <epoxy/gl.h> /* Ember: the shim; only the GL type names used below */\n"
     "#define HAVE_EXTERNAL_MEMORY 0")
edit("hw/xbox/nv2a/pgraph/vk/renderer.c",
     '#include "gloffscreen.h"\n', '')
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    const GLint gl_internal_format = GL_RGBA8;\n", "")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    image_create_info.pNext = &external_memory_image_create_info;\n",
     "    (void)external_memory_image_create_info;\n")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    alloc_info.pNext = &export_memory_alloc_info;\n",
     "    (void)export_memory_alloc_info;\n")
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "static char const *const required_device_extensions[] = {\n#ifdef WIN32\n",
     "static char const *const required_device_extensions[] = {\n"
     "    NULL, /* Ember: none; the external memory extensions were for OpenGL interop */\n"
     "#if 0\n#elif defined(WIN32)\n")
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
     "    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,\n#endif\n",
     "#endif\n")
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "                          ARRAY_SIZE(required_device_extensions));\n\n"
     "    g_array_append_vals(extensions, required_device_extensions,\n"
     "                        ARRAY_SIZE(required_device_extensions));\n",
     "                          ARRAY_SIZE(required_device_extensions));\n")
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "    for (int i = 0; i < ARRAY_SIZE(required_device_extensions); i++) {\n",
     "    for (int i = 0; i < 0; i++) {\n")

# MoltenVK is linked in; there is no Vulkan loader library to open.
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "    result = volkInitialize();\n",
     "    extern PFN_vkVoidFunction ember_mvk_get_instance_proc_addr(VkInstance, const char *);\n"
     "    volkInitializeCustom((PFN_vkGetInstanceProcAddr)ember_mvk_get_instance_proc_addr);\n"
     "    result = VK_SUCCESS;\n")

# d. device requirements
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "        F(geometryShader, true),", "        F(geometryShader, false),")
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "        F(shaderTessellationAndGeometryPointSize, true),",
     "        F(shaderTessellationAndGeometryPointSize, false),")
# A missing format is reported but no longer rules the device out: there is
# only one device, and the log says which format to deal with.
edit("hw/xbox/nv2a/pgraph/vk/instance.c",
     "           check_device_support_required_extensions(device) &&\n"
     "           check_device_support_required_formats(device);\n",
     "           check_device_support_required_extensions(device) &&\n"
     "           (check_device_support_required_formats(device), true);\n")
# Two 838 MB scratch buffers are sized for 10x rendering scale.
edit("hw/xbox/nv2a/pgraph/vk/buffer.c",
     "        .buffer_size = (1024 * 10) * (1024 * 10) * 8,",
     "        .buffer_size = 4096 * 4096 * 8,")
# If Vulkan cannot start, keep the console running without a picture.
edit("hw/xbox/nv2a/pgraph/pgraph.c",
     "    // FIXME: Try others\n",
     "    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;\n"
     "    if (attempt_renderer_init(pg)) {\n"
     "        fprintf(stderr, \"ember: the Vulkan renderer could not start; no picture (null renderer)\\n\");\n"
     "        return;\n"
     "    }\n")
edit("hw/xbox/nv2a/pgraph/pgraph.c",
     "void pgraph_init_thread(NV2AState *d)\n{\n    init_renderer(&d->pgraph);\n}\n",
     "static bool ember_renderer_ready;\n\n"
     "bool nv2a_ember_renderer_ready(void);\n"
     "bool nv2a_ember_renderer_ready(void)\n{\n"
     "    return qatomic_read(&ember_renderer_ready);\n}\n\n"
     "void pgraph_init_thread(NV2AState *d)\n{\n    init_renderer(&d->pgraph);\n"
     "    fprintf(stderr, \"ember: renderer: %s\\n\", d->pgraph.renderer->name);\n"
     "    qatomic_set(&ember_renderer_ready, true);\n}\n")

# c. no geometry shaders
edit("hw/xbox/nv2a/pgraph/glsl/geom.c",
     "bool pgraph_glsl_need_geom(const GeomState *state)\n{\n",
     "bool pgraph_glsl_need_geom(const GeomState *state)\n{\n"
     "    return false; /* Ember: Metal has no geometry shaders */\n\n")
# The start-up probe measures how the driver orders vertices handed to a
# geometry shader.
edit("hw/xbox/nv2a/pgraph/vk/gpuprops.c",
     "void pgraph_vk_determine_gpu_properties(NV2AState *d)\n{\n",
     "void pgraph_vk_determine_gpu_properties(NV2AState *d)\n{\n    return;\n\n")
# Depth. The geometry shader gave the fragment shader the triangle's three
# corners so it could compute exact depth itself. Without it, two of those
# outputs carry values that the rasteriser interpolates linearly across the
# screen instead: vtxPos0 the position (z is linear on screen) and vtxPos1
# 1/w (also linear on screen, and w is what a w-buffer stores). The slope
# used for polygon offset comes from screen-space derivatives.
edit("hw/xbox/nv2a/pgraph/glsl/common.c",
     '        { flat_s,      vec4_s,  "vtxPos0" },\n'
     '        { flat_s,      vec4_s,  "vtxPos1" },\n',
     '        { "noperspective ", vec4_s,  "vtxPos0" },\n'
     '        { "noperspective ", vec4_s,  "vtxPos1" },\n')
edit("hw/xbox/nv2a/pgraph/glsl/vsh.c",
     '                   "  vtxPos1 = vtxPos;\\n"\n',
     '                   "  vtxPos1 = vec4(1.0 / vtxPos.w);\\n"\n')
edit_span("hw/xbox/nv2a/pgraph/glsl/psh.c",
     '            "precise float bc0 = area(unscaled_xy, vtxPos1.xy, vtxPos2.xy);\\n"',
     '            "precise float zvalue = vtxPos0.w + (bc1*(vtxPos1.w - vtxPos0.w) + bc2*(vtxPos2.w - vtxPos0.w));\\n"',
     '            "float emberInvW = vtxPos1.x;\\n"\n'
     '            "float emberMZ = max(abs(dFdx(emberInvW)), abs(dFdy(emberInvW))) * float(surfaceScale.x);\\n"\n'
     '            "precise float zvalue = 1.0 / emberInvW;\\n"')
edit("hw/xbox/nv2a/pgraph/glsl/psh.c",
     "depthFactor*triMZ*zvalue*zvalue", "depthFactor*emberMZ*zvalue*zvalue")
edit_span("hw/xbox/nv2a/pgraph/glsl/psh.c",
     '            "precise float bc0 = area(unscaled_xy, vtxPos1.xy, vtxPos2.xy);\\n"',
     '            "precise float zvalue = vtxPos0.z + (bc1*(vtxPos1.z - vtxPos0.z) + bc2*(vtxPos2.z - vtxPos0.z));\\n"',
     '            "float emberMZ = max(abs(dFdx(vtxPos0.z)), abs(dFdy(vtxPos0.z))) * float(surfaceScale.x);\\n"\n'
     '            "precise float zvalue = vtxPos0.z;\\n"')
edit("hw/xbox/nv2a/pgraph/glsl/psh.c",
     '"zvalue += depthFactor*triMZ;\\n"', '"zvalue += depthFactor*emberMZ;\\n"')

# Primitive types Metal lacks become triangle lists (ember/vk-draw-ember.h).
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "    case PRIM_TYPE_TRIANGLE_FAN:\n        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;\n"
     "    case PRIM_TYPE_QUADS:\n        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;\n"
     "    case PRIM_TYPE_QUAD_STRIP:\n        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;\n"
     "    case PRIM_TYPE_POLYGON:\n",
     "    case PRIM_TYPE_TRIANGLE_FAN:\n    case PRIM_TYPE_QUADS:\n    case PRIM_TYPE_QUAD_STRIP:\n"
     "    case PRIM_TYPE_POLYGON:\n        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;\n"
     "    case PRIM_TYPE_INVALID:\n")
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "void pgraph_vk_flush_draw(NV2AState *d)\n{\n",
     '#include "vk-draw-ember.h"\n\nvoid pgraph_vk_flush_draw(NV2AState *d)\n{\n')
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "    r->num_vertex_ram_buffer_syncs = 0;\n\n    if (pg->draw_arrays_length) {\n",
     "    r->num_vertex_ram_buffer_syncs = 0;\n    ember_prims_begin(pg);\n\n    if (pg->draw_arrays_length) {\n")
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "        begin_pre_draw(pg);\n",
     "        begin_pre_draw(pg);\n        ember_prims_upload(pg);\n", count=4)
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "            vkCmdDraw(r->command_buffer, count, 1, start, 0);\n",
     "            if (!ember_prims_draw(pg, i)) {\n"
     "                vkCmdDraw(r->command_buffer, count, 1, start, 0);\n"
     "            }\n")
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "        vkCmdDrawIndexed(r->command_buffer, pg->inline_elements_length, 1, 0, 0,\n"
     "                         0);\n",
     "        if (!ember_prims_draw(pg, 0)) {\n"
     "            vkCmdDrawIndexed(r->command_buffer, pg->inline_elements_length, 1, 0, 0, 0);\n"
     "        }\n")
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "        vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);\n",
     "        if (!ember_prims_draw(pg, 0)) {\n"
     "            vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);\n"
     "        }\n")
edit("hw/xbox/nv2a/pgraph/vk/draw.c",
     "        vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);\n",
     "        if (!ember_prims_draw(pg, 0)) {\n"
     "            vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);\n"
     "        }\n")

# ---- 12. Display image to the app at the internal resolution -------------------
#
# xemu draws the displayed frame into a Vulkan image at the chosen surface
# scale. On the desktop OpenGL shows that image; here it is copied to host
# memory after each display pass (ember/vk-display-ember.h). The frame thread
# asks for it through the same "sync" request the desktop build uses.
shutil.copy(Path(__file__).parent / "ember" / "vk-display-ember.h",
            root / "hw" / "xbox" / "nv2a" / "pgraph" / "vk" / "vk-display-ember.h")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     '#include <math.h>\n',
     '#include <math.h>\n#include "vk-display-ember.h"\n')
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,",
     "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |\n"
     "                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    pgraph_vk_transition_image_layout(pg, cmd, disp->image,\n"
     "                                      VK_FORMAT_R8G8B8_UNORM,\n"
     "                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,",
     "    ember_display_record_copy(pg, cmd);\n\n"
     "    pgraph_vk_transition_image_layout(pg, cmd, disp->image,\n"
     "                                      VK_FORMAT_R8G8B8_UNORM,\n"
     "                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);\n",
     "    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);\n"
     "    ember_display_deliver(pg);\n")
# The frame thread's request renders the display (sync) instead of only
# downloading the surface into guest memory.
edit_span("hw/xbox/nv2a/pgraph/vk/renderer.c",
     "#if HAVE_EXTERNAL_MEMORY\n    qemu_event_reset(&d->pgraph.sync_complete);",
     "    return 0;\n#endif\n",
     "    extern int ember_display_from_renderer; /* ui/ember-core.c */\n"
     "    if (!ember_display_from_renderer) {\n"
     "        qemu_mutex_unlock(&d->pfifo.lock);\n"
     "        pgraph_vk_wait_for_surface_download(surface);\n"
     "        return 0;\n"
     "    }\n"
     "    qemu_event_reset(&d->pgraph.sync_complete);\n"
     "    qatomic_set(&pg->sync_pending, true);\n"
     "    pfifo_kick(d);\n"
     "    qemu_mutex_unlock(&d->pfifo.lock);\n"
     "    qemu_event_wait(&d->pgraph.sync_complete);\n"
     "    return 0;\n")
# Where a frame request goes, for the log (first few only).
edit("hw/xbox/nv2a/pgraph/vk/renderer.c",
     "    PGRAPHState *pg = &d->pgraph;\n    pgraph_vk_render_display(pg);\n",
     "    PGRAPHState *pg = &d->pgraph;\n"
     "    static int ember_syncs;\n"
     "    if (ember_syncs < 3) fprintf(stderr, \"ember: display: sync %d begins\\n\", ember_syncs);\n"
     "    pgraph_vk_render_display(pg);\n"
     "    if (ember_syncs < 3) fprintf(stderr, \"ember: display: sync %d done\\n\", ember_syncs);\n"
     "    ember_syncs++;\n")
edit("hw/xbox/nv2a/pgraph/vk/display.c",
     "    if (!disp->image || disp->width != width || disp->height != height) {\n"
     "        create_display_image(pg, width, height);\n",
     "    if (!disp->image || disp->width != width || disp->height != height) {\n"
     "        fprintf(stderr, \"ember: display: display image %ux%u\\n\", width, height);\n"
     "        create_display_image(pg, width, height);\n")

print(f"patch_xemu: {applied} edits applied")
