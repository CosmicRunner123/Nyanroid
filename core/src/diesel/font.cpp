#include "nyan/diesel/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// stb_truetype, private to this file (the renderer links its own copy).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

namespace nyan::diesel {

struct FontFace::Impl {
    stbtt_fontinfo info{};
    int offset = 0;
};

namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

// The name table's family names (IDs 1, 4 and 16), UTF-16BE (Unicode and Windows platforms) or
// single bytes (Macintosh Roman, read as Latin-1).
std::vector<std::string> readNames(const uint8_t* data, size_t size, int offset) {
    std::vector<std::string> out;
    uint32_t table = stbtt__find_table(const_cast<uint8_t*>(data), static_cast<stbtt_uint32>(offset), "name");
    if (!table || table + 6 > size) return out;
    const uint8_t* t = data + table;
    uint16_t count = be16(t + 2), strings = be16(t + 4);
    for (uint16_t i = 0; i < count; ++i) {
        size_t rec = table + 6 + static_cast<size_t>(i) * 12;
        if (rec + 12 > size) break;
        const uint8_t* r = data + rec;
        uint16_t platform = be16(r), encoding = be16(r + 2), nameId = be16(r + 6);
        uint16_t length = be16(r + 8), off = be16(r + 10);
        if (nameId != 1 && nameId != 4 && nameId != 16) continue;
        size_t at = table + strings + off;
        if (at + length > size) continue;
        const uint8_t* s = data + at;
        std::string name;
        if (platform == 0 || (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10))) {
            for (size_t k = 0; k + 1 < length; k += 2) {
                uint32_t c = be16(s + k);
                if (c >= 0xd800 && c < 0xdc00 && k + 3 < length) {
                    uint32_t lo = be16(s + k + 2);
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    k += 2;
                }
                appendUtf8(name, c);
            }
        } else if (platform == 1 && encoding == 0) {
            for (size_t k = 0; k < length; ++k) appendUtf8(name, s[k]);
        } else {
            continue;
        }
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

// The characters a vertical ("@") face turns to stand upright in a column (Guess: East Asian wide
// and full-width code points).
bool uprightInVertical(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
           (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
           (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x20000 && c <= 0x3fffd);
}

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

FontFace::~FontFace() = default;

std::vector<std::shared_ptr<FontFace>> FontFace::load(std::shared_ptr<const Bytes> data) {
    std::vector<std::shared_ptr<FontFace>> out;
    if (!data || data->size() < 12) return out;
    const auto* p = data->data();
    int count = stbtt_GetNumberOfFonts(p);
    if (count <= 0) count = 1;
    for (int i = 0; i < count; ++i) {
        int offset = stbtt_GetFontOffsetForIndex(p, i);
        if (offset < 0) continue;
        std::shared_ptr<FontFace> face(new FontFace());
        face->data_ = data;
        face->impl_ = std::make_unique<Impl>();
        face->impl_->offset = offset;
        if (!stbtt_InitFont(&face->impl_->info, p, offset)) continue;
        face->names_ = readNames(p, data->size(), offset);
        out.push_back(std::move(face));
    }
    return out;
}

bool FontFace::answersTo(const std::string& family) const {
    for (const auto& n : names_) {
        if (sameName(n, family)) return true;
    }
    return false;
}

FontInstance::FontInstance(std::shared_ptr<FontFace> face, int size, int weight, bool noAntialias, bool vertical,
                           int alphaBias)
    : face_(std::move(face)), size_(size), weight_(weight), noAntialias_(noAntialias), vertical_(vertical),
      alphaBias_(alphaBias) {
    if (!face_) return;
    const stbtt_fontinfo& info = face_->impl().info;
    // CreateFontW with a negative height asks for that em height.
    scale_ = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(size_));
    // TEXTMETRIC of a TrueType font: the OS/2 table's Windows ascent and descent, else hhea's
    // (Fitted: rounded, as GDI's unhinted metrics).
    int winAscent = 0, winDescent = 0;
    uint32_t os2 = stbtt__find_table(info.data, static_cast<stbtt_uint32>(face_->impl().offset), "OS/2");
    if (os2) {
        winAscent = be16(info.data + os2 + 74);
        winDescent = be16(info.data + os2 + 76);
    } else {
        int a = 0, d = 0, g = 0;
        stbtt_GetFontVMetrics(&info, &a, &d, &g);
        winAscent = a;
        winDescent = -d;
    }
    unitsAscent_ = winAscent;
    unitsDescent_ = winDescent;
    ascent_ = static_cast<int>(std::lround(static_cast<float>(winAscent) * scale_));
    descent_ = static_cast<int>(std::lround(static_cast<float>(winDescent) * scale_));
    uint32_t hhea = stbtt__find_table(info.data, static_cast<stbtt_uint32>(face_->impl().offset), "hhea");
    if (hhea) maxCharWidth_ = static_cast<int>(std::lround(be16(info.data + hhea + 10) * scale_));
}

std::shared_ptr<const GlyphImage> FontInstance::glyph(uint32_t codepoint, bool italic) {
    uint64_t key = static_cast<uint64_t>(codepoint) | (italic ? 1ull << 32 : 0ull);
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;
    auto g = std::make_shared<GlyphImage>();
    g->codepoint = codepoint;
    g->cellHeight = height();
    if (face_) {
        const stbtt_fontinfo& info = face_->impl().info;
        int index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, index, &adv, &lsb);
        g->advance = static_cast<int>(std::lround(static_cast<float>(adv) * scale_));
        // A vertical font's upright character: turned a quarter to the left, (x, y) to
        // (ascent - y, x - descent), which keeps the em box in place; its advance is tmHeight.
        const bool turn = vertical_ && uprightInVertical(codepoint);
        if (turn) g->advance = height();
        // The outline, turned, then sheared for the simulated italic.
        stbtt_vertex* verts = nullptr;
        int count = stbtt_GetGlyphShape(&info, index, &verts);
        constexpr float kSlant = 0.21255656f;  // tan 12 degrees
        int fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
        bool any = false;
        for (int i = 0; i < count; ++i) {
            stbtt_vertex& v = verts[i];
            if (turn) {
                auto quarter = [&](stbtt_vertex_type& x, stbtt_vertex_type& y) {
                    const int nx = unitsAscent_ - y, ny = x - unitsDescent_;
                    x = static_cast<stbtt_vertex_type>(nx);
                    y = static_cast<stbtt_vertex_type>(ny);
                };
                quarter(v.x, v.y);
                quarter(v.cx, v.cy);
                quarter(v.cx1, v.cy1);
            }
            if (italic) {
                v.x = static_cast<stbtt_vertex_type>(std::lround(v.x + v.y * kSlant));
                v.cx = static_cast<stbtt_vertex_type>(std::lround(v.cx + v.cy * kSlant));
                v.cx1 = static_cast<stbtt_vertex_type>(std::lround(v.cx1 + v.cy1 * kSlant));
            }
            int xs[3] = {v.x, v.cx, v.cx1}, ys[3] = {v.y, v.cy, v.cy1};
            int pts = v.type == STBTT_vcubic ? 3 : v.type == STBTT_vcurve ? 2 : 1;
            for (int k = 0; k < pts; ++k) {
                if (!any) {
                    fx0 = fx1 = xs[k];
                    fy0 = fy1 = ys[k];
                    any = true;
                }
                fx0 = std::min(fx0, xs[k]), fx1 = std::max(fx1, xs[k]);
                fy0 = std::min(fy0, ys[k]), fy1 = std::max(fy1, ys[k]);
            }
        }
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (any) {
            if (italic || turn) {
                x0 = static_cast<int>(std::floor(fx0 * scale_));
                y0 = static_cast<int>(std::floor(-fy1 * scale_));
                x1 = static_cast<int>(std::ceil(fx1 * scale_));
                y1 = static_cast<int>(std::ceil(-fy0 * scale_));
            } else {
                stbtt_GetGlyphBitmapBox(&info, index, scale_, scale_, &x0, &y0, &x1, &y1);
            }
        }
        bool empty = codepoint == 0x20 || codepoint == 0x3000 || !any || x1 <= x0 || y1 <= y0;
        // GDI gives an empty glyph a 1 x 1 box at the origin, and Mware no bitmap.
        int boxW = empty ? 1 : x1 - x0, boxH = empty ? 1 : y1 - y0;
        int originX = empty ? 0 : x0, originY = empty ? 0 : -y0;
        g->pitch = (boxW + 3) & ~3;
        g->rows = empty ? 0 : boxH;
        g->left = originX;
        g->top = ascent_ - originY;
        if (!empty) {
            std::vector<uint8_t> cover(static_cast<size_t>(g->pitch) * static_cast<size_t>(boxH), 0);
            stbtt__bitmap bm;
            bm.w = boxW;
            bm.h = boxH;
            bm.stride = g->pitch;
            bm.pixels = cover.data();
            stbtt_Rasterize(&bm, 0.35f, verts, count, scale_, scale_, 0.0f, 0.0f, x0, y0, 1, info.userdata);
            g->alpha.resize(cover.size());
            if (!noAntialias_) g->levels.resize(cover.size());
            for (size_t i = 0; i < cover.size(); ++i) {
                if (noAntialias_) {
                    g->alpha[i] = cover[i] >= 128 ? (alphaBias_ >= 0 ? 254 : 255) : 0;
                } else {
                    // GGO_GRAY8_BITMAP's 65 levels, spread as the font's maker spreads them.
                    const uint32_t v = static_cast<uint32_t>((cover[i] * 64 + 127) / 255);
                    g->levels[i] = static_cast<uint8_t>(v);
                    uint32_t a = 0;
                    if (alphaBias_ < 0) {
                        a = std::min(255u, v * 255 / 63);
                    } else if (v != 0) {
                        const uint32_t b = static_cast<uint32_t>(alphaBias_);
                        a = ((v * (0xffu - b)) / 0x3fu + b) * 0xffu >> 8;
                        if (a & 0xff00u) a = 0xff;
                    }
                    g->alpha[i] = static_cast<uint8_t>(a);
                }
            }
        }
        stbtt_FreeShape(&info, verts);
        // The cell (0x100501e0): wide enough for the bitmap; `adjust` brings the pen back. A
        // vertical font's cell is tmHeight wide and the advance (so widened) high.
        int& along = vertical_ ? g->cellHeight : g->cellWidth;
        if (vertical_) g->cellWidth = height();
        along = g->advance;
        if (originX < 0) {
            g->overhang = originX;
            g->left = 0;
            along = g->advance - originX;
        }
        if (along < g->left + g->pitch) along = g->left + g->pitch;
        if (g->advance < along + g->overhang) g->adjust = g->advance - along - g->overhang;
    }
    glyphs_.emplace(key, g);
    return g;
}

void blitGlyph(const GlyphImage& g, uint32_t* bits, int x, int y, int rowPixels, int height, uint32_t color,
               int alphaBias, bool vertical) {
    const bool gray = !g.levels.empty();
    if (g.rows <= 0 || g.pitch <= 0 || (gray ? g.levels.empty() : g.alpha.empty())) return;
    const int pitch = g.pitch, rows = g.rows;
    // The glyph in the colour, row by row (0x100509e0's buffer).
    std::vector<uint32_t> tmp(static_cast<size_t>(pitch) * static_cast<size_t>(rows));
    const uint32_t ca = color >> 24, rgb = color & 0xffffffu;
    const uint32_t bias = static_cast<uint32_t>(alphaBias);
    for (size_t i = 0; i < tmp.size(); ++i) {
        uint32_t a = 0;
        if (gray) {
            uint32_t v = g.levels[i];
            if (v != 0) a = ((v * (0xffu - bias)) / 0x3fu + bias) * ca >> 8;
            if (a & 0xff00u) a = 0xff;
            tmp[i] = a << 24 | rgb;
        } else {
            uint32_t bit = g.alpha[i] != 0 ? 1u : 0u;
            tmp[i] = (bit * ca * 0xff0000u & 0xff000000u) | rgb;
        }
    }
    auto blend = [](uint32_t& d, uint32_t s) {
        if ((d >> 24) == 0) {
            d = s;
            return;
        }
        const uint32_t a = s >> 24, ia = 0xff - a;
        auto mix = [&](uint32_t dc, uint32_t sc) { return ((dc * ia + sc * a) * 0x101 + 0x7fff) / 0xffff; };
        const uint32_t alpha = (((d >> 24) * ia + a * a) * 0x101 + 0x7fff) / 0xffff;
        d = (alpha & 0xffu) << 24 | (mix(d >> 16 & 0xff, s >> 16 & 0xff) & 0xffu) << 16 |
            (mix(d >> 8 & 0xff, s >> 8 & 0xff) & 0xffu) << 8 | (mix(d & 0xff, s & 0xff) & 0xffu);
    };
    if (!vertical) {
        for (int r = g.top; r < g.top + rows; ++r) {
            if (r >= g.cellHeight || y + r < 0 || y + r >= height) continue;
            for (int c = g.left; c < g.left + pitch; ++c) {
                if (c >= g.cellWidth || x + c < 0 || x + c >= rowPixels) continue;
                blend(bits[static_cast<size_t>(y + r) * static_cast<size_t>(rowPixels) + static_cast<size_t>(x + c)],
                      tmp[static_cast<size_t>(r - g.top) * static_cast<size_t>(pitch) + static_cast<size_t>(c - g.left)]);
            }
        }
    } else {
        for (int c = g.left; c < g.left + pitch; ++c) {
            if (c >= g.cellHeight || y + c < 0 || y + c >= height) continue;
            for (int r = g.top; r < g.top + rows; ++r) {
                const int dx = g.cellWidth - r + x - 1;
                if (r >= g.cellWidth || dx < 0 || dx >= rowPixels) continue;
                blend(bits[static_cast<size_t>(y + c) * static_cast<size_t>(rowPixels) + static_cast<size_t>(dx)],
                      tmp[static_cast<size_t>(r - g.top) * static_cast<size_t>(pitch) + static_cast<size_t>(c - g.left)]);
            }
        }
    }
}

int FontInstance::kerning(uint32_t left, uint32_t right) const {
    if (!face_) return 0;
    int k = stbtt_GetCodepointKernAdvance(&face_->impl().info, static_cast<int>(left), static_cast<int>(right));
    return static_cast<int>(std::lround(static_cast<float>(k) * scale_));
}

}  // namespace nyan::diesel
