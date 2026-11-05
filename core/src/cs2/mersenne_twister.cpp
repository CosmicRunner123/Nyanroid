#include "nyan/cs2/mersenne_twister.hpp"

namespace nyan::cs2 {

namespace {
constexpr size_t N = 624;
constexpr size_t M = 397;
constexpr uint32_t kMatrixA = 0x9908B0DF;
constexpr uint32_t kUpper = 0x80000000;
constexpr uint32_t kLower = 0x7FFFFFFF;
}  // namespace

MersenneTwister::MersenneTwister(uint32_t seed) {
    for (size_t i = 0; i < N; ++i) {
        state_[i] = seed & 0xFFFF0000;
        seed = 69069 * seed + 1;
        state_[i] |= (seed & 0xFFFF0000) >> 16;
        seed = 69069 * seed + 1;
    }
    index_ = N;
}

void MersenneTwister::twist() {
    auto mix = [](uint32_t a, uint32_t b, uint32_t c) {
        uint32_t y = (a & kUpper) | (b & kLower);
        return c ^ (y >> 1) ^ ((y & 1) ? kMatrixA : 0);
    };
    size_t kk = 0;
    for (; kk < N - M; ++kk) state_[kk] = mix(state_[kk], state_[kk + 1], state_[kk + M]);
    for (; kk < N - 1; ++kk) state_[kk] = mix(state_[kk], state_[kk + 1], state_[kk + M - N]);
    state_[N - 1] = mix(state_[N - 1], state_[0], state_[M - 1]);
    index_ = 0;
}

uint32_t MersenneTwister::next() {
    if (index_ >= N) twist();
    uint32_t y = state_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9D2C5680;
    y ^= (y << 15) & 0xEFC60000;
    y ^= y >> 18;
    return y;
}

}  // namespace nyan::cs2
