#include "mapped_file.h"

#include <algorithm>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ember {

MappedFile::MappedFile(size_t windowBytes, size_t maxWindows)
    : windowBytes_((windowBytes + 0xFFFF) & ~size_t(0xFFFF)), // 64 KiB multiple: valid for 4K and 16K pages
      maxWindows_(std::max<size_t>(maxWindows, 1)) {}

MappedFile::~MappedFile() { close(); }

bool MappedFile::open(const std::string& path) {
    close();
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    struct stat st {};
    if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    size_ = static_cast<uint64_t>(st.st_size);
    return true;
}

void MappedFile::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& w : windows_) munmap(w.ptr, w.length);
    windows_.clear();
    if (fd_ >= 0) ::close(fd_);
    fd_   = -1;
    size_ = 0;
}

MappedFile::Window* MappedFile::acquire(uint64_t index) {
    for (auto& w : windows_) {
        if (w.index == index) {
            w.lastUse = ++clock_;
            return &w;
        }
    }
    if (windows_.size() >= maxWindows_) {
        auto victim = std::min_element(windows_.begin(), windows_.end(),
                                       [](const Window& a, const Window& b) { return a.lastUse < b.lastUse; });
        munmap(victim->ptr, victim->length);
        windows_.erase(victim);
    }
    const uint64_t start  = index * windowBytes_;
    const size_t   length = static_cast<size_t>(std::min<uint64_t>(windowBytes_, size_ - start));
    void* p = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd_, static_cast<off_t>(start));
    if (p == MAP_FAILED) return nullptr;
    madvise(p, length, MADV_RANDOM); // disc images are read sparsely; don't read ahead whole windows
    windows_.push_back(Window{index, static_cast<uint8_t*>(p), length, ++clock_});
    return &windows_.back();
}

bool MappedFile::read(uint64_t offset, void* dst, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0 || offset > size_ || len > size_ - offset) return false;
    auto* out = static_cast<uint8_t*>(dst);
    while (len) {
        const uint64_t index  = offset / windowBytes_;
        const size_t   within = static_cast<size_t>(offset % windowBytes_);
        Window* w = acquire(index);
        if (!w) return false;
        const size_t chunk = std::min(len, w->length - within);
        std::memcpy(out, w->ptr + within, chunk);
        out += chunk;
        offset += chunk;
        len -= chunk;
    }
    return true;
}

} // namespace ember
