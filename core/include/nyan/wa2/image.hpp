#pragma once

#include <string_view>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

// A picture as the engine holds it between reading the file and making textures of it (WA2.exe
// 0x43ffa0): rows top-down, pixels as stored (8-bit indices; B, G, R; or B, G, R, A), and an 8-bit
// picture's palette in the order the engine puts it in for Direct3D (R, G, B, alpha per entry).
struct Picture {
    int width = 0;
    int height = 0;
    int bpp = 0;          // 8, 24 or 32
    Bytes pixels;         // height rows of width * bpp / 8 bytes, top row first
    Bytes palette;        // 8-bit: 256 entries of R, G, B, A (entries the file lacks are 0)
    int paletteSize = 0;  // entries the file gives
    bool paletteAlpha = false;  // the palette's own alpha counts (a 32-bit TGA palette)
};

// The engine's picture types (the low byte of an image slot's load flags, 0x43ffa0).
enum class PictureType {
    Opaque = 0,       // an X8R8G8B8 texture: alpha ignored
    Alpha = 1,        // A8R8G8B8 (2 is the same)
    Mask = 3,         // A8: an 8-bit picture's blue as alpha (transition masks)
    KeyedIndex0 = 4,  // A8R8G8B8, an 8-bit picture's palette entry 0 transparent
};

// Reads a TGA or a BMP as the engine does; the name's extension decides ("tga" in either case is a
// TGA, anything else a BMP). TGA (0x4500d0): types other than the run-length ones (9, 10, which the
// engine refuses), the image-ID and colour-map start fields ignored, a 24-bit palette opaque, 32-bit
// pixels or palette with alpha, bottom-up unless descriptor bit 0x20. BMP: the header's fields as
// they are (no compression, rows not padded), bottom-up for a positive height. Throws nyan::Error for
// what the engine cannot read.
Picture decodePicture(ByteSpan data, std::string_view name);

// A picture's size and depth from the start of its file (the first 64 bytes cover both headers); false
// where decodePicture would refuse the header.
bool pictureHeader(ByteSpan head, std::string_view name, int& width, int& height, int& bpp);

// The palette's alpha as an image slot of `type` sets it before making the texture (0x43ffa0): 0
// for entry 0 of KeyedIndex0, else opaque unless the palette's own alpha counts.
void preparePalette(Picture& picture, PictureType type);

// The slot's colour level (0x438610 / 0x4387f0): 0x100 leaves the picture, 0 makes it grey
// ((77 R + 151 G + 28 B) >> 8 by byte position), 1..255 mixes: c * t / 256 + grey * (255 - t) / 256.
// An 8-bit picture's palette is changed in its R, G, B order, so its red and blue weights swap (as
// the engine's).
void applyLevel(Picture& picture, int level);

// A colour table (.amp, 0x4388f0 / 0x438ed0 / 0x439060) of 256 bytes (one curve for every channel),
// 768 (curves 0, 1, 2) or 1280 (curves 1, 2, 3 then curve 0). Byte positions 2, 1, 0 of each pixel or
// palette entry get curves 0, 1, 2 (resp. 0 after 1, 2, 3), so an 8-bit picture's red and blue swap
// as in the engine. False if the table has none of these sizes (the engine logs it and skips it).
bool applyAmp(Picture& picture, ByteSpan table);

struct RgbaImage {
    int width = 0;
    int height = 0;
    Bytes pixels;  // R, G, B, A, top row first
};

// The texture's contents for a slot of `type`: R, G, B, A. Mask: an 8-bit picture's palette blue as
// alpha and no colour (Direct3D's A8 has none); other depths give an empty image, as the engine loads
// nothing for them.
RgbaImage toRgba(const Picture& picture, PictureType type);

}  // namespace nyan::wa2
