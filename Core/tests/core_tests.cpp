// Host-side tests for the portable core. Run on Linux/macOS CI:
//   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
//
// No game data is needed: every image (XBE, XISO, FATX, push buffer) is
// synthesised here.
#include "ember/core_api.h"

#include "../src/cpu/jit/jit_memory.h"
#include "../src/cpu/x86_state.h"
#include "../src/fs/byte_source.h"
#include "../src/fs/fatx.h"
#include "../src/fs/xbe.h"
#include "../src/fs/xiso.h"
#include "../src/kernel/xbe_loader.h"
#include "../src/mem/xbox_memory.h"
#include "../src/nv2a/pushbuffer.h"
#include "../src/nv2a/texture_convert.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace ember;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;

static void put32(Bytes& b, size_t at, uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}
static void put16(Bytes& b, size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
static void putBytes(Bytes& b, size_t at, const Bytes& src) {
    if (b.size() < at + src.size()) b.resize(at + src.size());
    std::memcpy(b.data() + at, src.data(), src.size());
}

// ---------------------------------------------------------------------------
// Synthetic images
// ---------------------------------------------------------------------------

// A one-section retail XBE: code at 0x11000, kernel thunk table at 0x11800.
static Bytes buildXbe(const Bytes& code, const std::vector<uint32_t>& ordinals) {
    Bytes x(0x2000, 0);
    std::memcpy(x.data(), "XBEH", 4);
    put32(x, 0x104, 0x00010000);              // base address
    put32(x, 0x108, 0x1000);                  // size of headers
    put32(x, 0x10C, 0x2000);                  // size of image
    put32(x, 0x118, 0x00010178);              // certificate
    put32(x, 0x11C, 1);                       // sections
    put32(x, 0x120, 0x00010400);              // section headers
    put32(x, 0x128, 0x00011000 ^ 0xA8FC57AB); // entry point (retail key)
    put32(x, 0x158, 0x00011800 ^ 0x5B6D40B6); // kernel thunk table (retail key)

    put32(x, 0x178 + 0x00, 0x1D0);
    put32(x, 0x178 + 0x08, 0x454D0001);       // 'E' 'M' 0001
    const char* name = "Ember Test";
    for (size_t i = 0; name[i]; ++i) x[0x178 + 0x0C + i * 2] = uint8_t(name[i]);
    put32(x, 0x178 + 0xA0, 0x7);              // all regions

    put32(x, 0x400 + 0x00, 0x7);              // flags
    put32(x, 0x400 + 0x04, 0x00011000);       // virtual address
    put32(x, 0x400 + 0x08, 0x1000);           // virtual size
    put32(x, 0x400 + 0x0C, 0x1000);           // raw address
    put32(x, 0x400 + 0x10, 0x1000);           // raw size
    put32(x, 0x400 + 0x14, 0x00010480);       // name
    std::memcpy(x.data() + 0x480, ".text", 6);

    putBytes(x, 0x1000, code);
    for (size_t i = 0; i < ordinals.size(); ++i) put32(x, 0x1800 + i * 4, 0x80000000u | ordinals[i]);
    return x;
}

// eax = 10 + 9 + ... + 1, then call the first kernel import.
static const Bytes kTitleCode = {
    0xB8, 0, 0, 0, 0,                   // mov  eax, 0
    0xB9, 10, 0, 0, 0,                  // mov  ecx, 10
    0x01, 0xC8,                         // add  eax, ecx
    0x49,                               // dec  ecx
    0x75, 0xFB,                         // jnz  -5
    0xFF, 0x15, 0x00, 0x18, 0x01, 0x00, // call dword ptr [0x00011800]
    0xF4,                               // hlt
};

static size_t putDirEntry(Bytes& img, size_t at, uint16_t left, uint16_t right, uint32_t sector, uint32_t size, uint8_t attr,
                          const char* name) {
    put16(img, at, left);
    put16(img, at + 2, right);
    put32(img, at + 4, sector);
    put32(img, at + 8, size);
    img[at + 12] = attr;
    img[at + 13] = uint8_t(std::strlen(name));
    std::memcpy(img.data() + at + 14, name, std::strlen(name));
    return (14 + std::strlen(name) + 3) & ~size_t(3);
}

// XDVDFS volume: /default.xbe, /media/readme.txt
static Bytes buildXiso(const Bytes& xbe) {
    const uint32_t S = 2048;
    Bytes img(S * 48, 0);
    std::memcpy(img.data() + 32 * S, "MICROSOFT*XBOX*MEDIA", 20);
    put32(img, 32 * S + 20, 33); // root directory sector
    put32(img, 32 * S + 24, S);  // root directory size

    std::memset(img.data() + 33 * S, 0xFF, S);
    const size_t first = putDirEntry(img, 33 * S, 0, 0, 40, uint32_t(xbe.size()), 0x20, "default.xbe");
    put16(img, 33 * S + 2, uint16_t(first / 4)); // right child
    putDirEntry(img, 33 * S + first, 0, 0, 34, S, 0x10, "media");

    std::memset(img.data() + 34 * S, 0xFF, S);
    putDirEntry(img, 34 * S, 0, 0, 36, 5, 0x20, "readme.txt");
    std::memcpy(img.data() + 36 * S, "hello", 5);
    putBytes(img, 40 * S, xbe);
    return img;
}

// 1 MiB FATX partition, 4 KiB clusters: /saves/game.sav and /hello.bin (two clusters).
static Bytes buildFatx() {
    Bytes v(1u << 20, 0);
    std::memcpy(v.data(), "FATX", 4);
    put32(v, 4, 0x1234ABCD); // volume id
    put32(v, 8, 8);          // sectors per cluster
    put32(v, 12, 1);
    const size_t fat = 0x1000, data = 0x2000, C = 4096;
    auto setFat = [&](uint32_t cluster, uint16_t next) { put16(v, fat + cluster * 2, next); };
    auto cluster = [&](uint32_t n) { return data + (n - 1) * C; };
    auto entry = [&](size_t at, const char* name, uint8_t attr, uint32_t first, uint32_t size) {
        v[at] = uint8_t(std::strlen(name));
        v[at + 1] = attr;
        std::memcpy(v.data() + at + 2, name, std::strlen(name));
        put32(v, at + 0x2C, first);
        put32(v, at + 0x30, size);
    };
    setFat(1, 0xFFFF); // root
    setFat(2, 0xFFFF); // saves/
    setFat(3, 4);      // hello.bin
    setFat(4, 0xFFFF);
    setFat(5, 0xFFFF); // game.sav
    entry(cluster(1), "saves", 0x10, 2, 0);
    entry(cluster(1) + 64, "hello.bin", 0, 3, 5000);
    v[cluster(1) + 128] = 0xFF;
    entry(cluster(2), "game.sav", 0, 5, 3);
    v[cluster(2) + 64] = 0xFF;
    for (uint32_t i = 0; i < 5000; ++i) v[cluster(3) + i] = uint8_t(i * 7); // clusters 3 and 4 are adjacent here
    std::memcpy(v.data() + cluster(5), "sav", 3);
    return v;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testMemory() {
    std::printf("guest memory\n");
    XboxMemory mem;
    CHECK(!mem.init(96)); // only the two real configurations
    CHECK(mem.init(64) && mem.size() == (64u << 20));
    CHECK(mem.write<uint32_t>(0x00010000, 0xDEADBEEF));
    uint32_t v = 0;
    CHECK(mem.read(0x80010000, v) && v == 0xDEADBEEF); // kernel physical map mirrors low memory
    uint8_t b = 0;
    CHECK(mem.read(0x00010000, b) && b == 0xEF);       // little-endian
    CHECK(!mem.read(0x04000000, v));                   // first byte past 64 MiB
    CHECK(!mem.read(0x03FFFFFE, v));                   // straddles the end
    CHECK(!mem.read(kernelTrapAddress(1), v));         // trap addresses are never backed by RAM
    CHECK(mem.init(128) && mem.read(0x07FFFFFC, v) && v == 0);
}

static void testInterpreter() {
    std::printf("x86 interpreter\n");
    XboxMemory mem;
    CHECK(mem.init(64));
    const uint32_t code = 0x00011000, data = 0x00020000;
    auto load = [&](const Bytes& bytes) { CHECK(mem.writeBytes(code, bytes.data(), uint32_t(bytes.size()))); };
    auto fresh = [&] {
        X86State s;
        s.eip = code;
        s.reg[ESP] = 0x00030000;
        return s;
    };

    // Sum 100..1 into eax, store it, halt.
    load({0xB8, 0, 0, 0, 0, 0xB9, 100, 0, 0, 0, 0x01, 0xC8, 0x49, 0x75, 0xFB,
          0x89, 0x05, 0x00, 0x00, 0x02, 0x00, // mov [0x00020000], eax
          0xF4});
    X86State s = fresh();
    RunResult r = interpret(s, mem, 100000);
    CHECK(r.reason == StopReason::Halt && s.reg[EAX] == 5050 && s.reg[ECX] == 0);
    CHECK(r.executed == 2 + 100 * 3 + 2);
    uint32_t stored = 0;
    CHECK(mem.read(data, stored) && stored == 5050);

    // Stack, signed compare, conditional branch.
    load({0x6A, 0x05,       // push 5
          0x5B,             // pop  ebx
          0x83, 0xFB, 0x07, // cmp  ebx, 7
          0x7C, 0x01,       // jl   +1
          0xF4,             // hlt        (skipped)
          0x43,             // inc  ebx
          0xF4});
    s = fresh();
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::Halt && s.reg[EBX] == 6 && s.reg[ESP] == 0x00030000);

    // SIB addressing, and the FS override used for thread-local data.
    load({0x8D, 0x44, 0x88, 0x10,                   // lea eax, [eax + ecx*4 + 0x10]
          0x64, 0x8B, 0x15, 0x20, 0x00, 0x00, 0x00, // mov edx, fs:[0x20]
          0xF4});
    s = fresh();
    s.reg[EAX] = 0x100;
    s.reg[ECX] = 3;
    s.fsBase = 0x00028000;
    CHECK(mem.write<uint32_t>(0x00028020, 0xCAFEBABE));
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::Halt && s.reg[EAX] == 0x11C && s.reg[EDX] == 0xCAFEBABE);

    // call/ret, and sub with borrow flags.
    load({0xE8, 0x01, 0, 0, 0, // call +1
          0xF4,                // hlt
          0x29, 0xC8,          // sub eax, ecx   (1 - 2)
          0xC3});              // ret
    s = fresh();
    s.reg[EAX] = 1;
    s.reg[ECX] = 2;
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::Halt && s.reg[EAX] == 0xFFFFFFFF);
    CHECK((s.eflags & kFlagCF) && (s.eflags & kFlagSF) && !(s.eflags & kFlagZF) && !(s.eflags & kFlagOF));

    // An indirect call through a resolved thunk stops as a kernel call.
    load({0xFF, 0x15, 0x00, 0x00, 0x02, 0x00}); // call dword ptr [0x00020000]
    CHECK(mem.write<uint32_t>(data, kernelTrapAddress(190)));
    s = fresh();
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::KernelCall && r.ordinal == 190 && r.executed == 1);
    uint32_t returnAddress = 0;
    CHECK(s.reg[ESP] == 0x0002FFFC && mem.read(s.reg[ESP], returnAddress) && returnAddress == code + 6);

    // Gaps are reported, not guessed at.
    load({0x0F, 0xA2}); // cpuid
    s = fresh();
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::Unimplemented && r.opcode == 0x0FA2 && r.address == code && s.eip == code);
    s = fresh();
    s.eip = 0x05000000;
    r = interpret(s, mem, 100);
    CHECK(r.reason == StopReason::Fault && r.address == 0x05000000);
}

static void testXbe() {
    std::printf("xbe parser + loader\n");
    MemorySource file(buildXbe(kTitleCode, {190, 327}));
    XbeImage xbe;
    std::string error;
    CHECK(parseXbe(file, xbe, error));
    CHECK(xbe.baseAddress == 0x10000 && xbe.entryPoint == 0x11000 && xbe.kernelThunkAddress == 0x11800 && !xbe.debugBuild);
    CHECK(xbe.titleName == "Ember Test" && xbe.titleId == 0x454D0001 && titleCode(xbe.titleId) == "EM-001");
    CHECK(xbe.sections.size() == 1 && xbe.sections[0].name == ".text" && xbe.sections[0].virtualAddress == 0x11000);
    CHECK(titleCode(0x00000001).empty());

    MemorySource debug(buildXbe(kTitleCode, {1}));
    put32(debug.data, 0x128, 0x00011000 ^ 0x94859D4B);
    put32(debug.data, 0x158, 0x00011800 ^ 0xEFB1F152);
    CHECK(parseXbe(debug, xbe, error) && xbe.debugBuild && xbe.entryPoint == 0x11000 && xbe.kernelThunkAddress == 0x11800);

    MemorySource bad(Bytes(0x2000, 0));
    CHECK(!parseXbe(bad, xbe, error));
    MemorySource truncated(buildXbe(kTitleCode, {1}));
    truncated.data.resize(0x1800); // section data cut off
    CHECK(!parseXbe(truncated, xbe, error));

    XboxMemory mem;
    CHECK(mem.init(64));
    LoadedXbe loaded;
    CHECK(loadXbe(file, mem, loaded, error));
    CHECK(loaded.importedOrdinals == std::vector<uint32_t>({190, 327}));
    uint32_t thunk = 0;
    CHECK(mem.read(0x11804, thunk) && thunk == kernelTrapAddress(327));
    char magic[4];
    CHECK(mem.readBytes(0x10000, magic, 4) && std::memcmp(magic, "XBEH", 4) == 0); // headers are mapped too

    X86State s;
    s.eip = loaded.image.entryPoint;
    s.reg[ESP] = 0x00030000;
    const RunResult r = interpret(s, mem, 1000);
    CHECK(r.reason == StopReason::KernelCall && r.ordinal == 190 && s.reg[EAX] == 55);
    CHECK(std::string(kernelExportName(190)) == "NtCreateFile" && kernelExportName(7) == nullptr);
}

static void testXiso() {
    std::printf("xiso (xdvdfs)\n");
    const Bytes xbe = buildXbe(kTitleCode, {190});
    MemorySource image(buildXiso(xbe));
    XisoReader disc;
    CHECK(disc.open(image) && disc.partitionOffset() == 0);

    std::vector<XisoEntry> entries;
    CHECK(disc.list("", entries) && entries.size() == 2);
    CHECK(entries[0].name == "default.xbe" && !entries[0].isDirectory && entries[1].name == "media" && entries[1].isDirectory);
    XisoEntry entry;
    CHECK(disc.find("DEFAULT.XBE", entry) && entry.size == xbe.size()); // case-insensitive
    char magic[4];
    CHECK(disc.readFile(entry, 0, magic, 4) && std::memcmp(magic, "XBEH", 4) == 0);
    CHECK(disc.find("media\\readme.txt", entry) && entry.size == 5);
    char text[5];
    CHECK(disc.readFile(entry, 0, text, 5) && std::memcmp(text, "hello", 5) == 0);
    CHECK(!disc.readFile(entry, 3, text, 3)); // past the end of the file
    CHECK(!disc.find("missing.bin", entry));
    CHECK(disc.list("media", entries) && entries.size() == 1);

    MemorySource notADisc(Bytes(2048 * 40, 0));
    CHECK(!disc.open(notADisc));
}

static void testFatx() {
    std::printf("fatx\n");
    // The partition sits 64 KiB into a larger "disk", as on a real HDD image.
    Bytes diskBytes(0x10000, 0);
    const Bytes partition = buildFatx();
    diskBytes.insert(diskBytes.end(), partition.begin(), partition.end());
    MemorySource disk(std::move(diskBytes));
    SliceSource slice(disk, 0x10000, partition.size());

    FatxVolume volume;
    CHECK(volume.open(slice) && volume.clusterBytes() == 4096 && volume.volumeId() == 0x1234ABCD);
    std::vector<FatxEntry> entries;
    CHECK(volume.list("", entries) && entries.size() == 2);
    CHECK(entries[0].name == "saves" && entries[0].isDirectory && entries[1].name == "hello.bin" && entries[1].size == 5000);

    FatxEntry entry;
    CHECK(volume.find("HELLO.BIN", entry));
    Bytes got(5000);
    CHECK(volume.readFile(entry, 0, got.data(), got.size()));
    bool same = true;
    for (uint32_t i = 0; i < 5000; ++i) same &= got[i] == uint8_t(i * 7);
    CHECK(same); // spans two clusters through the allocation table
    uint8_t tail[4];
    CHECK(volume.readFile(entry, 4996, tail, 4) && tail[3] == uint8_t(4999 * 7));
    CHECK(!volume.readFile(entry, 4999, tail, 2));

    CHECK(volume.find("saves/game.sav", entry) && entry.size == 3);
    char sav[3];
    CHECK(volume.readFile(entry, 0, sav, 3) && std::memcmp(sav, "sav", 3) == 0);
    CHECK(!volume.find("saves/none", entry));

    MemorySource blank(Bytes(1u << 20, 0));
    CHECK(!volume.open(blank));
    CHECK(kRetailPartitions[4].letter == 'E');
}

static void testPushBuffer() {
    std::printf("nv2a push buffer\n");
    using namespace nv2a;
    auto header = [](uint32_t method, uint32_t count, bool increasing = true) {
        return (count << 18) | method | (increasing ? 0 : 0x40000000u);
    };
    const uint32_t words[] = {
        header(NV097_SET_COLOR_CLEAR_VALUE, 2), 0xFF112233, 0xF3, // increasing: second word lands on CLEAR_SURFACE
        header(NV097_SET_BEGIN_END, 1), 5,
        header(NV097_INLINE_ARRAY, 3, false), 0x11, 0x22, 0x33,   // non-increasing: all three go to one method
        header(NV097_SET_BEGIN_END, 1), 0,
        52 | 1,                                                   // 44: jump to 52
        0xFFFFFFFF,                                               // 48: never executed
        64 | 2,                                                   // 52: call 64
        76 | 1,                                                   // 56: jump to the end
        0xFFFFFFFF,                                               // 60: never executed
        header(NV097_FLIP_STALL, 1), 0,                           // 64: subroutine
        0x00020000,                                               // 72: return
        0,                                                        // 76: put
    };
    const auto* dma = reinterpret_cast<const uint8_t*>(words);
    FrameTracker tracker;
    PushBufferResult r = runPushBuffer(dma, sizeof(words), 0, 76, tracker);
    CHECK(!r.error && r.get == 76 && r.methods == 8);
    CHECK(tracker.clearColor == 0xFF112233 && tracker.clears == 1 && tracker.primitives == 1);
    CHECK(tracker.inlineWords == 3 && tracker.flips == 1 && !tracker.insideBeginEnd);

    const uint32_t reserved[] = {0x80000000u, 0};
    r = runPushBuffer(reinterpret_cast<const uint8_t*>(reserved), sizeof(reserved), 0, 4, tracker);
    CHECK(r.error);
    const uint32_t loop[] = {0 | 1, 0}; // jumps to itself forever
    r = runPushBuffer(reinterpret_cast<const uint8_t*>(loop), sizeof(loop), 0, 4, tracker);
    CHECK(r.error);
    const uint32_t wild[] = {0x1000 | 1, 0}; // jump outside the buffer
    r = runPushBuffer(reinterpret_cast<const uint8_t*>(wild), sizeof(wild), 0, 4, tracker);
    CHECK(r.error);
}

static void testTextures() {
    std::printf("nv2a texture conversion\n");
    using namespace nv2a;
    CHECK(swizzleIndex(1, 0, 4, 4) == 1 && swizzleIndex(0, 1, 4, 4) == 2 && swizzleIndex(2, 0, 4, 4) == 4);
    CHECK(swizzleIndex(3, 3, 4, 4) == 15);
    CHECK(swizzleIndex(4, 0, 8, 2) == 8 && swizzleIndex(2, 1, 8, 2) == 6); // non-square: leftover x bits follow

    const uint32_t w = 8, h = 4;
    std::vector<uint16_t> swizzled(w * h), linear(w * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) swizzled[swizzleIndex(x, y, w, h)] = uint16_t(y * w + x);
    unswizzle(reinterpret_cast<const uint8_t*>(swizzled.data()), w, h, 2, reinterpret_cast<uint8_t*>(linear.data()), w * 2);
    bool ordered = true;
    for (uint32_t i = 0; i < w * h; ++i) ordered &= linear[i] == i;
    CHECK(ordered);

    auto one = [](TexelFormat format, uint32_t texel) {
        uint32_t out = 0;
        convertToBGRA8(format, reinterpret_cast<const uint8_t*>(&texel), 4, 1, 1, reinterpret_cast<uint8_t*>(&out), 4);
        return out; // 0xAARRGGBB
    };
    CHECK(one(TexelFormat::R5G6B5, 0xF800) == 0xFFFF0000);
    CHECK(one(TexelFormat::R5G6B5, 0x07E0) == 0xFF00FF00);
    CHECK(one(TexelFormat::A1R5G5B5, 0x7FFF) == 0x00FFFFFF);
    CHECK(one(TexelFormat::A1R5G5B5, 0x801F) == 0xFF0000FF);
    CHECK(one(TexelFormat::A4R4G4B4, 0xF00F) == 0xFF0000FF);
    CHECK(one(TexelFormat::X8R8G8B8, 0x00123456) == 0xFF123456);
    CHECK(one(TexelFormat::A8R8G8B8, 0x80123456) == 0x80123456);
}

static void testJit() {
    std::printf("jit dual mapping\n");
    jit::CodeRegion region;
    std::string error;
    const jit::Policy policy = jit::selectPolicy(false);
    if (!jit::allocateRegion(policy, 1u << 20, region, error)) {
        std::printf("  skipped: %s\n", error.c_str());
        return;
    }
    CHECK(region.rx != region.rw);
    jit::finishSetup(policy);
#if defined(__x86_64__) || defined(__aarch64__)
    CHECK(jit::smokeTest(region));
#endif
    jit::freeRegion(region);
    CHECK(!region);
}

static void testProbeAndCore(const fs::path& dir) {
    std::printf("title probe + stub backend through the C API\n");
    const Bytes xbe = buildXbe(kTitleCode, {190, 327});
    const Bytes iso = buildXiso(xbe);
    auto write = [&](const fs::path& path, const Bytes& bytes, uint64_t at = 0) {
        fs::create_directories(path.parent_path());
        std::fstream f(path, std::ios::binary | std::ios::out | std::ios::trunc);
        f.seekp(std::streamoff(at)); // leaves a hole: a sparse file on any modern filesystem
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return path.string();
    };
    em_title_info info;

    CHECK(em_probe_title(write(dir / "default.xbe", xbe).c_str(), &info));
    CHECK(info.format == EM_TITLE_XBE && info.verified && info.title_id == 0x454D0001);
    CHECK(std::string(info.name) == "Ember Test" && std::string(info.title_code) == "EM-001");

    const std::string trimmed = write(dir / "game.iso", iso);
    CHECK(em_probe_title(trimmed.c_str(), &info) && info.format == EM_TITLE_XISO && info.verified);
    CHECK(std::string(info.name) == "Ember Test");

    // Full disc dump: the same volume behind a 387 MiB video partition.
    const std::string redump = write(dir / "full dump.iso", iso, 0x18300000ull);
    CHECK(em_probe_title(redump.c_str(), &info) && info.format == EM_TITLE_XISO && info.verified);
    CHECK(std::string(info.name) == "Ember Test" && info.size_bytes == 0x18300000ull + iso.size());

    // An .iso that is not an Xbox disc is listed, but flagged.
    CHECK(em_probe_title(write(dir / "other console.iso", Bytes(4096, 0x55)).c_str(), &info));
    CHECK(info.format == EM_TITLE_XISO && !info.verified && info.title_id == 0);
    CHECK(!em_probe_title(write(dir / "notes.txt", Bytes(64, 0x55)).c_str(), &info));

    write(dir / "Extracted" / "Default.xbe", xbe);
    CHECK(em_probe_title((dir / "Extracted").string().c_str(), &info) && info.format == EM_TITLE_FOLDER && info.verified);

    em_config cfg{};
    cfg.ram_mb = 96;
    cfg.render_scale = 1.0f;
    cfg.vsync = true;
    CHECK(em_core_create(&cfg) == nullptr);
    cfg.ram_mb = 64;
    em_core* core = em_core_create(&cfg);
    CHECK(core != nullptr);
    if (!core) return;

    char err[160] = {};
    CHECK(!em_core_load_title(core, (dir / "other console.iso").string().c_str(), err, sizeof(err)) && err[0] != 0);
    CHECK(em_core_load_title(core, redump.c_str(), err, sizeof(err)));

    em_pad_state pad{};
    pad.analog[EM_ANALOG_A] = 255;
    em_core_set_pad(core, 0, &pad);
    em_core_start(core);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));

    const uint8_t* px = nullptr;
    uint32_t w = 0, h = 0;
    uint64_t serial = 0;
    CHECK(em_core_lock_frame(core, &px, &w, &h, &serial));
    CHECK(w == 640 && h == 480 && serial > 30 && px != nullptr);
    em_core_unlock_frame(core);

    std::vector<int16_t> pcm(512 * 2);
    CHECK(em_core_read_audio(core, pcm.data(), 512) == 512);
    bool nonSilent = false;
    for (int16_t sample : pcm) nonSilent |= sample != 0;
    CHECK(nonSilent);

    em_stats stats{};
    em_core_get_stats(core, &stats);
    CHECK(stats.fps > 50 && stats.fps < 70 && stats.ram_mb == 64);
    CHECK(std::strstr(stats.status, "kernel export 190 (NtCreateFile)") != nullptr);
    std::printf("  %.1f fps; status: %s\n", stats.fps, stats.status);

    em_core_set_render_scale(core, 4.5f); // live switch; the CPU-drawn stub caps itself at 2x
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    em_core_get_stats(core, &stats);
    CHECK(stats.frame_width == 1280 && stats.frame_height == 960);

    em_core_stop(core);
    em_core_destroy(core);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("ember-tests-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    testMemory();
    testInterpreter();
    testXbe();
    testXiso();
    testFatx();
    testPushBuffer();
    testTextures();
    testJit();
    testProbeAndCore(dir);
    fs::remove_all(dir);
    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
