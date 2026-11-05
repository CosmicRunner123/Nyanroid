#pragma once

#include <array>
#include <cstdint>

namespace nyan::diesel {

// Mware's random numbers (Mware.dll CRandom): a Mersenne Twister (MT19937) whose shifts are signed,
// as its code does them (the twist's >> 1, the seeding's >> 30 and the tempering's >> 11 and >> 18
// carry the sign bit), so its numbers differ from the standard generator's whenever a word's top bit
// is set. RandomI / RandomF use one the exes seed with time(0) when the VM is made.
class MwareRandom {
public:
    explicit MwareRandom(int32_t seed = 0) { this->seed(seed); }

    void seed(int32_t seed);           // 0x1003d7c0
    int32_t next();                    // GenerateInt32, 0x1003d6b0
    // An integer in [low, high]: (next() >> 1) % (high - low + 1) + low, the remainder taking the
    // dividend's sign as idiv's does (0x1003d880).
    int32_t nextInt(int32_t low, int32_t high);
    // A float in [low, high]: (next() % 100000001) * 1e-8 * (high - low) + low (0x1003d8a0).
    float nextFloat(float low, float high);

    // The generator's state as Mware's CRandom hands it over (GetMersenneTwisterCount / ...Size /
    // GetMersenneTwister: the index, then the 624 words).
    static constexpr int kN = 624;
    int index() const { return index_; }
    const std::array<int32_t, kN>& state() const { return mt_; }
    void restore(const std::array<int32_t, kN>& state, int index) { mt_ = state, index_ = index; }

private:
    static constexpr int kM = 397;
    std::array<int32_t, kN> mt_{};
    int index_ = kN;
};

}  // namespace nyan::diesel
