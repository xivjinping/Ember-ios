# Project status and handoff

Read this first when picking the project up in a new session. It says where
things stand, what comes next, and how the build rounds work. Last updated
2026-11-10 (step 5 built; first start on the device pending).

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
| App linked with xemu | **Builds** (16 MB binary, was 1.4 MB). `Core/src/backend/xemu_backend.cpp` implements `core_api.h`; backend name is `xemu`. **Confirmed on the iPad (2026-11-10):** launches, shows the xemu status line, and finds the boot ROM (`mcpx_1.0.bin`), BIOS (`Complex_4627.bin`, 1 MB) and hard disk (`xbox_hdd.qcow2`) in `Documents/Ember/System` |
| Code generator on Ember's JIT region | **Built, not yet run on the device.** `tcg/region.c` is patched to take its buffer from `tcg_ember_set_code_buffer()` (called through `ember_xemu_set_code_buffer` from `em_core_set_jit_region`): split-wx, no mmap/mprotect/guard pages |
| Coroutines on iOS | `Dependencies/xemu/ember/coroutine-ember.c` replaces the sigaltstack backend (sigaltstack fails silently on iOS). Control flow tested on Linux with an x86 twin of the stack switch; the arm64 version is untested until the device runs it |
| Starting the machine | **Enabled.** Needs the JIT region; otherwise the status line says to enable JIT |

The app links xemu and tries to start it. The status line shows which system
files are missing, that JIT is needed, or "Console running. CPU at 0x…".

## How the xemu side is put together

xemu is fetched at the revision in `Dependencies/xemu/XEMU_COMMIT`, changed
by `Dependencies/xemu/patch_xemu.py` (29 edits, each asserts the text it
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
   Confirmed on the device: the app launches and the system files are found.
5. ~~Point QEMU's code generator at Ember's JIT region.~~ Built. Whether it
   works is exactly what step 6 finds out.
6. **Start the BIOS on the device. Waiting on the owner's first attempt.**
   Procedure: Enable JIT in the library, start a game, wait half a minute,
   then send a screenshot of the status line and `Ember/xemu-log.txt` (and
   Settings > JIT log if the app died). What to look for:
   - `ember: starting xemu; code buffer from Ember's JIT region` must be the
     first line; `ember: machine built, entering the main loop` means
     `qemu_init` survived (block layer, coroutines, SDL audio, devices).
   - The status line's CPU address should keep changing. `0xFFFFxxxx` is the
     boot ROM/BIOS, `0x8001xxxx` and up is the kernel.
   - `-d cpu_reset` is on: repeated "CPU Reset" dumps mean a reset loop.
   - If the log ends before "machine built" the last lines name what
     `qemu_init` rejected (it calls `exit`, so the app just closes).
   Likely trouble, in rough order: the arm64 coroutine switch; SDL audio
   opening next to the app's own AVAudioEngine; something in TCG touching
   the region's protection that the patch missed.
7. After the BIOS runs: a renderer (Metal) for nv2a, audio through the app,
   and creating the hard-disk image automatically.

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
