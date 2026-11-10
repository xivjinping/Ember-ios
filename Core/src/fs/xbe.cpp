#include "xbe.h"

#include <cstdio>

namespace ember {

namespace {

// The entry point and kernel thunk address are stored XORed with one of two
// fixed constants depending on whether the image was built for retail or
// debug kernels. These are the same constants every open XBE loader uses.
constexpr uint32_t kEntryRetail = 0xA8FC57ABu, kEntryDebug = 0x94859D4Bu;
constexpr uint32_t kThunkRetail = 0x5B6D40B6u, kThunkDebug = 0xEFB1F152u;

constexpr uint32_t kHeaderSize      = 0x178;
constexpr uint32_t kSectionHdrSize  = 0x38;
constexpr uint32_t kMaxSections     = 512;

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string readCString(ByteSource& source, uint64_t offset, size_t maxLength) {
    std::string s;
    for (size_t i = 0; i < maxLength; ++i) {
        char c = 0;
        if (!source.readValue(offset + i, c) || c == 0) break;
        s += c;
    }
    return s;
}

} // namespace

std::string titleCode(uint32_t titleId) {
    const char a = char(titleId >> 24), b = char((titleId >> 16) & 0xFF);
    auto printable = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!printable(a) || !printable(b)) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%c-%03u", a, b, titleId & 0xFFFF);
    return buf;
}

bool parseXbe(ByteSource& source, XbeImage& out, std::string& error) {
    out = XbeImage{};
    uint8_t h[kHeaderSize];
    if (!source.read(0, h, sizeof(h))) {
        error = "file is too small to be an XBE";
        return false;
    }
    if (std::memcmp(h, "XBEH", 4) != 0) {
        error = "missing XBEH signature";
        return false;
    }

    out.baseAddress   = le32(h + 0x104);
    out.sizeOfHeaders = le32(h + 0x108);
    out.sizeOfImage   = le32(h + 0x10C);
    out.tlsAddress    = le32(h + 0x12C);
    const uint32_t certificateAddress = le32(h + 0x118);
    const uint32_t sectionCount       = le32(h + 0x11C);
    const uint32_t sectionsAddress    = le32(h + 0x120);
    const uint32_t rawEntry           = le32(h + 0x128);
    const uint32_t rawThunk           = le32(h + 0x158);

    if (out.sizeOfHeaders < kHeaderSize || out.sizeOfHeaders > source.size() || out.sizeOfImage < out.sizeOfHeaders) {
        error = "implausible header sizes";
        return false;
    }

    // Retail unless only the debug key yields an address inside the image.
    out.entryPoint = rawEntry ^ kEntryRetail;
    if (!out.contains(out.entryPoint) && out.contains(rawEntry ^ kEntryDebug)) {
        out.entryPoint = rawEntry ^ kEntryDebug;
        out.debugBuild = true;
    }
    if (!out.contains(out.entryPoint)) {
        error = "entry point does not decode to an address inside the image";
        return false;
    }
    out.kernelThunkAddress = rawThunk ^ (out.debugBuild ? kThunkDebug : kThunkRetail);

    // Header-relative structures are addressed by VA; the headers are loaded
    // at the base address, so file offset = VA - base.
    auto headerOffset = [&](uint32_t address, uint32_t length, uint64_t& offset) {
        if (address < out.baseAddress) return false;
        offset = address - out.baseAddress;
        return offset + length <= out.sizeOfHeaders;
    };

    uint64_t offset = 0;
    uint8_t cert[0xB0];
    if (!headerOffset(certificateAddress, sizeof(cert), offset) || !source.read(offset, cert, sizeof(cert))) {
        error = "certificate lies outside the headers";
        return false;
    }
    out.titleId      = le32(cert + 0x08);
    out.allowedMedia = le32(cert + 0x9C);
    out.region       = le32(cert + 0xA0);
    out.version      = le32(cert + 0xAC);
    for (int i = 0; i < 40; ++i) { // UTF-16LE, not necessarily terminated
        const uint32_t unit = uint32_t(cert[0x0C + i * 2]) | (uint32_t(cert[0x0D + i * 2]) << 8);
        if (unit == 0) break;
        appendUtf8(out.titleName, (unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
    }

    if (sectionCount > kMaxSections) {
        error = "implausible section count";
        return false;
    }
    for (uint32_t i = 0; i < sectionCount; ++i) {
        uint8_t sh[kSectionHdrSize];
        if (!headerOffset(sectionsAddress + i * kSectionHdrSize, sizeof(sh), offset) || !source.read(offset, sh, sizeof(sh))) {
            error = "section table lies outside the headers";
            return false;
        }
        XbeSection section;
        section.flags          = le32(sh + 0x00);
        section.virtualAddress = le32(sh + 0x04);
        section.virtualSize    = le32(sh + 0x08);
        section.rawAddress     = le32(sh + 0x0C);
        section.rawSize        = le32(sh + 0x10);
        uint64_t nameOffset = 0;
        if (headerOffset(le32(sh + 0x14), 1, nameOffset)) section.name = readCString(source, nameOffset, 32);
        if (uint64_t(section.rawAddress) + section.rawSize > source.size()) {
            error = "section '" + section.name + "' extends past the end of the file";
            return false;
        }
        out.sections.push_back(std::move(section));
    }
    return true;
}

} // namespace ember
