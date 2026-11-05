#include "nyan/wa2/lzss.hpp"

#include <algorithm>
#include <array>

namespace nyan::wa2 {

namespace {

constexpr uint32_t kRingSize = 0x1000;
constexpr uint32_t kStart = 0xfee;  // where writing starts; the engine fills the ring below it with spaces

}  // namespace

LzssResult lzssDecompress(ByteSpan packed, size_t size) {
    LzssResult r;
    r.data.assign(size, 0);
    // Below kStart: spaces, as the engine sets them for every file. Above it: the engine's global
    // ring as it is before the first file (zeroed data), with what was written there tracked.
    std::array<uint8_t, kRingSize> ring{};
    ring.fill(0);
    std::fill(ring.begin(), ring.begin() + kStart, uint8_t{0x20});
    std::array<bool, kRingSize - kStart> tailWritten{};
    uint32_t pos = kStart;
    auto put = [&](uint8_t c) {
        // The engine writes one byte past the end before it notices (0x436df0 checks the count
        // before each copy); that byte is dropped here.
        if (r.produced < size) r.data[r.produced] = c;
        ++r.produced;
        ring[pos] = c;
        if (pos >= kStart) tailWritten[pos - kStart] = true;
        pos = (pos + 1) & (kRingSize - 1);
    };

    size_t in = 0;
    uint32_t flags = 0;
    while (r.produced <= size) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            if (in >= packed.size()) break;
            flags = packed[in++] | 0xff00u;
        }
        if (in >= packed.size()) break;
        uint8_t b0 = packed[in++];
        if (flags & 1) {
            put(b0);
            continue;
        }
        if (in >= packed.size()) break;
        uint8_t b1 = packed[in++];
        uint32_t from = b0 | static_cast<uint32_t>(b1 & 0xf0) << 4;
        int length = (b1 & 0x0f) + 3;
        for (int k = 0; k < length && r.produced <= size; ++k) {
            uint32_t p = (from + static_cast<uint32_t>(k)) & (kRingSize - 1);
            if (p >= kStart && !tailWritten[p - kStart]) r.readStaleTail = true;
            put(ring[p]);
        }
    }
    return r;
}

}  // namespace nyan::wa2
