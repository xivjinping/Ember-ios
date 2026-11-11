# Project status and handoff

Read this first when picking the project up in a new session. It says where
things stand, what comes next, and how the build rounds work. Last updated
2026-11-11 (first game renders in-game on the device).

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
| Code generator on Ember's JIT region | **Works on the device (2026-11-10).** `tcg/region.c` is patched to take its buffer from `tcg_ember_set_code_buffer()` (called through `ember_xemu_set_code_buffer` from `em_core_set_jit_region`): split-wx, no mmap/mprotect/guard pages |
| Coroutines on iOS | `Dependencies/xemu/ember/coroutine-ember.c` replaces the sigaltstack backend (sigaltstack fails silently on iOS). Control flow tested on Linux with an x86 twin of the stack switch; the arm64 version ran on the device: `qemu_init` opened the qcow2 and the disc without trouble |
| Starting the machine | **Enabled.** Needs the JIT region; otherwise the status line says to enable JIT |

The app links xemu and tries to start it. The status line shows which system
files are missing, that JIT is needed, or "Console running. CPU at 0x…".

## How the xemu side is put together

xemu is fetched at the revision in `Dependencies/xemu/XEMU_COMMIT`, changed
by `Dependencies/xemu/patch_xemu.py` (62 edits, each asserts the text it
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
- The nv2a renderer is xemu's Vulkan renderer on MoltenVK (see step 7);
  the null renderer is the fallback.
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
6. ~~Start the BIOS on the device.~~ **Done (2026-11-10, iPad Air M2).**
   First attempt, with Red Dead Revolver opened in place from an app-group
   File Provider folder: the log shows `starting xemu; code buffer from
   Ember's JIT region`, the launch parameters, exactly two `CPU Reset` dumps
   (power-on; no reset loop), then `machine built, entering the main loop`.
   The status line read "Console running. CPU at 0x80014386": an address in
   the Xbox kernel, so the MCPX ROM and the BIOS ran and handed over to it.
   Last log line: `main loop returned 0`, a clean exit. Most likely the
   owner tapped X to close the game (that requests a shutdown); not yet
   confirmed it was not the guest powering itself off. Only one sample of
   the CPU address so far, so "kernel idling" vs "stuck" is still open.
   Add a log line when the app requests the stop, and log the PC a few times,
   to tell these apart next round.
7. **A picture. Works on the device (2026-11-11).**
   Route taken: xemu's Vulkan renderer on MoltenVK v1.4.0, linked statically.
   What had to change (all in `patch_xemu.py` section 11):
   - `fetch-moltenvk-ios.sh` repacks MoltenVK's release as one object with
     every symbol hidden except `ember_mvk_get_instance_proc_addr`, because
     volk defines globals named like the Vulkan functions. volk is started
     with `volkInitializeCustom`.
   - No OpenGL interop (`HAVE_EXTERNAL_MEMORY 0`). The picture is *read back*:
     `ember-core.c` runs a frame thread that calls
     `nv2a_get_framebuffer_surface()` (downloads the displayed surface into
     guest video memory) and then `graphic_hw_update()`, so the existing
     display listener sees it. Slow but simple; sharing the Vulkan image
     with the app's Metal view is the later optimisation. Video overlay
     (pvideo) and upscaling live in the skipped display pass.
   - **MoltenVK has no geometry shaders**, and this xemu revision uses one
     for every triangle (it passes the triangle's three corners to the
     fragment shader for exact depth). `pgraph_glsl_need_geom()` now returns
     false; the fragment shader takes depth from `noperspective` varyings
     (`vtxPos0` = position, `vtxPos1.x` = 1/w) and the polygon-offset slope
     from `dFdx/dFdy`. Flat shading uses Vulkan's first-vertex rule only.
     The start-up geometry-shader winding probe is skipped.
   - Quads, quad strips, fans, polygons and line loops are expanded to
     index lists on the CPU (`ember/vk-draw-ember.h`).
   - `geometryShader` and `shaderTessellationAndGeometryPointSize` are not
     required; unsupported formats are logged, not fatal; the two 838 MB
     compute scratch buffers are 128 MB each; if Vulkan fails to start the
     null renderer takes over (log line `ember: renderer: ...`).
   **Result on the iPad Air M2:** Red Dead Revolver boots through the BIOS
   into gameplay (tutorial scene: terrain, character, HUD, text and the
   controller overlay all drawn correctly in the owner's screenshot). Log:
   MoltenVK 1.4.0 on "Apple M2 GPU", `ember: renderer: Vulkan`, no format
   or shader errors. Only `VK_EXT_custom_border_color` is missing (texture
   borders fall back), and MoltenVK prints "Metal does not support
   disabling primitive restart" once per pipeline (harmless, but it floods
   the log; silence it by setting `primitiveRestartEnable` on iOS).
   Not yet known: real game speed (the overlay's "fps" counts display
   copies, not game frames), sound, whether touch input reaches the game,
   depth glitches in other scenes, other titles.
8. Audio through the app (xemu opens SDL audio itself; may clash with the
   app's AVAudioEngine), touch input check, creating the hard-disk image
   automatically when none is found, and showing a clear message when the
   console has already run once in this launch.

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

## 2026-11-11 (later): first play test

Device report: runs at full speed, no sound glitches heard, sticks fine, touch buttons needed a very hard or long press.
Cause (best guess, untested): a tap can start and end between two display frames, so `pushInput` never sees it. Fix: `InputMixer` latches every new touch press for 70 ms. Check on the device that taps now register.

## 2026-11-11 (later): speed readout and layout editor

- The stats overlay ("Show speed (fps)") now shows the game's own frame rate: `ember_xemu_game_fps()` reads xemu's `g_nv2a_stats.increment_fps` (buffer flips), 0 after a second without a flip. The "CPU at" status line hides once the game draws.
- Touch layout editor (`Frontend/Input/PadLayout.swift`): every control can be dragged and resized, in game (hand button, pauses the game) or from Settings. Saved in UserDefaults `input.padLayout`. Not yet tested on the device.
- No-JIT mode, not done: QEMU's interpreter (TCI) is a build-time choice (`--enable-tcg-interpreter`), so it cannot be a switch inside the same app. It would need a second app build ("Ember without JIT") with its own deps cache, and the `tcg/region.c` patch would need a plain buffer instead of the JIT region. Expect it to be many times slower than JIT. Waiting on the user's go-ahead.
