#include "nyan/diesel/asf_info.hpp"

#include <cstdlib>
#include <cstring>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

namespace {

// GUIDs as the file stores them (the first three fields little-endian).
constexpr uint8_t kHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kFileProperties[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                         0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kStreamProperties[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                           0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kHeaderExtension[16] = {0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11,
                                          0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kExtendedStreamProperties[16] = {0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6, 0x32, 0x43,
                                                   0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A};
constexpr uint8_t kVideoMedia[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                     0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kAudioMedia[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                     0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint64_t kMaxHeader = 64u << 20;

uint64_t readLE64(const uint8_t* p) { return readLE32(p) | (static_cast<uint64_t>(readLE32(p + 4)) << 32); }

bool is(const uint8_t* p, const uint8_t (&guid)[16]) { return std::memcmp(p, guid, 16) == 0; }

void streamProperties(const uint8_t* p, size_t n, AsfInfo& out) {
    // Stream type, error correction type, time offset (8), type-specific length (4), error
    // correction length (4), flags (2), reserved (4), then the type-specific data.
    if (n < 54) return;
    uint32_t typeLen = readLE32(p + 40);
    const uint8_t* t = p + 54;
    if (typeLen > n - 54) return;
    if (is(p, kAudioMedia)) {
        out.hasAudio = true;
    } else if (is(p, kVideoMedia) && out.width == 0 && typeLen >= 11) {
        // Encoded width (4), height (4), reserved (1), format data size (2), BITMAPINFOHEADER.
        out.width = static_cast<int>(readLE32(t));
        out.height = static_cast<int>(readLE32(t + 4));
        if (typeLen >= 11 + 20) {
            const uint8_t* bmi = t + 11;
            int32_t w = static_cast<int32_t>(readLE32(bmi + 4)), h = static_cast<int32_t>(readLE32(bmi + 8));
            if (w > 0 && h != 0) out.width = w, out.height = std::abs(h);
            out.videoCodec = readLE32(bmi + 16);
        }
    }
}

void objects(const uint8_t* p, size_t n, AsfInfo& out, int depth) {
    size_t at = 0;
    while (n - at >= 24) {
        const uint8_t* o = p + at;
        uint64_t size = readLE64(o + 16);
        if (size < 24 || size > n - at) break;
        const uint8_t* body = o + 24;
        size_t len = static_cast<size_t>(size - 24);
        if (is(o, kFileProperties) && len >= 80) {
            // File id (16), file size (8), creation date (8), packets (8), play duration (8), send
            // duration (8), preroll (8, ms), ...
            out.playDuration = readLE64(body + 40);
            out.prerollMs = readLE64(body + 56);
        } else if (is(o, kStreamProperties)) {
            streamProperties(body, len, out);
        } else if (is(o, kHeaderExtension) && depth == 0 && len >= 22) {
            // Reserved GUID (16), reserved (2), data size (4), then objects.
            size_t data = readLE32(body + 18);
            if (data <= len - 22) objects(body + 22, data, out, depth + 1);
        } else if (is(o, kExtendedStreamProperties) && len >= 64) {
            // The fixed part (64 bytes), stream names, payload extension systems, then perhaps a
            // Stream Properties Object of its own.
            size_t q = 64;
            unsigned names = readLE16(body + 60), systems = readLE16(body + 62);
            for (unsigned i = 0; i < names && q + 4 <= len; ++i) q += 4 + readLE16(body + q + 2);
            for (unsigned i = 0; i < systems && q + 22 <= len; ++i) q += 22 + readLE32(body + q + 18);
            if (q + 24 <= len && is(body + q, kStreamProperties)) {
                uint64_t s = readLE64(body + q + 16);
                if (s >= 24 && s <= len - q) streamProperties(body + q + 24, static_cast<size_t>(s - 24), out);
            }
        }
        at += static_cast<size_t>(size);
    }
}

}  // namespace

std::optional<AsfInfo> readAsfInfo(const RandomAccessFile& file) {
    if (file.size() < 30) return std::nullopt;
    uint8_t h[30];
    file.readAt(0, h, sizeof h);
    if (!is(h, kHeader)) return std::nullopt;
    uint64_t size = readLE64(h + 16);
    if (size < 30 || size > file.size() || size > kMaxHeader) return std::nullopt;
    Bytes header = file.readAt(30, static_cast<size_t>(size - 30));
    AsfInfo out;
    objects(header.data(), header.size(), out, 0);
    return out;
}

}  // namespace nyan::diesel
