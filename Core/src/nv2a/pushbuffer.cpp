#include "pushbuffer.h"

#include <cstring>

namespace ember::nv2a {

namespace {
inline uint32_t wordAt(const uint8_t* dma, uint32_t offset) {
    uint32_t v;
    std::memcpy(&v, dma + offset, 4);
    return v;
}
} // namespace

PushBufferResult runPushBuffer(const uint8_t* dma, uint32_t dmaSize, uint32_t get, uint32_t put, MethodSink& sink) {
    PushBufferResult r;
    uint32_t method = 0, subchannel = 0, remaining = 0;
    bool     increasing = true;
    bool     inSubroutine = false;
    uint32_t returnAddress = 0;
    uint64_t steps = 0;

    auto fail = [&] {
        r.error = true;
        r.get = get;
        return r;
    };

    while (get != put) {
        if (dmaSize < 4 || get > dmaSize - 4 || (get & 3)) return fail();
        if (++steps > (uint64_t(dmaSize) << 4)) return fail(); // a jump loop that never reaches `put`
        const uint32_t word = wordAt(dma, get);
        get += 4;

        if (remaining) { // parameter for the current method header
            sink.method(subchannel, method, word);
            r.methods++;
            if (increasing) method += 4;
            remaining--;
            continue;
        }

        if ((word & 0xE0000003u) == 0x20000000u) {          // old-style jump
            get = word & 0x1FFFFFFCu;
        } else if ((word & 3u) == 1u) {                     // jump
            get = word & 0xFFFFFFFCu;
        } else if ((word & 3u) == 2u) {                     // call
            if (inSubroutine) return fail();
            inSubroutine  = true;
            returnAddress = get;
            get = word & 0xFFFFFFFCu;
        } else if (word == 0x00020000u) {                   // return
            if (!inSubroutine) return fail();
            inSubroutine = false;
            get = returnAddress;
        } else if ((word & 0xE0030003u) == 0u || (word & 0xE0030003u) == 0x40000000u) {
            increasing = (word & 0x40000000u) == 0;         // method header
            method     = word & 0x1FFCu;
            subchannel = (word >> 13) & 7u;
            remaining  = (word >> 18) & 0x7FFu;
        } else {
            return fail();                                  // reserved encoding
        }
    }
    r.get = get;
    return r;
}

void FrameTracker::method(uint32_t, uint32_t method, uint32_t parameter) {
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL: surfaceWidth = parameter >> 16; break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:   surfaceHeight = parameter >> 16; break;
    case NV097_SET_COLOR_CLEAR_VALUE:       clearColor = parameter; break;
    case NV097_CLEAR_SURFACE:               clears++; break;
    case NV097_SET_BEGIN_END:
        if (parameter == 0) {
            if (insideBeginEnd) primitives++;
            insideBeginEnd = false;
        } else {
            insideBeginEnd = true;
        }
        break;
    case NV097_DRAW_ARRAYS:
        arrayDraws++;
        verticesDrawn += (parameter >> 24) + 1;
        break;
    case NV097_INLINE_ARRAY:                inlineWords++; break;
    case NV097_FLIP_STALL:                  flips++; break;
    default: break;
    }
}

} // namespace ember::nv2a
