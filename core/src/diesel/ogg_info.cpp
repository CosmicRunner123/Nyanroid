#include "nyan/diesel/ogg_info.hpp"

#include <cstring>

namespace nyan::diesel {

std::optional<OggInfo> readOggInfo(ByteSpan b) {
    OggInfo info;
    bool haveHeader = false;
    size_t o = 0;
    while (o + 27 <= b.size() && std::memcmp(b.data() + o, "OggS", 4) == 0) {
        int64_t granule = static_cast<int64_t>(readLE32(b.data() + o + 6)) |
                          (static_cast<int64_t>(readLE32(b.data() + o + 10)) << 32);
        size_t segments = b[o + 26];
        if (o + 27 + segments > b.size()) break;
        size_t body = o + 27 + segments, size = 0;
        for (size_t i = 0; i < segments; ++i) size += b[o + 27 + i];
        if (!haveHeader && body + 16 <= b.size() && std::memcmp(b.data() + body, "\x01vorbis", 7) == 0) {
            info.channels = b[body + 11];
            info.sampleRate = readLE32(b.data() + body + 12);
            haveHeader = true;
        }
        if (granule >= 0) info.samples = granule;
        o = body + size;
    }
    if (!haveHeader || info.sampleRate == 0) return std::nullopt;
    return info;
}

}  // namespace nyan::diesel
