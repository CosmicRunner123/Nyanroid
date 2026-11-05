#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nyan/diesel/font.hpp"
#include "nyan/diesel/particle.hpp"
#include "nyan/diesel/sprite.hpp"

namespace nyan::diesel {

struct ShadowBatch;

// The filters a sprite's material runs on its texture before it is drawn (Mware's CTextureFilter,
// 0x1002c440: the material's parameters, +0x15c, as its setters keep them; section 5o). The renderer
// runs the active ones over the whole texture in the material's order (ripple, raster, radial blur,
// directional blur, shading, negaposi, monochrome, sepia, colour level, HSV, lens) and draws the
// sprite's part of the result. Centres and the lens radius are texture pixels here (the material
// divides them by the texture's size, or its height).
struct TextureFilter {
    std::array<float, 2> radialCenter{};
    float radialPower = 0.0f;           // the sprite's power x 10 (0x10023a20)
    std::array<float, 2> directional{}; // normalised (0x10023a60)
    float directionalPower = 0.0f;      // x 3 (0x10023ae0)
    std::array<float, 2> rippleCenter{};
    float ripplePhase = 0.0f, rippleFrequency = 0.0f, ripplePower = 0.0f;
    float rasterRotate = 0.0f;          // degrees
    float rasterPhase = 0.0f, rasterFrequency = 0.0f, rasterPower = 0.0f;
    std::array<float, 2> lensCenter{};
    float lensRadius = 0.0f, lensDistance = 0.0f;
    // The ripple's and raster's sampler (CPostEffectDX11 +0x594 / +0x598 by the parameters' +0x18,
    // set by 0x10023000): wrapping when the sprite's UV offset is non-zero (a scroll adds into it),
    // else transparent outside the texture (a border of 0). The blurs and the lens clamp.
    bool wrap = false;
    // Shading (a blur over a pyramid of four smaller copies: 0x1002b110): the power (the update
    // clamps it to 0 .. 1), setShadingParameter's level and density (x 256: 0x10023770).
    float shadingPower = 0.0f;
    int32_t shadingLevel = 0;
    float shadingDensity = 0.0f;
    // The colour filters (+0x38, +0x3c, +0x48, +0x8c, +0x98): negaposi, monochrome (power, Cb, Cr),
    // sepia, colour level (min, max, power) and the HSV shift (hue, saturation, value, glow).
    float negaposi = 0.0f;
    std::array<float, 3> monochrome{};
    float sepia = 0.0f;
    std::array<float, 3> colorLevel{0.0f, 1.0f, 1.0f};
    std::array<float, 4> hsv{};

    // When each runs (0x1002c050, 0x1002c140, 0x1002bc90, 0x1002bf90, 0x1002c220).
    bool ripple() const { return ripplePower > 0.0f && rippleFrequency > 0.0f; }
    bool raster() const { return rasterPower > 0.0f && rasterFrequency > 0.0f; }
    bool radial() const { return radialPower > 0.0f; }
    bool directionalBlur() const { return directionalPower > 0.0f; }
    bool lens() const { return lensRadius > 0.0f && lensDistance > 0.0f; }
    // 0x1002b110, 0x1002bd50, 0x1002be10, 0x1002bed0, 0x1002ba80, 0x1002bb90.
    bool shading() const { return shadingPower > 0.0f; }
    bool negaposiOn() const { return negaposi > 0.0f; }
    bool monochromeOn() const { return monochrome[0] > 0.0f; }
    bool sepiaOn() const { return sepia > 0.0f; }
    bool colorLevelOn() const { return colorLevel[0] != 0.0f || colorLevel[1] != 1.0f || colorLevel[2] != 1.0f; }
    bool hsvOn() const { return hsv[0] != 0.0f || hsv[1] != 0.0f || hsv[2] != 0.0f || hsv[3] != 0.0f; }
    bool any() const {
        return ripple() || raster() || radial() || directionalBlur() || lens() || shading() || negaposiOn() ||
               monochromeOn() || sepiaOn() || colorLevelOn() || hsvOn();
    }
};

// One quad of the frame as the engine drew it, in draw order: the host lays the draw list out after
// the scripts' frame and before the task objects' updates (where muramasa_en.exe 0x42e7e0 draws), so
// a renderer shows what the PC would have shown.
struct DrawItem {
    enum class Kind { Texture, Color, Screen, Movie, Glyph, Shadow, Target, Particles };
    Kind kind = Kind::Texture;
    std::string texture;          // Texture / Movie: the file (Color: a white primitive)
    std::shared_ptr<const Image> picture;  // Texture: a texture made in memory (loadTextTexture's) instead
    // Texture: the texture is only this part of the file (x, y, w, h; createSubresource), which
    // srcX .. srcH then lie in.
    bool subresource = false;
    std::array<int, 4> subRect{};
    std::shared_ptr<const GlyphImage> glyph;  // Glyph: its coverage, drawn in the corner colours
    std::shared_ptr<const ShadowBatch> shadow;  // Shadow: a text's shadow
    // Particles: a particle system's frame (SQParticle: clip-space quads, the texture, the blend mode
    // and the colour mode; `texture` and `blendMode` repeat its texture and sprite.blend_mode).
    std::shared_ptr<const ParticleBatch> particles;
    int textureWidth = 0, textureHeight = 0;
    int srcX = 0, srcY = 0;       // the part of the texture (Screen: of the screen)
    int srcW = 0, srcH = 0;
    SpriteQuad quad;              // on the screen, game pixels
    // A split sprite's piece (section 5r): its corners' places in the part, 0..1 from its top-left,
    // instead of the part's corners.
    bool partUv = false;
    std::array<std::array<float, 2>, 4> uv{};
    // Each corner's colour (the corner colours' RGB, the fade's alpha), 0..1 (the vertex build
    // 0x456400 and 0x453be0 clamp them; bytes there).
    std::array<std::array<float, 4>, 4> color{};
    int32_t blendMode = 0;
    bool filtering = true;
    float uvOffsetX = 0.0f, uvOffsetY = 0.0f;  // added to every texture coordinate (vsSprite's g_Offset.zw)
    TextureFilter filter;         // Texture / Screen / Movie / Target: the material's filters
    // Movie: which opened movie (its id: one per createMovie), the position whose picture the
    // texture holds (100 ns), the pass of the movie that position is in, and whether the frame's
    // lower half is the upper half's alpha (createMovie's flag: then the texture is half as tall).
    // A transition (the sprite has a rule picture): the picture's red, sampled where the pixel is on
    // the screen, through the rule ramp for the threshold `transitionNow` and softness
    // `transitionRate`, scales the alpha (Mware's psSprite_*_Transition / psUpdateTransitionRule).
    std::string transitionRule;
    float transitionNow = 1.0f;
    float transitionRate = 0.0f;
    // Dither (setDither: Mware's g_DitherEnable / g_DitherPatternScale, all the sprite, primitive
    // and particle pixel shaders): the pixel is dropped where its alpha is below the pattern's red
    // at its place on the target (the pattern repeating from the target's top-left, moved by the
    // offset, point sampled), else drawn opaque; the multiply and screen modes mix the colour by the
    // alpha from before. The pattern is `ditherTexture`, or with that empty the default 4 x 4 one.
    bool dither = false;
    std::string ditherTexture;
    float ditherX = 0.0f, ditherY = 0.0f;
    // A displacement map (setDisplacementMap: an SQDisplacementMap's render target, by id; texture
    // and glyph items only): the texture coordinate moves by the map's (red, blue) x 2 - 1 where the
    // pixel is on its target, times the map's scale (texture fractions: psSprite_*_DisplacementMap*,
    // g_DisplacementMapSamplingScale), and the texture is sampled linearly.
    uint32_t displacementMap = 0;
    float displacementScaleX = 0.0f, displacementScaleY = 0.0f;
    // Where it is drawn: a render target or a mask (RenderTarget::id), 0 the screen; with `mask`
    // set, its pixels are multiplied by that mask's texture where they are on their target (alpha
    // by the mask's alpha, or 1 − it with `maskInverse`; colour by its colour: psSprite_*_Mask).
    uint32_t target = 0;
    uint32_t mask = 0;
    bool maskInverse = false;
    // Target: a render target's texture (its id).
    uint32_t renderTarget = 0;
    // Screen (createScreen): the sprite's picture of the screen, kept by the renderer under this id.
    // While `capture` is on (SQSprite +0xcc) it is taken again when the screen's pass reaches it:
    // the screen as drawn so far that frame (the task's draw calls SQSprite vtable +0x78 / +0x7c,
    // 0x50d490 / 0x50d4b0, for the screen's items only: 0x47ed90); a capture drawn into a render
    // target is not. Off, it stays the picture it last took (the exe shows a copy of it: the update
    // 0x50d250, texture vtable +0x3c). `screenPicture`: one read from a save, shown until a capture
    // replaces it.
    uint32_t screen = 0;
    bool capture = true;
    // The app's screen fade (CApp +0xf4): the last frame shown is copied in before this frame is
    // drawn (0x42e7e0, the graphics manager's copy at the start of the frame).
    bool takeLastFrame = false;
    std::shared_ptr<const ScreenPicture> screenPicture;
    uint32_t movie = 0;
    int64_t movieTime = 0;
    uint32_t moviePass = 0;
    bool movieAlpha = false;
};

// A render target or a mask of the frame: an offscreen texture the items with its id as `target`
// are drawn into, before the items that use it (Kind::Target, `mask`) are drawn (the task's draw,
// 0x4861b0, orders the targets so). A render target (SQSpriteRenderTarget) is reset to its clear
// colour each frame unless it keeps its picture (capture off, or copying another: CSprite +0xcf8);
// a mask (SQSpriteMask) is the screen's size, its sprite drawn into it, cleared each frame it draws.
struct RenderTarget {
    uint32_t id = 0;
    int width = 0, height = 0;
    bool mask = false;
    bool reset = true;        // cleared (to clearColor) before this frame's items
    bool draw = true;         // this frame's items are drawn into it (else its picture stays)
    bool clear = false;       // clear() asked: cleared once whatever the rest
    std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t copyFrom = 0;    // setCopyTarget: another target's picture copied in first
    // setDelayRendering (CSprite +0xcf0): drawn not before the screen but when the screen's pass
    // reaches the item showing it (muramasa_en.exe 0x47ed90 holds it back; SQSpriteRenderTarget
    // vtable +0x78 / +0x7c, 0x51bb90 / 0x51bbb0, draw it there).
    bool delayed = false;
};

// A text's shadow (CText's passes 0x4d92a0 / 0x4d9300 / 0x4d94a0): its glyphs drawn white at each of
// their offsets (screen pixels, y down) into a target cleared to white with alpha 0, with the
// render target's blend state (Mware: the colour over, the alpha added and clamped), then the target
// laid over the frame in the shadow's colour at the shadow's alpha byte.
struct ShadowBatch {
    std::vector<DrawItem> glyphs;                             // with their typing alpha
    std::vector<std::vector<std::array<float, 2>>> offsets;   // per glyph
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
};

}  // namespace nyan::diesel
