// Unit tests for nyan_core that don't need game data. Run via `ctest`.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>
#include <unistd.h>

#include "nyan/common/engine.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/cs2/anm.hpp"
#include "nyan/cs2/blowfish.hpp"
#include "nyan/cs2/cursor_image.hpp"
#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/expr.hpp"
#include "nyan/cs2/fes_runtime.hpp"
#include "nyan/cs2/fes_script.hpp"
#include "nyan/cs2/kcs_code.hpp"
#include "nyan/cs2/font_select.hpp"
#include "nyan/cs2/message_text.hpp"
#include "nyan/cs2/kif_archive.hpp"
#include "nyan/cs2/kif_seed_search.hpp"
#include "nyan/cs2/mersenne_twister.hpp"
#include "nyan/cs2/particle.hpp"
#include "nyan/cs2/save_data.hpp"
#include "nyan/cs2/scene_player.hpp"
#include "nyan/cs2/scene_text.hpp"
#include "nyan/cs2/vars.hpp"
#include "nyan/cs2/vcodes.hpp"

// stb_truetype, a private copy for this file (the renderer and the Diesel fonts build their own).
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

namespace {

int failures = 0;

#define CHECK_EQ(actual, expected)                                                                     \
    do {                                                                                               \
        auto a_ = (actual);                                                                            \
        auto e_ = (expected);                                                                          \
        if (!(a_ == e_)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #actual " == " #expected);              \
            ++failures;                                                                                \
        }                                                                                              \
    } while (0)

// A font of just a name table and an OS/2 table (big-endian), named `family` / `sub` in US English.
nyan::Bytes testFont(const std::string& family, const std::string& sub, uint16_t weight, uint16_t fsSelection) {
    nyan::Bytes d;
    auto u16 = [](nyan::Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v)); };
    auto u32 = [&](nyan::Bytes& b, uint32_t v) { u16(b, v >> 16), u16(b, v & 0xFFFF); };
    // name: format 0, records for IDs 1, 2, 4 (platform 3, encoding 1, language 0x409), UTF-16BE.
    const std::string full = family + " " + sub;
    const std::string strs[3] = {family, sub, full};
    const uint16_t ids[3] = {1, 2, 4};
    nyan::Bytes name, text;
    u16(name, 0), u16(name, 3), u16(name, 6 + 12 * 3);
    for (int i = 0; i < 3; ++i) {
        u16(name, 3), u16(name, 1), u16(name, 0x409), u16(name, ids[i]);
        u16(name, static_cast<uint32_t>(strs[i].size() * 2)), u16(name, static_cast<uint32_t>(text.size()));
        for (char c : strs[i]) u16(text, static_cast<uint8_t>(c));
    }
    name.insert(name.end(), text.begin(), text.end());
    nyan::Bytes os2(86, 0);
    os2[1] = 1;  // version 1
    os2[4] = static_cast<uint8_t>(weight >> 8), os2[5] = static_cast<uint8_t>(weight);
    os2[62] = static_cast<uint8_t>(fsSelection >> 8), os2[63] = static_cast<uint8_t>(fsSelection);
    os2[79] = 0x02;  // ulCodePageRange1 (bytes 78-81) bit 17: Japanese 932
    // sfnt header and two table records (sorted tags: OS/2, name).
    u32(d, 0x00010000), u16(d, 2), u16(d, 32), u16(d, 1), u16(d, 0);
    const uint32_t os2At = 12 + 32, nameAt = os2At + static_cast<uint32_t>(os2.size());
    u32(d, 0x4F532F32), u32(d, 0), u32(d, os2At), u32(d, static_cast<uint32_t>(os2.size()));
    u32(d, 0x6E616D65), u32(d, 0), u32(d, nameAt), u32(d, static_cast<uint32_t>(name.size()));
    d.insert(d.end(), os2.begin(), os2.end());
    d.insert(d.end(), name.begin(), name.end());
    return d;
}

void testFontStyle() {
    using namespace nyan::cs2;
    // Windows' font mapper tells a family's faces apart by weight and style (meiryo.ttc's regular
    // and meiryob.ttc's bold メイリオ share their family name).
    const nyan::Bytes bold = testFont("Test", "Bold", 700, 0x20), regular = testFont("Test", "Regular", 400, 0x40);
    const nyan::Bytes italic = testFont("Test", "Italic", 400, 0x01);
    CHECK_EQ(fontFaceCount(bold), 1);
    const FontFamilyInfo b = fontFamilyInfo(bold, 0), r = fontFamilyInfo(regular, 0), i = fontFamilyInfo(italic, 0);
    CHECK_EQ(b.family, std::string("Test"));
    CHECK_EQ(b.weight, 700);
    CHECK_EQ(b.bold, true);
    CHECK_EQ(b.italic, false);
    CHECK_EQ(r.weight, 400);
    CHECK_EQ(r.bold, false);
    CHECK_EQ(i.italic, true);
    CHECK_EQ(b.codePages, 1u << 17);
    // The family names alone: "Test", not the full name "Test Bold" (which fontFaceNames has).
    const auto families = fontFamilyNames(bold, 0);
    CHECK_EQ(families.size(), size_t{1});
    CHECK_EQ(families.empty() ? std::string() : families[0], std::string("Test"));
    const auto names = fontFaceNames(bold, 0);
    CHECK_EQ(std::find(names.begin(), names.end(), std::string("Test Bold")) != names.end(), true);
}

// A CFF2 (variable) font, as recent Android versions' Noto Sans CJK is: tools/gen/cff2_test_font.py's
// square "A", (100,0)-(500,400) at the default weight, each edge's move a blend with one region.
const char kCff2Font[] =
    "\x4f\x54\x54\x4f\x00\x0a\x00\x80\x00\x03\x00\x20\x43\x46\x46\x32\x42\xb4\x5b\x2d"
    "\x00\x00\x02\x68\x00\x00\x00\x5a\x4f\x53\x2f\x32\x45\x20\x44\xcb\x00\x00\x01\x10"
    "\x00\x00\x00\x60\x63\x6d\x61\x70\x00\x0c\x00\x94\x00\x00\x01\x78\x00\x00\x00\x34"
    "\x66\x76\x61\x72\x7e\x1d\x69\x92\x00\x00\x02\xc4\x00\x00\x00\x24\x68\x65\x61\x64"
    "\x61\x6e\x42\x70\x00\x00\x00\xac\x00\x00\x00\x36\x68\x68\x65\x61\x05\x7a\x01\xf7"
    "\x00\x00\x00\xe4\x00\x00\x00\x24\x68\x6d\x74\x78\x04\x4c\x00\x64\x00\x00\x01\x70"
    "\x00\x00\x00\x08\x6d\x61\x78\x70\x00\x02\x50\x00\x00\x00\x01\x08\x00\x00\x00\x06"
    "\x6e\x61\x6d\x65\x36\x60\xfe\x94\x00\x00\x01\xac\x00\x00\x00\x99\x70\x6f\x73\x74"
    "\x00\x03\x00\x00\x00\x00\x02\x48\x00\x00\x00\x20\x00\x01\x00\x00\x00\x01\x00\x00"
    "\x31\x33\x41\x22\x5f\x0f\x3c\xf5\x00\x03\x03\xe8\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x64\x00\x00\x01\xf4\x01\x90\x00\x00\x00\x03"
    "\x00\x02\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x03\x20\xff\x38\x00\x00\x02\x58"
    "\x00\x64\x00\x64\x01\xf4\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x02\x00\x00\x50\x00\x00\x02\x00\x00\x00\x03\x02\x26\x01\x90\x00\x05"
    "\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3f\x3f\x3f\x3f\x00\x00\x00\x41\x00\x41"
    "\x03\x20\x00\x00\x00\x00\x03\x20\x00\xc8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x20\x00\x00\x01\xf4\x00\x00\x02\x58\x00\x64\x00\x00\x00\x02"
    "\x00\x00\x00\x03\x00\x00\x00\x14\x00\x03\x00\x01\x00\x00\x00\x14\x00\x04\x00\x20"
    "\x00\x00\x00\x04\x00\x04\x00\x01\x00\x00\x00\x41\xff\xff\x00\x00\x00\x41\xff\xff"
    "\xff\xc0\x00\x01\x00\x00\x00\x00\x00\x00\x00\x06\x00\x4e\x00\x01\x00\x00\x00\x00"
    "\x00\x01\x00\x0c\x00\x00\x00\x01\x00\x00\x00\x00\x00\x02\x00\x07\x00\x0c\x00\x01"
    "\x00\x00\x00\x00\x01\x00\x00\x06\x00\x13\x00\x03\x00\x01\x04\x09\x00\x01\x00\x18"
    "\x00\x19\x00\x03\x00\x01\x04\x09\x00\x02\x00\x0e\x00\x31\x00\x03\x00\x01\x04\x09"
    "\x01\x00\x00\x0c\x00\x3f\x4e\x79\x61\x6e\x20\x54\x65\x73\x74\x20\x56\x46\x52\x65"
    "\x67\x75\x6c\x61\x72\x57\x65\x69\x67\x68\x74\x00\x4e\x00\x79\x00\x61\x00\x6e\x00"
    "\x20\x00\x54\x00\x65\x00\x73\x00\x74\x00\x20\x00\x56\x00\x46\x00\x52\x00\x65\x00"
    "\x67\x00\x75\x00\x6c\x00\x61\x00\x72\x00\x57\x00\x65\x00\x69\x00\x67\x00\x68\x00"
    "\x74\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x05\x00"
    "\x07\xdb\x0c\x24\xbb\x11\x9b\x18\x00\x00\x00\x00\x00\x1e\x00\x01\x00\x00\x00\x0c"
    "\x00\x01\x00\x00\x00\x16\x00\x01\x00\x01\x00\x00\x40\x00\x40\x00\x00\x00\x00\x00"
    "\x00\x01\x00\x00\x00\x00\x00\x02\x01\x01\x01\x19\xef\x8b\x15\xf8\x24\xef\x8c\x10"
    "\x8b\x05\x8b\xf8\x24\xef\x8c\x10\x05\xfc\x24\x27\x8c\x10\x8b\x05\x00\x00\x00\x01"
    "\x01\x01\x04\x8b\xe5\x12\x00\x00\x00\x01\x00\x00\x00\x10\x00\x02\x00\x01\x00\x14"
    "\x00\x00\x00\x08\x77\x67\x68\x74\x01\x90\x00\x00\x01\x90\x00\x00\x03\x84\x00\x00"
    "\x00\x00\x01\x00";

void testCff2Font() {
    const auto* data = reinterpret_cast<const unsigned char*>(kCff2Font);
    const nyan::Bytes bytes(data, data + sizeof(kCff2Font) - 1);
    CHECK_EQ(nyan::cs2::fontRenderable(bytes, 0), true);
    stbtt_fontinfo info;
    CHECK_EQ(stbtt_InitFont(&info, data, 0), 1);
    const int glyph = stbtt_FindGlyphIndex(&info, 'A');
    CHECK_EQ(glyph, 1);
    // At the default instance a blend keeps its defaults and drops the deltas.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBox(&info, glyph, &x0, &y0, &x1, &y1);
    CHECK_EQ(x0, 100);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 500);
    CHECK_EQ(y1, 400);
    // The outline ends with the charstring (CFF2 has no endchar): a closed square.
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&info, glyph, &v);
    CHECK_EQ(n, 5);
    if (n == 5) {
        const int want[5][2] = {{100, 0}, {500, 0}, {500, 400}, {100, 400}, {100, 0}};
        for (int i = 0; i < 5; ++i) {
            CHECK_EQ(static_cast<int>(v[i].x), want[i][0]);
            CHECK_EQ(static_cast<int>(v[i].y), want[i][1]);
        }
    }
    stbtt_FreeShape(&info, v);
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyph, &advance, &lsb);
    CHECK_EQ(advance, 600);
}

void testCgKey() {
    using nyan::cs2::ScenePlayer;
    // cs2.exe 0x790750: first and second token joined by "_", then the later tokens' first letters.
    CHECK_EQ(ScenePlayer::cgKey("CG9_01a"), std::string("CG9_01a"));
    CHECK_EQ(ScenePlayer::cgKey("e_abc01MN,1,1"), std::string("e_abc01MN_1,1"));
    CHECK_EQ(ScenePlayer::cgKey("evx04m,a,a,b,b,a"), std::string("evx04m_a,abba"));
    CHECK_EQ(ScenePlayer::cgKey("Txyz01l,2,10,cc"), std::string("Txyz01l_2,1c"));
    CHECK_EQ(ScenePlayer::cgKey(""), std::string());
}

void testMersenneTwister() {
    // Reference values from Trigger's Python implementation (catsys/mt19937.py).
    CHECK_EQ(nyan::cs2::MersenneTwister::first(0), 0x1C75C7C9u);
    CHECK_EQ(nyan::cs2::MersenneTwister::first(4357), 0xAAE64AC3u);
    CHECK_EQ(nyan::cs2::MersenneTwister::first(0xDEADBEEF), 0x563B95F8u);
}

// A .cur of one w x h DIB: `bpp` bits (palette of two for 1 bit), rows bottom-up, then the AND mask.
nyan::Bytes testCursor(int w, int h, int bpp, int hotX, int hotY, const std::vector<uint32_t>& top, const std::vector<bool>& clear) {
    nyan::Bytes d;
    auto u16 = [&](uint32_t v) { d.push_back(static_cast<uint8_t>(v)), d.push_back(static_cast<uint8_t>(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); };
    const size_t stride = (static_cast<size_t>(w) * bpp + 31) / 32 * 4, maskStride = (static_cast<size_t>(w) + 31) / 32 * 4;
    const size_t palette = bpp == 1 ? 8 : 0, size = 40 + palette + (stride + maskStride) * static_cast<size_t>(h);
    u16(0), u16(2), u16(1);
    d.push_back(static_cast<uint8_t>(w)), d.push_back(static_cast<uint8_t>(h)), d.push_back(0), d.push_back(0);
    u16(static_cast<uint32_t>(hotX)), u16(static_cast<uint32_t>(hotY)), u32(static_cast<uint32_t>(size)), u32(22);
    u32(40), u32(static_cast<uint32_t>(w)), u32(static_cast<uint32_t>(h * 2)), u16(1), u16(static_cast<uint32_t>(bpp));
    for (int i = 0; i < 6; ++i) u32(0);
    if (bpp == 1) u32(0x00000000), u32(0x00FFFFFF);  // black, white
    for (int y = h - 1; y >= 0; --y) {
        std::vector<uint8_t> row(stride, 0);
        for (int x = 0; x < w; ++x) {
            const uint32_t c = top[static_cast<size_t>(y) * w + x];
            if (bpp == 32) {
                for (int k = 0; k < 4; ++k) row[static_cast<size_t>(x) * 4 + k] = static_cast<uint8_t>(c >> (8 * k));
            } else if ((c & 0xFFFFFF) != 0) {
                row[static_cast<size_t>(x) / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
            }
        }
        d.insert(d.end(), row.begin(), row.end());
    }
    for (int y = h - 1; y >= 0; --y) {
        std::vector<uint8_t> row(maskStride, 0);
        for (int x = 0; x < w; ++x) {
            if (clear[static_cast<size_t>(y) * w + x]) row[static_cast<size_t>(x) / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
        }
        d.insert(d.end(), row.begin(), row.end());
    }
    return d;
}

void testCursorFile() {
    // 1 bit: white over black, the mask clearing the right column; rows come out top-down.
    std::vector<uint32_t> px = {0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF};
    auto c = nyan::cs2::decodeCursorFile(testCursor(2, 2, 1, 1, 0, px, {false, true, false, true}));
    CHECK_EQ(c.has_value(), true);
    if (c) {
        CHECK_EQ(c->width, 2);
        CHECK_EQ(c->hotX, 1);
        CHECK_EQ(c->argb[0], 0xFFFFFFFFu);
        CHECK_EQ(c->argb[1], 0u);
        CHECK_EQ(c->argb[2], 0xFF000000u);
        CHECK_EQ(c->argb[3], 0u);
    }
    // 32 bits with alpha: the alpha stands, the mask is not used.
    std::vector<uint32_t> argb = {0x80FF0000, 0xFF00FF00, 0x00000000, 0xFF0000FF};
    c = nyan::cs2::decodeCursorFile(testCursor(2, 2, 32, 0, 1, argb, {true, true, true, true}));
    CHECK_EQ(c.has_value(), true);
    if (c) {
        CHECK_EQ(c->argb[0], 0x80FF0000u);
        CHECK_EQ(c->argb[3], 0xFF0000FFu);
        CHECK_EQ(c->hotY, 1);
    }
    CHECK_EQ(nyan::cs2::decodeCursorFile(nyan::Bytes{'R', 'I', 'F', 'F', 0, 0}).has_value(), false);
}

void testConcent() {
    using namespace nyan::cs2;
    // 4 groups of a head and 9 trail pieces; the point mid-screen, the box 1500 x 900.
    ParticleParams p;
    p.count = 40, p.texW = 256, p.texH = 256;
    p.f[0] = 10, p.f[1] = 50, p.f[2] = 15, p.f[3] = 640, p.f[4] = 360, p.f[5] = 1500, p.f[6] = 900;
    const uint32_t seed = 7;
    auto e = ParticleEffect::create("CONCENT", p, seed);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->globals() != nullptr, true);
    const auto& r = e->records();
    CHECK_EQ(r[0].v[0].alpha, 0);  // heads stay clear
    CHECK_EQ(r[0].pos[2], 500.0f);
    CHECK_EQ(r[0].pivot[0], 128.0f);
    // Init takes two integers a head (x, y; the floats have their own generator); the update's
    // first is the join test.
    ParticleRandom rng(seed);
    for (int i = 0; i < 2 * 4; ++i) rng.next();
    const bool joined = rng.next() % 10 == 0;
    e->step(1);
    // The first update's step is 0: a piece at the head, at its scale, at full alpha.
    CHECK_EQ(r[4].v[0].alpha, 255);
    CHECK_EQ(r[4].pos == r[0].pos, true);
    CHECK_EQ(r[4].scale == r[0].scale, true);
    // One group runs at first (the DLL's global starts at 1).
    CHECK_EQ(r[4 + 9].v[0].alpha, joined ? 255 : 0);
    const auto before = r[0].pos;
    e->step(1);
    // The next piece halfway along the step, stretched to 0.7 of it; the first fading.
    const float dx = r[0].pos[0] - before[0], dy = r[0].pos[1] - before[1];
    const float dist = std::sqrt(dx * dx + dy * dy);
    CHECK_EQ(dist > 0, true);
    CHECK_EQ(std::fabs(r[5].pos[0] - (before[0] + r[0].pos[0]) * 0.5f) < 1e-3f, true);
    CHECK_EQ(std::fabs(r[5].scale[1] - dist * 0.7f * r[0].scale[1]) < 1e-4f, true);
    CHECK_EQ(r[5].scale[0], r[0].scale[0]);
    CHECK_EQ(r[5].v[0].alpha, 255);
    CHECK_EQ(r[4].v[0].alpha, 226);  // 255 over 9 frames, one in
    // Turned so its y runs along the step (D3DX's z rotation takes y to (-sin, cos)).
    const float t = r[5].rot[2];
    CHECK_EQ(std::fabs(-std::sin(t) - dx / dist) < 1e-3f && std::fabs(std::cos(t) - dy / dist) < 1e-3f, true);
    // A second layer running it shares the DLL's globals; a fresh load has its own.
    auto shared = ParticleEffect::create("concent", p, seed + 1, e->globals());
    auto fresh = ParticleEffect::create("concent", p, seed + 1);
    CHECK_EQ(shared->globals() == e->globals(), true);
    CHECK_EQ(fresh->globals() != e->globals(), true);
    CHECK_EQ(ParticleEffect::create("kirakira", p, seed)->globals() == nullptr, true);
    // The layer's blend, as each DLL's Init sets its +0x10: concent and kirakira additive,
    // warukira not.
    CHECK_EQ(e->blend(), 1);
    CHECK_EQ(ParticleEffect::create("kirakira", p, seed)->blend(), 1);
    CHECK_EQ(ParticleEffect::create("warukira", p, seed)->blend(), 0);
}

void testDigi() {
    using namespace nyan::cs2;
    // A digi layer: the sweep 640,730 -> 640,-50 over 42 frames; f13 1 (additive).
    ParticleParams p;
    p.count = 50, p.texW = 512, p.texH = 512;
    p.planePos = {640, 360};  // its plane at rest
    const float f[] = {640, 730, 640, -50, 42, 50, 50, 100, 50, 250, 5, 20, 200, 1};
    for (size_t i = 0; i < 14; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("digi", p, 3);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    e->step(1);
    int lit = 0;
    bool tinted = true, quarters = true;
    for (const auto& r : e->records()) {
        lit += r.v[0].alpha != 0;
        tinted = tinted && r.v[0].r >= 225 && r.v[0].g >= 225 && r.v[0].b >= 225 && r.v[3].r == r.v[0].r;
        quarters = quarters && (r.v[0].u == 0.0f || r.v[0].u == 0.5f) && r.v[3].u == r.v[0].u + 0.5f;
        // Placed round the emitter's start (+-250, +-5), one step of at most 1 on.
        tinted = tinted && std::fabs(r.pos[1] - 730.0f) <= 6.0f;
    }
    CHECK_EQ(lit > 0, true);
    CHECK_EQ(tinted, true);
    CHECK_EQ(quarters, true);
    p.f[13] = 0;
    CHECK_EQ(ParticleEffect::create("digi", p, 3)->blend(), 0);
    // A plane moved 100 right follows as the DLL does: + (position - half the screen) a frame.
    auto a = ParticleEffect::create("digi", p, 3), b = ParticleEffect::create("digi", p, 3);
    b->setPlanePos(740, 360);
    a->step(1), b->step(1);
    CHECK_EQ(std::fabs(b->records()[0].pos[0] - a->records()[0].pos[0] - 100.0f) < 0.01f, true);
}

void testFilmNoise() {
    using namespace nyan::cs2;
    // A layer of eight records and no floats, as Happiness 2 loads it.
    ParticleParams p;
    p.count = 8, p.texW = 1024, p.texH = 1024;
    auto e = ParticleEffect::create("filmnoise", p, 7);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    // The grain patches: over the screen, alpha 5, hidden until their first turn.
    CHECK_EQ(rs[0].pos[0] == 640.0f && rs[0].pos[1] == 360.0f && rs[0].pos[2] == 20.0f, true);
    CHECK_EQ(rs[0].scale[0] == 1.25f && rs[0].scale[1] == 0.8f, true);
    CHECK_EQ(static_cast<int>(rs[1].v[0].alpha), 5);
    CHECK_EQ(rs[1].v[0].v == 0.5f && rs[1].v[3].u == 0.625f && rs[1].v[3].v == 0.8125f, true);
    // Only the scratch shows at first.
    CHECK_EQ(projectParticles(*e, 1280, 720, 256).size(), static_cast<size_t>(1));
    CHECK_EQ(rs[2].pos[1] == 200.0f && rs[2].scale[0] >= 0.1f && rs[2].scale[0] <= 0.2f, true);
    for (int k = 3; k < 8; ++k) CHECK_EQ(rs[k].draw == 0 && rs[k].pos[2] == static_cast<float>(k - 2), true);
    // Both patches show in updates 4-5, 8-9, ... (a turn after more than one frame).
    std::string seen;
    for (int k = 0; k < 9; ++k) {
        e->step(1);
        seen += static_cast<char>('0' + rs[0].draw);
        CHECK_EQ(rs[0].draw, rs[1].draw);
    }
    CHECK_EQ(seen, std::string("000110011"));
    // The specks join one by one (one more when a random number % 30 is 0), all of them in time.
    int first[8] = {};
    for (int k = 1; k <= 2000; ++k) {
        e->step(1);
        for (int j = 3; j < 8; ++j)
            if (!first[j] && rs[j].draw) first[j] = k;
    }
    for (int j = 3; j < 8; ++j) CHECK_EQ(first[j] > 0 && (j == 3 || first[j] >= first[j - 1]), true);
    CHECK_EQ(rs[7].scale[0] >= 0.005f && rs[7].scale[0] <= 0.015f && rs[7].v[0].alpha < 200, true);
}

void testEnergy() {
    using namespace nyan::cs2;
    // `particle3 2 energy <texture> 80 18500 7 -40 390 640 780 90 30 600 120 1 20 30`.
    ParticleParams p;
    p.count = 80, p.texW = 256, p.texH = 256;
    const float f[] = {-40, 390, 640, 780, 90, 30, 600, 120, 1, 20, 30};
    for (size_t i = 0; i < 11; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("energy", p, 9);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    CHECK_EQ(rs[0].pos[0] == -40.0f && rs[0].pos[1] == 390.0f && rs[0].v[0].alpha == 0, true);
    for (const auto& r : rs) CHECK_EQ(r.scale[0] >= 0.6f && r.scale[0] <= 1.2f, true);
    // Two updates waiting (unseen, colours changing), then the fade-in from the third.
    e->step(1);
    e->step(1);
    CHECK_EQ(static_cast<int>(rs[5].v[0].alpha), 0);
    CHECK_EQ(rs[5].v[0].r >= 56 && rs[5].v[0].g >= 56 && rs[5].v[0].b >= 56, true);
    e->step(1);
    CHECK_EQ(rs[5].v[0].alpha > 0, true);
    for (int k = 0; k < 20; ++k) e->step(1);
    CHECK_EQ(static_cast<int>(rs[5].v[0].alpha), 255);  // in for 21 updates, then out
    const float before = rs[5].scale[0];
    e->step(1);
    CHECK_EQ(rs[5].scale[0] < before, true);
    // By then the path's 51 frames are over: every spark ends at the end point, out.
    for (int k = 0; k < 40; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.pos[0] == 640.0f && r.pos[1] == 780.0f && r.v[0].alpha == 0, true);
}

void testSteam() {
    using namespace nyan::cs2;
    // `particle3 7 steam <texture> 50 15500 0 120 80 80 90 100 70 60 50 40 10 60 0 720
    // 1250 720 55 1 1`: sand along the bottom edge, half of it under way at once.
    ParticleParams p;
    p.count = 50, p.texW = 256, p.texH = 256;
    const float f[] = {120, 80, 80, 90, 100, 70, 60, 50, 40, 10, 60, 0, 720, 1250, 720, 55, 1, 1};
    for (size_t i = 0; i < 18; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("steam", p, 11);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    // Every puff starts on the line (y 720, x 0-1250).
    for (const auto& r : rs) CHECK_EQ(std::fabs(r.pos[1] - 720.0f) < 0.01f && r.pos[0] >= -0.01f && r.pos[0] <= 1250.01f, true);
    for (int k = 0; k < 30; ++k) e->step(1);
    // The started ones rise (the line's start less its end, turned a quarter: up the screen).
    int up = 0, lit = 0;
    for (size_t i = 0; i < 25; ++i) {
        up += rs[i].pos[1] < 700.0f;
        lit += rs[i].v[0].alpha > 0;
    }
    CHECK_EQ(up >= 20 && lit >= 20, true);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha <= 85, true);  // alpha 60 +- 25
}

void testDash() {
    using namespace nyan::cs2;
    // `particle3 0 dash <texture> 300 37500 0 1 100 40 -90 1280 360 50`.
    ParticleParams p;
    p.count = 300, p.texW = 256, p.texH = 64;
    const float f[] = {1, 100, 40, -90, 1280, 360, 50};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("dash", p, 13);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    // The count is the DLL's global (1 in the file), up one an update: a second layer loaded
    // while this one runs shares it.
    auto e2 = ParticleEffect::create("dash", p, 14, e->globals());
    const auto& rs = e->records();
    for (int k = 0; k < 3; ++k) e->step(1);
    e2->step(1);  // 5 now
    int moving = 0;
    for (size_t i = 0; i < 6; ++i) moving += rs[i].v[0].alpha == 50;
    CHECK_EQ(moving, 4);  // heads 0-3 by the fourth update of the shared count (2, 3, 4, then 5)
    // Lines run left to right (-90 degrees), out of the band y 260-460 round the middle, even
    // ones above it and odd ones below.
    for (int k = 0; k < 3; ++k) e->step(1);
    for (size_t i = 0; i < 6; ++i) {
        CHECK_EQ(rs[i].pos[0] > -700.0f && rs[i].pos[0] < 0.0f, true);
        CHECK_EQ(i % 2 == 0 ? rs[i].pos[1] < 261.0f : rs[i].pos[1] > 459.0f, true);
        CHECK_EQ(std::fabs(rs[i].rot[2] + 1.5707964f) < 1e-6f, true);
    }
}

void testRising4() {
    using namespace nyan::cs2;
    // `particle3 0 rising4 <texture> 50 #259 3 580 500 0 40 0 0 580 -40 0`.
    ParticleParams p;
    p.count = 50, p.texW = 256, p.texH = 256;
    const float f[] = {580, 500, 0, 40, 0, 0, 580, -40, 0};
    for (size_t i = 0; i < 9; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("rising4", p, 15);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    CHECK_EQ(rs[0].pos[0] == 580.0f && rs[0].pos[1] == 500.0f && rs[0].v[0].alpha == 255 && rs[0].scale[0] == 0.4f, true);
    for (int k = 1; k <= 10; ++k) e->step(1);
    // One piece thrown an update; the head stays put (its second phase never starts).
    int lit = 0;
    for (size_t i = 1; i < 50; ++i) lit += rs[i].v[0].alpha > 0;
    CHECK_EQ(lit, 10);
    for (int k = 0; k < 100; ++k) e->step(1);
    CHECK_EQ(rs[0].pos[0] == 580.0f && rs[0].pos[1] == 500.0f, true);
    for (size_t i = 1; i < 50; ++i) {
        CHECK_EQ(std::fabs(rs[i].pos[0] - 580.0f) <= 250.5f && std::fabs(rs[i].pos[1] - 500.0f) <= 250.5f, true);
        CHECK_EQ(rs[i].scale[0] < 0.4f, true);
    }
    // rising3 (640 360 0, size 80, wait 1, 15 frames to 640 -80 0): the head goes.
    const float g[] = {640, 360, 0, 80, 1, 15, 640, -80, 0};
    for (size_t i = 0; i < 9; ++i) p.f[i] = g[i];
    auto r3 = ParticleEffect::create("rising3", p, 15);
    const auto& hs = r3->records();
    for (int k = 0; k < 3; ++k) r3->step(1);  // the wait ends at update 3 (clock 2 > 1)
    CHECK_EQ(hs[0].pos[1], 360.0f);
    for (int k = 0; k < 8; ++k) r3->step(1);
    CHECK_EQ(hs[0].pos[1] < 200.0f && hs[0].pos[1] > -80.0f, true);
    for (int k = 0; k < 20; ++k) r3->step(1);
    CHECK_EQ(hs[0].pos[0] == 640.0f && hs[0].pos[1] == -80.0f, true);
}

void testRingWave() {
    using namespace nyan::cs2;
    // `particle3 0 ring <texture> 20 22500 7 5 640 360 1 500 5 1 25`: four rings of
    // five records, one every five updates.
    ParticleParams p;
    p.count = 20, p.texW = 512, p.texH = 512;
    const float f[] = {5, 640, 360, 1, 500, 5, 1, 25};
    for (size_t i = 0; i < 8; ++i) p.f[i] = f[i];
    p.planePos = {640, 360};
    auto e = ParticleEffect::create("ring", p, 1);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    e->step(1);  // head 0: fades in to 200 in one frame's time, at 0.01
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 0);
    CHECK_EQ(rs[4].pos[2] == 1.0f && rs[4].v[0].alpha == 10, true);  // its first copy, alpha 10
    e->step(1);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 200);
    for (int k = 0; k < 13; ++k) e->step(1);  // update 15: head 0 is halfway (13 + 1 + 1 frames)
    CHECK_EQ(rs[0].scale[0] > 2.0f && rs[0].scale[0] < 3.0f && rs[0].v[0].alpha > 0, true);
    CHECK_EQ(rs[3].scale[0] == 0.01f && rs[3].v[0].alpha == 0, true);  // head 3 starts at update 16
    for (int k = 0; k < 30; ++k) e->step(1);
    CHECK_EQ(rs[0].scale[0] == 5.0f && rs[0].v[0].alpha == 0, true);  // done, and stays so
    // Heads follow their plane's move; their copies stay at the centre.
    e->setPlanePos(740, 360);
    e->step(1);
    CHECK_EQ(rs[3].pos[0] == 740.0f && rs[4].pos[0] == 640.0f, true);
    // hadou (640 360, 1-800 percent, every 50, alpha 50 over 20 in, 120 out, 220 wait)
    // is 1.1 times wider than high and starts again after its wait.
    ParticleParams q;
    q.count = 20, q.texW = 512, q.texH = 512;
    const float g[] = {5, 640, 360, 1, 800, 50, 50, 20, 120, 220};
    for (size_t i = 0; i < 10; ++i) q.f[i] = g[i];
    auto h = ParticleEffect::create("hadou", q, 2);
    const auto& hs = h->records();
    for (int k = 0; k < 60; ++k) h->step(1);
    CHECK_EQ(std::fabs(hs[0].scale[0] - hs[0].scale[1] * 1.1f) < 1e-5f && hs[0].v[0].alpha > 0 && hs[0].v[0].alpha <= 50, true);
    // Each phase turns once its count passes its frames: in at update 22, out at 143, the wait
    // at 364; update 365 starts the ring again at its first scale.
    for (int k = 0; k < 304; ++k) h->step(1);
    CHECK_EQ(hs[0].scale[1] == 8.0f, true);
    h->step(1);
    CHECK_EQ(hs[0].scale[1] == 0.01f && hs[0].scale[0] == 0.01f, true);
}

void testWcMirror() {
    using namespace nyan::cs2;
    // `particle3 3 wc_mirror <texture> 40 15500 0 640 360 90 30 2 1 400 40 100 120 30
    // 10 30 1`.
    ParticleParams p;
    p.count = 40, p.texW = 64, p.texH = 64;
    const float f[] = {640, 360, 90, 30, 2, 1, 400, 40, 100, 120, 30, 10, 30, 1};
    for (size_t i = 0; i < 14; ++i) p.f[i] = f[i];
    p.planePos = {640, 360};
    auto e = ParticleEffect::create("wc_mirror", p, 3);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    // Top corners coloured (one channel 225-255), bottom ones white; pivot on the bottom edge.
    const auto& v = rs[0].v;
    CHECK_EQ(std::max({v[0].r, v[0].g, v[0].b}) >= 225 && std::min({v[0].r, v[0].g, v[0].b}) < 30, true);
    CHECK_EQ(v[2].r == 255 && v[3].b == 255 && rs[0].pivot[1] == 64.0f, true);
    CHECK_EQ(std::fabs(rs[0].scale[1] - rs[0].scale[0] * 6.0f) < 1e-5f, true);
    for (int k = 0; k < 20; ++k) e->step(1);
    // Shards on their circles round the axis (radius 50-150), tilted by -30 degrees, facing it.
    for (size_t i = 0; i < 20; ++i) {
        const float dx = rs[i].pos[0] - 640.0f, dz = rs[i].pos[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        CHECK_EQ(d >= 49.0f && d <= 151.0f && rs[i].pos[1] == 360.0f, true);
        CHECK_EQ(std::fabs(rs[i].rot[0] + 0.5235988f) < 1e-5f, true);
    }
    CHECK_EQ(rs[25].v[0].alpha, 0);  // not started yet
    // wc_prism: the same, every corner coloured.
    auto q = ParticleEffect::create("wc_prism", p, 3);
    const auto& w = q->records()[0].v;
    CHECK_EQ(std::max({w[3].r, w[3].g, w[3].b}) >= 225 && std::min({w[3].r, w[3].g, w[3].b}) < 30, true);
}

void testPaper() {
    using namespace nyan::cs2;
    // `particle3 0 paper <texture> #250 #259 5 1 1000 10 50 1 1 1`.
    ParticleParams p;
    p.count = 40, p.texW = 128, p.texH = 128;
    const float f[] = {1, 1000, 10, 50, 1, 1, 1};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("paper", p, 21);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    // Colours from the DLL's table of seven; a line above the screen; 0.1-0.15 of the texture.
    for (const auto& r : rs) {
        const int b = r.v[0].b, g = r.v[0].g, red = r.v[0].r;
        CHECK_EQ((b == 240 && g == 240 && red == 240) || ((b == 110 || b == 255) && (g == 110 || g == 255) && (red == 110 || red == 255)), true);
        CHECK_EQ(r.pos[1] == -50.0f && r.scale[0] >= 0.1f && r.scale[0] <= 0.15f, true);
    }
    // Half start part-way (f5): those show at once, below the line.
    e->step(1);
    int shown = 0;
    for (size_t i = 0; i < 20; ++i) shown += rs[i].v[0].alpha == 255 && rs[i].pos[1] > -50.0f;
    CHECK_EQ(shown >= 15, true);
    // The CRT rand() (the first step's length) is the DLL's: a fresh load starts it again, a load
    // while another layer runs paper carries on its sequence.
    auto fresh = ParticleEffect::create("paper", p, 21);
    auto again = ParticleEffect::create("paper", p, 21);
    auto shared = ParticleEffect::create("paper", p, 21, e->globals());
    for (int k = 0; k < 30; ++k) fresh->step(1), again->step(1), shared->step(1);
    CHECK_EQ(fresh->records()[3].pos[1], again->records()[3].pos[1]);
    CHECK_EQ(fresh->records()[3].pos[1] != shared->records()[3].pos[1], true);
}

void testWcSlash() {
    using namespace nyan::cs2;
    // `particle3 4 wc_slash <texture> 100 17500 0 1200 920 1000 640 300 0 -1000 720
    // -1000 110 60 0 50 130 200 1`.
    ParticleParams p;
    p.count = 100, p.texW = 256, p.texH = 256;
    const float f[] = {1200, 920, 1000, 640, 300, 0, -1000, 720, -1000, 110, 60, 0, 50, 130, 200, 1};
    for (size_t i = 0; i < 16; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("wc_slash", p, 4);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    e->step(1);  // clock 0: no step yet, one piece from the start to itself
    CHECK_EQ(rs[0].v[0].x == 1200.0f && rs[0].v[1].x == 1200.0f, true);
    // The fan's point: the ends' middle (100, 820, 0) pushed half as far again from (640, 300, 0).
    CHECK_EQ(rs[0].v[2].x == -170.0f && rs[0].v[2].y == 1080.0f && rs[0].v[3].z == 0.0f, true);
    CHECK_EQ(rs[0].scale[0] == 1.3f && rs[0].pivot[0] == 128.0f, true);
    for (int k = 0; k < 10; ++k) e->step(1);
    // Pieces chain along the path: each starts where the one before ends.
    int chained = 0;
    for (size_t i = 2; i < 20; ++i) chained += rs[i].v[0].x == rs[i - 1].v[1].x || rs[i].v[3].alpha == 0;
    CHECK_EQ(chained, 18);
    CHECK_EQ(rs[1].v[0].alpha > 0, true);  // fading in towards 200
}

void testRubble() {
    using namespace nyan::cs2;
    // `particle3 0 rubble <texture> 500 8600 5 1080 230 0 280 510 1000 10 600 50`.
    ParticleParams p;
    p.count = 500, p.texW = 64, p.texH = 64;
    const float f[] = {1080, 230, 0, 280, 510, 1000, 10, 600, 50};
    for (size_t i = 0; i < 9; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("rubble", p, 5);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    CHECK_EQ(rs[0].pos[0] == 1080.0f && rs[0].pos[1] == 230.0f, true);  // piece 0 at the line's start
    CHECK_EQ(rs[250].pos[2] > 300.0f && rs[250].pos[2] < 700.0f, true);   // piece 250 about halfway
    e->step(1);  // 50 a update
    CHECK_EQ(rs[49].v[0].alpha == 0 && rs[50].v[0].alpha == 0, true);  // in from 0 over a tenth of life
    for (int k = 0; k < 40; ++k) e->step(1);
    CHECK_EQ(rs[0].v[0].alpha > 0 && rs[0].pos[1] < 230.0f, true);  // rising, fading in
    for (int k = 0; k < 700; ++k) e->step(1);
    const float y = rs[0].pos[1];
    e->step(1);
    CHECK_EQ(rs[0].pos[1] == y && rs[0].v[0].alpha == 255, true);  // done: it stays
}

void testWcThunder() {
    using namespace nyan::cs2;
    // `particle3 7 wc_thunder <texture> 4 37500 0 1 400 360 150 1 1 60`.
    ParticleParams p;
    p.count = 4, p.texW = 256, p.texH = 512;
    const float f[] = {1, 400, 360, 150, 1, 1, 60};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    p.planePos = {640, 360};
    auto e = ParticleEffect::create("wc_thunder", p, 6);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    // The CRT's first rand() (41) & 3 = 1: column 0.25-0.5; 1.05-1.8 wide, 4.5 tall.
    CHECK_EQ(rs[0].v[0].u == 0.25f && rs[0].v[3].u == 0.5f && rs[0].v[3].v == 1.0f, true);
    CHECK_EQ(rs[0].scale[0] >= 1.05f && rs[0].scale[0] <= 1.8f && rs[0].scale[1] == 4.5f, true);
    e->step(1);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 0);  // the first bolt starts at the second update
    e->step(1);
    e->step(1);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 255);
    for (int k = 0; k < 80; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha, 0);  // all done; the layer stops updating
}

void testProtect() {
    using namespace nyan::cs2;
    // `particle3 1 protect <texture> 26 7600 1 390 360 170 430 400` (f5 0: alpha blend
    // until the fade-out).
    ParticleParams p;
    p.count = 26, p.texW = 512, p.texH = 512;
    const float f[] = {390, 360, 170, 430, 400};
    for (size_t i = 0; i < 5; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("protect", p, 3);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    const auto& rs = e->records();
    CHECK_EQ(e->blend(), 0);
    CHECK_EQ(rs[0].v[0].alpha == 0 && rs[0].v[3].alpha == 0, true);  // the head is not drawn
    CHECK_EQ(rs[13].v[0].x == 0 && rs[13].v[3].y == 0, true);  // the faces start collapsed
    e->step(1);
    CHECK_EQ(e->samplerStates().size(), size_t{2});
    CHECK_EQ(e->samplerStates()[0].type == 1 && e->samplerStates()[0].value == 1, true);  // ADDRESSU WRAP
    CHECK_EQ(e->samplerStates()[1].type == 2 && e->samplerStates()[1].value == 1, true);  // ADDRESSV WRAP
    // Face 12 (record 13), the middle: (-50, -50, 0) x 1.7 about (390, 360); facing the camera,
    // full light; alpha 0 at the first update.
    CHECK_EQ(rs[13].v[0].x == 305.0f && rs[13].v[0].y == 275.0f && rs[13].v[3].x == 475.0f, true);
    CHECK_EQ(static_cast<int>(rs[13].v[0].r), 255);
    CHECK_EQ(static_cast<int>(rs[13].v[0].alpha), 0);
    // Face 4 (record 5), top right, turned 8.4 degrees away: (0.1455 - 0.9894) onto 255-60.
    CHECK_EQ(static_cast<int>(rs[5].v[0].g), 239);
    // The scroll: 0.043 eased by (1 - sin(90 / 65 degrees)) at the first update.
    CHECK_EQ(std::fabs(rs[13].v[0].u - (0.4f + 0.043f * 0.97584f)) < 1e-5f, true);
    CHECK_EQ(std::fabs(rs[13].v[1].u - rs[13].v[0].u - 0.2f) < 1e-6f, true);
    for (int k = 1; k <= 60; ++k) e->step(1);  // updates 1-60: fading in
    CHECK_EQ(static_cast<int>(rs[13].v[0].alpha), 255);
    CHECK_EQ(static_cast<int>(rs[5].v[2].alpha), 32);  // the table's 32
    CHECK_EQ(rs[13].v[0].u > 1.0f, true);  // past the texture: it wraps
    for (int k = 61; k <= 64; ++k) e->step(1);  // the hold; the scroll's last updates
    const float u = rs[13].v[0].u;
    e->step(1);
    e->step(1);
    CHECK_EQ(e->blend(), 0);
    CHECK_EQ(rs[13].v[0].u, u);  // the scroll has stopped (the head's clock at 65)
    e->step(1);  // update 67: the fade-out starts, additive
    CHECK_EQ(e->blend(), 1);
    e->step(1);
    CHECK_EQ(static_cast<int>(rs[13].v[0].alpha), 250);
    for (int k = 69; k <= 140; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha, 0);
}

void testStar() {
    using namespace nyan::cs2;
    // 560 records and six floats, as Happiness 2 loads it: seven groups of 80 (a head, 79 sparks).
    ParticleParams p;
    p.count = 560, p.texW = 512, p.texH = 512;
    const float f[] = {210, 560, 900, -800, 15, 10};
    for (size_t i = 0; i < 6; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("o_150102", p, 9);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    auto sparkOf = [&](size_t g, size_t k) -> const ParticleRecord& { return rs[7 + g * 79 + k]; };
    for (size_t g = 0; g < 7; ++g) CHECK_EQ(rs[g].v[0].alpha == 0 && rs[g].v[3].alpha == 0, true);  // heads: not drawn
    // Group 1's colour (.data 0x10012d30: b 10, g 255, r 255); a lower quarter of the texture.
    const auto& s1 = sparkOf(1, 0);
    CHECK_EQ(s1.v[0].b == 10 && s1.v[0].g == 255 && s1.v[0].r == 255 && s1.v[0].alpha == 0, true);
    CHECK_EQ((s1.v[0].u == 0.0f || s1.v[0].u == 0.5f) && s1.v[0].v == 0.5f && s1.v[3].v == 1.0f, true);
    CHECK_EQ(s1.pivot[0] == 256.0f && s1.pivot[1] == 256.0f, true);
    e->step(1);
    // Each head starts 17 sparks in its first update (o_150102's limit 16, plus one).
    for (size_t g = 0; g < 7; ++g) {
        int started = 0;
        for (size_t k = 0; k < 79; ++k) started += sparkOf(g, k).pos[0] != 0 || sparkOf(g, k).pos[1] != 0;
        CHECK_EQ(started, 17);
    }
    // At the start, within +-100 x the head's size (0.15 x 0.94: +-14.1 units).
    const auto& s0 = sparkOf(3, 0);
    CHECK_EQ(std::fabs(s0.pos[0] - 210.0f) < 14.2f && std::fabs(s0.pos[1] - 560.0f) < 14.2f, true);
    // The head's size as it starts it (0.15 x 0.94), 0.7 of that for the right quarter, then the
    // update's 0.99.
    const float want = 0.15f * 0.94f * (s0.v[0].u > 0.4f ? 0.7f : 1.0f) * 0.99f;
    CHECK_EQ(std::fabs(s0.scale[0] - want) < 1e-6f && s0.scale[1] == s0.scale[0], true);
    // Group 3 is the fan's middle (0 degrees): its head runs straight at the end, 10 frames, and
    // stops just past it (x 1.01^3).
    for (int k = 0; k < 11; ++k) e->step(1);
    const float dx = 690, dy = -1360, len = std::sqrt(dx * dx + dy * dy);
    const float hx = rs[3].pos[0], hy = rs[3].pos[1];
    CHECK_EQ(std::fabs((hx - 210) * dy - (hy - 560) * dx) / len < 0.5f, true);  // on the line
    CHECK_EQ(std::fabs(std::sqrt((hx - 210) * (hx - 210) + (hy - 560) * (hy - 560)) - len * 1.030301f) < 1.0f, true);
    // Group 0 is the fan's edge, 70 degrees anticlockwise of it (y down: turned towards -x).
    const float ax = rs[0].pos[0] - 210, ay = rs[0].pos[1] - 560;
    const float turn = std::atan2(ay, ax) - std::atan2(dy, dx);
    CHECK_EQ(std::fabs(turn + 70.0f * 3.14159265f / 180.0f) < 0.01f, true);
    // Arrived: no more sparks; after 120 frames all are done.
    for (int k = 0; k < 130; ++k) e->step(1);
    for (size_t i = 7; i < rs.size(); ++i) CHECK_EQ(rs[i].v[0].alpha, 0);
}

void testExprode() {
    using namespace nyan::cs2;
    // `particle3 0 exprode <texture> 50 #259 5 640 300 100 50 5 20 15`.
    ParticleParams p;
    p.count = 50, p.texW = 512, p.texH = 512;
    const float f[] = {640, 300, 100, 50, 5, 20, 15};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("exprode", p, 4);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    for (const auto& r : rs) {
        CHECK_EQ(r.pos[0] >= 590 && r.pos[0] <= 690 && r.pos[1] >= 250 && r.pos[1] <= 350, true);
        CHECK_EQ(r.scale[0] >= 0.5f && r.scale[0] <= 1.0f && r.scale[1] == r.scale[0], true);
        CHECK_EQ(r.v[0].alpha == 0 && r.v[3].u == 0.2f && r.v[3].v == 0.2f, true);
    }
    const float y11 = rs[11].pos[1];
    e->step(1);  // the DLL's count: 1 + 10 puffs move
    CHECK_EQ(rs[10].pos[1] < 350.0f && rs[11].pos[1] == y11, true);
    for (int k = 1; k <= 5; ++k) e->step(1);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 255);  // faded in at its clock's 5
    CHECK_EQ(rs[0].v[0].u == 0.6f && rs[0].v[0].v == 0.0f, true);  // cell round(5 / 40 x 24) = 3
    for (int k = 6; k <= 30; ++k) e->step(1);  // the hold ends at 27; fading out, spinning
    CHECK_EQ(rs[0].v[0].alpha < 255 && rs[0].rot[2] > 0.0f && rs[0].scale[0] < rs[1].scale[0] * 2.0f, true);
    for (int k = 31; k <= 60; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha, 0);
    // A second layer loaded while this one runs shares the count (50 already): all of its puffs
    // move from its first update.
    auto e2 = ParticleEffect::create("exprode", p, 5, e->globals());
    const float y49 = e2->records()[49].pos[1];
    e2->step(1);
    CHECK_EQ(e2->records()[49].pos[1] < y49, true);
}

void testCmnptcl() {
    using namespace nyan::cs2;
    // A Particle Designer file as cmnptcl reads it (ptcl<f0><f1>.plist through the host): one
    // particle a 6 frames (10 records over a 1 s life), straight up at 60 px/s, no variance.
    auto plist = [](const char* duration) {
        std::string s = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>angle</key><real>90</real><key>angleVariance</key><real>0</real>
<key>blendFuncDestination</key><integer>1</integer><key>blendFuncSource</key><integer>770</integer>
<key>duration</key><real>)";
        s += duration;
        s += R"(</real><key>emitterType</key><real>0</real>
<key>finishColorAlpha</key><real>1</real><key>finishColorBlue</key><real>0.25</real>
<key>finishColorGreen</key><real>0.5</real><key>finishColorRed</key><real>1</real>
<key>finishParticleSize</key><real>60</real><key>gravityx</key><real>0</real><key>gravityy</key><real>0</real>
<key>maxParticles</key><real>150</real><key>particleLifespan</key><real>1</real>
<key>particleLifespanVariance</key><real>0</real><key>radialAcceleration</key><real>0</real>
<key>rotationStart</key><real>0</real><key>sourcePositionx</key><real>640</real>
<key>sourcePositiony</key><real>360</real><key>speed</key><real>60</real><key>speedVariance</key><real>0</real>
<key>startColorAlpha</key><real>1</real><key>startColorBlue</key><real>0.25</real>
<key>startColorGreen</key><real>0.5</real><key>startColorRed</key><real>1</real>
<key>startParticleSize</key><real>60</real><key>tangentialAcceleration</key><real>0</real>
<key>textureFileName</key><string>particle_texture.png</string><key>textureImageData</key><string></string>
</dict></plist>)";
        return s;
    };
    ParticleParams p;
    p.count = 10, p.texW = 64, p.texH = 64;
    p.f[0] = 3, p.f[1] = 0;
    p.planePos = {640, 360};
    std::string asked;
    p.readFile = [&](const std::string& name) { asked = name; return plist("-1"); };
    auto e = ParticleEffect::create("cmnptcl", p, 2);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(asked, std::string("ptcl3000.plist"));
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    for (const auto& r : rs) CHECK_EQ(r.draw == 0 && r.pivot[0] == 32.0f, true);
    for (int k = 0; k < 6; ++k) e->step(1);
    CHECK_EQ(rs[0].draw, 0);  // 6 frames: not yet past one particle's 6
    e->step(1);
    CHECK_EQ(rs[0].draw == 1 && rs[1].draw == 0, true);
    // Up the screen (the angle negated), 1 px a frame; colours b g r a = 63 127 255 255; 60 / 30.
    CHECK_EQ(std::fabs(rs[0].pos[0] - 640.0f) < 1e-3f && std::fabs(rs[0].pos[1] - 359.0f) < 1e-4f, true);
    CHECK_EQ(rs[0].v[0].b == 63 && rs[0].v[0].g == 127 && rs[0].v[0].r == 255 && rs[0].v[0].alpha == 255, true);
    CHECK_EQ(rs[0].scale[0] == 2.0f && rs[0].scale[1] == 2.0f, true);
    for (int k = 0; k < 6; ++k) e->step(1);
    CHECK_EQ(rs[1].draw == 1 && std::fabs(rs[0].pos[1] - 353.0f) < 1e-3f, true);
    // f2 set: every particle at the first emission; a 0.1 s duration (6 frames) then stops them:
    // each is hidden as its life runs out and the layer then stops.
    p.f[2] = 1;
    p.readFile = [&](const std::string&) { return plist("0.1"); };
    auto all = ParticleEffect::create("cmnptcl", p, 2);
    for (int k = 0; k < 7; ++k) all->step(1);
    for (const auto& r : all->records()) CHECK_EQ(r.draw, 1);
    for (int k = 0; k < 60; ++k) all->step(1);
    for (const auto& r : all->records()) CHECK_EQ(r.draw, 0);
    // No file: Init stops there, the records keep the engine's defaults.
    p.readFile = [](const std::string&) { return std::string(); };
    auto none = ParticleEffect::create("cmnptcl", p, 2);
    none->step(1);
    CHECK_EQ(none->records()[0].draw == 1 && none->records()[0].v[0].alpha == 255, true);
}

void testWcDark() {
    using namespace nyan::cs2;
    // `particle3 6 wc_dark <texture> 30 16500 0 570 220 20 40 50`: dark_tri's
    // older build with one emitter (f0 f1), fall f2 / 1000, alpha f3, size f4 %.
    ParticleParams p;
    p.count = 30, p.texW = 256, p.texH = 256;
    const float f[] = {570, 220, 20, 40, 50};
    for (size_t i = 0; i < 5; ++i) p.f[i] = f[i];
    p.planePos = {640, 360};  // at rest: no move
    auto e = ParticleEffect::create("wc_dark", p, 8);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    CHECK_EQ(rs[0].scale[0] == 0.5f && rs[0].v[0].alpha == 0, true);
    e->step(1);
    // One step off the emitter (0.75-0.775 a frame, y also falling 0.02).
    const float dx = rs[0].pos[0] - 570.0f, dy = rs[0].pos[1] - 220.0f;
    CHECK_EQ(std::sqrt(dx * dx + dy * dy) < 0.8f && std::sqrt(dx * dx + dy * dy) > 0.7f, true);
    CHECK_EQ(rs[1].pos[0] == 0 && rs[1].pos[1] == 0, true);  // not started yet
    for (int k = 0; k < 4; ++k) e->step(1);  // a second one from the fifth update
    CHECK_EQ(rs[1].pos[0] != 0, true);
    for (int k = 0; k < 40; ++k) e->step(1);
    CHECK_EQ(rs[0].v[0].alpha > 0 && rs[0].v[0].alpha <= 40, true);
    // The plane's move is added (dark_tri adds the plane's position as it is).
    p.planePos = {700, 360};
    auto moved = ParticleEffect::create("wc_dark", p, 8);
    moved->setPlanePos(700, 360);
    moved->step(1);
    CHECK_EQ(std::fabs(moved->records()[0].pos[0] - 630.0f) < 0.8f, true);  // 570 + 60
}

void testRadiate() {
    using namespace nyan::cs2;
    // `particle3 0 radiate <texture> 400 37500 1 10 10 700 50 10 100 1500`: 40 groups of 10 (a
    // head, 9 streaks).
    ParticleParams p;
    p.count = 400, p.texW = 32, p.texH = 32;
    const float f[] = {10, 10, 700, 50, 10, 100, 1500};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("radiate", p, 12);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    CHECK_EQ(std::fabs(rs[0].pos[0] - 700.0f) <= 100.0f && std::fabs(rs[0].pos[1] - 50.0f) <= 100.0f, true);
    CHECK_EQ(rs[0].v[0].alpha == 0 && rs[0].scale[0] == 0.01f, true);  // the head is not drawn
    const std::array<float, 3> start = rs[0].pos;
    e->step(1);  // the first update: no move yet, the streak sits on the head
    CHECK_EQ(rs[40].pos == start && rs[40].v[0].alpha == 255, true);
    e->step(1);
    e->step(1);
    // The head moves straight out from the centre; the second streak lies halfway along its step,
    // turned along it.
    const float hx = rs[0].pos[0] - 700.0f, hy = rs[0].pos[1] - 50.0f;
    const float sx = start[0] - 700.0f, sy = start[1] - 50.0f;
    CHECK_EQ(std::fabs(hx * sy - hy * sx) < 1e-2f * std::sqrt(hx * hx + hy * hy) * std::sqrt(sx * sx + sy * sy), true);
    CHECK_EQ(std::sqrt(hx * hx + hy * hy) > std::sqrt(sx * sx + sy * sy), true);
    CHECK_EQ(rs[41].v[0].alpha > 200 && rs[41].scale[1] > rs[41].scale[0], true);
    for (int k = 0; k < 12; ++k) e->step(1);
    int fading = 0;  // streaks fade over 9 frames, then are free again
    for (size_t k = 40; k < 49; ++k) fading += rs[k].v[0].alpha > 0 && rs[k].v[0].alpha < 255;
    CHECK_EQ(fading > 0, true);
}

void testRain() {
    using namespace nyan::cs2;
    // `particle3 7 rain <texture> 100 22600 0 20 10 5 10 60 5 0`: 20-30 frames to fall
    // 1020, a slant of 0.05 +- 0.05, five more drops every update (61 - 60 = 1).
    ParticleParams p;
    p.count = 100, p.texW = 64, p.texH = 512;
    const float f[] = {20, 10, 5, 10, 60, 5, 0};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("rain", p, 21);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    const auto& rs = e->records();
    for (const auto& r : rs) {
        CHECK_EQ(r.pos[1] == -100.0f && r.pos[0] >= -1152.0f && r.pos[0] <= 2432.0f, true);
        CHECK_EQ(r.rot[2] >= 0.0f && r.rot[2] <= 0.1f && r.v[0].alpha == 0, true);
    }
    const float x0 = rs[0].pos[0];
    e->step(1);  // five drops move: down 34-51 a frame, a little to the left (the slant)
    CHECK_EQ(rs[0].pos[1] > -100.0f + 33.0f && rs[0].pos[1] < -100.0f + 52.0f && rs[0].pos[0] < x0, true);
    CHECK_EQ(rs[5].pos[1], -100.0f);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 0);  // fading in over 2 frames
    e->step(1);
    e->step(1);
    CHECK_EQ(rs[0].v[0].alpha >= 100 && rs[0].v[0].alpha < 170, true);
    for (int k = 0; k < 40; ++k) e->step(1);  // past 200 below the screen: started again
    CHECK_EQ(rs[0].pos[1] < 720.0f + 200.0f + 52.0f, true);
}

void testSpark2() {
    using namespace nyan::cs2;
    // `particle3 1 spark2 <texture> 800 22600 0 1920 -900 200 100 32 120 20 30 40
    // 300 10 1 -550 60 60 0 1 5 10`: petals from the line (1920, -900) -> (200, 100), each its own
    // record (the memset's ECX of 0 as the group size).
    ParticleParams p;
    p.count = 800, p.texW = 256, p.texH = 256;
    const float f[] = {1920, -900, 200, 100, 32, 120, 20, 30, 40, 300, 10, 1, -550, 60, 60, 0, 1, 5, 10};
    for (size_t i = 0; i < 19; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("spark2", p, 13);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    for (size_t i = 0; i < 20; ++i) {
        const auto& r = rs[i];
        // On the line (a straight two-point path): (x - 200) / -1720 == (y - 100) / 1000.
        CHECK_EQ(std::fabs((r.pos[0] - 200.0f) * 1000.0f + (r.pos[1] - 100.0f) * 1720.0f) < 1.0f * 1720.0f, true);
        CHECK_EQ(r.scale[0] >= 0.32f && r.scale[0] <= 0.64f && r.v[0].alpha == 0, true);
    }
    const std::array<float, 3> p0 = rs[0].pos, p70 = rs[70].pos;
    e->step(1);  // 1 + 60 petals run
    e->step(1);
    // Petal 0 sets off down and to the left (x -3..-2.9, y -(-5.5 rand + 0.05) hundredths: down).
    CHECK_EQ(rs[0].pos[0] < p0[0] && rs[0].pos[1] > p0[1], true);
    CHECK_EQ(rs[70].pos == p70, true);  // not started yet
    CHECK_EQ(rs[0].v[0].alpha > 0 && rs[0].v[0].alpha < 255, true);
    for (int k = 0; k < 25; ++k) e->step(1);
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 255);  // faded in over 20, holding
}

void testDispMotion() {
    using namespace nyan::cs2;
    // `pl 3 mcdisp 48 48` (mot.int mcDisp): shown for 48 frames, hidden for 48, over and over;
    // `mcdisp 10 20 30`: shown 10, hidden 20, shown 30.
    Motion m;
    m.kind = Motion::Kind::Disp;
    m.loop = true;
    m.start = 5;
    m.period = 96;
    m.keys = {{48, 0, 0}, {48, 0, 0}};
    std::vector<Motion> ms{m};
    auto at = [&](uint64_t t) { return evaluate(ms, 5 + t).setVisible; };
    CHECK_EQ(at(0), 1);
    CHECK_EQ(at(47), 1);
    CHECK_EQ(at(48), 0);
    CHECK_EQ(at(95), 0);
    CHECK_EQ(at(96), 1);
    ms[0].period = 60;
    ms[0].keys = {{10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    CHECK_EQ(at(9), 1);
    CHECK_EQ(at(10), 0);
    CHECK_EQ(at(29), 0);
    CHECK_EQ(at(30), 1);
    CHECK_EQ(at(59), 1);
    CHECK_EQ(evaluate({}, 0).setVisible, -1);
}

void testCFadeMotion() {
    using namespace nyan::cs2;
    // `pl 4 mccfade 120 $99ffffff $6484eeff` (mot.int mcCFade): every byte from the
    // first colour to the second and back over 120 frames; its alpha is the opacity.
    Motion m;
    m.kind = Motion::Kind::CFade;
    m.loop = true;
    m.start = 10;
    m.period = 120;
    const uint32_t from = 0x99ffffff, to = 0x6484eeff;
    for (int b = 0; b < 4; ++b)
        m.keys.push_back({static_cast<float>((from >> (8 * b)) & 0xFF), static_cast<float>((to >> (8 * b)) & 0xFF), 0});
    const std::vector<Motion> ms{m};
    auto at = [&](uint64_t t) { return evaluate(ms, 10 + t); };
    CHECK_EQ(at(0).hasColor && at(0).color == from, true);
    CHECK_EQ(at(0).setAlpha, 0x99);
    CHECK_EQ(at(60).color, to);  // the turn, half way through the cycle
    CHECK_EQ(at(60).setAlpha, 0x64);
    // A quarter through: half way, each byte rounded (127, 194, 247, 255).
    CHECK_EQ(at(30).color, 0x7fc2f7ffu);
    CHECK_EQ(at(90).color, at(30).color);
    CHECK_EQ(at(120).color, from);  // and again
    CHECK_EQ(at(150).color, at(30).color);
}

void testImageMode() {
    using namespace nyan::cs2;
    StageImage img;
    img.parts = {"bgtest_19"};
    loadImageMode(img);
    // Happiness 2: `bg 0 mode mono` greys the loaded backdrop on the next frame; the bare `bg 0 mode`
    // loads it again from its file, in colour.
    setImageMode(img, 1, true);
    CHECK_EQ(img.filterApplied, std::string());  // nothing until the frame's step
    stepImageMode(img);
    CHECK_EQ(img.filterApplied, std::string("1"));
    stepImageMode(img);  // once only
    CHECK_EQ(img.filterApplied, std::string("1"));
    setImageMode(img, 0, true);
    stepImageMode(img);
    CHECK_EQ(img.filterApplied, std::string());
    // Each `mode` converts the pixels as they are: sepia twice is sepia of sepia, np twice undoes it.
    setImageMode(img, 2, true);
    stepImageMode(img);
    setImageMode(img, 2, true);
    stepImageMode(img);
    CHECK_EQ(img.filterApplied, std::string("22"));
    setImageMode(img, 3, true);
    stepImageMode(img);
    CHECK_EQ(img.filterApplied, std::string("223"));
    // `mode` then a load in one frame (a pattern Fruit uses): converted once, by the load.
    setImageMode(img, 3, true);
    loadImageMode(img);
    stepImageMode(img);
    CHECK_EQ(img.filterApplied, std::string("3"));
    // An empty slot keeps the mode for its next load and marks nothing.
    StageImage empty;
    setImageMode(empty, 2, true);
    CHECK_EQ(empty.filterPending, false);
    empty.parts = {"BGTEST1n"};
    loadImageMode(empty);
    CHECK_EQ(empty.filterApplied, std::string("2"));
    // Fruit's sscript: `mode` changes nothing shown until the next load.
    StageImage fruit;
    fruit.parts = {"BGTEST2"};
    loadImageMode(fruit);
    setImageMode(fruit, 2, false);
    stepImageMode(fruit);
    CHECK_EQ(fruit.filterApplied, std::string());
    loadImageMode(fruit);
    CHECK_EQ(fruit.filterApplied, std::string("2"));
}

void testKcsNativeCalls() {
    using namespace nyan::cs2;
    // `callidx 737, 8` at the image load and in the per-frame step, a `push 737` that is not a call,
    // and a byte the interpreter does not know (stepped over alone).
    std::vector<uint8_t> code;
    auto ins = [&](uint8_t op, std::initializer_list<uint32_t> args) {
        code.push_back(op);
        for (uint32_t v : args) {
            code.push_back(3);
            for (int b = 0; b < 4; ++b) code.push_back(static_cast<uint8_t>(v >> (8 * b)));
        }
    };
    ins(0x25, {737});
    ins(0x2b, {737, 8});
    ins(0x3c, {});
    code.push_back(0x40);
    ins(0x2b, {736, 24});
    ins(0x2b, {737, 8});
    ins(0x2c, {0, 4});
    std::vector<uint8_t> kcs(0x34, 0);
    std::copy_n("KCS", 4, kcs.begin());
    kcs[4 + 2 * 4] = 0x34;
    kcs[4 + 3 * 4] = static_cast<uint8_t>(code.size());
    uLongf size = compressBound(static_cast<uLong>(code.size()));
    std::vector<uint8_t> z(size);
    CHECK_EQ(compress(z.data(), &size, code.data(), static_cast<uLong>(code.size())), Z_OK);
    kcs.insert(kcs.end(), z.begin(), z.begin() + static_cast<std::ptrdiff_t>(size));
    CHECK_EQ(kcsNativeCallSites(kcs, 737), size_t{2});
    CHECK_EQ(kcsNativeCallSites(kcs, 736), size_t{1});
    CHECK_EQ(kcsNativeCallSites(kcs, 13), size_t{0});
    CHECK_EQ(kcsNativeCallSites(std::vector<uint8_t>(8, 0), 737), size_t{0});  // not a .kcs
}

void testSArrow() {
    using namespace nyan::cs2;
    // `particle3 0 s_arrow <texture> #258 #259 5 #258 0 360 1600 360 60 48`
    // (#258 150): one group, a head and 149 sparkles.
    ParticleParams p;
    p.count = 150, p.texW = 256, p.texH = 256;
    const float f[] = {150, 0, 360, 1600, 360, 60, 48};
    for (size_t i = 0; i < 7; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("s_arrow", p, 6);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    const auto& h = rs[0];
    // The head: the texture's top half about its centre, 1.2 x 0.06, alpha 0x80, heading right
    // (acos 0 negated, plus pi/2: 0).
    CHECK_EQ(h.v[3].u == 1.0f && h.v[3].v == 0.5f && h.pivot[0] == 128.0f && h.pivot[1] == 128.0f, true);
    CHECK_EQ(h.scale[0] == 1.2f && std::fabs(h.scale[1] - 0.06f) < 1e-7f && h.rot[2] == 0.0f, true);
    CHECK_EQ(static_cast<int>(h.v[0].alpha), 128);
    for (size_t i = 1; i < rs.size(); ++i) {
        const auto& r = rs[i];
        CHECK_EQ(r.v[0].alpha == 0 && r.v[3].u - r.v[0].u == 0.25f && r.v[0].v >= 0.5f, true);  // a bottom-left cell
    }
    e->step(1);
    CHECK_EQ(h.pos[0] == 0.0f && h.pos[1] == 360.0f, true);
    int started = 0;
    for (size_t i = 1; i < rs.size(); ++i) {
        if (rs[i].v[0].alpha == 0) continue;
        ++started;
        CHECK_EQ(static_cast<int>(rs[i].v[0].alpha), 255);
        CHECK_EQ(rs[i].pos[0] == 0.0f && rs[i].pos[1] == 360.0f, true);  // at the head
        CHECK_EQ(std::fabs(rs[i].scale[0] - 0.06f * 0.99f) < 1e-6f, true);
    }
    CHECK_EQ(started >= 1, true);
    for (int k = 1; k <= 24; ++k) e->step(1);
    CHECK_EQ(h.pos[0], 800.0f);  // mapRange(0, 1600, 0, 48, 24)
    for (size_t i = 1; i < rs.size(); ++i) {
        if (rs[i].v[0].alpha == 0) continue;
        // On ahead of and below where the head was (the curve bows a few pixels past its points).
        CHECK_EQ(rs[i].pos[0] >= -5.0f && rs[i].pos[0] <= 800.0f + 60 + 160 + 5 && rs[i].pos[1] >= 355.0f, true);
    }
    for (int k = 25; k <= 49; ++k) e->step(1);
    CHECK_EQ(h.pos[0], 1600.0f);
    for (int k = 50; k <= 230; ++k) e->step(1);  // none start once the head has arrived
    for (size_t i = 1; i < rs.size(); ++i) CHECK_EQ(rs[i].v[0].alpha, 0);
    CHECK_EQ(static_cast<int>(h.v[0].alpha), 128);  // the head stays, off the screen's right edge
}

void testSwordSpark() {
    using namespace nyan::cs2;
    // `particle3 0 swordspark <texture> 150 #259 5 640 360 15 18 1 1 10 80 110`.
    ParticleParams p;
    p.count = 150, p.texW = 64, p.texH = 32;
    const float f[] = {640, 360, 15, 18, 1, 1, 10, 80, 110};
    for (size_t i = 0; i < 9; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("swordspark", p, 4);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);  // f4 1: alpha-blended
    const auto& rs = e->records();
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha == 0 && r.v[3].u == 0.5f && r.v[3].v == 0.5f, true);  // the top-left quarter
    e->step(1);
    auto centre = [&](size_t i) {
        const auto& r = rs[i];
        return std::array<float, 3>{(r.v[0].x + r.v[3].x) / 2 - 640, (r.v[0].y + r.v[3].y) / 2 - 360, (r.v[0].z + r.v[3].z) / 2};
    };
    for (size_t i = 0; i < rs.size(); ++i) {
        const auto& r = rs[i];
        CHECK_EQ(static_cast<int>(r.v[0].alpha), 255);
        CHECK_EQ(r.v[0].r >= 128 && r.v[0].r == r.v[2].b, true);
        // The size (1 to 2) x 15 / 1000 of the texture, x and y alike, turned any way.
        auto d = [](const auto& a, const auto& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
        const float sx = d(r.v[0], r.v[1]) / 64, sy = d(r.v[0], r.v[2]) / 32;
        CHECK_EQ(sx >= 0.0149f && sx <= 0.0301f && std::fabs(sx - sy) < 1e-4f, true);
        // Out from the centre at i x 2.4 degrees from straight down, z 1.5 times the move across.
        const auto c = centre(i);
        const float across = std::sqrt(c[0] * c[0] + c[1] * c[1]);
        CHECK_EQ(across >= 9 / 1.118034f - 0.01f && across <= 27 / 1.118034f + 0.01f, true);
        CHECK_EQ(std::fabs(c[2] / across - 1.5f) < 1e-3f, true);
        const float a = std::atan2(-c[0], c[1]), want = static_cast<float>(i) * 2.4f * 3.14159265f / 180.0f;
        CHECK_EQ(std::fabs(std::remainder(a - want, 6.2831853f)) < 0.012f, true);
    }
    const auto c1 = centre(0);
    for (int k = 1; k <= 11; ++k) e->step(1);  // updates 1-11 at full speed (the clock past f6 at 11)
    const auto c12 = centre(0);
    e->step(1);  // then 0.3 of it
    const auto c13 = centre(0);
    CHECK_EQ(std::fabs((c12[1] - c1[1]) / 11 - c1[1]) < 1e-3f, true);
    CHECK_EQ(std::fabs((c13[1] - c12[1]) - c1[1] * 0.3f) < 1e-3f, true);
    for (int k = 13; k <= 95; ++k) e->step(1);  // the fade from f7 (80): 255 x (1 - 15 / 30) at 95
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 127);
    for (int k = 96; k <= 200; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha, 0);  // and nothing starts again
    p.f[4] = 0.5f;
    auto g = ParticleEffect::create("swordspark", p, 4);
    CHECK_EQ(g->blend(), 1);
    g->step(1);
    CHECK_EQ(static_cast<int>(g->records()[0].v[0].alpha), 223);  // 64 x 0.5 + 191
}

void testWcSteam() {
    using namespace nyan::cs2;
    // `particle3 ... wc_steam <texture> 50 15500 0 30 60 60 80 100 50 200 50 40
    // 260 180 0 570 1050 640 50 1 0`.
    ParticleParams p;
    p.count = 50, p.texW = 256, p.texH = 256;
    const float f[] = {30, 60, 60, 80, 100, 50, 200, 50, 40, 260, 180, 0, 570, 1050, 640, 50, 1, 0};
    for (size_t i = 0; i < 18; ++i) p.f[i] = f[i];
    auto w = ParticleEffect::create("wc_steam", p, 9);
    auto s = ParticleEffect::create("steam", p, 9);
    CHECK_EQ(w != nullptr && s != nullptr, true);
    if (!w || !s) return;
    CHECK_EQ(w->blend(), 0);  // alpha-blended unless f18
    CHECK_EQ(s->blend(), 1);
    // The same puffs (the same draws) on paths with other tangents: the same ends, other middles.
    const auto& rw = w->records();
    const auto& rs = s->records();
    bool differ = false;
    for (int k = 0; k <= 481; ++k) {  // record 0's clock at k - 1: the end at 480
        if (k == 0 || k == 481) CHECK_EQ(rw[0].pos == rs[0].pos, true);
        if (k == 240 && rw[0].pos != rs[0].pos) differ = true;
        if (k == 240) CHECK_EQ(rw[0].v[0].alpha, rs[0].v[0].alpha);
        w->step(1), s->step(1);
    }
    CHECK_EQ(differ, true);
    for (const auto& r : rw) CHECK_EQ(r.rot[2], 0.0f);  // not turned without f19
    // f18 and f19: additive, each puff turned to its way: acos of -y of (last - now), negated
    // when x is under 0 (heading right).
    p.f[18] = 1, p.f[19] = 1;
    auto t = ParticleEffect::create("wc_steam", p, 9);
    CHECK_EQ(t->blend(), 1);
    const auto& rt = t->records();
    for (int k = 0; k < 100; ++k) t->step(1);
    const auto before = rt[0].pos;
    t->step(1);
    const auto now = rt[0].pos;
    float x = before[0] - now[0], y = before[1] - now[1];
    const float len = std::sqrt(x * x + y * y);
    x /= len, y /= len;
    float a = std::acos(-y);
    if (x < 0) a = -a;
    CHECK_EQ(std::fabs(rt[0].rot[2] - a) < 1e-5f, true);
}

void testWcRound() {
    using namespace nyan::cs2;
    // `particle3 7 wc_roundz <texture> 120 20500 0 640 1600 1600`, and 2712:
    // `particle3 6 wc_roundy <texture> 150 10500 0 640 1010 1200`.
    for (int y = 0; y < 2; ++y) {
        ParticleParams p;
        p.count = y ? 150 : 120, p.texW = 512, p.texH = 512;
        const float f[] = {640, y ? 1010.0f : 1600.0f, y ? 1200.0f : 1600.0f};
        for (size_t i = 0; i < 3; ++i) p.f[i] = f[i];
        auto e = ParticleEffect::create(y ? "wc_roundy" : "wc_roundz", p, 5);
        CHECK_EQ(e != nullptr, true);
        if (!e) return;
        const auto& rs = e->records();
        const size_t groups = y ? 30 : 20, faces = y ? 4 : 5;
        CHECK_EQ(e->blend(), 1);
        int peak = 0;
        for (int k = 0; k < 300; ++k) {
            e->step(1);
            for (size_t g = 0; g < groups; ++g) {
                CHECK_EQ(static_cast<int>(rs[g].v[0].alpha), 0);  // the heads are not drawn
                const auto& r0 = rs[groups + g * faces];
                for (size_t q = 0; q < faces; ++q) {
                    const auto& r = rs[groups + g * faces + q];
                    for (const auto& v : r.v) {
                        CHECK_EQ(v.alpha == r0.v[0].alpha && v.r == r.v[0].r && v.g == v.r && v.b == v.r, true);
                        CHECK_EQ(v.r >= 60, true);
                    }
                    peak = std::max(peak, static_cast<int>(r.v[0].alpha));
                }
            }
            if (y == 0 && k == 0) {
                // The middle face of each strip, 30 x 50 by its head (x 0.6, turned onto its
                // side), centred at the head's (0, -u x 1.005, u). The layer turns it about y by
                // the head's spin and then scales x and y by 16 (0x100012a0 multiplies the turn
                // in before the scale), so the whirl is an ellipse 16 times as wide as it is deep.
                for (size_t g = 0; g < groups; ++g) {
                    const auto& r = rs[groups + g * faces + 2];
                    const float e1x = r.v[1].x - r.v[0].x, e1z = r.v[1].z - r.v[0].z;
                    const float e2x = r.v[2].x - r.v[0].x, e2z = r.v[2].z - r.v[0].z;
                    CHECK_EQ(std::fabs((e1x / 480) * (e1x / 480) + (e1z / 30) * (e1z / 30) - 1) < 1e-4f, true);
                    CHECK_EQ(std::fabs((e2x / 800) * (e2x / 800) + (e2z / 50) * (e2z / 50) - 1) < 1e-4f, true);
                    CHECK_EQ(std::fabs(r.v[1].y - r.v[0].y) < 1e-3f && std::fabs(r.v[2].y - r.v[0].y) < 0.7f, true);
                    const float cx = ((r.v[0].x + r.v[3].x) / 2 - 640) / 16, cz = (r.v[0].z + r.v[3].z) / 2;
                    const float cy = (r.v[0].y + r.v[3].y) / 2;
                    const float u = std::sqrt(cx * cx + cz * cz);
                    CHECK_EQ(u <= 100.01f && std::fabs(cy - (1600 - 16 * u * 1.005f)) < 0.05f, true);
                }
            }
        }
        CHECK_EQ(peak, y ? 100 : 200);
    }
}

void testS040101() {
    using namespace nyan::cs2;
    // `particle3 0 s_040101 <texture> 300 15100 1 1 834 388 795 521 731 709 834 385
    // 795 521 731 709 5 6 5 5 30 12 0`: every twinkle runs at once, peaking at alpha 128, once.
    ParticleParams p;
    p.count = 300, p.texW = 128, p.texH = 128;
    const float f[] = {1, 834, 388, 795, 521, 731, 709, 834, 385, 795, 521, 731, 709, 5, 6, 5, 5, 30, 12, 0};
    for (size_t i = 0; i < 20; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("s_040101", p, 14);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 1);
    const auto& rs = e->records();
    for (const auto& r : rs) CHECK_EQ(r.pos[0] >= 700 && r.pos[0] <= 840 && r.pos[1] >= 380 && r.pos[1] <= 720, true);
    const std::array<float, 3> p299 = rs[299].pos;
    for (int k = 0; k < 7; ++k) e->step(1);
    CHECK_EQ(rs[299].pos != p299, true);  // the last one moves from the start too
    CHECK_EQ(static_cast<int>(rs[0].v[0].alpha), 128);
    for (int k = 0; k < 40; ++k) e->step(1);
    for (const auto& r : rs) CHECK_EQ(r.v[0].alpha, 0);
    const std::array<float, 3> done = rs[0].pos;
    e->step(1);
    CHECK_EQ(rs[0].pos == done, true);  // done: it stays
}

void testParticleOrder() {
    using namespace nyan::cs2;
    // Five records at one depth: the engine's quicksort leaves them as 2, 0, 3, 1, 4; a deeper one
    // (z -10, farther away) goes first.
    struct Fixed : ParticleEffect {
        explicit Fixed(const ParticleParams& p) : ParticleEffect(p, 1) {
            for (size_t i = 0; i < records_.size(); ++i) records_[i].v[0].u = static_cast<float>(i);
        }
        void step(int32_t) override {}
        void sink(size_t i) { records_[i].pos[2] = -10; }
    };
    ParticleParams p;
    p.count = 5, p.texW = 16, p.texH = 16;
    Fixed e(p);
    auto order = [&] {
        std::string s;
        for (const auto& q : projectParticles(e, 1280, 720, 256)) s += static_cast<char>('0' + static_cast<int>(q.u0));
        return s;
    };
    CHECK_EQ(order(), std::string("20314"));
    e.sink(3);
    CHECK_EQ(order().substr(0, 1), std::string("3"));
}

void testDarkTri() {
    using namespace nyan::cs2;
    // A dark_tri layer: emitters 640,250 / 550,425 / 730,425, fall 20, alpha 40, size 50.
    ParticleParams p;
    p.count = 36, p.texW = 512, p.texH = 512;
    const float f[] = {640, 250, 550, 425, 730, 425, 20, 40, 50};
    for (size_t i = 0; i < 9; ++i) p.f[i] = f[i];
    auto e = ParticleEffect::create("dark_tri", p, 5);
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(e->blend(), 0);
    auto lit = [&] {
        int n = 0;
        for (const auto& r : e->records()) n += r.v[0].alpha != 0;
        return n;
    };
    CHECK_EQ(lit(), 0);  // all start white at alpha 0
    e->step(1);
    // Particle 0 is the only one moving: off its emitter by one step (speed under 0.8).
    const auto& r0 = e->records()[0];
    CHECK_EQ(std::fabs(r0.pos[0] - 640.0f) < 1.0f && std::fabs(r0.pos[1] - 250.0f) < 1.1f, true);
    CHECK_EQ(e->records()[1].pos[0], 0.0f);
    CHECK_EQ(r0.scale[0] >= 0.5f && r0.scale[0] < 0.51f, true);
    for (int k = 0; k < 4; ++k) e->step(1);
    // The fifth update starts particle 1, at emitter 1; by now particle 0 is fading in.
    const auto& r1 = e->records()[1];
    CHECK_EQ(std::fabs(r1.pos[0] - 550.0f) < 1.0f && std::fabs(r1.pos[1] - 425.0f) < 1.1f, true);
    CHECK_EQ(lit() >= 1, true);
    // At update 47 particle 0 has faded in fully (over 15-45) and holds (30) at alpha 40.
    for (int k = 0; k < 42; ++k) e->step(1);
    CHECK_EQ(static_cast<int>(e->records()[0].v[0].alpha), 40);
    // `particle3 <n> set` with a new size moves the emitters (Update reads f8 against its own).
    std::array<float, 32> g = p.f;
    g[0] = 100, g[1] = 100, g[8] = 80;
    e->setFloats(g);
    // Particle k (k >= 1) starts at update 4k + 1: update 49 starts particle 12, on emitter 0,
    // now at 100,100, and particle 13 waits for update 53.
    for (int k = 0; k < 2; ++k) e->step(1);
    const auto& r12 = e->records()[12];
    CHECK_EQ(std::fabs(r12.pos[0] - 100.0f) < 1.0f && std::fabs(r12.pos[1] - 100.0f) < 1.1f, true);
    CHECK_EQ(r12.scale[0] >= 0.8f && r12.scale[0] < 0.81f, true);
    CHECK_EQ(e->records()[13].pos[0], 0.0f);  // not started yet
    p.f[9] = 1;
    CHECK_EQ(ParticleEffect::create("dark_tri", p, 5)->blend(), 1);
    // dark_tri adds the plane's position as it is (not less the screen's centre).
    p.f[9] = 0;
    auto c = ParticleEffect::create("dark_tri", p, 5);
    c->setPlanePos(10, 20);
    c->step(1);
    CHECK_EQ(std::fabs(c->records()[0].pos[0] - 650.0f) < 1.0f && std::fabs(c->records()[0].pos[1] - 270.0f) < 1.1f, true);
}

void testVCodeSeed() {
    // A made-up V_CODE2 (CRC-32 bit order, each byte's result inverted).
    CHECK_EQ(nyan::cs2::vcodeSeed("NY-TESTCODE1"), 0x9534D489u);
}

void testBlowfish() {
    // Standard Eric Young test vector: all-zero key and block.
    uint8_t key[8] = {};
    nyan::cs2::Blowfish bf(nyan::ByteSpan(key, 8));
    uint32_t l = 0, r = 0;
    bf.encryptBlock(l, r);
    CHECK_EQ(l, 0x4EF99745u);
    CHECK_EQ(r, 0x6198DD78u);
    bf.decryptBlock(l, r);
    CHECK_EQ(l, 0u);
    CHECK_EQ(r, 0u);

    // Byte-level API round trip, including an unencrypted 3-byte tail.
    std::string text = "CatSystem2 rocks!!!";
    nyan::Bytes buf(text.begin(), text.end());
    bf.encrypt(buf.data(), buf.size());
    CHECK_EQ(std::string(buf.end() - 3, buf.end()), std::string("!!!"));
    bf.decrypt(buf.data(), buf.size());
    CHECK_EQ(std::string(buf.begin(), buf.end()), text);
}

void testKifNameCipher() {
    // The cipher is its own inverse.
    std::string name = "Hello_World.hg3";
    nyan::cs2::unobfuscateKifName(name, 1234);
    CHECK_EQ(name == "Hello_World.hg3", false);
    nyan::cs2::unobfuscateKifName(name, 1234);
    CHECK_EQ(name, std::string("Hello_World.hg3"));
}

// A protected exe hides its V_CODE2: the names' seed is found from the names (a made-up table).
void testKifSeedSearch() {
    const uint32_t seed = 0x00012345;
    std::vector<std::string> raw = {"__key__.dat"};
    for (int i = 1; i <= 20; ++i) {
        char n[32];
        std::snprintf(n, sizeof n, i % 2 ? "bgm%03d.ogg" : "ev%03da.hg3", i);
        raw.emplace_back(n);
        nyan::cs2::unobfuscateKifName(raw.back(), seed + static_cast<uint32_t>(i));  // its own inverse
    }
    CHECK_EQ(nyan::cs2::kifSeedFits(raw, seed), true);
    CHECK_EQ(nyan::cs2::kifSeedFits(raw, seed + 1), false);
    CHECK_EQ(nyan::cs2::kifSeedCached(raw), false);
    CHECK_EQ(nyan::cs2::findKifSeed(raw), std::optional<uint32_t>(seed));
    CHECK_EQ(nyan::cs2::kifSeedCached(raw), true);
}

// Files in a type's folder (startup.xml INT/intNN/dir) override its archive but not the update
// patches (cs2.exe 0x566490: the patches first, then the folder, then the archive).
void testLooseFolders() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("nyan-loose-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "config");
    fs::create_directories(dir / "image");
    auto write = [](const fs::path& path, const std::string& data) {
        std::ofstream(path, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    // A plain KIF: "KIF\0", the count, 72-byte entries (name, offset, length), the data.
    auto kif = [](const std::vector<std::pair<std::string, std::string>>& files) {
        std::string out = std::string("KIF\0", 4);
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i))); };
        u32(static_cast<uint32_t>(files.size()));
        uint32_t offset = 8 + 72 * static_cast<uint32_t>(files.size());
        for (const auto& [name, data] : files) {
            std::string n = name;
            n.resize(64, '\0');
            out += n;
            u32(offset);
            u32(static_cast<uint32_t>(data.size()));
            offset += static_cast<uint32_t>(data.size());
        }
        for (const auto& f : files) out += f.second;
        return out;
    };
    write(dir / "image.int", kif({{"a.hg3", "BASE-A"}, {"b.hg3", "BASE-B"}, {"c.hg3", "BASE-C"}}));
    write(dir / "update00.int", kif({{"b.hg3", "PATCH-B"}}));
    write(dir / "config" / "startup.xml",
          "<?xml version=\"1.0\" encoding=\"Shift_JIS\"?><document><INT><int00><name>image</name>"
          "<int>image.int</int><ext>hg3</ext><dir>image</dir></int00></INT></document>");
    write(dir / "image" / "a.hg3", "LOOSE-A");
    write(dir / "image" / "b.hg3", "LOOSE-B");
    write(dir / "image" / "notes.txt", "not an hg3");
    {
        nyan::DirectoryGameFolder folder(dir.string());
        const auto index = nyan::cs2::AssetIndex::load(folder);
        auto text = [&](const char* name) {
            nyan::Bytes b = index.read(name);
            return std::string(b.begin(), b.end());
        };
        CHECK_EQ(text("a.hg3"), std::string("LOOSE-A"));
        CHECK_EQ(text("b.hg3"), std::string("PATCH-B"));
        CHECK_EQ(text("c.hg3"), std::string("BASE-C"));
        CHECK_EQ(index.find("notes.txt").has_value(), false);
    }
    fs::remove_all(dir);
}

// The stand viewers' table objects as Amakano 2's cs2.exe has them: a BGTABLE2's CSV (0x727d90:
// backgrounds are the rows marked "○", pictures their fields from the third), its tone table
// (0x728fb0: comments, spaces and empty lines dropped, `#name(r,g,b)` then its pictures, the first
// of a name counting) and a CHTABLE3's sections (0x729c50), whose choices build a name part by part
// (0x72a230). The lists here are made up.
void testFesTables() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("nyan-tables-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto kif = [](const std::vector<std::pair<std::string, std::string>>& files) {
        std::string out = std::string("KIF\0", 4);
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i))); };
        u32(static_cast<uint32_t>(files.size()));
        uint32_t offset = 8 + 72 * static_cast<uint32_t>(files.size());
        for (const auto& [name, data] : files) {
            std::string n = name;
            n.resize(64, '\0');
            out += n;
            u32(offset);
            u32(static_cast<uint32_t>(data.size()));
            offset += static_cast<uint32_t>(data.size());
        }
        for (const auto& f : files) out += f.second;
        return out;
    };
    const std::string mark = "\x81\x9b";  // "○" in Shift-JIS
    const std::string csv = mark + ",bgA,bgA1,bgA2\r\nx,skip,no\r\n" + mark + ",bgB,\"bgB,1\",,bgB3,\r\n\r\n" + mark +
                            ",bgC\r\n";
    const std::string tone = "/* a\r\n comment */#t1 ( 100, 90 ,80 ) // day\r\nbgA1\r\n\r\n#t2(50,60,70)\r\nBGB3\r\nbga1\r\n";
    const std::string cl = "#_TOP_\r\n@ p1\r\n@ p2\r\n#p1\r\nbase1 w1\r\nbase2  w1 \r\n#w1\r\n@,a f1\r\n@,b\r\n"
                           "#f1\r\n@,@,@\r\n@,@,c x\r\n#p2\r\nother\r\n";
    std::ofstream(dir / "config.int", std::ios::binary)
        << kif({{"bg.csv", csv}, {"tone.tbl", tone}, {"ch.cl", cl}});
    {
        nyan::DirectoryGameFolder folder(dir.string());
        const auto assets = nyan::cs2::AssetIndex::load(folder);
        nyan::cs2::VarStore vars;
        auto script = nyan::cs2::FesScript::parse(
            "#DEFINE\n"
            "BGTABLE2 bg\n"
            "CHTABLE3 ch\n"
            "#OBJECT POS PRI BTYPE TABLE\n"
            "bg 0,0 1 0 config.int/bg.csv\n"
            "ch 0,0 2 1 config.int/ch.cl\n"
            "#START\n"
            "bg max 1\n"
            "bg exist 1 0 2\n"
            "bg exist 1 1 3\n"
            "bg exist 1 2 4\n"
            "bg exist 1 3 5\n"
            "bg exist 3 0 6\n"
            "bg gettonetbl 7 8 9\n"
            "bg tonetbl config.int/tone.tbl\n"
            "bg load 0 0\n"
            "bg gettonetbl 10 11 12\n"
            "bg load 1 2\n"
            "bg gettonetbl 13 14 15\n"
            "bg load 2 0\n"
            "ch max 20\n"
            "ch max 21 0\n"
            "ch max 22 0 1\n"
            "ch max 23 0 0 1\n"
            "ch max 24 5\n"
            "ch max 25 1\n"
            "ch load 0 0 0 0\n"
            "wait 1\n"
            "ch baseopt L\n"
            "ch load 0 1 0 1\n"
            "wait 1\n"
            "#END\n");
        auto fes = nyan::cs2::FesInstance::create(assets, vars, "t", std::move(script), 0);
        fes->tick(1);
        CHECK_EQ(vars.get(1), 3);
        CHECK_EQ(vars.get(2), 1);  // "bgB,1": quoted, its comma kept
        CHECK_EQ(vars.get(3), 0);  // an empty field
        CHECK_EQ(vars.get(4), 1);
        CHECK_EQ(vars.get(5), 0);  // past the row's last field (a trailing comma adds none)
        CHECK_EQ(vars.get(6), 0);  // only three backgrounds
        CHECK_EQ(vars.get(7), 100);  // no tone table yet
        CHECK_EQ(vars.get(10), 100);
        CHECK_EQ(vars.get(11), 90);
        CHECK_EQ(vars.get(12), 80);
        CHECK_EQ(vars.get(13), 50);
        CHECK_EQ(vars.get(14), 60);
        CHECK_EQ(vars.get(15), 70);
        CHECK_EQ(fes->objectFile("bg"), std::string("bgB3"));  // bgC has no pictures: kept
        CHECK_EQ(vars.get(20), 2);
        CHECK_EQ(vars.get(21), 2);
        CHECK_EQ(vars.get(22), 2);
        CHECK_EQ(vars.get(23), 0);  // "@,b" leads nowhere
        CHECK_EQ(vars.get(24), 0);  // out of range
        CHECK_EQ(vars.get(25), 1);
        CHECK_EQ(fes->objectFile("ch"), std::string("base1,a,0"));
        fes->tick(2);
        CHECK_EQ(fes->objectFile("ch"), std::string("base2L,a,c"));
    }
    fs::remove_all(dir);
}

void testSjis() {
    CHECK_EQ(nyan::sjisToUtf8("abc\\x"), std::string("abc\\x"));
    // "あ" (0x82A0) and half-width katakana "ｱ" (0xB1).
    CHECK_EQ(nyan::sjisToUtf8("\x82\xA0\xB1"), std::string("\xE3\x81\x82\xEF\xBD\xB1"));
    // Windows code page 932, not JIS X 0208: "～" is U+FF5E (0x8160), NEC "①" (0x8740) and IBM
    // extension "纊" (0xFA5C) decode.
    CHECK_EQ(nyan::sjisToUtf8("\x81\x60"), std::string("\xEF\xBD\x9E"));
    CHECK_EQ(nyan::sjisToUtf8("\x87\x40"), std::string("\xE2\x91\xA0"));
    CHECK_EQ(nyan::sjisToUtf8("\xFA\x5C"), std::string("\xE7\xBA\x8A"));
}

void testExpr() {
    // The tree the engine's calculator runs: a well-formed line is the whole line; a line
    // `!= 0) \(10+\0) = 1` (as one of Yukikoi's scripts has) leaves `)` (over `!=` and its missing operand), then
    // the assignment, which runs.
    using nyan::cs2::lastExpressionTree;
    CHECK_EQ(std::string(lastExpressionTree("!= 0) \\(10+\\0) = 1")), std::string("\\(10+\\0) = 1"));
    CHECK_EQ(std::string(lastExpressionTree("$(50+\\0) = 1")), std::string("$(50+\\0) = 1"));
    CHECK_EQ(std::string(lastExpressionTree("(1+2)*3")), std::string("(1+2)*3"));
    CHECK_EQ(std::string(lastExpressionTree("-1 + $2 * (3 - -4)")), std::string("-1 + $2 * (3 - -4)"));
    CHECK_EQ(std::string(lastExpressionTree("$5++")), std::string("$5++"));
    CHECK_EQ(std::string(lastExpressionTree("$1 = $2 = 0")), std::string("$1 = $2 = 0"));
    CHECK_EQ(std::string(lastExpressionTree("")), std::string());

    nyan::cs2::VarStore vars;
    nyan::cs2::ExprEvaluator e(vars);
    CHECK_EQ(e.eval("512-256"), 256);
    CHECK_EQ(e.eval("1+2*3"), 7);
    CHECK_EQ(e.eval("(1+2)*3"), 9);
    CHECK_EQ(e.eval("0x10 | 1"), 17);
    CHECK_EQ(e.eval("5 > 3 && 2 >= 2"), 1);
    CHECK_EQ(e.eval("1 << 4 >> 2"), 4);
    CHECK_EQ(e.eval("-3 + 1"), -2);
    CHECK_EQ(e.eval("7 / 0"), 0);
    CHECK_EQ(e.eval("@+5", 10), 15);

    CHECK_EQ(e.assign("#300=3"), true);
    CHECK_EQ(vars.get(300), 3);
    CHECK_EQ(e.assign("#(950+#300)=512+192"), true);
    CHECK_EQ(vars.get(953), 704);
    CHECK_EQ(e.eval("#(950+#300)+#300"), 707);
    CHECK_EQ(e.assign("#300+=2"), true);
    CHECK_EQ(vars.get(300), 5);
    CHECK_EQ(e.assign("#300++"), true);
    CHECK_EQ(vars.get(300), 6);
    CHECK_EQ(e.assign("#300==6"), false);  // comparison, not assignment
    CHECK_EQ(e.eval("#300==6"), 1);
    CHECK_EQ(*e.variableIndex("#(70+4)"), 74);
    // A scene's `#` line takes `@` from the calculator (0 after a frame break, as the original
    // showed), not from the variable: as in Happiness 2.
    CHECK_EQ(e.assign("#196 = @+520", 0), true);
    CHECK_EQ(vars.get(196), 520);
    CHECK_EQ(e.assign("#196 = @-700", 0), true);
    CHECK_EQ(vars.get(196), -700);
    CHECK_EQ(e.assign("#197 = @", 0), true);
    CHECK_EQ(vars.get(197), 0);

    // The FE scripts' evaluator returns 0 for a root unary plus (cs2.exe 0x5f40dc leaves the
    // returned register alone): Amakano 1's logo screen's `move 120 +900` moves to 0. Under an operator
    // the plus keeps its value; the scenes' evaluator is not so.
    CHECK_EQ(e.eval("+900"), 900);
    nyan::cs2::ExprEvaluator fe(vars, '$');
    fe.setPlusRootIsZero(true);
    CHECK_EQ(fe.eval("+900"), 0);
    CHECK_EQ(fe.eval(" ( +5 ) "), 0);
    CHECK_EQ(fe.eval("+(5)"), 0);
    CHECK_EQ(fe.eval("+-5"), 0);
    CHECK_EQ(fe.eval("+5*2"), 10);
    CHECK_EQ(fe.eval("(+5)*2"), 10);
    CHECK_EQ(fe.eval("3 + +5"), 8);
    CHECK_EQ(fe.eval("-900"), -900);
    CHECK_EQ(fe.eval("900"), 900);
    CHECK_EQ(fe.eval("@+5", 10), 15);
}

void testMessage() {
    auto m = nyan::cs2::parseMessage("\\fnI [don`t] [know.]\\fn\\@");
    CHECK_EQ(m.plain(), std::string("I don`t know."));
    CHECK_EQ(m.continues, true);

    m = nyan::cs2::parseMessage("\\fn[A]\\n\\w0[B]\\fss[c]\\fn");
    CHECK_EQ(m.plain(), std::string("A\nBc"));
    CHECK_EQ(m.spans.size(), size_t{4});  // [A], \n, [B], [c]: one span per word group
    CHECK_EQ(m.spans[3].size == nyan::cs2::TextSize::Smaller, true);
    CHECK_EQ(m.spans[0].group, int16_t{0});
    CHECK_EQ(m.spans[1].group, int16_t{-1});
    CHECK_EQ(m.spans[3].group, int16_t{2});
    CHECK_EQ(m.groups, int16_t{3});

    // \c<n> has red in its low byte; \fn changes only the size (cs2.exe 0x6937a0).
    m = nyan::cs2::parseMessage("\\c0xff78b6ff;Note\\fn x");
    CHECK_EQ(m.spans.size(), size_t{1});
    CHECK_EQ(m.spans[0].color, 0xffffb678u);
    CHECK_EQ(m.plain(), std::string("Note x"));

    m = nyan::cs2::parseMessage("\\n\\fn\\c0x0000ff;\\pc[The] [End!]\\c\\fn x");
    CHECK_EQ(m.plain(), std::string("\nThe End! x"));
    CHECK_EQ(m.spans[0].align, uint8_t{0});
    CHECK_EQ(m.spans[1].text, std::string("The"));
    CHECK_EQ(m.spans[3].text, std::string("End!"));
    CHECK_EQ(m.spans[3].color, 0xffff0000u);
    CHECK_EQ(m.spans[3].align, uint8_t{1});
    CHECK_EQ(m.spans.back().color, 0u);
    CHECK_EQ(m.spans.back().align, uint8_t{1});  // \pc holds until \pl or \pr

    // Lines joined before a click are one string: the next goes on in the last one's style.
    auto first = nyan::cs2::parseMessage("\\pc\\c0xff;\\fl");
    m = nyan::cs2::parseMessage("A\\plB", false, &first);
    CHECK_EQ(m.spans[0].align, uint8_t{1});
    CHECK_EQ(m.spans[0].color, 0xffff0000u);
    CHECK_EQ(m.spans[0].size == nyan::cs2::TextSize::Large, true);
    CHECK_EQ(m.spans[1].align, uint8_t{0});

    // Numbers: decimal or 0x hex, one ';' after them is part of the escape.
    m = nyan::cs2::parseMessage("a\\f10...it\\fn b\\w0;c\\f+4;d\\f-2e");
    CHECK_EQ(m.plain(), std::string("a...it bcde"));
    CHECK_EQ(m.spans[1].px, int16_t{10});
    CHECK_EQ(m.spans[2].px, int16_t{-1});
    CHECK_EQ(m.spans[3].dpx, int16_t{4});
    CHECK_EQ(m.spans[4].dpx, int16_t{2});

    // `\_` is a space (the string's text processing); any other escaped character is itself.
    m = nyan::cs2::parseMessage("\\'x\\_\\\\");
    CHECK_EQ(m.plain(), std::string("'x \\"));
    CHECK_EQ(nyan::cs2::cleanLabel("Youichi\\_"), std::string("Youichi "));

    // Shift-JIS "―" (0x815C) has a backslash trail byte that must not start an escape.
    m = nyan::cs2::parseMessage("a\x81\x5Cn");
    CHECK_EQ(m.plain(), std::string("a\xE2\x80\x95n"));

    // `\$<n>;`: the UTF-16 character n (cs2.exe 0x6937a0, amakanoPE.exe 0x581b60's GetGlyphOutlineW).
    m = nyan::cs2::parseMessage("it\\$39;s\\$8212;\\$0x41;\\$1.5;\\$233");
    CHECK_EQ(m.plain(), std::string("it's\xE2\x80\x94" "A\xC3\xA9"));

    // A `]` outside a group and a `[` inside one are drawn; groups go on into a joined line.
    m = nyan::cs2::parseMessage("a] [b [c] d");
    CHECK_EQ(m.plain(), std::string("a] b [c d"));
    CHECK_EQ(m.spans.back().group, int16_t{-1});
    auto open = nyan::cs2::parseMessage("[one ");
    m = nyan::cs2::parseMessage("two] [x]", false, &open);
    CHECK_EQ(m.spans[0].group, int16_t{0});
    CHECK_EQ(m.spans[2].group, int16_t{1});
    open.append(m);
    CHECK_EQ(open.groups, int16_t{2});

    // Only a trailing `\@` continues the page (Amakano 1's narration opens with `\fs\@`).
    CHECK_EQ(nyan::cs2::parseMessage("\\fs\\@[Snow] [country.]").continues, false);
    CHECK_EQ(nyan::cs2::parseMessage("[it.]\\@").continues, true);

    // `\t` text styles (the 2.6.1.90 readers): `\ti;` italic on, `\t;` off (Yukikoi OP02).
    m = nyan::cs2::parseMessage("It might \\ti;look\\t; the same", true, nullptr, true);
    CHECK_EQ(m.spans.size(), size_t{3});
    if (m.spans.size() == 3) {
        CHECK_EQ(m.spans[0].text + "|" + m.spans[1].text + "|" + m.spans[2].text, std::string("It might |look| the same"));
        CHECK_EQ(m.spans[0].italic || m.spans[2].italic, false);
        CHECK_EQ(m.spans[1].italic && !m.spans[1].bold, true);
    }
    // Letters up to `;` (either case); another character ends the list and is text. A joined line
    // goes on in the style the last one ended in.
    m = nyan::cs2::parseMessage("\\tBi;a\\tbx", true, nullptr, true);
    CHECK_EQ(m.plain(), std::string("ax"));
    CHECK_EQ(m.spans.size() == 2 && m.spans[0].bold && m.spans[0].italic && m.spans[1].bold && !m.spans[1].italic, true);
    auto styled = nyan::cs2::parseMessage("\\ti;one", true, nullptr, true);
    CHECK_EQ(nyan::cs2::parseMessage("two", true, &styled, true).spans.front().italic, true);
    // Older readers (Nekopara 2.6.1.76, Amakano 1) draw the `t`.
    CHECK_EQ(nyan::cs2::parseMessage("\\ti;look\\t;", true).plain(), std::string("ti;lookt;"));
}

}  // namespace

void testSaveRoundTrip() {
    using namespace nyan::cs2;
    SaveFile s;
    s.time = 1760000000;
    s.strings[0x200] = "Hello";
    s.thumbWidth = 2;
    s.thumbHeight = 1;
    s.thumbnail = {1, 2, 3, 4, 5, 6, 7, 8};
    s.resume.scene = "op";
    s.resume.pc = 42;
    s.resume.state = ScenePlayer::State::Choice;
    s.resume.bgm[0] = "bgm01";
    s.resume.se[2] = "se_rain";
    s.resume.channelVolume[{1, 2}] = 40;
    s.resume.transitionDefaults = {15, 0, 40, 2};
    s.resume.vars.set(5, -7);
    s.resume.vars.setStr(200, "start");
    StageImage img;
    img.parts = {"a", "b"};
    img.x = {0, 100, 1000, 30};  // started 10 frames before the save frame
    Motion m;
    m.kind = Motion::Kind::Rotate;
    m.loop = true;
    m.start = 990;
    m.a[2] = 1.5f;
    img.motions.push_back(m);
    s.resume.shown.layer(Layer::Cg)[3] = img;
    s.resume.shown.window.text.spans.push_back({"line", TextSize::Large, 0xFF00FF00, 12, 0, 1});
    s.resume.shown.choices.push_back({"next", "Go"});

    auto data = encodeSave(s, 1010);
    SaveFile d = decodeSave(data, 10);  // restored into a player at frame 10
    CHECK_EQ(d.time, s.time);
    CHECK_EQ(d.strings[0x200], std::string("Hello"));
    CHECK_EQ(d.thumbnail.size(), size_t{8});
    CHECK_EQ(d.resume.scene, std::string("op"));
    CHECK_EQ(d.resume.pc, size_t{42});
    CHECK_EQ(d.resume.state, ScenePlayer::State::Choice);
    CHECK_EQ(d.resume.bgm[0], std::string("bgm01"));
    CHECK_EQ(d.resume.se[2], std::string("se_rain"));
    CHECK_EQ((d.resume.channelVolume[{1, 2}]), 40);
    CHECK_EQ(d.resume.transitionDefaults[2], 40);
    CHECK_EQ(d.resume.transitionDefaults[3], 2);
    CHECK_EQ(d.resume.vars.get(5), -7);
    CHECK_EQ(d.resume.vars.str(200), std::string("start"));
    const auto& di = d.resume.shown.layer(Layer::Cg).at(3);
    CHECK_EQ(di.parts.size(), size_t{2});
    CHECK_EQ(di.x.to, 100);
    CHECK_EQ(di.x.start, uint64_t{0});  // 1000 - 1010 + 10
    CHECK_EQ(di.motions.at(0).start, uint64_t{0});  // 990 - 1010 + 10 clamps at 0
    CHECK_EQ(di.motions.at(0).a[2], 1.5f);
    CHECK_EQ(d.resume.shown.window.text.spans.at(0).text, std::string("line"));
    CHECK_EQ(d.resume.shown.window.text.spans.at(0).size, TextSize::Large);
    CHECK_EQ(d.resume.shown.window.text.spans.at(0).px, int16_t{12});
    CHECK_EQ(d.resume.shown.window.text.spans.at(0).align, uint8_t{1});
    CHECK_EQ(d.resume.shown.choices.at(0).prompt, std::string("Go"));

    GlobalData g;
    g.flags[517] = 3;
    g.config["play 3 1"] = 0;
    auto gd = decodeGlobal(encodeGlobal(g));
    CHECK_EQ(gd.flags[517], 3);
    CHECK_EQ(gd.config["play 3 1"], 0);
}

// An ANM script as cs2.exe runs it (0x849840): a mouth that opens while variable 0 is set.
void testAnm() {
    using namespace nyan::cs2;
    auto line = [](int32_t op, std::vector<std::pair<int32_t, int32_t>> p) {
        AnmScript::Line l;
        l.op = op;
        for (size_t k = 0; k < p.size(); ++k) l.p[k] = {p[k].first, p[k].second};
        return l;
    };
    auto s = std::make_shared<AnmScript>();
    s->lines = {line(0, {{0, 0}, {0, 1}, {0, 1}}),    // 0: ID 0 1 1
                line(4, {{1, 0}, {0, 3}}),            // 1: IF $0 3
                line(3, {{0, 0}}),                    // 2: JUMP 0
                line(1, {{0, 5}, {0, 4}, {0, 4}}),    // 3: SET 5 4 4
                line(0, {{0, 1}, {1, 5}, {1, 5}}),    // 4: ID 1 $5 $5
                line(0, {{0, 2}, {0, 2}, {0, 2}}),    // 5: ID 2 2 2
                line(3, {{0, 0}})};                   // 6: JUMP 0
    AnmPlayer p(s, 1);
    p.advance(100);
    CHECK_EQ(p.frame(), 0);
    p.advance(110);
    CHECK_EQ(p.frame(), 0);  // waits for the variable
    p.setVar(0, 1);
    p.advance(111);
    CHECK_EQ(p.frame(), 1);  // frame 1 for $5 = 4 ticks
    p.advance(114);
    CHECK_EQ(p.frame(), 1);
    p.advance(115);
    CHECK_EQ(p.frame(), 2);
    p.setVar(0, 0);
    p.advance(117);
    CHECK_EQ(p.frame(), 0);
    CHECK_EQ(p.history().front(), 0);
    CHECK_EQ(p.history().at(1), 2);
}

void testUtf8ToSjis() {
    // 【Policeman】 round-trips through Shift-JIS.
    std::string u = "\xE3\x80\x90Policeman\xE3\x80\x91";
    CHECK_EQ(nyan::sjisToUtf8(nyan::utf8ToSjis(u)), u);
    CHECK_EQ(nyan::utf8ToSjis("abc"), std::string("abc"));
}

void testSceneTranslation() {
    using namespace nyan::cs2;
    // CSTL: "CSTL", 4 reserved bytes, FF-encoded counts and length-prefixed UTF-8 strings.
    nyan::Bytes file = {'C', 'S', 'T', 'L', 0, 0, 0, 0};
    auto str = [&](const std::string& s) {
        file.push_back(static_cast<uint8_t>(s.size()));
        file.insert(file.end(), s.begin(), s.end());
    };
    file.push_back(2);
    str("en");
    str("jp");
    file.push_back(4);
    const char* rows[4][4] = {{"Yo", "Okay.", "Y", "j0"},
                              {"", "She shakes her head.", "", "j1"},
                              {"Yo", "A novel.\x1F(Two lines)", "Y", "j2"},
                              {"", "Choice", "", "j3"}};
    for (auto& r : rows)
        for (const char* s : r) str(s);
    SceneTranslations t = SceneTranslations::parse(file);
    CHECK_EQ(t.languageIndex("EN"), 0);
    CHECK_EQ(t.entries.size(), size_t{4});

    // A `\@` message continued by an empty line is one message; a name after a `\@` line starts the
    // next; an input wait with no text starts none; repeated choice prompts share an entry.
    CstScene scene;
    auto& L = scene.lines();
    using T = CstLineType;
    L = {{T::Name, "Y"}, {T::Message, "a\\@"}, {T::Command, "wait"}, {T::Message, ""}, {T::Input, ""},
         {T::Message, "b\\@"}, {T::Command, "wait"}, {T::Name, "Y"}, {T::Message, "c"}, {T::Message, "\\nd"},
         {T::Input, ""}, {T::Message, ""}, {T::Input, ""}, {T::Command, "fselect"},
         {T::Command, "if (#1 == 0) 1 next1 X"}, {T::Command, "if (#1 != 0) 999 next1 X"}};
    CHECK_EQ(localizeScene(scene, t, 0), true);
    CHECK_EQ(L[0].text, std::string("Yo"));
    CHECK_EQ(L[1].text, std::string("Okay.\\@"));
    CHECK_EQ(L[3].text, std::string(""));
    CHECK_EQ(L[5].text, std::string("She shakes her head.\\@"));
    CHECK_EQ(L[8].text, std::string("A novel.\\n(Two lines)"));
    CHECK_EQ(L[9].text, std::string(""));
    CHECK_EQ(L[14].text, std::string("if (#1 == 0) 1 next1 Choice"));
    CHECK_EQ(L[15].text, std::string("if (#1 != 0) 999 next1 Choice"));
    CHECK_EQ(L[8].utf8, true);
    CHECK_EQ(L[0].own, std::string("Y"));  // the name table's key

    // The scene's own language: left as written.
    CstScene own;
    own.lines() = {{T::Message, "j0"}, {T::Input, ""}, {T::Message, "j1"}, {T::Input, ""}};
    CHECK_EQ(localizeScene(own, t, 1), false);
    CHECK_EQ(own.lines()[0].text, std::string("j0"));
}

void testFesEmptyLabels() {
    // Labels one after another are separate empty blocks, not aliases of the code below.
    auto s = nyan::cs2::FesScript::parse("#START\n#SLEEP\n#btn.FOCUS\n$1 = 1\n#END\n");
    size_t start = 0, end = 0;
    CHECK_EQ(s.block("SLEEP", start, end), true);
    CHECK_EQ(end - start, size_t{0});
    CHECK_EQ(s.block("btn.FOCUS", start, end), true);
    CHECK_EQ(end - start, size_t{1});
}

// A game folder that only lists file names (all engine detection looks at so far).
class ListedFolder final : public nyan::GameFolder {
public:
    explicit ListedFolder(std::vector<std::string> names) : names_(std::move(names)) {}
    std::vector<std::string> list() const override { return names_; }
    std::unique_ptr<nyan::RandomAccessFile> open(const std::string&) const override { return nullptr; }
    std::string displayName() const override { return "test"; }

private:
    std::vector<std::string> names_;
};

// The FE update's waits and skips as Amakano 1's cs2.exe has them (0x60de30): the checks see the
// motions as the update before left them, a bare `wait` runs no statements in the update that finds
// nothing moving, loops count as moving until stopped, and a held Force Skip key is events 0x13 /
// 0x14 (0x52eb08).
void testFesWaitTiming() {
    struct TestHost final : nyan::cs2::FesInstance::Host {
        bool held = false;
        void execKcs(const std::string&) override {}
        void endKcs(const std::string&) override {}
        int32_t kcsExitCode(const std::string&) override { return -1; }
        bool forceSkipHeld() override { return held; }
    } host;
    const nyan::cs2::AssetIndex assets;  // no archives: the objects have no images
    nyan::cs2::VarStore vars;
    auto script = nyan::cs2::FesScript::parse(
        "#SUBCOMMAND\n"
        "oneshot move motMove p[30] px[0] py[0] pz[0] : [0] 0 0 0 [1] [2] [3]\n"
        "continue mcrot mcRot p[0] p[0] p[0] : [0] [1] [2]\n"
        "#DEFINE\n"
        "IMAGE a\n"
        "IMAGE b\n"
        "#START\n"
        "a move 10 100 0\n"
        "wait\n"
        "$1 = _FRAME_\n"
        "b mcrot 120 0 360\n"
        "wait 1\n"
        "b ismot 2\n"
        "a ismot 3\n"
        "wait\n"
        "$4 = _FRAME_\n"
        "a move 10 0 0\n"
        "wait a\n"
        "$5 = _FRAME_\n"
        "keyskip ctrl\n"
        "a move 100 100 0\n"
        "wait\n"
        "$6 = _FRAME_\n"
        "$7 = _CLICK_L_\n"
        "wait 1\n"
        "$8 = _CLICK_L_\n"
        "wait 1\n"
        "$9 = _CLICK_L_\n"
        "#END\n");
    auto fes = nyan::cs2::FesInstance::create(assets, vars, "t", std::move(script), 0, nullptr, &host);
    uint64_t f = 1;
    auto runTo = [&](uint64_t last) {
        for (; f <= last; ++f) fes->tick(f);
    };
    // Frame 1: the move (ends at its update in frame 11), then `wait`. Frame 12 finds nothing
    // moving; the statements go on in frame 13.
    runTo(12);
    CHECK_EQ(vars.get(1), 0);
    runTo(13);
    CHECK_EQ(vars.get(1), 13);
    // The loop counts as moving; the bare `wait` holds until a click stops it (frame 30: the skip;
    // 31 finds nothing moving; 32 goes on).
    runTo(29);
    CHECK_EQ(vars.get(2), 1);
    CHECK_EQ(vars.get(3), 0);
    CHECK_EQ(vars.get(4), 0);
    fes->pressSkip();
    runTo(31);
    CHECK_EQ(vars.get(4), 0);
    runTo(32);
    CHECK_EQ(vars.get(4), 32);
    // `wait <object>` goes on in the update that finds it still: the move from 32 ends at 42.
    runTo(43);
    CHECK_EQ(vars.get(5), 43);
    // The held key at frame 50 skips the move; `_CLICK_L_` is the hold's -1 and stays while held
    // under `keyskip ctrl`; letting go clears it.
    runTo(49);
    CHECK_EQ(vars.get(6), 0);
    host.held = true;
    runTo(52);
    CHECK_EQ(vars.get(6), 52);
    CHECK_EQ(vars.get(7), -1);
    runTo(53);
    CHECK_EQ(vars.get(8), -1);
    host.held = false;
    runTo(54);
    CHECK_EQ(vars.get(9), 0);
}

void testEngineDetection() {
    using nyan::Engine;
    CHECK_EQ(nyan::detectEngine(ListedFolder({"grisaia.exe", "scene.int", "image.int"})),
             std::optional{Engine::CatSystem2});
    CHECK_EQ(nyan::detectEngine(ListedFolder({"UPDATE00.INT"})), std::optional{Engine::CatSystem2});
    CHECK_EQ(nyan::detectEngine(ListedFolder({"game.exe", "data.arc", "readme.txt"})).has_value(), false);
    CHECK_EQ(nyan::detectEngine(ListedFolder({})).has_value(), false);
    CHECK_EQ(std::string(nyan::engineName(Engine::CatSystem2)), std::string("CatSystem2"));
}

int main() {
    testAnm();
    testFontStyle();
    testCff2Font();
    testCgKey();
    testSceneTranslation();
    testFesEmptyLabels();
    testFesWaitTiming();
    testUtf8ToSjis();
    testSaveRoundTrip();
    testExpr();
    testMessage();
    testMersenneTwister();
    testCursorFile();
    testConcent();
    testDigi();
    testDarkTri();
    testFilmNoise();
    testEnergy();
    testSteam();
    testDash();
    testRising4();
    testRingWave();
    testWcMirror();
    testPaper();
    testWcSlash();
    testRubble();
    testWcThunder();
    testProtect();
    testStar();
    testExprode();
    testCmnptcl();
    testWcDark();
    testRadiate();
    testRain();
    testSpark2();
    testS040101();
    testWcRound();
    testWcSteam();
    testSwordSpark();
    testSArrow();
    testCFadeMotion();
    testDispMotion();
    testImageMode();
    testKcsNativeCalls();
    testParticleOrder();
    testVCodeSeed();
    testBlowfish();
    testKifNameCipher();
    testKifSeedSearch();
    testLooseFolders();
    testFesTables();
    testSjis();
    testEngineDetection();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("All core tests passed");
    return 0;
}
