// SQText: the scripts' text object (Diesel::Squirrel::SQText; natives muramasa_en.exe 0x521300 ff.),
// a rendering object holding a Diesel::CText (0x85c bytes; CTextEnglish for initialize(1),
// CTextVertical for 2; constructor 0x4cf8a0).
//
// The text is laid out by its kind's layout (text_layout.cpp: CText's Japanese layout with ruby,
// CTextEnglish's), typed character by character as the exe's vertex build reads the typing, and
// drawn as glyph quads on the text's matrix with the shadow setShadow asks for.

#include <algorithm>

#include <cmath>

#include "natives.hpp"
#include "nyan/diesel/sprite_geometry.hpp"
#include "nyan/diesel/text_layout.hpp"
#include "nyan/diesel/text_tokenizer.hpp"

namespace nyan::diesel {

namespace {

// A registered font of the app's pool as the layout reads it (CSystemFont: vtable +0x1c / +0x20).
struct PoolFont final : TextFont {
    std::shared_ptr<FontInstance> font;
    explicit PoolFont(std::shared_ptr<FontInstance> f) : font(std::move(f)) {}
    TextGlyph glyph(uint32_t code, bool italic) override {
        auto g = font->glyph(code, italic);
        return {g->cellWidth, g->cellHeight, g->overhang, g->adjust, g};
    }
    int height() const override { return font->height(); }
    int maxWidth() const override { return font->cellWidth(); }
};

struct TextImage {
    int32_t type = 0;                    // initialize(): 0 CText, 1 English, 2 vertical
    std::string font, rubyFont;          // +0xbc / +0xd4
    std::string text;                    // +0xec: what create / print gave
    TextLayoutSettings settings;         // +0x124 .. +0x140 (the ruby offset: SMALL characters up by it)
    // addPositionFixRule's pairs (+0x850; kept through clear and relayout, read by the next layout).
    std::vector<PositionFixRule> fixRules;
    // setShadow(type, gap, power, alpha) (0x4d6650): +0x618 type (FONT_SHADOW_TYPE_*: 1 left,
    // 2 right, 4 up, 8 down, their pairs, 15 around), the gap, +0x76c power, +0x78c alpha (the
    // framework's "alpha exponent": 10 for "around" unless the font gives 1; the shadow sprite's
    // alpha byte is its low byte x 255).
    int32_t shadowType = 0, shadowGap = 0;
    float shadowPower = 0.0f, shadowAlpha = 0.0f;
    // setShadowShadingParameter(level, density, power) (SQText 0x5219b0): +0x794 level, +0x798
    // density, +0x790 power, and +0x780 set (the shadow drawn again). The shadow's blur is not drawn
    // yet; the defaults are a guess.
    int32_t shadowShadingLevel = 0;
    float shadowShadingDensity = 0.0f, shadowShadingPower = 0.0f;
    float typingSpeed = 0.2951f;         // +0x148 (0x3e9719a3)
    float maxRate = 5.3125f;             // +0x14c: alpha per 16 ms at the slowest
    float minRate = 212.5f;              // +0x150: at the fastest
    int32_t typingThreshold = -1;        // +0x57c: setTypingThreshold(f) keeps (int)(f * 255)
    int32_t beginType = 0;               // +0x108
    bool typingComplete = false;         // +0x104: pushAll shows every character
    Tween typing{1, 0.0f};               // +0x3f4: alpha units typed
    TextLayout layout;                   // the characters, the ruby and the pen
    float centerX = 0.0f, centerY = 0.0f;  // +0x2a0
    float pivotX = 0.0f, pivotY = 0.0f;    // +0x2a8
    int32_t blendMode = 1;
    int32_t textureFiltering = 1;
    float timeScale = 1.0f;                // +0x154
    Tween fade{1, 1.0f};                   // +0x2b4
    Tween move{2, 0.0f, 0.0f};             // +0x2e0
    Tween scale{2, 1.0f, 1.0f};            // +0x31c
    Tween rotate{3, 0.0f, 0.0f, 0.0f};     // +0x358
    // shake / randomShake (CText +0x84c, 0x4d75c0 / 0x4d7670: CEffectShake / CEffectRandomShake as a
    // sprite's): the update steps it while it shakes, the matrix adds its offset to the position.
    std::unique_ptr<Shake> shake;
    // setBezier / beginBezier's path (CBezier<Vector3D> at +0x420, as a sprite's): while it runs,
    // each frame its point's x and y become the move tween's value, before the spline's (0x4d3960).
    SpriteImage::Bezier bezier;
    // setSpline / beginSpline's path (+0x470, section 5m): while it runs, each frame its point's x
    // and y become the move tween's value (0x4d3960).
    TimedSpline spline;
    // transition(...)'s threshold tween (+0x3a4), rule file (+0x3d0) and softness (+0x3f0).
    Tween transition{1, 0.0f};
    std::string transitionFile;
    float transitionRate = 0.0f;
    // setBlur / setBlurMode (SQText 0x522890 / 0x5228c0 -> CText 0x4d9020): the two glyph batches'
    // CBlurs (+0x500 / +0x53c) take the same settings; each keeps its own frames.
    SpriteImage::Blur blur;
    TextBlurFrames blurFrames;
    // setDither (SQText 0x5225c0 -> CText 0x4d8200): kept (+0x4fc on, +0x4f4 / +0x4f8 the offset,
    // +0x4dc the file; `ditherFile` here the pattern if it loaded, "" the default one) and handed to
    // both glyph batches and the shadow's sprite (+0x788). setSurface makes that sprite again
    // (vtable +0x4c, 0x4d6190) without it: `shadowDither` says the sprite has the last setting.
    bool dither = false;
    std::string ditherFile;
    float ditherX = 0.0f, ditherY = 0.0f;
    bool shadowDither = false;
    // setDisplacementMap (SQText 0x5224b0 -> CText 0x4d8170): the map (+0x7bc), handed to both glyph
    // batches (vtable +0x70), not to the shadow's sprite; the shadow pass draws through the batches.
    std::shared_ptr<const DisplacementMap> displacement;
    // The render-tree node (CRenderObject +0xa0 / +0xa4): link()'s parent (another object's image)
    // and what objects linked to the text hang from. It carries the text's placement, the field as
    // its part size (CText vtable +0x1c, 0x4d3f70).
    SpriteImage node;
    std::array<Tween, 4> fontColor{Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f},
                                   Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f}};  // +0x158
    std::array<Tween, 4> shadowColor{Tween{3, 0.0f, 0.0f, 0.0f}, Tween{3, 0.0f, 0.0f, 0.0f},
                                     Tween{3, 0.0f, 0.0f, 0.0f}, Tween{3, 0.0f, 0.0f, 0.0f}};

    template <typename F>
    void forEachTween(F&& f) {
        f(typing), f(fade), f(move), f(scale), f(rotate), f(transition), f(bezier.t), f(spline.t);
        for (auto& t : fontColor) f(t);
        for (auto& t : shadowColor) f(t);
    }
    // The typing rate (alpha per 16 ms) for the speed (0x4d6c00 / 0x4ca640).
    float rate() const {
        if (!(typingSpeed > 0.0f)) return 0.0f;
        float s = 1.0f - typingSpeed;
        s = s * s;
        return (minRate - maxRate) * s * s + maxRate;
    }
    // A font of the app's pool by name (only names it registered: a layout without one fails).
    static std::shared_ptr<FontInstance> poolFont(ScriptHost::Impl& h, const std::string& name) {
        if (h.fonts.find(name) == h.fonts.end()) return nullptr;
        return h.fontInstance(name);
    }
    void clear() {  // 0x4d70a0
        text.clear();
        layout.clear();
        beginType = 0;
        typingComplete = false;
        typing = Tween{1, 0.0f};
        typing.timeScale = timeScale;
    }
    // vtable +0x80 (0x4ca640): `s` laid out after the text (s empty: the whole text again). Both
    // fonts must be in the pool, else the text is cleared. After new text, typing starts over:
    // its target is the largest 255 - shown over the characters (a new one shows -48 for each unit
    // read before it), each character's start becomes what it shows, and the time is 16 / rate per
    // alpha unit (0x4cb57a ff.).
    int32_t lay(ScriptHost::Impl& h, const std::string& s) {
        auto main = poolFont(h, font), ruby = poolFont(h, rubyFont);
        // (The older English layout reads only the main font: 0x49e6c0.)
        if (!ruby && layout.older && layout.type == 1) ruby = main;
        if (!main || !ruby) {
            clear();
            return -1;
        }
        PoolFont m(main), r(ruby);
        if (!layout.layout(s, text, m, r, settings, h.lineBreaks)) return -1;
        layout.cornerOffsets(settings, fixRules);  // 0x4d2cf0's vt+0x88
        if (s.empty()) return 0;
        beginType = 1;
        typingComplete = false;
        float target = 0.0f;
        for (uint32_t e : layout.pool) {  // every character made, ruby too (0x4cc550 over +0x588)
            TextChar& c = layout.at(e);
            float need = 255.0f - c.shown;
            if (need > target) target = need;
            c.typingStart = c.shown;
        }
        float rt = rate();
        float ms = rt == 0.0f ? 0.0f : 16.0f / rt * target;
        typing.set({0.0f, 0.0f, 0.0f}, {target, 0.0f, 0.0f}, static_cast<int32_t>(ms), 0);
        typing.timeScale = timeScale;
        return 0;
    }
    // A setting that moves characters (setField, setPitchSpace, setRowSpace, setRubyOffset):
    // laid out again if there is text.
    void relayout(ScriptHost::Impl& h) {
        if (!text.empty()) lay(h, std::string());
    }
    // create (0x4d3780): the text replaces what was there; -1 if empty or not laid out.
    int32_t create(ScriptHost::Impl& h, const std::string& s) {
        clear();
        text = s;
        if (text.empty()) return -1;
        return lay(h, text) < 0 ? -1 : 0;
    }
    // print (0x4d37e0): appended after the pitch for its first printable character (an empty
    // text: create); -1 for nothing.
    int32_t print(ScriptHost::Impl& h, const std::string& s) {
        if (s.empty()) return -1;
        if (text.empty()) return create(h, s);
        layout.pitchBeforePrint(s, settings);
        if (lay(h, s) < 0) return -1;
        text += s;
        return 0;
    }
    // The typing value the characters read (0x4d45d0): with a threshold, scaled to start there.
    float typedUnits() const {
        float now = typing.now[0];
        if (typingThreshold >= 0) {
            float dst = typing.dst[0];
            float thr = static_cast<float>(typingThreshold);
            now = ((dst - thr) * now) / dst + thr;
        }
        return now;
    }
    // Where the text is placed (CText vtable +0x18, 0x4d3fb0: the sprites' matrix with the field
    // as the picture and z = 0).
    Placement placement() const {
        Placement p;
        p.width = static_cast<float>(settings.fieldWidth);
        p.height = static_cast<float>(settings.fieldHeight);
        p.centerX = centerX, p.centerY = centerY, p.pivotX = pivotX, p.pivotY = pivotY;
        p.scaleX = scale.now[0], p.scaleY = scale.now[1];
        p.rotate = rotate.now;
        p.position = {move.now[0], move.now[1], 0.0f};
        // A shake moves the position (0x4d4223: its offset added before the translation).
        if (shake) {
            auto o = shake->offset();
            for (size_t i = 0; i < 3; ++i) p.position[i] += o[i];
        }
        return p;
    }
    // The node takes the placement, for what hangs from the text.
    void syncNode() {
        Placement p = placement();
        node.srcW = settings.fieldWidth, node.srcH = settings.fieldHeight;
        node.centerX = centerX, node.centerY = centerY, node.pivotX = pivotX, node.pivotY = pivotY;
        node.scale.now[0] = p.scaleX, node.scale.now[1] = p.scaleY;
        node.rotate.now = p.rotate;
        node.move.now = p.position;
        node.actionCompatible = 0;
    }
    // The world matrix: placed in the space of what it hangs from (its part size, else the target:
    // 0x4a2a50), then that object's world.
    Mat4 world(int sw, int sh) const {
        float spaceW = static_cast<float>(sw), spaceH = static_cast<float>(sh);
        if (node.parent) spaceW = static_cast<float>(node.parent->srcW), spaceH = static_cast<float>(node.parent->srcH);
        Mat4 m = placementMatrix(placement(), spaceW, spaceH);
        if (node.parent) m *= spriteWorldMatrix(*node.parent, sw, sh);
        return m;
    }
};

// A colour channel as CText packs it (0x4d3960's updates, 0x4d94a0): (char)(int)(clamp(v) x 255).
float packed(float v) {
    return static_cast<float>(static_cast<uint8_t>(static_cast<int32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f))) / 255.0f;
}

// The shadow's offsets for a font (0x4d6650), screen pixels with y down: one for a direction, or
// for "around" 8 * gap of them on a circle of radius gap (clamped to 1 .. 0.15 * the cell height),
// 45 / gap degrees apart.
std::vector<std::array<float, 2>> shadowOffsets(int type, int gap, int cellHeight) {
    std::vector<std::array<float, 2>> out;
    float g = static_cast<float>(gap);
    if (type == 15) {
        float most = static_cast<float>(cellHeight) * 0.15f;
        if (g < 1.0f) g = 1.0f;
        else if (most < g) g = most;
        float vx = -g, vy = 0.0f;
        float step = (1.0f / g) * 45.0f * 3.14159265358979f / 180.0f;
        int count = static_cast<int>(g) << 3;
        for (int i = 0; i < count; ++i) {
            out.push_back({vx, vy});
            float c = std::cos(step), s = std::sin(step);
            float nx = vx * c - vy * s, ny = vx * s + vy * c;
            vx = nx, vy = ny;
        }
        return out;
    }
    if (type & 4) {
        if (type & 1) out.push_back({-g, -g});
        else if (type & 2) out.push_back({g, -g});
        else out.push_back({0.0f, -g});
    } else if (type & 8) {
        if (type & 1) out.push_back({-g, g});
        else if (type & 2) out.push_back({g, g});
        else out.push_back({0.0f, g});
    } else if (type & 1) {
        out.push_back({-g, 0.0f});
    } else if (type & 2) {
        out.push_back({g, 0.0f});
    }
    return out;
}

// The move tween put at a point's x and y without running (+0x304, the value without power; +0x2fc
// that times the power: 0x4d8f60, 0x4d9be0).
void placeMove(TextImage& im, const std::array<float, 3>& p) {
    for (size_t k = 0; k < 2; ++k) {
        im.move.nowNoPower[k] = p[k];
        im.move.now[k] = p[k] * im.move.power;
    }
}

struct TextObject final : NativeObject, FrameTask, Drawable {
    ScriptHost::Impl* host;
    std::unique_ptr<TextImage> img;
    float timeScale = 1.0f;
    int32_t lock = 0, disused = 0, hidable = 0;

    TextObject(ScriptHost::Impl* h, std::string name) : host(h) {
        taskName = std::move(name);
        created = h->clock.milliTime;
        h->addTask(this);
        h->addDrawable(this, 0, created);
    }
    ~TextObject() override {
        host->removeTask(this);
        host->removeDrawable(this);
    }
    // The task update (0x4d3960): the tweens; typing only while begun; the characters' and the ruby
    // characters' own effects: a character's shake steps while it shakes and goes once done.
    void update(ScriptHost::Impl& h) override {
        if (!img) return;
        if (img->beginType) img->typing.step(h.clock);
        img->fade.step(h.clock);
        img->move.step(h.clock);
        if (img->bezier.step(h.clock)) placeMove(*img, img->bezier.point);
        if (img->spline.step(h.clock)) placeMove(*img, img->spline.point);
        img->scale.step(h.clock);
        img->rotate.step(h.clock);
        img->transition.step(h.clock);
        for (auto& t : img->fontColor) t.step(h.clock);
        for (auto& t : img->shadowColor) t.step(h.clock);
        auto own = [&h](TextChar& c) {
            if (c.moveSet) c.move.step(h.clock);
            if (c.fadeSet) c.fade.step(h.clock);
            if (c.scaleSet) c.scale.step(h.clock);
            if (c.rotateSet) c.rotate.step(h.clock);
            if (c.shake && !c.shake->shaking()) c.shake.reset();
            if (c.shake && c.shake->shaking()) c.shake->update(h.clock);
            if (c.colorSet) {
                for (auto& t : c.color) t.step(h.clock);
            }
        };
        for (auto& c : img->layout.chars) own(c);
        for (auto& c : img->layout.rubies) own(c);
        // The shake steps while it shakes (0x4d3b2b).
        if (img->shake && img->shake->shaking()) img->shake->update(h.clock);
        img->syncNode();
    }
    Drawable* drawable() override { return this; }
    SpriteImage* linkNode() override {
        if (!img) return nullptr;
        img->syncNode();
        return &img->node;
    }

    // The characters' quads (the vertex build 0x4d45d0, over every character made, ruby ones too):
    // each character's alpha units are its typing start plus the typing (255 once pushAll), its
    // alpha their 1/255 (or, with a threshold, 0 below it and 1 from it), times the text's fade; its
    // cell sits in the field centred on the text's matrix, the corners moved by vtable +0x88's
    // offsets (0x4d2cf0), the glyph in it; the corners take the four font colours. A character with
    // effects of its own (or a shake) scales and turns about its cell's centre and moves by its move
    // plus its shake. A shadow (setShadow) draws first: the glyphs at its offsets, their alpha added
    // up in a render target (Mware's render-target blend), in the shadow colour at the shadow's alpha
    // byte.
    void emit(ScriptHost::Impl& h, std::vector<DrawItem>& out) override {
        if (!img) return;
        TextImage& t = *img;
        float fade = std::clamp(t.fade.now[0] * t.fade.power, 0.0f, 1.0f);
        if (static_cast<int>(fade * 255.0f) <= 0) {
            for (uint32_t e : t.layout.pool) {
                TextChar& c = t.layout.at(e);
                c.shown = t.typingComplete ? 255.0f : std::min(255.0f, c.typingStart + t.typedUnits());
            }
            return;
        }
        int sw = h.layoutWidth, sh = h.layoutHeight;  // the target it draws into
        t.syncNode();
        Mat4 world = t.world(sw, sh);
        float halfW = static_cast<float>(t.settings.fieldWidth) * 0.5f;
        float halfH = static_cast<float>(t.settings.fieldHeight) * 0.5f;
        float typed = t.typedUnits();
        std::array<std::vector<TextGlyphDraw>, 2> current;  // the main batch (+0x59c), the ruby one (+0x5a0)
        for (uint32_t e : t.layout.pool) {
            TextChar& c = t.layout.at(e);
            float units = t.typingComplete ? 255.0f : std::min(255.0f, c.typingStart + typed);
            c.shown = units;
            float a = t.typingThreshold < 0 ? units * 0.003921568859368563f
                                            : (units < static_cast<float>(t.typingThreshold) ? 0.0f : 1.0f);
            a = std::clamp(a, 0.0f, 1.0f);
            a = std::clamp(c.fade.now[0] * a, 0.0f, 1.0f);
            if (a <= 0.0f || !c.image || c.image->rows == 0) continue;
            float px[4], py[4];
            const auto& k = c.corner;
            if (k[0] == k[1] && k[0] == k[2] && k[0] == k[3]) {
                float x0 = c.x + static_cast<float>(c.image->left) - halfW + k[0][0];
                float x1 = x0 + static_cast<float>(c.image->pitch);
                float y0 = halfH - (c.y + static_cast<float>(c.image->top)) + k[0][1];
                float y1 = y0 - static_cast<float>(c.image->rows);
                px[0] = px[2] = x0, px[1] = px[3] = x1;
                py[0] = py[1] = y0, py[2] = py[3] = y1;
            } else {
                // The cell's corners moved apart: the glyph where it falls in the cell.
                float cw = static_cast<float>(std::max(c.cellWidth, 1)), ch = static_cast<float>(std::max(c.cellHeight, 1));
                float l = c.x - halfW, r = c.x + static_cast<float>(c.cellWidth) - halfW;
                float top = halfH - c.y, bottom = halfH - (c.y + static_cast<float>(c.cellHeight));
                const float cxs[4] = {l + k[0][0], r + k[1][0], l + k[2][0], r + k[3][0]};
                const float cys[4] = {top + k[0][1], top + k[1][1], bottom + k[2][1], bottom + k[3][1]};
                const float us[2] = {static_cast<float>(c.image->left) / cw, static_cast<float>(c.image->left + c.image->pitch) / cw};
                const float vs[2] = {static_cast<float>(c.image->top) / ch, static_cast<float>(c.image->top + c.image->rows) / ch};
                for (int i = 0; i < 4; ++i) {
                    float u = us[i & 1], v = vs[i >> 1];
                    px[i] = (1.0f - v) * ((1.0f - u) * cxs[0] + u * cxs[1]) + v * ((1.0f - u) * cxs[2] + u * cxs[3]);
                    py[i] = (1.0f - v) * ((1.0f - u) * cys[0] + u * cys[1]) + v * ((1.0f - u) * cys[2] + u * cys[3]);
                }
            }
            // The character's own matrix (0x4d45d0): about its cell's centre it scales and turns,
            // then moves by its move and its shake.
            Mat4 m = world;
            if (c.effects || c.moveSet || c.scaleSet || c.rotateSet || c.shake) {
                float cx = static_cast<float>(c.cellWidth) * 0.5f + c.x - halfW;
                float cy = static_cast<float>(c.cellHeight) * 0.5f + c.y - halfH;
                float vx = c.move.now[0], vy = c.move.now[1];
                if (c.shake) {
                    auto o = c.shake->offset();
                    vx = vx + o[0], vy = o[1] + vy;
                }
                Mat4 own = Mat4::translation(-cx, cy, 0.0f);
                own *= Mat4::scaling(c.scale.now[0], c.scale.now[1], 1.0f);
                // Degrees, negated, as Mware's RotationZXY takes them (CText 0x4d4c55).
                constexpr float kDegrees = 0.0174532924f;
                own *= Mat4::rotationZXY(-c.rotate.now[0] * kDegrees, -c.rotate.now[1] * kDegrees, -c.rotate.now[2] * kDegrees);
                own *= Mat4::translation(cx + vx, -cy - vy, 0.0f);
                m = own * world;
            }
            DrawItem d;
            d.kind = DrawItem::Kind::Glyph;
            d.glyph = c.image;
            d.srcW = c.image->pitch;
            d.srcH = c.image->rows;
            // The colours as packed bytes and the alpha byte (int)(a x 255) (0x4d4a8d); the draw
            // takes it times the fade (below).
            float alphaByte = static_cast<float>(static_cast<int32_t>(a * 255.0f));
            for (size_t i = 0; i < 4; ++i) {
                d.quad.corner[i] = toScreen(m, px[i], py[i], sw, sh);
                const auto& rgb = c.colorSet ? c.color[i].now : t.fontColor[i].now;
                for (size_t j = 0; j < 3; ++j) d.color[i][j] = packed(rgb[j]);
                d.color[i][3] = alphaByte / 255.0f;
            }
            // A transition: the batches' material takes the threshold (clamped) and the softness
            // (0x4d5fa8 -> vtable +0x34, as a sprite's CSprite2D 0x10022730); the shadow pass draws
            // through the same batches.
            if (!t.transitionFile.empty()) {
                d.transitionRule = t.transitionFile;
                d.transitionNow = std::clamp(t.transition.now[0], 0.0f, 1.0f);
                d.transitionRate = t.transitionRate;
            }
            d.blendMode = t.blendMode;
            d.filtering = t.textureFiltering != 0;
            d.dither = t.dither;
            d.ditherTexture = t.ditherFile;
            d.ditherX = t.ditherX, d.ditherY = t.ditherY;
            if (t.displacement) {
                d.displacementMap = t.displacement->target;
                d.displacementScaleX = t.displacement->scaleX;
                d.displacementScaleY = t.displacement->scaleY;
            }
            current[c.ruby ? 1 : 0].push_back({std::move(d), alphaByte, c.small || c.ruby});
        }
        // Each batch through its trail (0x4d5394 ff.): this frame's glyphs at (int)(alpha byte x
        // fade); with setBlur the frames the trail picks, older ones at (int)(alpha byte x
        // clamp(fade x weight)); the main batch first, then the ruby one.
        std::vector<DrawItem> glyphs;
        std::vector<bool> rubyFont;
        auto put = [&](const TextGlyphDraw& g, float mul) {
            DrawItem d = g.item;
            float alpha = static_cast<float>(static_cast<int32_t>(g.alphaByte * mul)) / 255.0f;
            for (auto& col : d.color) col[3] = alpha;
            glyphs.push_back(std::move(d));
            rubyFont.push_back(g.rubyFont);
        };
        for (size_t k = 0; k < 2; ++k) {
            auto& frames = t.blurFrames[k];
            if (t.blur.levels <= 0) {
                frames.clear();
                for (const auto& g : current[k]) put(g, fade);
                continue;
            }
            frames.push_back(std::move(current[k]));
            if (frames.size() > blurHistoryLimit(t.blur) && frames.size() > 1) frames.pop_front();
            size_t size = frames.size();
            auto same = [&frames](size_t i, size_t j) {
                const auto& x = frames[i];
                const auto& y = frames[j];
                if (x.size() != y.size()) return false;
                for (size_t q = 0; q < x.size(); ++q) {
                    if (x[q].item.glyph != y[q].item.glyph || x[q].alphaByte != y[q].alphaByte ||
                        x[q].item.quad.corner != y[q].item.quad.corner || x[q].item.color != y[q].item.color)
                        return false;
                }
                return true;
            };
            auto pick = blurPicks(t.blur, size, same);
            for (size_t i = 0; i < pick.size(); ++i) {
                float mul = pick[i] == size - 1 ? fade : std::clamp(fade * blurWeight(t.blur, i, pick.size()), 0.0f, 1.0f);
                for (const auto& g : frames[pick[i]]) put(g, mul);
            }
        }
        if (glyphs.empty()) return;
        if (t.shadowType != 0) {
            auto batch = std::make_shared<ShadowBatch>();
            auto main = TextImage::poolFont(h, t.font), ruby = TextImage::poolFont(h, t.rubyFont);
            auto mainOffsets = shadowOffsets(t.shadowType, t.shadowGap, main ? main->height() : 0);
            auto rubyOffsets = shadowOffsets(t.shadowType, t.shadowGap, ruby ? ruby->height() : 0);
            for (size_t i = 0; i < glyphs.size(); ++i) {
                batch->glyphs.push_back(glyphs[i]);
                batch->offsets.push_back(rubyFont[i] ? rubyOffsets : mainOffsets);
            }
            // The sprite's colour is packed (0x4d94a0): the shadow colour and, as the alpha byte,
            // (char)(int)(setShadow's fourth value x 255): 1 gives 255, Phantom's 5 gives 251.
            float alphaByte = static_cast<float>(static_cast<uint8_t>(static_cast<int32_t>(t.shadowAlpha * 255.0f)));
            batch->color = {packed(t.shadowColor[0].now[0]), packed(t.shadowColor[0].now[1]), packed(t.shadowColor[0].now[2]),
                            alphaByte / 255.0f};
            DrawItem d;
            d.kind = DrawItem::Kind::Shadow;
            d.shadow = std::move(batch);
            d.blendMode = 1;
            if (t.shadowDither && t.dither) {
                d.dither = true;
                d.ditherTexture = t.ditherFile;
                d.ditherX = t.ditherX, d.ditherY = t.ditherY;
            }
            out.push_back(std::move(d));
        }
        for (auto& d : glyphs) out.push_back(std::move(d));
    }
};

NativeObject* make(HSQUIRRELVM v) { return new TextObject(&host(v), argString(v, 2)); }

#define TEXT_SELF                                                    \
    TextObject* o = self<TextObject>(v);                             \
    if (!o) return sq_throwerror(v, "SQText: not an instance");
#define TEXT_IMAGE \
    TEXT_SELF      \
    if (!o->img) return 0;

SQInteger pushInt(HSQUIRRELVM v, SQInteger i) {
    sq_pushinteger(v, i);
    return 1;
}
SQInteger pushFloat(HSQUIRRELVM v, SQFloat f) {
    sq_pushfloat(v, f);
    return 1;
}
int32_t argI(HSQUIRRELVM v, SQInteger idx) { return static_cast<int32_t>(argInt(v, idx)); }
std::array<float, 3> vec(HSQUIRRELVM v, SQInteger idx, int n) {
    std::array<float, 3> out{};
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = argFloat(v, idx + i);
    return out;
}

// initialize(type): int (0x521300): a new CText of the type, hanging from the task's root.
SQInteger initialize(HSQUIRRELVM v) {
    TEXT_SELF
    o->img = std::make_unique<TextImage>();
    o->img->type = argI(v, 2);
    o->img->layout.type = o->img->type;
    o->img->layout.older = host(v).olderText;
    o->img->layout.clear();
    // The field starts as the screen (0x485910 passes app +0x11c to the constructor 0x4cf8a0).
    o->img->settings.fieldWidth = host(v).screenWidth();
    o->img->settings.fieldHeight = host(v).screenHeight();
    o->img->timeScale = o->timeScale;
    o->img->forEachTween([&](Tween& t) { t.timeScale = o->timeScale; });
    return pushInt(v, 0);
}
// create(text): int (0x521470): the exe answers 0 whatever its CText says.
SQInteger create(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->create(host(v), argString(v, 2));
    return pushInt(v, 0);
}
SQInteger print(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->print(host(v), argString(v, 2));
    return 0;
}
SQInteger clear(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->clear();
    return 0;
}
// getString(): the text create / print gave (CText +0xec).
SQInteger getString(HSQUIRRELVM v) {
    TEXT_SELF
    std::string s = o->img ? o->img->text : std::string();
    sq_pushstring(v, s.c_str(), static_cast<SQInteger>(s.size()));
    return 1;
}
SQInteger setField(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->settings.fieldWidth = argI(v, 2);
    o->img->settings.fieldHeight = argI(v, 3);
    o->img->settings.fieldType = argI(v, 4);
    o->img->relayout(host(v));
    return 0;
}
SQInteger getFieldSize(HSQUIRRELVM v) {
    TEXT_SELF
    if (o->img) {
        host(v).setResult(static_cast<float>(o->img->settings.fieldWidth), static_cast<float>(o->img->settings.fieldHeight));
    }
    return 0;
}
SQInteger getFieldType(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->img ? o->img->settings.fieldType : 0);
}
SQInteger setFont(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->font = argString(v, 2);
    return 0;
}
SQInteger setRubyFont(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->rubyFont = argString(v, 2);
    return 0;
}
SQInteger setPitchSpace(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->settings.pitchFull = argI(v, 2);
    o->img->settings.pitchHalf = argI(v, 3);
    o->img->relayout(host(v));
    return 0;
}
SQInteger setRowSpace(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->settings.rowSpace = argI(v, 2);
    o->img->relayout(host(v));
    return 0;
}
SQInteger setRubyOffset(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->settings.rubyOffset = argI(v, 2);
    o->img->relayout(host(v));
    return 0;
}
SQInteger setRubyMarginRate(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->settings.rubyMarginRate = argFloat(v, 2);
    return 0;
}
SQInteger setShadow(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->shadowType = argI(v, 2);
    o->img->shadowGap = argI(v, 3);
    o->img->shadowPower = argFloat(v, 4);
    o->img->shadowAlpha = argFloat(v, 5);
    return 0;
}
// Splines (SQText 0x5221b0 / 0x5221c0 / 0x522260 / 0x522f80 / 0x5232f0 -> CText 0x4d8ee0, the
// spline's 0x45fa50, 0x4d8f60, +0x490, 0x4d9be0).
SQInteger resetSpline(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->spline.reset();
    return 0;
}
SQInteger setSpline(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->spline.add({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argFloat(v, 6));
    return 0;
}
SQInteger beginSpline(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->spline.begin(argI(v, 2), argI(v, 3));
    placeMove(*o->img, o->img->spline.point);
    return 0;
}
SQInteger getSplineTimer(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->spline.t.timer : 0.0f);
}
// setSplineTimer(ms): within the spline's time only.
SQInteger setSplineTimer(HSQUIRRELVM v) {
    TEXT_IMAGE
    float ms = argFloat(v, 2);
    if (ms < 0.0f || ms > o->img->spline.t.actionTime) return 0;
    o->img->spline.setTimer(ms);
    placeMove(*o->img, o->img->spline.point);
    return 0;
}

// Beziers (SQText 0x5220b0 / 0x5220f0 / 0x522170 / 0x522f90 / 0x523310 -> CText 0x4d9ca0): as a
// sprite's, driving the move. The scripts pass z = 0 (CNutText.setBezier(x, y)).
// resetBezier: the points dropped; a running path ends where it was going.
SQInteger resetBezier(HSQUIRRELVM v) {
    TEXT_IMAGE
    auto& b = o->img->bezier;
    b.points.clear();
    if (b.t.active) {
        b.t.active = 0;
        b.t.nowNoPower[0] = b.t.dst[0];
        b.t.now[0] = b.t.power * b.t.dst[0];
    }
    return 0;
}
SQInteger setBezier(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->bezier.points.push_back({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)});
    return 0;
}
// beginBezier(time, curve) (CBezier +4, 0x45ecf0): with a whole number of four-point segments the
// path runs from its first point over the time along the curve, at the text's time scale (+0x154);
// otherwise the point is the last one given and nothing runs. The move is not placed until the
// update steps the path.
SQInteger beginBezier(HSQUIRRELVM v) {
    TEXT_IMAGE
    auto& b = o->img->bezier;
    size_t n = b.points.size();
    if (n == 0) return 0;
    if (n >= 4 && n % 4 == 0) {
        b.t.set({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, argI(v, 2), argI(v, 3));
        b.point = b.points.front();
        b.t.timeScale = o->img->timeScale;
    } else {
        b.point = b.points.back();
    }
    return 0;
}
SQInteger getBezierTimer(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->bezier.t.timer : 0.0f);
}
// setBezierTimer(ms) (0x4d9ca0): the timer set (clamped, eased), the path's point made the move's
// x and y. Unlike the sprite's there is no check for points (the point stays as it was).
SQInteger setBezierTimer(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->bezier.t.setTimer(argFloat(v, 2));
    o->img->bezier.evaluate();
    placeMove(*o->img, o->img->bezier.point);
    return 0;
}
// The font colours' timers (SQText 0x522f20 / 0x5232b0 -> CText 0x4d9dd0): index < 0 reads the
// first and sets all four; 4 and up read 0 and set nothing.
SQInteger getColorTimer(HSQUIRRELVM v) {
    TEXT_SELF
    if (!o->img) return pushFloat(v, 0.0f);
    int32_t i = argI(v, 2);
    if (i < 0) return pushFloat(v, o->img->fontColor[0].timer);
    if (i < 4) return pushFloat(v, o->img->fontColor[static_cast<size_t>(i)].timer);
    return pushFloat(v, 0.0f);
}
SQInteger setColorTimer(HSQUIRRELVM v) {
    TEXT_IMAGE
    int32_t i = argI(v, 2);
    float ms = argFloat(v, 3);
    if (i < 0) {
        for (auto& t : o->img->fontColor) t.setTimer(ms);
    } else if (i < 4) {
        o->img->fontColor[static_cast<size_t>(i)].setTimer(ms);
    }
    return 0;
}
// transition(from, to, softness, file, ms, curve) (0x521cd0 -> CText 0x4d7250 / 0x4d74e0): a rule
// file is loaded when the name changes (a missing one leaves none: the name is cleared); the
// threshold tween runs from `from` to `to` (Tween::set is 0x4d74e0's); the softness is kept.
SQInteger transition(HSQUIRRELVM v) {
    TEXT_IMAGE
    auto& h = host(v);
    TextImage& t = *o->img;
    std::string rule = argString(v, 5);
    if (rule != t.transitionFile) {
        int w = 0, ht = 0;
        t.transitionFile = !rule.empty() && h.textureSize(rule, w, ht) ? rule : std::string();
    }
    t.transition.set({argFloat(v, 2), 0.0f, 0.0f}, {argFloat(v, 3), 0.0f, 0.0f}, argI(v, 6), argI(v, 7));
    t.transitionRate = argFloat(v, 4);
    return 0;
}
// setDither(on, x, y, file) (0x5225c0 -> CText 0x4d8200): kept, and the pattern (the file if it is
// named and loads, else the graphics' default one; none when off) handed with the offset to the
// shadow's sprite and both glyph batches.
SQInteger setDither(HSQUIRRELVM v) {
    TEXT_IMAGE
    TextImage& t = *o->img;
    t.dither = argI(v, 2) != 0;
    t.ditherX = static_cast<float>(argI(v, 3));
    t.ditherY = static_cast<float>(argI(v, 4));
    std::string file = argString(v, 5);
    int w = 0, ht = 0;
    t.ditherFile = t.dither && !file.empty() && host(v).textureSize(file, w, ht) ? file : std::string();
    t.shadowDither = true;
    return 0;
}
// setDisplacementMap(name) (0x5224b0 -> CText 0x4d8170): the map held before goes; a named
// SQDisplacementMap's map (the task's +0xf8; the exe reads that field of whatever the name finds,
// so only maps are taken here) is held and handed to both glyph batches.
SQInteger setDisplacementMap(HSQUIRRELVM v) {
    TEXT_IMAGE
    TextImage& t = *o->img;
    t.displacement.reset();
    std::string name = argString(v, 2);
    FrameTask* task = name.empty() ? nullptr : host(v).findTask(name);
    if (task) t.displacement = displacementMapOf(*task);
    return 0;
}
// setBlur(on, levels, power, interval, order, fade) (0x522890 -> CText 0x4d9020): both batches'
// trails set afresh (the levels 0 when off, the interval at least 16 ms, unsigned), with the text's
// blur mode; setBlurMode(mode) (0x5228c0): +0x578 and both trails' mode.
SQInteger setBlur(HSQUIRRELVM v) {
    TEXT_IMAGE
    auto& b = o->img->blur;
    b.levels = argI(v, 2) ? std::max(argI(v, 3), 0) : 0;
    b.power = argFloat(v, 4);
    b.interval = std::max(static_cast<uint32_t>(argI(v, 5)), 16u);
    b.order = argI(v, 6);
    b.fade = argI(v, 7);
    for (auto& f : o->img->blurFrames) f.clear();
    return 0;
}
SQInteger setBlurMode(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->blur.mode = argI(v, 2);
    return 0;
}
// link(name) (0x50d380, the sprites' too): the text hangs from the named object's image (a sprite's
// or a text's), or from the task's root for an empty or unknown name.
SQInteger link(HSQUIRRELVM v) {
    TEXT_SELF
    auto& h = host(v);
    SpriteImage* parent = nullptr;
    std::string name = argString(v, 2);
    if (!name.empty()) {
        if (FrameTask* task = h.findTask(name)) {
            if (Drawable* d = task->drawable()) parent = d->linkNode();
        }
    }
    if (o->img) o->img->node.setParent(parent);
    return 0;
}
// The transition's timer and file (SQText 0x522ee0 / 0x522e00 / 0x5231a0).
SQInteger getTransitionTimer(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->transition.timer : 0.0f);
}
SQInteger getTransitionFilename(HSQUIRRELVM v) {
    TEXT_SELF
    sq_pushstring(v, o->img ? o->img->transitionFile.c_str() : "", -1);
    return 1;
}
SQInteger setTransitionTimer(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->transition.setTimer(argFloat(v, 2));
    return 0;
}
SQInteger setShadowShadingParameter(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->shadowShadingLevel = argI(v, 2);
    o->img->shadowShadingDensity = argFloat(v, 3);
    o->img->shadowShadingPower = argFloat(v, 4);
    return 0;
}

// Typing.
SQInteger beginType(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->beginType = argI(v, 2);
    return 0;
}
SQInteger getBeginType(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->img ? o->img->beginType : 0);
}
SQInteger isTyping(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->img && o->img->beginType && o->img->typing.active ? 1 : 0);
}
// pushAll() (0x4d6280): every character shown, typing at its end.
SQInteger pushAll(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->typingComplete = true;
    o->img->typing.setTimer(o->img->typing.actionTime);
    return 0;
}
// getSize() (0x4d9190): the laid-out width and height to SQMATH_RET_1 / 2.
SQInteger getSize(HSQUIRRELVM v) {
    TEXT_SELF
    if (o->img) host(v).setResult(static_cast<float>(o->img->layout.maxX), static_cast<float>(o->img->layout.totalHeight), 0.0f, 0.0f);
    else host(v).setResult(0.0f, 0.0f, 0.0f, 0.0f);
    return 0;
}
// getCharacterRect(i) (0x4d6d80): to SQMATH_RET_1..4 the character's place at the text's position
// moved by its own move and its overhang, and its width (adjustment + cell - overhang) and height
// (a SMALL one as tall as the main font's cells) times the text's scale times its own; zeros out
// of range.
SQInteger getCharacterRect(HSQUIRRELVM v) {
    TEXT_SELF
    auto& h = host(v);
    int i = argI(v, 2);
    if (!o->img || i < 0 || i >= static_cast<int>(o->img->layout.chars.size())) {
        h.setResult(0.0f, 0.0f, 0.0f, 0.0f);
        return 0;
    }
    const TextImage& t = *o->img;
    const TextChar& c = t.layout.chars[static_cast<size_t>(i)];
    float x = t.move.now[0] + c.x;
    x = x + c.move.now[0];
    x = x + static_cast<float>(c.overhang);
    float y = t.move.now[1] + c.y;
    y = y + c.move.now[1];
    float sx = t.scale.now[0] * c.scale.now[0], sy = t.scale.now[1] * c.scale.now[1];
    float height = static_cast<float>(c.cellHeight);
    if (c.small) {
        if (auto main = TextImage::poolFont(h, t.font)) height = static_cast<float>(main->height());
    }
    h.setResult(x, y, sx * static_cast<float>(c.adjust + c.cellWidth - c.overhang), sy * height);
    return 0;
}
// getRubyRect(i) (0x522b00 -> 0x4d6f40): the ruby character's place at the text's position moved
// by its own move, its cell times the text's scale times its own; zeros out of range.
SQInteger getRubyRect(HSQUIRRELVM v) {
    TEXT_SELF
    auto& h = host(v);
    int i = argI(v, 2);
    if (!o->img || i < 0 || i >= static_cast<int>(o->img->layout.rubies.size())) {
        h.setResult(0.0f, 0.0f, 0.0f, 0.0f);
        return 0;
    }
    const TextImage& t = *o->img;
    const TextChar& c = t.layout.rubies[static_cast<size_t>(i)];
    float x = t.move.now[0] + c.x;
    x = x + c.move.now[0];
    float y = t.move.now[1] + c.y;
    y = y + c.move.now[1];
    float sx = t.scale.now[0] * c.scale.now[0], sy = t.scale.now[1] * c.scale.now[1];
    h.setResult(x, y, sx * static_cast<float>(c.cellWidth), sy * static_cast<float>(c.cellHeight));
    return 0;
}
// The per-character effects (0x4d7740 fade, 0x4d77c0 move, 0x4d78a0 scale, 0x4d7960 rotate,
// 0x4d8420 colours) and the ruby characters' (0x4d7c20, 0x4d7ca0, 0x4d7d80, 0x4d7e40, 0x4d8730):
// from where they are, at the text's time scale, marking the character's effects; a colour for one
// corner (0..3) or all (< 0), any corner making it take its own colours. An index out of range does
// nothing (the exe checks only the top).
TextChar* character(TextObject* o, int i) {
    if (!o->img || i < 0 || i >= static_cast<int>(o->img->layout.chars.size())) return nullptr;
    return &o->img->layout.chars[static_cast<size_t>(i)];
}
TextChar* rubyCharacter(TextObject* o, int i) {
    if (!o->img || i < 0 || i >= static_cast<int>(o->img->layout.rubies.size())) return nullptr;
    return &o->img->layout.rubies[static_cast<size_t>(i)];
}
using CharAt = TextChar* (*)(TextObject*, int);
SQInteger fadeChar(HSQUIRRELVM v, CharAt at) {
    TEXT_SELF
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    c->fade.moveTo({argFloat(v, 3), 0.0f, 0.0f}, argI(v, 4), argI(v, 5));
    c->fade.timeScale = o->img->timeScale;
    c->fadeSet = c->effects = true;
    return 0;
}
SQInteger moveChar(HSQUIRRELVM v, CharAt at) {
    TEXT_SELF
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    c->move.moveTo({static_cast<float>(argI(v, 3)), static_cast<float>(argI(v, 4)), 0.0f}, argI(v, 5), argI(v, 6));
    c->move.timeScale = o->img->timeScale;
    c->moveSet = c->effects = true;
    return 0;
}
SQInteger scaleChar(HSQUIRRELVM v, CharAt at) {
    TEXT_SELF
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    c->scale.moveTo({argFloat(v, 3), argFloat(v, 4), 0.0f}, argI(v, 5), argI(v, 6));
    c->scale.timeScale = o->img->timeScale;
    c->scaleSet = c->effects = true;
    return 0;
}
SQInteger rotateChar(HSQUIRRELVM v, CharAt at) {
    TEXT_SELF
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    c->rotate.moveTo({argFloat(v, 3), argFloat(v, 4), argFloat(v, 5)}, argI(v, 6), argI(v, 7));
    c->rotate.timeScale = o->img->timeScale;
    c->rotateSet = c->effects = true;
    return 0;
}
SQInteger colorChar(HSQUIRRELVM v, CharAt at) {
    TEXT_SELF
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    int corner = argI(v, 3);
    std::array<float, 3> rgb{argFloat(v, 4), argFloat(v, 5), argFloat(v, 6)};
    for (int k = 0; k < 4; ++k) {
        if (corner >= 0 && corner != k) continue;
        c->color[static_cast<size_t>(k)].moveTo(rgb, argI(v, 7), argI(v, 8));
        c->color[static_cast<size_t>(k)].timeScale = o->img->timeScale;
    }
    c->colorSet = c->effects = true;
    return 0;
}
SQInteger fadeCharacter(HSQUIRRELVM v) { return fadeChar(v, character); }
SQInteger moveCharacter(HSQUIRRELVM v) { return moveChar(v, character); }
SQInteger scaleCharacter(HSQUIRRELVM v) { return scaleChar(v, character); }
SQInteger rotateCharacter(HSQUIRRELVM v) { return rotateChar(v, character); }
SQInteger setCharacterColor(HSQUIRRELVM v) { return colorChar(v, character); }
SQInteger fadeRuby(HSQUIRRELVM v) { return fadeChar(v, rubyCharacter); }
SQInteger moveRuby(HSQUIRRELVM v) { return moveChar(v, rubyCharacter); }
SQInteger scaleRuby(HSQUIRRELVM v) { return scaleChar(v, rubyCharacter); }
SQInteger rotateRuby(HSQUIRRELVM v) { return rotateChar(v, rubyCharacter); }
SQInteger setRubyColor(HSQUIRRELVM v) { return colorChar(v, rubyCharacter); }
// shakeCharacter / randomShakeCharacter(i, start, end, frequency, interpolation, time, packed)
// (0x521e60 / 0x521ea0 -> 0x4d7a30 / 0x4d7b10) and the ruby's (0x521fb0 / 0x521ff0 -> 0x4d7f10 /
// 0x4d7ff0): a new shake (the old one dropped) with the text's time scale; `packed` holds the tempo
// (% 100), the beat tempo (/ 100, % 100 for the random one) and the random one's seed (/ 10000).
// Phantom of Inferno's take no vectors: SQMATH_V3_1 and _2 (phantom.exe 0x140154740).
SQInteger shakeChar(HSQUIRRELVM v, CharAt at, bool random) {
    TEXT_SELF
    auto& h = host(v);
    std::optional<std::array<float, 4>> start, end;
    SQInteger first = 5;
    if (h.mathClasses) {
        start = argMath(v, 3, "Vector3D"), end = argMath(v, 4, "Vector3D");
        if (!start || !end) return sq_throwerror(v, "shakeCharacter: start and end must be Vector3D");
    } else {
        start = std::array<float, 4>{h.mathVars[0], h.mathVars[1], h.mathVars[2], 0.0f};
        end = std::array<float, 4>{h.mathVars[3], h.mathVars[4], h.mathVars[5], 0.0f};
        first = 3;
    }
    TextChar* c = at(o, argI(v, 2));
    if (!c) return 0;
    uint32_t packed = static_cast<uint32_t>(argI(v, first + 3));
    int32_t tempo = static_cast<int32_t>(packed % 100);
    int32_t beat = static_cast<int32_t>(random ? (packed / 100) % 100 : packed / 100);
    auto s = std::make_shared<Shake>(random, random ? static_cast<int32_t>(packed / 10000) : 0);
    s->setTimeScale(o->img->timeScale);
    s->set({(*start)[0], (*start)[1], (*start)[2]}, {(*end)[0], (*end)[1], (*end)[2]}, static_cast<uint32_t>(argI(v, first)),
           argI(v, first + 1), static_cast<uint32_t>(argI(v, first + 2)), tempo, beat);
    c->shake = std::move(s);
    return 0;
}
SQInteger shakeCharacter(HSQUIRRELVM v) { return shakeChar(v, character, false); }
SQInteger randomShakeCharacter(HSQUIRRELVM v) { return shakeChar(v, character, true); }
SQInteger shakeRuby(HSQUIRRELVM v) { return shakeChar(v, rubyCharacter, false); }
SQInteger randomShakeRuby(HSQUIRRELVM v) { return shakeChar(v, rubyCharacter, true); }
// getRubyBaseAlpha(i) / setRubyBaseAlpha(i, a) (0x522b50 / 0x522bb0): the ruby character's typing
// start / 255 (0 out of range), a * 255.
SQInteger getRubyBaseAlpha(HSQUIRRELVM v) {
    TEXT_SELF
    TextChar* c = rubyCharacter(o, argI(v, 2));
    return pushFloat(v, c ? c->typingStart * 0.003921568859368563f : 0.0f);
}
SQInteger setRubyBaseAlpha(HSQUIRRELVM v) {
    TEXT_SELF
    TextChar* c = rubyCharacter(o, argI(v, 2));
    if (c) c->typingStart = argFloat(v, 3) * 255.0f;
    return 0;
}
// addPositionFixRule(first, second, value) (0x521510 -> 0x4d91e0; not in You and Me and Her's exe):
// a kerning pair by the first UTF-16 unit of each string (both non-empty), (int) value pixels; an
// existing pair takes the new value, 0 removes it. The next layout reads them (cornerOffsets).
SQInteger addPositionFixRule(HSQUIRRELVM v) {
    TEXT_IMAGE
    std::u16string a = utf8ToUtf16(argString(v, 2)), b = utf8ToUtf16(argString(v, 3));
    float value = argFloat(v, 4);
    if (a.empty() || b.empty()) return 0;
    auto& rules = o->img->fixRules;
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (it->first != a[0] || it->second != b[0]) continue;
        if (value != 0.0f) it->value = static_cast<int32_t>(value);
        else rules.erase(it);
        return 0;
    }
    rules.push_back({a[0], b[0], static_cast<int32_t>(value)});
    return 0;
}
// setCharacterBaseAlpha(i, a) (0x522ab0): the typing start, a * 255.
SQInteger setCharacterBaseAlpha(HSQUIRRELVM v) {
    TEXT_SELF
    TextChar* c = character(o, argI(v, 2));
    if (c) c->typingStart = argFloat(v, 3) * 255.0f;
    return 0;
}
// getCharacterBaseAlpha(i) (0x522a50): the character's typing start / 255.
SQInteger getCharacterBaseAlpha(HSQUIRRELVM v) {
    TEXT_SELF
    int i = argI(v, 2);
    if (!o->img || i < 0 || i >= static_cast<int>(o->img->layout.chars.size())) return pushFloat(v, 0.0f);
    return pushFloat(v, o->img->layout.chars[static_cast<size_t>(i)].typingStart * 0.003921568859368563f);
}
// resetType() (0x521a10): the text laid out again (typing from the start).
SQInteger resetType(HSQUIRRELVM v) {
    TEXT_IMAGE
    std::string s = o->img->text;
    o->img->create(host(v), s);
    return 0;
}
// setTypingSpeed(s) (0x4d6c00): the duration for the new rate, the timer keeping its fraction.
SQInteger setTypingSpeed(HSQUIRRELVM v) {
    TEXT_IMAGE
    TextImage& t = *o->img;
    t.typingSpeed = argFloat(v, 2);
    float fraction = t.typing.actionTime > 0.0f ? t.typing.timer / t.typing.actionTime : 1.0f;
    float r = t.rate();
    float ms = r == 0.0f ? 0.0f : 16.0f / r * t.typing.dst[0];
    t.typing.actionTime = static_cast<float>(static_cast<int32_t>(ms));
    t.typing.refresh();
    float timer = ms * fraction;
    t.typing.timer = timer >= 0.0f ? (timer <= t.typing.actionTime ? timer : t.typing.actionTime) : 0.0f;
    if (t.typing.actionTime > 0.0f) t.typing.active = 1;
    t.typing.refresh();
    return 0;
}
SQInteger getTypingSpeed(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->typingSpeed : 0.0f);
}
// setMinTypingTime / setMaxTypingTime(seconds per character) (0x522910 / 0x522940): the rate is
// 1/60 / t * 255 alpha per frame.
SQInteger setMinTypingTime(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->minRate = (0.01666666753590107f / argFloat(v, 2)) * 255.0f;
    return 0;
}
SQInteger setMaxTypingTime(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->maxRate = (0.01666666753590107f / argFloat(v, 2)) * 255.0f;
    return 0;
}
// getMinTypingTime / getMaxTypingTime() (0x522e60 / 0x522e80): the setters undone, 1/60 / rate * 255 on
// the x87 stack (MSVC's double precision), then the float the binding returns.
SQInteger getMinTypingTime(HSQUIRRELVM v) {
    TEXT_SELF
    if (!o->img) return pushFloat(v, 0.0f);
    return pushFloat(v, static_cast<float>(static_cast<double>(0.01666666753590107f) / o->img->minRate * 255.0));
}
SQInteger getMaxTypingTime(HSQUIRRELVM v) {
    TEXT_SELF
    if (!o->img) return pushFloat(v, 0.0f);
    return pushFloat(v, static_cast<float>(static_cast<double>(0.01666666753590107f) / o->img->maxRate * 255.0));
}
SQInteger setTypingThreshold(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->typingThreshold = static_cast<int32_t>(static_cast<double>(argFloat(v, 2)) * 255.0);
    return 0;
}
SQInteger getTotalTime(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->typing.actionTime : 0.0f);
}
SQInteger getRemainTime(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->typing.actionTime - o->img->typing.timer : 0.0f);
}
SQInteger getTypingTimer(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->typing.timer : 0.0f);
}
SQInteger setTypingTimer(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->typing.setTimer(argFloat(v, 2));
    return 0;
}
// getTypingAlphaRange() (0x522d20): the typing target / 255.
SQInteger getTypingAlphaRange(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img ? o->img->typing.dst[0] * 0.003921568859368563f : 0.0f);
}

// Tweens of the whole text.
// setSurface(name) / setMask(name) (0x522390 / 0x522270): as the sprites'. setSurface also hands
// the surface (or none) to the CText (vtable +0x4c, 0x4d6190), which makes its shadow's target and
// sprite again (the sprite without the dither).
SQInteger setSurface(HSQUIRRELVM v) {
    TEXT_SELF
    setDrawableSurface(host(v), *o, argString(v, 2));
    if (o->img) o->img->shadowDither = false;
    return 0;
}
SQInteger setMask(HSQUIRRELVM v) {
    TEXT_SELF
    setDrawableMask(host(v), *o, argString(v, 2));
    return 0;
}

#define TEXT_TWEEN(Name, member, n)                                                         \
    SQInteger Name##Set(HSQUIRRELVM v) {                                                     \
        TEXT_IMAGE                                                                           \
        o->img->member.moveTo(vec(v, 2, n), argI(v, 2 + n), argI(v, 3 + n));                 \
        return 0;                                                                            \
    }                                                                                        \
    SQInteger Name##Get(HSQUIRRELVM v) {                                                     \
        TEXT_SELF                                                                            \
        if (!o->img) return n == 1 ? pushFloat(v, 0.0f) : 0;                                 \
        const Tween& t = o->img->member;                                                     \
        if (n == 1) return pushFloat(v, t.now[0]);                                           \
        host(v).setResult(t.now[0], t.now[1], n > 2 ? t.now[2] : 0.0f);                      \
        return 0;                                                                            \
    }                                                                                        \
    SQInteger Name##Active(HSQUIRRELVM v) {                                                  \
        TEXT_SELF                                                                            \
        return pushInt(v, o->img ? o->img->member.active : 0);                               \
    }                                                                                        \
    SQInteger Name##GetTimer(HSQUIRRELVM v) {                                                \
        TEXT_SELF                                                                            \
        return pushFloat(v, o->img ? o->img->member.timer : 0.0f);                           \
    }                                                                                        \
    SQInteger Name##SetTimer(HSQUIRRELVM v) {                                                \
        TEXT_IMAGE                                                                           \
        o->img->member.setTimer(argFloat(v, 2));                                             \
        return 0;                                                                            \
    }
TEXT_TWEEN(fade, fade, 1)
TEXT_TWEEN(move, move, 2)
TEXT_TWEEN(scale, scale, 2)
TEXT_TWEEN(rotate, rotate, 3)
#undef TEXT_TWEEN

// isMoving() (0x522d40): the move (CText +0x310) or the bezier (+0x458) under way; Totono's CText
// vtable +0x1dc (0x4aceb0: +0x330 / +0x474) and Phantom's (+0x35c / +0x4d0) the same. The spline
// is not asked.
SQInteger isMoving(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->img && (o->img->move.active || o->img->bezier.t.active) ? 1 : 0);
}

// setFontColor(corner, r, g, b, ms, curve) (0x4d8a40): one corner or all (corner < 0); getFontColor
// (corner): r, g, b and the fade to SQMATH_RET_1..4. The same for the shadow.
SQInteger setColors(HSQUIRRELVM v, std::array<Tween, 4>& colors) {
    int i = argI(v, 2);
    auto to = vec(v, 3, 3);
    int32_t ms = argI(v, 6), curve = argI(v, 7);
    if (i < 0) {
        for (auto& c : colors) c.moveTo(to, ms, curve);
    } else if (i < 4) {
        colors[static_cast<size_t>(i)].moveTo(to, ms, curve);
    }
    return 0;
}
SQInteger setFontColor(HSQUIRRELVM v) {
    TEXT_IMAGE
    return setColors(v, o->img->fontColor);
}
SQInteger setShadowColor(HSQUIRRELVM v) {
    TEXT_IMAGE
    return setColors(v, o->img->shadowColor);
}
SQInteger getColors(HSQUIRRELVM v, const std::array<Tween, 4>& colors, float alpha) {
    int i = argI(v, 2);
    if (i < 0 || i > 3) i = 0;
    const Tween& c = colors[static_cast<size_t>(i)];
    host(v).setResult(c.now[0], c.now[1], c.now[2], alpha);
    return 0;
}
SQInteger getFontColor(HSQUIRRELVM v) {
    TEXT_IMAGE
    return getColors(v, o->img->fontColor, o->img->fade.now[0]);
}
SQInteger getShadowColor(HSQUIRRELVM v) {
    TEXT_IMAGE
    return getColors(v, o->img->shadowColor, o->img->fade.now[0]);
}

// Plain properties.
SQInteger setCenter(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->centerX = argFloat(v, 2);
    o->img->centerY = argFloat(v, 3);
    return 0;
}
SQInteger setPivot(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->pivotX = argFloat(v, 2);
    o->img->pivotY = argFloat(v, 3);
    return 0;
}
SQInteger setBlendMode(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->blendMode = argI(v, 2);
    return 0;
}
SQInteger setTextureFiltering(HSQUIRRELVM v) {
    TEXT_IMAGE
    o->img->textureFiltering = argI(v, 2);
    return 0;
}
// setTimeScale(s) (CText 0x4d8d30): the text's tweens (not the spline's or the shadow colours'),
// its shake, and each character's and ruby character's tweens and shake.
SQInteger setTimeScale(HSQUIRRELVM v) {
    TEXT_SELF
    float ts = argFloat(v, 2);
    o->timeScale = ts;
    if (o->img) {
        TextImage& t = *o->img;
        t.timeScale = ts;
        for (Tween* tw : {&t.fade, &t.move, &t.scale, &t.rotate, &t.transition, &t.bezier.t, &t.typing}) tw->timeScale = ts;
        if (t.shake) t.shake->setTimeScale(ts);
        for (auto& c : t.fontColor) c.timeScale = ts;
        auto own = [ts](TextChar& c) {
            c.move.timeScale = c.fade.timeScale = c.scale.timeScale = c.rotate.timeScale = ts;
            if (c.shake) c.shake->setTimeScale(ts);
            for (auto& k : c.color) k.timeScale = ts;
        };
        for (auto& c : t.layout.chars) own(c);
        for (auto& c : t.layout.rubies) own(c);
    }
    return 0;
}
// shake / randomShake (0x522050 / 0x522080 -> CText 0x4d75c0 / 0x4d7670): a new shake replaces the
// old, with the text's time scale; isShaking / getShakeTimer / setShakeTimer (0x522d90 / 0x522ef0 /
// 0x523250) as a sprite's.
SQInteger startTextShake(HSQUIRRELVM v, bool random) {
    TEXT_SELF
    std::unique_ptr<Shake> s;
    SQInteger r = shakeFromArgs(v, random, o->img ? o->img->timeScale : 1.0f, s);
    if (r < 0) return r;
    if (o->img) o->img->shake = std::move(s);
    return 0;
}
SQInteger shake(HSQUIRRELVM v) { return startTextShake(v, false); }
SQInteger randomShake(HSQUIRRELVM v) { return startTextShake(v, true); }
SQInteger isShaking(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->img && o->img->shake && o->img->shake->shaking() ? 1 : 0);
}
SQInteger getShakeTimer(HSQUIRRELVM v) {
    TEXT_SELF
    return pushFloat(v, o->img && o->img->shake ? o->img->shake->timer() : 0.0f);
}
SQInteger setShakeTimer(HSQUIRRELVM v) {
    TEXT_SELF
    if (!o->img || !o->img->shake) return 0;
    float t = argFloat(v, 2);
    float time = o->img->shake->time();
    o->img->shake->setTimer(t >= 0.0f ? (time < t ? time : t) : 0.0f);
    return 0;
}
SQInteger setPriority(HSQUIRRELVM v) {
    TEXT_SELF
    host(v).setDrawPriority(o, argI(v, 2));
    return 0;
}
SQInteger getPriority(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, host(v).drawPriority(o));
}
SQInteger setErase(HSQUIRRELVM v) {
    TEXT_SELF
    o->erase = argI(v, 2);
    return 0;
}
SQInteger getErase(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->erase);
}
SQInteger setLock(HSQUIRRELVM v) {
    TEXT_SELF
    o->lock = argI(v, 2);
    return 0;
}
SQInteger getLock(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->lock);
}
SQInteger setDisused(HSQUIRRELVM v) {
    TEXT_SELF
    o->disused = argI(v, 2);
    return 0;
}
SQInteger getDisused(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->disused);
}
SQInteger setHidable(HSQUIRRELVM v) {
    TEXT_SELF
    o->hidable = argI(v, 2);
    return 0;
}
SQInteger getHidable(HSQUIRRELVM v) {
    TEXT_SELF
    return pushInt(v, o->hidable);
}

}  // namespace

// serialize(file) / deserialize(file): the text's engine state in the save (natives_vmstate.cpp).
SQInteger serializeText(HSQUIRRELVM v) {
    TEXT_SELF
    EngineState st;
    st.hasImage = o->img != nullptr;
    if (o->img) {
        st.chars = &o->img->layout.chars, st.rubies = &o->img->layout.rubies, st.fixRules = &o->img->fixRules, st.shake = &o->img->shake;
        st.textBlur = &o->img->blur, st.textBlurFrames = &o->img->blurFrames;
    }
    bool ok = writeEngineRecord(*o->host, argString(v, 2), EngineRecord::Text, st);
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}
SQInteger deserializeText(HSQUIRRELVM v) {
    TEXT_SELF
    EngineState st;
    st.hasImage = o->img != nullptr;
    if (o->img) {
        st.chars = &o->img->layout.chars, st.rubies = &o->img->layout.rubies, st.fixRules = &o->img->fixRules, st.shake = &o->img->shake;
        st.textBlur = &o->img->blur, st.textBlurFrames = &o->img->blurFrames;
    }
    sq_pushinteger(v, readEngineRecord(*o->host, argString(v, 2), EngineRecord::Text, st) ? 1 : 0);
    return 1;
}

ClassNatives textObjectNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {
        {"initialize", initialize},
        {"serialize", serializeText},
        {"deserialize", deserializeText},
        {"create", create},
        {"print", print},
        {"clear", clear},
        {"getString", getString},
        {"setField", setField},
        {"getFieldSize", getFieldSize},
        {"getFieldType", getFieldType},
        {"setFont", setFont},
        {"setRubyFont", setRubyFont},
        {"setPitchSpace", setPitchSpace},
        {"setRowSpace", setRowSpace},
        {"setRubyOffset", setRubyOffset},
        {"setRubyMarginRate", setRubyMarginRate},
        {"setShadow", setShadow},
        {"setShadowShadingParameter", setShadowShadingParameter},
        {"resetSpline", resetSpline},
        {"setSpline", setSpline},
        {"beginSpline", beginSpline},
        {"getSplineTimer", getSplineTimer},
        {"setSplineTimer", setSplineTimer},
        {"resetBezier", resetBezier},
        {"setBezier", setBezier},
        {"beginBezier", beginBezier},
        {"getBezierTimer", getBezierTimer},
        {"setBezierTimer", setBezierTimer},
        {"getColorTimer", getColorTimer},
        {"setColorTimer", setColorTimer},
        {"transition", transition},
        {"setDither", setDither},
        {"setDisplacementMap", setDisplacementMap},
        {"setBlur", setBlur},
        {"setBlurMode", setBlurMode},
        {"link", link},
        {"getTransitionTimer", getTransitionTimer},
        {"getTransitionFilename", getTransitionFilename},
        {"setTransitionTimer", setTransitionTimer},
        {"beginType", beginType},
        {"getBeginType", getBeginType},
        {"isTyping", isTyping},
        {"pushAll", pushAll},
        {"getSize", getSize},
        {"getCharacterRect", getCharacterRect},
        {"getCharacterBaseAlpha", getCharacterBaseAlpha},
        {"setCharacterBaseAlpha", setCharacterBaseAlpha},
        {"fadeCharacter", fadeCharacter},
        {"moveCharacter", moveCharacter},
        {"scaleCharacter", scaleCharacter},
        {"rotateCharacter", rotateCharacter},
        {"setCharacterColor", setCharacterColor},
        {"shakeCharacter", shakeCharacter},
        {"randomShakeCharacter", randomShakeCharacter},
        {"fadeRuby", fadeRuby},
        {"moveRuby", moveRuby},
        {"scaleRuby", scaleRuby},
        {"rotateRuby", rotateRuby},
        {"shakeRuby", shakeRuby},
        {"randomShakeRuby", randomShakeRuby},
        {"setRubyColor", setRubyColor},
        {"getRubyRect", getRubyRect},
        {"getRubyBaseAlpha", getRubyBaseAlpha},
        {"setRubyBaseAlpha", setRubyBaseAlpha},
        {"addPositionFixRule", addPositionFixRule},
        {"resetType", resetType},
        {"setTypingSpeed", setTypingSpeed},
        {"getTypingSpeed", getTypingSpeed},
        {"setMinTypingTime", setMinTypingTime},
        {"setMaxTypingTime", setMaxTypingTime},
        {"getMinTypingTime", getMinTypingTime},
        {"getMaxTypingTime", getMaxTypingTime},
        {"setTypingThreshold", setTypingThreshold},
        {"getTotalTime", getTotalTime},
        {"getDurationTime", getTotalTime},
        {"getRemainTime", getRemainTime},
        {"getTypingTimer", getTypingTimer},
        {"setTypingTimer", setTypingTimer},
        {"getTypingAlphaRange", getTypingAlphaRange},
        {"fade", fadeSet},
        {"getFade", fadeGet},
        {"isFading", fadeActive},
        {"getFadeTimer", fadeGetTimer},
        {"setFadeTimer", fadeSetTimer},
        {"move", moveSet},
        {"getMove", moveGet},
        {"isMoving", isMoving},
        {"getMoveTimer", moveGetTimer},
        {"setMoveTimer", moveSetTimer},
        {"scale", scaleSet},
        {"getScale", scaleGet},
        {"isScaling", scaleActive},
        {"getScaleTimer", scaleGetTimer},
        {"setScaleTimer", scaleSetTimer},
        {"rotate", rotateSet},
        {"getRotate", rotateGet},
        {"isRotating", rotateActive},
        {"getRotateTimer", rotateGetTimer},
        {"setRotateTimer", rotateSetTimer},
        {"setFontColor", setFontColor},
        {"getFontColor", getFontColor},
        {"setShadowColor", setShadowColor},
        {"getShadowColor", getShadowColor},
        {"setCenter", setCenter},
        {"setPivot", setPivot},
        {"setBlendMode", setBlendMode},
        {"setTextureFiltering", setTextureFiltering},
        {"setTimeScale", setTimeScale},
        {"shake", shake},
        {"randomShake", randomShake},
        {"isShaking", isShaking},
        {"getShakeTimer", getShakeTimer},
        {"setShakeTimer", setShakeTimer},
        {"setPriority", setPriority},
        {"setSurface", setSurface},
        {"setMask", setMask},
        {"getPriority", getPriority},
        {"setErase", setErase},
        {"getErase", getErase},
        {"setLock", setLock},
        {"getLock", getLock},
        {"setDisused", setDisused},
        {"getDisused", getDisused},
        {"setHidable", setHidable},
        {"getHidable", getHidable},
    };
    return c;
}

}  // namespace nyan::diesel
