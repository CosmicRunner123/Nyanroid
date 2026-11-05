#pragma once

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// A decoded picture: rows top-down, R, G, B, A per pixel, alpha not premultiplied.
struct Image {
    int width = 0;
    int height = 0;
    Bytes rgba;
};

// The file formats Mware's loaders read, told apart by content, not by name (most of the games'
// ".png" files are WebP): WebP (CWebPLoader; every WebP in both games is lossless VP8L), PNG
// (CPngLoader) and JPEG (CJpegLoader). Throws nyan::Error for anything else or a broken file.
Image decodeImage(ByteSpan data);

// The format's name for reports: "webp", "png", "jpeg" or "" (unknown).
const char* imageFormat(ByteSpan data);

// Size from the header without decoding; false if not a known picture.
bool imageSize(ByteSpan data, int& width, int& height);

}  // namespace nyan::diesel
