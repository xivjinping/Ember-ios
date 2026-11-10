# Architecture

Ember is an iOS shell for an original-Xbox emulator: a SwiftUI frontend, an
Objective-C++ bridge, and a C++ core behind one C header. This document says
what each layer does, what is implemented here, and what is not.

## 1. The honest scope

"Complete and production-ready" is not something this repository is. An
original-Xbox emulator is, at minimum:

- an x86 → ARM64 dynamic recompiler for a Pentium III, including x87, MMX and SSE;
- an NV2A GPU emulator: push-buffer engine, fixed-function pipeline, vertex
  programs and register combiners translated to shaders, dozens of texture
  and surface formats;
- either the real kernel and BIOS running on emulated hardware (LLE, the
  xemu approach) or a reimplementation of ~370 kernel exports (HLE, the
  Cxbx-Reloaded approach);
- the MCPX audio processor, USB controllers, IDE/DVD, EEPROM, and timers.

Those are years of work in the projects that have done them, and nothing
here replaces them. What this repository contains is everything around that
engine, plus the Xbox-specific pieces that are tractable and testable in
isolation:

| Layer | In this repo | State |
|---|---|---|
| SwiftUI frontend: library, in-game UI, settings | `Frontend/` | Written; not compiled here (no Xcode in the authoring environment) |
| Touch controller, GameController mapping | `Frontend/Input/` | Written; not compiled here |
| Metal presenter: scaling presets, FXAA, sharpening, MetalFX | `Frontend/Render/`, `MetalShaders/` | Written; not compiled here |
| Audio output (AVAudioEngine) | `Frontend/Audio/` | Written; not compiled here |
| Obj-C++ bridge, pipeline cache | `Bridge/` | Written; not compiled here |
| JIT memory (dual map, CS_DEBUGGED, TXM protocol) | `Core/src/cpu/jit/` | Host path built and tested; iOS path untested on device |
| XBE parser (header, certificate, sections) | `Core/src/fs/xbe.*` | Built and tested |
| XDVDFS disc images (trimmed and full dumps) | `Core/src/fs/xiso.*` | Built and tested |
| FATX volumes (read-only) | `Core/src/fs/fatx.*` | Built and tested against a synthetic volume |
| HLE title loader (map sections, resolve kernel imports) | `Core/src/kernel/` | Built and tested |
| Guest memory, 64 / 128 MB | `Core/src/mem/` | Built and tested |
| x86 interpreter | `Core/src/cpu/x86_interpreter.cpp` | 32-bit integer subset, tested. A reference skeleton, not a game-capable CPU |
| NV2A push-buffer decoder and frame tracker | `Core/src/nv2a/pushbuffer.*` | Built and tested |
| NV2A texture unswizzle + format conversion | `Core/src/nv2a/texture_convert.*` | Built and tested |
| x86 → ARM64 recompiler | — | **Not here** |
| NV2A renderer, shader translation, surface blits | — | **Not here** |
| HLE kernel, or LLE hardware + BIOS support | — | **Not here** |
| MCPX APU (voice and DSP emulation) | — | **Not here** |

With the bundled stub backend, picking a real disc image does this: the
image is opened, `default.xbe` is found in its XDVDFS volume, its sections
are mapped into guest RAM, its kernel imports are resolved to trap
addresses, and the interpreter runs from the entry point until the first
kernel call or the first instruction it does not implement. The app then
shows where that was, over a test pattern. That is as far as it goes.

## 2. Layering

```
 SwiftUI (Frontend/)
   LibraryView ─ EmulationView ─ SettingsView
        │             │
        │      EmulatorSession ── InputMixer ◄─ VirtualGamePadView (touch)
        │        │    │    │                 ◄─ ControllerManager (GCController)
        │        │    │    └─ AudioEngine
        │        │    └─ MetalRenderView / FrameRenderer (MTKView)
        ▼        ▼
 Obj-C++ (Bridge/)   EMEmulatorCore · EMJITSupport · EMPipelineCache
        │
        ▼  one C header: Core/include/ember/core_api.h
 C++ core (Core/)
   backend ─ XboxMemory ─ interpreter ─ jit::CodeRegion
           ─ XBE / XDVDFS / FATX ─ XBE loader ─ NV2A push buffer + textures
```

- Swift never sees C++. The bridging header exposes Objective-C classes and
  the plain C structs from `core_api.h`.
- The core never sees UIKit. Paths and the JIT region are passed in; frames,
  audio and stats are pulled out.
- The real-time audio callback calls the C API directly, not Objective-C.

## 3. Getting a real engine: two routes

### Route A — port xemu (recommended)

xemu is the accurate, actively maintained original-Xbox emulator. It is a
fork of QEMU that emulates the hardware at a low level and boots the
console's own BIOS. Three properties make it the practical base for iOS:

1. **The CPU recompiler already targets ARM64.** QEMU's TCG translates x86
   to the host's instruction set and has an AArch64 backend. xemu publishes
   ARM64 builds for other platforms.
2. **TCG already supports iOS's JIT constraints.** It can keep its code
   buffer as separate writable and executable views, which was contributed
   upstream by the UTM project for exactly this purpose. The region from
   `Core/src/cpu/jit/jit_memory.cpp` is what it should be given.
3. **It has a Vulkan renderer** alongside OpenGL. Vulkan runs on iOS through
   MoltenVK, which is far less work than writing a Metal NV2A renderer from
   scratch, at some cost in performance and shader-compile latency.

What the port involves: building QEMU's core for iOS (its build system and
glib dependency are the first hurdle), replacing its SDL window, audio and
input layers with this shell's `core_api.h` seam, and running its main loop
on a background thread. Consequences to accept:

- **Licence.** xemu and QEMU are GPL-2.0. Linking them makes the whole app
  GPL; this shell would have to be released under compatible terms.
- **User-supplied system files.** Low-level emulation needs the MCPX boot
  ROM, a BIOS image and a hard-disk image. They are copyrighted and must
  come from the user's own console. The HLE loader in this repo is not used
  on this route.
- **Performance is unproven on phones.** The one Android attempt on this
  code lineage (March 2026) reported 10–15 fps on high-end devices as a
  proof of concept. Expect substantial optimisation work.

### Route B — HLE (the Cxbx-Reloaded approach)

Load the XBE directly, as `Core/src/kernel/xbe_loader.cpp` does, and
implement the kernel exports and a hardware layer natively. No BIOS is
needed. Cxbx-Reloaded proves the approach, but it runs title code directly
on the host's x86 CPU, so its CPU strategy does not transfer to ARM: a
recompiler still has to be written or borrowed, and its Direct3D-based
rendering path is Windows-only. This is the larger project of the two.

The pieces here (loader, kernel trap convention, push-buffer decoder,
texture conversion, FATX) are the starting blocks for route B and are also
useful as diagnostics tooling on route A.

## 4. CPU

- **Interpreter** (`x86_interpreter.cpp`): always available, no entitlement.
  Implements mov/lea/push/pop, the eight ALU operations in their 32-bit
  forms, inc/dec, test, call/ret/jmp/jcc, ModRM+SIB addressing and the FS
  override. Anything else stops with the opcode and address. Extending it to
  the full instruction set gives a reference to test a recompiler against;
  it will never be fast enough to play on.
- **Recompiler**: not present. It is enabled only when the platform layer has
  delivered an executable region (`em_core_set_jit_region`). docs/JIT.md
  covers how that region is obtained and the constraints it puts on the
  code generator (dual mapping, fixed-size cache).

Kernel calls use a trap convention: the loader writes
`kernelTrapAddress(ordinal)` into each import slot; those addresses are
outside guest RAM, so when EIP reaches one the CPU stops with
`StopReason::KernelCall`. An HLE kernel dispatches on the ordinal, reads
stdcall arguments from the guest stack, and returns by popping them.

## 5. GPU

Games statically link Direct3D 8, which writes NV2A methods into a push
buffer. Emulation therefore starts at the hardware interface, not at an API:

1. **FIFO** — `runPushBuffer` decodes method headers, jumps and calls.
2. **State** — methods set registers: surfaces, viewport, texture stages,
   vertex-attribute arrays, combiner and blend state. `FrameTracker` shows
   the shape; a renderer tracks all of it.
3. **Shaders** — two programmable stages have to become MSL:
   - vertex: either fixed-function transform and lighting, or an NV2A
     *vertex program* (up to 136 instructions of register-based microcode);
   - pixel: *register combiners* (up to 8 stages plus a final combiner) fed
     by 4 texture stages with per-stage addressing modes.
   Generate MSL from the state, key it by a hash of that state, and cache
   the source on disk.
4. **Textures** — unswizzle and convert (`texture_convert.cpp`); upload DXT
   as BC where the device supports it, otherwise decode on the CPU.
5. **Surfaces** — colour and depth targets live in guest memory and are
   read back by games; render-to-texture and image blits (a separate NV2A
   engine) must keep the two views consistent. Not implemented here.

**Avoiding stutter.** `EMPipelineCache` builds pipelines on a background
queue and returns "not ready" so the renderer can skip a draw instead of
blocking; finished pipelines go into an `MTLBinaryArchive` so later launches
skip compilation; a per-title list of state keys allows precompiling during
the boot screen.

## 6. Resolution scaling and post-processing

Two different things are called "scaling", and both are handled:

- **Internal resolution** (`em_core_set_render_scale`): the guest's render
  targets are allocated at 640×480 times the scale, and viewport/scissor
  state is multiplied to match. Presets are 1× (480p), 1.5× (720p), 2.25×
  (1080p), 3× (1440p) and 4.5× (4K). Switching is live. With the stub this
  only changes the test pattern's size, capped at 2× because it is drawn on
  the CPU.
- **Output scaling** (`FrameRenderer`): the internal image is brought to the
  display's size by one of:
  - bilinear sampling in the final pass;
  - a compute pass that resamples and applies contrast-adaptive sharpening;
  - MetalFX spatial upscaling on iOS 16+.
  FXAA is an optional compute pass before any of these.

On "FSR": AMD's FSR 1 is a spatial upscaler, and MetalFX Spatial is Apple's
equivalent, tuned for its own GPUs and available without shipping extra
code. FSR 1's own shaders are MIT-licensed and could be added as a third
compute path if a like-for-like comparison is wanted; they are not
reproduced here.

On 4K: an iPhone's screen is roughly 1170–1320 pixels on its short side, so
internal rendering above 1080p is discarded by the display. 3× and 4.5× are
for iPad Pro and external displays, and each step roughly doubles the GPU
fill cost. "Without dropping frames" depends on the engine and the title.

## 7. Memory

The console has 64 MB shared by CPU and GPU (128 MB on upgraded units).
`XboxMemory` is one allocation of that size, visible at 0x00000000 and
mirrored at 0x80000000. The guest cannot exceed it, so the usual iOS
out-of-memory risk comes from the emulator's own allocations instead:

| Consumer | Size | Control |
|---|---|---|
| Guest RAM | 64 / 128 MB, resident only when touched | fixed |
| Code cache | 64 MB default | Settings, fixed after JIT setup |
| Render targets | 480p ≈ 2 MB per target; 4K ≈ 50 MB per target | internal resolution |
| Texture cache | title dependent | needs a budget with eviction |
| Disc image | up to ~7 GB on disk, at most 64 MB mapped | `MappedFile` windows |

## 8. Audio

Real hardware: the MCPX APU mixes up to 256 voices through a voice
processor and two DSPs, and hands PCM to an AC97 codec at 48 kHz. An LLE
engine emulates the APU; an HLE engine intercepts DirectSound. Either way
the result is 48 kHz stereo written to a lock-free ring that
`AudioEngine` drains on the I/O thread with a ~5 ms buffer.

## 9. Storage

- **Game discs**: XDVDFS, read through `XisoReader` over windowed mappings.
- **Hard disk**: FATX partitions at fixed offsets (`kRetailPartitions`). An
  LLE engine needs a full disk image; an HLE engine can map partition
  letters to folders under `Documents/Ember` instead, which is simpler and
  lets saves be managed from the Files app.
- **EEPROM**: region, video standard and language settings; an HLE engine
  synthesises one.

## 10. Input

Port 1 receives the merge of the touch overlay and one physical controller.
The original pad's face buttons and triggers are pressure-sensitive
(0–255); controllers that report pressure pass it through, and touch
buttons report full pressure. Black and White map to the right and left
shoulder buttons on modern pads. Rumble is not wired up.

## 11. Not addressed

System Link and Xbox Live, memory-unit emulation, multi-controller and
split-screen input, per-game patches and widescreen hacks, save states, and
PAL 50 Hz pacing.
