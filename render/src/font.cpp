#include "font.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace nyan::render {

uint32_t nextCodepoint(const std::string& s, size_t& i) {
    auto c = static_cast<uint8_t>(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); ++k) cp = (cp << 6) | (static_cast<uint8_t>(s[i++]) & 0x3F);
    return cp;
}

FontAtlas::FontAtlas(Bytes ttf, int face, bool fakeBold)
    : data_(std::move(ttf)), face_(face), fakeBold_(fakeBold), pixels_(kSize * kSize, 0) {
    if (!data_.empty() && stbtt_InitFont(&info_, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), face))) {
        valid_ = true;
        stbtt_GetFontVMetrics(&info_, &ascent_, &descent_, &lineGap_);
    }
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kSize, kSize, 0, GL_RED, GL_UNSIGNED_BYTE, pixels_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

FontAtlas::~FontAtlas() {
    glDeleteTextures(1, &texture_);
}

float FontAtlas::kern(uint32_t a, uint32_t b, int pixelSize) const {
    if (!valid_) return 0;
    return stbtt_GetCodepointKernAdvance(&info_, static_cast<int>(a), static_cast<int>(b)) * scale(pixelSize);
}

bool FontAtlas::rasterize(uint32_t cp, int px, std::vector<uint8_t>& out, int& x0, int& y0, int& w, int& h) const {
    const float s = scale(px);
    const int c = static_cast<int>(cp);
    if (!fakeBold_) {
        int x1, y1;
        stbtt_GetCodepointBitmapBox(&info_, c, s, s, &x0, &y0, &x1, &y1);
        w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) return false;
        out.assign(static_cast<size_t>(w) * h, 0);
        stbtt_MakeCodepointBitmap(&info_, out.data(), w, h, w, s, s, c);
        return true;
    }
    // Windows' own bold for a face that has none (GDI, as Wine reproduces it: FT_Outline_Embolden by
    // ppem / 24, which grows the outline that much rightwards and upwards, its left and bottom edges
    // staying): the outline swept over that square, here the coverage's maximum over renders
    // shifted across it (0, half and all of it each way). Fitted: not compared with PC.
    const float k = static_cast<float>(px) / 24.0f;
    struct Pass {
        std::vector<uint8_t> a;
        int x0, y0, w, h;
    };
    std::vector<Pass> passes;
    int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            // Right by dx, up by dy: stb shifts right and down by a fraction, the whole pixels after.
            const float dx = k * static_cast<float>(i) / 2.0f, dy = -k * static_cast<float>(j) / 2.0f;
            const float ix = std::floor(dx), iy = std::floor(dy);
            const float fx = dx - ix, fy = dy - iy;
            int px0, py0, px1, py1;
            stbtt_GetCodepointBitmapBoxSubpixel(&info_, c, s, s, fx, fy, &px0, &py0, &px1, &py1);
            Pass p{{}, px0 + static_cast<int>(ix), py0 + static_cast<int>(iy), px1 - px0, py1 - py0};
            if (p.w <= 0 || p.h <= 0) continue;
            p.a.assign(static_cast<size_t>(p.w) * p.h, 0);
            stbtt_MakeCodepointBitmapSubpixel(&info_, p.a.data(), p.w, p.h, p.w, s, s, fx, fy, c);
            bx0 = std::min(bx0, p.x0), by0 = std::min(by0, p.y0);
            bx1 = std::max(bx1, p.x0 + p.w), by1 = std::max(by1, p.y0 + p.h);
            passes.push_back(std::move(p));
        }
    }
    if (passes.empty()) return false;
    x0 = bx0, y0 = by0, w = bx1 - bx0, h = by1 - by0;
    out.assign(static_cast<size_t>(w) * h, 0);
    for (const auto& p : passes) {
        for (int y = 0; y < p.h; ++y) {
            uint8_t* row = &out[static_cast<size_t>(p.y0 - y0 + y) * w + (p.x0 - x0)];
            const uint8_t* src = &p.a[static_cast<size_t>(y) * p.w];
            for (int x = 0; x < p.w; ++x) row[x] = std::max(row[x], src[x]);
        }
    }
    return true;
}

const Glyph& FontAtlas::glyph(uint32_t cp, int px) {
    uint64_t key = (static_cast<uint64_t>(px) << 32) | cp;
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;

    Glyph g{};
    if (valid_) {
        float s = scale(px);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info_, static_cast<int>(cp), &adv, &lsb);
        g.advance = adv * s;
        // Windows' emboldened glyph steps one pixel further (Wine: the advance plus 1 << 6).
        if (fakeBold_) g.advance += 1.0f;
        int x0, y0, w, h;
        std::vector<uint8_t> a;
        int ax = 0, ay = 0;
        if (rasterize(cp, px, a, x0, y0, w, h) && reserve(w, h, ax, ay)) {
            for (int y = 0; y < h; ++y)
                std::memcpy(&pixels_[static_cast<size_t>(ay + y) * kSize + ax], &a[static_cast<size_t>(y) * w], static_cast<size_t>(w));
            g.u0 = static_cast<float>(ax) / kSize;
            g.v0 = static_cast<float>(ay) / kSize;
            g.u1 = static_cast<float>(ax + w) / kSize;
            g.v1 = static_cast<float>(ay + h) / kSize;
            g.x0 = static_cast<float>(x0);
            g.y0 = static_cast<float>(y0);
            g.x1 = static_cast<float>(x0 + w);
            g.y1 = static_cast<float>(y0 + h);
        }
    }
    return glyphs_.emplace(key, g).first->second;
}

bool FontAtlas::reserve(int w, int h, int& x, int& y) {
    if (w + 2 >= kSize || h + 2 >= kSize) return false;
    if (penX_ + w + 1 >= kSize) {
        penX_ = 1;
        penY_ += rowHeight_ + 1;
        rowHeight_ = 0;
    }
    if (penY_ + h + 1 >= kSize) {
        // Atlas full: start over. Glyphs are re-rasterised as they are requested again.
        std::fill(pixels_.begin(), pixels_.end(), 0);
        glyphs_.clear();
        edges_.clear();
        ++generation_;
        penX_ = penY_ = 1;
        rowHeight_ = 0;
    }
    x = penX_;
    y = penY_;
    penX_ += w + 1;
    rowHeight_ = std::max(rowHeight_, h);
    dirty_ = true;
    return true;
}

const Glyph& FontAtlas::edge(uint32_t cp, int px, bool around, bool rightdown) {
    const uint64_t key = (static_cast<uint64_t>(px) << 34) | (static_cast<uint64_t>(around) << 33) |
                         (static_cast<uint64_t>(rightdown) << 32) | cp;
    auto it = edges_.find(key);
    if (it != edges_.end()) return it->second;
    Glyph e{};
    int x0, y0, w, h;
    std::vector<uint8_t> glyphAlpha;
    if (valid_ && (around || rightdown) && rasterize(cp, px, glyphAlpha, x0, y0, w, h)) {
        const int x1 = x0 + w, y1 = y0 + h;
        {
            // The glyph's alpha, padded by 2 on each side (the font image's margin).
            constexpr int kPad = 2;
            const int W = w + 2 * kPad, H = h + 2 * kPad;
            std::vector<uint8_t> a(static_cast<size_t>(W) * H, 0), edge(a.size(), 0), row(a.size(), 0);
            for (int y = 0; y < h; ++y)
                std::memcpy(&a[static_cast<size_t>(kPad + y) * W + kPad], &glyphAlpha[static_cast<size_t>(y) * w], static_cast<size_t>(w));
            auto at = [&](const std::vector<uint8_t>& v, int x, int y) -> int {
                return x < 0 || y < 0 || x >= W || y >= H ? 0 : v[static_cast<size_t>(y) * W + x];
            };
            if (around) {
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        row[static_cast<size_t>(y) * W + x] = static_cast<uint8_t>(std::min(255, at(a, x - 1, y) + at(a, x, y) + at(a, x + 1, y)));
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        edge[static_cast<size_t>(y) * W + x] =
                            static_cast<uint8_t>(std::min(255, at(row, x, y - 1) + at(row, x, y) + at(row, x, y + 1)));
            }
            if (rightdown) {
                // Each pixel from the edge as it was: the glyph and the edge one pixel up-left added.
                std::vector<uint8_t> base = edge;
                for (int y = 1; y < H; ++y)
                    for (int x = 1; x < W; ++x)
                        edge[static_cast<size_t>(y) * W + x] = static_cast<uint8_t>(
                            std::min(255, at(a, x - 1, y - 1) + at(base, x - 1, y - 1) + at(base, x, y)));
            }
            int ax = 0, ay = 0;
            if (reserve(W, H, ax, ay)) {
                for (int y = 0; y < H; ++y)
                    std::memcpy(&pixels_[static_cast<size_t>(ay + y) * kSize + ax], &edge[static_cast<size_t>(y) * W], static_cast<size_t>(W));
                e.u0 = static_cast<float>(ax) / kSize;
                e.v0 = static_cast<float>(ay) / kSize;
                e.u1 = static_cast<float>(ax + W) / kSize;
                e.v1 = static_cast<float>(ay + H) / kSize;
                e.x0 = static_cast<float>(x0 - kPad);
                e.y0 = static_cast<float>(y0 - kPad);
                e.x1 = static_cast<float>(x1 + kPad);
                e.y1 = static_cast<float>(y1 + kPad);
            }
        }
    }
    return edges_.emplace(key, e).first->second;
}

void FontAtlas::flush() {
    if (!dirty_) return;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSize, kSize, GL_RED, GL_UNSIGNED_BYTE, pixels_.data());
    dirty_ = false;
}

}  // namespace nyan::render
