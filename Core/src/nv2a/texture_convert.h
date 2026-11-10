// NV2A texture decoding for Metal.
//
// Two things stand between guest texture memory and an MTLTexture:
//   * Layout. Most NV2A textures are "swizzled": texels are stored along a
//     Z-order (Morton) curve so the GPU's cache behaves well. Metal wants rows.
//   * Format. The 16-bit formats have no renderable Metal equivalent with the
//     same channel order on every device, so they are expanded to BGRA8.
// DXT1/3/5 need neither step: where BC formats are supported they upload as-is.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ember::nv2a {

enum class TexelFormat {
    A8R8G8B8, // 32-bit, already BGRA8 in memory
    X8R8G8B8, // 32-bit, alpha forced to opaque
    R5G6B5,
    A1R5G5B5,
    A4R4G4B4,
};

uint32_t bytesPerTexel(TexelFormat format);

// Index of texel (x, y) in a swizzled width x height texture (both powers of
// two). Bits of x and y interleave, x lowest, until the smaller dimension
// runs out; the larger one's remaining bits follow.
uint32_t swizzleIndex(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// Rearranges a swizzled image into rows. `dstPitch` is bytes per output row.
void unswizzle(const uint8_t* src, uint32_t width, uint32_t height, uint32_t bytesPerTexel, uint8_t* dst, size_t dstPitch);

// Expands linear (row-ordered) texels to BGRA8, the layout of MTLPixelFormatBGRA8Unorm.
void convertToBGRA8(TexelFormat format, const uint8_t* src, size_t srcPitch, uint32_t width, uint32_t height, uint8_t* dst,
                    size_t dstPitch);

} // namespace ember::nv2a
