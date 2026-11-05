#include "nyan/diesel/random.hpp"

namespace nyan::diesel {

namespace {

// The words are signed in Mware's code: >> on them is arithmetic. Multiplication and xor are done
// on the unsigned bits (the same in two's complement).
inline int32_t sar(int32_t v, int n) { return v >> n; }
inline int32_t mul(int32_t a, uint32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * b); }

constexpr int32_t kMag[2] = {0, static_cast<int32_t>(0x9908b0dfu)};

inline int32_t mix(int32_t hi, int32_t lo) {
    // ((lo ^ hi) & 0x7fffffff) ^ hi: hi's top bit, lo's other bits.
    return ((lo ^ hi) & 0x7fffffff) ^ hi;
}

}  // namespace

void MwareRandom::seed(int32_t s) {
    mt_[0] = s;
    for (int i = 1; i < kN; ++i) mt_[i] = mul(sar(mt_[i - 1], 30) ^ mt_[i - 1], 0x6c078965u) + i;
    index_ = kN;
}

int32_t MwareRandom::next() {
    if (index_ >= kN) {
        int kk = 0;
        for (; kk < kN - kM; ++kk) {
            int32_t y = mix(mt_[kk], mt_[kk + 1]);
            mt_[kk] = sar(y, 1) ^ kMag[y & 1] ^ mt_[kk + kM];
        }
        for (; kk < kN - 1; ++kk) {
            int32_t y = mix(mt_[kk], mt_[kk + 1]);
            mt_[kk] = sar(y, 1) ^ kMag[y & 1] ^ mt_[kk + kM - kN];
        }
        int32_t y = mix(mt_[kN - 1], mt_[0]);
        mt_[kN - 1] = kMag[y & 1] ^ mt_[kM - 1] ^ sar(y, 1);
        index_ = 0;
    }
    int32_t y = mt_[index_++];
    y ^= sar(y, 11);
    y ^= static_cast<int32_t>((static_cast<uint32_t>(y) & 0xff3a58adu) << 7);
    y ^= static_cast<int32_t>((static_cast<uint32_t>(y) & 0xffffdf8cu) << 15);
    return sar(y, 18) ^ y;
}

int32_t MwareRandom::nextInt(int32_t low, int32_t high) {
    int32_t r = next();
    int32_t range = static_cast<int32_t>(static_cast<uint32_t>(high) - static_cast<uint32_t>(low) + 1u);
    if (range == 0) return low;  // idiv by zero: the exe faults
    if (range == -1) return low;  // INT_MIN / -1 also faults; any remainder of -1 is 0
    return sar(r, 1) % range + low;
}

float MwareRandom::nextFloat(float low, float high) {
    int32_t r = next() % 100000001;
    return static_cast<float>(r) * 1e-8f * (high - low) + low;
}

}  // namespace nyan::diesel
