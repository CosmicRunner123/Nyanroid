#include "nyan/diesel/crypto.hpp"

#include <cstring>

namespace nyan::diesel {

namespace {

struct Tables {
    uint8_t sbox[256];
    uint8_t inv[256];
    uint32_t te[4][256];  // forward round tables
    uint32_t td[4][256];  // inverse round tables

    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
            b >>= 1;
        }
        return r;
    }

    Tables() {
        // The S-box: the multiplicative inverse in GF(2^8) followed by the affine map.
        for (int x = 0; x < 256; ++x) {
            uint8_t invx = 0;
            if (x) {
                for (int y = 1; y < 256; ++y) {
                    if (mul(static_cast<uint8_t>(x), static_cast<uint8_t>(y)) == 1) {
                        invx = static_cast<uint8_t>(y);
                        break;
                    }
                }
            }
            uint8_t s = invx;
            uint8_t r = s;
            for (int i = 0; i < 4; ++i) {
                s = static_cast<uint8_t>((s << 1) | (s >> 7));
                r ^= s;
            }
            sbox[x] = static_cast<uint8_t>(r ^ 0x63);
        }
        for (int x = 0; x < 256; ++x) inv[sbox[x]] = static_cast<uint8_t>(x);
        for (int x = 0; x < 256; ++x) {
            uint8_t s = sbox[x];
            uint32_t e = (uint32_t(mul(s, 2)) << 24) | (uint32_t(s) << 16) | (uint32_t(s) << 8) | mul(s, 3);
            uint8_t v = inv[x];
            uint32_t d = (uint32_t(mul(v, 14)) << 24) | (uint32_t(mul(v, 9)) << 16) |
                         (uint32_t(mul(v, 13)) << 8) | mul(v, 11);
            for (int t = 0; t < 4; ++t) {
                te[t][x] = t ? (e >> (8 * t)) | (e << (32 - 8 * t)) : e;
                td[t][x] = t ? (d >> (8 * t)) | (d << (32 - 8 * t)) : d;
            }
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void putBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

}  // namespace

Aes256::Aes256(const std::array<uint8_t, 32>& key) {
    const Tables& t = tables();
    auto subWord = [&](uint32_t w) {
        return (uint32_t(t.sbox[w >> 24]) << 24) | (uint32_t(t.sbox[(w >> 16) & 0xff]) << 16) |
               (uint32_t(t.sbox[(w >> 8) & 0xff]) << 8) | t.sbox[w & 0xff];
    };
    for (int i = 0; i < 8; ++i) enc_[i] = be32(&key[4 * i]);
    uint32_t rcon = 0x01;
    for (int i = 8; i < 60; ++i) {
        uint32_t w = enc_[i - 1];
        if (i % 8 == 0) {
            w = subWord((w << 8) | (w >> 24)) ^ (rcon << 24);
            rcon = Tables::mul(static_cast<uint8_t>(rcon), 2);
        } else if (i % 8 == 4) {
            w = subWord(w);
        }
        enc_[i] = enc_[i - 8] ^ w;
    }
    // Equivalent inverse cipher: round keys in reverse order, InvMixColumns on the middle ones.
    for (int r = 0; r <= 14; ++r) {
        for (int c = 0; c < 4; ++c) {
            uint32_t w = enc_[(14 - r) * 4 + c];
            if (r != 0 && r != 14) {
                w = t.td[0][t.sbox[w >> 24]] ^ t.td[1][t.sbox[(w >> 16) & 0xff]] ^
                    t.td[2][t.sbox[(w >> 8) & 0xff]] ^ t.td[3][t.sbox[w & 0xff]];
            }
            dec_[r * 4 + c] = w;
        }
    }
}

void Aes256::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const Tables& t = tables();
    uint32_t s0 = be32(in) ^ enc_[0], s1 = be32(in + 4) ^ enc_[1];
    uint32_t s2 = be32(in + 8) ^ enc_[2], s3 = be32(in + 12) ^ enc_[3];
    for (int r = 1; r < 14; ++r) {
        const uint32_t* k = &enc_[r * 4];
        uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][(s1 >> 16) & 0xff] ^ t.te[2][(s2 >> 8) & 0xff] ^ t.te[3][s3 & 0xff] ^ k[0];
        uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][(s2 >> 16) & 0xff] ^ t.te[2][(s3 >> 8) & 0xff] ^ t.te[3][s0 & 0xff] ^ k[1];
        uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][(s3 >> 16) & 0xff] ^ t.te[2][(s0 >> 8) & 0xff] ^ t.te[3][s1 & 0xff] ^ k[2];
        uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][(s0 >> 16) & 0xff] ^ t.te[2][(s1 >> 8) & 0xff] ^ t.te[3][s2 & 0xff] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint32_t* k = &enc_[56];
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return (uint32_t(t.sbox[a >> 24]) << 24) | (uint32_t(t.sbox[(b >> 16) & 0xff]) << 16) |
               (uint32_t(t.sbox[(c >> 8) & 0xff]) << 8) | t.sbox[d & 0xff];
    };
    putBe32(out, last(s0, s1, s2, s3) ^ k[0]);
    putBe32(out + 4, last(s1, s2, s3, s0) ^ k[1]);
    putBe32(out + 8, last(s2, s3, s0, s1) ^ k[2]);
    putBe32(out + 12, last(s3, s0, s1, s2) ^ k[3]);
}

void Aes256::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const Tables& t = tables();
    uint32_t s0 = be32(in) ^ dec_[0], s1 = be32(in + 4) ^ dec_[1];
    uint32_t s2 = be32(in + 8) ^ dec_[2], s3 = be32(in + 12) ^ dec_[3];
    for (int r = 1; r < 14; ++r) {
        const uint32_t* k = &dec_[r * 4];
        uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xff] ^ t.td[2][(s2 >> 8) & 0xff] ^ t.td[3][s1 & 0xff] ^ k[0];
        uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xff] ^ t.td[2][(s3 >> 8) & 0xff] ^ t.td[3][s2 & 0xff] ^ k[1];
        uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xff] ^ t.td[2][(s0 >> 8) & 0xff] ^ t.td[3][s3 & 0xff] ^ k[2];
        uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xff] ^ t.td[2][(s1 >> 8) & 0xff] ^ t.td[3][s0 & 0xff] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    const uint32_t* k = &dec_[56];
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return (uint32_t(t.inv[a >> 24]) << 24) | (uint32_t(t.inv[(b >> 16) & 0xff]) << 16) |
               (uint32_t(t.inv[(c >> 8) & 0xff]) << 8) | t.inv[d & 0xff];
    };
    putBe32(out, last(s0, s3, s2, s1) ^ k[0]);
    putBe32(out + 4, last(s1, s0, s3, s2) ^ k[1]);
    putBe32(out + 8, last(s2, s1, s0, s3) ^ k[2]);
    putBe32(out + 12, last(s3, s2, s1, s0) ^ k[3]);
}

void Aes256::decryptCbc(uint8_t* data, size_t blocks, const uint8_t iv[16]) const {
    uint8_t prev[16], cur[16];
    std::memcpy(prev, iv, 16);
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t* p = data + 16 * b;
        std::memcpy(cur, p, 16);
        decryptBlock(cur, p);
        for (int i = 0; i < 16; ++i) p[i] ^= prev[i];
        std::memcpy(prev, cur, 16);
    }
}

std::array<uint8_t, 32> sha256(ByteSpan data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    size_t n = data.size();
    size_t full = n / 64;
    for (size_t i = 0; i < full; ++i) block(data.data() + 64 * i);
    uint8_t tail[128] = {};
    size_t rem = n - full * 64;
    if (rem) std::memcpy(tail, data.data() + full * 64, rem);
    tail[rem] = 0x80;
    size_t tailLen = rem < 56 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    block(tail);
    if (tailLen == 128) block(tail + 64);
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) putBe32(&out[4 * i], h[i]);
    return out;
}

}  // namespace nyan::diesel
