#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// AES-256 decryption (FIPS-197), as Mware.dll's NPK3 reader uses it: CBC over each archive's index
// and over each stored segment, restarting from the archive's IV every time (Mware.dll of Full Metal
// Daemon Muramasa, 0x100b7090 and 0x100b8400; AES-NI with a software fallback, same result).
class Aes256 {
public:
    explicit Aes256(const std::array<uint8_t, 32>& key);

    // Decrypts `blocks` 16-byte blocks in place in CBC mode, starting from `iv`.
    void decryptCbc(uint8_t* data, size_t blocks, const uint8_t iv[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    // For tests: one block forward.
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t enc_[60];  // the expanded key
    uint32_t dec_[60];  // the equivalent inverse cipher's round keys
};

// SHA-256 (FIPS 180-4). Each NPK3 entry carries the digest of its unpacked data, and Mware keys its
// entry tables by the digest of the lower-cased name (0x100b69f0, 0x100b6e30).
std::array<uint8_t, 32> sha256(ByteSpan data);

}  // namespace nyan::diesel
