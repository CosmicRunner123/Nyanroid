#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nyan::cs2 {

// The 1999-10-28 variant of MT19937 (seeded with the old Knuth 69069 LCG),
// which is what CatSystem2 links against. Not interchangeable with std::mt19937.
class MersenneTwister {
public:
    explicit MersenneTwister(uint32_t seed);
    uint32_t next();

    // First output for a given seed; this is all KIF decryption needs.
    static uint32_t first(uint32_t seed) { return MersenneTwister(seed).next(); }

private:
    void twist();

    std::array<uint32_t, 624> state_{};
    std::size_t index_ = 624;
};

}  // namespace nyan::cs2
