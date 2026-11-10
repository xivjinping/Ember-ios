#include "fatx.h"

#include <algorithm>
#include <cctype>

namespace ember {

namespace {

constexpr uint32_t kSuperblockBytes = 0x1000;
constexpr uint32_t kSectorBytes     = 512;
constexpr uint32_t kDirEntryBytes   = 64;
constexpr uint32_t kMaxNameLength   = 42;
constexpr uint32_t kRootCluster     = 1;
constexpr uint32_t kAttrDirectory   = 0x10;

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) parts.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(std::move(current));
    return parts;
}

} // namespace

bool FatxVolume::open(ByteSource& source) {
    source_ = nullptr;
    uint8_t sb[16];
    if (!source.read(0, sb, sizeof(sb)) || std::memcmp(sb, "FATX", 4) != 0) return false;
    const uint32_t sectorsPerCluster = le32(sb + 8);
    // Valid cluster sizes are powers of two from 1 to 128 sectors (512 B .. 64 KiB).
    if (sectorsPerCluster == 0 || sectorsPerCluster > 128 || (sectorsPerCluster & (sectorsPerCluster - 1))) return false;

    volumeId_     = le32(sb + 4);
    clusterBytes_ = sectorsPerCluster * kSectorBytes;
    clusterCount_ = static_cast<uint32_t>(source.size() / clusterBytes_);
    // Small volumes use 16-bit allocation-table entries, large ones 32-bit.
    fatEntrySize_ = clusterCount_ < 0xFFF0 ? 2 : 4;
    const uint64_t fatBytes = (uint64_t(clusterCount_) * fatEntrySize_ + 0xFFF) & ~uint64_t(0xFFF);
    dataOffset_ = kSuperblockBytes + fatBytes;
    if (dataOffset_ >= source.size()) return false;
    source_ = &source;
    return true;
}

bool FatxVolume::nextCluster(uint32_t cluster, uint32_t& next) {
    const uint64_t at = kSuperblockBytes + uint64_t(cluster) * fatEntrySize_;
    if (fatEntrySize_ == 2) {
        uint16_t v = 0;
        if (!source_->readValue(at, v)) return false;
        next = v >= 0xFFF8 ? 0xFFFFFFFFu : v;
    } else {
        uint32_t v = 0;
        if (!source_->readValue(at, v)) return false;
        next = v >= 0xFFFFFFF8u ? 0xFFFFFFFFu : v;
    }
    return true;
}

bool FatxVolume::clusterChain(uint32_t first, std::vector<uint32_t>& chain) {
    chain.clear();
    uint32_t cluster = first;
    while (cluster != 0xFFFFFFFFu) {
        if (cluster == 0 || cluster >= clusterCount_ || chain.size() > clusterCount_) return false; // free, out of range, or a loop
        chain.push_back(cluster);
        if (!nextCluster(cluster, cluster)) return false;
    }
    return true;
}

bool FatxVolume::readDirectory(uint32_t firstCluster, std::vector<FatxEntry>& out) {
    out.clear();
    std::vector<uint32_t> chain;
    if (!source_ || !clusterChain(firstCluster, chain)) return false;
    std::vector<uint8_t> block(clusterBytes_);
    for (uint32_t cluster : chain) {
        if (!source_->read(clusterOffset(cluster), block.data(), block.size())) return false;
        for (uint32_t at = 0; at + kDirEntryBytes <= clusterBytes_; at += kDirEntryBytes) {
            const uint8_t* e = block.data() + at;
            const uint8_t nameLength = e[0];
            if (nameLength == 0x00 || nameLength == 0xFF) return true; // end of directory
            if (nameLength == 0xE5 || nameLength > kMaxNameLength) continue; // deleted
            FatxEntry entry;
            entry.name.assign(reinterpret_cast<const char*>(e + 2), nameLength);
            entry.isDirectory  = (e[1] & kAttrDirectory) != 0;
            entry.firstCluster = le32(e + 0x2C);
            entry.size         = le32(e + 0x30);
            out.push_back(std::move(entry));
        }
    }
    return true;
}

bool FatxVolume::list(const std::string& directory, std::vector<FatxEntry>& out) {
    if (splitPath(directory).empty()) return readDirectory(kRootCluster, out);
    FatxEntry entry;
    if (!find(directory, entry) || !entry.isDirectory) return false;
    return readDirectory(entry.firstCluster, out);
}

bool FatxVolume::find(const std::string& path, FatxEntry& out) {
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty()) return false;
    uint32_t cluster = kRootCluster;
    std::vector<FatxEntry> entries;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!readDirectory(cluster, entries)) return false;
        auto it = std::find_if(entries.begin(), entries.end(), [&](const FatxEntry& e) { return equalsIgnoreCase(e.name, parts[i]); });
        if (it == entries.end()) return false;
        if (i + 1 == parts.size()) {
            out = *it;
            return true;
        }
        if (!it->isDirectory) return false;
        cluster = it->firstCluster;
    }
    return false;
}

bool FatxVolume::readFile(const FatxEntry& entry, uint64_t offset, void* dst, size_t length) {
    if (!source_ || entry.isDirectory || offset > entry.size || length > entry.size - offset) return false;
    if (length == 0) return true;
    std::vector<uint32_t> chain;
    if (!clusterChain(entry.firstCluster, chain)) return false;
    auto* out = static_cast<uint8_t*>(dst);
    while (length) {
        const uint64_t index  = offset / clusterBytes_;
        const uint32_t within = static_cast<uint32_t>(offset % clusterBytes_);
        if (index >= chain.size()) return false; // file size claims more than its chain holds
        const size_t chunk = std::min<size_t>(length, clusterBytes_ - within);
        if (!source_->read(clusterOffset(chain[index]) + within, out, chunk)) return false;
        out += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

} // namespace ember
