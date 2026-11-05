#include "nyan/diesel/text_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

#include "nyan/diesel/text_tokenizer.hpp"

namespace nyan::diesel {

namespace {

// iswalpha / iswdigit for the Latin-1 units the layout asks about (the MSVC runtime's wide table:
// ASCII letters and digits, the Latin-1 letters, ª µ º, and ² ³ ¹ as digits).
bool isAlnum(uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    if (c == 0xaa || c == 0xb5 || c == 0xba || c == 0xb2 || c == 0xb3 || c == 0xb9) return true;
    return c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7;
}

// The characters the next-character pitch looks past (the ruby shorthand's marks and line breaks).
bool skippedForPitch(uint32_t c) { return c == '#' || c == '}' || c == '{' || c == '\n' || c == '\r'; }

// cvttss2si: the float truncated, 0x80000000 for NaN or out of range.
int32_t truncate(float f) {
    if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) return INT32_MIN;
    return static_cast<int32_t>(f);
}

// An unsigned int as the exe makes a float of it: through a double, 2^32 added when the top bit
// is set (0x5eeff0's table).
float unsignedToFloat(uint32_t v) {
    double d = static_cast<double>(static_cast<int32_t>(v));
    if (static_cast<int32_t>(v) < 0) d += 4294967296.0;
    return static_cast<float>(d);
}

bool contains(const std::vector<uint32_t>& table, uint32_t c) {
    return std::find(table.begin(), table.end(), c) != table.end();
}

// The characters a word's end may hang past the field (up to two of them).
bool hangs(uint32_t c) {
    return c == ' ' || c == '.' || c == ')' || c == '"' || c == '?' || c == '!' || c == '\'' || c == '}';
}

}  // namespace

std::u16string utf8ToUtf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto b = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (b < 0x80) {
            cp = b, extra = 0;
        } else if (b >= 0xc2 && b < 0xe0) {
            cp = b & 0x1fu, extra = 1;
        } else if (b >= 0xe0 && b < 0xf0) {
            cp = b & 0x0fu, extra = 2;
        } else if (b >= 0xf0 && b < 0xf5) {
            cp = b & 0x07u, extra = 3;
        } else {
            out.push_back(0xfffd);
            ++i;
            continue;
        }
        bool ok = true;
        size_t j = i + 1;
        for (int k = 0; k < extra && ok; ++k, ++j) {
            if (j >= s.size() || (static_cast<unsigned char>(s[j]) & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            cp = cp << 6 | (static_cast<unsigned char>(s[j]) & 0x3fu);
        }
        if (!ok || (extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp < 0xe000)) {
            out.push_back(0xfffd);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i = j;
    }
    return out;
}

// 0x4d70a0: the characters, the pen and the sizes; CTextVertical's clear (0x4ceee0) then sets
// +0x4d4. The line-break run (+0x580 / +0x584) stays.
void TextLayout::clear() {
    chars.clear();
    rubies.clear();
    pool.clear();
    x = y = column = lineHeight = maxX = totalHeight = 0;
    lines = 1;
    lastWide = false;
    noPrintPitch = type == 2 ? 1 : 0;
}

// 0x4d37e0 / 0x4cef00: the pitch for the first character that is not a ruby mark or a line break.
void TextLayout::pitchBeforePrint(const std::string& text, const TextLayoutSettings& s) {
    if (noPrintPitch != 0) return;
    for (char16_t c : utf8ToUtf16(text)) {
        if (skippedForPitch(c)) continue;
        int p = ((c & 0xff00) == 0 || !lastWide) ? s.pitchHalf : s.pitchFull;
        if (type == 2) y += p;
        else x += p;
        break;
    }
}

bool TextLayout::layout(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                        const TextLayoutSettings& s, const LineBreakTables& breaks) {
    if (type == 0) return layoutJapanese(text, full, main, ruby, s, breaks);
    if (type == 2) return layoutVertical(text, full, main, ruby, s, breaks);
    return layoutEnglish(text, full, main, ruby, s);
}

bool TextLayout::layoutEnglish(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                               const TextLayoutSettings& s) {
    const bool relayout = text.empty();
    int charIndex = relayout ? 0 : static_cast<int>(chars.size());
    const int mainHeight = main.height();
    if (relayout) {
        noPrintPitch = 0;
        x = y = lineHeight = 0;
        totalHeight = maxX = 0;
        lines = 1;
        lastWide = false;
    }
    auto lines_ = tokenizeText(relayout ? full : text, true);
    if (!lines_) return false;

    // The style the tags set (the struct at EBP-0xb0): italic, centring, SMALL, and where centring
    // began (character, pen x).
    bool italic = false, center = false, small = false;
    int centerStart = -1, centerX = 0;
    std::vector<TextChar> word;
    float wordX = 0.0f, wordY = 0.0f, wordWidth = 0.0f, prevPitch = 0.0f;
    int n = 0;  // units read by this call: the typing offset

    auto fontOf = [&](const TextChar& c) -> TextFont& { return c.small ? ruby : main; };
    // The next line (0x4cb6f0).
    auto newline = [&]() {
        maxX = std::max(maxX, x);
        x = 0;
        int lh = lineHeight != 0 ? lineHeight : mainHeight;
        y += s.rowSpace + lh;
        totalHeight = y;
        lineHeight = 0;
        ++lines;
        if (s.fieldHeight < mainHeight + y && s.fieldType == 1) y = 0;
    };
    // Centring what was laid out since CENTER (0x4cb7b0); false if it moved the run to a new line.
    auto finishCenter = [&]() -> bool {
        int cs = centerStart;
        if (cs < 0 || cs >= charIndex) {
            if (!center) centerStart = -1;
            centerX = 0;
            return true;
        }
        const TextChar& first = chars[static_cast<size_t>(cs)];
        const TextChar& last = chars[static_cast<size_t>(charIndex - 1)];
        TextGlyph g1 = fontOf(first).glyph(first.code, first.italic);
        TextGlyph g2 = fontOf(last).glyph(last.code, last.italic);
        int pen = x;
        int shift = (s.fieldWidth + static_cast<int>(first.x) - pen - g1.overhang + g2.adjust) / 2 - centerX;
        if (centerX != 0 &&
            static_cast<float>(centerX) > static_cast<float>(g2.adjust) + first.x + static_cast<float>(shift)) {
            x = pen - centerX;
            int oldY = y;
            int lh = lineHeight != 0 ? lineHeight : mainHeight;
            y += s.rowSpace + lh;
            totalHeight = y;
            lineHeight = 0;
            ++lines;
            float dx = static_cast<float>(-centerX), dy = static_cast<float>(y - oldY);
            for (int k = cs; k < charIndex; ++k) {
                chars[static_cast<size_t>(k)].x += dx;
                chars[static_cast<size_t>(k)].y += dy;
            }
            centerX = 0;
            return false;
        }
        for (int k = cs; k < charIndex; ++k) chars[static_cast<size_t>(k)].x += static_cast<float>(shift);
        centerStart = center ? charIndex : -1;
        centerX = 0;
        return true;
    };

    for (const auto& line : *lines_) {
        for (const auto& tok : line) {
            const std::string& name = tok.name;
            if (name == "I") {
                italic = true;
            } else if (name == "/I") {
                italic = false;
            } else if (name == "SMALL" && !older) {
                small = true;
            } else if (name == "/SMALL" && !older) {
                small = false;
            } else if (name == "CENTER") {
                center = true;
                centerX = x;
                centerStart = charIndex;
            } else if (name == "/CENTER") {
                center = false;
                finishCenter();
                if (charIndex > 0 && charIndex <= static_cast<int>(chars.size())) {
                    const TextChar& last = chars[static_cast<size_t>(charIndex - 1)];
                    TextGlyph g = fontOf(last).glyph(last.code, last.italic);
                    x = static_cast<int>(last.x) + last.cellWidth + g.adjust;
                }
            }
            if (tok.text.empty()) continue;
            std::u16string w = utf8ToUtf16(tok.text);
            const int len = static_cast<int>(w.size());
            int alnum = 0;
            for (int i = 0; i < len; ++i, ++n) {
                lastWide = false;
                bool wordEnd = false, lineEnd = false;
                uint32_t c = w[static_cast<size_t>(i)];
                if (c == '\t') continue;
                if (c & 0xff00) {
                    lastWide = true;
                } else if (c == '\r' || c == '\n') {
                    uint32_t d = i + 1 < len ? w[static_cast<size_t>(i + 1)] : 0;
                    if (d == '\r' || d == '\n') ++i;
                    lineEnd = true;
                    wordEnd = true;
                } else if (c == ' ') {
                    wordEnd = true;
                } else if (isAlnum(c)) {
                    ++alnum;
                }
                if (i == len - 1) wordEnd = true;
                if (!lineEnd) {
                    TextFont& font = small ? ruby : main;
                    TextGlyph g = font.glyph(c, italic);
                    // Upright after italic: the italic character's adjustment comes off.
                    if (!italic) {
                        const TextChar* prev = nullptr;
                        if (word.empty()) {
                            if (charIndex > 0 && charIndex <= static_cast<int>(chars.size()) &&
                                chars[static_cast<size_t>(charIndex - 1)].italic) {
                                prev = &chars[static_cast<size_t>(charIndex - 1)];
                            }
                        } else if (word.back().italic) {
                            prev = &word.back();
                        }
                        if (prev) wordX -= static_cast<float>(fontOf(*prev).glyph(prev->code, prev->italic).adjust);
                    }
                    TextChar ch;
                    ch.code = c;
                    ch.italic = italic;
                    ch.small = small;
                    ch.x = static_cast<float>(g.overhang) + wordX;
                    ch.y = wordY;
                    ch.cellWidth = g.cellWidth;
                    ch.cellHeight = g.cellHeight;
                    ch.adjust = g.adjust;
                    ch.overhang = g.overhang;
                    ch.shown = ch.typingStart = static_cast<float>(n) * -48.0f;
                    ch.image = g.image;
                    word.push_back(ch);
                    int adv = g.advance();
                    float f = static_cast<float>(adv);
                    wordWidth = (f + prevPitch) + wordWidth;
                    wordX = static_cast<float>(adv) + wordX;
                    // The pitch for the next printable character.
                    if (i < len - 1) {
                        for (int j = i + 1; j < len; ++j) {
                            uint32_t c2 = w[static_cast<size_t>(j)];
                            // (The older layout looks at the unit's low byte, and its pitch is the
                            // half-width one.)
                            if (skippedForPitch(older ? c2 & 0xffu : c2)) continue;
                            int p = older || (c2 & 0xff00) == 0 ? s.pitchHalf : (lastWide ? s.pitchFull : s.pitchHalf);
                            prevPitch = static_cast<float>(p);
                            wordX = prevPitch + wordX;
                            break;
                        }
                    }
                }
                if (!wordEnd) continue;
                // Up to two trailing characters may hang past the field.
                for (int k = static_cast<int>(word.size()) - 1, hung = 0; k >= 0 && hung < 2; --k, ++hung) {
                    if (!hangs(word[static_cast<size_t>(k)].code)) break;
                    wordWidth -= static_cast<float>(word[static_cast<size_t>(k)].cellWidth);
                }
                bool cut = false;
                if (older) {
                    // 0x49ee2d: past the field with the pen off the line's start: the next line;
                    // otherwise a word wider than the field is cut.
                    if (!(x < 1 || static_cast<float>(x) + wordWidth <= static_cast<float>(s.fieldWidth))) {
                        if (finishCenter()) newline();
                    } else {
                        cut = static_cast<float>(s.fieldWidth) < wordWidth;
                    }
                } else {
                    if (alnum > 0 && static_cast<float>(x) + wordWidth > static_cast<float>(s.fieldWidth)) {
                        if (finishCenter()) newline();
                    }
                    cut = wordWidth > static_cast<float>(s.fieldWidth);
                }
                if (cut) {
                    // Wider than the field: keep what fits, read the rest again on the next line.
                    --i;
                    int count = static_cast<int>(word.size());
                    // (The exe would drop the first character too and read before the word when
                    // not even it fits, looping on it; Nyanroid keeps it.)
                    for (int k = count - 1; k >= 1; --k) {
                        const TextChar& p = word[static_cast<size_t>(k)];
                        TextGlyph g = fontOf(p).glyph(p.code, p.italic);
                        bool fits = older ? !hangs(p.code) && truncate((static_cast<float>(g.cellWidth) + p.x) -
                                                                       static_cast<float>(g.adjust)) <= s.fieldWidth
                                          : static_cast<int>(p.x) - g.adjust + x + g.cellWidth <= s.fieldWidth;
                        if (fits) break;
                        --i;
                        --count;
                    }
                    word.resize(static_cast<size_t>(count));
                    lineEnd = true;
                    if (!word.empty()) {
                        const TextChar& last = word.back();
                        TextGlyph g = fontOf(last).glyph(last.code, last.italic);
                        wordX = (static_cast<float>(g.cellWidth) + last.x) - static_cast<float>(g.adjust);
                    } else {
                        wordX = 0.0f;
                    }
                }
                for (auto& ch : word) {
                    ch.x += static_cast<float>(x);
                    ch.y += static_cast<float>(y);
                    lineHeight = std::max(lineHeight, ch.cellHeight);
                    if (relayout) {
                        if (charIndex < static_cast<int>(chars.size())) {
                            chars[static_cast<size_t>(charIndex)].x = ch.x;
                            chars[static_cast<size_t>(charIndex)].y = ch.y;
                        }
                    } else {
                        pool.push_back(static_cast<uint32_t>(chars.size()));
                        chars.push_back(ch);
                    }
                    ++charIndex;
                }
                x += static_cast<int>(wordX);
                maxX = std::max(maxX, x);
                totalHeight = std::max(totalHeight, lineHeight + y);
                if (lineEnd) {
                    if (!finishCenter()) finishCenter();
                    newline();
                }
                word.clear();
                wordX = wordY = 0.0f;
                wordWidth = 0.0f;
            }
        }
    }
    return true;
}

// CText's layout (muramasa_en.exe 0x4d08c0; phantom.exe 0x1400e7910 is the same).
bool TextLayout::layoutJapanese(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                                const TextLayoutSettings& s, const LineBreakTables& breaks) {
    const bool relayout = text.empty();
    int charIndex = relayout ? 0 : static_cast<int>(chars.size());
    int rubyIndex = relayout ? 0 : static_cast<int>(rubies.size());
    int hang = 0;  // characters let hang past the field in a row (EBP-0x40)
    // vtable +0x20 of both fonts: the widest cell and the height.
    const int mainHeight = main.height(), rubyHeight = ruby.height(), rubyMaxWidth = ruby.maxWidth();
    if (relayout) {
        noPrintPitch = 1;
        x = y = column = lineHeight = 0;
        totalHeight = maxX = 0;
        lines = 1;
        lastWide = false;
        hangRun = 0;
        lastCode = 0;
    }
    auto lines_ = tokenizeText(relayout ? full : text, true);
    if (!lines_) return false;

    // The state the tags set (0x4ce9d0): the ruby text and the character it starts on, italic,
    // centring (where it began: the pen, the character, the ruby character).
    std::u16string rubyText;
    int rubyStart = 0;
    bool italic = false, center = false;
    int centerX = 0, centerChar = -1, centerRuby = -1;
    int n = 0;  // units read by this call: the typing offset (EBP-0xac)

    // The next line (0x4d2640).
    auto newline = [&]() {
        x = 0;
        y += s.rowSpace + (lineHeight != 0 ? lineHeight : mainHeight);
        totalHeight = y;
        lineHeight = 0;
        ++lines;
        lastWide = false;
        hang = 0;
        noPrintPitch = 1;
        hangRun = 0;
        lastCode = 0;
        if (mainHeight + y > s.fieldHeight && s.fieldType == 1) y = 0;
    };
    // Centring what was laid out since CENTER (0x4d2700); false if it moved the run to a new line.
    auto finishCenter = [&]() -> bool {
        int cs = centerChar;
        if (cs < 0 || cs >= charIndex) {
            if (!center) centerChar = centerRuby = -1;
            centerX = 0;
            return true;
        }
        const TextChar& first = chars[static_cast<size_t>(cs)];
        const TextChar& last = chars[static_cast<size_t>(charIndex - 1)];
        TextGlyph g1 = main.glyph(first.code, first.italic);
        TextGlyph g2 = main.glyph(last.code, last.italic);
        int pen = x;
        int shift = (s.fieldWidth + static_cast<int>(first.x) - pen - g1.overhang + g2.adjust) / 2 - centerX;
        int rs = std::max(centerRuby, 0);
        if (centerX != 0 &&
            static_cast<float>(centerX) > static_cast<float>(g2.adjust) + first.x + static_cast<float>(shift)) {
            int oldY = y;
            x = pen - centerX;
            y += s.rowSpace + (lineHeight != 0 ? lineHeight : mainHeight);
            totalHeight = y;
            lineHeight = 0;
            ++lines;
            hang = 0;
            noPrintPitch = 1;
            float dx = static_cast<float>(-centerX), dy = static_cast<float>(y - oldY);
            for (int k = cs; k < charIndex; ++k) {
                chars[static_cast<size_t>(k)].x += dx;
                chars[static_cast<size_t>(k)].y += dy;
            }
            for (int k = rs; k < rubyIndex; ++k) {
                rubies[static_cast<size_t>(k)].x += dx;
                rubies[static_cast<size_t>(k)].y += dy;
            }
            centerX = 0;
            return false;
        }
        for (int k = cs; k < charIndex; ++k) chars[static_cast<size_t>(k)].x += static_cast<float>(shift);
        // The ruby moves with it where it sits on the last ruby character's row.
        if (rubyIndex > centerRuby && rubyIndex > 0) {
            float lastY = rubies[static_cast<size_t>(rubyIndex - 1)].y;
            for (int k = rs; k < rubyIndex; ++k) {
                if (rubies[static_cast<size_t>(k)].y == lastY) rubies[static_cast<size_t>(k)].x += static_cast<float>(shift);
            }
        }
        if (center) {
            centerChar = charIndex;
            centerRuby = rubyIndex;
        } else {
            centerChar = centerRuby = -1;
        }
        centerX = 0;
        return true;
    };
    // </RUBY>: the ruby text over the characters since <RUBY> (0x4d0c0f ff.). Its characters take
    // the typing of the characters under them.
    auto placeRuby = [&]() {
        const size_t len = rubyText.size();
        if (len == 0) return;
        std::vector<std::array<int32_t, 2>> places;
        std::vector<float> starts;
        if (static_cast<int64_t>(len) == static_cast<int64_t>(charIndex - rubyStart)) {
            // One over each: centred on its character's cell, the ruby font's height above it.
            for (size_t k = 0; k < len; ++k) {
                const TextChar& b = chars[static_cast<size_t>(rubyStart) + k];
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                TextGlyph bg = main.glyph(b.code, italic);
                int rw = rg.adjust - rg.overhang + rg.cellWidth;
                int bw = b.cellWidth - bg.overhang + bg.adjust;
                float fx = (static_cast<float>(bw) - static_cast<float>(rw)) * 0.5f + b.x + 0.5f;
                float fy = b.y - static_cast<float>(rubyHeight);
                fy = fy + static_cast<float>(s.rubyOffset);
                fy = fy + static_cast<float>(b.cellHeight) * 0.25f;
                fy = fy + 0.5f;
                places.push_back({truncate(fx), truncate(fy)});
                starts.push_back(b.shown);
            }
        } else if (older) {
            // Row by row (totono_en.exe 0x4a4cd1 ff.): a row is the characters until one is on
            // another line or further left; it takes trunc(its characters x the ruby's length / the
            // characters left) as the end of its ruby characters (the last row: the rest). One over
            // each when they match, else spread as below (spaced only with two or more), the x
            // rounded, a quarter of the row's first cell down from it.
            float firstShown = chars[static_cast<size_t>(rubyStart)].shown;
            float span = chars[static_cast<size_t>(charIndex - 1)].shown - firstShown;
            int restAt = rubyStart;
            uint32_t start = 0, end = static_cast<uint32_t>(len);
            while (restAt < charIndex) {
                uint32_t remaining = static_cast<uint32_t>(charIndex - restAt);
                const TextChar& first = chars[static_cast<size_t>(restAt)];
                float rowX = first.x, rowY = first.y, prevX = first.x, rowW = 0.0f;
                int rowCell = first.cellHeight;
                std::vector<int> row;
                while (restAt < charIndex) {
                    const TextChar& c = chars[static_cast<size_t>(restAt)];
                    if ((!(rowY == static_cast<float>(truncate(c.y))) || prevX > c.x) && !row.empty()) {
                        uint32_t taken = remaining - static_cast<uint32_t>(charIndex - restAt);
                        float share = unsignedToFloat(taken * static_cast<uint32_t>(len)) / unsignedToFloat(remaining);
                        end = static_cast<uint32_t>(share);  // __ftol3: truncated
                        break;
                    }
                    rowW = (static_cast<float>(c.cellWidth) + c.x) - rowX;
                    prevX = c.x;
                    row.push_back(restAt++);
                }
                float rubyWidth = 0.0f;
                for (uint32_t k = start; k < end && k < len; ++k) {
                    TextGlyph rg = ruby.glyph(rubyText[k], italic);
                    rubyWidth = (static_cast<float>(rg.adjust + rg.overhang) + static_cast<float>(rg.cellWidth)) + rubyWidth;
                }
                uint32_t count = end - start;
                if (row.size() == count) {
                    for (uint32_t k = 0; k < count; ++k) {
                        const TextChar& b = chars[static_cast<size_t>(row[k])];
                        TextGlyph rg = ruby.glyph(rubyText[start + k], italic);
                        TextGlyph bg = main.glyph(b.code, italic);
                        int rw = rg.adjust - rg.overhang + rg.cellWidth;
                        int bw = b.cellWidth - bg.overhang + bg.adjust;
                        float fx = (static_cast<float>(bw) - static_cast<float>(rw)) * 0.5f + b.x + 0.5f;
                        float fy = b.y - static_cast<float>(rubyHeight);
                        fy = fy + static_cast<float>(s.rubyOffset);
                        fy = fy + static_cast<float>(b.cellHeight) * 0.25f;
                        fy = fy + 0.5f;
                        places.push_back({truncate(fx), truncate(fy)});
                        starts.push_back(b.shown);
                    }
                } else {
                    float spacing = 0.0f;
                    if (count > 1 && !(rowW < rubyWidth) && !std::isnan(rowW) && !std::isnan(rubyWidth)) {
                        float target = rowW - s.rubyMarginRate * rowW;
                        float half = rowW * 0.5f;
                        target = target > rubyWidth ? target : rubyWidth;  // maxss
                        float gaps = unsignedToFloat(count - 1);
                        float inv = 1.0f / gaps;
                        float least = static_cast<float>(rubyMaxWidth) * -0.25f;
                        float a = (target - rubyWidth) * inv;
                        float b = half * inv;
                        spacing = least;
                        if (!(least > a)) spacing = b < a ? b : a;  // minss
                        rubyWidth = rubyWidth + spacing * gaps;
                    }
                    float startX = rowX - (rubyWidth - rowW) * 0.5f;
                    float quarter = static_cast<float>(rowCell) * 0.25f;
                    for (uint32_t k = start; k < end && k < len; ++k) {
                        TextGlyph rg = ruby.glyph(rubyText[k], italic);
                        float fy = rowY - static_cast<float>(rubyHeight);
                        fy = fy + static_cast<float>(s.rubyOffset);
                        fy = fy + quarter;
                        fy = fy + 0.5f;
                        places.push_back({truncate(startX + 0.5f), truncate(fy)});
                        float t = 0.5f;
                        if (len > 1) t = unsignedToFloat(k) / unsignedToFloat(static_cast<uint32_t>(len - 1));
                        starts.push_back(span * t + firstShown);
                        float step = static_cast<float>(rg.cellWidth) + spacing;
                        step = step + static_cast<float>(rg.adjust + rg.overhang);
                        startX = startX + step;
                    }
                }
                start = end;
                end = static_cast<uint32_t>(len);
            }
        } else {
            // Spread over them: the characters' width (row by row when they wrapped), the ruby's;
            // a narrower ruby is spaced out towards the width less the margin rate, at most half the
            // width between two, at least -1/4 of the ruby font's widest cell.
            const TextChar& first = chars[static_cast<size_t>(rubyStart)];
            float firstShown = first.shown;
            float span = chars[static_cast<size_t>(charIndex - 1)].shown;
            float rubyY = static_cast<float>(s.rubyOffset) + first.y;
            rubyY = rubyY + static_cast<float>(mainHeight) * 0.25f;
            rubyY = rubyY + 0.5f;
            int rowY = truncate(first.y);
            float left = first.x, lineStart = first.x, prevRight = first.x, sum = 0.0f, maxRight = 0.0f;
            bool multi = false;
            for (int k = rubyStart; k < charIndex; ++k) {
                const TextChar& b = chars[static_cast<size_t>(k)];
                int by = truncate(b.y);
                if (rowY != by) {
                    prevRight = prevRight - lineStart;
                    multi = true;
                    lineStart = b.x;
                    rowY = by;
                    sum = sum + prevRight;
                }
                float right = static_cast<float>(b.cellWidth) + b.x;
                right = right + static_cast<float>(b.overhang);
                if (right > maxRight) maxRight = right;
                prevRight = right;
            }
            float baseWidth = (prevRight - lineStart) + sum;
            float rubyWidth = 0.0f;
            for (size_t k = 0; k < len; ++k) {
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                rubyWidth = (static_cast<float>(rg.adjust - rg.overhang) + static_cast<float>(rg.cellWidth)) + rubyWidth;
            }
            float spacing = 0.0f;
            if (!(baseWidth < rubyWidth) && !std::isnan(baseWidth) && !std::isnan(rubyWidth)) {
                float target = baseWidth - s.rubyMarginRate * baseWidth;
                float half = baseWidth * 0.5f;
                target = target > rubyWidth ? target : rubyWidth;  // maxss
                float gaps = unsignedToFloat(static_cast<uint32_t>(len - 1));
                float inv = 1.0f / gaps;
                float least = static_cast<float>(rubyMaxWidth) * -0.25f;
                float a = (target - rubyWidth) * inv;
                float b = half * inv;
                spacing = least;
                if (!(least > a)) spacing = b < a ? b : a;  // minss
                rubyWidth = rubyWidth + spacing * gaps;
            }
            float startX = left - (rubyWidth - baseWidth) * 0.5f;
            span = span - firstShown;
            for (size_t k = 0; k < len; ++k) {
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                // Past the first row's right edge: on to the next row (the characters wrapped).
                if (multi && !(static_cast<float>(rg.cellWidth / 2) + startX < maxRight) &&
                    !std::isnan(startX) && !std::isnan(maxRight)) {
                    startX = startX - maxRight;
                    rubyY = static_cast<float>(s.rowSpace + (lineHeight != 0 ? lineHeight : mainHeight)) + rubyY;
                }
                places.push_back({truncate(startX), truncate(rubyY - static_cast<float>(rubyHeight))});
                float t = 0.5f;
                if (len > 1) t = unsignedToFloat(static_cast<uint32_t>(k)) / unsignedToFloat(static_cast<uint32_t>(len - 1));
                starts.push_back(t * span + firstShown);
                float step = static_cast<float>(rg.cellWidth) + spacing;
                step = step + static_cast<float>(rg.adjust);
                step = step + static_cast<float>(rg.overhang);
                startX = startX + step;
            }
        }
        // (The older rows can leave a share empty or go back over one: the exe then reads past or
        // short of its places; Nyanroid makes as many as it has.)
        for (size_t k = 0; k < len && k < places.size(); ++k) {
            float px = static_cast<float>(places[k][0]), py = static_cast<float>(places[k][1]);
            if (relayout) {
                if (rubyIndex < static_cast<int>(rubies.size())) {
                    rubies[static_cast<size_t>(rubyIndex)].x = px;
                    rubies[static_cast<size_t>(rubyIndex)].y = py;
                }
            } else {
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                TextChar ch;
                ch.code = rubyText[k];
                ch.ruby = true;
                ch.italic = italic;
                ch.x = px, ch.y = py;
                ch.cellWidth = rg.cellWidth;
                ch.cellHeight = rg.cellHeight;
                ch.adjust = rg.adjust;
                ch.overhang = rg.overhang;
                ch.shown = ch.typingStart = starts[k];
                ch.image = rg.image;
                pool.push_back(static_cast<uint32_t>(rubies.size()) | kRubyBit);
                rubies.push_back(std::move(ch));
            }
            ++rubyIndex;
        }
    };

    for (const auto& line : *lines_) {
        for (const auto& tok : line) {
            const std::string& name = tok.name;
            if (name == "RUBY") {
                rubyText.clear();
                for (const auto& a : tok.attributes) {
                    if (a.first != "TEXT") continue;
                    rubyText = utf8ToUtf16(a.second);
                    rubyStart = charIndex;
                    break;
                }
            } else if (name == "/RUBY") {
                placeRuby();
                rubyText.clear();
            } else if (name == "I") {
                italic = true;
            } else if (name == "/I") {
                italic = false;
            } else if (name == "CENTER") {
                centerX = x;
                centerChar = charIndex;
                center = true;
                centerRuby = rubyIndex;
            } else if (name == "/CENTER") {
                center = false;
                if (!finishCenter()) finishCenter();
                centerChar = centerRuby = -1;
                if (!chars.empty() && charIndex > 0) {
                    const TextChar& last = chars[static_cast<size_t>(charIndex - 1)];
                    TextGlyph g = main.glyph(last.code, last.italic);
                    x = truncate(static_cast<float>(last.cellWidth + g.adjust) + last.x);
                }
            }
            if (tok.text.empty()) continue;
            std::u16string w = utf8ToUtf16(tok.text);
            const int len = static_cast<int>(w.size());
            for (int i = 0; i < len; ++i, ++n) {
                uint32_t c = w[static_cast<size_t>(i)];
                if (c == '\t') continue;
                if (c == '\r' || c == '\n') {
                    if (finishCenter()) finishCenter();
                    newline();
                    uint32_t d = i + 1 < len ? w[static_cast<size_t>(i + 1)] : 0;
                    if (d == '\r' || d == '\n') ++i;
                    continue;
                }
                // Upright after italic: the italic character's adjustment comes off.
                if (!italic && x > 0 && charIndex > 0) {
                    const TextChar& prev = chars[static_cast<size_t>(charIndex - 1)];
                    if (prev.italic) x -= main.glyph(prev.code, prev.italic).adjust;
                }
                TextGlyph g = main.glyph(c, italic);
                lineHeight = std::max(lineHeight, g.cellHeight);
                if (noPrintPitch == 0) x += ((c & 0xff00) != 0 && lastWide) ? s.pitchFull : s.pitchHalf;
                const int adv = g.overhang + g.adjust + g.cellWidth;
                noPrintPitch = 0;
                // Not at a line's end: to the next line when a full-width character would not fit.
                if (s.pitchFull + g.cellWidth + adv + x > s.fieldWidth && contains(breaks.endOfLine, c)) x = s.fieldWidth;
                const bool small = !older && contains(breaks.smallKana, c);
                const bool mayHang = contains(breaks.headOfLine, c) || small;
                if (older && adv + x > s.fieldWidth) {
                    // The older rule (0x4a5b6f): a head-of-line character hangs while fewer than
                    // two have.
                    if (hang < 2 && mayHang) {
                        ++hang;
                    } else {
                        if (finishCenter()) newline();
                        noPrintPitch = 1;
                        hang = 0;
                    }
                } else if (adv + x > s.fieldWidth) {
                    bool breakLine = false;
                    if (hang >= 2 || !mayHang) {
                        breakLine = true;
                    } else if (!small) {
                        ++hang;
                    } else if (hangRun == 0 && hang >= 1) {
                        breakLine = true;
                    } else if (c == lastCode && hangRun > 0) {
                        breakLine = true;
                    } else if (hangRun >= 0) {
                        ++hang;
                    }
                    if (breakLine) {
                        if (finishCenter()) newline();
                        noPrintPitch = 1;
                        hang = 0;
                    }
                }
                x += g.overhang;
                if (relayout) {
                    if (charIndex < static_cast<int>(chars.size())) {
                        chars[static_cast<size_t>(charIndex)].x = static_cast<float>(x);
                        chars[static_cast<size_t>(charIndex)].y = static_cast<float>(y);
                    }
                } else {
                    TextChar ch;
                    ch.code = c;
                    ch.italic = italic;
                    ch.x = static_cast<float>(x);
                    ch.y = static_cast<float>(y);
                    ch.cellWidth = g.cellWidth;
                    ch.cellHeight = g.cellHeight;
                    ch.adjust = g.adjust;
                    ch.overhang = g.overhang;
                    ch.shown = ch.typingStart = static_cast<float>(n) * -48.0f;
                    ch.image = g.image;
                    pool.push_back(static_cast<uint32_t>(chars.size()));
                    chars.push_back(std::move(ch));
                }
                x += g.adjust + g.cellWidth;
                ++charIndex;
                maxX = std::max(maxX, x);
                totalHeight = std::max(totalHeight, y + lineHeight);
                lastWide = (c & 0xff00) != 0;
                lastCode = c;
                hangRun = small ? hangRun + 1 : 0;
            }
        }
    }
    return true;
}

void TextLayout::cornerOffsets(const TextLayoutSettings& s, const std::vector<PositionFixRule>& rules) {
    for (auto& c : chars) c.corner = {};
    for (auto& c : rubies) c.corner = {};
    if (type == 1) {
        for (auto& c : chars) {
            if (!c.small) continue;
            for (auto& k : c.corner) k[1] = static_cast<float>(s.rubyOffset) + k[1];
        }
    } else if (type == 0 && chars.size() > 1) {
        for (size_t i = 0; i + 1 < chars.size(); ++i) {
            TextChar& a = chars[i];
            const TextChar& b = chars[i + 1];
            if (!(a.y == b.y) || a.small != b.small || a.italic != b.italic) continue;
            for (const auto& r : rules) {
                if (r.first != a.code || r.second != b.code) continue;
                a.corner[1] = {static_cast<float>(r.value), 0.0f};
                a.corner[3] = {static_cast<float>(r.value), 0.0f};
                break;
            }
        }
    }
}

// CTextVertical's layout (muramasa_en.exe 0x4ccb30).
bool TextLayout::layoutVertical(const std::string& text, const std::string& full, TextFont& main, TextFont& ruby,
                                const TextLayoutSettings& s, const LineBreakTables& breaks) {
    const bool relayout = text.empty();
    std::vector<TextChar> list, rubyList;  // this call's characters and ruby (EBP-0x48 / EBP-0x88)
    int hang = 0;
    const int mainWidth = main.maxWidth(), rubyWidth = ruby.maxWidth(), rubyHeight = ruby.height();
    if (relayout) {
        noPrintPitch = 1;
        x = y = column = lineHeight = 0;
        totalHeight = maxX = 0;
        lines = 1;
        lastWide = false;
        hangRun = 0;
        lastCode = 0;
    }
    auto lines_ = tokenizeText(relayout ? full : text, true);
    if (!lines_) return false;
    std::u16string rubyText;
    int rubyStart = 0;
    bool italic = false, center = false;
    int centerY = 0, centerChar = -1, centerRuby = -1;
    int n = 0;

    // The next column (0x4cea10).
    auto newline = [&]() {
        y = 0;
        x += s.rowSpace + (column != 0 ? column : mainWidth);
        maxX = x;
        column = 0;
        ++lines;
        lastWide = false;
        hang = 0;
        noPrintPitch = 1;
        hangRun = 0;
        lastCode = 0;
        if (mainWidth + x > s.fieldWidth && s.fieldType == 1) x = 0;
    };
    // Centring down the column since CENTER (0x4cead0); false if it moved the run to a new column.
    auto finishCenter = [&]() -> bool {
        int count = static_cast<int>(list.size());
        int cs = centerChar;
        if (cs < 0 || cs >= count) {
            if (!center) centerChar = centerRuby = -1;
            centerY = 0;
            return true;
        }
        const TextChar& first = list[static_cast<size_t>(cs)];
        const TextChar& last = list.back();
        TextGlyph g1 = main.glyph(first.code, first.italic);
        TextGlyph g2 = main.glyph(last.code, last.italic);
        int pen = y;
        int shift = (s.fieldHeight + static_cast<int>(first.y) - pen - g1.overhang + g2.adjust) / 2 - centerY;
        if (centerY != 0 &&
            static_cast<float>(centerY) > (static_cast<float>(g2.adjust) + first.y) + static_cast<float>(shift)) {
            y = pen - centerY;
            int oldX = x;
            x += s.rowSpace + (column != 0 ? column : mainWidth);
            maxX = x;
            column = 0;
            ++lines;
            hang = 0;
            noPrintPitch = 1;
            float dx = static_cast<float>(x - oldX), dy = static_cast<float>(-centerY);
            for (int k = cs; k < count; ++k) {
                list[static_cast<size_t>(k)].x += dx;
                list[static_cast<size_t>(k)].y += dy;
            }
            for (uint32_t k = static_cast<uint32_t>(centerRuby); k < rubyList.size(); ++k) {
                rubyList[k].x += dx;
                rubyList[k].y += dy;
            }
            centerY = 0;
            return false;
        }
        for (int k = cs; k < count; ++k) list[static_cast<size_t>(k)].y += static_cast<float>(shift);
        // The ruby moves with it where it is in the last ruby character's column.
        if (!rubyList.empty()) {
            float lastX = rubyList.back().x;
            for (uint32_t k = static_cast<uint32_t>(centerRuby); k < rubyList.size(); ++k) {
                if (lastX == rubyList[k].x) rubyList[k].y += static_cast<float>(shift);
            }
        }
        if (center) {
            centerChar = count;
            centerRuby = static_cast<int>(rubyList.size());
        } else {
            centerChar = centerRuby = -1;
        }
        centerY = 0;
        return true;
    };
    // </RUBY> (0x4ccea8 ff.): the ruby text beside the characters since <RUBY>, the ruby font's
    // widest cell to their left (before the turn).
    auto placeRuby = [&]() {
        const size_t len = rubyText.size();
        if (len == 0) return;
        int count = static_cast<int>(list.size());
        if (rubyStart < 0 || rubyStart >= count) return;
        std::vector<std::array<int32_t, 2>> places;
        std::vector<float> starts;
        if (static_cast<int64_t>(len) == static_cast<int64_t>(count - rubyStart)) {
            for (size_t k = 0; k < len; ++k) {
                const TextChar& b = list[static_cast<size_t>(rubyStart) + k];
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                TextGlyph bg = main.glyph(b.code, italic);
                int bh = b.cellHeight - bg.overhang + bg.adjust;
                int rh = rg.adjust - rg.overhang + rg.cellHeight;
                float fy = (static_cast<float>(bh) - static_cast<float>(rh)) * 0.5f + b.y + 0.5f;
                float fx = b.x - static_cast<float>(rubyWidth);
                places.push_back({truncate(fx), truncate(fy)});
                starts.push_back(b.shown);
            }
        } else {
            const TextChar& first = list[static_cast<size_t>(rubyStart)];
            float firstShown = first.shown;
            float span = list.back().shown;
            int colX = truncate(first.x);
            float colXf = first.x, top = first.y, colStart = first.y, prevBottom = first.y, sum = 0.0f, maxBottom = 0.0f;
            bool multi = false;
            for (int k = rubyStart; k < count; ++k) {
                const TextChar& b = list[static_cast<size_t>(k)];
                int bx = truncate(b.x);
                if (colX != bx) {
                    prevBottom = prevBottom - colStart;
                    multi = true;
                    colStart = b.y;
                    colX = bx;
                    sum = sum + prevBottom;
                }
                float bottom = static_cast<float>(b.cellHeight) + b.y;
                bottom = bottom + static_cast<float>(b.overhang);
                if (bottom > maxBottom) maxBottom = bottom;
                prevBottom = bottom;
            }
            float baseHeight = (prevBottom - colStart) + sum;
            float rubyTotal = 0.0f;
            for (size_t k = 0; k < len; ++k) {
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                rubyTotal = (static_cast<float>(rg.overhang + rg.adjust) + static_cast<float>(rg.cellHeight)) + rubyTotal;
            }
            float spacing = 0.0f;
            if (!(baseHeight < rubyTotal) && !std::isnan(baseHeight) && !std::isnan(rubyTotal)) {
                float target = baseHeight - s.rubyMarginRate * baseHeight;
                float half = baseHeight * 0.5f;
                target = target > rubyTotal ? target : rubyTotal;  // maxss
                float gaps = unsignedToFloat(static_cast<uint32_t>(len - 1));
                float inv = 1.0f / gaps;
                float least = static_cast<float>(rubyHeight) * -0.25f;
                float a = (target - rubyTotal) * inv;
                float b = half * inv;
                spacing = least;
                if (!(least > a)) spacing = a < b ? a : b;  // minss(a, b)
                rubyTotal = rubyTotal + spacing * gaps;
            }
            float startY = top - (rubyTotal - baseHeight) * 0.5f;
            span = span - firstShown;
            for (size_t k = 0; k < len; ++k) {
                TextGlyph rg = ruby.glyph(rubyText[k], italic);
                if (multi && !(static_cast<float>(rg.cellHeight / 2) + startY < maxBottom) && !std::isnan(startY) &&
                    !std::isnan(maxBottom)) {
                    startY = startY - maxBottom;
                    colXf = static_cast<float>(s.rowSpace + (column != 0 ? column : mainWidth)) + colXf;
                }
                places.push_back({truncate(colXf - static_cast<float>(rubyWidth)), truncate(startY)});
                float t = 0.5f;
                if (len > 1) t = unsignedToFloat(static_cast<uint32_t>(k)) / unsignedToFloat(static_cast<uint32_t>(len - 1));
                starts.push_back(span * t + firstShown);
                float step = static_cast<float>(rg.cellHeight) + spacing;
                step = step + static_cast<float>(rg.adjust);
                step = step + static_cast<float>(rg.overhang);
                startY = step + startY;
            }
        }
        for (size_t k = 0; k < len && k < places.size(); ++k) {
            TextGlyph rg = ruby.glyph(rubyText[k], italic);
            TextChar ch;
            ch.code = rubyText[k];
            ch.ruby = true;
            ch.italic = italic;
            ch.x = static_cast<float>(places[k][0]);
            ch.y = static_cast<float>(places[k][1]);
            ch.cellWidth = rg.cellWidth;
            ch.cellHeight = rg.cellHeight;
            ch.shown = ch.typingStart = starts[k];
            ch.image = rg.image;
            rubyList.push_back(std::move(ch));
        }
    };

    for (const auto& line : *lines_) {
        for (const auto& tok : line) {
            const std::string& name = tok.name;
            if (name == "RUBY") {
                rubyText.clear();
                for (const auto& a : tok.attributes) {
                    if (a.first != "TEXT") continue;
                    rubyText = utf8ToUtf16(a.second);
                    rubyStart = static_cast<int>(list.size());
                    break;
                }
            } else if (name == "/RUBY") {
                placeRuby();
                rubyText.clear();
            } else if (name == "I") {
                italic = true;
            } else if (name == "/I") {
                italic = false;
            } else if (name == "CENTER") {
                center = true;
                centerY = y;
                centerChar = static_cast<int>(list.size());
                centerRuby = static_cast<int>(rubyList.size());
            } else if (name == "/CENTER") {
                center = false;
                if (!finishCenter()) finishCenter();
                centerChar = centerRuby = -1;
                if (!list.empty()) {
                    const TextChar& last = list.back();
                    TextGlyph g = main.glyph(last.code, last.italic);
                    y = truncate(last.y) + last.cellHeight + g.adjust;
                }
            }
            if (tok.text.empty()) continue;
            std::u16string w = utf8ToUtf16(tok.text);
            const int len = static_cast<int>(w.size());
            for (int i = 0; i < len; ++i, ++n) {
                uint32_t c = w[static_cast<size_t>(i)];
                if (c == '\t') continue;
                // (A wide character is marked before its own pitch, so it always takes the
                // full-width one; nothing clears the mark but the next column.)
                if (c & 0xff00) lastWide = true;
                if (c == '\r' || c == '\n') {
                    if (finishCenter()) finishCenter();
                    newline();
                    uint32_t d = i + 1 < len ? w[static_cast<size_t>(i + 1)] : 0;
                    if (d == '\r' || d == '\n') ++i;
                    continue;
                }
                if (!italic && y > 0 && !list.empty() && list.back().italic) {
                    const TextChar& prev = list.back();
                    y -= main.glyph(prev.code, prev.italic).adjust;
                }
                TextGlyph g = main.glyph(c, italic);
                column = std::max(column, g.cellWidth);
                if (noPrintPitch == 0) y += ((c & 0xff00) != 0 && lastWide) ? s.pitchFull : s.pitchHalf;
                const int adv = g.adjust + g.cellHeight + g.overhang;
                if (s.pitchFull + g.cellHeight + adv + y > s.fieldHeight && contains(breaks.endOfLine, c)) y = s.fieldHeight;
                noPrintPitch = 0;
                const bool small = !older && contains(breaks.smallKana, c);
                const bool mayHang = contains(breaks.headOfLine, c) || small;
                if (adv + y > s.fieldHeight) {
                    bool breakLine = false;
                    if (hang >= 2 || !mayHang) {
                        breakLine = true;
                    } else if (!small) {
                        ++hang;
                    } else if (hangRun == 0 && hang >= 1) {
                        breakLine = true;
                    } else if (c == lastCode && hangRun > 0) {
                        breakLine = true;
                    } else if (hangRun >= 0) {
                        ++hang;
                    }
                    if (breakLine) {
                        if (finishCenter()) newline();
                        noPrintPitch = 1;
                        hang = 0;
                    }
                }
                y += g.overhang;
                TextChar ch;
                ch.code = c;
                ch.italic = italic;
                ch.x = static_cast<float>(x);
                ch.y = static_cast<float>(y);
                ch.cellWidth = g.cellWidth;
                ch.cellHeight = g.cellHeight;
                ch.adjust = g.adjust;
                ch.overhang = g.overhang;
                ch.shown = ch.typingStart = static_cast<float>(n) * -48.0f;
                ch.image = g.image;
                y += g.cellHeight + g.adjust;
                noPrintPitch = 0;
                list.push_back(std::move(ch));
                lastCode = c;
                totalHeight = std::max(totalHeight, y);
                maxX = std::max(maxX, x + column);
                hangRun = small ? hangRun + 1 : 0;
            }
        }
    }
    // Right to left: each centred on the list's mean cell width (0x4cde58 ff.); then made, or their
    // places given to the characters laid out before (a relayout).
    auto turn = [&s](std::vector<TextChar>& v) {
        if (v.empty()) return;
        float sum = 0.0f;
        for (const auto& c : v) sum = sum + static_cast<float>(c.cellWidth);
        float mean = sum / unsignedToFloat(static_cast<uint32_t>(v.size()));
        for (auto& c : v) {
            float cw = static_cast<float>(c.cellWidth);
            float nx = static_cast<float>(s.fieldWidth) - c.x;
            float off = mean - cw;
            nx = nx - cw;
            off = off * 0.5f;
            c.x = nx - off;
        }
    };
    turn(list);
    turn(rubyList);
    if (relayout) {
        for (size_t k = 0; k < list.size() && k < chars.size(); ++k) chars[k].x = list[k].x, chars[k].y = list[k].y;
        for (size_t k = 0; k < rubyList.size() && k < rubies.size(); ++k) rubies[k].x = rubyList[k].x, rubies[k].y = rubyList[k].y;
    } else {
        for (auto& c : list) {
            pool.push_back(static_cast<uint32_t>(chars.size()));
            chars.push_back(std::move(c));
        }
        for (auto& c : rubyList) {
            pool.push_back(static_cast<uint32_t>(rubies.size()) | kRubyBit);
            rubies.push_back(std::move(c));
        }
    }
    return true;
}

// --- The older text code: CTextTyping (SQApp.loadTextTexture) -------------------------------------

namespace {

// A laid-out character (0x4c bytes: the font's 0x48-byte glyph, [5] / [6] the place, and the font).
struct TypingGlyph {
    std::shared_ptr<const GlyphImage> g;
    const FontInstance* font = nullptr;
    int x = 0, y = 0;  // [5] / [6]: the cell's top-left
    int cellW() const { return g->cellWidth; }   // [7]
    int cellH() const { return g->cellHeight; }  // [8]
    int adjust() const { return g->adjust; }     // [9]
    int overhang() const { return g->overhang; } // [10]
    uint16_t code() const { return static_cast<uint16_t>(g->codepoint); }  // [11]
};

// One of the four fonts (+0x24 main, +0x28 ruby, +0x2c / +0x30 their italic ones).
struct TypingFont {
    std::shared_ptr<FontInstance> f;
    bool italic = false;
    TypingGlyph glyph(uint16_t c) const {  // vtable +0x10, antialiased
        TypingGlyph t;
        t.g = f->glyph(c, italic);
        t.font = f.get();
        return t;
    }
    std::array<int, 2> cell() const { return f->cell(); }  // vtable +0xc
};

// The object (0x16c bytes on the native's stack; its fields from +4 are the states' context).
struct Typing {
    int penX = 0, penY = 0;                  // +4 / +8
    int areaX = 0, areaY = 0, areaW = 0, areaH = 0;  // +0xc ..
    int extW = 0, extH = 0;                  // +0x1c / +0x20: the laid-out size
    TypingFont main, ruby, mainItalic, rubyItalic;
    uint16_t prev = 0;                       // +0x34: the last character (0 at a line's start)
    int rowSpace = 0, pitchFull = 0, pitchHalf = 0, rubyOffset = 0;
    float rate = 0.0f;
    int hang = 0;                            // +0x4c: characters hung past the line's end
    int run = 0;                             // +0x54: read by the hanging rule; nothing sets it
    int italic = 0;                          // +0x58
    std::vector<TypingGlyph> glyphs;         // +0x64
    struct Ruby {
        int start = 0, end = 0;              // the base characters
        std::vector<TypingGlyph> glyphs;
    };
    std::vector<Ruby> rubies;                // +0x70
    const LineBreakTables* breaks = nullptr;
    const std::function<void(const std::string&)>* log = nullptr;

    const TypingFont& font() const { return italic ? mainItalic : main; }
    // The pitch before c: none at a line's start, the half-width one only between two Latin-1
    // characters.
    int pitch(uint16_t c) const {
        if (prev == 0) return 0;
        if ((prev & 0xff00) != 0 || (c & 0xff00) != 0) return pitchFull;
        return pitchHalf;
    }
    void note(const std::string& what) const {
        if (log && *log) (*log)("loadTextTexture: " + what);
    }
    void resetLine() {
        prev = 0;
        hang = 0;
        run = 0;
    }
    void newline(const TypingFont& f) {  // a horizontal line feed
        penX = 0;
        penY += rowSpace + f.cell()[1];
        resetLine();
    }
    void newColumn(const TypingFont& f) {  // a vertical one: columns go left
        penY = 0;
        penX -= rowSpace + f.cell()[0];
        resetLine();
    }
    // 0x562d40 (and its vertical copy in 0x562e00): a break before a character that would cross the
    // field; a last character that may not end a line moves to the new one.
    void breakLine(const TypingFont& f, bool vertical) {
        if (vertical) {
            penY = 0;
            penX -= rowSpace + f.cell()[0];
        } else {
            penX = 0;
            penY += rowSpace + f.cell()[1];
        }
        if (!glyphs.empty() && contains(breaks->endOfLine, prev)) {
            TypingGlyph& last = glyphs.back();
            last.x = penX;
            last.y = penY;
            if (vertical) penY += last.overhang() + last.adjust() + last.cellH();
            else penX += last.overhang() + last.adjust() + last.cellW();
        }
        resetLine();
    }
    // The hanging rule (0x562b5a ff.) for a character that would cross the field: true to break.
    bool mustBreak(uint16_t c) {
        const bool small = contains(breaks->smallKana, c);
        const bool mayHang = small || contains(breaks->headOfLine, c);
        if (hang >= 2 || !mayHang) return true;
        if (small) {
            if (run == 0 && hang >= 1) return true;
            if (c == prev && run > 0) return true;
            if (run < 0) return false;
        }
        ++hang;
        return false;
    }
};

// CTextTyping::CState: vtable +4 lays out text, +8 ends the state, +0xc / +0x10 resume / suspend
// it around a nested one.
struct TypingState {
    explicit TypingState(Typing& t) : t(t) {}
    virtual ~TypingState() = default;
    virtual void addText(const std::u16string& s) = 0;
    virtual void finish() {}
    virtual void resume() {}
    virtual void suspend() {}
    Typing& t;
};

// CState_Base (0x562990): character by character, a line ending at the field's width.
struct BaseState : TypingState {
    using TypingState::TypingState;
    void addText(const std::u16string& s) override {
        const TypingFont& f = t.font();
        for (char16_t c : s) {
            if (c == 9 || c == 13) continue;
            if (c == 10) {
                t.newline(f);
                continue;
            }
            t.penX += t.pitch(c);
            TypingGlyph g = f.glyph(c);
            const int adv = g.adjust() + g.overhang() + g.cellW();
            if (t.penX + adv > t.areaW && t.mustBreak(c)) t.breakLine(f, false);
            g.x = t.penX + g.overhang();
            g.y = t.penY;
            t.glyphs.push_back(g);
            t.penX += adv;
            t.extW = std::max(t.extW, t.penX);
            t.extH = std::max(t.extH, t.penY + f.cell()[1]);
            t.prev = c;
        }
    }
};

// CState_BaseVertical (0x562e00): the same down columns (x is the column's right edge until the
// drawing adds the width).
struct BaseVerticalState : TypingState {
    using TypingState::TypingState;
    void addText(const std::u16string& s) override {
        const TypingFont& f = t.font();
        for (char16_t c : s) {
            if (c == 9 || c == 13) continue;
            if (c == 10) {
                t.newColumn(f);
                continue;
            }
            t.penY += t.pitch(c);
            TypingGlyph g = f.glyph(c);
            const int adv = g.cellH() + g.adjust() + g.overhang();
            if (t.penY + adv > t.areaH && t.mustBreak(c)) t.breakLine(f, true);
            g.y = t.penY + g.overhang();
            g.x = t.penX - t.main.cell()[0];
            t.glyphs.push_back(g);
            t.penY += adv;
            t.extH = std::max(t.extH, t.penY);
            t.extW = std::max(t.extW, std::abs(t.penX - t.main.cell()[0]));
            t.prev = c;
        }
    }
};

// CState_BaseWardwrap (0x563370, a word's flush 0x563580): words gathered up to a space, each
// placed whole on the line or the next. The line's advance (+0x18) counts no pitches, the word's
// width (+0x14) does.
struct WordWrapState : TypingState {
    explicit WordWrapState(Typing& t) : TypingState(t), lineX(t.penX) {}
    std::vector<TypingGlyph> word;  // +8
    int wordW = 0;                  // +0x14
    int lineX = 0;                  // +0x18
    bool noted = false;
    void addText(const std::u16string& s) override {
        for (char16_t c : s) {
            if (c == 9 || c == 13) continue;
            if (c == 10) {
                flush();
                const TypingFont& f = t.font();
                t.penX = 0;
                t.penY += t.rowSpace + f.cell()[1];
                t.prev = 0;
                t.hang = 0;
                lineX = 0;
                continue;
            }
            TypingGlyph g = t.font().glyph(c);
            const int pitch = t.pitch(c);
            const int adv = g.adjust() + g.overhang() + g.cellW();
            g.x = wordW + g.overhang() + pitch;
            lineX += adv;
            wordW = adv + wordW + pitch;
            word.push_back(g);
            if (c == 0x20) flush();
            t.prev = c;
        }
    }
    void finish() override { flush(); }
    void suspend() override { flush(); }
    void resume() override { lineX = t.penX; }
    void flush() {
        const TypingFont& f = t.font();
        if (lineX > t.areaW) {
            if (t.penX != 0) {
                t.penX = 0;
                t.penY += t.rowSpace + f.cell()[1];
                t.prev = 0;
                t.hang = 0;
                lineX = 0;
            }
            lineX = wordW;
        }
        int shift = 0;
        for (TypingGlyph& g : word) {
            if (g.cellW() + g.overhang() + g.x + t.penX + g.adjust() + shift > t.areaW) {
                // The exe flushes the same word again here, with nothing changed, and so on until
                // its stack overflows (a word wider than the field crashes the PC game). Nyanroid
                // goes on as the code after that call does (Guess).
                if (!noted) t.note("a word wider than the field (the exe recurses until it crashes)");
                noted = true;
                t.penX = 0;
                t.penY += t.rowSpace + f.cell()[1];
                t.prev = 0;
                t.hang = 0;
                lineX = 0;
                shift = -g.x;
            }
            g.x = t.penX + g.x + shift;
            g.y = t.penY;
        }
        if (shift != 0) {
            const TypingGlyph& last = word.back();
            t.penX = last.x + last.overhang() + last.adjust() + last.cellW();
        } else {
            t.penX = lineX;
        }
        t.extW = std::max(t.extW, t.penX);
        t.extH = std::max(t.extH, t.penY + f.cell()[1]);
        t.glyphs.insert(t.glyphs.end(), word.begin(), word.end());
        word.clear();
        wordW = 0;
    }
};

// The CENTER states: characters gathered from where the tag was met, then centred line by line
// (CState_Cneter 0x563b60 / 0x5637b0, CState_CneterVertical 0x5640b0 / 0x563ce0,
// CState_CneterWardwrap 0x564640 / 0x564230). `pitches[k]` is the pitch after the line's k-th
// character; the line's width loses the last one gathered.
struct CenterState : TypingState {
    enum class Kind { Characters, Vertical, Words };
    CenterState(Typing& t, Kind kind) : TypingState(t), kind(kind), start(static_cast<int>(t.glyphs.size())) {}
    Kind kind;
    int start;  // [2]: the first character not yet centred
    bool noted = false;
    void addText(const std::u16string& s) override {
        for (char16_t c : s) {
            if (c == 9 || c == 13) continue;
            const TypingFont& f = t.font();
            if (c == 10) {
                centre();
                if (kind == Kind::Vertical) t.newColumn(f);
                else t.newline(f);
                continue;
            }
            t.glyphs.push_back(f.glyph(c));
        }
    }
    void finish() override { centre(); }
    int along(const TypingGlyph& g) const { return kind == Kind::Vertical ? g.cellH() : g.cellW(); }
    void centre() {
        const TypingFont& f = t.font();
        const bool vertical = kind == Kind::Vertical;
        const int field = vertical ? t.areaH : t.areaW;
        while (start < static_cast<int>(t.glyphs.size())) {
            std::vector<int> pitches;
            int lineW = 0, i = start;
            const int count = static_cast<int>(t.glyphs.size());
            if (kind != Kind::Words) {
                for (; i < count; ++i) {
                    const TypingGlyph& g = t.glyphs[static_cast<size_t>(i)];
                    const uint16_t c = g.code();
                    if (lineW > 0) {
                        const int p = t.pitch(c);
                        lineW += p;
                        pitches.push_back(p);
                    }
                    const int w = g.overhang() + g.adjust() + along(g) + lineW;
                    if (w > field) break;
                    lineW = w;
                    t.prev = c;
                }
                if (i == start) {
                    // The exe takes no character and starts the same line again, for ever (a
                    // character longer than the field hangs the PC game). Nyanroid sets it alone (Guess).
                    if (!noted) t.note("a character longer than the field in CENTER (the exe never returns)");
                    noted = true;
                    const TypingGlyph& g = t.glyphs[static_cast<size_t>(i)];
                    pitches.clear();
                    lineW = g.overhang() + g.adjust() + along(g);
                    t.prev = g.code();
                    ++i;
                }
            } else {
                int wordW = 0, lastSpace = start;
                bool over = false;
                for (; i < count; ++i) {
                    const TypingGlyph& g = t.glyphs[static_cast<size_t>(i)];
                    const uint16_t c = g.code();
                    if (wordW > 0 || lineW > 0) {
                        const int p = t.pitch(c);
                        wordW += p;
                        pitches.push_back(p);
                    }
                    wordW += g.overhang() + g.adjust() + g.cellW();
                    t.prev = c;
                    if (c == 0x20) {
                        if (lineW + wordW > field) {
                            over = true;
                            break;
                        }
                        lineW += wordW;
                        wordW = 0;
                        lastSpace = i;
                    }
                }
                if (over) {
                    // Back to the last space: the exe adds that space's cell less its corrections
                    // and the last pitch (meaning, it seems, to take the space off).
                    if (lastSpace > 0) {
                        const TypingGlyph& sp = t.glyphs[static_cast<size_t>(lastSpace)];
                        if (sp.code() == 0x20) {
                            int popped = 0;
                            if (!pitches.empty()) {
                                popped = pitches.back();
                                pitches.pop_back();
                            }
                            lineW += sp.cellW() - sp.overhang() - sp.adjust() - popped;
                        }
                    }
                    i = lastSpace + 1;
                    wordW = 0;
                }
                if (wordW > 0) lineW += wordW;
            }
            if (!pitches.empty()) lineW -= pitches.back();
            pitches.push_back(0);
            const int at = (field - lineW) / 2;
            if (vertical) {
                if (t.penY > at) t.newColumn(f);
                t.penY = at;
            } else {
                if (t.penX > at) t.newline(f);
                t.penX = at;
            }
            for (int j = start; j < i; ++j) {
                TypingGlyph& g = t.glyphs[static_cast<size_t>(j)];
                // (A character of no width leaves fewer pitches than characters: the exe reads past
                // them.)
                const size_t k = static_cast<size_t>(j - start);
                const int p = k < pitches.size() ? pitches[k] : 0;
                if (vertical) {
                    g.y = t.penY + g.overhang();
                    g.x = t.penX - f.cell()[0];
                    t.penY += p + g.adjust() + g.cellH() + g.overhang();
                } else {
                    g.x = t.penX + g.overhang();
                    g.y = t.penY;
                    t.penX += p + g.adjust() + g.cellW() + g.overhang();
                }
            }
            if (vertical) {
                t.extH = std::max(t.extH, t.penY);
                t.extW = std::max(t.extW, std::abs(t.penX - t.main.cell()[0]));
            } else {
                t.extW = std::max(t.extW, t.penX);
                t.extH = std::max(t.extH, t.penY + f.cell()[1]);
            }
            start = i;
        }
    }
};

float minss(float a, float b) { return a < b ? a : b; }
float maxss(float a, float b) { return a > b ? a : b; }

// The ruby (0x561130's last part): over each base run, one ruby character per base character
// centred over it, or the ruby spread over the run (and on to the next line where the run
// wrapped); offset by the ruby offset beside the text.
void placeRuby(Typing& t, bool vertical) {
    for (Typing::Ruby& r : t.rubies) {
        const int n = static_cast<int>(r.glyphs.size());
        if (r.end - r.start == n) {
            for (int k = 0; k < n; ++k) {
                const TypingGlyph& b = t.glyphs[static_cast<size_t>(r.start + k)];
                TypingGlyph& g = r.glyphs[static_cast<size_t>(k)];
                if (vertical) {
                    g.y = truncate((static_cast<float>(b.adjust() - b.overhang() + b.cellH()) -
                                    static_cast<float>(g.adjust() - g.overhang() + g.cellH())) * 0.5f +
                                   static_cast<float>(b.y) + 0.5f);
                    g.x = truncate(((static_cast<float>(b.x + t.main.cell()[0]) - static_cast<float>(g.font->cell()[0])) +
                                    static_cast<float>(t.rubyOffset)) +
                                   static_cast<float>(b.cellW()) * 0.25f + 0.5f);
                } else {
                    g.x = truncate((static_cast<float>(b.adjust() - b.overhang() + b.cellW()) -
                                    static_cast<float>(g.adjust() - g.overhang() + g.cellW())) * 0.5f +
                                   static_cast<float>(b.x) + 0.5f);
                    g.y = truncate(static_cast<float>(b.cellH()) * 0.25f +
                                   (static_cast<float>(t.rubyOffset) + (static_cast<float>(b.y) - static_cast<float>(g.font->cell()[1]))) +
                                   0.5f);
                }
            }
            continue;
        }
        // Spread: the run's length over its lines, the ruby's own, and the spacing between ruby
        // characters (at most half the run over n - 1, the run shrunk by the margin rate less the
        // ruby over n - 1; the exe's float division by n - 1 gives a lone ruby character a NaN place).
        const TypingGlyph& b0 = t.glyphs[static_cast<size_t>(r.start)];
        int lineAt = vertical ? b0.x : b0.y;
        float across = vertical ? static_cast<float>(b0.cellW()) * 0.25f + static_cast<float>(b0.x) + 0.5f +
                                      static_cast<float>(t.rubyOffset)
                                : static_cast<float>(b0.cellH()) * 0.25f + static_cast<float>(b0.y) + 0.5f +
                                      static_cast<float>(t.rubyOffset);
        float segStart = static_cast<float>(vertical ? b0.y : b0.x);
        float segEnd = segStart, done = 0.0f, maxEnd = 0.0f;
        bool wrapped = false;
        for (int k = r.start; k < r.end; ++k) {
            const TypingGlyph& g = t.glyphs[static_cast<size_t>(k)];
            const int line = vertical ? g.x : g.y;
            if (vertical ? lineAt > line : lineAt < line) {
                segEnd -= segStart;
                wrapped = true;
                segStart = static_cast<float>(vertical ? g.y : g.x);
                done += segEnd;
                lineAt = line;
            }
            segEnd = static_cast<float>(vertical ? g.overhang() + g.cellH() + g.y : g.overhang() + g.x + g.cellW());
            maxEnd = maxss(maxEnd, segEnd);
        }
        const float baseLen = (segEnd - segStart) + done;
        float rubyLen = 0.0f;
        for (const TypingGlyph& g : r.glyphs) rubyLen += static_cast<float>(g.overhang() + g.adjust() + (vertical ? g.cellH() : g.cellW()));
        float spacing = 0.0f, total = rubyLen;
        if (baseLen >= rubyLen) {
            // (Separate statements: no fused multiply-add, as the exe's SSE code has none.)
            const float margin = t.rate * baseLen;
            const float room = maxss(baseLen - margin, rubyLen);
            const float gaps = unsignedToFloat(static_cast<uint32_t>(n - 1));
            const float even = (room - rubyLen) / gaps;
            const auto rubyCell = t.ruby.cell();
            spacing = static_cast<float>(vertical ? rubyCell[1] : rubyCell[0]) * -0.25f;
            if (!(spacing > even)) spacing = minss(baseLen * 0.5f / gaps, even);
            const float gapsLen = gaps * spacing;
            total = gapsLen + rubyLen;
        }
        float along = static_cast<float>(vertical ? b0.y : b0.x) - (total - baseLen) * 0.5f;
        for (TypingGlyph& g : r.glyphs) {
            const int len = vertical ? g.cellH() : g.cellW();
            if (wrapped && static_cast<float>(len / 2) + along >= maxEnd) {
                along -= maxEnd;
                if (vertical) across -= static_cast<float>(t.rowSpace + t.main.cell()[0]);
                else across += static_cast<float>(t.rowSpace + t.main.cell()[1]);
            }
            if (vertical) {
                g.y = truncate(along) + g.overhang();
                g.x = t.main.cell()[0] + (truncate(across) - g.font->cell()[0]);
            } else {
                g.x = truncate(along) + g.overhang();
                g.y = truncate(across) - g.font->cell()[1];
            }
            along = static_cast<float>(len) + spacing + static_cast<float>(g.overhang()) + static_cast<float>(g.adjust()) + along;
        }
    }
}

// 0x562810: a quarter of the outline shadow (shadow type 15): each texel's alpha the sum of the
// alphas within the radius (the shadow offset, 3 at most), weighted by their distance |dx| + |dy|
// (256, 240, 196, 128, 64, 32, 16 / 256), 255 at most, times the shadow's alpha / 256.
void outlineRows(uint32_t* dst, const uint32_t* src, int rowStart, int rowEnd, int width, int height, int offset,
                 uint32_t shadowAlpha) {
    static const int kWeight[7] = {0x100, 0xf0, 0xc4, 0x80, 0x40, 0x20, 0x10};
    const int r = offset < 3 ? offset : 3;
    const uint32_t power = shadowAlpha >> 24;
    for (int y = rowStart; y < rowEnd; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t sum = 0;
            for (int dy = -r; dy <= r; ++dy) {
                if (y + dy < 0 || y + dy >= height) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    if (x + dx < 0 || x + dx >= width) continue;
                    const uint32_t a = src[static_cast<size_t>(y + dy) * static_cast<size_t>(width) + static_cast<size_t>(x + dx)] >> 24;
                    sum += a * static_cast<uint32_t>(kWeight[std::abs(dx) + std::abs(dy)]) >> 8;
                }
            }
            if (sum & 0xff00) sum = 0xff;
            uint32_t& d = dst[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            d = (d & 0xffffffu) | ((sum * power >> 8) & 0xffu) << 24;
        }
    }
}

}  // namespace

Image renderTextTexture(const std::string& text, const TextTextureSettings& s, const TypingFonts& fonts,
                        const LineBreakTables& breaks, const std::function<void(const std::string&)>& log) {
    const bool vertical = s.typingType == 2;
    Typing t;
    t.breaks = &breaks;
    t.log = &log;
    // 0x561030 and the native's own settings, then 0x5610d0: the pen starts a row space down (to
    // the left in vertical text).
    t.areaX = s.area[0], t.areaY = s.area[1], t.areaW = s.area[2], t.areaH = s.area[3];
    t.rowSpace = s.rowSpace, t.pitchFull = s.pitchFull, t.pitchHalf = s.pitchHalf, t.rubyOffset = s.rubyOffset;
    t.rate = s.rubyMargineRate;
    if (vertical) {
        t.penY = 0;
        t.penX = -std::max(s.rowSpace, 0);
    } else {
        t.penX = 0;
        t.penY = std::max(s.rowSpace, 0);
    }
    // The layout (0x561130).
    auto mainFont = fonts(s.textSize), rubyFont = fonts(s.rubySize);
    auto lines = mainFont && rubyFont ? tokenizeText(text, true) : std::nullopt;
    if (lines) {
        t.main = {mainFont, false};
        t.mainItalic = {mainFont, true};
        t.ruby = {rubyFont, false};
        t.rubyItalic = {rubyFont, true};
        std::vector<std::unique_ptr<TypingState>> states;
        if (s.typingType == 1) states.push_back(std::make_unique<WordWrapState>(t));
        else if (vertical) states.push_back(std::make_unique<BaseVerticalState>(t));
        else states.push_back(std::make_unique<BaseState>(t));
        const CenterState::Kind centerKind = s.typingType == 1 ? CenterState::Kind::Words
                                             : vertical         ? CenterState::Kind::Vertical
                                                                : CenterState::Kind::Characters;
        for (const TextLine& line : *lines) {
            for (const TextToken& tok : line) {
                const std::string& name = tok.name;
                if (name == "RUBY") {
                    for (const auto& a : tok.attributes) {
                        if (a.first != "TEXT") continue;
                        Typing::Ruby r;
                        r.start = r.end = static_cast<int>(t.glyphs.size());
                        const TypingFont& f = t.italic ? t.rubyItalic : t.ruby;
                        for (char16_t c : utf8ToUtf16(a.second)) {
                            if (c != 9 && c != 13 && c != 10) r.glyphs.push_back(f.glyph(c));
                        }
                        t.rubies.push_back(std::move(r));
                        break;
                    }
                } else if (name == "/RUBY") {
                    // (Without a RUBY before it the exe writes before its ruby list.)
                    if (!t.rubies.empty()) t.rubies.back().end = static_cast<int>(t.glyphs.size());
                } else if (name == "I") {
                    t.italic = 1;
                } else if (name == "/I") {
                    t.italic = 0;
                } else if (name == "CENTER") {
                    states.back()->suspend();
                    states.push_back(std::make_unique<CenterState>(t, centerKind));
                    states.back()->resume();
                } else if (name == "/CENTER") {
                    if (states.size() > 1) {
                        states.back()->suspend();
                        states.back()->finish();
                        states.pop_back();
                        states.back()->resume();
                    } else {
                        // The exe ends its base state and resumes one from before its list.
                        t.note("/CENTER without CENTER (the exe reads outside its state list)");
                    }
                }
                if (!tok.text.empty()) states.back()->addText(utf8ToUtf16(tok.text));
            }
        }
        for (auto it = states.rbegin(); it != states.rend(); ++it) (*it)->finish();
        placeRuby(t, vertical);
        for (const Typing::Ruby& r : t.rubies) {
            for (const TypingGlyph& g : r.glyphs) {
                if (vertical) t.extH = std::max(t.extH, g.cellH() + g.y);
                else t.extW = std::max(t.extW, g.cellW() + g.x);
            }
        }
        // The typing area's x and y are margins: twice into the size, once into each place.
        t.extW += t.areaX * 2;
        t.extH += t.areaY * 2;
        auto shift = [&](TypingGlyph& g) {
            g.x += vertical ? -t.areaX : t.areaX;
            g.y += t.areaY;
        };
        for (TypingGlyph& g : t.glyphs) shift(g);
        for (Typing::Ruby& r : t.rubies) {
            for (TypingGlyph& g : r.glyphs) shift(g);
        }
    }

    // The drawing (0x562310).
    int w = s.width >= 1 ? s.width : t.extW;
    int h = s.height >= 1 ? s.height : t.extH;
    Image img;
    if (w <= 0 || h <= 0) return img;  // the texture is not made (Direct3D refuses an empty one)
    // Mware clamps a texture to the device's largest (16384 in Direct3D 11) and the exe then draws
    // the size it asked for into it, past its rows; Nyanroid keeps to the texture (Guess).
    constexpr int kLargest = 16384;
    if (w > kLargest || h > kLargest) t.note("a texture larger than 16384 (the exe draws past its end)");
    w = std::min(w, kLargest);
    h = std::min(h, kLargest);
    if (vertical) {
        for (TypingGlyph& g : t.glyphs) g.x += w;
        for (Typing::Ruby& r : t.rubies) {
            for (TypingGlyph& g : r.glyphs) g.x += w;
        }
    }
    std::vector<uint32_t> bits(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    auto fillClear = [&](uint32_t color) {  // the colour under every texel still transparent
        for (uint32_t& p : bits) {
            if ((p & 0xff000000u) == 0) p = color;
        }
    };
    auto drawAll = [&](int dx, int dy, uint32_t color) {
        auto one = [&](const TypingGlyph& g) {
            blitGlyph(*g.g, bits.data(), g.x + dx, g.y + dy, w, h, color, s.alphaBias, g.font->vertical());
        };
        for (const TypingGlyph& g : t.glyphs) one(g);
        for (const Typing::Ruby& r : t.rubies) {
            for (const TypingGlyph& g : r.glyphs) one(g);
        }
    };
    if (s.shadowType != 0) {
        uint32_t color = s.shadowAlpha + s.shadowColor;
        fillClear(s.shadowColor);
        int dx = s.shadowX, dy = s.shadowY;
        if (s.shadowType == 15) {
            color |= 0xff000000u;
            dx = dy = 0;
        }
        drawAll(dx, dy, color);
        if (s.shadowType == 15) {
            // Spread round the text (four threads in the exe, a quarter of the rows each).
            const std::vector<uint32_t> copy = bits;
            for (int k = 0; k < 4; ++k) {
                const int from = k * h / 4, to = k == 3 ? h : (k + 1) * h / 4;
                outlineRows(bits.data(), copy.data(), from, to, w, h, s.shadowX, s.shadowAlpha);
            }
        }
    }
    fillClear(s.fontColor);
    drawAll(0, 0, s.fontColor | 0xff000000u);
    fillClear(s.shadowColor);
    img.width = w;
    img.height = h;
    img.rgba.resize(bits.size() * 4);
    for (size_t i = 0; i < bits.size(); ++i) {
        img.rgba[i * 4 + 0] = static_cast<uint8_t>(bits[i]);
        img.rgba[i * 4 + 1] = static_cast<uint8_t>(bits[i] >> 8);
        img.rgba[i * 4 + 2] = static_cast<uint8_t>(bits[i] >> 16);
        img.rgba[i * 4 + 3] = static_cast<uint8_t>(bits[i] >> 24);
    }
    return img;
}

}  // namespace nyan::diesel
