#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

// Little-endian writer and reader for Nyanroid's own save files.
class Writer {
public:
    void u8(uint8_t v) { out_.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        u32(b);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void bytes(ByteSpan b) {
        u32(static_cast<uint32_t>(b.size()));
        out_.insert(out_.end(), b.begin(), b.end());
    }
    Bytes take() { return std::move(out_); }

private:
    Bytes out_;
};

class Reader {
public:
    explicit Reader(ByteSpan in) : r_(in, "save data") {}
    uint8_t u8() { return r_.u8(); }
    uint32_t u32() { return r_.u32(); }
    int32_t i32() { return r_.i32(); }
    float f32() {
        uint32_t b = r_.u32();
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }
    std::string str() {
        uint32_t n = r_.u32();
        auto s = r_.span(n);
        return std::string(s.begin(), s.end());
    }
    Bytes bytes() {
        uint32_t n = r_.u32();
        auto s = r_.span(n);
        return Bytes(s.begin(), s.end());
    }
    bool done() const { return r_.remaining() == 0; }

private:
    ByteReader r_;
};

}  // namespace nyan::wa2
