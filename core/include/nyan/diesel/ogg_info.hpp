#pragma once

#include <cstdint>
#include <optional>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// An Ogg Vorbis file's sample rate and length (the identification header's rate, the last page's
// granule position), without decoding: what the headless host needs for SQSound's times.
struct OggInfo {
    uint32_t sampleRate = 0;
    int64_t samples = 0;
    uint32_t channels = 0;
    // Length in milliseconds (rounded down).
    uint32_t milliseconds() const {
        return sampleRate ? static_cast<uint32_t>(samples * 1000 / sampleRate) : 0;
    }
};

// nullopt if the data is not an Ogg Vorbis stream.
std::optional<OggInfo> readOggInfo(ByteSpan data);

}  // namespace nyan::diesel
