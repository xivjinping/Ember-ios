# Project status and handoff

Read this first when picking the project up in a new session. It says where
things stand, what comes next, and how the build rounds work. Last updated
2026-11-10 (after the link step).

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
| xemu compile for iOS | **All 1,687 steps compile** |
| xemu link for iOS | **Everything resolves except `_main`**, which is expected: xemu's `main` lives in `ui/xemu.c`, which is excluded. Step 4 turns the target into a library |
| DSP56300 (Rust) | Built from source for `aarch64-apple-ios` by `Dependencies/xemu/build-dsp56300-ios.sh` (cached); xemu's own download is macOS-only |
| Front-end replacements | `Dependencies/xemu/ember/ember-frontend.c`: controller ports, notifications, snapshot hooks, pcap stub |

The app still runs the stub backend. No xemu code is in the app yet.

## Where stage 3 stands

xemu is fetched at the revision in `Dependencies/xemu/XEMU_COMMIT`, changed
by `Dependencies/xemu/patch_xemu.py` (23 edits, each asserts the text it
replaces, plus a copy of `ember/ember-frontend.c` into `ui/`), configured by
`configure-xemu-ios.sh` and compiled by `compile-xemu-ios.sh`, which prints a
grouped error summary. Steps 1 to 3 below are done: the last two files
compile, and the link finds every symbol except `_main`.

`ember-frontend.c` exposes `ember_input_set(port, buttons, axes)` and
`ember_input_get_rumble(port, &l, &r)` for the app to feed the emulated
gamepads. Notifications only go to the log for now, and snapshots store plain
machine state without xemu's thumbnail header.

## Next steps, in order

1. ~~Fix the last two files.~~ Done.
2. ~~Link.~~ Done apart from `_main` (see step 4).
3. ~~Ember's replacements for the missing symbols.~~ Done for the nine the
   link needed. More will turn up once the core runs (settings paths, the
   display hand-off to Metal, audio).
4. **Produce a static library** instead of an executable, link it into the
   app in place of `Core/src/backend/stub_backend.cpp`, and implement
   `Core/include/ember/core_api.h` on top of it. QEMU's `main` (xemu's is in
   `ui/xemu.c`; QEMU's generic one is `system/main.c`) must become a function
   the app calls on a background thread. Expect to bundle `libdsp56300_emu_ffi.a`
   and the QEMU/xemu archives into one library, and to need the font/ROM data
   and any `-force_load` archives from the current link command (printed in
   the log).
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
  GPL. `LICENSE` (GPL-2.0) is already in the repository.
- The app icon is original artwork (`scripts/make_icon.py`). Do not use
  console makers' logos or names in the app's branding.
