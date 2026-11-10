// FATX: the filesystem on the console's hard disk and memory units. Read-only.
//
// An HDD image holds several FATX partitions at fixed offsets (no partition
// table on a retail disk). Saves live on E:, the dashboard on C:, and X/Y/Z
// are per-title caches.
#pragma once

#include "byte_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ember {

struct FatxPartition {
    char     letter;
    uint64_t offset;
    uint64_t size;
};

// Fixed layout of a stock retail hard disk. Re-check against a reference
// (e.g. xemu's HDD tooling) before trusting it with real saves.
inline constexpr FatxPartition kRetailPartitions[] = {
    {'X', 0x00080000ull, 0x2EE00000ull},
    {'Y', 0x2EE80000ull, 0x2EE00000ull},
    {'Z', 0x5DC80000ull, 0x2EE00000ull},
    {'C', 0x8CA80000ull, 0x1F400000ull},
    {'E', 0xABE80000ull, 0x131F00000ull},
};

struct FatxEntry {
    std::string name;
    uint32_t    firstCluster = 0;
    uint32_t    size         = 0;
    bool        isDirectory  = false;
};

class FatxVolume {
public:
    // `source` is the partition itself (wrap an HDD image in a SliceSource).
    bool open(ByteSource& source);

    uint32_t clusterBytes() const { return clusterBytes_; }
    uint32_t volumeId() const { return volumeId_; }

    // Paths use '/' or '\\' and are case-insensitive. "" is the root.
    bool list(const std::string& directory, std::vector<FatxEntry>& out);
    bool find(const std::string& path, FatxEntry& out);
    bool readFile(const FatxEntry& entry, uint64_t offset, void* dst, size_t length);

private:
    bool nextCluster(uint32_t cluster, uint32_t& next);
    bool clusterChain(uint32_t first, std::vector<uint32_t>& chain);
    bool readDirectory(uint32_t firstCluster, std::vector<FatxEntry>& out);
    uint64_t clusterOffset(uint32_t cluster) const { return dataOffset_ + uint64_t(cluster - 1) * clusterBytes_; }

    ByteSource* source_       = nullptr;
    uint32_t    volumeId_     = 0;
    uint32_t    clusterBytes_ = 0;
    uint32_t    clusterCount_ = 0;
    uint32_t    fatEntrySize_ = 2;
    uint64_t    dataOffset_   = 0;
};

} // namespace ember
