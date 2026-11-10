// XDVDFS ("XISO"): the Xbox game-disc filesystem. Read-only.
//
// Works on trimmed images (filesystem at offset 0) and on full disc dumps,
// where the game partition sits behind a DVD-Video partition.
#pragma once

#include "byte_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ember {

struct XisoEntry {
    std::string name;
    uint32_t    sector      = 0;
    uint32_t    size        = 0;
    bool        isDirectory = false;
};

class XisoReader {
public:
    static constexpr uint32_t kSectorSize = 2048;

    // Returns false if no XDVDFS volume descriptor is found.
    bool open(ByteSource& source);

    uint64_t partitionOffset() const { return partitionOffset_; }

    // Paths use '/' or '\\' and are case-insensitive, as on the console. "" is the root.
    bool list(const std::string& directory, std::vector<XisoEntry>& out);
    bool find(const std::string& path, XisoEntry& out);
    bool readFile(const XisoEntry& entry, uint64_t offset, void* dst, size_t length);

    // Byte range of a file within the underlying source, for wrapping in a SliceSource.
    uint64_t fileOffset(const XisoEntry& entry) const { return partitionOffset_ + uint64_t(entry.sector) * kSectorSize; }

private:
    bool readDirectory(uint32_t sector, uint32_t size, std::vector<XisoEntry>& out);

    ByteSource* source_          = nullptr;
    uint64_t    partitionOffset_ = 0;
    uint32_t    rootSector_      = 0;
    uint32_t    rootSize_        = 0;
};

} // namespace ember
