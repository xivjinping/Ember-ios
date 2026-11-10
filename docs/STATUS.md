# Project status and handoff

Read this first when picking the project up in a new session. It says where
things stand, what comes next, and how the build rounds work. Last updated
2026-11-10.

## Goal

Run original-Xbox games on iPad/iPhone by porting xemu's emulation core into
this app (Ember). `docs/ARCHITECTURE.md` explains the design and why xemu;
`docs/JIT.md` covers JIT on iOS.

## What is done

| Item | State |
|---|---|
| App shell (SwiftUI, bridge, Metal presenter, input, audio) | Builds on GitHub Actions, installs and launches on the owner's device |
| JIT memory on a real device | **Verified** on iPad Air M2 (`iPad14,8`), iPadOS 27.0.1, StikDebug + `universal.js`, started from the app's Enable JIT button. See `docs/JIT.md` §8 |
| iOS libraries | GLib 2.88.4, pixman 0.44.2, libsamplerate 0.2.2 and SDL3 (xemu's pinned revision) build for iOS arm64 |
| xemu configure for iOS | Passes |
| xemu compile for iOS | **1,677 of 1,679 steps compile.** Two files fail |

The app still runs the stub backend. No xemu code is in the app yet.

## Where stage 3 stands

xemu is fetched at the revision in `Dependencies/xemu/XEMU_COMMIT`, changed
by `Dependencies/xemu/patch_xemu.py` (17 edits so far, each one asserts the
text it replaces), configured by `configure-xemu-ios.sh` and compiled by
`compile-xemu-ios.sh`, which prints a grouped error summary.

The two files that still fail, both with `'epoxy/gl.h' file not found`:

- `hw/xbox/nv2a/pgraph/null/renderer.c` (via `null/renderer.h`)
- `migration/savevm.c` (via `ui/xemu-snapshots.h`, which uses `GLuint`)

## Next steps, in order

1. **Fix the last two files.** Give them the few GL type definitions they
   need without real OpenGL (a small shim header on the include path, or
   edits in `patch_xemu.py`). Note that even xemu's *null* renderer is
   written against SDL and the offscreen-GL helper, which is left out on
   iOS; expect to patch the renderer itself, not just its header.
2. **Link.** The compile step builds the `qemu-system-i386` target, so once
   everything compiles the linker runs. xemu's desktop front end
   (`ui/xemu*.c`, `ui/xui/`, ImGui) is excluded on iOS, so expect a list of
   undefined symbols for whatever the core still calls: settings
   (`g_config`), controller state (`xemu-input`), display, snapshots,
   notifications. `compile-xemu-ios.sh` already prints linker output.
   Also check that `libffi.a` and the libintl stub are in the prefix: GLib
   built them as subprojects and they did not appear in the installed list.
3. **Write Ember's replacements** for those symbols (a new source directory
   added through `patch_xemu.py` or a separate static library).
4. **Produce a static library** instead of an executable, link it into the
   app in place of `Core/src/backend/stub_backend.cpp`, and implement
   `Core/include/ember/core_api.h` on top of it. QEMU's `main` must become a
   function the app calls on a background thread.
5. **Point QEMU's code generator at Ember's JIT region.** TCG has a
   split-wx mode with a `vm_remap` path in `tcg/region.c`; on iOS the RX
   region must come from the debugger-script protocol
   (`Core/src/cpu/jit/jit_memory.cpp`), all of it allocated before the
   script detaches.
6. **Try to start the BIOS on the device** (interpreter first is not an
   option in TCG; use the JIT path). The owner supplies the MCPX boot ROM,
   a BIOS image and a hard-disk image on the device.

## How a build round works

- Two workflows. `build.yml` builds the app and uploads
  `Ember-unsigned-ipa`; it ignores changes under `Dependencies/` and
  `docs/`. `deps.yml` runs only for `Dependencies/**` and does: library
  build (cached by the hash of `build-ios-deps.sh`) → xemu configure →
  xemu compile.
- A round is: edit, commit, push, wait for the run, read the failed step's
  log with `gh run view <id> --log-failed`. A full `deps.yml` round takes
  roughly 10–15 minutes when the library cache hits.
- **Artifacts cannot be downloaded from the assistant's session** (the
  storage host is blocked), so anything needed for diagnosis must be
  printed in the job log. `compile-xemu-ios.sh` does this.
- Changing `build-ios-deps.sh` invalidates the library cache and adds about
  ten minutes to the next round.
- The Linux job in `build.yml` runs the C++ core's tests
  (`Core/tests/core_tests.cpp`); keep it passing.

## Device and JIT facts

- Test device: iPad Air M2 (`iPad14,8`), iPadOS 27.0.1, 8 GB RAM.
- Working JIT recipe: sideloaded with a development signature, StikDebug
  set up, then **Enable JIT** in the app (top right of the library). Start
  it from the app, not from StikDebug's app list.
- Each JIT step is logged to `Documents/Ember/jit-trace.txt` and shown in
  Settings › JIT log. Ask for that log when anything JIT-related fails.

## Rules

- Never commit the MCPX boot ROM, BIOS images, hard-disk images, EEPROM
  dumps or games. `.gitignore` blocks the usual extensions.
- xemu and QEMU are GPL-2.0. Once their code is linked in, the whole app is
  GPL; the repository still needs a LICENSE file.
- The app icon is original artwork (`scripts/make_icon.py`). Do not use
  console makers' logos or names in the app's branding.
