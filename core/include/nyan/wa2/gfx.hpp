#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nyan/wa2/assets.hpp"
#include "nyan/wa2/image.hpp"
#include "nyan/wa2/text.hpp"

namespace nyan::wa2 {

// The engine's 2D layer (WA2.exe 0x439640.., 0x43b590, 0x441f70): 1024 image slots holding textures,
// 2048 sprite objects that draw them, and eight global tint groups. The game's systems (backgrounds,
// characters, text, menus) create objects and call setters every logic frame; nothing here animates.
// buildFrame() does the frame draw's work up to the device: the sort, the composite kinds, the quad
// geometry, colours and tiling; a renderer executes the commands it returns.

constexpr int kSlotCount = 1024;
constexpr int kObjectCount = 2048;
constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;

// An image slot (0xa93fb0 + slot * 0x80).
struct ImageSlot {
    bool loaded = false;   // +0x00
    uint8_t type = 0;      // +0x01: picture type (load flags & 0xff); 1 for render targets
    bool single = false;   // +0x03: one power-of-two texture (masks, render targets) instead of tiles
    bool half = false;     // +0x04: half resolution (Nyanroid keeps the full picture)
    std::string file;      // +0x05
    std::string amp1, amp2;  // +0x25 / +0x35
    std::string group;     // +0x45
    int level = 0x100;     // +0x58
    int texW = 0, texH = 0;  // +0x5c / +0x5e: the texture area W', H' (tile-rounded or a power of two)
    int tileW = 0, tileH = 0;  // +0x60 / +0x62
    int tilesX = 0, tilesY = 0;  // +0x64 / +0x66
    int rtState = 0;       // +0x68: 0 picture, 1 render target made, 2 render target to make on first use
    int picW = 0, picH = 0;  // +0x7c / +0x7e
    bool hasTexture = false;  // a tiled mask (type 3) is only a CPU array: no texture
    RgbaImage pixels;      // texW x texH, the picture at the top left (empty for render targets)
    uint32_t version = 0;  // changes with the contents (the renderer uploads again)
};

// A sprite object (0xab3fb0 + obj * 0x100). Corners: 0 top left, 1 top right, 2 bottom left, 3 bottom
// right. Colours are ARGB.
struct SpriteObject {
    bool used = false;      // +0x00
    bool visible = false;   // +0x01
    int16_t priority = 0;   // +0x02
    uint32_t mask = 1;      // +0x04
    int32_t value = 0;      // +0x08 kind 8
    int16_t digits = 0;     // +0x0c kind 8
    int16_t digitSpacing = 0;  // +0x0e kind 8
    std::string text;       // +0x10 kind 7
    uint8_t kind = 0;       // +0x14
    int8_t vertexCount = 4; // +0x15
    bool shaderOn = false;  // +0x16
    uint32_t shader = 0;    // +0x18
    std::array<float, 4> c0{};  // +0x1c
    uint32_t blend = 1;     // +0x3c
    uint32_t filter = 0;    // +0x40
    uint32_t mirror = 0;    // +0x44
    int16_t target = -1;    // +0x48 render-target slot
    int16_t slot = -1;      // +0x4a texture slot; kind 1: the .ani instance
    int16_t maskSlot = -1;  // +0x4c
    int16_t maskSoft = 0;   // +0x4e
    int16_t textRecord = 0; // +0x50 kind 0xb
    uint8_t tintGroup = 0;  // +0x52
    std::array<uint32_t, 4> specular{0xff000000, 0xff000000, 0xff000000, 0xff000000};  // +0x53
    std::array<uint32_t, 4> diffuse{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};    // +0x63
    float rotation = 0;     // +0x74 degrees
    float pivotX = 0, pivotY = 0;  // +0x78
    float scaleX = 1, scaleY = 1;  // +0x80
    std::array<float, 4> x{}, y{};  // +0x88 / +0x98
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;  // +0xbc..+0xc8 source rectangle in texels
};

// One draw of the frame, in the order the device gets them.
struct DrawVertex {
    float x = 0, y = 0;  // target pixels, OpenGL convention (pixel centres at +0.5)
    float u = 0, v = 0;  // texture coordinates 0..1 of the slot's texture area
    std::array<uint8_t, 4> diffuse{};   // R, G, B, A
    std::array<uint8_t, 3> specular{};  // R, G, B
};

struct DrawCommand {
    enum class Prim : uint8_t {
        Quads,        // 4 vertices per quad: top left, top right, bottom left, bottom right
        Lines,        // pairs
        Triangles,
        Points,
        TriangleFan,
        LineStrip,
        Clear,        // the target to 0
        MakeTarget,   // a render target's first use: create and clear it
    };
    Prim prim = Prim::Quads;
    int target = -1;    // render-target slot, -1 the screen
    int texture = -1;   // slot, -1 untextured (output = the diffuse colour)
    int mask = -1;      // second texture (pattern fades)
    int shader = -1;    // pf00..pf03 with the mask: alpha = f(mask.a + c0w); -1: none
    float c0w = 0;
    uint32_t blend = 1; // the object's mode (WA2.exe switch 0x43dd70)
    bool linear = false;  // sampler filter
    bool alphaFromDiffuseSquared = false;  // the texture is a render target: alpha = diffuse.a^2
    std::vector<DrawVertex> vertices;
};

class Gfx {
public:
    explicit Gfx(const Assets& assets) : assets_(assets) {}
    const Assets& assets() const { return assets_; }

    std::array<ImageSlot, kSlotCount> slots;
    std::array<TextSlot, 128> texts;
    // Without a renderer (nyan-cli, tests) pictures are only measured: slots get their sizes and no
    // texels.
    bool keepPixels = true;
    std::array<SpriteObject, kObjectCount> objects;
    // Global tint groups (0x4c2290): R, G, B, mode; mode 2 leaves colours alone.
    struct Tint {
        int r = 0x80, g = 0x80, b = 0x80, mode = 2;
    };
    std::array<Tint, 8> tints;

    // ---- Slots ----
    // Loads a picture (0x4493b0 -> 0x43ffa0): `flags` low byte = type, 0x100 mirror, 0x200 flip,
    // 0x400 invert (single path). Colour level, then the two .amp tables from `grp`. False (and the
    // slot left empty) if the file is missing or unreadable, as the engine logs and returns.
    bool load(int slot, std::string_view file, uint32_t flags, bool half, std::string_view amp1,
              std::string_view amp2, int level, std::string_view group, int maxTile = 0x100, bool single = false);
    // A render-target slot (0x43e2e0): w, h powers of two; its texture is made on first use.
    void makeTarget(int slot, int w, int h);
    void release(int slot);  // 0x43e180
    // 0x441d80: every slot (but the glyph atlases 0x1c4..0x1c7, which Nyanroid keeps loaded), object
    // and text record released.
    void reset();
    // 0x43fe00: a copy of a slot (its texture and record).
    void copySlot(int dst, int src);
    // 0x449440: the picture's size.
    bool pictureSize(int slot, int& w, int& h) const;

    // ---- Objects (constructors clear the object first, 0x4395d0) ----
    void remove(int obj);                                           // 0x4395d0
    void makeImage(int obj, int slot, int prio, bool visible);      // 0x439640
    void makeNineSlice(int obj, int slot, int prio, bool visible);  // 0x4397e0
    void makePolygon(int obj, int n, int prio, bool visible);       // 0x439810: untextured, full screen
    void makeString(int obj, int slot, int prio, bool visible, std::string_view s);  // 0x439ff0
    void makeNumber(int obj, int slot, int prio, bool visible, int value, int digits, int spacing);  // 0x43a280
    // 0x439e40: a text record's object (kind 0xb) on the record's atlas.
    void makeText(int obj, int record, int prio, bool visible);

    // ---- Text records (0x4383c0 via 0x449260, 0x438200, 0x438310) ----
    void makeTextSlot(int index, int atlas, const std::string& text, int size, int cols, int rows, int spacing, int gap,
                      int em, int cell, bool outline, int r, int g, int b, int or_, int og, int ob, bool ruby);
    // A new string: page and reveal back to -1 (all shown); colours given as -1 are kept.
    void setTextString(int index, const std::string& text, int r = -1, int g = -1, int b = -1, int or_ = -1, int og = -1,
                       int ob = -1);
    void setTextColor(int index, int r, int g, int b, int or_ = -1, int og = -1, int ob = -1);
    void releaseTextSlot(int index);
    // Through a text object (0x43a930 / 0x43a980 / 0x43a9d0): set the page or the reveal; returns
    // the end flag / the complete flag of the last draw.
    bool setTextPage(int obj, int page);
    bool setTextReveal(int obj, int reveal);
    void textAnchor(int obj, int& x, float& y) const;

    // ---- Setters (WA2.exe 0x43a470..0x43b510) ----
    void setRect(int obj, float x, float y, float u, float v, float w, float h);  // 0x43a500
    void setDest(int obj, float x, float y, float w, float h);                    // 0x43a5c0
    void setSource(int obj, float u, float v, float w, float h);                  // 0x43a640
    void moveTo(int obj, float x, float y);                                       // 0x43aa30
    void moveSource(int obj, float u, float v);                                   // 0x43ab30
    void setPivot(int obj, float x, float y);                                     // 0x43ac10
    void setRotation(int obj, float degrees);                                     // 0x43ac30
    void setScale(int obj, float s);                                              // 0x43ac50
    void setScale(int obj, float sx, float sy);                                   // 0x43ac70
    void setVisible(int obj, bool on);                                            // 0x43ac90
    void setPriority(int obj, int prio);                                          // 0x43a490
    void setMask(int obj, uint32_t mask);                                         // 0x43a4b0
    void setTarget(int obj, int rtSlot);                                          // 0x43a470
    void setSlot(int obj, int slot);                                              // 0x43acb0
    void setMaskSlot(int obj, int slot, int soft);                                // 0x43acd0
    void clearMaskSlot(int obj);                                                  // 0x43acf0
    void setShader(int obj, int shader, float c0w);                               // 0x43ae00
    void setBlend(int obj, uint32_t mode);                                        // 0x43aed0
    void setFilter(int obj, uint32_t filter);                                     // 0x43aef0
    void setMirror(int obj, uint32_t flags);                                      // 0x43af10
    void setColor(int obj, int a, int r, int g, int b);                           // 0x43af30
    void setCornerColor(int obj, int corner, int a, int r, int g, int b);         // 0x43b080
    void setBrightness(int obj, int v);                                           // 0x43b1b0
    void setTone(int obj, int r, int g, int b);                                   // 0x43b220
    void setAlpha(int obj, int a);                                                // 0x43b2a0
    void setTintGroup(int obj, int group);                                        // 0x43b4f0
    void setTint(int r, int g, int b, int mode);                                  // 0x43b510 (group 0)
    void setString(int obj, std::string_view s);                                  // 0x43a1f0
    void setValue(int obj, int value);                                            // 0x43a450

    // The .ani instances (kind 1) are expanded by the animation player: it emits their parts as
    // objects through `draw`.
    using DrawObject = std::function<void(const SpriteObject&)>;
    std::function<void(const SpriteObject& obj, const DrawObject& draw)> drawAni;
    // Drawn over everything at the end of a frame (the English patch's subtitle overlay).
    std::function<void(const DrawObject& draw)> overlay;
    // The renderer's read of a render target's texels (0x4391f0), top row first; unset without one.
    std::function<bool(int slot, RgbaImage& out)> readTarget;
    // A slot from texels (a picture that is not TGA / BMP, e.g. the patch's PNG), one texture.
    void loadPixels(int slot, RgbaImage image);

    // The frame draw (0x441f70): `clear` the screen first (the engine's request flag 0x4c221c).
    // Render targets met for the first time are made here (their state goes 2 -> 1), as the device
    // does it on the first draw into them.
    std::vector<DrawCommand> buildFrame(bool clear);
    // The text objects' part of a frame draw without drawing: their records' end flags and anchors,
    // which the message code reads on the next frame. For runs without a renderer.
    void layoutTexts();
    // A frame was built since the last call (and so the text records are current).
    bool takeDrawn() {
        bool d = drawn_;
        drawn_ = false;
        return d;
    }

private:
    void quad(const SpriteObject& o, std::vector<DrawCommand>& out);  // 0x43b590
    void drawKind(const SpriteObject& o, std::vector<DrawCommand>& out);

    const Assets& assets_;
    uint32_t versions_ = 0;
    int currentTarget_ = -1;  // the device's render target while a frame is built
    bool drawn_ = false;
    std::vector<int16_t> sorted();  // 0x442073
};

}  // namespace nyan::wa2
