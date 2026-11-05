#include "nyan/cs2/blowfish.hpp"

#include <cstring>
#include <utility>

namespace nyan::cs2 {
namespace {
#include "cs2/blowfish_tables.inc"
}  // namespace

Blowfish::Blowfish(ByteSpan key) {
    if (key.empty()) fail("Blowfish key must not be empty");
    std::memcpy(p_, kInitialP, sizeof(p_));
    std::memcpy(s_, kInitialS, sizeof(s_));

    size_t k = 0;
    for (uint32_t& p : p_) {
        uint32_t word = 0;
        for (int b = 0; b < 4; ++b) {
            word = (word << 8) | key[k];
            k = (k + 1) % key.size();
        }
        p ^= word;
    }

    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        encryptBlock(l, r);
        p_[i] = l;
        p_[i + 1] = r;
    }
    for (auto& box : s_) {
        for (int i = 0; i < 256; i += 2) {
            encryptBlock(l, r);
            box[i] = l;
            box[i + 1] = r;
        }
    }
}

void Blowfish::encryptBlock(uint32_t& left, uint32_t& right) const {
    uint32_t l = left, r = right;
    for (int i = 0; i < 16; ++i) {
        l ^= p_[i];
        r ^= feistel(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[16];
    l ^= p_[17];
    left = l;
    right = r;
}

void Blowfish::decryptBlock(uint32_t& left, uint32_t& right) const {
    uint32_t l = left, r = right;
    for (int i = 17; i > 1; --i) {
        l ^= p_[i];
        r ^= feistel(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[1];
    l ^= p_[0];
    left = l;
    right = r;
}

void Blowfish::encrypt(uint8_t* data, size_t length) const {
    for (size_t i = 0; i + 8 <= length; i += 8) {
        uint32_t l = readLE32(data + i), r = readLE32(data + i + 4);
        encryptBlock(l, r);
        writeLE32(data + i, l);
        writeLE32(data + i + 4, r);
    }
}

void Blowfish::decrypt(uint8_t* data, size_t length) const {
    for (size_t i = 0; i + 8 <= length; i += 8) {
        uint32_t l = readLE32(data + i), r = readLE32(data + i + 4);
        decryptBlock(l, r);
        writeLE32(data + i, l);
        writeLE32(data + i + 4, r);
    }
}

}  // namespace nyan::cs2
