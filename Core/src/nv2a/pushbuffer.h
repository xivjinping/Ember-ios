// NV2A command stream.
//
// Titles do not call a graphics API the emulator can intercept: Direct3D 8
// is statically linked into each game and writes GPU commands ("methods")
// straight into a push buffer that the NV2A's FIFO engine consumes. This is
// the front of the GPU emulator: decoding that stream into (subchannel,
// method, parameter) triples and tracking the state they set up.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ember::nv2a {

// Kelvin (the 3D engine, class 0x97) methods used by the state tracker.
// Values follow the public register maps; cross-check against xemu's
// nv2a_regs.h when extending.
enum : uint32_t {
    NV097_NO_OPERATION              = 0x0100,
    NV097_FLIP_STALL                = 0x0130,
    NV097_SET_SURFACE_CLIP_HORIZONTAL = 0x0200, // (width << 16) | x
    NV097_SET_SURFACE_CLIP_VERTICAL = 0x0204,   // (height << 16) | y
    NV097_SET_SURFACE_FORMAT        = 0x0208,
    NV097_SET_BEGIN_END             = 0x17FC,   // primitive type, or 0 to end
    NV097_ARRAY_ELEMENT16           = 0x1800,
    NV097_ARRAY_ELEMENT32           = 0x1808,
    NV097_DRAW_ARRAYS               = 0x1810,   // ((count - 1) << 24) | start
    NV097_INLINE_ARRAY              = 0x1818,
    NV097_SET_ZSTENCIL_CLEAR_VALUE  = 0x1D8C,
    NV097_SET_COLOR_CLEAR_VALUE     = 0x1D90,   // A8R8G8B8
    NV097_CLEAR_SURFACE             = 0x1D94,   // bit mask: Z, stencil, R, G, B, A
};

class MethodSink {
public:
    virtual ~MethodSink() = default;
    virtual void method(uint32_t subchannel, uint32_t method, uint32_t parameter) = 0;
};

struct PushBufferResult {
    uint32_t get      = 0;     // where parsing stopped (== put on success)
    uint64_t methods  = 0;     // method/parameter pairs delivered
    bool     error    = false; // reserved command encoding or out-of-bounds jump
};

// Runs the FIFO from `get` to `put` (byte offsets into the DMA object that
// holds the push buffer). Handles increasing and non-increasing method
// headers, jumps, and one level of call/return, as the hardware does.
PushBufferResult runPushBuffer(const uint8_t* dma, uint32_t dmaSize, uint32_t get, uint32_t put, MethodSink& sink);

// Minimal state tracker: enough to know what a frame consists of. A renderer
// replaces this with one that builds Metal state from the same methods.
class FrameTracker final : public MethodSink {
public:
    void method(uint32_t subchannel, uint32_t method, uint32_t parameter) override;

    uint32_t clearColor     = 0;
    uint32_t surfaceWidth   = 0, surfaceHeight = 0;
    uint32_t clears         = 0;
    uint32_t primitives     = 0; // completed begin/end pairs
    uint32_t arrayDraws     = 0; // DRAW_ARRAYS batches
    uint32_t verticesDrawn  = 0;
    uint32_t inlineWords    = 0;
    uint32_t flips          = 0;
    bool     insideBeginEnd = false;
};

} // namespace ember::nv2a
