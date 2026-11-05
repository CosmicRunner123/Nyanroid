#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/error.hpp"

namespace nyan {

using Bytes = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;

inline uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// Bounds-checked little-endian cursor over a byte span. Throws nyan::Error on overrun.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data, std::string_view what = "data") : data_(data), what_(what) {}

    size_t size() const { return data_.size(); }
    size_t pos() const { return pos_; }
    size_t remaining() const { return data_.size() - pos_; }
    void seek(size_t pos) {
        if (pos > data_.size()) overrun(pos - data_.size());
        pos_ = pos;
    }
    void skip(size_t n) { seek(pos_ + require(n)); }

    uint8_t u8() { require(1); return data_[pos_++]; }
    uint16_t u16() { require(2); auto v = readLE16(&data_[pos_]); pos_ += 2; return v; }
    uint32_t u32() { require(4); auto v = readLE32(&data_[pos_]); pos_ += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }

    ByteSpan span(size_t n) {
        require(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    // Fixed-size field holding a NUL-terminated string.
    std::string fixedString(size_t n) {
        auto s = span(n);
        size_t len = 0;
        while (len < n && s[len] != 0) ++len;
        return std::string(reinterpret_cast<const char*>(s.data()), len);
    }

private:
    size_t require(size_t n) {
        if (n > remaining()) overrun(n - remaining());
        return n;
    }
    [[noreturn]] void overrun(size_t by) {
        fail("Unexpected end of " + std::string(what_) + " (needed " + std::to_string(by) +
             " more bytes at offset " + std::to_string(pos_) + ")");
    }

    ByteSpan data_;
    std::string_view what_;
    size_t pos_ = 0;
};

}  // namespace nyan
