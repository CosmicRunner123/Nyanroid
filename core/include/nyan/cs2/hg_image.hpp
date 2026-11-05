#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// Decoded pixels: tightly packed RGBA8, top-down rows, straight (non-premultiplied) alpha.
struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    Bytes pixels;
};

// Named point/rect attached to an HG-3 frame (tag "ats####"), e.g. where a manpu sprite goes.
struct HgAttribute {
    uint32_t id;
    int32_t x, y;
    uint32_t width, height;
    uint32_t color;  // 0xAARRGGBB
};

enum class HgEncoding { Standard, Jpeg, JpegWithAlpha, WebP, Unsupported };

struct HgFrame {
    uint32_t id = 0;
    // Condensed image (transparent borders cropped away).
    uint32_t width = 0, height = 0;
    uint32_t bitDepth = 32;
    // Placement of the condensed image inside the full canvas.
    int32_t offsetX = 0, offsetY = 0;
    uint32_t totalWidth = 0, totalHeight = 0;
    bool transparent = false;
    // Anchor point used when drawing in-game.
    int32_t baseX = 0, baseY = 0;
    std::vector<HgAttribute> attributes;

    HgEncoding encoding = HgEncoding::Unsupported;
    std::string unsupportedTag;  // set when encoding == Unsupported

    // Location of the encoded pixel data inside the file (internal use).
    struct Standard {
        size_t dataOffset = 0;
        uint32_t compressedData = 0, decompressedData = 0;
        uint32_t compressedCmd = 0, decompressedCmd = 0;
    } standard;
    size_t blobOffset = 0, blobLength = 0;           // img_jpg / img_wbp payload
    size_t alphaOffset = 0;                          // img_al payload
    uint32_t alphaCompressed = 0, alphaDecompressed = 0;
};

// An HG-2 or HG-3 image file. Parsing only reads frame headers; pixels are
// decoded on demand per frame.
class HgImage {
public:
    static HgImage parse(Bytes data);

    int version() const { return version_; }  // 2 or 3
    const std::vector<HgFrame>& frames() const { return frames_; }

    // Decodes a frame's condensed image (width x height).
    RgbaImage decode(size_t frameIndex) const;
    // Decodes a frame and places it on its full transparent canvas (totalWidth x totalHeight).
    RgbaImage decodeFull(size_t frameIndex) const;

private:
    static void parseHg3(HgImage& img, ByteReader& r);
    static void parseHg2(HgImage& img, ByteReader& r, uint32_t type);

    Bytes data_;
    int version_ = 0;
    std::vector<HgFrame> frames_;
};

// Core of the standard HG encoding: zero-run expansion + weighted-table/delta
// reconstruction. Returns BGR(A) rows, bottom-up, each padded to a 4-byte stride.
Bytes hgProcessImage(ByteSpan rleData, ByteSpan rleCmd, uint32_t width, uint32_t height, uint32_t depthBytes);

}  // namespace nyan::cs2
