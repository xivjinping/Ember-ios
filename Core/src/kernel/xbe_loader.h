// HLE title loader: does what the kernel does when it launches an XBE,
// without a BIOS image. Copies the headers and sections into guest RAM and
// resolves the kernel import table to HLE trap addresses.
#pragma once

#include "../fs/xbe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ember {

class XboxMemory;

struct LoadedXbe {
    XbeImage              image;
    std::vector<uint32_t> importedOrdinals; // kernel exports the title links against, in table order
};

// On success the title's entry point is `out.image.entryPoint`, and every
// kernel thunk holds kernelTrapAddress(ordinal) (see cpu/x86_state.h), so a
// call into the kernel stops the CPU with StopReason::KernelCall.
//
// Not handled here, and required before real titles run: data exports (some
// ordinals are variables, not functions), TLS setup, the KPCR/TIB that FS
// points at, and library-version-specific patches.
bool loadXbe(ByteSource& source, XboxMemory& memory, LoadedXbe& out, std::string& error);

// Name of a kernel export for diagnostics, or nullptr if not in the short table.
const char* kernelExportName(uint32_t ordinal);

} // namespace ember
