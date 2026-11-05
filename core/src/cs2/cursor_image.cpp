#include "nyan/cs2/cursor_image.hpp"

#include <cstdlib>
#include <cstring>

#include "stb_image.h"

namespace nyan::cs2 {
namespace {

uint16_t u16(const Bytes& d, size_t o) { return static_cast<uint16_t>(d[o] | d[o + 1] << 8); }
uint32_t u32(const Bytes& d, size_t o) { return static_cast<uint32_t>(d[o] | d[o + 1] << 8 | d[o + 2] << 16) | static_cast<uint32_t>(d[o + 3]) << 24; }

std::optional<CursorImage> decodePng(const Bytes& d, size_t off, size_t size) {
    int w = 0, h = 0, comp = 0;
    uint8_t* px = stbi_load_from_memory(d.data() + off, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) return std::nullopt;
    CursorImage img;
    img.width = w, img.height = h;
    img.argb.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < img.argb.size(); ++i) {
        const uint8_t* p = px + i * 4;
        img.argb[i] = static_cast<uint32_t>(p[3]) << 24 | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
    }
    stbi_image_free(px);
    return img;
}

// A DIB as icon files hold it: BITMAPINFOHEADER (height counting both bitmaps), the palette, the
// colour bitmap, then the 1-bit AND mask; rows bottom-up, each padded to 4 bytes. A mask bit of 1
// is see-through (where the colour is not black Windows inverts the screen there; drawn as
// see-through). A 32-bit bitmap carries its own alpha unless every alpha byte is 0.
std::optional<CursorImage> decodeDib(const Bytes& d, size_t off, size_t size) {
    if (size < 40 || off + size > d.size()) return std::nullopt;
    const size_t end = off + size;
    const uint32_t header = u32(d, off);
    const int32_t w = static_cast<int32_t>(u32(d, off + 4));
    const int32_t h2 = static_cast<int32_t>(u32(d, off + 8));
    const uint16_t bpp = u16(d, off + 14);
    const uint32_t compression = u32(d, off + 16);
    uint32_t colours = u32(d, off + 32);
    if (header < 40 || w <= 0 || w > 1024 || h2 <= 0 || compression != 0) return std::nullopt;
    const int32_t h = h2 / 2;
    if (h <= 0 || h > 1024) return std::nullopt;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return std::nullopt;
    if (bpp <= 8 && colours == 0) colours = 1u << bpp;
    size_t p = off + header;
    std::vector<uint32_t> palette;
    if (bpp <= 8) {
        if (p + colours * 4 > end) return std::nullopt;
        for (uint32_t i = 0; i < colours; ++i, p += 4) palette.push_back(0xFF000000u | static_cast<uint32_t>(d[p + 2]) << 16 | d[p + 1] << 8 | d[p]);
    }
    const size_t stride = (static_cast<size_t>(w) * bpp + 31) / 32 * 4;
    const size_t maskStride = (static_cast<size_t>(w) + 31) / 32 * 4;
    const size_t colourAt = p, maskAt = p + stride * static_cast<size_t>(h);
    if (maskAt > end) return std::nullopt;
    const bool haveMask = maskAt + maskStride * static_cast<size_t>(h) <= end;
    CursorImage img;
    img.width = w, img.height = h;
    img.argb.resize(static_cast<size_t>(w) * h);
    bool anyAlpha = false;
    for (int32_t y = 0; y < h; ++y) {
        const size_t row = colourAt + stride * static_cast<size_t>(h - 1 - y);
        for (int32_t x = 0; x < w; ++x) {
            uint32_t c = 0;
            switch (bpp) {
            case 32: {
                const uint8_t* q = &d[row + static_cast<size_t>(x) * 4];
                c = static_cast<uint32_t>(q[3]) << 24 | static_cast<uint32_t>(q[2]) << 16 | q[1] << 8 | q[0];
                anyAlpha = anyAlpha || q[3] != 0;
                break;
            }
            case 24: {
                const uint8_t* q = &d[row + static_cast<size_t>(x) * 3];
                c = 0xFF000000u | static_cast<uint32_t>(q[2]) << 16 | q[1] << 8 | q[0];
                break;
            }
            default: {
                const size_t bit = static_cast<size_t>(x) * bpp;
                const uint8_t byte = d[row + bit / 8];
                const uint32_t index = (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                c = index < palette.size() ? palette[index] : 0xFF000000u;
                break;
            }
            }
            img.argb[static_cast<size_t>(y) * w + x] = c;
        }
    }
    if (bpp == 32 && anyAlpha) return img;
    for (int32_t y = 0; y < h; ++y) {
        const size_t row = maskAt + maskStride * static_cast<size_t>(h - 1 - y);
        for (int32_t x = 0; x < w; ++x) {
            uint32_t& c = img.argb[static_cast<size_t>(y) * w + x];
            const bool clear = haveMask && (d[row + x / 8] >> (7 - x % 8) & 1);
            c = clear ? 0 : (c | 0xFF000000u);
        }
    }
    return img;
}

}  // namespace

std::optional<CursorImage> decodeCursorFile(const Bytes& d) {
    if (d.size() < 6 || u16(d, 0) != 0) return std::nullopt;
    const uint16_t type = u16(d, 2), count = u16(d, 4);
    if ((type != 1 && type != 2) || count == 0 || d.size() < 6 + 16u * count) return std::nullopt;
    // The image Windows would take for a cursor: 32 x 32 if there is one, else the largest.
    int best = -1, bestArea = -1;
    for (int i = 0; i < count; ++i) {
        const size_t e = 6 + 16u * static_cast<size_t>(i);
        const int w = d[e] ? d[e] : 256, h = d[e + 1] ? d[e + 1] : 256;
        const int area = w == 32 && h == 32 ? 1 << 30 : w * h;
        if (area > bestArea) best = i, bestArea = area;
    }
    const size_t e = 6 + 16u * static_cast<size_t>(best);
    const uint32_t size = u32(d, e + 8), off = u32(d, e + 12);
    if (off >= d.size() || size > d.size() - off) return std::nullopt;
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::optional<CursorImage> img = size >= 8 && std::memcmp(&d[off], kPng, 8) == 0 ? decodePng(d, off, size) : decodeDib(d, off, size);
    if (!img) return std::nullopt;
    if (type == 2) img->hotX = u16(d, e + 4), img->hotY = u16(d, e + 6);
    return img;
}

}  // namespace nyan::cs2
