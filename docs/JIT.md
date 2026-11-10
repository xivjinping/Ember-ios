# JIT on a sideloaded iOS build — step by step

This is the full path from "unsigned .ipa out of CI" to "recompiled x86 code
executing on the phone", with the entitlement each step depends on and the file
in this repository that implements it.

Checked against current sources on 2026-11-10 (links at the end). iOS JIT
rules have changed twice in two years; re-check before relying on any of this
for a new iOS release.

## 0. What iOS forbids, and the one thing it allows

A recompiler needs memory it can write machine code into and then execute.
iOS blocks every ordinary way of getting that:

| Technique | Why it is unavailable to a sideloaded app |
|---|---|
| `mmap(PROT_WRITE \| PROT_EXEC)` | W+X pages are refused outright. |
| `MAP_JIT` + `pthread_jit_write_protect_np` | Needs the `dynamic-codesigning` entitlement, which Apple grants only to its own processes (and, in the EU, to approved browser engines). |
| `mprotect` RW → RX | Fails code-signing enforcement: the pages are not covered by the app's signature. |

The exception: **a process that has been debugged**. When a debugger attaches,
the kernel sets the `CS_DEBUGGED` flag on the process and stops enforcing
signatures on its executable pages (a debugger has to be able to plant
breakpoints). The flag stays set after the debugger detaches. Every sideloaded
JIT emulator on iOS is built on this.

So "enabling JIT" means: get a debugger to attach once. Whether a debugger
*may* attach is decided by one entitlement.

## 1. Entitlements

Both live in `Resources/Ember.entitlements`.

| Entitlement | Needed for | How a sideloaded build gets it |
|---|---|---|
| `get-task-allow` = true | **JIT.** Without it nothing can attach and JIT is impossible for that install. | Automatic with any *development* signature: AltStore, SideStore and Sideloadly sign with the user's own Apple ID development certificate. Builds signed with a distribution or enterprise certificate (most "certificate signer" services) do not have it. |
| `com.apple.developer.kernel.increased-memory-limit` | Headroom for high internal resolutions (render targets at 3× and 4.5× are large). Optional: the guest itself is only 64 or 128 MB. | Must be enabled as a capability on the App ID. Straightforward with a paid developer account; free Apple IDs need extra tooling and results vary. If your profile lacks it, delete the key before signing. |

Not needed and not useful: `com.apple.security.cs.allow-jit` (macOS hardened
runtime only) and `dynamic-codesigning` (not grantable).

The app checks `get-task-allow` at runtime (`EMJITSupport.hasGetTaskAllow`,
via `SecTaskCopyValueForEntitlement`) and tells the user to re-sign instead of
waiting for a debugger that can never attach.

## 2. Build and signing pipeline (no Mac required)

1. **CI builds unsigned.** `.github/workflows/build.yml` runs `xcodegen
   generate`, then `xcodebuild … CODE_SIGNING_ALLOWED=NO`.
2. **CI embeds the entitlements.** An unsigned binary has none, so the
   workflow ad-hoc signs with `ldid -SResources/Ember.entitlements`. This is
   only a carrier: it lets the next step see which entitlements the app wants.
3. **CI zips `Payload/Ember.app` into `Ember.ipa`** and uploads it.
4. **The user sideloads** with SideStore, AltStore/AltServer or Sideloadly.
   The tool re-signs with the user's development certificate and a profile
   for their device. That re-signing is what produces a real `get-task-allow`.
5. **Developer Mode** must be on (Settings › Privacy & Security), as for any
   development-signed app on iOS 16+.

## 3. Runtime sequence

Implemented in `Frontend/App/JITCoordinator.swift`, `Bridge/EMJITSupport.mm`
and `Core/src/cpu/jit/jit_memory.cpp`.

1. **Preflight.** If `get-task-allow` is missing → interpreter, with a clear
   message. Stop.
2. **Classify the device.** `EMJITSupport.txmStatus` reports whether the
   Trusted Execution Monitor is present (iOS 26+ on newer chips). This picks
   between the two allocation policies in step 5. If it cannot tell, the app
   refuses to guess and asks the user to set *Force JIT script*.
3. **Get a debugger attached.** One of:
   - *StikDebug (on device).* The app opens
     `stikdebug://enable-jit?bundle-id=<id>&pid=<pid>` and, when TXM is
     present, adds `&script-name=universal.js`. StikDebug needs its one-time
     setup: a pairing file, the LocalDevVPN loopback VPN, and a mounted
     Developer Disk Image.
   - *Launch with JIT.* The user starts Ember from StikDebug's Enable JIT
     list (or from LiveContainer with "Launch with JIT"). The app is then
     already debugged when it starts; `EmberApp` detects that and continues
     from step 4 immediately.
   - *Wait for debugger.* The app just waits. Anything that attaches works on
     non-TXM systems: AltServer/AltJIT, SideJITServer, Jitterbug, or Xcode.
4. **Wait for `CS_DEBUGGED`.** `ember::jit::processIsDebugged()` polls
   `csops(getpid(), CS_OPS_STATUS)` for the `0x10000000` bit, with a timeout.
   Nothing JIT-related runs before this is true. Opening the URL succeeding
   means only that iOS delivered it.
5. **Allocate the code cache — once, for the whole process.**
   - *No TXM* (`DebuggedDualMap`): `mmap` an RX region, then `vm_remap` the
     same pages to a second address and `mprotect` that alias RW.
   - *TXM* (`TxmUniversal`): the same, plus one extra call between the
     `mmap` and the `vm_remap` — see section 5.
6. **Release the debugger** (TXM only): the detach command. After this no new
   executable region can be created in this process, which is why the cache
   is a fixed size and shared by every game session.
7. **Smoke test.** Write `mov w0, #42; ret` through the RW alias, flush the
   instruction cache, call it through the RX address.
8. **Hand it to the core.** `em_core_set_jit_region(core, rx, rw, size)` at
   each game boot. If any step failed, that session runs the interpreter.

## 4. Why dual mapping

One mapping is never both writable and executable. The emitter writes through
the RW address; the CPU runs from the RX address; both name the same physical
pages. Consequences for the recompiler backend:

- Every emitted pointer (block entry, branch target, patch site) is an **RX**
  address. Every store goes to `rw + (addr - rx)`.
- After writing, call `flushInstructionCache` on the **RX** range.
- No per-thread write-protect toggling exists, so block linking and
  invalidation from another guest thread just need ordinary locking.
- The cache cannot grow. Size it up front (default 64 MB, 32–256 in
  Settings) and implement a full flush when it fills.

### What this means for an x86 engine

If the CPU engine is QEMU's TCG (which is what xemu uses), most of this is
already built in: TCG has an AArch64 code generator, and it supports exactly
this split layout — a writable and an executable view of one code buffer —
which was added upstream for iOS by the UTM project. The work is to point
TCG's buffer allocation at the region from step 5 instead of letting it
`mmap` its own, and to keep its buffer at the fixed size chosen here.

## 5. iOS 26 and TXM: the breakpoint protocol

On devices where the Trusted Execution Monitor (TXM/SPTM) is active,
`CS_DEBUGGED` alone is no longer enough: every executable region has to be
registered through the debug connection before code runs from it. The app
cannot do that itself, so it asks the attached debugger, using breakpoints as
a system-call mechanism. This repository implements the *universal* protocol
(`universal.js`, bundled with StikDebug and StikJIT):

| Call | Registers | Meaning |
|---|---|---|
| `brk #0xf00d` with `x16 = 1` | `x0` = address (or 0), `x1` = length | Prepare an executable region. The script returns the RX address in `x0`. |
| `brk #0xf00d` with `x16 = 0` | — | Detach. |

Rules that follow from how this works:

- A `brk` with no script listening **kills the app**. Never issue one unless
  `CS_DEBUGGED` is set and the enabler was asked to attach *with the script*.
- Prepare **all** regions, then detach. A detached script cannot be asked for
  more.
- The script is the developer's choice, fixed in code (`JITCoordinator.scriptName`),
  not a user setting.
- On non-TXM devices, omit the script parameter; attach-and-detach is enough.

## 6. When JIT is not available

The interpreter always works and needs no entitlement. It is the right tool
for homebrew, for debugging the recompiler against a reference, and for
proving a port boots. It is not a way to play games: interpreting a 733 MHz
x86 CPU instruction by instruction on a phone lands at a small fraction of
full speed.

## 7. Troubleshooting

| Symptom | Likely cause |
|---|---|
| "This install lacks get-task-allow" | Signed with a distribution/enterprise certificate. Reinstall with a development-signing tool. |
| Times out waiting for a debugger | StikDebug not set up (pairing file, VPN, DDI), or the app was not selected in it. |
| JIT enables, app dies on first game boot (iOS 26) | TXM device but the script was not attached; relaunch through the enabler with `universal.js`. |
| App hangs at "Preparing code cache…" | A plain debugger is attached and has stopped at the `brk`; it is not running the script. |
| Crash under memory pressure, not at boot | Internal resolution too high for the device; drop a step, or add increased-memory-limit. |

## 8. What has and has not been verified

- **Verified here:** the dual-mapped allocator's logic, on Linux (memfd-based
  twin of the iOS path), including executing code through it — see
  `Core/tests/core_tests.cpp`.
- **Not verified here:** anything that needs an iPhone. The iOS branches of
  `jit_memory.cpp`, the TXM handshake and the TXM detection heuristic were
  written from the published protocol and common practice in current
  sideloaded emulators, and have not been run on a device. Expect to debug
  them on hardware, starting with a non-TXM device on iOS 17–18 where only
  steps 1–5 (no TXM) are involved.
- **Never App Store.** Entitlement SPI, debugger-dependent JIT and
  `get-task-allow` all rule it out.

## Sources

- [StikJIT integration guide — iOS 26 universal protocol, StikDebug URL scheme](https://github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md)
- [StikDebug — requirements and iOS version support](https://github.com/StikDebug/StikDebug)
- [StikJIT — framework overview](https://github.com/StikDebug/StikJIT)
