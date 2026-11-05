#pragma once

#include <cstddef>
#include <cstdint>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// Blowfish as used by CatSystem2: the standard cipher, except that each 8-byte
// block is read and written as two *little-endian* 32-bit halves.
class Blowfish {
public:
    explicit Blowfish(ByteSpan key);

    void encryptBlock(uint32_t& left, uint32_t& right) const;
    void decryptBlock(uint32_t& left, uint32_t& right) const;

    // In-place ECB over the leading multiple-of-8 bytes; any trailing 0-7 bytes are left as-is.
    void encrypt(uint8_t* data, size_t length) const;
    void decrypt(uint8_t* data, size_t length) const;

private:
    uint32_t feistel(uint32_t x) const {
        return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) + s_[3][x & 0xFF];
    }

    uint32_t p_[18];
    uint32_t s_[4][256];
};

}  // namespace nyan::cs2
