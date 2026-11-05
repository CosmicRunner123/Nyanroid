#include "nyan/wa2/subtitles.hpp"

#include <sstream>

#include "nyan/wa2/gfx.hpp"

namespace nyan::wa2 {

void Subtitles::load(ByteSpan file) {
    blocks_.clear();
    std::string all(file.begin(), file.end());
    std::vector<std::string> lines;
    std::istringstream in(all);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty() || lines[i][0] != '{') continue;
        Block b;
        if (++i >= lines.size()) break;
        std::istringstream head(lines[i]);
        head >> b.scene >> b.key >> b.endScene >> b.endMsg;
        for (++i; i < lines.size() && (lines[i].empty() || lines[i][0] != '}'); ++i) {
            std::istringstream ls(lines[i]);
            Line line;
            uint64_t duration = 0;
            if (!(ls >> line.start >> duration)) continue;
            line.end = line.start + duration;
            std::string rest;
            std::getline(ls, rest);
            line.text = rest.empty() ? rest : rest.substr(1);  // less the separating space
            b.lines.push_back(std::move(line));
        }
        blocks_.push_back(std::move(b));
    }
}

namespace {

// The DLL's glyph table (d3d9.dll 0x10039798, indexed by the code): code, x, y, w, h, y offset in
// font.png. '{' and '}' carry the codes of '[' and ']' and so do not match.
struct Glyph {
    int code, x, y, w, h, yoff;
};
constexpr Glyph kGlyphs[0x5f] = {
    {32, 17, 150, 19, 3, 0}, {33, 19, 68, 15, 30, 0}, {34, 355, 127, 17, 15, 0}, {35, 252, 98, 23, 26, 0},
    {36, 209, 0, 24, 35, 0}, {37, 75, 38, 32, 30, 0}, {38, 138, 38, 30, 30, 0}, {39, 372, 127, 11, 15, 0},
    {40, 169, 0, 20, 36, 0}, {41, 189, 0, 20, 36, 0}, {42, 278, 127, 19, 18, 0}, {43, 48, 127, 23, 23, 0},
    {44, 319, 127, 13, 16, 20}, {45, 0, 150, 17, 9, 12}, {46, 419, 127, 11, 11, 20}, {47, 28, 0, 25, 37, 0},
    {48, 275, 98, 24, 25, 6}, {49, 364, 98, 18, 24, 6}, {50, 299, 98, 23, 24, 6}, {51, 336, 38, 24, 30, 6},
    {52, 360, 38, 24, 30, 6}, {53, 80, 98, 25, 29, 6}, {54, 408, 38, 23, 30, 6}, {55, 105, 98, 25, 29, 6},
    {56, 384, 38, 24, 30, 6}, {57, 431, 38, 23, 30, 6}, {58, 222, 127, 14, 23, 6}, {59, 220, 98, 15, 28, 6},
    {60, 322, 98, 21, 24, 0}, {61, 332, 127, 23, 15, 0}, {62, 343, 98, 21, 24, 0}, {63, 0, 68, 19, 30, 0},
    {64, 276, 0, 31, 33, 0}, {65, 206, 68, 30, 29, 0}, {66, 353, 68, 28, 29, 0}, {67, 256, 38, 28, 30, 0},
    {68, 175, 68, 31, 29, 0}, {69, 266, 68, 29, 29, 0}, {70, 295, 68, 29, 29, 0}, {71, 198, 38, 29, 30, 0},
    {72, 74, 68, 35, 29, 0}, {73, 176, 98, 22, 29, 2}, {74, 284, 38, 26, 30, 0}, {75, 143, 68, 32, 29, 0},
    {76, 54, 98, 26, 29, 0}, {77, 34, 68, 40, 29, 0}, {78, 39, 38, 36, 30, 0}, {79, 227, 38, 29, 30, 0},
    {80, 381, 68, 28, 29, 0}, {81, 96, 0, 29, 36, 0}, {82, 324, 68, 29, 29, 0}, {83, 310, 38, 26, 30, 0},
    {84, 0, 98, 27, 29, 0}, {85, 107, 38, 31, 30, 0}, {86, 168, 38, 30, 30, 0}, {87, 0, 38, 39, 30, 0},
    {88, 109, 68, 34, 29, 0}, {89, 409, 68, 28, 29, 0}, {90, 236, 68, 30, 29, 0}, {91, 233, 0, 22, 35, 0},
    {92, 75, 0, 12, 37, 0}, {93, 255, 0, 21, 35, 0}, {94, 297, 127, 22, 17, 0}, {95, 430, 127, 28, 9, 0},
    {96, 383, 127, 13, 14, 0}, {97, 71, 127, 23, 23, 8}, {98, 378, 0, 22, 32, 0}, {99, 162, 127, 20, 23, 8},
    {100, 307, 0, 24, 32, -1}, {101, 182, 127, 20, 23, 8}, {102, 5, 0, 20, 38, 0}, {103, 130, 98, 23, 29, 8},
    {104, 331, 0, 24, 32, -2}, {105, 415, 0, 15, 31, 0}, {106, 53, 0, 22, 37, 0}, {107, 355, 0, 23, 32, 0},
    {108, 400, 0, 15, 32, -1}, {109, 382, 98, 33, 23, 8}, {110, 94, 127, 23, 23, 8}, {111, 140, 127, 22, 23, 8},
    {112, 27, 98, 27, 29, 8}, {113, 198, 98, 22, 29, 8}, {114, 258, 127, 20, 22, 8}, {115, 202, 127, 20, 23, 8},
    {116, 235, 98, 17, 27, 3}, {117, 0, 127, 24, 23, 8}, {118, 117, 127, 23, 23, 8}, {119, 415, 98, 32, 23, 8},
    {120, 24, 127, 24, 23, 8}, {121, 153, 98, 23, 29, 8}, {122, 236, 127, 22, 22, 8}, {91, 125, 0, 22, 36, 0},
    {124, 87, 0, 9, 37, 0}, {93, 147, 0, 22, 36, 0}, {126, 396, 127, 23, 12, 0}
};

}  // namespace

void Subtitles::draw(const std::string& text, int fontSlot, const std::function<void(const SpriteObject&)>& emit) {
    auto rowStart = [&](size_t from) {
        size_t n = text.find('^', from);
        n = (n == std::string::npos ? text.size() : n) - from;
        return static_cast<float>(((64 - static_cast<int>(n)) * 20) / 2);
    };
    float x = rowStart(0);
    int row = 0;
    Glyph g{0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<uint8_t>(text[i]);
        if (c < 0x20 || c > 0x7e) continue;  // not drawn, no advance
        if (c == '^') {
            row += 35;
            if (++i >= text.size()) break;
            c = static_cast<uint8_t>(text[i]);
            x = rowStart(i);
        }
        if (c < 0x20 || c > 0x7e) continue;
        const Glyph& t = kGlyphs[c - 0x20];
        int yoff = 0;
        if (t.code == c) {
            g = t;
            yoff = t.yoff;
        }
        SpriteObject q;
        q.used = q.visible = true;
        q.slot = static_cast<int16_t>(fontSlot);
        q.blend = 1;
        q.filter = 1;
        float y = static_cast<float>(50 + row + yoff);
        q.x = {x, x + static_cast<float>(g.w), x, x + static_cast<float>(g.w)};
        q.y = {y, y, y + static_cast<float>(g.h), y + static_cast<float>(g.h)};
        q.u0 = static_cast<float>(g.x);
        q.u1 = static_cast<float>(g.x + g.w);
        q.v0 = static_cast<float>(g.y);
        q.v1 = static_cast<float>(g.y + g.h);
        emit(q);
        x += static_cast<float>(g.w - 2);
    }
}

void Subtitles::endIfReached(int viScene, int curMsg) {
    if (!playing_ || block_ < 0 || block_ >= static_cast<int>(blocks_.size())) return;
    const Block& b = blocks_[static_cast<size_t>(block_)];
    if (b.endScene == viScene && b.endMsg <= curMsg) {
        playing_ = false;
        shown_.clear();
    }
}

void Subtitles::trigger(int key, int type, int viScene, int curMsg, uint64_t nowMs) {
    endIfReached(viScene, curMsg);
    if (playing_) return;  // a running block is never replaced
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        if (key != b.key) continue;
        if (type == 2 ? (b.scene == 0 || b.scene == viScene) : b.scene == viScene) {
            playing_ = true;
            start_ = nowMs;
            line_ = 0;
            block_ = static_cast<int>(i);
            return;
        }
    }
}

void Subtitles::messageMarked(int viScene, int curMsg) {
    endIfReached(viScene, curMsg);
}

void Subtitles::step(uint64_t nowMs) {
    shown_.clear();
    if (!playing_ || block_ < 0) return;
    const Block& b = blocks_[static_cast<size_t>(block_)];
    if (b.lines.empty()) {
        playing_ = false;
        return;
    }
    uint64_t t = nowMs - start_;
    const Line& l = b.lines[line_];
    if (l.start < t && t < l.end) shown_ = l.text;
    if (l.end < t) {
        if (line_ + 1 < b.lines.size()) ++line_;
        else playing_ = false;
    }
}

}  // namespace nyan::wa2
