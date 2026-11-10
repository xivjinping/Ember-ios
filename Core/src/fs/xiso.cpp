#include "xiso.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ember {

namespace {

const char     kMagic[]          = "MICROSOFT*XBOX*MEDIA"; // 20 bytes, at the start of sector 32
constexpr uint32_t kMagicLength  = 20;
constexpr uint32_t kVolumeSector = 32;

// Where the game partition can start: trimmed image, then a full original-
// Xbox disc dump (the game data follows a 387 MiB video partition).
constexpr uint64_t kPartitionOffsets[] = {0, 0x18300000ull};

constexpr uint32_t kMaxDirectoryBytes = 16u << 20;
constexpr size_t   kMaxEntries        = 65536;

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
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

bool XisoReader::open(ByteSource& source) {
    source_ = nullptr;
    for (uint64_t base : kPartitionOffsets) {
        uint8_t descriptor[kMagicLength + 8];
        const uint64_t at = base + uint64_t(kVolumeSector) * kSectorSize;
        if (!source.read(at, descriptor, sizeof(descriptor))) continue;
        if (std::memcmp(descriptor, kMagic, kMagicLength) != 0) continue;
        source_          = &source;
        partitionOffset_ = base;
        rootSector_      = le32(descriptor + kMagicLength);
        rootSize_        = le32(descriptor + kMagicLength + 4);
        return true;
    }
    return false;
}

// A directory is a binary search tree stored in one blob. Each node holds the
// offsets of its left and right children in 4-byte units, then the file's
// location, size, attributes and name.
bool XisoReader::readDirectory(uint32_t sector, uint32_t size, std::vector<XisoEntry>& out) {
    out.clear();
    if (!source_ || size > kMaxDirectoryBytes) return false;
    if (size == 0) return true; // empty directory
    std::vector<uint8_t> blob(size);
    if (!source_->read(partitionOffset_ + uint64_t(sector) * kSectorSize, blob.data(), size)) return false;

    std::vector<uint32_t> pending{0};
    std::vector<bool>     visited(size / 4 + 1, false); // guards against cyclic images
    while (!pending.empty()) {
        const uint32_t unit = pending.back();
        pending.pop_back();
        const uint64_t at = uint64_t(unit) * 4;
        if (at + 14 > size || visited[unit]) continue;
        visited[unit] = true;

        const uint8_t* node = blob.data() + at;
        const uint16_t left = le16(node), right = le16(node + 2);
        if (left == 0xFFFF && right == 0xFFFF) continue; // sector padding
        const uint8_t nameLength = node[13];
        if (at + 14 + nameLength > size) return false;

        XisoEntry entry;
        entry.sector      = le32(node + 4);
        entry.size        = le32(node + 8);
        entry.isDirectory = (node[12] & 0x10) != 0;
        entry.name.assign(reinterpret_cast<const char*>(node + 14), nameLength);
        out.push_back(std::move(entry));
        if (out.size() > kMaxEntries) return false;

        if (right != 0 && right != 0xFFFF) pending.push_back(right);
        if (left != 0 && left != 0xFFFF) pending.push_back(left);
    }
    std::sort(out.begin(), out.end(), [](const XisoEntry& a, const XisoEntry& b) { return a.name < b.name; });
    return true;
}

bool XisoReader::list(const std::string& directory, std::vector<XisoEntry>& out) {
    if (directory.empty() || splitPath(directory).empty()) return readDirectory(rootSector_, rootSize_, out);
    XisoEntry entry;
    if (!find(directory, entry) || !entry.isDirectory) return false;
    return readDirectory(entry.sector, entry.size, out);
}

bool XisoReader::find(const std::string& path, XisoEntry& out) {
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty()) return false;
    uint32_t sector = rootSector_, size = rootSize_;
    std::vector<XisoEntry> entries;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!readDirectory(sector, size, entries)) return false;
        auto it = std::find_if(entries.begin(), entries.end(), [&](const XisoEntry& e) { return equalsIgnoreCase(e.name, parts[i]); });
        if (it == entries.end()) return false;
        if (i + 1 == parts.size()) {
            out = *it;
            return true;
        }
        if (!it->isDirectory) return false;
        sector = it->sector;
        size   = it->size;
    }
    return false;
}

bool XisoReader::readFile(const XisoEntry& entry, uint64_t offset, void* dst, size_t length) {
    if (!source_ || entry.isDirectory || offset > entry.size || length > entry.size - offset) return false;
    return source_->read(fileOffset(entry) + offset, dst, length);
}

} // namespace ember
