#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// A KX2 mesh (`kx2.int`): triangles a plane is drawn through, e.g. `pl 1 half_planeL` keeps plane 1
// to the left wedge of a split screen. Layout: "KX2\0", bounding box (x0, y0, x1, y1 floats), the
// triangle count, then three 16-byte vertices per triangle (x, y, z floats, ARGB colour).
// Coordinates are in the plane's own space (where its images are placed).
struct Kx2Mesh {
    struct Vertex {
        float x, y;
        uint32_t color;
    };
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<Vertex> vertices;  // three per triangle

    static std::optional<Kx2Mesh> parse(const Bytes& data);
};

}  // namespace nyan::cs2
