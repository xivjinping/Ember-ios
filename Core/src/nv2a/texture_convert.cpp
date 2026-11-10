#include "texture_convert.h"

#include <cstring>

namespace ember::nv2a {

uint32_t bytesPerTexel(TexelFormat format) {
    return (format == TexelFormat::A8R8G8B8 || format == TexelFormat::X8R8G8B8) ? 4 : 2;
}

uint32_t swizzleIndex(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t index = 0, outBit = 0;
    for (uint32_t bit = 0; (1u << bit) < width || (1u << bit) < height; ++bit) {
        if ((1u << bit) < width) index |= ((x >> bit) & 1u) << outBit++;
        if ((1u << bit) < height) index |= ((y >> bit) & 1u) << outBit++;
    }
    return index;
}

void unswizzle(const uint8_t* src, uint32_t width, uint32_t height, uint32_t bpt, uint8_t* dst, size_t dstPitch) {
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = dst + size_t(y) * dstPitch;
        for (uint32_t x = 0; x < width; ++x)
            std::memcpy(row + size_t(x) * bpt, src + size_t(swizzleIndex(x, y, width, height)) * bpt, bpt);
    }
}

namespace {
// Replicate the high bits into the low ones so full-scale stays full-scale (31 -> 255).
inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t expand4(uint32_t v) { return (v << 4) | v; }
inline uint32_t pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }
} // namespace

void convertToBGRA8(TexelFormat format, const uint8_t* src, size_t srcPitch, uint32_t width, uint32_t height, uint8_t* dst,
                    size_t dstPitch) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* in = src + size_t(y) * srcPitch;
        uint8_t* out      = dst + size_t(y) * dstPitch;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t argb = 0;
            if (bytesPerTexel(format) == 4) {
                std::memcpy(&argb, in + size_t(x) * 4, 4);
                if (format == TexelFormat::X8R8G8B8) argb |= 0xFF000000u;
            } else {
                uint16_t v;
                std::memcpy(&v, in + size_t(x) * 2, 2);
                switch (format) {
                case TexelFormat::R5G6B5:
                    argb = pack(0xFF, expand5(v >> 11), expand6((v >> 5) & 0x3F), expand5(v & 0x1F));
                    break;
                case TexelFormat::A1R5G5B5:
                    argb = pack((v & 0x8000) ? 0xFF : 0, expand5((v >> 10) & 0x1F), expand5((v >> 5) & 0x1F), expand5(v & 0x1F));
                    break;
                default: // A4R4G4B4
                    argb = pack(expand4(v >> 12), expand4((v >> 8) & 0xF), expand4((v >> 4) & 0xF), expand4(v & 0xF));
                    break;
                }
            }
            std::memcpy(out + size_t(x) * 4, &argb, 4); // little-endian 0xAARRGGBB == bytes B, G, R, A
        }
    }
}

} // namespace ember::nv2a
