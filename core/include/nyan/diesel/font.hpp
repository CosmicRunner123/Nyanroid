#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// One face of a TrueType / OpenType font file (a .ttc holds several), as Windows sees it once
// Mware's AddFontMemResourceEx (Mware.dll 0x1004fd20) has loaded the file: found by its family
// names.
class FontFace {
public:
    // Every face in the file; empty if it is not a font.
    static std::vector<std::shared_ptr<FontFace>> load(std::shared_ptr<const Bytes> data);
    ~FontFace();

    // The family names it answers to (name IDs 1 and 16 in every language, and 4), UTF-8.
    const std::vector<std::string>& names() const { return names_; }
    bool answersTo(const std::string& family) const;

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    FontFace() = default;
    std::shared_ptr<const Bytes> data_;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> names_;
};

// A glyph as Mware's CFontRendererGDI keeps it (0x100501e0, GetGlyphOutlineW with GGO_GRAY8_BITMAP):
// the bitmap's place in the character cell and the cell's width.
struct GlyphImage {
    uint32_t codepoint = 0;
    int left = 0;      // [1]: the bitmap's left edge in the cell (a negative origin moves to `overhang`)
    int top = 0;       // [2]: tmAscent - the origin's y: the bitmap's top below the cell's top
    int pitch = 0;     // [3]: the bitmap's width rounded up to 4
    int rows = 0;      // [4]
    int cellWidth = 0;  // [7]: the cell, wide enough for the bitmap
    int cellHeight = 0; // [8]: tmHeight
    int adjust = 0;    // [9]: brings the pen back to the advance when the cell was widened (<= 0)
    int overhang = 0;  // [10]: the origin's x when negative
    int advance = 0;   // gmCellIncX
    // Coverage per pixel, rows top first, `pitch` bytes a row: GDI's 65 levels spread to 0..255 as
    // the font's maker draws them into its glyph atlas (FontInstance's `alphaBias`).
    std::vector<uint8_t> alpha;
    // GGO_GRAY8_BITMAP's levels themselves (0..64, same layout; empty without antialiasing): the
    // blit with an alpha bias (blitGlyph) needs them, 63 and 64 both spreading to 255 above.
    std::vector<uint8_t> levels;
};

// A font as CreateFontW(-size, 0, 0, 0, weight, ...) makes it of a face (0x1004ff00): TEXTMETRIC's
// values and glyphs on demand.
class FontInstance {
public:
    // `vertical`: Mware's vertical font (0x1004ff00 with its fifth argument set): CreateFontW of "@"
    // and the face, whose glyphs lie for a column (see glyph()), its cell turned. `alphaBias`: how
    // the glyphs' `alpha` comes about: -1 as You and Me and Her's own GDI code makes it (totono_en.exe
    // 0x4845xx: v / 63 as a float, 1 at most, x 255 truncated; no bias), else as Mware's blit draws
    // the glyph in white with that bias (CSystemFont's atlas, 0x4a3d40 -> 0x10050430: see blitGlyph;
    // a pixel without antialiasing 254).
    FontInstance(std::shared_ptr<FontFace> face, int size, int weight, bool noAntialias, bool vertical = false,
                 int alphaBias = -1);

    int size() const { return size_; }
    int weight() const { return weight_; }
    int height() const { return ascent_ + descent_; }  // tmHeight
    int ascent() const { return ascent_; }             // tmAscent
    int descent() const { return descent_; }           // tmDescent
    int maxCharWidth() const { return maxCharWidth_; } // tmMaxCharWidth
    // The cell size Mware's font reports (CSystemFont vtable +0x20: tmMaxCharWidth * 135 / 100, and
    // tmHeight).
    int cellWidth() const { return maxCharWidth_ * 135 / 100; }
    bool vertical() const { return vertical_; }
    // CFontRendererGDI vtable +0xc (0x100501c0): the cell as the font reports it (+0x4c / +0x50):
    // (cellWidth(), tmHeight), turned for a vertical font (tmHeight, cellWidth()).
    std::array<int, 2> cell() const {
        return vertical_ ? std::array<int, 2>{height(), cellWidth()} : std::array<int, 2>{cellWidth(), height()};
    }
    // The glyph for a UTF-16 / UTF-32 code point (a missing one: the face's .notdef; a space or an
    // ideographic space: no bitmap), upright or italic. CSystemFont keeps a second GDI font made with
    // bItalic set; a face without an italic gets GDI's simulated slant (Guess: a shear of tan 12
    // degrees about the baseline, the advance kept).
    // A vertical font (0x100501e0's vertical branch): the cell's width is tmHeight and its height
    // the advance (widened for the bitmap as the width is in a horizontal font); a character that
    // stands upright in a column comes turned a quarter to the left, its em box where an upright
    // one's is and its advance tmHeight (Fitted: GDI's "@" faces; which characters turn is a Guess,
    // East Asian wide and full-width ones, and the faces' vertical alternates are not looked up),
    // the rest as in horizontal text (lying in the column once the blit turns them).
    std::shared_ptr<const GlyphImage> glyph(uint32_t codepoint, bool italic = false);
    // Kerning between two characters, pixels (rounded).
    int kerning(uint32_t left, uint32_t right) const;

private:
    std::shared_ptr<FontFace> face_;
    int size_, weight_;
    bool noAntialias_, vertical_;
    int alphaBias_;
    float scale_ = 0.0f;
    int ascent_ = 0, descent_ = 0, maxCharWidth_ = 0;
    int unitsAscent_ = 0, unitsDescent_ = 0;  // the Windows ascent and descent in font units
    std::unordered_map<uint64_t, std::shared_ptr<const GlyphImage>> glyphs_;
};

// Mware's CFontRendererGDI blit (0x10050430): a glyph whose cell's top-left is at (x, y) into a
// 32-bit picture `rowPixels` texels a row and `height` rows high, in `color` (the texel as a
// little-endian word: alpha in the top byte). Each pixel's alpha is GDI's level v spread with the
// font's alpha bias b ((v (255 - b) / 63 + b) x the colour's alpha / 256, 255 at most; 0 where v is
// 0), or for a glyph without antialiasing the colour's alpha x 255 / 256. Only the cell is drawn,
// clipped to the picture; a vertical font's glyph turns a quarter to the right on the way (the
// cell's row r lands in column x + cell width - 1 - r). A texel whose alpha is 0 takes the glyph's
// texel as it is; another is mixed (each channel (d (255 - a) + s a) x 257 rounded / 65535, the
// alpha (d (255 - a) + a a) likewise).
void blitGlyph(const GlyphImage& g, uint32_t* bits, int x, int y, int rowPixels, int height, uint32_t color,
               int alphaBias, bool vertical);

}  // namespace nyan::diesel
