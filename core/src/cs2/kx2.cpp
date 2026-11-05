#include "nyan/cs2/kx2.hpp"

#include <cstring>

namespace nyan::cs2 {
namespace {

float readF32(const uint8_t* p) {
    uint32_t u = readLE32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

}  // namespace

std::optional<Kx2Mesh> Kx2Mesh::parse(const Bytes& d) {
    constexpr size_t kHeader = 0x18, kVertex = 16;
    if (d.size() < kHeader || std::memcmp(d.data(), "KX2", 3) != 0) return std::nullopt;
    Kx2Mesh m;
    m.x0 = readF32(&d[4]);
    m.y0 = readF32(&d[8]);
    m.x1 = readF32(&d[12]);
    m.y1 = readF32(&d[16]);
    uint32_t triangles = readLE32(&d[20]);
    if (kHeader + static_cast<size_t>(triangles) * 3 * kVertex > d.size()) return std::nullopt;
    m.vertices.reserve(static_cast<size_t>(triangles) * 3);
    for (size_t i = 0; i < static_cast<size_t>(triangles) * 3; ++i) {
        const uint8_t* v = &d[kHeader + i * kVertex];
        m.vertices.push_back({readF32(v), readF32(v + 4), readLE32(v + 12)});
    }
    return m;
}

}  // namespace nyan::cs2
