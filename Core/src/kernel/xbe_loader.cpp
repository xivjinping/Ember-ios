#include "xbe_loader.h"

#include "../cpu/x86_state.h"
#include "../mem/xbox_memory.h"

#include <algorithm>

namespace ember {

namespace {
constexpr uint32_t kImportFlag  = 0x80000000u; // thunk entries are (flag | ordinal) until resolved
constexpr uint32_t kMaxImports  = 512;
constexpr uint32_t kChunk       = 1u << 16;

bool copyIn(ByteSource& source, uint64_t fileOffset, XboxMemory& memory, uint32_t address, uint32_t length) {
    std::vector<uint8_t> buffer(std::min(length, kChunk));
    while (length) {
        const uint32_t n = std::min(length, kChunk);
        if (!source.read(fileOffset, buffer.data(), n) || !memory.writeBytes(address, buffer.data(), n)) return false;
        fileOffset += n;
        address += n;
        length -= n;
    }
    return true;
}
} // namespace

bool loadXbe(ByteSource& source, XboxMemory& memory, LoadedXbe& out, std::string& error) {
    out = LoadedXbe{};
    if (!parseXbe(source, out.image, error)) return false;
    const XbeImage& xbe = out.image;

    if (!memory.translate(xbe.baseAddress, xbe.sizeOfImage)) {
        error = "image does not fit in guest RAM";
        return false;
    }
    if (!copyIn(source, 0, memory, xbe.baseAddress, xbe.sizeOfHeaders)) {
        error = "could not load the image headers";
        return false;
    }
    for (const XbeSection& section : xbe.sections) {
        // Sections are zero-filled to their virtual size (.bss-style tails),
        // then the stored bytes are copied in. Every section is loaded up
        // front; the console loads non-preload sections on demand.
        if (!memory.fill(section.virtualAddress, 0, section.virtualSize) ||
            !copyIn(source, section.rawAddress, memory, section.virtualAddress, std::min(section.rawSize, section.virtualSize))) {
            error = "section '" + section.name + "' does not fit in guest RAM";
            return false;
        }
    }

    // Kernel imports: a zero-terminated array of ordinals.
    for (uint32_t i = 0; i < kMaxImports; ++i) {
        const uint32_t slot = xbe.kernelThunkAddress + i * 4;
        uint32_t value = 0;
        if (!memory.read(slot, value)) {
            error = "kernel thunk table lies outside guest RAM";
            return false;
        }
        if (value == 0) return true;
        if (!(value & kImportFlag)) continue; // already an address
        const uint32_t ordinal = value & ~kImportFlag;
        if (ordinal >= kKernelTrapCount) {
            error = "kernel import with an out-of-range ordinal";
            return false;
        }
        memory.write(slot, kernelTrapAddress(ordinal));
        out.importedOrdinals.push_back(ordinal);
    }
    error = "kernel thunk table is not terminated";
    return false;
}

// A few well-known exports, for readable diagnostics only. The full table
// (about 370 entries) belongs with the HLE kernel implementation.
const char* kernelExportName(uint32_t ordinal) {
    switch (ordinal) {
    case 49:  return "HalReturnToFirmware";
    case 95:  return "KeBugCheck";
    case 165: return "MmAllocateContiguousMemory";
    case 184: return "NtAllocateVirtualMemory";
    case 187: return "NtClose";
    case 190: return "NtCreateFile";
    case 219: return "NtReadFile";
    case 236: return "NtWriteFile";
    case 255: return "PsCreateSystemThreadEx";
    case 322: return "XboxHardwareInfo";
    case 324: return "XboxKrnlVersion";
    case 327: return "XeLoadSection";
    default:  return nullptr;
    }
}

} // namespace ember
