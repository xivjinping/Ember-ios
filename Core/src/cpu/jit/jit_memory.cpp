#include "jit_memory.h"

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
} // namespace

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
    kern_return_t kr = vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                reinterpret_cast<vm_address_t>(rx), FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        error = "vm_remap failed (kern_return " + std::to_string(kr) + ")";
        return false;
    }
    if (mprotect(reinterpret_cast<void*>(rw), size, PROT_READ | PROT_WRITE) != 0) {
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
    void* rx = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) {
        error = "mmap(RX) refused";
        return false;
    }
    if (policy == Policy::TxmUniversal) {
        // The script registers the range with the Trusted Execution Monitor.
        // It returns the usable RX address (normally the one we passed).
        void* prepared = txmPrepareRegion(rx, size);
        if (!prepared) {
            munmap(rx, size);
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
    return true;
}

void finishSetup(Policy policy) {
    if (policy == Policy::TxmUniversal) txmDetach();
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
    std::memcpy(region.rw, code, sizeof(code));
    flushInstructionCache(region.rx, sizeof(code));
    auto fn = reinterpret_cast<int (*)()>(region.rx);
    return fn() == 42;
}

} // namespace ember::jit
