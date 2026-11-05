#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/diesel/draw_item.hpp"
#include "nyan/diesel/movie_frames.hpp"
#include "nyan/render/gl.hpp"

namespace nyan::render::dieselr {

// Draws a Diesel frame (the host's DrawItems, in order) on OpenGL ES 3: into a back buffer of the
// game screen's size, then the back buffer onto the window, letterboxed. Needs a current GL context.
class Renderer {
public:
    using ReadFile = std::function<std::optional<Bytes>(const std::string&)>;
    using OpenFile = diesel::MovieFrames::OpenFile;
    using Log = std::function<void(const std::string&)>;

    // `open` streams movie files (null: movies are not drawn).
    Renderer(int screenW, int screenH, ReadFile read, Log log, OpenFile open = nullptr);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void draw(const std::vector<diesel::DrawItem>& items);
    // With the frame's render targets and masks (ScriptHost::renderTargets()).
    void draw(const std::vector<diesel::DrawItem>& items, const std::vector<diesel::RenderTarget>& targets);
    // The back buffer scaled to fit `width` x `height` of framebuffer `target`, black around it.
    void present(int width, int height, GLuint target);
    // Where the back buffer lands in a `width` x `height` framebuffer: x, y, w, h (y from the top).
    std::array<float, 4> viewport(int width, int height) const;
    // A screen capture's picture (DrawItem::screen; one never drawn, or 0: the last frame) scaled
    // to width x height, RGBA rows from the top (SQSprite.saveImage, serialize).
    bool readScreen(uint32_t screen, int width, int height, std::vector<uint8_t>& rgba);
    // A render target's picture as last drawn (RenderTarget::id), scaled the same way.
    bool readTarget(uint32_t target, int width, int height, std::vector<uint8_t>& rgba);
    // Textures kept while unused for this many draws are released.
    static constexpr int kTextureIdleDraws = 600;

private:
    struct Texture {
        GLuint tex = 0;
        int w = 0, h = 0;
        uint64_t lastUsed = 0;
        bool failed = false;
    };
    // A picture file's texture (`rect`: only that part of it, createSubresource's), loaded once.
    Texture* texture(const std::string& path, const std::array<int, 4>* rect = nullptr);
    // A texture made in memory (loadTextTexture's), uploaded once and kept while drawn.
    Texture* pictureTexture(const std::shared_ptr<const diesel::Image>& picture);
    // A movie's texture, its picture uploaded when it changed (keyed by the movie's id).
    struct MovieTexture {
        GLuint tex = 0;
        int w = 0, h = 0;
        uint64_t serial = 0, lastUsed = 0;
    };
    MovieTexture* movieTexture(const diesel::DrawItem& d);
    std::unique_ptr<diesel::MovieFrames> movies_;
    std::map<uint32_t, MovieTexture> movieTextures_;
    // Glyphs share an atlas (R8), packed in shelves; when it fills it starts again.
    struct GlyphSlot {
        std::shared_ptr<const diesel::GlyphImage> glyph;  // kept alive while it has a slot
        int x = 0, y = 0;
    };
    const GlyphSlot* glyphSlot(const std::shared_ptr<const diesel::GlyphImage>& g);
    static constexpr int kAtlasSize = 2048;
    GLuint atlas_ = 0;
    int atlasX_ = 1, atlasY_ = 1, atlasRow_ = 0;
    std::unordered_map<const diesel::GlyphImage*, GlyphSlot> glyphSlots_;
    void setBlend(int mode);
    // Modes 6 .. 20 read the picture so far.
    static bool readsTarget(int mode) { return mode >= 6 && mode <= 20; }
    void copyDestination(const diesel::DrawItem& d);
    GLuint dstTex_ = 0;
    int dstW_ = 0, dstH_ = 0;
    // One glyph quad (glyph shader), moved by (dx, dy) pixels; the atlas must be bound.
    bool drawGlyph(const diesel::DrawItem& d, float dx, float dy, const float (*color)[4]);
    bool bindTransition(const diesel::DrawItem& d);
    // The item's dither (its pattern, or the default one, on unit kDitherUnit): false if it has none.
    struct DitherUniforms {
        GLint on = -1, size = -1, offset = -1;
    };
    static constexpr int kDitherUnit = 6;
    bool bindDither(const diesel::DrawItem& d, const DitherUniforms& u);
    // The item's displacement map (its target's texture on unit kDisplacementUnit): false if none.
    static constexpr int kDisplacementUnit = 5;
    bool bindDisplacement(const diesel::DrawItem& d);
    // The item's mask (its texture on unit 2, the inverse flag set): false if it has none drawn.
    bool bindMask(const diesel::DrawItem& d);
    void drawShadow(const diesel::DrawItem& d);
    GLuint shadowTex_ = 0, shadowFbo_ = 0;
    void releaseIdle();
    // The material's texture filters (DrawItem::filter) on a texture `w` x `h`: the filtered picture
    // (one of two buffers kept at the last size), or `src` when none runs. Leaves the frame's
    // program, framebuffer, viewport and blending as they were.
    GLuint applyFilter(GLuint src, int w, int h, const diesel::TextureFilter& f);
    GLuint progRipple_ = 0, progRaster_ = 0, progLens_ = 0, progBlur_ = 0;
    GLuint progColor_ = 0, progGauss_ = 0, progDown_ = 0, progCommit_ = 0;
    // Shading's buffers (the texture's size: the commit's target; and two of each smaller copy).
    struct ShadeBuffer {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0;
    };
    ShadeBuffer shadeFull_, shadeA_[4], shadeB_[4];
    int shadeW_ = 0, shadeH_ = 0;
    // Shading on `src` (`next`: the filter buffer the next pass draws into, moved on as it uses them).
    GLuint shade(GLuint src, int w, int h, const diesel::TextureFilter& f, int& next);
    void releaseShading();
    GLuint filterVao_ = 0;
    GLuint filterTex_[2] = {0, 0}, filterFbo_[2] = {0, 0};
    int filterW_ = 0, filterH_ = 0;
    // Render targets and masks: their textures, kept by id while they are named.
    struct TargetTexture {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0;
        uint64_t lastUsed = 0;
    };
    std::map<uint32_t, TargetTexture> targets_;
    // Reads a framebuffer of sw x sh (rows from the bottom) and stretches it to width x height.
    bool readFramebuffer(GLuint fbo, int sw, int sh, int width, int height, std::vector<uint8_t>& rgba);
    // Screen captures (createScreen): each sprite's picture of the screen, taken when the screen's
    // pass reaches it while it captures (or put there from a save).
    struct ScreenTexture {
        GLuint tex = 0;
        uint64_t lastUsed = 0;
        bool taken = false;  // holds a capture or a loaded picture
        const diesel::ScreenPicture* picture = nullptr;  // the loaded one it shows
    };
    std::map<uint32_t, ScreenTexture> screens_;
    void captureScreens(const std::vector<diesel::DrawItem>& items);
    ScreenTexture& screenTexture(const diesel::DrawItem& d);
    void captureNow(const diesel::DrawItem& d, GLuint current);
    TargetTexture* targetTexture(const diesel::RenderTarget& t);
    void bindTarget(GLuint fbo, int w, int h);
    void drawList(const std::vector<const diesel::DrawItem*>& list);
    void drawParticles(const diesel::DrawItem& d);
    GLuint progParticle_ = 0, particleVao_ = 0, particleVbo_ = 0;
    GLint uParticleCalc_ = -1, uParticleGroup_ = -1;
    GLuint currentFbo_ = 0;
    int currentW_ = 0, currentH_ = 0;

    int screenW_, screenH_;
    ReadFile read_;
    Log log_;
    GLuint prog_ = 0, progPresent_ = 0;
    GLint uSize_ = -1, uMode_ = -1, uTextured_ = -1, uGlyph_ = -1;
    GLint uTransition_ = -1, uTransNow_ = -1, uTransRate_ = -1, uMasked_ = -1, uMaskInverse_ = -1;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint samplerNearest_ = 0, samplerLinear_ = 0, samplerRepeat_ = 0, samplerRepeatNearest_ = 0, samplerDither_ = 0;
    GLuint ditherDefault_ = 0;
    DitherUniforms spriteDither_, particleDither_;
    GLint uDisplaced_ = -1, uDisplaceScale_ = -1;
    GLuint backTex_ = 0, backFbo_ = 0;
    std::map<std::string, Texture> textures_;
    struct PictureTexture {
        Texture t;
        std::shared_ptr<const diesel::Image> picture;  // held so the key stays its own
    };
    std::map<const diesel::Image*, PictureTexture> pictures_;
    std::set<int> unsupportedModes_;
    uint64_t draws_ = 0;
};

}  // namespace nyan::render::dieselr
