// Random-access byte sources, so every parser (XBE, XDVDFS, FATX) works the
// same on a file on disk, a file inside a disc image, or a test buffer.
#pragma once

#include "mapped_file.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ember {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const = 0;
    virtual bool read(uint64_t offset, void* dst, size_t length) = 0;

    template <typename T> bool readValue(uint64_t offset, T& value) { return read(offset, &value, sizeof(T)); }
};

// A file on disk, read through a small window of mappings (see mapped_file.h).
class FileSource final : public ByteSource {
public:
    bool open(const std::string& path) { return file_.open(path); }
    uint64_t size() const override { return file_.size(); }
    bool read(uint64_t offset, void* dst, size_t length) override { return file_.read(offset, dst, length); }

private:
    MappedFile file_;
};

class MemorySource final : public ByteSource {
public:
    MemorySource() = default;
    explicit MemorySource(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}
    uint64_t size() const override { return data.size(); }
    bool read(uint64_t offset, void* dst, size_t length) override {
        if (offset > data.size() || length > data.size() - offset) return false;
        std::memcpy(dst, data.data() + offset, length);
        return true;
    }
    std::vector<uint8_t> data;
};

// A window onto another source: a partition in an HDD image, or one file in a disc image.
class SliceSource final : public ByteSource {
public:
    SliceSource(ByteSource& parent, uint64_t offset, uint64_t length) : parent_(parent), offset_(offset), length_(length) {}
    uint64_t size() const override { return length_; }
    bool read(uint64_t offset, void* dst, size_t length) override {
        if (offset > length_ || length > length_ - offset) return false;
        return parent_.read(offset_ + offset, dst, length);
    }

private:
    ByteSource& parent_;
    uint64_t    offset_, length_;
};

} // namespace ember
