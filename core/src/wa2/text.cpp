#include "nyan/wa2/text.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "nyan/wa2/gfx.hpp"

namespace nyan::wa2 {

namespace {

// Code ranges of the glyph atlases (0x4a14b8), indices consecutive.
constexpr std::array<std::pair<uint16_t, uint16_t>, 59> kGlyphRanges = {{
    {0x0020, 0x007d}, {0xffb1, 0xffdd}, {0x8140, 0x81ac}, {0x81b8, 0x81bf}, {0x81c8, 0x81ce}, {0x81da, 0x81fc},
    {0x824f, 0x8258}, {0x8260, 0x8279}, {0x8281, 0x829a}, {0x829f, 0x82f1}, {0x8340, 0x8396}, {0x839f, 0x83b6},
    {0x83bf, 0x83d6}, {0x8440, 0x8460}, {0x8470, 0x8491}, {0x849f, 0x84be}, {0x8740, 0x875d}, {0x875f, 0x8775},
    {0x877e, 0x878f}, {0x889f, 0x88fc}, {0x8940, 0x89fc}, {0x8a40, 0x8afc}, {0x8b40, 0x8bfc}, {0x8c40, 0x8cfc},
    {0x8d40, 0x8dfc}, {0x8e40, 0x8efc}, {0x8f40, 0x8ffc}, {0x9040, 0x90fc}, {0x9140, 0x91fc}, {0x9240, 0x92fc},
    {0x9340, 0x93fc}, {0x9440, 0x94fc}, {0x9540, 0x95fc}, {0x9640, 0x96fc}, {0x9740, 0x97fc}, {0x9840, 0x9872},
    {0x989f, 0x98fc}, {0x9940, 0x99fc}, {0x9a40, 0x9afc}, {0x9b40, 0x9bfc}, {0x9c40, 0x9cfc}, {0x9d40, 0x9dfc},
    {0x9e40, 0x9efc}, {0x9f40, 0x9ffc}, {0xe040, 0xe0fc}, {0xe140, 0xe1fc}, {0xe240, 0xe2fc}, {0xe340, 0xe3fc},
    {0xe440, 0xe4fc}, {0xe540, 0xe5fc}, {0xe640, 0xe6fc}, {0xe740, 0xe7fc}, {0xe840, 0xe8fc}, {0xe940, 0xe9fc},
    {0xea40, 0xeaa4}, {0xf040, 0xf0c0}, {0xfa40, 0xfafc}, {0xfb40, 0xfbfc}, {0xfc40, 0xfc4b},
}};

// `<C n`'s colours (0x4c2538): R, G, B.
constexpr std::array<std::array<uint8_t, 3>, 16> kTagColors = {{
    {0xff, 0xff, 0xff}, {0xc0, 0xc0, 0xc0}, {0x50, 0x50, 0x50}, {0x00, 0x00, 0x00},
    {0xff, 0x00, 0x00}, {0x00, 0xff, 0x00}, {0x00, 0x00, 0xff}, {0xff, 0xff, 0x00},
    {0x00, 0xff, 0xff}, {0xff, 0x00, 0xff}, {0xff, 0x80, 0x00}, {0x00, 0xff, 0x80},
    {0x80, 0x00, 0xff}, {0x80, 0xff, 0x00}, {0x00, 0x80, 0xff}, {0xff, 0x00, 0x80},
}};

// The two-byte strings a line may not start with (0x4a52ec..0x4a532c, 0x4a3090): such a character
// hangs past the wrap column.
constexpr std::array<uint16_t, 18> kNoBreakBefore = {0x8142, 0x8141, 0x8148, 0x8149, 0x816a, 0x8176, 0x8178, 0x82e1, 0x82e3,
                                                     0x82e5, 0x82c1, 0xf044, 0xf043, 0xf042, 0xf041, 0xf045, 0xf040, 0x8140};
constexpr uint16_t kIdeographicSpace = 0x8140;

bool isSingleByte(uint8_t b) {
    return b < 0x80 || (b >= 0xb1 && b <= 0xdd);
}

uint16_t pairAt(const std::string& s, size_t i) {
    uint8_t a = i < s.size() ? static_cast<uint8_t>(s[i]) : 0;
    uint8_t b = i + 1 < s.size() ? static_cast<uint8_t>(s[i + 1]) : 0;
    return static_cast<uint16_t>(a << 8 | b);
}

// 0x445140: the reveal cost of a character at speed `sp` (tenths of a unit), carried between characters.
struct Cost {
    int acc = 0;
    int operator()(int sp) {
        int v = acc + sp % 10;
        acc = v % 10;
        return v / 10 + sp / 10;
    }
};

// 0x4450e0: decimal digits at `i` (`i` left on the last digit, or on a ':' that follows); the digit
// count, 0 if none (value 0).
int parseNumber(const std::string& s, size_t& i, int& value) {
    value = 0;
    int n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        value = value * 10 + (s[i] - '0');
        ++i;
        ++n;
    }
    if (i >= s.size() || s[i] != ':') --i;
    return n;
}

}  // namespace

int glyphIndex(uint32_t code) {
    int index = 0;
    for (auto [first, last] : kGlyphRanges) {
        if (code >= first && code <= last) return index + static_cast<int>(code - first);
        index += last - first + 1;
    }
    return index;
}

bool renderText(const SpriteObject& obj, TextSlot& slot, bool outlinePass,
                const std::function<void(const SpriteObject&)>& emit) {
    // 0x4462c0. Arguments as the frame draw passes them (0x442c26).
    const int x0 = 0, cols = slot.cols, rows = slot.rows, spacing = slot.spacing, gap = slot.gap, base = slot.base;
    const int y0 = gap;
    const int reveal = slot.reveal, page = slot.page;
    const int spriteAlpha = static_cast<int>(obj.diffuse[0] >> 24);
    std::string& text = slot.text;
    slot.endX = x0;
    slot.endY = static_cast<float>(y0);
    slot.endReached = false;

    struct Tag {
        int type;
        uint32_t saved;
    };
    std::array<Tag, 16> tags{};
    int depth = 0;
    int r = slot.r, g = slot.g, b = slot.b;
    int x = x0, y = y0, s = base, lh = base, lineEnd = 0;
    int sp = 10, wait = 0, pos = 0, seg = 0;
    bool emphasis = false, hanging = false, wrapped = false, stopAtPage = false;
    Cost cost;

    // A glyph quad (0x4460a0): the object's copy at the pen, on atlas `slot.atlas + variant`.
    auto glyph = [&](uint32_t code, int qx0, int qy0, int qx1, int qy1, int srcBase, int srcSize, int a, int variant) {
        SpriteObject q = obj;
        q.blend = 1;
        if (slot.cell != qx1 - qx0) q.filter = 1;
        q.slot = static_cast<int16_t>(obj.slot + variant);
        float ox = obj.x[0], oy = obj.y[0];
        q.x = {ox + static_cast<float>(qx0), ox + static_cast<float>(qx1), ox + static_cast<float>(qx0), ox + static_cast<float>(qx1)};
        q.y = {oy + static_cast<float>(qy0), oy + static_cast<float>(qy0), oy + static_cast<float>(qy1), oy + static_cast<float>(qy1)};
        if (!(code >= 0xffb1 && code <= 0xffdd) && code > 0xff) code = (code & 0xff) << 8 | (code >> 8 & 0xff);
        int index = glyphIndex(code);
        float cell = variant < 2 ? 40.0f : 24.0f;
        q.u0 = static_cast<float>(srcBase) + static_cast<float>(index % 80) * cell;
        q.u1 = q.u0 + static_cast<float>(srcSize);
        q.v0 = static_cast<float>(index / 80) * cell + static_cast<float>(srcBase);
        q.v1 = q.v0 + static_cast<float>(srcSize);
        uint32_t color = outlinePass ? (static_cast<uint32_t>(slot.outlineR) << 16 | static_cast<uint32_t>(slot.outlineG) << 8 | slot.outlineB)
                                     : (static_cast<uint32_t>(r & 0xff) << 16 | static_cast<uint32_t>(g & 0xff) << 8 | static_cast<uint32_t>(b & 0xff));
        color |= static_cast<uint32_t>(a & 0xff) << 24;
        q.diffuse = {color, color, color, color};
        q.specular = {0xff000000, 0xff000000, 0xff000000, 0xff000000};
        emit(q);
    };
    // The small atlases' character (ruby, emphasis dots): 24-px cells, scale 1.
    auto smallGlyph = [&](uint32_t code, int qx, int qy, int a) {
        if (outlinePass) glyph(code, qx, qy, qx + 24, qy + 24, 0, 24, a, 3);
        else glyph(code, qx, qy, qx + 24, qy + 24, 0, 24, a, 2);
    };
    auto newLine = [&] {
        lineEnd = x;
        int old = lh;
        lh = s;
        x = x0;
        y += old + gap;
    };

    size_t i = 0;
    while (true) {
        if (reveal != -1 && reveal <= pos && page <= seg) break;
        if (wait > 0) {
            // <W n: units pass with nothing drawn.
            bool stop = false;
            while (wait > 0) {
                --wait;
                if (page <= seg) ++pos;
                if (reveal != -1 && reveal <= pos && page <= seg) {
                    stop = true;
                    break;
                }
            }
            if (stop) break;
        }
        int alpha = spriteAlpha;
        if (reveal != -1) {
            int v = std::clamp(reveal - pos, 0, 16);
            alpha = (v * spriteAlpha * 16) >> 8;
        }
        if (seg < page) alpha = spriteAlpha;

        uint8_t c = i < text.size() ? static_cast<uint8_t>(text[i]) : 0;
        size_t at = i;            // the byte drawn, for the glyph cases
        bool draw = false;
        bool skipCheck = false;   // the "skip" cases look at the \k stop first
        switch (c) {
        case 0:
            if (spriteAlpha <= alpha) slot.endReached = true;
            goto end;
        case '\n':
            if (!wrapped) newLine();
            wrapped = false;
            break;
        case '<': {
            ++i;  // the tag letter
            if (depth >= 15) break;
            uint8_t t = i < text.size() ? static_cast<uint8_t>(text[i]) : 0;
            size_t k = i + 1;
            int v = 0;
            auto savedColor = [&] { return 0xff000000u | static_cast<uint32_t>(r & 0xff) << 16 | static_cast<uint32_t>(g & 0xff) << 8 | static_cast<uint32_t>(b & 0xff); };
            switch (t) {
            case 'A': case 'a':
                tags[static_cast<size_t>(++depth)] = {6, 0};
                emphasis = true;
                break;
            case 'B': case 'b':
                tags[static_cast<size_t>(++depth)] = {2, savedColor()};
                if (parseNumber(text, k, v)) r = v;
                ++k;
                if (parseNumber(text, k, v)) g = v;
                ++k;
                if (parseNumber(text, k, v)) b = v;
                i = k;
                break;
            case 'C': case 'c':
                tags[static_cast<size_t>(++depth)] = {1, savedColor()};
                if (parseNumber(text, k, v) && v >= 0 && v < 16) {
                    r = kTagColors[static_cast<size_t>(v)][0];
                    g = kTagColors[static_cast<size_t>(v)][1];
                    b = kTagColors[static_cast<size_t>(v)][2];
                }
                i = k;
                break;
            case 'D': case 'd':
                tags[static_cast<size_t>(++depth)] = {0, 0};
                parseNumber(text, k, v);
                i = k;
                break;
            case 'F': case 'f':
                tags[static_cast<size_t>(++depth)] = {4, static_cast<uint32_t>(s)};
                if (parseNumber(text, k, v)) {
                    s = v;
                    if (lh < v) lh = v;
                }
                i = k;
                break;
            case 'K': case 'k':
                tags[static_cast<size_t>(++depth)] = {3, 0xffffffffu};
                parseNumber(text, k, v);
                ++k;
                parseNumber(text, k, v);
                ++k;
                parseNumber(text, k, v);
                i = k;
                break;
            case 'R': case 'r':
                tags[static_cast<size_t>(++depth)] = {5, static_cast<uint32_t>(x)};
                break;
            case 'S': case 's': {
                tags[static_cast<size_t>(++depth)] = {7, static_cast<uint32_t>(sp)};
                static constexpr int kSpeeds[11] = {100, 80, 60, 40, 20, 10, 6, 4, 2, 1, 0};
                if (parseNumber(text, k, v) && v >= 0 && v <= 10) sp = kSpeeds[v];
                i = k;
                break;
            }
            case 'W': case 'w':
                tags[static_cast<size_t>(++depth)] = {8, 0};
                if (parseNumber(text, k, v)) wait = v;
                i = k;
                break;
            default:
                skipCheck = true;  // other letters: nothing
                break;
            }
            break;
        }
        case '>':
            if (depth < 1) break;
            switch (tags[static_cast<size_t>(depth--)].type) {
            case 1:
            case 2: {
                uint32_t v = tags[static_cast<size_t>(depth + 1)].saved;
                r = static_cast<int>(v >> 16 & 0xff);
                g = static_cast<int>(v >> 8 & 0xff);
                b = static_cast<int>(v & 0xff);
                break;
            }
            case 3:
            case 5:
                skipCheck = true;
                break;
            case 4:
                s = static_cast<int>(tags[static_cast<size_t>(depth + 1)].saved);
                lh = x != x0 ? std::max(lh, s) : s;
                break;
            case 6:
                emphasis = false;
                break;
            case 7:
                sp = static_cast<int>(tags[static_cast<size_t>(depth + 1)].saved);
                break;
            default:
                break;
            }
            break;
        case '\\': {
            ++i;
            uint8_t n = i < text.size() ? static_cast<uint8_t>(text[i]) : 0;
            if (n == '<' || n == '>' || n == '\\' || n == '^' || n == '|' || n == '~') {
                at = i;
                draw = true;
            } else if (n == 'k') {
                if (page <= seg && page != -1) stopAtPage = true;
                ++seg;
                skipCheck = true;
            } else if (n == 'n') {
                if (!wrapped) newLine();
                wrapped = false;
            } else if (n >= 0x3c && n <= 0x7e) {
                skipCheck = true;  // both bytes skipped
            }
            // (bytes outside 0x3c..0x7e: both skipped, without the stop check)
            break;
        }
        case '^':
            text[i] = ' ';
            draw = true;
            break;
        case '`':
            text[i] = '\'';
            draw = true;
            break;
        case '~':
            text[i] = ',';
            draw = true;
            break;
        case '|': {
            ++i;
            if (depth > 0 && tags[static_cast<size_t>(depth)].type == 5) {
                // Ruby: the reading up to '>' spread over the base text.
                std::string reading;
                while (i < text.size() && text[i] != '>') reading += text[i++];
                int n = static_cast<int>(reading.size());
                int xs = static_cast<int>(tags[static_cast<size_t>(depth)].saved);
                int spare = ((x - ((n + 2) * 16) / 2) - xs) / 2;
                int step = 0, start = spare;
                if (spare >= 1) {
                    auto first = static_cast<uint8_t>(reading.empty() ? 0 : reading[0]);
                    int count = (first < '!' || first > '~') && (first < 0xa1 || first > 0xdf) ? n / 2 : n;
                    step = count != 0 ? (spare * 2) / count : 0;
                    start = step / 2;
                }
                if (slot.ruby && !reading.empty()) {
                    int stride = step * 2 + 0x20;
                    int px = s / 2 + start + xs;
                    for (size_t k = 0, off = 0; k < reading.size(); k += 2, off += static_cast<size_t>(stride)) {
                        uint16_t code = static_cast<uint16_t>(static_cast<uint8_t>(reading[k]) |
                                                              (k + 1 < reading.size() ? static_cast<uint8_t>(reading[k + 1]) : 0) << 8);
                        smallGlyph(code, px + static_cast<int>(off) / 2 - 5, y - 13, alpha);
                    }
                }
                --i;  // on the byte before '>'
            }
            break;
        }
        default:
            draw = true;
            break;
        }

        if (draw) {
            uint8_t gb = static_cast<uint8_t>(text[at]);
            float k = static_cast<float>(s) / static_cast<float>(slot.base);
            int inset = (slot.cell - slot.em) / 2;
            auto fx = static_cast<float>(x);
            if (!isSingleByte(gb)) {
                uint16_t code = static_cast<uint16_t>(gb | (at + 1 < text.size() ? static_cast<uint8_t>(text[at + 1]) : 0) << 8);
                int yc = y + (slot.base - s) / 2;
                auto fy = static_cast<float>(yc);
                if (outlinePass) {
                    glyph(code, x, yc, static_cast<int>(fx + static_cast<float>(slot.cell) * k),
                          static_cast<int>(fy + static_cast<float>(slot.cell) * k), 0, slot.cell, alpha, 1);
                } else {
                    glyph(code, static_cast<int>(fx + static_cast<float>(inset) * k), static_cast<int>(fy + static_cast<float>(inset) * k),
                          static_cast<int>(fx + static_cast<float>(inset + slot.em) * k),
                          static_cast<int>(fy + static_cast<float>(inset + slot.em) * k), inset, slot.em, alpha, 0);
                }
                int cst = cost(sp);
                if (page <= seg) pos += cst;
                if (emphasis) {
                    int xc = s / 2 + x;
                    smallGlyph(0x4581, static_cast<int>(static_cast<float>(xc) - 5.0f * k), y - 13, alpha);
                }
                x += s + spacing;
                wrapped = false;
                i = at + 1;
            } else {
                uint32_t code = gb >= 0xb1 && gb <= 0xdd ? (0xff00u | gb) : gb;
                auto fy = static_cast<float>(y);
                if (outlinePass) {
                    glyph(code, x, y, static_cast<int>(fx + static_cast<float>(slot.cell) * k),
                          static_cast<int>(fy + static_cast<float>(slot.cell) * k), 0, slot.cell, alpha, 1);
                } else {
                    glyph(code, static_cast<int>(fx + static_cast<float>(inset) * k), static_cast<int>(fy + static_cast<float>(inset) * k),
                          static_cast<int>(fx + static_cast<float>(inset + slot.em) * k),
                          static_cast<int>(fy + static_cast<float>(inset + slot.em) * k), inset, slot.em, alpha, 0);
                }
                int cst = cost(sp);
                if (page <= seg) pos += cst;
                x = spacing / 2 + x + s / 2;
                wrapped = false;
                i = at;
            }
        } else if (skipCheck && stopAtPage) {
            break;
        }
        ++i;
        // Wrap (0x4466f9): past the line's width, unless the next character may not start a line.
        if ((spacing + base) * cols - s - spacing + 1 <= x - x0 && !(depth > 0 && tags[static_cast<size_t>(depth)].type == 5)) {
            uint16_t next = pairAt(text, i);
            if (hanging) {
                if (next != kIdeographicSpace) {
                    newLine();
                    hanging = false;
                    wrapped = true;
                }
            } else if (std::find(kNoBreakBefore.begin(), kNoBreakBefore.end(), next) != kNoBreakBefore.end()) {
                hanging = true;
            } else {
                newLine();
                wrapped = true;
            }
        }
        // Overflow below the last line.
        if ((gap + base) * rows - s - gap + 1 <= y - y0) {
            slot.endReached = true;
            break;
        }
    }
end:
    if (x == x0 && y != y0 && lineEnd - x0 <= (base + spacing) * (cols + 1)) {
        slot.endX = lineEnd;
        slot.endY = static_cast<float>(y - lh - gap);
    } else {
        slot.endX = x;
        slot.endY = static_cast<float>(y);
    }
    return slot.endReached || pos <= reveal - 16;
}

}  // namespace nyan::wa2
