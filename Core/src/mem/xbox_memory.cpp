#include "xbox_memory.h"

#include <sys/mman.h>

namespace ember {

bool XboxMemory::init(uint32_t ramMB) {
    shutdown();
    if (ramMB != 64 && ramMB != 128) return false;
    const size_t bytes = size_t(ramMB) << 20;
    // Anonymous pages are zero-filled and only become resident when touched,
    // so a title that uses 30 MiB costs 30 MiB.
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<uint8_t*>(p);
    size_ = static_cast<uint32_t>(bytes);
    return true;
}

void XboxMemory::shutdown() {
    if (base_) munmap(base_, size_);
    base_ = nullptr;
    size_ = 0;
}

bool XboxMemory::readBytes(uint32_t addr, void* dst, uint32_t length) const {
    if (length == 0) return true;
    const uint8_t* p = translate(addr, length);
    if (!p) return false;
    std::memcpy(dst, p, length);
    return true;
}

bool XboxMemory::writeBytes(uint32_t addr, const void* src, uint32_t length) {
    if (length == 0) return true;
    uint8_t* p = translate(addr, length);
    if (!p) return false;
    std::memcpy(p, src, length);
    return true;
}

bool XboxMemory::fill(uint32_t addr, uint8_t value, uint32_t length) {
    if (length == 0) return true;
    uint8_t* p = translate(addr, length);
    if (!p) return false;
    std::memset(p, value, length);
    return true;
}

} // namespace ember
