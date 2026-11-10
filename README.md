# Ember

An iOS/iPadOS shell for an original-Xbox emulator: SwiftUI frontend,
Objective-C++ bridge, portable C++ core, and the iOS-specific plumbing an
emulation engine needs (JIT memory, Metal presentation with resolution
scaling, input, audio, CI).

**What this is not:** a working emulator. There is no x86→ARM64 recompiler,
no NV2A renderer and no kernel here; those are the multi-year parts, and no
iOS port of them exists yet. The bundled backend loads a real disc image as
far as its entry point, reports where execution stops, and shows a test
pattern. `docs/ARCHITECTURE.md` §1 lists what is and is not implemented, and
§3 describes the realistic route to a real engine (porting xemu).

## Layout

```
ember/
├── project.yml                 XcodeGen spec → Ember.xcodeproj (generated, not committed)
├── CMakeLists.txt              Core static library + host tests
├── codemagic.yaml              Codemagic build
├── .github/workflows/build.yml GitHub Actions: Linux core tests, unsigned + signed .ipa
│
├── Core/                       Portable C++20. No UIKit, no Objective-C.
│   ├── include/ember/core_api.h    The one C header everything above uses
│   ├── src/
│   │   ├── backend/stub_backend.cpp    Stand-in engine
│   │   ├── cpu/
│   │   │   ├── x86_state.h             Pentium III register file, kernel-trap convention
│   │   │   ├── x86_interpreter.cpp     Fallback interpreter (reference subset)
│   │   │   └── jit/jit_memory.{h,cpp}  Dual-mapped code cache, CS_DEBUGGED, TXM protocol
│   │   ├── mem/xbox_memory.{h,cpp}     64 / 128 MB guest RAM with the 0x80000000 mirror
│   │   ├── nv2a/
│   │   │   ├── pushbuffer.{h,cpp}      GPU command-stream decoder, frame tracker
│   │   │   └── texture_convert.{h,cpp} Unswizzle, 16-bit formats → BGRA8
│   │   ├── fs/
│   │   │   ├── byte_source.h           File / memory / slice sources
│   │   │   ├── mapped_file.{h,cpp}     Windowed mmap for multi-GB images
│   │   │   ├── xbe.{h,cpp}             Executable header, certificate, sections
│   │   │   ├── xiso.{h,cpp}            XDVDFS disc images
│   │   │   ├── fatx.{h,cpp}            Hard-disk filesystem (read-only)
│   │   │   └── title_probe.cpp         Library detection: name and title ID from the disc
│   │   └── kernel/xbe_loader.{h,cpp}   HLE loader: map sections, resolve kernel imports
│   └── tests/core_tests.cpp        Runs on Linux and macOS; synthesises every image it needs
│
├── Bridge/                     Objective-C++: the only code that sees both sides
│   ├── Ember-Bridging-Header.h
│   ├── EMEmulatorCore.{h,mm}       Core lifecycle, frame as an MTLTexture, pad, stats
│   ├── EMJITSupport.{h,mm}         Entitlement check, TXM detection, code-cache setup
│   └── EMPipelineCache.{h,mm}      Async pipeline builds + MTLBinaryArchive
│
├── MetalShaders/
│   ├── Present.metal           Final pass, aspect-fitted
│   └── PostProcess.metal       Compute kernels: FXAA, upscale + adaptive sharpen
│
├── Frontend/                   SwiftUI, iOS 15+
│   ├── App/        EmberApp.swift, JITCoordinator.swift
│   ├── Library/    GameLibrary.swift, LibraryView.swift
│   ├── Emulation/  EmulationView.swift, EmulatorSession.swift
│   ├── Render/     MetalRenderView.swift, FrameRenderer.swift
│   ├── Input/      VirtualGamePadView.swift, ControllerManager.swift, PadState.swift
│   ├── Audio/      AudioEngine.swift
│   └── Settings/   Settings.swift, SettingsView.swift
│
├── Resources/      Info.plist, Ember.entitlements
├── Dependencies/   Where an engine goes (see its README)
└── docs/           ARCHITECTURE.md, JIT.md
```

## Building without a Mac

1. Push the repository to GitHub. The **Build** workflow runs the core tests
   on Linux, then builds on a `macos-latest` runner.
2. Download the `Ember-unsigned-ipa` artifact and install it with SideStore,
   AltStore or Sideloadly. They sign it with your own Apple ID, which is
   what provides `get-task-allow` for JIT.
3. For JIT, follow `docs/JIT.md`.

### Signed .ipa from CI

A signed build needs your signing identity; nobody else can supply it. Add
three repository secrets and the same workflow also uploads
`Ember-signed-ipa`:

| Secret | Contents |
|---|---|
| `BUILD_CERTIFICATE_BASE64` | `base64 -i certificate.p12` of an **Apple Development** certificate with its private key |
| `P12_PASSWORD` | The password set when exporting the .p12 |
| `PROVISIONING_PROFILE_BASE64` | `base64 -i profile.mobileprovision` of a **development** profile containing that certificate and your device |

Team ID and bundle ID are read from the profile. Creating the certificate
and profile requires an Apple Developer Program membership (and, to export
the .p12, either a Mac or an OpenSSL-generated signing request). Without
those, use the unsigned artifact; the result on the device is the same.

With a Mac: `brew install xcodegen && xcodegen generate && open Ember.xcodeproj`.

Core tests anywhere:

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Status of the code

- `Core/` builds warning-free with GCC and Clang on Linux and its tests pass:
  guest memory, the interpreter, XBE parsing and loading, XDVDFS (including
  a full-dump layout), FATX, the push-buffer decoder, texture conversion,
  dual-mapped code execution, and the stub backend end to end.
- `Bridge/`, `Frontend/`, `MetalShaders/` and the workflow were written
  without access to Xcode, an iOS SDK or a macOS runner. They have **not**
  been compiled or run. Expect a round of compiler fixes on the first CI
  run, and device debugging after that.
- Format knowledge (XBE offsets, FATX layout, NV2A method numbers) comes
  from public documentation of these formats and is exercised only against
  images synthesised by the tests. Validate against real dumps before
  trusting it, especially FATX.

## Games and system files

Ember ships no games, BIOS, boot ROM, keys or hard-disk images, and cannot
download any. Use images of discs you own, placed in **Files › Ember ›
Games** or picked with the + button: `.iso` / `.xiso` (XDVDFS), `.xbe`, or
an extracted-disc folder containing `default.xbe`.

Cover art is optional: put `<TITLEID>.png` or `.jpg` (the 8-digit hex ID
shown under each game) in **Files › Ember › Covers**.
