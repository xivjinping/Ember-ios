# Project status and handoff

Read this first when picking the project up in a new session. It says where
things stand, what comes next, and how the build rounds work. Last updated
2026-11-10 (after step 4: xemu linked into the app).

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
| xemu link for iOS | **Links.** The executable is only a CI check; the app uses the libraries below |
| DSP56300 (Rust) | Built from source for `aarch64-apple-ios` by `Dependencies/xemu/build-dsp56300-ios.sh` (cached); xemu's own download is macOS-only |
| xemu as libraries | `package-xemu-ios.sh` repacks the link inputs as `libember-xemu-core.a` (force-loaded) and `libember-xemu-deps.a`, writes `ember-xemu.xcconfig`, test-links them. About 46 MB together |
| App linked with xemu | **Builds** (16 MB binary, was 1.4 MB). `Core/src/backend/xemu_backend.cpp` implements `core_api.h`; backend name is `xemu`. **Not yet confirmed on the device** that it launches |
| Starting the machine | **Deliberately blocked** in `xemu_backend.cpp` (`kCodeGeneratorReady = false`) until step 5; QEMU would otherwise fail to allocate code memory and end the app |

The app links xemu but does not start it yet. Starting a game shows a status
line: which system files are missing, or that they were found.

## How the xemu side is put together

xemu is fetched at the revision in `Dependencies/xemu/XEMU_COMMIT`, changed
by `Dependencies/xemu/patch_xemu.py` (25 edits, each asserts the text it
replaces, plus copies of the files in `Dependencies/xemu/ember/` into xemu's
`ui/`), configured by `configure-xemu-ios.sh`, compiled and linked by
`compile-xemu-ios.sh` (prints a grouped error summary) and packaged by
`package-xemu-ios.sh`.

Ember's own files inside xemu (`Dependencies/xemu/ember/`):

- `ember-xemu.h`: the C interface the app uses. Nothing else crosses over.
- `ember-core.c`: `ember_xemu_start()` loads xemu's settings, fills in the
  file paths and runs `qemu_init` + `qemu_main_loop` on its own thread. It
  registers the `xemu` display that `vl.c` asks for, without a window: a
  display listener copies the guest surface to a BGRA buffer
  (`ember_xemu_lock_frame`). It plugs gamepads into the USB ports, and does
  pause and stop through bottom halves on QEMU's main loop.
- `ember-frontend.c`: controller state fed by `ember_input_set`,
  notifications (to the log), snapshot hooks, pcap stub.
- `ember-exe-main.c`: `main()` for the CI-only executable.

Things to know:

- QEMU keeps process-wide state: the machine can be started **once per app
  launch**. The backend says so if asked again.
- Fatal errors inside QEMU call `exit()`, which ends the app. The backend
  checks the files first and sends stdout/stderr to
  `Documents/Ember/xemu-log.txt`.
- System files go in `Documents/Ember/System` (Files > Ember > Ember >
  System) and are recognised by size/extension, not name: the 512-byte boot
  ROM, a BIOS `.bin` of 256 KB/512 KB/1 MB, a `.qcow2` or `.img` hard disk.
  xemu writes `eeprom.bin` there itself.
- The nv2a renderer is xemu's *null* renderer: nothing the GPU draws is
  visible. Expect a black picture even when the BIOS runs; progress shows in
  the log. A Metal renderer is a later stage.
- Audio: xemu's APU plays through SDL directly, not through
  `em_core_read_audio`. This may clash with the app's own audio session;
  look at it once the BIOS runs.
- Only `.iso` titles are accepted by the xemu backend.

## Next steps, in order

1. ~~Fix the last two files.~~ Done.
2. ~~Link.~~ Done.
3. ~~Ember's replacements for the missing symbols.~~ Done for what the link
   needed.
4. ~~Static library, linked into the app, `core_api.h` on top.~~ Done in CI.
   **Waiting on the owner:** install the `.ipa` and confirm the app launches,
   Settings shows "Core backend: xemu", and starting a game shows the status
   line about system files. A crash at launch would point at constructors
   that QEMU/GLib run when the app loads.
5. **Point QEMU's code generator at Ember's JIT region.** TCG has a
   split-wx mode with a `vm_remap` path in `tcg/region.c`; on iOS the RX
   region must come from the debugger-script protocol
   (`Core/src/cpu/jit/jit_memory.cpp`), all of it allocated before the
   script detaches. `em_core_set_jit_region` in `xemu_backend.cpp` receives
   the region and currently only remembers that it exists; pass it through
   `ember-xemu.h` (and `extra_args`, e.g. `-accel tcg,split-wx=on,tb-size=N`).
   Then set `kCodeGeneratorReady = true`.
6. **Try to start the BIOS on the device.** The owner supplies the MCPX boot
   ROM, a BIOS image and a hard-disk image in `Documents/Ember/System`. Ask
   for `xemu-log.txt` after each attempt.

## How a build round works

- Two workflows. `deps.yml` ("iOS dependencies") runs only for
  `Dependencies/**` and does: library build (cached by the hash of
  `build-ios-deps.sh`) → DSP library (cached) → xemu configure → compile and
  link → package → save `build-deps/xemu-lib` to the Actions cache
  (`xemu-ios-lib-…`). `build.yml` ("Build") builds the app and uploads
  `Ember-unsigned-ipa`; it ignores pushes that only touch `Dependencies/`
  or `docs/`, restores the newest `xemu-ios-lib-` cache, and **runs again by
  itself when "iOS dependencies" succeeds**. If the cache is gone (GitHub
  evicts unused caches after 7 days) the app silently falls back to the
  stub; the log line "linked backend:" says which one was linked. Re-run
  "iOS dependencies" to get the cache back.
- A push that changes both the app and `Dependencies/` builds the app twice:
  at once with the previous libraries, and again after the new ones exist.
  The second one is the one to install.
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
