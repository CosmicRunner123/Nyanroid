#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace nyan::wa2 {

struct SpriteObject;

// A text record (WA2.exe 0xa823a8 + i * 0x34, 128 of them): the string a text object (kind 0xb)
// draws, its layout parameters and what the last draw reported.
struct TextSlot {
    bool active = false;   // +0x00
    int base = 28;         // +0x01: size
    int em = 32;           // +0x02: the glyph box inside a cell
    int cell = 40;         // +0x03
    int cols = 0, rows = 0;  // +0x04 / +0x05
    int spacing = 0;       // +0x06
    int gap = 0;           // +0x07: line gap, and the first line's y
    bool outline = false;  // +0x08: an outline pass before the body
    bool ruby = false;     // +0x09
    std::string text;      // +0x0c (the renderer changes `^`, `` ` ``, `~` in place)
    int page = -1;         // +0x10: `\k` segments passed (-1: no stops)
    int reveal = -1;       // +0x14: reveal units (-1: all shown)
    bool endReached = false;  // +0x18 (out)
    bool complete = false;    // +0x1c (out)
    int endX = 0;          // +0x20 (out): where the cursor goes
    float endY = 0;        // +0x24 (out)
    int atlas = 0x1c4;     // +0x28: the body atlas; +1 outline, +2 / +3 the 14 pt pair
    uint8_t outlineR = 0, outlineG = 0, outlineB = 0;  // +0x2c..
    uint8_t r = 0xff, g = 0xff, b = 0xff;              // +0x30..
};

// The atlas index of a character code (WA2.exe 0x401050, ranges at 0x4a14b8): 0x20 is 0, codes outside
// the ranges give 7656 (a blank cell).
int glyphIndex(uint32_t code);

// One pass of the text renderer (WA2.exe 0x4462c0) for a text object: lays out the slot's string from
// (0, gap) relative to the object's first corner and emits a glyph quad object per character (the
// object with its corners, source, slot and colour changed, 0x4460a0). The outline pass draws whole
// cells from the outline atlas in the outline colour; the body pass the glyph box from the body atlas.
// Writes the slot's end flag and anchor; returns the "complete" result the slot keeps (+0x1c).
bool renderText(const SpriteObject& obj, TextSlot& slot, bool outlinePass,
                const std::function<void(const SpriteObject&)>& emit);

}  // namespace nyan::wa2
