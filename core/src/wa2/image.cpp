#include "nyan/wa2/image.hpp"

#include <algorithm>
#include <cstdlib>

#include "nyan/wa2/kcap_archive.hpp"

namespace nyan::wa2 {

namespace {

constexpr size_t kTgaHeader = 18;
constexpr size_t kBmpHeaders = 0x36;  // file header and BITMAPINFOHEADER

bool isTgaName(std::string_view name) {
    auto dot = name.rfind('.');
    return dot != std::string_view::npos && sameName(name.substr(dot + 1), "tga");
}

// Copies height rows of `rowBytes` from `src`, reversing their order when the file stores the
// bottom row first.
Bytes readRows(ByteSpan data, size_t at, size_t rowBytes, int height, bool bottomUp, const char* what) {
    size_t total = rowBytes * static_cast<size_t>(height);
    if (at > data.size() || data.size() - at < total) fail(std::string(what) + ": pixel data shorter than the picture");
    Bytes out(total);
    for (int y = 0; y < height; ++y) {
        size_t from = at + rowBytes * static_cast<size_t>(bottomUp ? height - 1 - y : y);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(from), rowBytes, out.begin() + static_cast<std::ptrdiff_t>(rowBytes * y));
    }
    return out;
}

Picture decodeTga(ByteSpan d) {
    if (d.size() < kTgaHeader) fail("TGA: shorter than its header");
    uint8_t type = d[2];
    // 0x4500d0 refuses run-length pictures.
    if (type == 9 || type == 10) fail("TGA: run-length encoded (type " + std::to_string(type) + "), which the engine does not read");
    Picture p;
    p.width = readLE16(&d[12]);
    p.height = readLE16(&d[14]);
    p.bpp = d[16];
    bool bottomUp = (d[17] & 0x20) == 0;
    size_t at = kTgaHeader;  // the image-ID field is not skipped
    if (p.bpp == 8) {
        p.paletteSize = readLE16(&d[5]);
        int entryBits = d[7];
        size_t entry = entryBits == 24 ? 3 : 4;  // anything but 24 is read as 32-bit entries
        if (d.size() - at < entry * p.paletteSize) fail("TGA: palette shorter than its size");
        p.palette.assign(256 * 4, 0);
        for (int i = 0; i < p.paletteSize && i < 256; ++i) {
            const uint8_t* e = &d[at + entry * i];
            // Stored B, G, R[, A]; the engine puts entries in R, G, B, A order for Direct3D.
            p.palette[i * 4 + 0] = e[2];
            p.palette[i * 4 + 1] = e[1];
            p.palette[i * 4 + 2] = e[0];
            p.palette[i * 4 + 3] = entry == 3 ? 0xff : e[3];
        }
        p.paletteAlpha = entry == 4;
        at += entry * p.paletteSize;
    } else if (p.bpp != 24 && p.bpp != 32) {
        fail("TGA: " + std::to_string(p.bpp) + "-bit pixels");
    }
    p.pixels = readRows(d, at, static_cast<size_t>(p.width) * p.bpp / 8, p.height, bottomUp, "TGA");
    return p;
}

Picture decodeBmp(ByteSpan d) {
    if (d.size() < kBmpHeaders || d[0] != 'B' || d[1] != 'M') fail("BMP: no BM header");
    uint32_t offBits = readLE32(&d[10]);
    auto height = static_cast<int32_t>(readLE32(&d[0x16]));
    Picture p;
    p.width = static_cast<int32_t>(readLE32(&d[0x12]));
    p.height = std::abs(height);
    p.bpp = readLE16(&d[0x1c]);
    if (offBits < kBmpHeaders || offBits > d.size()) fail("BMP: pixel offset outside the file");
    if (p.bpp == 8) {
        // The palette is what lies between the headers and the pixels (0x43ffa0).
        p.paletteSize = static_cast<int>((offBits - kBmpHeaders) / 4);
        p.palette.assign(256 * 4, 0);
        for (int i = 0; i < p.paletteSize && i < 256; ++i) {
            const uint8_t* e = &d[kBmpHeaders + 4 * i];
            p.palette[i * 4 + 0] = e[2];
            p.palette[i * 4 + 1] = e[1];
            p.palette[i * 4 + 2] = e[0];
            p.palette[i * 4 + 3] = e[3];
        }
    } else if (p.bpp != 24 && p.bpp != 32) {
        fail("BMP: " + std::to_string(p.bpp) + "-bit pixels");
    }
    if (p.width <= 0) fail("BMP: no width");
    p.pixels = readRows(d, offBits, static_cast<size_t>(p.width) * p.bpp / 8, p.height, height > 0, "BMP");
    return p;
}

// The bytes the colour passes change: an 8-bit picture's palette entries, else its pixels.
struct Channels {
    uint8_t* data;
    size_t count;
    size_t stride;
};

Channels channels(Picture& p) {
    if (p.bpp == 8) return {p.palette.data(), static_cast<size_t>(std::clamp(p.paletteSize, 0, 256)), 4};
    size_t stride = static_cast<size_t>(p.bpp / 8);
    return {p.pixels.data(), p.pixels.size() / stride, stride};
}

}  // namespace

bool pictureHeader(ByteSpan d, std::string_view name, int& width, int& height, int& bpp) {
    if (isTgaName(name)) {
        if (d.size() < kTgaHeader || d[2] == 9 || d[2] == 10) return false;
        width = readLE16(&d[12]);
        height = readLE16(&d[14]);
        bpp = d[16];
    } else {
        if (d.size() < kBmpHeaders || d[0] != 'B' || d[1] != 'M') return false;
        width = static_cast<int32_t>(readLE32(&d[0x12]));
        height = std::abs(static_cast<int32_t>(readLE32(&d[0x16])));
        bpp = readLE16(&d[0x1c]);
        if (width <= 0) return false;
    }
    return bpp == 8 || bpp == 24 || bpp == 32;
}

Picture decodePicture(ByteSpan data, std::string_view name) {
    return isTgaName(name) ? decodeTga(data) : decodeBmp(data);
}

void preparePalette(Picture& p, PictureType type) {
    if (p.bpp != 8 || type == PictureType::Mask) return;
    for (int i = 0; i < std::min(p.paletteSize, 256); ++i) {
        uint8_t& a = p.palette[i * 4 + 3];
        if (type == PictureType::KeyedIndex0 && i == 0) a = 0;
        else if (!p.paletteAlpha) a = 0xff;
    }
}

void applyLevel(Picture& p, int level) {
    if (level == 0x100) return;
    Channels c = channels(p);
    for (size_t i = 0; i < c.count; ++i) {
        uint8_t* px = c.data + i * c.stride;
        int grey = (px[2] * 0x4d + px[0] * 0x1c + px[1] * 0x97) >> 8;
        if (level == 0) {
            px[0] = px[1] = px[2] = static_cast<uint8_t>(grey);
            continue;
        }
        // The engine's tables (0x4385b0): row t holds min(c * t >> 8, 255).
        auto scale = [](int v, int t) { return std::min((v * t) >> 8, 255); };
        int g = scale(grey, 255 - level);
        for (int k = 0; k < 3; ++k) px[k] = static_cast<uint8_t>(scale(px[k], level) + g);
    }
}

bool applyAmp(Picture& p, ByteSpan t) {
    if (t.size() != 0x100 && t.size() != 0x300 && t.size() != 0x500) return false;
    Channels c = channels(p);
    for (size_t i = 0; i < c.count; ++i) {
        uint8_t* px = c.data + i * c.stride;
        if (t.size() == 0x100) {
            for (int k = 0; k < 3; ++k) px[k] = t[px[k]];
        } else if (t.size() == 0x300) {
            px[2] = t[px[2]];
            px[1] = t[0x100 + px[1]];
            px[0] = t[0x200 + px[0]];
        } else {
            px[2] = t[t[0x100 + px[2]]];
            px[1] = t[t[0x200 + px[1]]];
            px[0] = t[t[0x300 + px[0]]];
        }
    }
    return true;
}

RgbaImage toRgba(const Picture& p, PictureType type) {
    RgbaImage out;
    if (type == PictureType::Mask && p.bpp != 8) return out;
    out.width = p.width;
    out.height = p.height;
    size_t n = static_cast<size_t>(p.width) * p.height;
    out.pixels.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = &out.pixels[i * 4];
        if (p.bpp == 8) {
            const uint8_t* e = &p.palette[p.pixels[i] * 4];
            if (type == PictureType::Mask) {
                o[0] = o[1] = o[2] = 0;
                o[3] = e[2];  // the file's blue: the first byte of its entry
                continue;
            }
            o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = e[3];
        } else {
            const uint8_t* s = &p.pixels[i * (p.bpp / 8)];
            o[0] = s[2], o[1] = s[1], o[2] = s[0];
            o[3] = p.bpp == 32 ? s[3] : 0xff;
        }
        if (type == PictureType::Opaque) o[3] = 0xff;
    }
    return out;
}

}  // namespace nyan::wa2
