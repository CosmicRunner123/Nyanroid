#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/render/gl.hpp"
#include "stb_truetype.h"

namespace nyan::render {

struct Glyph {
    float u0, v0, u1, v1;   // atlas UVs
    float x0, y0, x1, y1;   // quad relative to pen position on the baseline
    float advance;
};

// Glyph cache backed by a single-channel GL texture, rasterised on demand with stb_truetype.
class FontAtlas {
public:
    // `fakeBold`: bold asked of a face that is not bold, which Windows draws bolder itself (see
    // rasterize).
    explicit FontAtlas(Bytes ttf, int face = 0, bool fakeBold = false);
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    bool valid() const { return valid_; }
    const Glyph& glyph(uint32_t codepoint, int pixelSize);
    // The engine's text edge for the glyph (cs2.exe / grisaia.exe kcFontImage, startup.xml
    // FONT/shadow): the glyph's alpha padded by 2 pixels; `around` sums it over 3 pixels across and
    // then 3 down, each capped at 255 (0x6a3ee0); `rightdown` then adds the glyph and that edge
    // again 1 pixel right and down (0x6a40c0, the font's default offset 1, 1). Drawn in the edge
    // colour under the glyph. The quad is relative to the pen like the glyph's.
    const Glyph& edge(uint32_t codepoint, int pixelSize, bool around, bool rightdown);
    // Bumped whenever the atlas is cleared (glyphs and edges looked up before it are gone).
    uint32_t generation() const { return generation_; }
    float ascent(int pixelSize) const { return ascent_ * scale(pixelSize); }
    float lineHeight(int pixelSize) const { return (ascent_ - descent_ + lineGap_) * scale(pixelSize); }
    float kern(uint32_t a, uint32_t b, int pixelSize) const;
    GLuint texture() const { return texture_; }
    const Bytes& data() const { return data_; }
    int face() const { return face_; }
    // Uploads glyphs rasterised since the last call.
    void flush();

private:
    float scale(int px) const { return stbtt_ScaleForMappingEmToPixels(&info_, static_cast<float>(px)); }
    // Room for a w x h bitmap in the atlas (clearing it when full); false if it can never fit.
    bool reserve(int w, int h, int& x, int& y);
    // The glyph's coverage, `stride` bytes a row, and its box relative to the pen on the baseline
    // (x0, y0 the top left); emboldened when fakeBold_. False if it has no pixels.
    bool rasterize(uint32_t cp, int px, std::vector<uint8_t>& out, int& x0, int& y0, int& w, int& h) const;

    Bytes data_;
    int face_ = 0;
    bool fakeBold_ = false;
    stbtt_fontinfo info_{};
    bool valid_ = false;
    int ascent_ = 0, descent_ = 0, lineGap_ = 0;

    static constexpr int kSize = 2048;
    std::vector<uint8_t> pixels_;
    int penX_ = 1, penY_ = 1, rowHeight_ = 0;
    bool dirty_ = false;
    GLuint texture_ = 0;
    std::unordered_map<uint64_t, Glyph> glyphs_;
    std::unordered_map<uint64_t, Glyph> edges_;
    uint32_t generation_ = 0;
};

// Decodes the next UTF-8 code point from `s` at `i` (advancing `i`).
uint32_t nextCodepoint(const std::string& s, size_t& i);

}  // namespace nyan::render
