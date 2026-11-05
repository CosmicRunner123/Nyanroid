#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <array>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <limits>
#include <vector>

#include "nyan/media/mpeg_video.hpp"
#include "nyan/cs2/anm.hpp"
#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/emote.hpp"
#include "nyan/cs2/fes_runtime.hpp"
#include "nyan/cs2/kx2.hpp"
#include "nyan/cs2/stage.hpp"
#include "nyan/cs2/window_theme.hpp"
#include "nyan/render/gl.hpp"

namespace nyan::render {

class FontAtlas;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Draws a cs2::Stage with OpenGL ES 3.0. Must be created, used and destroyed
// on the thread that owns the GL context.
class StageRenderer {
public:
    struct Options {
        int virtualWidth = 1024;
        int virtualHeight = 576;
        uint32_t windowColor = 0xCC000C10;  // ARGB
        bool textAround = true, textRightDown = true;  // startup.xml FONT/shadow (see FontAtlas::edge); setTextShadow
        size_t textureBudgetBytes = 384u * 1024 * 1024;
        const cs2::WindowTheme* theme = nullptr;  // real window art/layout; null = generic window
        int fontFace = 0;                         // face index within a font collection
        // "User font": characters drawn as frames of an image (code point -> frame id), e.g. a heart.
        std::string userFontImage;
        std::map<uint32_t, int32_t> userFontGlyphs;
        // vram.xml layout: where a plane the script has not touched puts its surface (pos - base),
        // per plane with a default, and each image slot's shift onto its plane's surface.
        std::pair<int32_t, int32_t> planeOffset{0, 0};
        std::map<int32_t, std::pair<int32_t, int32_t>> planeOffsets;
        // Fonts scripts name (`layout font <face>` / `fontind <n> <file>`; empty: the game's own) at
        // a weight (400, 700 bold): data and face index, or empty data if the game has no such
        // font; `fakeBold` set when bold was asked of a face that is not bold.
        std::function<Bytes(const std::string& name, int32_t weight, int& face, bool& fakeBold)> fontLoader;
        // vram.xml: each plane's window in the VRAM canvas (x, y, width, height).
        std::map<int32_t, std::array<int32_t, 4>> planeVram;
        std::map<std::pair<cs2::Layer, int32_t>, std::pair<int32_t, int32_t>> imageShift;
        // Textures named "@..." (e.g. save thumbnails) come from here: RGBA, top row first.
        std::function<bool(const std::string& name, int& w, int& h, Bytes& rgba)> externalTexture;
    };

    StageRenderer(const cs2::AssetIndex& assets, Bytes font, Options options);
    ~StageRenderer();
    StageRenderer(const StageRenderer&) = delete;
    StageRenderer& operator=(const StageRenderer&) = delete;

    // Renders a frame into framebuffer `target` (0 = the window) of size (width, height).
    // `overlay`: quads from a running FE script, drawn over the stage and under the message window.
    // `window`: the message window's own script output (art, buttons); replaces the reconstructed window art.
    void render(const cs2::Stage& stage, uint64_t frame, int width, int height, GLuint target = 0,
                const std::vector<cs2::FesQuad>& overlay = {}, const std::vector<cs2::FesQuad>* window = nullptr,
                const std::vector<cs2::FesQuad>* top = nullptr);
    // NVL pages: where the page's text ends (last line's top), relative to the text box; set by render().
    std::optional<std::pair<int32_t, int32_t>> novelTextEnd() const { return novelEnd_; }
    // `top`: engine screens over everything (the in-game options screen).
    // A full-screen movie frame (RGBA, w x h), fitted into the game's screen; `changed`: upload it.
    void renderMovie(const uint8_t* rgba, int w, int h, bool changed, int width, int height, GLuint target = 0);
    // Hide Message Window: the window and its text are not drawn.
    void setWindowHidden(bool hidden) { windowHidden_ = hidden; }
    // Default message / name text colours (the window script's `str` / `name` layout colour); 0 = white.
    // Fonts the window script chose for the message and the name (empty: the game's), and whether
    // each uses proportional spacing (`layout fontfix 0`).
    // The weight the game's text is drawn at (cs2::Game::textWeight: 700 with the config's
    // `fontweight`), for every font.
    void setTextWeight(int32_t weight) { textWeight_ = weight; }
    // The text edge (see FontAtlas::edge) as the config has it (cs2::Game::textAround /
    // textRightDown: `fshadow 0` / `fshadow 1`, from startup.xml FONT/shadow).
    void setTextShadow(bool around, bool rightdown) {
        textAround_ = around;
        textRightDown_ = rightdown;
    }
    void setTextFonts(std::string text, bool textProportional, std::string name, bool nameProportional) {
        textFont_ = std::move(text);
        nameFont_ = std::move(name);
        textProportional_ = textProportional;
        nameProportional_ = nameProportional;
    }
    // The `effspd` setting (0-4): Task_Eff_Raster advances its wave x1, 1.5, 2, 3 or 4 per frame.
    // The first window-face E-mote bank (ScenePlayer::Layout::mplFaceFrom).
    void setMplFaceFrom(int32_t n) { mplFaceFrom_ = n; }
    // Whether the message window is shown: the face banks (from mplFaceFrom) show only then.
    void setFacesShown(bool on) { facesShown_ = on; }
    void setEffectSpeed(int32_t level) {
        static constexpr float kSpeed[5] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
        effectSpeed_ = kSpeed[std::clamp(level, 0, 4)];
    }
    // The window script's live text layout, overriding the theme's static one where set.
    void setTextLayout(const cs2::FesInstance::StringLayout& l) { liveLayout_ = l; }
    // The speaker's name as the window script wrote it (nullopt: the scene's name).
    // `align` as FesInstance::objectAlign has it (0 left, 1 right, 2 centred in the name box).
    void setWindowName(std::optional<std::string> name, int align = 0) {
        scriptName_ = std::move(name);
        scriptNameAlign_ = align;
    }
    // The window script's sub-language text object (`strsub`): layout and font.
    void setSubText(const cs2::FesInstance::StringLayout& l, std::string font, bool proportional) {
        subLayout_ = l;
        subFont_ = std::move(font);
        subProportional_ = proportional;
    }
    void setTextColors(uint32_t text, uint32_t name, uint32_t textEdge = 0, uint32_t nameEdge = 0) {
        textColor_ = text;
        nameColor_ = name;
        textEdge_ = textEdge;
        nameEdge_ = nameEdge;
    }

    // Converts window pixel coordinates to virtual-screen coordinates (last rendered layout).
    void toVirtual(float wx, float wy, float& vx, float& vy) const;
    // Index of the choice button at virtual coordinates, or -1.
    int choiceAt(const cs2::Stage& stage, float vx, float vy) const;
    void setHoveredChoice(int index) { hovered_ = index; }
    // The game draws its own choice screen: our stand-in choice buttons stay hidden.
    void setChoicesHidden(bool hidden) { choicesHidden_ = hidden; }
    // What the scripts beside the message window draw (WindowScript::drawSide), over the window.
    void setSideQuads(std::vector<cs2::FesQuad> quads) { sideQuads_ = std::move(quads); }
    // The window script's quads above this priority (its message string's) draw over the text;
    // the rest under it (Nekopara's quick-save card, 61000, over the text, 60300).
    void setWindowTextLayer(int32_t layer) { windowTextLayer_ = layer; }
    // Black over the whole game frame, 0..1 (the fade around a load).
    void setScreenFade(float a) { screenFade_ = a; }
    // The story (the stage, its overlays and the window) not drawn: black under the top screen
    // (an in-game menu's, Game::menuHidesStory).
    void setStoryHidden(bool on) { storyHidden_ = on; }
    // The config's messpd, for a window whose `layout frame` is -1.
    void setMessageSpeed(int32_t messpd) { messageSpeed_ = messpd; }
    // The last frame's wipe reveal has passed the end of the window's text.
    // The wipe of the message set at frame `start` has passed the end of its text.
    bool wipeDone(uint64_t start) const { return wipeDone_ && wipeDoneStart_ == start; }
    // An FE text box as drawTextBox lays it out: the lines' lengths added up (virtual pixels), the
    // length a wipe reveal runs along.
    std::optional<float> textWipeLength(const cs2::FesQuad& q);
    std::optional<int32_t> textLineCount(const cs2::FesQuad& q);
    std::optional<std::pair<float, float>> textDrawArea(const cs2::FesQuad& q);

    // The last rendered scene (stage only, no UI) scaled to w x h: RGBA, top row first.
    Bytes captureThumbnail(int w, int h);
    // One image drawn alone at the virtual screen's size, on black: ARGB, top row first (touch
    // mode's hit surfaces: sscript hides everything but the bank and captures the screen).
    std::vector<uint32_t> captureImage(const cs2::StageImage& img, uint64_t frame);

    // Drops all cached textures (e.g. when the app goes to the background).
    void trimCache();

private:
    int32_t mplFaceFrom_ = 10;
    bool facesShown_ = true;
    bool windowHidden_ = false;
    bool choicesHidden_ = false;
    uint32_t textColor_ = 0, nameColor_ = 0;
    uint32_t textEdge_ = 0, nameEdge_ = 0;  // outline colours; 0 = black
    cs2::FesInstance::StringLayout liveLayout_, subLayout_;
    std::optional<std::string> scriptName_;
    int scriptNameAlign_ = 0;
    std::string subFont_;
    bool subProportional_ = false;
    float effectSpeed_ = 1.0f;  // setEffectSpeed
    struct Texture {
        GLuint id = 0;
        int width = 0, height = 0;
        int offsetX = 0, offsetY = 0, baseX = 0, baseY = 0;
        int canvasW = 0, canvasH = 0;  // the frame's full canvas (the pixels sit at offsetX/Y in it)
        std::map<uint32_t, std::pair<int, int>> attributes;  // the frame's named points (HG-3 "ats")
        size_t bytes = 0;
        std::list<std::string>::iterator lru;
    };
    struct Vertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    // Mix: two textures (premultiplied) mixed by batchMix_ as straight colour and alpha.
    enum class Mode : int { Texture = 0, Solid = 1, Glyph = 2, Mix = 3 };

    const Texture* texture(const std::string& name);
    const cs2::Kx2Mesh* mesh(const std::string& name);
    // An image's parts merged into one texture (premultiplied), cached; null if they can't be.
    const Texture* partsTexture(const std::vector<std::string>& parts);
    std::unordered_map<std::string, std::optional<cs2::Kx2Mesh>> meshes_;
    // E-mote models (`mpl` banks) and their atlas textures, most recently used first; a few are kept.
    struct EmoteEntry {
        std::shared_ptr<const cs2::EmoteModel> model;  // the scene player's model (shared)
        GLuint tex = 0;
    };
    std::list<EmoteEntry> emotes_;
    EmoteEntry* emote(const std::shared_ptr<const cs2::EmoteModel>& model);
    // A bank's crossfade (StageImage::emoteFadeFrom): both poses, frozen, drawn once into textures
    // over their joint bounds (bank-local units, k texels per unit).
    struct EmoteFade {
        const cs2::EmotePlayer* from = nullptr;
        uint64_t start = 0, used = 0;
        GLuint a = 0, b = 0;
        float x0 = 0, y0 = 0, w = 0, h = 0;
    };
    std::list<EmoteFade> emoteFades_;
    GLuint poseTexture(EmoteEntry& e, const cs2::EmotePlayer& player, float offX, float offY, float modelScale, float x0, float y0,
                       float k, int w, int h);
    void sweepEmoteFades(uint64_t frame);
    // The user-font image frame drawn for `cp`, or null when the font draws it.
    const Texture* userGlyph(uint32_t cp);
    bool isUserGlyph(uint32_t cp) const { return opt_.userFontGlyphs.count(cp) != 0; }
    // Draws a user-font frame filling a `cell`-pixel square whose top-left is (x, y), window pixels.
    void drawUserGlyph(const Texture& t, float x, float y, float cell, float alpha);
    void evict();

    // Batched quad drawing in the current target's pixel space.
    void begin(int targetW, int targetH);
    void quad(Mode mode, GLuint tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t argb, float alpha);
    // As quad, the opacity going linearly from `left` at the left edge to `right` at the right.
    void quadLR(Mode mode, GLuint tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t argb, float left,
                float right);
    struct Vec2 {
        float x, y;
    };
    // Arbitrary quad: corners top-left, top-right, bottom-right, bottom-left.
    void quadCorners(Mode mode, GLuint tex, const Vec2 c[4], float u0, float v0, float u1, float v1, uint32_t argb,
                     float alpha);
    // As quadCorners, each corner with its own colour and opacity (ARGB), blended across the quad.
    void quadCorners(Mode mode, GLuint tex, const Vec2 c[4], float u0, float v0, float u1, float v1,
                     const uint32_t argb[4]);
    // Plane-local -> screen transform for one frame.
    struct PlaneXform {
        float baseX = 0, baseY = 0, posX = 0, posY = 0;
        // The rotation as a 2x2 (row vector times matrix): z turn and the x/y tilts, drawn flat.
        float m00 = 1, m01 = 0, m10 = 0, m11 = 1, scaleX = 1, scaleY = 1;
        float alpha = 1;
        uint32_t color = 0xFFFFFFFF;  // the plane's `color`, modulating what it draws
        bool visible = true;
        // The 2D camera after the plane (cs2::kCameraPlane): the point v goes to
        // C - P + (S (v - C)) R, C (camCX, camCY) the screen centre, P (camX, camY), S and R as
        // the camera's scale and its turn (row vector times matrix).
        bool camera = false;
        float camCX = 0, camCY = 0, camX = 0, camY = 0, camSX = 1, camSY = 1;
        float c00 = 1, c01 = 0, c10 = 0, c11 = 1;
        Vec2 apply(float x, float y) const;
    };
    // The camera's part of a plane's transform (PlaneXform::camera).
    void cameraXform(const cs2::Stage& stage, uint64_t frame, PlaneXform& xf) const;
    PlaneXform planeXform(const cs2::Stage& stage, int32_t plane, uint64_t frame) const;
    void flush();

    void drawScene(const cs2::Stage& stage, uint64_t frame);
    // An image drawn through a plane; off: its VRAM offset when shown through another plane's window.
    struct PlaneItem {
        cs2::Layer layer;
        int32_t index;
        const cs2::StageImage* img;
        float offX, offY;
    };
    // The plane's window in VRAM (x, y, width, height), if it has a known one.
    std::optional<std::array<int32_t, 4>> vramWindow(const cs2::Stage& stage, int32_t plane) const;
    void setBlendMode(uint8_t mode);
    uint8_t blendMode_ = 0;  // the mode setBlendMode last set
    PlaneXform afterXform(const cs2::Stage& stage, int32_t plane, uint64_t frame, int32_t copy) const;
    void drawPlane(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, uint64_t frame);
    // Each plane's images (with what other planes show through its VRAM window), in depth order.
    std::map<int32_t, std::vector<PlaneItem>> planeItems(const cs2::Stage& stage) const;
    void drawPlaneFade(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, uint64_t frame);
    // clip: apply the plane's shape and size (off when drawing it flat into its own surface).
    // A particle layer, drawn on its own among the planes (drawScene).
    void drawParticles(const cs2::Stage& stage, const cs2::ParticleLayer& p, uint64_t frame);
    void drawPlaneContent(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, const PlaneXform& xf,
                          uint64_t frame, bool clip = true);
    void drawRaster(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, const PlaneXform& xf,
                    const cs2::Kx2Mesh& grid, const cs2::Raster& wave, uint64_t frame);
    // A mask image's texture and its screen rectangle (drawImage with `mask` set computes this
    // instead of drawing).
    struct ImageMask {
        GLuint tex = 0;
        Rect rect{};
    };
    void drawImage(const cs2::StageImage& img, uint64_t frame, const PlaneXform& plane, float scale, float ox,
                   float oy, float shiftX = 0, float shiftY = 0, ImageMask* mask = nullptr);
    void drawEmote(const cs2::StageImage& img, EmoteEntry& e, uint64_t frame, const PlaneXform& plane, float scale,
                   float ox, float oy, float ax, float ay, float sx, float sy, float alpha);
    // A pose's images through `toScreen` (model space -> target), with blend modes and stencils.
    void emitEmote(const std::vector<cs2::EmoteModel::Draw>& draws, const EmoteEntry& e,
                   const std::function<Vec2(float, float)>& toScreen, std::array<float, 3> tone, float alpha,
                   bool stencils);
    void drawEmoteFade(const cs2::StageImage& img, uint64_t frame, const PlaneXform& plane, float scale, float ox,
                       float oy, float ax, float ay, float sx, float sy, float alpha);
    void composite(const cs2::Stage& stage, uint64_t frame);
    void drawPageFlip(const cs2::Transition& tr, float t);
    void drawBreak(const cs2::Transition& tr, uint64_t elapsed);
    void drawNovelPage(const cs2::MessageWindow& w, const cs2::WindowLayout& L, const Rect& area, const float sizes[5],
                       float lineHeight, float alpha);
    void drawUi(const cs2::Stage& stage, uint64_t frame);
    void drawWindowQuads(bool overText);
    void drawQuads(const std::vector<cs2::FesQuad>& quads);
    std::vector<cs2::FesQuad> lastOverlay_;  // the screen drawn over the stage last frame (thumbnails)
    void drawTextBox(const cs2::FesQuad& q);
    // drawTextBox's layout (in the box font, cur_): code points, their sizes and cell widths, lines.
    struct BoxLayout {
        std::vector<uint32_t> cps;
        std::vector<float> sizes, cells;
        struct Line {
            size_t begin, end;
            float width;
        };
        std::vector<Line> lines;
        float lineH = 0, boxW = 0;
    };
    BoxLayout layoutTextBox(const cs2::FesQuad& q);
    const std::vector<cs2::FesQuad>* windowQuads_ = nullptr;
    int32_t windowTextLayer_ = std::numeric_limits<int32_t>::max();
    void setFrameScissor();
    void drawText(const std::string& text, float x, float y, float sizeV, uint32_t argb, float alpha, size_t maxChars);
    struct LaidGlyph {
        uint32_t cp;
        float x, y;  // virtual coords of pen on baseline
        float size;  // virtual pixel size
        uint32_t color;
        float wipe = -1;  // the wipe reveal: where the glyph's left edge is along the text (-1: shown)
        float pen = 0, adv = 0;  // window pixels: the glyph's cell start and width
        uint8_t align = 0;       // its span's `\pl` / `\pc` / `\pr`
        bool italic = false;     // its span's `\ti;`
    };
    // The wipe reveal in progress (window pixels): the wipe's trailing edge along the text and its
    // soft width. Glyphs with a `wipe` offset take their opacity from it.
    // Glyphs drawn slanted (`\ti;`): each vertex moved right by shear_ times its height over the
    // baseline shearY_ (window pixels); 0 upright.
    float shear_ = 0, shearY_ = 0;
    bool wipeOn_ = false;
    float wipeHead_ = 0, wipeWidth_ = 1;
    bool wipeDone_ = false;
    uint64_t wipeDoneStart_ = UINT64_MAX;  // the message (its wipeStart) wipeDone_ is about
    int32_t messageSpeed_ = 4;
    float wipeAlpha(float offset) const;
    float placeWipe(std::vector<LaidGlyph>& glyphs, size_t from);
    // sizes: virtual pixel size for each cs2::TextSize level (Smaller..Larger); lineHeight in virtual pixels.
    // alignBox: the left and width centred or right-aligned text starting a line is placed in (the
    // area when null).
    std::vector<LaidGlyph> layoutMessage(const cs2::MessageText& text, const Rect& area, const float sizes[5],
                                         float lineHeight, const Rect* alignBox = nullptr) const;
    void drawGlyphs(const std::vector<LaidGlyph>& glyphs, size_t count, float alpha, bool outline,
                    uint32_t defaultColor = 0, uint32_t edgeColor = 0);
    std::vector<Rect> choiceRects(const cs2::Stage& stage) const;
    float measure(const std::string& text, float sizeV) const;

    const cs2::AssetIndex& assets_;
    Options opt_;
    std::unique_ptr<FontAtlas> font_;
    // Script-chosen fonts by name, and bold ones (the game's own too) by name and weight.
    std::map<std::pair<std::string, int32_t>, std::unique_ptr<FontAtlas>> fonts_;
    int32_t textWeight_ = 400;
    bool textAround_ = true, textRightDown_ = true;
    FontAtlas* cur_ = nullptr;   // the font text is being laid out / drawn in
    bool proportional_ = false;  // that text uses glyph advances, not the cell grid
    bool hanging_ = false;
    // For the next layoutMessage: the right margin, room a word or a closing character may still
    // take at a line's end, and the text object's line-break sets (null: the engine's defaults).
    float wrapSlack_ = 0;
    const std::string* kinsoku_ = nullptr;
    bool storyHidden_ = false;
    float screenFade_ = 0;       // `layout indent`: later lines hang under an opening bracket
    std::string textFont_, nameFont_;
    bool textProportional_ = false, nameProportional_ = false;
    FontAtlas* atlasFor(const std::string& key, int32_t weight = 0);  // weight 0: the config's

    GLuint quadProgram_ = 0, transitionProgram_ = 0;
    GLint uTarget_ = -1, uMode_ = -1, uTex_ = -1, uMosaic_ = -1, uPremul_ = -1, uTex2_ = -1, uMix_ = -1, uOutPremul_ = -1, uAlphaTest_ = -1;
    GLint uScreenMask_ = -1, uHasScreenMask_ = -1, uMaskRect_ = -1;
    GLuint screenMask_ = 0;  // while set, drawing is limited by this texture's alpha over screenMaskRect_
    Rect screenMaskRect_{};
    bool premul_ = false;  // the current batch's texture holds premultiplied colour
    bool multiply_ = false;  // the current batch blends as an E-mote multiply layer
    bool alphaTest_ = false;  // the current batch discards alpha <= 64/255 (E-mote stencil masks)
    std::vector<cs2::FesQuad> sideQuads_;
    std::array<float, 3> tone_{1, 1, 1};  // colour scale for quads being emitted (an image's `tone`)
    GLint uPrev_ = -1, uNext_ = -1, uMask_ = -1, uT_ = -1, uKind_ = -1, uHasMask_ = -1, uHard_ = -1;
    // An FE script's `rwipe` / `rdraw` over the whole screen: the frame as last shown, drawn over the
    // new one and wiped away.
    GLuint wipeProgram_ = 0;
    GLint uWipePrev_ = -1, uWipeMask_ = -1, uWipeT_ = -1, uWipeHasMask_ = -1;
    GLuint vao_ = 0, vbo_ = 0, emptyVao_ = 0;
    std::vector<Vertex> batch_;
    GLuint batchTex_ = 0, batchTex2_ = 0;
    float batchMix_ = 0;
    Mode batchMode_ = Mode::Texture;
    float targetW_ = 0, targetH_ = 0;

    // Offscreen targets at virtual resolution: scene being drawn, previous frame snapshot, composited output.
    struct Target {
        GLuint fbo = 0, tex = 0;
        GLuint stencil = 0;  // scene target only: plane shapes are stencilled
    };
    Target scene_, prev_, display_;
    Target surface_;  // a plane drawn flat for `raster`, sized to its mesh
    // A plane's own cross-fade (`draw ... <plane>`): its images before and after, and their mix.
    std::optional<std::pair<int32_t, int32_t>> novelEnd_;
    GLuint movieTex_ = 0;
    int movieW_ = 0, movieH_ = 0;
    // Plane videos (`pl N video`): a decoder and texture per plane, stepped by the stage's clock.
    struct PlaneVideo {
        std::string name;
        std::unique_ptr<media::MpegVideo> video;
        uint64_t start = UINT64_MAX;  // the plane's videoStart this state follows
        uint64_t lapStart = 0;        // frame the current play-through began
        int32_t lapsDone = 0;
        GLuint tex = 0;
        bool uploaded = false;
    };
    std::map<int32_t, PlaneVideo> planeVideos_;
    // The plane's video picture for this frame (decoding up to it), or null.
    PlaneVideo* planeVideo(int32_t n, const cs2::Plane& p, uint64_t frame);
    Target fadeOld_, fadeNew_, fadeMix_;
    // The finished frame (game area) while an FE script shows: what its `rwipe` starts from.
    Target lastFrame_;
    uint64_t lastFrameAt_ = 0;
    bool haveLastFrame_ = false;
    uint64_t wipeStart_ = UINT64_MAX;
    bool wipeFromSnapshot_ = false;
    void drawScreenWipe(const std::vector<cs2::FesQuad>& overlay, uint64_t frame, float vw, float vh);
    int surfaceW_ = 0, surfaceH_ = 0;
    uint64_t lastTransitionId_ = 0;
    // `wbreak`'s pieces (see drawBreak): each triangle's centre, its offset so far, velocity, spin
    // per frame about x, y and z, and its turn so far (3 x 3, row vectors as D3DX); the corners.
    struct BreakPiece {
        std::array<float, 3> centre{}, pos{}, vel{}, spin{};
        std::array<float, 9> turn{1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::array<std::array<float, 4>, 3> corner{};  // x, y (D3D pixel coordinates), u, v
    };
    std::vector<BreakPiece> breakPieces_;
    uint64_t breakId_ = UINT64_MAX;
    uint64_t breakElapsed_ = 0;
    bool havePrev_ = false;
    bool displayed_ = false;  // a composited frame exists (the next transition can start from it)

    // Letterbox placement of the virtual screen in the window.
    float viewX_ = 0, viewY_ = 0, viewScale_ = 1;
    int hovered_ = -1;

    std::unordered_map<std::string, Texture> textures_;
    std::unordered_map<std::string, bool> missingTextures_;
    std::shared_ptr<cs2::HgImage> parsed_;  // the multi-frame image last read (see texture)
    std::string parsedFile_;
    std::unordered_map<std::string, std::shared_ptr<const cs2::AnmScript>> animations_;
    const cs2::AnmScript* animation(const std::string& part);
    // A part's script for an image the scene player hasn't stepped yet (it makes the image's own
    // players): run on the screen's clock.
    std::unordered_map<std::string, std::unique_ptr<cs2::AnmPlayer>> looseAnm_;
    std::list<std::string> lru_;
    size_t textureBytes_ = 0;
};

}  // namespace nyan::render
