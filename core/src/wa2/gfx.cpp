#include "nyan/wa2/gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nyan/common/error.hpp"

namespace nyan::wa2 {

namespace {

bool validObj(int obj) {
    return obj >= 0 && obj < kObjectCount;
}

bool validSlot(int slot) {
    return slot >= 0 && slot < kSlotCount;
}

// The tiled path's texture size for a picture side n (0x43ffa0), M the largest tile.
int tileRound(int n, int m) {
    if (n > m / 2) return (n + m - 1) / m * m;
    if (n <= 16) return 16;
    if (n <= 32) return 32;
    if (n <= 64) return 64;
    return 128;
}

int nextPow2(int n) {
    int p = 16;
    while (p < n && p < 4096) p *= 2;
    return p;
}

uint32_t argb(int a, int r, int g, int b) {
    return static_cast<uint32_t>(a & 0xff) << 24 | static_cast<uint32_t>(r & 0xff) << 16 |
           static_cast<uint32_t>(g & 0xff) << 8 | static_cast<uint32_t>(b & 0xff);
}

int chan(uint32_t c, int shift) {
    return static_cast<int>(c >> shift & 0xff);
}

// A corner's colours after the tint group (0x43bb4f..0x43beea): R, G, B, A and the specular R, G, B.
struct CornerColor {
    std::array<float, 4> d{};  // diffuse R, G, B, A
    std::array<float, 3> s{};  // specular R, G, B
};

CornerColor tinted(uint32_t diffuse, uint32_t specular, const Gfx::Tint& t) {
    CornerColor out;
    int dc[3] = {chan(diffuse, 16), chan(diffuse, 8), chan(diffuse, 0)};
    int sc[3] = {chan(specular, 16), chan(specular, 8), chan(specular, 0)};
    if (t.mode == 0 || t.mode == 1) {
        int tc[3] = {t.r, t.g, t.b};
        for (int i = 0; i < 3; ++i) {
            int sum = dc[i] + sc[i];
            int v;
            if (t.mode == 0) v = tc[i] <= 128 ? (sum * tc[i]) >> 7 : sum + ((tc[i] - 128) * (510 - sum)) / 127;
            else v = tc[i] <= 128 ? sum - 512 + 4 * tc[i] : sum - 510 + 4 * tc[i];
            v = std::clamp(v, 0, 510);
            if (v < 255) {
                dc[i] = v;
                sc[i] = 0;
            } else {
                dc[i] = 255;
                sc[i] = std::min(v - 255, 255);
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        out.d[static_cast<size_t>(i)] = static_cast<float>(dc[i]);
        out.s[static_cast<size_t>(i)] = static_cast<float>(sc[i]);
    }
    out.d[3] = static_cast<float>(chan(diffuse, 24));
    return out;
}

CornerColor lerp(const CornerColor& a, const CornerColor& b, float t) {
    CornerColor out;
    for (size_t i = 0; i < 4; ++i) out.d[i] = a.d[i] + (b.d[i] - a.d[i]) * t;
    for (size_t i = 0; i < 3; ++i) out.s[i] = a.s[i] + (b.s[i] - a.s[i]) * t;
    return out;
}

// x87 FISTP in its default mode: to nearest, ties to even.
uint8_t roundByte(float v) {
    return static_cast<uint8_t>(std::clamp(std::nearbyint(v), 0.0f, 255.0f));
}

DrawVertex vertex(float x, float y, float u, float v, const CornerColor& c) {
    DrawVertex out;
    out.x = x;
    out.y = y;
    out.u = u;
    out.v = v;
    for (size_t i = 0; i < 4; ++i) out.diffuse[i] = roundByte(c.d[i]);
    for (size_t i = 0; i < 3; ++i) out.specular[i] = roundByte(c.s[i]);
    return out;
}

}  // namespace

// ---- Slots -------------------------------------------------------------------------------------------

bool Gfx::load(int slot, std::string_view file, uint32_t flags, bool half, std::string_view amp1,
               std::string_view amp2, int level, std::string_view group, int maxTile, bool single) {
    if (!validSlot(slot)) return false;
    release(slot);
    ImageSlot& s = slots[static_cast<size_t>(slot)];
    int type = static_cast<int>(flags & 0xff);
    s.type = static_cast<uint8_t>(type);
    s.single = single;
    s.half = half;
    s.file = std::string(file);
    s.amp1 = std::string(amp1);
    s.amp2 = std::string(amp2);
    s.group = group.empty() ? std::string("grp") : std::string(group);
    s.level = level;
    Picture p;
    if (keepPixels) {
        auto data = assets_.read(s.group, s.file);
        if (!data) return false;
        try {
            p = decodePicture(*data, s.file);
        } catch (const Error&) {
            return false;
        }
    } else {
        auto head = assets_.readHead(s.group, s.file, 64);
        if (!head || !pictureHeader(*head, s.file, p.width, p.height, p.bpp)) return false;
    }
    s.picW = p.width;
    s.picH = p.height;
    // A mask from a 24- or 32-bit picture keeps its own texels (alpha from the file); the mirror flag
    // is not applied to it and the invert flag inverts all four bytes.
    bool deepMask = type == 3 && single && p.bpp != 8;
    if (deepMask) flags &= ~0x100u;
    auto t = static_cast<PictureType>(type == 2 || type == 5 || deepMask ? 1 : type);
    if (type == 3 && !single) {
        // The tiled path keeps a mask as a CPU array only: no texture to draw.
        if (p.bpp != 8) return false;
        s.texW = s.tileW = p.width;
        s.texH = s.tileH = p.height;
        s.tilesX = s.tilesY = 1;
        s.loaded = true;
        return true;
    }
    // Mirror and flip (0x100 / 0x200) on the top-down picture.
    if (keepPixels && (flags & 0x300)) {
        int bpp = p.bpp / 8;
        Bytes out(p.pixels.size());
        size_t row = static_cast<size_t>(p.width) * static_cast<size_t>(bpp);
        for (int y = 0; y < p.height; ++y) {
            int sy = (flags & 0x200) ? p.height - 1 - y : y;
            for (int x = 0; x < p.width; ++x) {
                int sx = (flags & 0x100) ? p.width - 1 - x : x;
                std::memcpy(&out[static_cast<size_t>(y) * row + static_cast<size_t>(x * bpp)],
                            &p.pixels[static_cast<size_t>(sy) * row + static_cast<size_t>(sx * bpp)], static_cast<size_t>(bpp));
            }
        }
        p.pixels = std::move(out);
    }
    RgbaImage img;
    if (keepPixels) {
        preparePalette(p, t);
        applyLevel(p, level);
        for (const std::string* a : {&s.amp1, &s.amp2}) {
            if (a->empty()) continue;
            if (auto table = assets_.read("grp", *a)) applyAmp(p, *table);
        }
        img = toRgba(p, t);
        if (img.pixels.empty()) return false;
        if (single && (flags & 0x400)) {
            for (size_t i = 0; i < img.pixels.size(); ++i) {
                if (type == 3 && !deepMask && i % 4 != 3) continue;
                img.pixels[i] = static_cast<uint8_t>(255 - img.pixels[i]);
            }
        }
    } else if (type == 3 && p.bpp != 8 && !deepMask) {
        return false;
    }
    int w = p.width, h = p.height;
    if (single) {
        s.texW = s.tileW = nextPow2(w);
        s.texH = s.tileH = nextPow2(h);
        s.tilesX = s.tilesY = 1;
    } else {
        int m = maxTile > 0 ? maxTile : 0x100;
        int tw = tileRound(w, m), th = tileRound(h, m);
        if (tw > th * 16) th = tileRound(tw / 16, m);
        else if (th > tw * 16) tw = tileRound(th / 16, m);
        int tile = std::min(tw, th) <= m ? std::min(tw, th) : m;
        s.texW = tw;
        s.texH = th;
        s.tileW = std::min(tile, tw);
        s.tileH = std::min(tile, th);
        s.tilesX = tw / tile;
        s.tilesY = th / tile;
    }
    if (keepPixels) {
        s.pixels.width = s.texW;
        s.pixels.height = s.texH;
        s.pixels.pixels.assign(static_cast<size_t>(s.texW) * static_cast<size_t>(s.texH) * 4, 0);
        int cw = std::min(w, s.texW), ch = std::min(h, s.texH);
        for (int y = 0; y < ch; ++y) {
            std::memcpy(&s.pixels.pixels[static_cast<size_t>(y) * static_cast<size_t>(s.texW) * 4],
                        &img.pixels[static_cast<size_t>(y) * static_cast<size_t>(w) * 4], static_cast<size_t>(cw) * 4);
        }
    }
    s.hasTexture = true;
    s.loaded = true;
    s.version = ++versions_;
    return true;
}

void Gfx::loadPixels(int slot, RgbaImage image) {
    if (!validSlot(slot)) return;
    release(slot);
    ImageSlot& s = slots[static_cast<size_t>(slot)];
    s.type = 2;
    s.single = true;
    s.picW = image.width;
    s.picH = image.height;
    s.texW = s.tileW = image.width;
    s.texH = s.tileH = image.height;
    s.tilesX = s.tilesY = 1;
    s.pixels = std::move(image);
    s.hasTexture = true;
    s.loaded = true;
    s.version = ++versions_;
}

void Gfx::makeTarget(int slot, int w, int h) {
    if (!validSlot(slot)) return;
    release(slot);
    ImageSlot& s = slots[static_cast<size_t>(slot)];
    s.loaded = true;
    s.type = 1;
    s.single = true;
    s.texW = s.tileW = s.picW = w;
    s.texH = s.tileH = s.picH = h;
    s.tilesX = s.tilesY = 1;
    s.rtState = 2;
    s.version = ++versions_;
}

void Gfx::release(int slot) {
    if (!validSlot(slot)) return;
    slots[static_cast<size_t>(slot)] = ImageSlot{};
    slots[static_cast<size_t>(slot)].version = ++versions_;
}

void Gfx::reset() {
    for (int s = 0; s < kSlotCount; ++s) {
        if (s >= 0x1c4 && s <= 0x1c8) continue;
        if (slots[static_cast<size_t>(s)].loaded) release(s);
    }
    objects.fill(SpriteObject{});
    texts.fill(TextSlot{});
    tints.fill(Tint{});
}

void Gfx::copySlot(int dst, int src) {
    if (!validSlot(dst) || !validSlot(src) || dst == src) return;
    slots[static_cast<size_t>(dst)] = slots[static_cast<size_t>(src)];
    slots[static_cast<size_t>(dst)].version = ++versions_;
}

bool Gfx::pictureSize(int slot, int& w, int& h) const {
    if (!validSlot(slot)) return false;
    const ImageSlot& s = slots[static_cast<size_t>(slot)];
    w = s.picW;
    h = s.picH;
    return s.loaded;
}

// ---- Objects -----------------------------------------------------------------------------------------

void Gfx::remove(int obj) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)] = SpriteObject{};
}

void Gfx::makeImage(int obj, int slot, int prio, bool visible) {
    if (!validObj(obj)) return;
    SpriteObject o;
    o.used = true;
    o.visible = visible;
    o.priority = static_cast<int16_t>(prio);
    o.slot = static_cast<int16_t>(slot);
    float w = 0, h = 0;
    if (validSlot(slot)) {
        w = static_cast<float>(slots[static_cast<size_t>(slot)].texW);
        h = static_cast<float>(slots[static_cast<size_t>(slot)].texH);
    }
    o.x = {0, w, 0, w};
    o.y = {0, 0, h, h};
    o.u1 = w;
    o.v1 = h;
    objects[static_cast<size_t>(obj)] = std::move(o);
}

void Gfx::makeNineSlice(int obj, int slot, int prio, bool visible) {
    makeImage(obj, slot, prio, visible);
    if (validObj(obj)) objects[static_cast<size_t>(obj)].kind = 6;
}

void Gfx::makePolygon(int obj, int n, int prio, bool visible) {
    if (!validObj(obj)) return;
    SpriteObject o;
    o.used = true;
    o.visible = visible;
    o.priority = static_cast<int16_t>(prio);
    o.vertexCount = static_cast<int8_t>(n);
    o.x = {0, kScreenWidth, 0, kScreenWidth};
    o.y = {0, 0, kScreenHeight, kScreenHeight};
    objects[static_cast<size_t>(obj)] = std::move(o);
}

void Gfx::makeString(int obj, int slot, int prio, bool visible, std::string_view s) {
    makeImage(obj, slot, prio, visible);
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.kind = 7;
    o.text = std::string(s);
    float w = static_cast<float>(static_cast<int>(o.u1) / 10);
    o.x = {0, w, 0, w};
}

void Gfx::makeNumber(int obj, int slot, int prio, bool visible, int value, int digits, int spacing) {
    makeImage(obj, slot, prio, visible);
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.kind = 8;
    o.value = value;
    o.digits = static_cast<int16_t>(digits);
    o.digitSpacing = static_cast<int16_t>(spacing);
    float w = static_cast<float>(static_cast<int>(o.u1) / 10);
    o.x = {0, w, 0, w};
}

void Gfx::makeText(int obj, int record, int prio, bool visible) {
    if (!validObj(obj) || record < 0 || record >= static_cast<int>(texts.size())) return;
    const TextSlot& t = texts[static_cast<size_t>(record)];
    SpriteObject o;
    o.used = true;
    o.visible = visible;
    o.priority = static_cast<int16_t>(prio);
    o.kind = 0xb;
    o.slot = static_cast<int16_t>(t.atlas);
    o.textRecord = static_cast<int16_t>(record);
    // The record's columns and rows as its corners (only the first corner is used).
    auto fw = static_cast<float>(t.cols), fh = static_cast<float>(t.rows);
    o.x = {0, fw, 0, fw};
    o.y = {0, 0, fh, fh};
    o.u1 = fw;
    o.v1 = fh;
    objects[static_cast<size_t>(obj)] = std::move(o);
}

// ---- Setters -----------------------------------------------------------------------------------------

void Gfx::setRect(int obj, float x, float y, float u, float v, float w, float h) {
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.x = {x, x + w, x, x + w};
    o.y = {y, y, y + h, y + h};
    o.u0 = u;
    o.u1 = u + w;
    o.v0 = v;
    o.v1 = v + h;
    if (o.kind == 1) o.pivotX = x, o.pivotY = y;
}

void Gfx::setDest(int obj, float x, float y, float w, float h) {
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.x = {x, x + w, x, x + w};
    o.y = {y, y, y + h, y + h};
    if (o.kind == 1) o.pivotX = x, o.pivotY = y;
}

void Gfx::setSource(int obj, float u, float v, float w, float h) {
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.u0 = u;
    o.u1 = u + w;
    o.v0 = v;
    o.v1 = v + h;
}

void Gfx::moveTo(int obj, float x, float y) {
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    float dx = x - o.x[0], dy = y - o.y[0];
    for (size_t i = 0; i < 4; ++i) o.x[i] += dx, o.y[i] += dy;
    if (o.kind == 1) o.pivotX = x, o.pivotY = y;
}

void Gfx::moveSource(int obj, float u, float v) {
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.u1 = u + (o.u1 - o.u0);
    o.v1 = v + (o.v1 - o.v0);
    o.u0 = u;
    o.v0 = v;
}

void Gfx::setPivot(int obj, float x, float y) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].pivotX = x, objects[static_cast<size_t>(obj)].pivotY = y;
}

void Gfx::setRotation(int obj, float degrees) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].rotation = degrees;
}

void Gfx::setScale(int obj, float s) {
    setScale(obj, s, s);
}

void Gfx::setScale(int obj, float sx, float sy) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].scaleX = sx, objects[static_cast<size_t>(obj)].scaleY = sy;
}

void Gfx::setVisible(int obj, bool on) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].visible = on;
}

void Gfx::setPriority(int obj, int prio) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].priority = static_cast<int16_t>(prio);
}

void Gfx::setMask(int obj, uint32_t mask) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].mask = mask;
}

void Gfx::setTarget(int obj, int rtSlot) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].target = static_cast<int16_t>(rtSlot);
}

void Gfx::setSlot(int obj, int slot) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].slot = static_cast<int16_t>(slot);
}

void Gfx::setMaskSlot(int obj, int slot, int soft) {
    if (!validObj(obj)) return;
    objects[static_cast<size_t>(obj)].maskSlot = static_cast<int16_t>(slot);
    objects[static_cast<size_t>(obj)].maskSoft = static_cast<int16_t>(soft);
}

void Gfx::clearMaskSlot(int obj) {
    // 0x43acf0 would also force the picture's texel alpha to 0xff when it is an A8R8G8B8 texture; its
    // only callers use it on render-target display layers, which cannot be locked, so nothing changes.
    if (!validObj(obj)) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    if (o.slot != -1 && o.maskSlot != -1) o.maskSlot = -1;
}

void Gfx::setShader(int obj, int shader, float c0w) {
    // Only when the shader exists: pf00..pf03.
    if (!validObj(obj) || shader < 0 || shader > 3) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    o.shaderOn = true;
    o.shader = static_cast<uint32_t>(shader);
    o.c0 = {0, 0, 0, c0w};
}

void Gfx::setBlend(int obj, uint32_t mode) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].blend = mode;
}

void Gfx::setFilter(int obj, uint32_t filter) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].filter = filter;
}

void Gfx::setMirror(int obj, uint32_t flags) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].mirror = flags;
}

void Gfx::setCornerColor(int obj, int corner, int a, int r, int g, int b) {
    // The 128-neutral encoding of a textured object: below 128 the diffuse scales, above it the
    // specular adds. Untextured objects take the bytes as they are.
    if (!validObj(obj) || corner < 0 || corner > 3) return;
    SpriteObject& o = objects[static_cast<size_t>(obj)];
    a = std::clamp(a, 0, 255);
    auto c = static_cast<size_t>(corner);
    if (o.slot >= 0) {
        int d[3], s[3];
        int in[3] = {r, g, b};
        for (int i = 0; i < 3; ++i) {
            if (in[i] < 128) {
                d[i] = std::max(2 * in[i], 0);
                s[i] = 0;
            } else {
                d[i] = 255;
                s[i] = std::min(2 * in[i] - 256, 255);
            }
        }
        o.diffuse[c] = argb(a, d[0], d[1], d[2]);
        o.specular[c] = argb(0xff, s[0], s[1], s[2]);
    } else {
        o.diffuse[c] = argb(a, r, g, b);
        o.specular[c] = 0xff000000;
    }
}

void Gfx::setColor(int obj, int a, int r, int g, int b) {
    for (int c = 0; c < 4; ++c) setCornerColor(obj, c, a, r, g, b);
}

void Gfx::setBrightness(int obj, int v) {
    if (!validObj(obj)) return;
    for (int c = 0; c < 4; ++c) setCornerColor(obj, c, chan(objects[static_cast<size_t>(obj)].diffuse[static_cast<size_t>(c)], 24), v, v, v);
}

void Gfx::setTone(int obj, int r, int g, int b) {
    if (!validObj(obj)) return;
    for (int c = 0; c < 4; ++c) setCornerColor(obj, c, chan(objects[static_cast<size_t>(obj)].diffuse[static_cast<size_t>(c)], 24), r, g, b);
}

void Gfx::setAlpha(int obj, int a) {
    if (!validObj(obj)) return;
    a = std::clamp(a, 0, 255);
    for (auto& d : objects[static_cast<size_t>(obj)].diffuse) d = (d & 0x00ffffff) | static_cast<uint32_t>(a) << 24;
}

void Gfx::setTintGroup(int obj, int group) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].tintGroup = static_cast<uint8_t>(group);
}

void Gfx::setTint(int r, int g, int b, int mode) {
    tints[0] = {std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), mode};
}

void Gfx::setString(int obj, std::string_view s) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].text = std::string(s);
}

void Gfx::setValue(int obj, int value) {
    if (validObj(obj)) objects[static_cast<size_t>(obj)].value = value;
}

// ---- The frame ---------------------------------------------------------------------------------------

// ---- Text records -----------------------------------------------------------------------------------

void Gfx::makeTextSlot(int index, int atlas, const std::string& text, int size, int cols, int rows, int spacing,
                       int gap, int em, int cell, bool outline, int r, int g, int b, int or_, int og, int ob, bool ruby) {
    if (index < 0 || index >= static_cast<int>(texts.size())) return;
    TextSlot t;
    t.active = true;
    t.atlas = atlas;
    t.text = text.substr(0, 1023);  // wsprintfA's limit
    t.base = size;
    t.cols = cols;
    t.rows = rows;
    t.spacing = spacing;
    t.gap = gap;
    t.em = em;
    t.cell = cell;
    t.outline = outline;
    t.r = static_cast<uint8_t>(r);
    t.g = static_cast<uint8_t>(g);
    t.b = static_cast<uint8_t>(b);
    t.outlineR = static_cast<uint8_t>(or_);
    t.outlineG = static_cast<uint8_t>(og);
    t.outlineB = static_cast<uint8_t>(ob);
    t.ruby = ruby;
    texts[static_cast<size_t>(index)] = std::move(t);
}

void Gfx::setTextString(int index, const std::string& text, int r, int g, int b, int or_, int og, int ob) {
    if (index < 0 || index >= static_cast<int>(texts.size())) return;
    TextSlot& t = texts[static_cast<size_t>(index)];
    if (!t.active) return;
    setTextColor(index, r, g, b, or_, og, ob);
    t.text = text;
    t.endReached = t.complete = false;
    t.page = t.reveal = -1;
}

void Gfx::setTextColor(int index, int r, int g, int b, int or_, int og, int ob) {
    if (index < 0 || index >= static_cast<int>(texts.size())) return;
    TextSlot& t = texts[static_cast<size_t>(index)];
    if (!t.active) return;
    if (r != -1) {
        t.r = static_cast<uint8_t>(r);
        t.g = static_cast<uint8_t>(g);
        t.b = static_cast<uint8_t>(b);
    }
    if (or_ != -1) {
        t.outlineR = static_cast<uint8_t>(or_);
        t.outlineG = static_cast<uint8_t>(og);
        t.outlineB = static_cast<uint8_t>(ob);
    }
}

void Gfx::releaseTextSlot(int index) {
    if (index >= 0 && index < static_cast<int>(texts.size())) texts[static_cast<size_t>(index)] = TextSlot{};
}

bool Gfx::setTextPage(int obj, int page) {
    if (!validObj(obj)) return false;
    TextSlot& t = texts[static_cast<size_t>(objects[static_cast<size_t>(obj)].textRecord) & 0x7f];
    t.page = page;
    return t.endReached;
}

bool Gfx::setTextReveal(int obj, int reveal) {
    if (!validObj(obj)) return false;
    TextSlot& t = texts[static_cast<size_t>(objects[static_cast<size_t>(obj)].textRecord) & 0x7f];
    t.reveal = reveal;
    return t.complete;
}

void Gfx::textAnchor(int obj, int& x, float& y) const {
    if (!validObj(obj)) return;
    const TextSlot& t = texts[static_cast<size_t>(objects[static_cast<size_t>(obj)].textRecord) & 0x7f];
    x = t.endX;
    y = t.endY;
}

// ---- The frame ---------------------------------------------------------------------------------------

std::vector<int16_t> Gfx::sorted() {
    // 0x442073: insertion by signed priority; an object goes before equal priorities, so of equal
    // priorities the lower index is drawn last. 30000 marks a free entry.
    std::array<int16_t, kObjectCount> prio;
    std::array<int16_t, kObjectCount> order;
    prio.fill(30000);
    order.fill(-1);
    for (int obj = 0; obj < kObjectCount; ++obj) {
        const SpriteObject& o = objects[static_cast<size_t>(obj)];
        if (!o.used || !o.visible || o.mask == 0) continue;
        for (int i = 0; i <= obj; ++i) {
            if (prio[static_cast<size_t>(i)] >= o.priority) {
                if (prio[static_cast<size_t>(i)] != 30000) {
                    for (int k = obj; k > i; --k) {
                        prio[static_cast<size_t>(k)] = prio[static_cast<size_t>(k - 1)];
                        order[static_cast<size_t>(k)] = order[static_cast<size_t>(k - 1)];
                    }
                }
                prio[static_cast<size_t>(i)] = o.priority;
                order[static_cast<size_t>(i)] = static_cast<int16_t>(obj);
                break;
            }
        }
    }
    std::vector<int16_t> out;
    for (int16_t obj : order) {
        if (obj >= 0) out.push_back(obj);
    }
    return out;
}

std::vector<DrawCommand> Gfx::buildFrame(bool clear) {
    std::vector<DrawCommand> out;
    currentTarget_ = -1;
    drawn_ = true;
    if (clear) {
        DrawCommand c;
        c.prim = DrawCommand::Prim::Clear;
        out.push_back(std::move(c));
    }
    for (int16_t obj : sorted()) {
        const SpriteObject& o = objects[static_cast<size_t>(obj)];
        if (!o.used || !o.visible) continue;
        drawKind(o, out);
    }
    if (overlay) {
        currentTarget_ = -1;
        overlay([&](const SpriteObject& piece) { quad(piece, out); });
    }
    return out;
}

void Gfx::layoutTexts() {
    auto none = [](const SpriteObject&) {};
    for (int16_t obj : sorted()) {
        const SpriteObject& o = objects[static_cast<size_t>(obj)];
        if (!o.used || !o.visible || o.kind != 0xb) continue;
        TextSlot& t = texts[static_cast<size_t>(o.textRecord) & 0x7f];
        if (t.outline) t.complete = renderText(o, t, true, none);
        t.complete = renderText(o, t, false, none);
    }
}

void Gfx::drawKind(const SpriteObject& o, std::vector<DrawCommand>& out) {
    auto draw = [&](const SpriteObject& piece) { quad(piece, out); };
    switch (o.kind) {
    case 0:
    case 2:
        quad(o, out);
        break;
    case 1:
        if (drawAni) drawAni(o, draw);
        break;
    case 6: {
        // Nine-slice (0x4421e3): borders 1:1, the centre stretched.
        auto cw = static_cast<float>(static_cast<int>((o.u1 - o.u0) / 3));
        auto ch = static_cast<float>(static_cast<int>((o.v1 - o.v0) / 3));
        float xs[4] = {o.x[0], o.x[0] + cw, o.x[1] - cw, o.x[1]};
        float ys[4] = {o.y[0], o.y[0] + ch, o.y[2] - ch, o.y[2]};
        for (int k = 0; k < 9; ++k) {
            int col = k % 3, row = k / 3;
            SpriteObject p = o;
            p.x = {xs[col], xs[col + 1], xs[col], xs[col + 1]};
            p.y = {ys[row], ys[row], ys[row + 1], ys[row + 1]};
            p.u0 = o.u0 + static_cast<float>(col) * cw;
            p.u1 = p.u0 + cw;
            p.v0 = o.v0 + static_cast<float>(row) * ch;
            p.v1 = p.v0 + ch;
            quad(p, out);
        }
        break;
    }
    case 7: {
        // ASCII string (0x442664): 16 glyphs per row from the space on.
        float uw = o.u1 - o.u0, vh = o.v1 - o.v0;
        int k = 0, line = 0;
        for (char ch : o.text) {
            if (ch == '\n') {
                k = 0;
                ++line;
                continue;
            }
            if (ch == ' ') {
                ++k;
                continue;
            }
            auto c = static_cast<uint8_t>(static_cast<uint8_t>(ch) - 0x20);
            int col = c % 16, row = c / 16;
            auto fk = static_cast<float>(k), fl = static_cast<float>(line);
            SpriteObject p = o;
            p.x[0] = o.x[0] + fk * (o.x[1] - o.x[0]);
            p.x[1] = p.x[0] + (o.x[1] - o.x[0]);
            p.x[2] = o.x[2] + fk * (o.x[3] - o.x[2]);
            p.x[3] = p.x[2] + (o.x[3] - o.x[2]);
            p.y[0] = o.y[0] + fl * (o.y[2] - o.y[0]);
            p.y[1] = o.y[1] + fl * (o.y[3] - o.y[1]);
            p.y[2] = p.y[0] + (o.y[2] - o.y[0]);
            p.y[3] = p.y[1] + (o.y[3] - o.y[1]);
            p.u0 = o.u0 + static_cast<float>(col) * uw;
            p.u1 = o.u1 + static_cast<float>(col) * uw;
            p.v0 = o.v0 + static_cast<float>(row) * vh;
            p.v1 = o.v1 + static_cast<float>(row) * vh;
            quad(p, out);
            ++k;
        }
        break;
    }
    case 8: {
        // Number (0x4429b7): from the last digit; digits > 0 right-aligned, < 0 with leading zeros.
        int n = o.digits;
        int value = o.value;
        if (n == 0) {
            n = 1;
            for (int v = value / 10; v != 0; v /= 10) ++n;
        } else if (n < 0) {
            n = -n;
        }
        float uw = o.u1 - o.u0;
        for (int i = 1;; ++i) {
            int p = n - i;
            int d = value % 10;
            auto fp = static_cast<float>(p);
            float sp = static_cast<float>(o.digitSpacing) * fp;
            SpriteObject q = o;
            q.x[0] = o.x[0] + fp * (o.x[1] - o.x[0]) + sp;
            q.x[2] = o.x[2] + fp * (o.x[3] - o.x[2]) + sp;
            q.x[1] = q.x[0] + (o.x[1] - o.x[0]);
            q.x[3] = q.x[2] + (o.x[3] - o.x[2]);
            q.u0 = o.u0 + static_cast<float>(d) * uw;
            q.u1 = o.u1 + static_cast<float>(d) * uw;
            quad(q, out);
            value /= 10;
            if (!(i < n && (o.digits < 1 || value != 0))) break;
        }
        break;
    }
    case 0xb: {
        // 0x442c26: an outline pass first when the record has one, then the body.
        TextSlot& t = texts[static_cast<size_t>(o.textRecord) & 0x7f];
        if (t.outline) t.complete = renderText(o, t, true, draw);
        t.complete = renderText(o, t, false, draw);
        break;
    }
    default:
        break;  // 3 (grid) and 9 (text slot) have no creator in the game; 4, 5, 10, 12+ draw nothing
    }
}

void Gfx::quad(const SpriteObject& o, std::vector<DrawCommand>& out) {
    // 0x43b590. The render target: the screen, a render target (made and cleared on first use), or
    // for a slot that is no render target, whatever the previous object left.
    if (o.target < 0) {
        currentTarget_ = -1;
    } else if (validSlot(o.target)) {
        ImageSlot& rt = slots[static_cast<size_t>(o.target)];
        if (rt.rtState == 2) {
            DrawCommand c;
            c.prim = DrawCommand::Prim::MakeTarget;
            c.target = o.target;
            out.push_back(std::move(c));
            rt.rtState = 1;
            currentTarget_ = o.target;
        } else if (rt.rtState == 1) {
            currentTarget_ = o.target;
        }
    }

    // Geometry (0x43b961): about the absolute pivot; the shift for a negative source origin uses the
    // y scale for both axes.
    float th = o.rotation * 0.01745329238f;
    auto c = static_cast<float>(std::cos(static_cast<double>(th)));
    auto s = static_cast<float>(std::sin(static_cast<double>(th)));
    float ox = o.u0 < 0 ? o.u0 : 0, oy = o.v0 < 0 ? o.v0 : 0;
    std::array<float, 4> X, Y;
    for (size_t i = 0; i < 4; ++i) {
        float dx = o.x[i] - o.pivotX, dy = o.y[i] - o.pivotY;
        if (o.mirror & 1) dx = -dx;
        if (o.mirror & 2) dy = -dy;
        X[i] = (c * dx + s * dy) * o.scaleX + o.pivotX - o.scaleY * ox;
        Y[i] = (c * dy - s * dx) * o.scaleY + o.pivotY - o.scaleY * oy;
    }
    // Colours through the tint group.
    Tint none;
    const Tint& tint = o.tintGroup < tints.size() ? tints[o.tintGroup] : none;
    std::array<CornerColor, 4> col;
    for (size_t i = 0; i < 4; ++i) col[i] = tinted(o.diffuse[i], o.specular[i], tint);

    DrawCommand cmd;
    cmd.target = currentTarget_;
    cmd.blend = o.blend;

    if (o.slot < 0) {
        // Untextured (0x43dd9c): the diffuse colour only, no half-pixel correction.
        for (auto& cc : col) cc.s = {0, 0, 0};
        auto v = [&](size_t i) { return vertex(X[i] + 0.5f, Y[i] + 0.5f, 0, 0, col[i]); };
        if (o.slot == -1) {
            switch (o.vertexCount) {
            case 2:
                cmd.prim = DrawCommand::Prim::Lines;
                cmd.vertices = {v(0), v(1)};
                break;
            case 3:
                cmd.prim = DrawCommand::Prim::Triangles;
                cmd.vertices = {v(0), v(1), v(2)};
                break;
            case 4:
                cmd.prim = DrawCommand::Prim::Quads;
                cmd.vertices = {v(0), v(1), v(2), v(3)};
                break;
            default:
                cmd.prim = DrawCommand::Prim::Points;
                cmd.vertices = {v(0)};
                break;
            }
        } else if (o.slot == -2) {
            cmd.prim = DrawCommand::Prim::Clear;
        } else {
            return;  // discs and circles (-3, -4) have no creator in the game
        }
        out.push_back(std::move(cmd));
        return;
    }

    if (!validSlot(o.slot)) return;
    const ImageSlot& tex = slots[static_cast<size_t>(o.slot)];
    if (!tex.loaded) return;
    bool texReady = tex.rtState == 1 || (tex.rtState == 0 && tex.hasTexture);
    if (!texReady) return;
    cmd.texture = o.slot;
    cmd.linear = o.filter == 1 || o.filter == 2;
    cmd.alphaFromDiffuseSquared = tex.rtState == 1;
    float W = static_cast<float>(tex.texW), H = static_cast<float>(tex.texH);
    if (W <= 0 || H <= 0) return;

    if (o.maskSlot >= 0) {
        // Two textures (0x43c57a): one quad, both stages addressed in the picture's texture space.
        if (!validSlot(o.maskSlot)) return;
        const ImageSlot& m = slots[static_cast<size_t>(o.maskSlot)];
        if (!m.loaded || !(m.rtState == 1 || (m.rtState == 0 && m.hasTexture))) return;
        cmd.mask = o.maskSlot;
        if (o.shaderOn) {
            cmd.shader = static_cast<int>(o.shader);
            cmd.c0w = o.c0[3];
        }
        cmd.prim = DrawCommand::Prim::Quads;
        float us[4] = {o.u0, o.u1, o.u0, o.u1}, vs[4] = {o.v0, o.v0, o.v1, o.v1};
        for (size_t i = 0; i < 4; ++i) cmd.vertices.push_back(vertex(X[i] + 0.5f, Y[i] + 0.5f, us[i] / W, vs[i] / H, col[i]));
        out.push_back(std::move(cmd));
        return;
    }

    // Tiles (0x43c514): the source rectangle cut along the slot's tiles; the covered part maps onto
    // the quad from its start, so texels outside the texture are not drawn.
    float U0 = o.u0, U1 = o.u1, V0 = o.v0, V1 = o.v1;
    if (U1 == U0 || V1 == V0) return;
    int tw = tex.tileW, tht = tex.tileH;
    int nx = std::min(tex.tilesX, 16), ny = std::min(tex.tilesY, 16);
    if (tw <= 0 || tht <= 0) return;
    auto ftw = static_cast<float>(tw), fth = static_cast<float>(tht);
    std::array<float, 17> uL{}, uR{}, vT{}, vB{};
    for (int i = 0; i < nx; ++i) {
        uL[static_cast<size_t>(i)] = std::clamp((U0 - static_cast<float>(i) * ftw) / ftw, 0.0f, 1.0f);
        uR[static_cast<size_t>(i)] = std::clamp((U1 - static_cast<float>(i) * ftw) / ftw, 0.0f, 1.0f);
    }
    for (int j = 0; j < ny; ++j) {
        vT[static_cast<size_t>(j)] = std::clamp((V0 - static_cast<float>(j) * fth) / fth, 0.0f, 1.0f);
        vB[static_cast<size_t>(j)] = std::clamp((V1 - static_cast<float>(j) * fth) / fth, 0.0f, 1.0f);
    }
    // Grid points and their colours.
    std::array<float, 17> sCol{}, tRow{};
    float acc = 0;
    for (int i = 0; i <= nx; ++i) {
        sCol[static_cast<size_t>(i)] = ftw * acc / (U1 - U0);
        if (i < nx) acc += uR[static_cast<size_t>(i)] - uL[static_cast<size_t>(i)];
    }
    acc = 0;
    for (int j = 0; j <= ny; ++j) {
        tRow[static_cast<size_t>(j)] = fth * acc / (V1 - V0);
        if (j < ny) acc += vB[static_cast<size_t>(j)] - vT[static_cast<size_t>(j)];
    }
    struct Point {
        float x, y;
        CornerColor c;
    };
    auto grid = [&](int j, int i) {
        float t = tRow[static_cast<size_t>(j)], sc = sCol[static_cast<size_t>(i)];
        float lx = X[0] + (X[2] - X[0]) * t, ly = Y[0] + (Y[2] - Y[0]) * t;
        float rx = X[1] + (X[3] - X[1]) * t, ry = Y[1] + (Y[3] - Y[1]) * t;
        CornerColor lc = lerp(col[0], col[2], t), rc = lerp(col[1], col[3], t);
        return Point{lx + (rx - lx) * sc, ly + (ry - ly) * sc, lerp(lc, rc, sc)};
    };
    cmd.prim = DrawCommand::Prim::Quads;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            auto ii = static_cast<size_t>(i), jj = static_cast<size_t>(j);
            if (uR[ii] == uL[ii] || vB[jj] == vT[jj]) continue;
            float fi = static_cast<float>(i), fj = static_cast<float>(j);
            if (!(U0 < (fi + 1) * ftw && V0 < (fj + 1) * fth && fi * ftw < U1 && fj * fth < V1)) continue;
            Point p[4] = {grid(j, i), grid(j, i + 1), grid(j + 1, i), grid(j + 1, i + 1)};
            float ul = (fi + uL[ii]) * ftw / W, ur = (fi + uR[ii]) * ftw / W;
            float vt = (fj + vT[jj]) * fth / H, vb = (fj + vB[jj]) * fth / H;
            float us[4] = {ul, ur, ul, ur}, vs[4] = {vt, vt, vb, vb};
            // The engine's vertices sit half a pixel up and left of the corners (0x4c2280), which in
            // OpenGL's pixel convention is the corner itself.
            for (int k = 0; k < 4; ++k) cmd.vertices.push_back(vertex(p[k].x, p[k].y, us[k], vs[k], p[k].c));
        }
    }
    if (!cmd.vertices.empty()) out.push_back(std::move(cmd));
}

}  // namespace nyan::wa2
