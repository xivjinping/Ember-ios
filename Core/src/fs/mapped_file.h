// Windowed, read-only memory mapping for multi-gigabyte title files.
//
// Mapping a 20 GB .wud in one piece burns address space and invites the
// kernel to keep far more of it resident than a 4-6 GB phone can afford.
// Instead a small LRU of fixed-size windows is mapped on demand. The pages
// are clean and file-backed, so iOS can drop them under pressure without
// counting them against the app's dirty-memory (jetsam) limit, and the
// worst-case resident cost is windowBytes * maxWindows per open file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ember {

class MappedFile {
public:
    explicit MappedFile(size_t windowBytes = 8u << 20, size_t maxWindows = 8);
    ~MappedFile();
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool     open(const std::string& path);
    void     close();
    bool     isOpen() const { return fd_ >= 0; }
    uint64_t size() const { return size_; }

    // Copies `len` bytes starting at `offset`. Fails on out-of-range reads.
    bool read(uint64_t offset, void* dst, size_t len);

    size_t mappedWindowCount() const { return windows_.size(); }

private:
    struct Window {
        uint64_t index;
        uint8_t* ptr;
        size_t   length;
        uint64_t lastUse;
    };
    Window* acquire(uint64_t index);

    int                 fd_   = -1;
    uint64_t            size_ = 0;
    size_t              windowBytes_;
    size_t              maxWindows_;
    uint64_t            clock_ = 0;
    std::vector<Window> windows_;
    std::mutex          mutex_;
};

} // namespace ember
