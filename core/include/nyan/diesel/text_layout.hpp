#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nyan/diesel/font.hpp"
#include "nyan/diesel/image.hpp"
#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

class Shake;

// A glyph as CSystemFont's vtable +0x1c (0x4a3d10) answers it for the layout: the cell and the
// corrections around it (see GlyphImage).
struct TextGlyph {
    int cellWidth = 0, cellHeight = 0, overhang = 0, adjust = 0;
    std::shared_ptr<const GlyphImage> image;
    int advance() const { return adjust + cellWidth + overhang; }
};

// The fonts a text lays out with (its font and its ruby font, both named in the app's font pool).
class TextFont {
public:
    virtual ~TextFont() = default;
    virtual TextGlyph glyph(uint32_t code, bool italic) = 0;
    virtual int height() const = 0;  // the cell height (CSystemFont vtable +0x20's second value)
    // vtable +0x20's first value: tmMaxCharWidth * 135 / 100.
    virtual int maxWidth() const { return 0; }
};

// A laid-out character (CText's 0x3d4-byte entries, the fields the layout and the draw use). Ruby
// characters are the same entries in a list of their own.
struct TextChar {
    uint32_t code = 0;        // +0x24: a UTF-16 unit
    bool ruby = false;        // +0x38: a ruby character (the ruby font)
    bool italic = false;      // +0x3c
    bool small = false;       // +0x40: SMALL text, in the ruby font
    float x = 0.0f, y = 0.0f; // +0x14 / +0x18: the cell's top-left in the field (x with the overhang)
    int cellWidth = 0, cellHeight = 0;  // +0xc / +0x10
    int adjust = 0, overhang = 0;       // +0x44 / +0x48: the glyph's
    float shown = 0.0f;       // +0x1c: alpha units shown at the last draw (the layout: -48 n)
    float typingStart = 0.0f; // +0x20: added to the typing to give the alpha units
    std::shared_ptr<const GlyphImage> image;
    // The character's own effects (fadeCharacter, moveCharacter, ...: 0x4d7740 ff.), each stepped
    // once set (+0x50 / +0x80 / +0xc0 / +0x100 / +0x150); a relayout keeps them. +0x4c says one was
    // set (the vertex build then makes the character's own matrix).
    Tween fade{1, 1.0f};                   // +0x54
    Tween move{2, 0.0f, 0.0f};             // +0x84
    Tween scale{2, 1.0f, 1.0f};            // +0xc4
    Tween rotate{3, 0.0f, 0.0f, 0.0f};     // +0x104
    std::array<Tween, 4> color{Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f},
                               Tween{3, 1.0f, 1.0f, 1.0f}};  // +0x154
    bool fadeSet = false, moveSet = false, scaleSet = false, rotateSet = false;
    bool colorSet = false;  // +0x150: its own corner colours instead of the text's
    bool effects = false;   // +0x4c
    // shakeCharacter / shakeRuby's shake (+0x294): its offset is added to the character's move.
    std::shared_ptr<Shake> shake;
    // vtable +0x88's offsets of the cell's corners (+0x298 ff.: top-left, top-right, bottom-left,
    // bottom-right; x right, y up), added to them when the corners are worked out (0x4d2cf0).
    std::array<std::array<float, 2>, 4> corner{};
};

// The settings the layout reads (CText +0x124 .. +0x140).
struct TextLayoutSettings {
    int fieldWidth = 0, fieldHeight = 0, fieldType = 0;  // setField
    int pitchFull = 0, pitchHalf = 0;                     // setPitchSpace(a, b): +0x130 / +0x134
    int rubyOffset = 0;                                   // +0x138: setRubyOffset
    float rubyMarginRate = 0.0f;                          // +0x13c: setRubyMarginRate
    int rowSpace = 0;                                     // +0x140
};

// The line-break tables ("NG codes": muramasa_en.exe 0x446160 builds them at start-up, the globals
// ResetNGCode / AddNGCode_HeadOfLine / AddNGCode_EndOfLine change them): characters a line may not
// begin with (they may hang past the field instead), small kana (the same, with a rule for
// repeats; You and Me and Her's exe has none and the simpler rule), and characters a line may not
// end with.
struct LineBreakTables {
    std::vector<uint32_t> headOfLine, smallKana, endOfLine;
    bool hangingRule = true;  // the small-kana rule (Muramasa, Phantom of Inferno)
};

// A kerning pair of addPositionFixRule (CText +0x850: first and second character, pixels).
struct PositionFixRule {
    uint32_t first = 0, second = 0;
    int32_t value = 0;
};

// A CText's layout (vtable +0x80) and the state it keeps between prints, for the three kinds
// initialize(type) makes: 0 CText's Japanese layout (0x4d08c0), 1 CTextEnglish's (0x4ca640), 2
// CTextVertical's (0x4ccb30).
//
// English: text is read through the tokenizer, line by line, its tags I / SMALL / CENTER (and their
// closing forms) switching italic, the ruby font and centring (RUBY is skipped: the English text
// has no ruby). Each UTF-16 unit of text is a character placed in words: a word ends at a space, a
// line break or the token's end; it goes to the next line if the token has had a letter or digit
// and it would cross the field (up to two trailing spaces or . ) " ? ! ' } left out); a word wider
// than the field is cut and its tail laid out again on the next line. Between characters the pitch
// for the next printable one is added (setPitchSpace's second value before a half-width one).
//
// Japanese: character by character, the pitch before each (none at a line's start: +0x4d4), tags
// I / CENTER and RUBY text='...' ... /RUBY, whose text is laid out in the ruby font over the
// characters between. A character that would cross the field starts the next line unless the
// line-break tables let it hang (at most two in a row); one a line may not end with goes to the
// next line when a full-width character would not fit after it.
class TextLayout {
public:
    int type = 1;                 // initialize(type)
    // You and Me and Her's older text code (totono_en.exe; its English layout knows no SMALL): the
    // English layout (0x49e6c0) has no SMALL, needs only the main font, keeps the half-width pitch
    // between characters, wraps a word that does not fit unless the pen is at the line's start and
    // cuts an over-wide word only there; the Japanese layout (0x4a47e0) lets only head-of-line
    // characters hang and spreads ruby row by row.
//
// Vertical: the Japanese layout turned: characters down columns (the pen's y; a column as wide as
// its widest cell, the next one the row space further), ruby beside them; laid out into a list of
// its own, then turned right to left (x = field width - x - cell, each centred on the list's mean
// cell width) and made, characters first, then ruby. The font is the usual one (glyphs upright).
    bool older = false;
    std::vector<TextChar> chars;  // +0x5d4
    std::vector<TextChar> rubies; // +0x5e0
    // The characters in the order they were made (the pool at +0x588 the draw walks): an index in
    // `chars`, or in `rubies` with kRubyBit set.
    static constexpr uint32_t kRubyBit = 0x80000000u;
    std::vector<uint32_t> pool;
    int x = 0, y = 0;             // +0x114 / +0x118: the pen
    int column = 0;               // +0x11c: vertical text's widest cell of the column
    int lineHeight = 0;           // +0x120: the tallest cell of the line so far
    int maxX = 0, totalHeight = 0;  // +0x298 / +0x29c: getSize
    int lines = 1;                // +0x144
    bool lastWide = false;        // +0x4d8: the last character was not Latin-1
    int noPrintPitch = 0;         // +0x4d4: no pitch before the next character (a line's start)
    int hangRun = 0;              // +0x580: small kana in a row (Japanese)
    uint32_t lastCode = 0;        // +0x584: the last character laid out (Japanese)

    // The text cleared (0x4d70a0's share; CTextVertical's 0x4ceee0 then sets +0x4d4).
    void clear();
    // vt+0x80: `text` laid out after what is there (create and print), or, `text` empty, `full`
    // laid out again from the start, the characters keeping everything but their places. False if
    // the tokenizer refuses the text.
    bool layout(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                const TextLayoutSettings& settings, const LineBreakTables& breaks);
    // print's step before laying out more text (0x4d37e0; CTextVertical 0x4cef00 down the column):
    // the pitch for its first printable character.
    void pitchBeforePrint(const std::string& text, const TextLayoutSettings& settings);
    // vt+0x88 (0x4d2cf0's first step): the corner offsets. English (0x4cbcb0): SMALL characters up
    // by the ruby offset. Japanese (0x4d29f0): a pair of neighbours on a row, alike in SMALL and
    // italic, with a fix rule for their codes moves the first one's right corners by its value.
    // Vertical (0x4cf080): none.
    void cornerOffsets(const TextLayoutSettings& settings, const std::vector<PositionFixRule>& rules);

    TextChar& at(uint32_t poolEntry) { return poolEntry & kRubyBit ? rubies[poolEntry & ~kRubyBit] : chars[poolEntry]; }

private:
    bool layoutEnglish(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                       const TextLayoutSettings& s);
    bool layoutJapanese(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                        const TextLayoutSettings& s, const LineBreakTables& breaks);
    bool layoutVertical(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                        const TextLayoutSettings& s, const LineBreakTables& breaks);
};

// MultiByteToWideChar(CP_UTF8, ...): UTF-16 units (U+FFFD for each broken sequence).
std::u16string utf8ToUtf16(const std::string& s);

// SQApp.loadTextTexture's settings as its native leaves them in the older text code it drives
// (Diesel::CTextTyping, muramasa_en.exe 0x560d30 ff.; the native: section 5c of diesel-status.md).
struct TextTextureSettings {
    int typingType = 0;         // +0xac: 1 word wrap, 2 vertical, anything else character by character
    int width = 0, height = 0;  // +0x98 / +0x9c: the texture's size (below 1: the laid-out size)
    std::array<int, 4> area{0, 0, 512, 512};  // +0xc: typing_area x, y, w, h
    int textSize = 0x20, rubySize = 0xc, weight = 400, alphaBias = 0;  // the fonts (0x1004fd90)
    int rowSpace = 0, pitchFull = 0, pitchHalf = 0, rubyOffset = 0;    // +0x38 / +0x3c / +0x40 / +0x44
    float rubyMargineRate = 0.0f;                                      // +0x48
    uint32_t fontColor = 0xffffff;      // +0x8c: red in the low byte (the texel's order)
    uint32_t shadowColor = 0;           // +0xa0
    uint32_t shadowAlpha = 0xff000000;  // +0xa4: shadow_power as an alpha in the top byte
    int shadowType = 0;                 // +0xa8
    int shadowX = 0, shadowY = 0;       // +0x90 / +0x94
};

// The fonts CTextTyping asks Mware's factory for (vtable +0x10: the native's font name and charset,
// a size, the weight, vertical for typing type 2, upright or italic, the alpha bias): by size.
using TypingFonts = std::function<std::shared_ptr<FontInstance>(int size)>;

// CTextTyping's layout (0x561130) and drawing (0x562310) of a text: the texture, rows from the top,
// RGBA bytes (Mware makes it R8G8B8A8; the alpha is not premultiplied). `log` hears of the inputs
// on which the exe crashes or never returns, where Nyanroid takes a Guess.
Image renderTextTexture(const std::string& text, const TextTextureSettings& settings, const TypingFonts& fonts,
                        const LineBreakTables& breaks, const std::function<void(const std::string&)>& log);

}  // namespace nyan::diesel
