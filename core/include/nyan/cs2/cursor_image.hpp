#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// A mouse cursor picture: ARGB pixels (0xAARRGGBB, rows top-down) and the hotspot, the pixel that
// points.
struct CursorImage {
    int width = 0, height = 0;
    std::vector<uint32_t> argb;
    int hotX = 0, hotY = 0;
};

// A Windows .cur / .ico file (what cs2.exe hands to LoadCursorFromFileW): its largest image, as a
// DIB of 1, 4, 8, 24 or 32 bits with its AND mask, or as PNG. A .cur's hotspot is its own; an
// .ico's is (0, 0). Nothing for a file it cannot read (an animated .ani among them).
std::optional<CursorImage> decodeCursorFile(const Bytes& data);

}  // namespace nyan::cs2
