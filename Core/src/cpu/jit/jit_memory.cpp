#include "jit_memory.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#endif

namespace ember::jit {

namespace {
constexpr size_t kHostPage = 16384; // arm64 iOS page; also a multiple of 4 KiB hosts
size_t roundUp(size_t v) { return (v + kHostPage - 1) & ~(kHostPage - 1); }

TraceHook gTraceHook = nullptr;
bool      gDebuggerAllocates = false;

__attribute__((format(printf, 1, 2))) void trace(const char* format, ...) {
    if (!gTraceHook) return;
    char line[200];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gTraceHook(line);
}
} // namespace

void setTraceHook(TraceHook hook) { gTraceHook = hook; }
void setDebuggerAllocates(bool enabled) { gDebuggerAllocates = enabled; }

// ---------------------------------------------------------------------------
// iOS
// ---------------------------------------------------------------------------
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR

extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);

namespace {
constexpr unsigned int kCsOpsStatus = 0;
constexpr uint32_t     kCsDebugged  = 0x10000000;

// Universal iOS 26 JIT protocol (StikDebug / StikJIT `universal.js`).
// The attached debugger script traps `brk #0xf00d`, reads the command from
// x16 and the arguments from x0/x1, performs the privileged work over the
// debug connection, writes the result to x0 and resumes after the brk.
//   x16 = 1: prepare region (x0 = address or NULL, x1 = length) -> x0 = RX address
//   x16 = 0: detach
__attribute__((noinline, naked)) void* txmPrepareRegion(void* /*address*/, size_t /*length*/) {
    __asm__("mov x16, #1\n"
            "brk #0xf00d\n"
            "ret\n");
}

__attribute__((noinline, naked)) void txmDetach() {
    __asm__("mov x16, #0\n"
            "brk #0xf00d\n"
            "ret\n");
}

bool remapWritable(uint8_t* rx, size_t size, uint8_t*& rwOut, std::string& error) {
    vm_address_t rw = 0;
    vm_prot_t    cur = 0, max = 0;
    trace("vm_remap: creating the writable alias of %p", static_cast<void*>(rx));
    kern_return_t kr = vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                reinterpret_cast<vm_address_t>(rx), FALSE, &cur, &max, VM_INHERIT_NONE);
    trace("vm_remap: returned %d, alias %p, cur prot %d, max prot %d", kr, reinterpret_cast<void*>(rw), cur, max);
    if (kr != KERN_SUCCESS) {
        error = "vm_remap failed (kern_return " + std::to_string(kr) + ")";
        return false;
    }
    trace("mprotect: making the alias read-write");
    if (mprotect(reinterpret_cast<void*>(rw), size, PROT_READ | PROT_WRITE) != 0) {
        trace("mprotect: failed, errno %d", errno);
        error = "mprotect(RW alias) failed";
        vm_deallocate(mach_task_self(), rw, size);
        return false;
    }
    rwOut = reinterpret_cast<uint8_t*>(rw);
    return true;
}
} // namespace

bool processIsDebugged() {
    uint32_t flags = 0;
    if (csops(getpid(), kCsOpsStatus, &flags, sizeof(flags)) != 0) return false;
    return (flags & kCsDebugged) != 0;
}

Policy selectPolicy(bool txmPresent) { return txmPresent ? Policy::TxmUniversal : Policy::DebuggedDualMap; }

bool allocateRegion(Policy policy, size_t size, CodeRegion& out, std::string& error) {
    size = roundUp(size);
    if (!processIsDebugged()) {
        error = "process is not CS_DEBUGGED; attach a JIT enabler first";
        return false;
    }
    const bool txm = policy == Policy::TxmUniversal;
    trace("allocate: %zu bytes, policy %s%s", size, txm ? "TXM script" : "plain dual map",
          txm && gDebuggerAllocates ? " (debugger allocates)" : "");

    void* rx = nullptr;
    if (!(txm && gDebuggerAllocates)) {
        trace("mmap: requesting an RX region");
        rx = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (rx == MAP_FAILED) {
            trace("mmap: refused, errno %d", errno);
            error = "mmap(RX) refused";
            return false;
        }
        trace("mmap: got %p", rx);
    }
    if (txm) {
        // The script registers the range with the Trusted Execution Monitor
        // and returns the usable RX address. If the app dies on the next line,
        // no script was listening for the breakpoint.
        trace("brk: asking the debugger script to prepare %p", rx);
        void* prepared = txmPrepareRegion(rx, size);
        trace("brk: script returned %p", prepared);
        if (!prepared) {
            if (rx) munmap(rx, size);
            error = "debugger script did not prepare the region";
            return false;
        }
        rx = prepared;
    }
    uint8_t* rw = nullptr;
    if (!remapWritable(static_cast<uint8_t*>(rx), size, rw, error)) {
        munmap(rx, size);
        return false;
    }
    out = CodeRegion{static_cast<uint8_t*>(rx), rw, size};
    trace("allocate: done, rx %p rw %p", rx, static_cast<void*>(rw));
    return true;
}

void finishSetup(Policy policy) {
    if (policy != Policy::TxmUniversal) return;
    trace("brk: telling the debugger script to detach");
    txmDetach();
    trace("brk: detach returned");
}

void freeRegion(CodeRegion& r) {
    if (r.rw) vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(r.rw), r.size);
    if (r.rx) munmap(r.rx, r.size);
    r = CodeRegion{};
}

// ---------------------------------------------------------------------------
// Host builds (Linux CI, macOS, iOS Simulator): same dual-mapped shape so the
// recompiler's emitter is exercised identically in unit tests.
// ---------------------------------------------------------------------------
#else

bool processIsDebugged() { return true; }

Policy selectPolicy(bool) { return Policy::HostDualMap; }

bool allocateRegion(Policy, size_t size, CodeRegion& out, std::string& error) {
    size = roundUp(size);
#if defined(__linux__)
    int fd = memfd_create("ember-jit", 0);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) close(fd);
        error = "memfd_create failed";
        return false;
    }
    void* rw = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* rx = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    if (rw == MAP_FAILED || rx == MAP_FAILED) {
        if (rw != MAP_FAILED) munmap(rw, size);
        if (rx != MAP_FAILED) munmap(rx, size);
        error = "mmap of the dual views failed";
        return false;
    }
    out = CodeRegion{static_cast<uint8_t*>(rx), static_cast<uint8_t*>(rw), size};
    return true;
#else
    (void)out;
    error = "no host JIT mapping implemented for this platform (use the interpreter)";
    return false;
#endif
}

void finishSetup(Policy) {}

void freeRegion(CodeRegion& r) {
    if (r.rw) munmap(r.rw, r.size);
    if (r.rx) munmap(r.rx, r.size);
    r = CodeRegion{};
}

#endif

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------
void flushInstructionCache(const uint8_t* rx, size_t length) {
#if defined(__APPLE__)
    sys_icache_invalidate(const_cast<uint8_t*>(rx), length);
#else
    char* begin = const_cast<char*>(reinterpret_cast<const char*>(rx));
    __builtin___clear_cache(begin, begin + length);
#endif
}

bool smokeTest(const CodeRegion& region) {
    if (!region) return false;
#if defined(__aarch64__)
    const uint8_t code[] = {0x40, 0x05, 0x80, 0x52,  // mov w0, #42
                            0xC0, 0x03, 0x5F, 0xD6}; // ret
#elif defined(__x86_64__)
    const uint8_t code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax, 42
                            0xC3};                        // ret
#else
    return false;
#endif
    if (region.size < sizeof(code)) return false;
    trace("smoke test: writing %zu bytes through the RW alias", sizeof(code));
    std::memcpy(region.rw, code, sizeof(code));
    trace("smoke test: checking the bytes are visible through the RX view");
    const bool visible = std::memcmp(region.rx, code, sizeof(code)) == 0;
    trace("smoke test: RX view %s", visible ? "matches" : "DOES NOT match (the two views are not the same memory)");
    if (!visible) return false;
    flushInstructionCache(region.rx, sizeof(code));
    // If the app dies on the next line, the pages are mapped but iOS refused
    // to execute them: the region was not registered the way this OS expects.
    trace("smoke test: executing generated code at %p", static_cast<void*>(region.rx));
    auto fn = reinterpret_cast<int (*)()>(region.rx);
    const int result = fn();
    trace("smoke test: code returned %d (expected 42)", result);
    return result == 42;
}

} // namespace ember::jit
