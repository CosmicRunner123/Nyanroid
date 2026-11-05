#pragma once

#include <array>
#include <memory>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nyan/diesel/camera.hpp"
#include "nyan/diesel/image.hpp"
#include "nyan/diesel/shake.hpp"
#include "nyan/diesel/spline.hpp"
#include "nyan/diesel/split.hpp"
#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

// A sprite's quad on the screen, in game pixels (y down): its picture's top-left, top-right,
// bottom-left and bottom-right corners, each x, y and the projective w (1 unless corner moves bend
// it; textures are interpolated by it).
struct SpriteQuad {
    std::array<std::array<float, 3>, 4> corner{};
};

// A picture of the screen kept in a save (a frozen screen capture, SQSprite.serialize), shown until
// the sprite captures again. Rows from the top, RGBA.
struct ScreenPicture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// A named surface, `suf://<name>`: Mware's named render targets (CGraphics vtable +0x70; phantom.exe
// +0xe0 through 0x140099610). A render target, a screen capture (createScreen) or a scene capture
// (createScene) whose object has a name makes its texture as "suf://" + the name (muramasa_en.exe
// 0x485790, createScreen 0x50d8b0, createScene 0x50daa0; totono_en.exe 0x46ee60, 0x4df830, 0x4dfa70;
// phantom.exe 0x1401369a0, 0x140136c80); a name in use gives the texture already made (Mware 0x10017240 looks the name up and
// adds a reference; the size asked for is then ignored), and the name lives while anything holds the
// texture. SQSprite.create("suf://<name>") shows it like a picture file.
struct NamedSurface {
    enum class Kind { Screen, Target, Scene };
    Kind kind = Kind::Screen;
    uint32_t id = 0;  // the screen picture's or the render target's id
    int width = 0, height = 0;
};

// An SQDisplacementMap's map (Diesel::CSpriteDisplacementMap, muramasa_en.exe 0x566f00 / 0x567110,
// over Mware's CDisplacementMap): its render target (by id) and the sampling scale its create was
// given. Held by the objects drawn through it (CSprite +0x650, CText +0x7bc), so a map made again
// leaves them the old one.
struct DisplacementMap {
    uint32_t target = 0;
    float scaleX = 0.0f, scaleY = 0.0f;
};

// A sprite's image and its animated properties: the exes' Diesel::CSprite (0xce0 bytes, made by
// SQSprite.create / createColor / createScreen / createMovie / createSubresource / createScene; base
// set up at muramasa_en.exe 0x450960). Field comments give CSprite offsets. What is not here yet:
// the page curl.
struct SpriteImage {
    // Scene: createScene's picture of the 3D scene (no scene renders yet: nothing is drawn).
    enum class Source { None, Texture, Color, Screen, Movie, Target, Scene };
    Source source = Source::None;
    std::string path;             // the texture or movie file
    // createSubresource: the texture is this part of the file (x, y, w, h: Mware's texture loader
    // given a rectangle, 0x10016770, its own texture under the name and the rectangle).
    std::optional<std::array<int, 4>> subresource;
    // A texture loadTextTexture made (the app's texture map under `path`), held by the sprite.
    std::shared_ptr<const Image> picture;
    int textureWidth = 0;         // the texture's size (0 until known)
    int textureHeight = 0;
    int srcX = 0, srcY = 0;       // +0xe0: the part of the texture shown
    int srcW = 0, srcH = 0;

    float centerX = 0.5f, centerY = 0.5f;  // +0x100: the anchor, as fractions of the size
    float pivotX = 0.0f, pivotY = 0.0f;    // +0x108: added to the anchor, pixels
    int32_t blendMode = 1;                 // +0x110 (1 normal: 0x450960)
    int32_t actionCompatible = 0;          // +0xbc: rotations by quaternions (1) or Z-X-Y
    int32_t clickable = 0;                 // +0x114
    int32_t clickableAlpha = 0;            // +0x118 (0 without a texture)
    std::vector<uint8_t> hitMask;          // +0xd4: the part's alpha, made by the update (0x459610)
    uint8_t hitThreshold = 0;              // +0x53c: alpha above it is hit
    int32_t changeCursor = 0;              // +0x138
    int32_t textureFiltering = 1;          // +0x638
    float uvOffsetX = 0.0f, uvOffsetY = 0.0f;  // +0x5f4
    float timeScale = 1.0f;                // +0x654
    std::string transitionFile;            // +0x4dc: transition()'s rule picture ("" none)
    float transitionRate = 0.0f;           // +0x4f4: transition()'s softness (the rule's ramp width)
    // setDither (0x459d30): handed straight to its Mware sprite (CSprite2D vtable +0x44: the pattern
    // texture at +0x94, the offset), not kept by the CSprite, so a new create() has none. `ditherFile`
    // the pattern if it loaded, "" the default one.
    bool dither = false;
    std::string ditherFile;
    float ditherX = 0.0f, ditherY = 0.0f;
    // setDisplacementMap (0x50ef00 -> 0x459cb0): the map (+0x650), handed to the texture sprite (+0xc8,
    // vtable +0x70 -> CSprite2D +0x98); a primitive (createColor) has none to take it.
    std::shared_ptr<const DisplacementMap> displacement;
    // A movie's texture (Source::Movie): which opened movie, the position (100 ns) and pass whose
    // picture it holds, and createMovie's alpha flag (the frame's lower half is the alpha).
    uint32_t targetId = 0;                 // Source::Target: the render target shown
    // Source::Screen: the renderer's picture of the screen for this sprite (createScreen's texture,
    // made by the device: +0xc0), and one read from a save.
    uint32_t screenId = 0;
    std::shared_ptr<const ScreenPicture> screenPicture;
    // The named surface its texture is (a capture's or render target's own, or one shown by
    // create("suf://...")): held while the image is.
    std::shared_ptr<NamedSurface> surface;
    uint32_t movieId = 0;
    int64_t movieTime = 0;
    uint32_t moviePass = 0;
    bool movieAlpha = false;
    bool volumeMoved = false;              // the volume tween stepped in the last update
    std::unique_ptr<Shake> shake;          // +0x658: shake / randomShake (kept when it ends)
    // setCamera (0x459af0): the camera it is drawn through (+0x63c, kept alive while held; null: the
    // task's), the source scale its quad is sized by (+0x640: 1, or what a depth makes it) and the
    // depth given (+0x64c).
    std::shared_ptr<const CameraView> camera;
    std::array<float, 3> sourceScale{1.0f, 1.0f, 1.0f};
    float cameraDepth = 0.0f;
    // A bezier path for the position (CBezier<Vector3D> at +0x544, 0x45ebe0 ff.): control points in
    // groups of four, a cubic segment each, run through by a 0-to-1 tween; while it runs, each frame
    // its point becomes the move tween's (set with no time: 0x453280).
    struct Bezier {
        std::vector<std::array<float, 3>> points;  // +0x548 (a deque)
        Tween t{1, 0.0f};                          // +0x55c
        std::array<float, 3> point{};              // +0x588
        bool step(const FrameClock& clock);        // vtable +0: true while it ran
        void evaluate();                           // 0x4611e0
    } bezier;
    // setSpline / beginSpline's path for the position (+0x594): while it runs, each frame its point
    // becomes the move tween's, after the bezier's (0x453280).
    TimedSpline spline;
    // splitFace and its actions (+0xbe0 ff.): the pieces the vertex build draws instead of the quad.
    SplitFace split;
    // The page turn (CSprite +0xa94 ff.; set at the constructor 0x450960): its state, which saves
    // read. No scene turns a page, so the curl is not drawn.
    Tween pageDirection{1, 0.0f};              // +0xa94 (value +0xaa8): setPageDirection
    Tween pageMove{1, 1.0f};                   // +0xac0 (value +0xad4): beginPageAction's
    Tween pageRadius{1, 0.3f};                 // +0xaec (value +0xb00): setPageRadius
    Tween pageBackColor{3, 1.0f, 1.0f, 1.0f};  // +0xb18 (value +0xb3c): setPageBackColor
    Tween pageBackAlpha{1, 1.0f};              // +0xb64 (value +0xb78): setPageBackAlpha
    std::string pageActionFile;                // setPageAction's texture (0x45b310)
    int32_t pageActionMode = 0;
    // setBlur's trail (+0x65c ff.; drawn by the vertex build 0x456400 from 0x4a4320's choice): the
    // sprite as drawn in earlier frames, `levels` of them `interval` ms apart, under or over it.
    struct BlurFrame {
        SpriteQuad quad;
        std::array<std::array<float, 3>, 4> rgb{};
    };
    struct Blur {
        int32_t levels = 0;         // +0x660 (0: off)
        uint32_t interval = 16;     // +0x664: ms, at least 16
        float power = 0.0f;         // +0x668
        int32_t order = 0;          // +0x66c: 0 the sprite first, older copies over it; else under
        int32_t fade = 0;           // +0x670: older copies fainter
        int32_t mode = 0;           // +0x674 (setBlurMode, +0x698)
        std::deque<BlurFrame> history;
    } blur;

    // Where the last frame drew it (the vertex build keeps the matrix the hit test inverts, +0xca0);
    // none if it was not drawn.
    std::optional<SpriteQuad> drawnQuad;

    SpriteImage* parent = nullptr;  // link(): another sprite's image (null: the task's root)
    std::vector<SpriteImage*> children;

    Tween fade{1, 1.0f};                     // +0x13c: alpha
    Tween move{3, 0.0f, 0.0f, 0.0f};         // +0x168: position
    std::array<Tween, 4> vertexMove{Tween{2, 0.0f, 0.0f}, Tween{2, 0.0f, 0.0f}, Tween{2, 0.0f, 0.0f},
                                    Tween{2, 0.0f, 0.0f}};  // +0x1b4: corner offsets
    Tween scale{2, 1.0f, 1.0f};              // +0x2a4
    Tween rotate{3, 0.0f, 0.0f, 0.0f};       // +0x2e0: degrees about x, y, z
    std::array<Tween, 4> color{Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f}, Tween{3, 1.0f, 1.0f, 1.0f},
                               Tween{3, 1.0f, 1.0f, 1.0f}};  // +0x32c: corner colours
    Tween colorBalance{3, 1.0f, 1.0f, 1.0f};  // +0x45c
    Tween transition{1, 0.0f};                // +0x4a8: the rule's threshold
    Tween volume{1, 1.0f};                    // +0x4f8 (movies)
    Tween scroll{2, 0.0f, 0.0f};              // +0x5fc: texture scroll speed
    Tween colorHue{1, 0.0f};                  // +0x69c
    Tween colorSaturation{1, 0.0f};           // +0x6c8
    Tween colorBrightness{1, 0.0f};           // +0x6f4
    Tween colorGlow{1, 0.0f};                 // +0x720
    Tween negaposi{1, 0.0f};                  // +0x74c
    Tween monochrome{3, 0.0f, 0.0f, 0.0f};    // +0x778: power, Cb, Cr
    Tween sepia{1, 0.0f};                     // +0x7c4
    Tween shading{1, 0.0f};                   // +0x7f0
    // setShadingParameter(level, density) (SQSprite 0x50e8f0 -> CSprite 0x459940): kept by the
    // material (+0xc8, vtable +0x44; read back by +0x4c / +0x50) for the shading filter (section 5o);
    // the defaults are a guess (the material's constructor not read).
    int32_t shadingLevel = 0;
    float shadingDensity = 0.0f;
    Tween colorLevel{3, 0.0f, 1.0f, 1.0f};    // +0xb94: min, max, power
    // The texture filters the material runs on the sprite's texture (Mware's CTextureFilter).
    // The setters hand the material the values at once; the
    // update (0x453280) steps the tweens, advances the ripple's and raster's phases and hands on
    // what changed. All 0 at first (0x450960).
    Tween radialBlurPower{1, 0.0f};                    // +0x840
    std::array<float, 2> radialBlurCenter{};           // +0x86c: texture pixels
    Tween directionalBlurPower{1, 0.0f};               // +0x878
    std::array<float, 2> directionalBlurDirection{};   // +0x8a4
    Tween ripplePower{1, 0.0f};                        // +0x8b0
    Tween rippleFrequency{1, 0.0f};                    // +0x8dc
    Tween rippleSpeed{1, 0.0f};                        // +0x908
    float ripplePhase = 0.0f;                          // +0x934: cycles
    std::array<float, 2> rippleCenter{};               // +0x938: texture pixels
    Tween rasterRotate{1, 0.0f};                       // +0x9cc: degrees
    Tween rasterPower{1, 0.0f};                        // +0x944
    Tween rasterFrequency{1, 0.0f};                    // +0x970
    Tween rasterSpeed{1, 0.0f};                        // +0x99c
    float rasterPhase = 0.0f;                          // +0x9c8: cycles
    Tween lensCenter{2, 0.0f, 0.0f};                   // +0x9fc: texture pixels
    Tween lensPower{1, 0.0f};                          // +0xa38
    Tween lensRadius{1, 0.0f};                         // +0xa64: texture pixels

    SpriteImage() = default;
    SpriteImage(const SpriteImage&) = delete;
    SpriteImage& operator=(const SpriteImage&) = delete;
    ~SpriteImage();

    // setTimeScale (SQSprite 0x50e490 -> 0x459450): every tween's time scale.
    void setTimeScale(float s);
    // The frame's update (task object slot 14, 0x453280): the tweens step.
    void update(const FrameClock& clock);
    // link(): a new parent, this image leaving the old one's children (0x4aa810).
    void setParent(SpriteImage* p);
    // The tweens, for setTimeScale and update.
    template <typename F>
    void forEachTween(F&& f) {
        f(fade), f(move), f(scale), f(rotate), f(colorBalance), f(transition), f(volume), f(scroll);
        f(colorHue), f(colorSaturation), f(colorBrightness), f(colorGlow), f(negaposi), f(monochrome);
        f(sepia), f(shading), f(colorLevel), f(bezier.t), f(spline.t), f(split.t);
        f(pageDirection), f(pageMove), f(pageRadius), f(pageBackColor), f(pageBackAlpha);
        f(radialBlurPower), f(directionalBlurPower), f(ripplePower), f(rippleFrequency), f(rippleSpeed);
        f(rasterRotate), f(rasterPower), f(rasterFrequency), f(rasterSpeed), f(lensCenter), f(lensPower);
        f(lensRadius);
        for (auto& t : vertexMove) f(t);
        for (auto& t : color) f(t);
    }
};

// setBlur's trail as CBlur keeps it (muramasa_en.exe 0x4a4160 / 0x4a4320, for sprites and for
// each of a text's two glyph batches): the history holds at most (levels + 1 for mode 0) x interval
// x 0.06 frames (the oldest leaves first, one always stays); the frames drawn are those levels x
// interval ms back and nearer (mode 1: the oldest kept when the history is short, frames like this
// one dropped; mode 0: those it has, the oldest first if short, frames like their next dropped),
// this frame (the last of `size`) first and the older in age order after it (order 0), or the
// oldest first and this frame last. `same(i, j)`: whether two frames hold the same vertices.
size_t blurHistoryLimit(const SpriteImage::Blur& b);
std::vector<size_t> blurPicks(const SpriteImage::Blur& b, size_t size, const std::function<bool(size_t, size_t)>& same);
// A copy's weight, the factor on its alpha before the fade: the power, or with `fade` the power x
// (1 - its place among the copies / their number); `i` its index among `count` picks.
float blurWeight(const SpriteImage::Blur& b, size_t i, size_t count);

}  // namespace nyan::diesel
