// Executable memory for the PowerPC -> ARM64 recompiler on iOS.
//
// iOS never grants a sideloaded app W+X pages or MAP_JIT (that needs the
// Apple-only `dynamic-codesigning` entitlement). What it does allow is
// unsigned executable pages in a process that has been debugged
// (CS_DEBUGGED). So the code cache is always *dual mapped*: one RX view the
// CPU executes from, one RW alias of the same physical pages the emitter
// writes through. W^X is never violated on any single mapping.
//
//   Policy            When                               How the RX view is obtained
//   ----------------  ---------------------------------  ---------------------------------------
//   DebuggedDualMap   iOS <= 18, or iOS 26 without TXM   mmap(RX) after CS_DEBUGGED, vm_remap -> RW
//   TxmUniversal      iOS 26+ on devices with TXM/SPTM   same, plus a brk #0xf00d "prepare region"
//                                                        call serviced by the attached debugger
//                                                        script (StikDebug universal.js)
//   HostDualMap       Linux/macOS host tests             memfd / plain mmap
//
// docs/JIT.md walks through the whole flow.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ember::jit {

enum class Policy { DebuggedDualMap, TxmUniversal, HostDualMap };

struct CodeRegion {
    uint8_t* rx   = nullptr; // execute from here
    uint8_t* rw   = nullptr; // write through here
    size_t   size = 0;
    explicit operator bool() const { return rx && rw && size; }
};

// True once a debugger has attached to this process at least once
// (csops CS_OPS_STATUS & CS_DEBUGGED). Always true for HostDualMap builds.
bool processIsDebugged();

// The policy this build would use; `txmPresent` comes from the platform layer.
Policy selectPolicy(bool txmPresent);

// Allocates one dual-mapped region. Preconditions on iOS: processIsDebugged()
// is true, and for TxmUniversal the debugger script is still attached (a brk
// with nothing listening kills the process). Size is rounded up to 16 KiB.
bool allocateRegion(Policy policy, size_t size, CodeRegion& out, std::string& error);

// TxmUniversal only: tells the debugger script to detach. Call exactly once,
// after *every* region has been allocated — regions cannot be added afterwards.
void finishSetup(Policy policy);

void freeRegion(CodeRegion& region);

// Must be called on the RX address range after writing code through RW.
void flushInstructionCache(const uint8_t* rx, size_t length);

// Emits and runs `return 42`. Note: on iOS a region that was not correctly
// prepared does not fail politely — the kernel kills the process on first
// execution — so this is a sanity check for the allocator, not a probe.
bool smokeTest(const CodeRegion& region);

} // namespace ember::jit
