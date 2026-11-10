// Guest physical memory: one allocation of 64 or 128 MiB.
//
// The whole console has 64 MiB (128 on debug kits and upgraded boards),
// shared between CPU and GPU, so the emulator's largest fixed cost is small
// and known up front — no sparse reservation tricks are needed, and the app
// cannot be pushed over the iOS memory limit by the guest.
//
// The region is visible to the guest twice: at 0x00000000 (where title code
// runs; XBEs load at 0x00010000) and at 0x80000000 (the kernel's linear map
// of physical memory, used for contiguous/GPU allocations). Both ranges
// translate to the same host bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ember {

class XboxMemory {
public:
    static constexpr uint32_t kPhysicalMapBase = 0x80000000u;

    XboxMemory() = default;
    ~XboxMemory() { shutdown(); }
    XboxMemory(const XboxMemory&)            = delete;
    XboxMemory& operator=(const XboxMemory&) = delete;

    // ramMB must be 64 or 128; anything else is rejected.
    bool init(uint32_t ramMB);
    void shutdown();

    uint32_t size() const { return size_; }
    uint8_t* base() const { return base_; } // for the recompiler: host = base + (addr & mask) after a range check

    inline uint8_t* translate(uint32_t addr, uint32_t length = 1) const {
        const uint32_t physical = addr & ~kPhysicalMapBase;
        if (physical >= size_ || length > size_ - physical) return nullptr;
        return base_ + physical;
    }

    // x86 is little-endian, as is every iOS device, so accessors are plain copies.
    template <typename T> inline bool read(uint32_t addr, T& value) const {
        const uint8_t* p = translate(addr, sizeof(T));
        if (!p) return false;
        std::memcpy(&value, p, sizeof(T));
        return true;
    }
    template <typename T> inline bool write(uint32_t addr, T value) {
        uint8_t* p = translate(addr, sizeof(T));
        if (!p) return false;
        std::memcpy(p, &value, sizeof(T));
        return true;
    }
    bool readBytes(uint32_t addr, void* dst, uint32_t length) const;
    bool writeBytes(uint32_t addr, const void* src, uint32_t length);
    bool fill(uint32_t addr, uint8_t value, uint32_t length);

private:
    uint8_t* base_ = nullptr;
    uint32_t size_ = 0;
};

} // namespace ember
