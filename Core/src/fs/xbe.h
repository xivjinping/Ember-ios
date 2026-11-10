// XBE: the Xbox executable format. Parses the image header, the certificate
// (title name, title ID, region) and the section table.
#pragma once

#include "byte_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ember {

struct XbeSection {
    std::string name;
    uint32_t    flags          = 0;
    uint32_t    virtualAddress = 0;
    uint32_t    virtualSize    = 0;
    uint32_t    rawAddress     = 0; // file offset
    uint32_t    rawSize        = 0;
};

struct XbeImage {
    uint32_t baseAddress        = 0; // 0x00010000 for every retail title
    uint32_t sizeOfHeaders      = 0;
    uint32_t sizeOfImage        = 0;
    uint32_t entryPoint         = 0; // decoded virtual address
    uint32_t kernelThunkAddress = 0; // decoded virtual address
    uint32_t tlsAddress         = 0;
    bool     debugBuild         = false;

    uint32_t    titleId      = 0;
    uint32_t    region       = 0;
    uint32_t    allowedMedia = 0;
    uint32_t    version      = 0;
    std::string titleName;   // UTF-8

    std::vector<XbeSection> sections;

    bool contains(uint32_t address) const {
        return address >= baseAddress && address - baseAddress < sizeOfImage;
    }
};

bool parseXbe(ByteSource& source, XbeImage& out, std::string& error);

// Title IDs are two ASCII publisher characters plus a 16-bit number; they are
// conventionally written like "MS-004". Returns "" if the ID is not of that shape.
std::string titleCode(uint32_t titleId);

} // namespace ember
