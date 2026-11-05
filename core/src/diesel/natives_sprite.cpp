// SQSprite: the scripts' picture object (Diesel::Squirrel::SQSprite, 0xd8 bytes; constructor
// muramasa_en.exe 0x50cfe0; natives registered at 0x50bf20). It is a task object in the draw list
// and holds a Diesel::CSprite (SpriteImage) once one of the create* natives has made it. Addresses
// are Muramasa's; signatures as its SqPlus dispatchers check them.

#include <algorithm>
#include <set>
#include <cmath>

#include "natives.hpp"
#include "nyan/diesel/asf_info.hpp"
#include "nyan/diesel/image.hpp"
#include "nyan/diesel/movie_clock.hpp"
#include "nyan/diesel/sprite_geometry.hpp"
#include "stb_image_write.h"

namespace nyan::diesel {

namespace {

// A movie on a sprite (createMovie, 0x4853d0 / CSprite 0x452560): Mware's CMovieMediaFoundation
// (its clock, and the XAudio2 voice its sound plays on) rendering into the sprite's texture, and the
// sound manager record of type MOVIE (CSprite +0x524) through which its volume follows the movie
// type's volume and is ducked.
struct MovieObject final : ScriptHost::Impl::SoundRecord, ScriptHost::Impl::MovieTask {
    ScriptHost::Impl* host;
    SpriteImage* img;
    uint32_t id;
    std::string path;
    bool alpha;
    AsfInfo info;
    MovieClock clock;
    uint32_t lastMilli = 0;
    // The voice (Mware +0x210): started by play, paused by pause, stopped by stop and at the end.
    uint32_t voiceKey = 0;
    bool voiceOn = false, voicePaused = false;
    float amplitude = 0.0f;  // Mware +0x74: what the voice was last set to

    MovieObject(ScriptHost::Impl* h, SpriteImage* image, std::string file, bool withAlpha, const AsfInfo& asf)
        : host(h), img(image), id(h->nextMovieId++), path(std::move(file)), alpha(withAlpha), info(asf) {
        type = 4;  // SOUND_TYPE_MOVIE (0x44fe50 with 4)
        clock.duration = info.duration();
        lastMilli = h->clock.milliTime;
        img->movieId = id;
        img->movieAlpha = alpha;
        h->soundRecords.push_back(this);
        h->movies.push_back(this);
    }
    ~MovieObject() override {
        stopVoice();
        std::erase(host->soundRecords, static_cast<ScriptHost::Impl::SoundRecord*>(this));
        std::erase(host->movies, static_cast<ScriptHost::Impl::MovieTask*>(this));
    }

    void voiceEvent(SoundEvent::Kind kind) {
        SoundEvent e;
        e.kind = kind;
        e.voice = voiceKey;
        host->soundEvents.push_back(std::move(e));
    }
    // play: the voice goes on where it was (after a pause) or starts at the position, looping with
    // the movie's passes (the reader repeats as the picture does: Mware +0x64).
    void startVoice() {
        if (!info.hasAudio) return;
        if (voiceOn) {
            if (voicePaused) voiceEvent(SoundEvent::Kind::Resume);
            voicePaused = false;
            return;
        }
        SoundEvent e;
        e.kind = SoundEvent::Kind::Play;
        e.voice = voiceKey = host->nextSoundKey++;
        e.file = path;
        e.type = type;
        e.plays = clock.loops < 0 ? 0 : std::max(clock.loops, 1);
        e.startMs = static_cast<uint32_t>(std::max(clock.positionMs(), 0));
        e.movie = true;
        host->soundEvents.push_back(std::move(e));
        voiceOn = true;
        voicePaused = false;
    }
    void stopVoice() {
        if (voiceOn) voiceEvent(SoundEvent::Kind::Stop);
        voiceOn = voicePaused = false;
    }
    // The record as CSprite's play / stop / pause / resume leave it (0x459170 ff.).
    void mark(bool hear) {
        audible = hear;
        dirty = true;
        first = true;
    }

    void play(int32_t loops) {
        mark(true);
        clock.play(loops == 1 ? -1 : loops);  // CSprite::play: 1 is for ever
        lastMilli = host->clock.milliTime;    // Mware +0x44: the clock starts now
        startVoice();
    }
    void stop() {
        mark(false);
        clock.stop();
        stopVoice();
    }
    void pause() {
        mark(false);
        if (clock.pause() && voiceOn && !voicePaused) {
            voiceEvent(SoundEvent::Kind::Pause);
            voicePaused = true;
        }
    }
    void resume() {
        mark(true);
        if (clock.resume() && voiceOn && voicePaused) {
            voiceEvent(SoundEvent::Kind::Resume);
            voicePaused = false;
        }
    }
    void seek(int32_t ms) {
        bool was = clock.playing;
        clock.seek(ms);
        stopVoice();
        if (was) startVoice();
        img->movieTime = clock.position;
    }
    // CSprite vtable +0x98 (0x453af0): heard when the volume is above 0, the movie plays unpaused and
    // its groups and type are not silent; the voice is set to control × volume × groups × type
    // through Mware's curve (0x1006d7e0: v^1.661 above 0.001, unclamped).
    void applyVolume() {
        float own = img->volume.now[0] > 0.0f ? img->volume.now[0] : 0.0f;
        float g = groupProduct * host->typeVolume(type);
        bool hear = img->volume.now[0] > 0.0f && clock.playing && !clock.paused && g > 0.0f;
        if (audible != hear) dirty = true;
        audible = hear;
        float v = control.now * own * g;
        amplitude = v > 0.001f ? std::pow(10.0f, std::log10(v) * 33.22f * 0.05f) : 0.0f;
        dirty = false;
    }
    bool typeMoving() const {
        auto it = host->typeVolumes.find(type);
        return it != host->typeVolumes.end() && (it->second.active || it->second.wasActive);
    }
    // Mware's thread: the clock by the time since the last step; the texture holds the picture of
    // the position while the movie plays (stopped, it keeps the last one: the end's at the end).
    void stepMovie(ScriptHost::Impl& h) override {
        uint32_t now = h.clock.milliTime;
        int64_t elapsed = static_cast<int64_t>(now - lastMilli) * 10000;
        lastMilli = now;
        if (clock.step(elapsed)) {
            stopVoice();
            img->movieTime = clock.duration;
        } else if (clock.playing) {
            img->movieTime = clock.position;
            img->moviePass = clock.pass;
        }
    }
    void voices(std::vector<SoundVoice>& out) const override {
        if (!voiceOn) return;
        SoundVoice s;
        s.voice = voiceKey;
        s.type = type;
        s.gain = amplitude;
        s.pan = 0.0f;
        s.frequency = clock.timeScale;  // SetFrequencyRatio with the time scale (0x1006d8d0)
        s.paused = voicePaused;
        out.push_back(s);
    }
};

struct SpriteObject final : NativeObject, FrameTask, Drawable {
    // SQSprite; SQSpriteMask (CSpriteMask: the sprite drawn into a mask texture of its target's size,
    // which setMask(name) hands to other objects); SQSpriteRenderTarget (CSpriteRenderTarget: a
    // texture of its own size that setSurface(name) has other objects draw into, shown as a sprite);
    // SQDisplacementMap (CSpriteDisplacementMap, a CSpriteRenderTarget: a target other objects draw
    // into, cleared to grey each frame and not shown, whose texture moves where objects given it
    // with setDisplacementMap sample their textures).
    enum class Kind { Sprite, Mask, Target, Displacement };
    ScriptHost::Impl* host;
    Kind kind = Kind::Sprite;
    std::unique_ptr<SpriteImage> img;  // +0xc4
    std::unique_ptr<MovieObject> movie;  // CSprite +0xd0 (released before the image)
    float timeScale = 1.0f;            // +0xc0
    int32_t lock = 0, disused = 0, hidable = 0;  // Totono's (vtable +0xb0 / +0xb8 / +0x8c)
    uint32_t ownTarget = 0;            // the mask's or render target's texture
    // Render targets (SQSpriteRenderTarget +0xcc .. +0xf4; CSpriteRenderTarget +0xce8 .. +0xcf8).
    int32_t capture = 0;               // +0xcc: set by create / createScreen; off, the picture stays
    int32_t screenCapture = 0;         // +0xd0: made by createScreen (the copy needs it, 0x50d4b0)
    uint32_t copyFrom = 0;             // +0xf0: setCopyTarget's texture
    std::string captureTarget;         // +0xd8: setCaptureTarget's name (kept; its use not found)
    int32_t delayRendering = 0;        // CSprite +0xcf0
    bool clearRequested = false;       // +0xf4: clear()
    std::array<float, 4> clearColor{}; // CSpriteRenderTarget +0xce8 (Fitted: transparent black)
    // Masks (ISpriteMask +0xd4 / +0xd8).
    int32_t diffuseOnly = 0, inverseAlpha = 0;
    // Displacement maps (SQDisplacementMap +0xf8): the map create() made.
    std::shared_ptr<DisplacementMap> map;

    SpriteObject(ScriptHost::Impl* h, std::string name, Kind k = Kind::Sprite) : host(h), kind(k) {
        if (k != Kind::Sprite) ownTarget = h->nextTargetId++;
        taskName = std::move(name);
        created = h->clock.milliTime;
        h->addTask(this);
        h->addDrawable(this, 0, created);
    }
    ~SpriteObject() override {
        host->removeTask(this);
        host->removeDrawable(this);
    }
    // The update (0x453280): a clickable sprite with the alpha mode makes its mask once its texture
    // is in (0x459610: the part's alpha, 0 outside the texture; a texture without alpha turns the
    // mode off), then the tweens step.
    void update(ScriptHost::Impl& h) override {
        if (!img) return;
        SpriteImage& im = *img;
        if (im.clickable && im.clickableAlpha && im.hitMask.empty() && im.source == SpriteImage::Source::Texture) {
            ScriptHost::Impl::AlphaPlane own;
            const ScriptHost::Impl::AlphaPlane* plane = nullptr;
            if (im.picture) {
                own.width = im.picture->width;
                own.height = im.picture->height;
                own.alpha.resize(static_cast<size_t>(own.width) * static_cast<size_t>(own.height));
                for (size_t i = 0; i < own.alpha.size(); ++i) own.alpha[i] = im.picture->rgba[i * 4 + 3];
                plane = &own;
            } else {
                plane = h.textureAlpha(im.path);
            }
            if (!plane || im.srcW <= 0 || im.srcH <= 0) {
                im.clickableAlpha = 0;
            } else {
                im.hitMask.assign(static_cast<size_t>(im.srcW) * static_cast<size_t>(im.srcH), 0);
                // A subresource's texture is its part of the file (outside the file: transparent).
                const int ox = im.subresource ? (*im.subresource)[0] : 0, oy = im.subresource ? (*im.subresource)[1] : 0;
                for (int y = 0; y < im.srcH; ++y) {
                    int ty = im.srcY + y;
                    if (ty < 0 || ty >= im.textureHeight || oy + ty < 0 || oy + ty >= plane->height) continue;
                    ty += oy;
                    for (int x = 0; x < im.srcW; ++x) {
                        int tx = im.srcX + x;
                        if (tx < 0 || tx >= im.textureWidth || ox + tx < 0 || ox + tx >= plane->width) continue;
                        tx += ox;
                        im.hitMask[static_cast<size_t>(y) * static_cast<size_t>(im.srcW) + static_cast<size_t>(x)] =
                            plane->alpha[static_cast<size_t>(ty) * static_cast<size_t>(plane->width) + static_cast<size_t>(tx)];
                    }
                }
            }
        }
        im.update(h.clock);
        // A movie's volume is applied when it moves or its record or type asks (0x453280).
        if (movie && (im.volumeMoved || movie->dirty || movie->typeMoving())) movie->applyVolume();
    }
    Drawable* drawable() override { return this; }
    SpriteImage* image() override { return img.get(); }
    uint32_t targetId() const override {
        if (kind == Kind::Mask) return img ? ownTarget : 0;
        if (kind == Kind::Target) return img && img->source == SpriteImage::Source::Target ? ownTarget : 0;
        if (kind == Kind::Displacement) return img && map ? map->target : 0;
        return 0;
    }
    bool isMask() const override { return kind == Kind::Mask; }

    // A new image replaces the old (create*: the old CSprite is released), takes the object's time
    // scale and hangs from the task's root (0x459450, 0x4aa810).
    void release() {
        movie.reset();
        img.reset();
    }
    SpriteImage& fresh() {
        release();
        img = std::make_unique<SpriteImage>();
        img->setTimeScale(timeScale);
        // A mask's CSprite starts with blending off (CSpriteMask 0x4a6730 / 0x4a6830: +0x110 = 0, and
        // Mware's setBlend(0)); CNutMask records BLEND_MODE_DISABLE as its first state for the same.
        if (kind == Kind::Mask) img->blendMode = 0;
        return *img;
    }
};

NativeObject* make(HSQUIRRELVM v) { return new SpriteObject(&host(v), argString(v, 2)); }
NativeObject* makeMask(HSQUIRRELVM v) { return new SpriteObject(&host(v), argString(v, 2), SpriteObject::Kind::Mask); }
NativeObject* makeTarget(HSQUIRRELVM v) {
    return new SpriteObject(&host(v), argString(v, 2), SpriteObject::Kind::Target);
}
NativeObject* makeDisplacementMap(HSQUIRRELVM v) {
    return new SpriteObject(&host(v), argString(v, 2), SpriteObject::Kind::Displacement);
}

#define SPRITE_SELF                                                  \
    SpriteObject* o = self<SpriteObject>(v);                         \
    if (!o) return sq_throwerror(v, "SQSprite: not an instance");
// The natives that read or change the image do nothing without one, as the exe's check +0xc4.
#define SPRITE_IMAGE \
    SPRITE_SELF      \
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

// --- Making the image ----------------------------------------------------------------------------

// create(file, x, y, w, h): int (0x50d510 -> 0x484ff0): the texture's part (x < 0: all of it); 0, or
// -1 if the texture does not load.
SQInteger create(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    o->release();
    std::string path = argString(v, 2);
    int tw = 0, th = 0;
    // A named surface (sprite.hpp): its texture, as a picture (no capture of its own).
    std::shared_ptr<NamedSurface> surface;
    // The app's texture map first (0x484f70): a texture loadTextTexture made.
    std::shared_ptr<const Image> picture;
    if (auto it = h.textTextures.find(path); it != h.textTextures.end()) {
        picture = it->second;
        tw = picture->width, th = picture->height;
    } else if (path.rfind("suf://", 0) == 0) {
        surface = h.findSurface(path);
        if (!surface) {
            h.note(h.missing, path);
            return pushInt(v, -1);
        }
        tw = surface->width, th = surface->height;
    } else if (!h.textureSize(path, tw, th)) {
        return pushInt(v, -1);
    }
    SpriteImage& im = o->fresh();
    o->screenCapture = 0;
    im.source = SpriteImage::Source::Texture;
    im.path = path;
    im.picture = picture;
    if (surface) {
        im.surface = surface;
        if (surface->kind == NamedSurface::Kind::Screen) {
            im.source = SpriteImage::Source::Screen;
            im.screenId = surface->id;
        } else if (surface->kind == NamedSurface::Kind::Scene) {
            im.source = SpriteImage::Source::Scene;
        } else {
            im.source = SpriteImage::Source::Target;
            im.targetId = surface->id;
        }
    }
    im.textureWidth = tw;
    im.textureHeight = th;
    int x = argI(v, 3);
    if (x < 0) {
        im.srcX = im.srcY = 0;
        im.srcW = tw;
        im.srcH = th;
    } else {
        im.srcX = x;
        im.srcY = argI(v, 4);
        im.srcW = argI(v, 5);
        im.srcH = argI(v, 6);
    }
    return pushInt(v, 0);
}
// createColor(w, h, r, g, b): int (0x50d7b0 -> 0x485340): a white primitive of that size whose four
// corner colours are set to (r, g, b) at once (CSprite vtable +0x8c, 0x452f20); 0 (the exe clears
// EAX before returning).
SQInteger createColor(HSQUIRRELVM v) {
    SPRITE_SELF
    SpriteImage& im = o->fresh();
    im.source = SpriteImage::Source::Color;
    im.srcW = argI(v, 2);
    im.srcH = argI(v, 3);
    std::array<float, 3> rgb{argFloat(v, 4), argFloat(v, 5), argFloat(v, 6)};
    for (auto& c : im.color) c.moveTo(rgb, 0, 0);
    return pushInt(v, 0);
}
// createSubresource(file, x, y, w, h): int (0x50d660 -> 0x485130): a sprite showing a texture made
// of the part x, y, w, h of the file (Mware's loader given the rectangle: its own texture under the
// name and the rectangle, decoded to that part), all of it; -1 (and no image) if nothing loads. Its
// edges, filters, UV offsets and displacement work on that texture, not the whole picture. Fitted:
// a part reaching outside the picture is transparent there.
SQInteger createSubresource(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    o->release();
    std::string path = argString(v, 2);
    std::array<int, 4> rect{argI(v, 3), argI(v, 4), argI(v, 5), argI(v, 6)};
    int tw = 0, th = 0;
    if (rect[2] <= 0 || rect[3] <= 0 || !h.textureSize(path, tw, th)) return pushInt(v, -1);
    SpriteImage& im = o->fresh();
    o->screenCapture = 0;
    im.source = SpriteImage::Source::Texture;
    im.path = path;
    im.subresource = rect;
    im.textureWidth = im.srcW = rect[2];
    im.textureHeight = im.srcH = rect[3];
    im.srcX = im.srcY = 0;
    return pushInt(v, 0);
}
// createScene(x, y, w, h): int (0x50daa0 -> 0x485240): a sprite showing the 3D scene's picture: the
// named render target "suf://" + its name, the screen's size (CGraphics +0x70), handed to the
// scene's list of targets (CApp +0x13c, 0x445710); the part x, y, w, h of it (x < 0: all of it),
// capturing (+0xcc, +0xd4). No 3D scene renders in Nyanroid yet, so the picture is drawn as nothing.
SQInteger createScene(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    o->release();
    SpriteImage& im = o->fresh();
    im.source = SpriteImage::Source::Scene;
    im.textureWidth = h.screenWidth();
    im.textureHeight = h.screenHeight();
    im.surface = h.namedSurface("suf://" + o->taskName, NamedSurface::Kind::Scene, 0, im.textureWidth, im.textureHeight);
    if (im.surface->kind != NamedSurface::Kind::Scene) im.surface.reset();
    if (argI(v, 2) < 0) {
        im.srcX = im.srcY = 0;
        im.srcW = im.textureWidth;
        im.srcH = im.textureHeight;
    } else {
        im.srcX = argI(v, 2);
        im.srcY = argI(v, 3);
        im.srcW = argI(v, 4);
        im.srcH = argI(v, 5);
    }
    o->capture = 1;
    return pushInt(v, 0);
}
// createScreen(x, y, w, h): int (0x50d8b0 -> 0x485240): a sprite showing a picture of the screen
// (a screen-sized texture the device makes: CSprite +0xc0), the part x, y, w, h of it (x < 0: all
// of it). The picture is taken each frame while capture is on (+0xcc, set here; +0xd0 marks the
// sprite a capture): the renderer's DrawItem::screen.
SQInteger createScreen(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    SpriteImage& im = o->fresh();
    im.source = SpriteImage::Source::Screen;
    im.textureWidth = h.screenWidth();
    im.textureHeight = h.screenHeight();
    // Its texture is the named surface "suf://" + its name when it has one (0x50d8ec), shared with
    // whatever already holds that name (a render target's under it: Guess, a new picture).
    if (!o->taskName.empty()) {
        im.surface = h.namedSurface("suf://" + o->taskName, NamedSurface::Kind::Screen, h.nextScreenId, im.textureWidth,
                                    im.textureHeight);
        if (im.surface->kind != NamedSurface::Kind::Screen) im.surface.reset();
    }
    if (im.surface) {
        im.screenId = im.surface->id;
        if (im.screenId == h.nextScreenId) ++h.nextScreenId;
    } else {
        im.screenId = h.nextScreenId++;
    }
    o->screenCapture = 1;
    if (argI(v, 2) < 0) {
        im.srcX = im.srcY = 0;
        im.srcW = im.textureWidth;
        im.srcH = im.textureHeight;
    } else {
        im.srcX = argI(v, 2);
        im.srcY = argI(v, 3);
        im.srcW = argI(v, 4);
        im.srcH = argI(v, 5);
    }
    o->capture = 1;
    return pushInt(v, 0);
}
// setCapture(on) on a screen capture (0x50f170): off, its picture stays what it last took (+0xc8:
// the next update, 0x50d250, has the texture copy itself into a snapshot that is shown instead,
// CRenderTargetTexture vtable +0x3c 0x1006a6d0, CopyResource); on again, the snapshot is dropped
// (+0x44, 0x1006a840) and it follows the screen. The renderer's texture keeps the last picture taken,
// which is the snapshot's. Nothing on other sprites. isCaptureEnabled() (0x50f910): +0xcc.
SQInteger setCaptureScreen(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->img || o->img->source != SpriteImage::Source::Screen || !o->screenCapture) return 0;
    o->capture = argI(v, 2);
    return 0;
}
SQInteger isCaptureEnabledScreen(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->capture);
}

// A picture scaled as the device's stretch copy does (linear filter: Fitted, sampled at pixel
// centres), RGBA.
std::vector<uint8_t> scalePicture(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4);
    for (int y = 0; y < dh; ++y) {
        float fy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f, 0.0f,
                              static_cast<float>(sh - 1));
        int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, sh - 1);
        float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < dw; ++x) {
            float fx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f,
                                  0.0f, static_cast<float>(sw - 1));
            int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, sw - 1);
            float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) {
                    return static_cast<float>(src[(static_cast<size_t>(yy) * static_cast<size_t>(sw) + static_cast<size_t>(xx)) * 4 + static_cast<size_t>(c)]);
                };
                float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                float val = top + (bottom - top) * ty;
                out[(static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(val + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

// A picture in Mware's writers' formats (CGraphicsDX11 0x10057d40): 0 BMP (24-bit, 0x1000f8d0),
// 1 JPEG (0x1000fff0; quality 90: Fitted), else PNG (0x1000fa90).
Bytes encodePicture(const std::vector<uint8_t>& rgba, int w, int h, int format) {
    Bytes out;
    auto sink = [](void* ctx, void* data, int size) {
        auto* b = static_cast<Bytes*>(ctx);
        const auto* p = static_cast<const uint8_t*>(data);
        b->insert(b->end(), p, p + size);
    };
    if (format == 0 || format == 1) {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
        for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i) {
            for (size_t c = 0; c < 3; ++c) rgb[i * 3 + c] = rgba[i * 4 + c];
        }
        if (format == 0) stbi_write_bmp_to_func(sink, &out, w, h, 3, rgb.data());
        else stbi_write_jpg_to_func(sink, &out, w, h, 3, rgb.data(), 90);
    } else {
        stbi_write_png_to_func(sink, &out, w, h, 4, rgba.data(), w * 4);
    }
    return out;
}

// saveImage(file, width, height, format): int (0x510cb0 -> CSprite 0x45c970): the sprite's picture
// (its whole texture, +0xc0; a render target's sprite and SQSpriteRenderTarget's own share it) as
// it is when its size is width x height, else stretched into a new texture of that size (device
// +0xc8, linear), written to the file in a format of Mware's writers (0x10057d40); 0, or -1
// without a picture. The save screens' thumbnails: Muramasa's and Totono's SaveImage a screen
// capture in format 1, the save screen's SaveNutImage (Mojika) a render target in format 2 (PNG,
// as the PC's thum.npf files are).
SQInteger saveImage(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    if (!o->img) return pushInt(v, -1);
    std::string path = argString(v, 2);
    int w = argI(v, 3), ht = argI(v, 4), format = argI(v, 5);
    if (w <= 0 || ht <= 0 || w > 8192 || ht > 8192) return pushInt(v, -1);
    const SpriteImage& im = *o->img;
    std::vector<uint8_t> rgba;
    if (im.source == SpriteImage::Source::Screen) {
        if (!h.screenReader || !h.screenReader(im.screenId, w, ht, rgba)) return pushInt(v, -1);
    } else if (im.source == SpriteImage::Source::Target) {
        // A render target's sprite: its texture (+0xc0) as the last frame drew it, which is why
        // Mojika's SaveNutImage waits a frame (Draw: suspendWait) before saving it.
        if (!h.targetReader || !h.targetReader(im.targetId, w, ht, rgba)) return pushInt(v, -1);
    } else if (im.source == SpriteImage::Source::Texture && im.picture) {
        if (im.picture->width <= 0 || im.picture->height <= 0) return pushInt(v, -1);
        rgba = scalePicture(im.picture->rgba.data(), im.picture->width, im.picture->height, w, ht);
    } else if (im.source == SpriteImage::Source::Texture) {
        auto bytes = h.readAnyFile(im.path);
        if (!bytes) return pushInt(v, -1);
        try {
            Image pic = decodeImage(*bytes);
            rgba = scalePicture(pic.rgba.data(), pic.width, pic.height, w, ht);
        } catch (const std::exception&) {
            return pushInt(v, -1);
        }
    } else {
        return pushInt(v, -1);
    }
    if (rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(ht) * 4) return pushInt(v, -1);
    Bytes file = encodePicture(rgba, w, ht, format);
    if (file.empty() || !h.saves->write(h.writablePath(path), file)) return pushInt(v, -1);
    return pushInt(v, 0);
}
// createMovie(file, alpha): int (0x50dc90 -> 0x4853d0): the movie opened (Mware's source reader)
// on a texture of its frame size, half as tall with the alpha flag (whose frames carry the alpha
// below the picture); 0, or -1 if it does not open. It shows its first picture until played.
SQInteger createMovie(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    o->release();
    std::string path = argString(v, 2);
    bool alpha = argI(v, 3) != 0;
    auto file = h.openAnyFile(path);
    if (!file) {
        h.note(h.missing, path);
        return pushInt(v, -1);
    }
    std::optional<AsfInfo> info;
    try {
        info = readAsfInfo(*file);
    } catch (const std::exception& e) {
        if (h.log) h.log("movie " + path + ": " + e.what());
    }
    if (!info || info->width <= 0 || info->height <= 0) {
        if (h.log) h.log("movie " + path + ": not a movie Nyanroid reads");
        return pushInt(v, -1);
    }
    SpriteImage& im = o->fresh();
    im.source = SpriteImage::Source::Movie;
    im.path = path;
    im.textureWidth = info->width;
    im.textureHeight = alpha ? info->height / 2 : info->height;  // Mware +0x78
    im.srcW = im.textureWidth;
    im.srcH = im.textureHeight;
    o->movie = std::make_unique<MovieObject>(&h, &im, path, alpha, *info);
    o->movie->clock.setTimeScale(o->timeScale);
    return pushInt(v, 0);
}
// link(name) (0x50d380): hangs the image from the named object's (none, or not found: the task's root).
SQInteger link(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    SpriteImage* parent = nullptr;
    std::string name = argString(v, 2);
    if (!name.empty()) {
        if (FrameTask* t = h.findTask(name)) {
            if (Drawable* d = t->drawable()) parent = d->linkNode();
        }
    }
    if (o->img) o->img->setParent(parent);
    return 0;
}

// --- The draw list --------------------------------------------------------------------------------

SQInteger setPriority(HSQUIRRELVM v) {
    SPRITE_SELF
    host(v).setDrawPriority(o, argI(v, 2));
    return 0;
}
SQInteger getPriority(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, host(v).drawPriority(o));
}
SQInteger setErase(HSQUIRRELVM v) {
    SPRITE_SELF
    o->erase = argI(v, 2);
    return 0;
}
SQInteger getErase(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->erase);
}

// --- Tweens ---------------------------------------------------------------------------------------

std::array<float, 3> vec(HSQUIRRELVM v, SQInteger idx, int n) {
    std::array<float, 3> out{};
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = argFloat(v, idx + i);
    return out;
}
// A getter's result: one value as a float; more to SQMATH_RET_*.
SQInteger pushTween(HSQUIRRELVM v, const Tween& t) {
    if (t.n == 1) return pushFloat(v, t.now[0]);
    host(v).setResult(t.now[0], t.now[1], t.n > 2 ? t.now[2] : 0.0f);
    return 0;
}

// The tweens that work alike: set(values..., ms, curve) from where it is, get, the timer.
#define TWEEN_NATIVES(Name, member, components)                                     \
    [[maybe_unused]] SQInteger set##Name(HSQUIRRELVM v) {                            \
        SPRITE_IMAGE                                                                 \
        auto& t = o->img->member;                                                    \
        t.moveTo(vec(v, 2, components), argI(v, 2 + components), argI(v, 3 + components)); \
        return 0;                                                                    \
    }                                                                                \
    [[maybe_unused]] SQInteger get##Name(HSQUIRRELVM v) {                            \
        SPRITE_SELF                                                                  \
        if (!o->img) return components == 1 ? pushFloat(v, 0.0f) : 0;               \
        return pushTween(v, o->img->member);                                         \
    }                                                                                \
    [[maybe_unused]] SQInteger get##Name##Timer(HSQUIRRELVM v) {                     \
        SPRITE_SELF                                                                  \
        return pushFloat(v, o->img ? o->img->member.timer : 0.0f);                   \
    }                                                                                \
    [[maybe_unused]] SQInteger set##Name##Timer(HSQUIRRELVM v) {                     \
        SPRITE_IMAGE                                                                 \
        o->img->member.setTimer(argFloat(v, 2));                                     \
        return 0;                                                                    \
    }

TWEEN_NATIVES(Fade, fade, 1)
TWEEN_NATIVES(Scale, scale, 2)
TWEEN_NATIVES(Rotate, rotate, 3)
TWEEN_NATIVES(Scroll, scroll, 2)
TWEEN_NATIVES(ColorBalance, colorBalance, 3)
TWEEN_NATIVES(ColorHue, colorHue, 1)
TWEEN_NATIVES(ColorSaturation, colorSaturation, 1)
TWEEN_NATIVES(ColorBrightness, colorBrightness, 1)
TWEEN_NATIVES(ColorGlow, colorGlow, 1)
TWEEN_NATIVES(NegaposiPower, negaposi, 1)
TWEEN_NATIVES(SepiaPower, sepia, 1)
TWEEN_NATIVES(ShadingPower, shading, 1)
TWEEN_NATIVES(MonochromePower, monochrome, 3)
TWEEN_NATIVES(ColorLevel, colorLevel, 3)
#undef TWEEN_NATIVES

// setShadingParameter(level, density) (0x50e8f0), getShadingLevel / getShadingDensity (0x50ea40 /
// 0x50ea50): the material's shading settings (SpriteImage::shadingLevel).
SQInteger setShadingParameter(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->shadingLevel = argI(v, 2);
    o->img->shadingDensity = argFloat(v, 3);
    return 0;
}
SQInteger getShadingLevel(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img ? o->img->shadingLevel : 0);
}
SQInteger getShadingDensity(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->shadingDensity : 0.0f);
}

// --- Cameras (section 5p) -------------------------------------------------------------------------
// setCamera(name, depth, fov) (0x50f0a0 -> CSprite 0x459af0): the camera task object of that name
// (none, or an unknown name: the task's); the source scale back to 1, and with a depth above 0 on a
// perspective camera the size that keeps the sprite's pixels at that depth: 2 tan(fov / 2) depth over
// the screen's height (fov: the given one, or the camera's when 0 or less).
SQInteger setCamera(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    std::string name = argString(v, 2);
    im.camera = name.empty() ? nullptr : cameraView(host(v), name);
    im.sourceScale = {1.0f, 1.0f, 1.0f};
    im.cameraDepth = argFloat(v, 3);
    if (im.camera && im.cameraDepth > 0.0f && !im.camera->ortho) {
        float fov = argFloat(v, 4);
        if (fov <= 0.0f) fov = im.camera->fov;
        float t = std::tan(fov * 0.00872664619f);
        float s = (t * im.cameraDepth + t * im.cameraDepth) / static_cast<float>(host(v).screenHeight());
        im.sourceScale = {s, s, s};
    }
    return 0;
}

// --- Splines (section 5m) -------------------------------------------------------------------------
// resetSpline / setSpline / beginSpline (SQSprite 0x50f600 / 0x50f610 / 0x50f6b0 -> CSprite 0x4597c0,
// the spline's 0x45fa50, 0x459840); getSplineTimer / setSplineTimer (0x511000 / 0x5114e0 -> 0x45b850).

// The move tween put at a point without running (+0x18c / +0x198: now and the value without power).
void placeMove(SpriteImage& im, const std::array<float, 3>& p) {
    for (size_t i = 0; i < 3; ++i) {
        im.move.nowNoPower[i] = p[i];
        im.move.now[i] = p[i] * im.move.power;
    }
}
SQInteger resetSpline(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->spline.reset();
    return 0;
}
SQInteger setSpline(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->spline.add({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argFloat(v, 6));
    return 0;
}
// beginSpline(time, curve) (0x459840): the spline starts; the bezier's points go and its tween ends
// where it was going; a running move ends, and the position is the spline's first point.
SQInteger beginSpline(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    im.spline.begin(argI(v, 2), argI(v, 3));
    im.bezier.points.clear();
    if (im.bezier.t.active) {
        im.bezier.t.active = 0;
        im.bezier.t.nowNoPower[0] = im.bezier.t.dst[0];
        im.bezier.t.now[0] = im.bezier.t.power * im.bezier.t.dst[0];
    }
    im.move.active = 0;  // 0x45d550
    placeMove(im, im.spline.point);
    return 0;
}
SQInteger getSplineTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->spline.t.timer : 0.0f);
}
SQInteger setSplineTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    if (!im.spline.built()) return 0;
    im.spline.setTimer(argFloat(v, 2));
    placeMove(im, im.spline.point);
    return 0;
}

// --- Split faces (section 5r) ------------------------------------------------------------------------
// splitFace(rows, cols, linkRate, seed, noDistortion) (0x50fd00 -> 0x45a3e0); clearSplitAction
// (0x50fd30); pushSplitAction(table) (0x50fd70: the table read by 0x50ae90 -> 0x45a490);
// beginSplitAction(start, end, time, curve, keepGoing) (0x510110 -> 0x45b120); the timer (0x5110e0,
// 0x511860 -> 0x45c550).
SQInteger splitFace(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->split.split(argI(v, 2), argI(v, 3), argFloat(v, 4), argI(v, 5), argI(v, 6), o->img->srcW, o->img->srcH);
    return 0;
}
SQInteger clearSplitAction(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->split.clearActions();
    return 0;
}
// The table's fields as SqPlus reads them: present or not; numbers either way; flags as numbers or
// booleans.
struct SplitTable {
    HSQUIRRELVM v;
    SQInteger idx;
    bool push(const char* key) {
        sq_pushstring(v, key, -1);
        return SQ_SUCCEEDED(sq_rawget(v, idx));
    }
    bool number(const char* key, float& out) {
        if (!push(key)) return false;
        SQObjectType t = sq_gettype(v, -1);
        bool ok = t == OT_INTEGER || t == OT_FLOAT;
        if (ok) {
            SQFloat f = 0;
            sq_getfloat(v, -1, &f);
            out = static_cast<float>(f);
        }
        sq_pop(v, 1);
        return ok;
    }
    bool integer(const char* key, int32_t& out) {
        if (!push(key)) return false;
        SQObjectType t = sq_gettype(v, -1);
        bool ok = true;
        if (t == OT_INTEGER || t == OT_FLOAT) {
            SQFloat f = 0;
            sq_getfloat(v, -1, &f);
            out = static_cast<int32_t>(f);
        } else if (t == OT_BOOL) {
            SQBool b = SQFalse;
            sq_getbool(v, -1, &b);
            out = b ? 1 : 0;
        } else {
            ok = false;
        }
        sq_pop(v, 1);
        return ok;
    }
    // A sub-table (pushed; the caller pops it), or false.
    bool table(const char* key) {
        if (!push(key)) return false;
        if (sq_gettype(v, -1) == OT_TABLE) return true;
        sq_pop(v, 1);
        return false;
    }
    void vec3(const char* key, SplitFace::V3& out) {  // 0x50b2d0: x, y, z
        if (!table(key)) return;
        SplitTable t{v, sq_gettop(v)};
        t.number("x", out[0]);
        t.number("y", out[1]);
        t.number("z", out[2]);
        sq_pop(v, 1);
    }
    void edge(SplitFace::V2& out) {  // 0x50b400: front, side
        if (!table("dist_edge")) return;
        SplitTable t{v, sq_gettop(v)};
        t.number("front", out[0]);
        t.number("side", out[1]);
        sq_pop(v, 1);
    }
};
void readTerm(HSQUIRRELVM v, SplitTable& parent, const char* key, SplitFace::Term& term) {  // 0x50b4e0
    if (!parent.table(key)) return;
    term = SplitFace::Term{};
    term.on = 1;
    SplitTable t{v, sq_gettop(v)};
    t.edge(term.edge);
    t.number("dist_min", term.distMin);
    t.number("dist_max", term.distMax);
    t.number("dist_rand_min", term.distRandMin);
    t.number("dist_rand_max", term.distRandMax);
    t.vec3("rand_min", term.randMin);
    t.vec3("rand_max", term.randMax);
    t.integer("inv", term.inv);
    t.vec3("center", term.center);
    t.vec3("dir", term.dir);
    t.integer("aspect", term.aspect);
    t.vec3("mask", term.mask);
    float l = std::sqrt(term.dir[0] * term.dir[0] + term.dir[1] * term.dir[1] + term.dir[2] * term.dir[2]);
    if (l > 0.0f) term.dir = {term.dir[0] / l, term.dir[1] / l, term.dir[2] / l};
    sq_pop(v, 1);
}
void readDelay(HSQUIRRELVM v, SplitTable& parent, SplitFace::DelayTerm& term) {  // 0x50b9e0
    if (!parent.table("delay")) return;
    term = SplitFace::DelayTerm{};
    term.on = 1;
    SplitTable t{v, sq_gettop(v)};
    t.edge(term.edge);
    t.number("dist_min", term.distMin);
    t.number("dist_max", term.distMax);
    t.number("dist_rand_min", term.distRandMin);
    t.number("dist_rand_max", term.distRandMax);
    t.number("rand_min", term.randMin);
    t.number("rand_max", term.randMax);
    t.integer("inv", term.inv);
    t.vec3("center", term.center);
    t.vec3("dir", term.dir);
    t.integer("aspect", term.aspect);
    t.number("mask", term.mask);
    float l = std::sqrt(term.dir[0] * term.dir[0] + term.dir[1] * term.dir[1] + term.dir[2] * term.dir[2]);
    if (l > 0.0f) term.dir = {term.dir[0] / l, term.dir[1] / l, term.dir[2] / l};
    sq_pop(v, 1);
}
SQInteger pushSplitAction(HSQUIRRELVM v) {
    SPRITE_SELF
    if (sq_gettype(v, 2) != OT_TABLE) return 0;
    SplitFace::ActionSpec a;
    // The seed defaults to the timer (0x50ae90); the time is read in ms, kept in seconds.
    a.seed = static_cast<int32_t>(host(v).clock.milliTime);
    SplitTable t{v, 2};
    t.number("regist", a.regist);
    t.number("ang_regist", a.angRegist);
    float ms = 0.0f;
    if (t.number("time", ms)) a.time = ms * 0.001f;
    t.integer("seed", a.seed);
    readDelay(v, t, a.delay);
    readTerm(v, t, "vel", a.vel);
    readTerm(v, t, "acc", a.acc);
    readTerm(v, t, "ang_vel", a.angVel);
    readTerm(v, t, "ang_acc", a.angAcc);
    if (o->img) o->img->split.push(a, o->img->srcW, o->img->srcH);
    return 0;
}
SQInteger beginSplitAction(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->split.begin(argFloat(v, 2), argFloat(v, 3), argI(v, 4), argI(v, 5), argI(v, 6));
    return 0;
}
SQInteger getSplitActionTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->split.timer() : 0.0f);
}
SQInteger setSplitActionTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) o->img->split.setTimer(argFloat(v, 2));
    return 0;
}

// --- Texture filters (section 5o) ------------------------------------------------------------------
// The setters (SQSprite 0x50f940 .. 0x50fc40 -> CSprite 0x459e50 .. 0x45a2a0) do nothing without an
// image; the getters read CSprite (the timers: each effect's first tween's, 0x510f80 ff.); setXTimer
// (0x511680 ff. -> 0x45b6b0 ff.) sets every tween of that effect.

std::array<float, 3> one(float x) { return {x, 0.0f, 0.0f}; }
// From where the tween is, with its power (the setters' source is the tween's +0x14).
void startFromNow(Tween& t, const std::array<float, 3>& to, int32_t ms, int32_t curve) { t.set(t.now, to, ms, curve); }

// setRadialBlur(cx, cy, from, to, time, curve) (0x459e50): the centre at once; the power from `from`
// to `to`. getRadialBlurPower / getRadialBlurCenter (0x50f980 / 0x50f990).
SQInteger setRadialBlur(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->radialBlurCenter = {argFloat(v, 2), argFloat(v, 3)};
    o->img->radialBlurPower.set(one(argFloat(v, 4)), one(argFloat(v, 5)), argI(v, 6), argI(v, 7));
    return 0;
}
SQInteger getRadialBlurPower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->radialBlurPower.now[0] : 0.0f);
}
SQInteger getRadialBlurCenter(HSQUIRRELVM v) {
    SPRITE_IMAGE
    host(v).setResult(o->img->radialBlurCenter[0], o->img->radialBlurCenter[1], 0.0f);
    return 0;
}
SQInteger getRadialBlurTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->radialBlurPower.timer : 0.0f);
}
SQInteger setRadialBlurTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->radialBlurPower.setTimer(argFloat(v, 2));
    return 0;
}
// setDirectionalBlur(dx, dy, from, to, time, curve) (0x459f70), as the radial blur.
SQInteger setDirectionalBlur(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->directionalBlurDirection = {argFloat(v, 2), argFloat(v, 3)};
    o->img->directionalBlurPower.set(one(argFloat(v, 4)), one(argFloat(v, 5)), argI(v, 6), argI(v, 7));
    return 0;
}
SQInteger getDirectionalBlurPower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->directionalBlurPower.now[0] : 0.0f);
}
SQInteger getDirectionalBlurDirection(HSQUIRRELVM v) {
    SPRITE_IMAGE
    host(v).setResult(o->img->directionalBlurDirection[0], o->img->directionalBlurDirection[1], 0.0f);
    return 0;
}
SQInteger getDirectionalBlurTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->directionalBlurPower.timer : 0.0f);
}
SQInteger setDirectionalBlurTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->directionalBlurPower.setTimer(argFloat(v, 2));
    return 0;
}
// setRipple(cx, cy, power, frequency, speed, time, curve) (0x45a090): the centre at once; the three
// tweens from where they are (their +0x18, without power). The phase (setRipplePhase, 0x50fb50)
// advances by the speed each frame.
SQInteger setRipple(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    im.rippleCenter = {argFloat(v, 2), argFloat(v, 3)};
    int32_t ms = argI(v, 7), curve = argI(v, 8);
    im.ripplePower.moveTo(one(argFloat(v, 4)), ms, curve);
    im.rippleFrequency.moveTo(one(argFloat(v, 5)), ms, curve);
    im.rippleSpeed.moveTo(one(argFloat(v, 6)), ms, curve);
    return 0;
}
SQInteger getRippleCenter(HSQUIRRELVM v) {
    SPRITE_IMAGE
    host(v).setResult(o->img->rippleCenter[0], o->img->rippleCenter[1], 0.0f);
    return 0;
}
SQInteger getRipplePower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->ripplePower.now[0] : 0.0f);
}
SQInteger getRippleFrequency(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rippleFrequency.now[0] : 0.0f);
}
SQInteger getRippleSpeed(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rippleSpeed.now[0] : 0.0f);
}
SQInteger getRipplePhase(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->ripplePhase : 0.0f);
}
SQInteger setRipplePhase(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->ripplePhase = argFloat(v, 2);
    return 0;
}
SQInteger getRippleTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->ripplePower.timer : 0.0f);
}
SQInteger setRippleTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    float ms = argFloat(v, 2);
    for (Tween* t : {&o->img->ripplePower, &o->img->rippleFrequency, &o->img->rippleSpeed}) t->setTimer(ms);
    return 0;
}
// setRaster(rotate, power, frequency, speed, time, curve) (0x45a1a0): four tweens from where they
// are; the phase (setRasterPhase, 0x50fc10) advances by the speed each frame.
SQInteger setRaster(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    int32_t ms = argI(v, 6), curve = argI(v, 7);
    startFromNow(im.rasterRotate, one(argFloat(v, 2)), ms, curve);
    startFromNow(im.rasterPower, one(argFloat(v, 3)), ms, curve);
    startFromNow(im.rasterFrequency, one(argFloat(v, 4)), ms, curve);
    startFromNow(im.rasterSpeed, one(argFloat(v, 5)), ms, curve);
    return 0;
}
SQInteger getRasterRotate(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterRotate.now[0] : 0.0f);
}
SQInteger getRasterPower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterPower.now[0] : 0.0f);
}
SQInteger getRasterFrequency(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterFrequency.now[0] : 0.0f);
}
SQInteger getRasterSpeed(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterSpeed.now[0] : 0.0f);
}
SQInteger getRasterPhase(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterPhase : 0.0f);
}
SQInteger setRasterPhase(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->rasterPhase = argFloat(v, 2);
    return 0;
}
SQInteger getRasterTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->rasterPower.timer : 0.0f);
}
SQInteger setRasterTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    float ms = argFloat(v, 2);
    auto& im = *o->img;
    for (Tween* t : {&im.rasterRotate, &im.rasterPower, &im.rasterFrequency, &im.rasterSpeed}) t->setTimer(ms);
    return 0;
}
// setLens(cx, cy, power, radius, time, curve) (0x45a2a0): the centre, the power (the distance pixels
// move) and the radius, from where they are.
SQInteger setLens(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& im = *o->img;
    int32_t ms = argI(v, 6), curve = argI(v, 7);
    startFromNow(im.lensCenter, {argFloat(v, 2), argFloat(v, 3), 0.0f}, ms, curve);
    startFromNow(im.lensPower, one(argFloat(v, 4)), ms, curve);
    startFromNow(im.lensRadius, one(argFloat(v, 5)), ms, curve);
    return 0;
}
SQInteger getLensCenter(HSQUIRRELVM v) {
    SPRITE_IMAGE
    host(v).setResult(o->img->lensCenter.now[0], o->img->lensCenter.now[1], 0.0f);
    return 0;
}
SQInteger getLensPower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->lensPower.now[0] : 0.0f);
}
SQInteger getLensRadius(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->lensRadius.now[0] : 0.0f);
}
SQInteger getLensTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->lensPower.timer : 0.0f);
}
SQInteger setLensTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    float ms = argFloat(v, 2);
    for (Tween* t : {&o->img->lensCenter, &o->img->lensPower, &o->img->lensRadius}) t->setTimer(ms);
    return 0;
}

// setVolume(v, ms, curve) (0x50f430): the movie's volume from where it is; with no time it is
// applied at once (CSprite vtable +0x98).
SQInteger setVolume(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->volume.moveTo({argFloat(v, 2), 0.0f, 0.0f}, argI(v, 3), argI(v, 4));
    if (argI(v, 3) == 0 && o->movie) o->movie->applyVolume();
    return 0;
}
// getVolume() (0x50ecf0, CSprite +0x510): the value before its power.
SQInteger getVolume(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->volume.nowNoPower[0] : 0.0f);
}
SQInteger getVolumeTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->volume.timer : 0.0f);
}
SQInteger setVolumeTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->volume.setTimer(argFloat(v, 2));
    return 0;
}

// The scripts' names for these differ from the pattern: fade / scale / rotate / scroll set, and
// getMonochromeCb / Cr and getColorLevelMin / Max / Power read single components.
SQInteger getMonochromeCb(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->monochrome.now[1] : 0.0f);
}
SQInteger getMonochromeCr(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->monochrome.now[2] : 0.0f);
}
SQInteger getMonochromePowerOnly(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->monochrome.now[0] : 0.0f);
}
SQInteger getColorLevelMin(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->colorLevel.now[0] : 0.0f);
}
SQInteger getColorLevelMax(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->colorLevel.now[1] : 0.0f);
}
SQInteger getColorLevelPower(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->colorLevel.now[2] : 0.0f);
}

// move(x, y, ms, curve) (0x50de30 -> 0x458ab0): z stays where it is.
SQInteger move(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& t = o->img->move;
    t.moveTo({argFloat(v, 2), argFloat(v, 3), t.now[2]}, argI(v, 4), argI(v, 5));
    return 0;
}
// move3D(x, y, z, ms, curve) (0x458bf0).
SQInteger move3D(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->move.moveTo(vec(v, 2, 3), argI(v, 5), argI(v, 6));
    return 0;
}
SQInteger getMove(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) pushTween(v, o->img->move);
    return 0;
}
SQInteger getMoveTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->move.timer : 0.0f);
}
SQInteger setMoveTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->move.setTimer(argFloat(v, 2));
    return 0;
}
// isMoving() (0x50f820; SQSpriteRenderTarget 0x51cae0 the same on its sprite): the move (CSprite
// +0x1a8), the bezier (+0x57c) or the spline (+0x5d0) under way. Totono's CSprite vtable +0x180
// (0x44f8e0: +0x1d4 / +0x5a8 / +0x5fc) and Phantom's (+0x1f8 / +0x600 / +0x66c) test the same three.
// WaitMove waits on it, so a BezierMove(..., wait) holds its script until the path ends.
SQInteger isMoving(HSQUIRRELVM v) {
    SPRITE_SELF
    const SpriteImage* im = o->img.get();
    return pushInt(v, im && (im->move.active || im->bezier.t.active || im->spline.t.active) ? 1 : 0);
}
SQInteger isFading(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img ? o->img->fade.active : 0);
}
SQInteger isScaling(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img ? o->img->scale.active : 0);
}
SQInteger isRotating(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img ? o->img->rotate.active : 0);
}
// fade(alpha, ms, curve) (0x50dde0); scale(x, y, ms, curve); rotate(x, y, z, ms, curve); scroll(x,
// y, ms, curve): from where they are.
SQInteger fade(HSQUIRRELVM v) { return setFade(v); }
SQInteger scale(HSQUIRRELVM v) { return setScale(v); }
SQInteger rotate(HSQUIRRELVM v) { return setRotate(v); }
SQInteger scroll(HSQUIRRELVM v) { return setScroll(v); }

// moveVertex(corner, x, y, ms, curve) (0x458d20) and its getters.
SQInteger moveVertex(HSQUIRRELVM v) {
    SPRITE_IMAGE
    int i = argI(v, 2);
    if (i < 0 || i > 3) return 0;
    o->img->vertexMove[static_cast<size_t>(i)].moveTo(vec(v, 3, 2), argI(v, 5), argI(v, 6));
    return 0;
}
SQInteger getVertexMove(HSQUIRRELVM v) {
    SPRITE_SELF
    int i = argI(v, 2);
    if (o->img && i >= 0 && i < 4) pushTween(v, o->img->vertexMove[static_cast<size_t>(i)]);
    return 0;
}
SQInteger getMoveVertexTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    int i = argI(v, 2);
    return pushFloat(v, o->img && i >= 0 && i < 4 ? o->img->vertexMove[static_cast<size_t>(i)].timer : 0.0f);
}
SQInteger setMoveVertexTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    int i = argI(v, 2);
    if (i >= 0 && i < 4) o->img->vertexMove[static_cast<size_t>(i)].setTimer(argFloat(v, 3));
    return 0;
}

// setColor(corner, r, g, b, ms, curve) (0x50e220 -> 0x453160): one corner, or all for corner < 0.
SQInteger setColor(HSQUIRRELVM v) {
    SPRITE_IMAGE
    int i = argI(v, 2);
    auto to = vec(v, 3, 3);
    int32_t ms = argI(v, 6), curve = argI(v, 7);
    if (i < 0) {
        for (auto& c : o->img->color) c.moveTo(to, ms, curve);
    } else if (i < 4) {
        o->img->color[static_cast<size_t>(i)].moveTo(to, ms, curve);
    }
    return 0;
}
// getColor(corner) (0x50e4f0): the corner's r, g, b and the fade to SQMATH_RET_1..4; corner < 0: all
// four corners to SQMATH_RET_1..16.
SQInteger getColor(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    if (!o->img) return 0;
    int i = argI(v, 2);
    float a = o->img->fade.now[0];
    auto put = [&](int slot, const Tween& c) {
        for (int k = 0; k < 3; ++k) h.mathVars[ScriptHost::Impl::kMathRet + slot * 4 + k] = c.now[static_cast<size_t>(k)];
        h.mathVars[ScriptHost::Impl::kMathRet + slot * 4 + 3] = a;
    };
    if (i < 0) {
        for (int k = 0; k < 4; ++k) put(k, o->img->color[static_cast<size_t>(k)]);
    } else if (i < 4) {
        put(0, o->img->color[static_cast<size_t>(i)]);
    }
    return 0;
}
SQInteger getColorTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    int i = argI(v, 2);
    return pushFloat(v, o->img && i >= 0 && i < 4 ? o->img->color[static_cast<size_t>(i)].timer : 0.0f);
}
SQInteger setColorTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    int i = argI(v, 2);
    float t = argFloat(v, 3);
    if (i < 0) {
        for (auto& c : o->img->color) c.setTimer(t);
    } else if (i < 4) {
        o->img->color[static_cast<size_t>(i)].setTimer(t);
    }
    return 0;
}

// --- Plain properties -----------------------------------------------------------------------------

SQInteger setCenter(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) {
        o->img->centerX = argFloat(v, 2);
        o->img->centerY = argFloat(v, 3);
    }
    return 0;
}
SQInteger setPivot(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) {
        o->img->pivotX = static_cast<float>(argI(v, 2));
        o->img->pivotY = static_cast<float>(argI(v, 3));
    }
    return 0;
}
// getSize() (0x50eaf0): the image's width and height to SQMATH_RET_1 / _2.
SQInteger getSize(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) host(v).setResult(static_cast<float>(o->img->srcW), static_cast<float>(o->img->srcH));
    return 0;
}
SQInteger setBlendMode(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->blendMode = argI(v, 2);
    return 0;
}
// setTimeScale(s) (0x50e490): the object's, and its image's tweens.
SQInteger setTimeScale(HSQUIRRELVM v) {
    SPRITE_SELF
    o->timeScale = argFloat(v, 2);
    if (o->img) o->img->setTimeScale(o->timeScale);
    if (o->movie) o->movie->clock.setTimeScale(o->timeScale);  // Mware +0x48 (0x459450)
    return 0;
}
SQInteger setActionCompatibleMode(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->actionCompatible = argI(v, 2);
    return 0;
}
SQInteger setTextureFiltering(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->textureFiltering = argI(v, 2);
    return 0;
}
// setClickable(on, alpha) (0x50f730): the alpha test only with a texture.
SQInteger setClickable(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->img) return 0;
    o->img->clickable = argI(v, 2);
    o->img->clickableAlpha = o->img->source == SpriteImage::Source::Texture ? argI(v, 3) : 0;
    return 0;
}
// --- Hit tests -----------------------------------------------------------------------------------

// Whether the pointer is over the sprite as the last frame drew it (CSprite vtable +0xb0, 0x4587b0):
// clickable, drawn, the pointer inside the quad (0x4584a0) and, in the alpha mode with its mask made,
// the part's alpha there above the threshold.
bool pointerOver(ScriptHost::Impl& h, const SpriteImage& im) {
    if (!im.clickable || !im.drawnQuad) return false;
    auto uv = quadPointToUv(*im.drawnQuad, static_cast<float>(h.input.mouse.x()), static_cast<float>(h.input.mouse.y()));
    if (!uv) return false;
    if (im.hitMask.empty() || !im.clickableAlpha) return true;
    float fx = static_cast<float>(im.srcW) * (*uv)[0];
    float fy = static_cast<float>(im.srcH) * (*uv)[1];
    fx = fx < 0.0f ? 0.0f : std::min(fx, static_cast<float>(im.srcW - 1));
    fy = fy < 0.0f ? 0.0f : std::min(fy, static_cast<float>(im.srcH - 1));
    size_t i = static_cast<size_t>(static_cast<int>(fy)) * static_cast<size_t>(im.srcW) + static_cast<size_t>(static_cast<int>(fx));
    return i < im.hitMask.size() && im.hitMask[i] > im.hitThreshold;
}
// isOver() (Muramasa 0x50f900) / isOver(flag) (Totono 0x4e16f0 -> 0x44e180, whose flag also asks
// for a state Nyanroid does not track yet: ignored, Guess).
SQInteger isOver(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img && pointerOver(host(v), *o->img) ? 1 : 0);
}
// Totono's isDown(trigger) (0x4e1710 -> 0x44e380): over it, and the left button or Return / keypad
// Enter down (with trigger, only as it goes down).
SQInteger isDown(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    if (!o->img || !pointerOver(h, *o->img)) return pushInt(v, 0);
    bool trigger = argI(v, 2) != 0;
    const auto& kb = h.input.keyboard;
    int left = h.input.mouse.buttonState(Mouse::Left);
    bool down = trigger ? left == 2 || kb.isDown(0x1c, true) || kb.isDown(0x9c, true)
                        : left == 2 || left == 3 || kb.isDown(0x1c, false) || kb.isDown(0x1c, true) ||
                              kb.isDown(0x9c, false) || kb.isDown(0x9c, true);
    return pushInt(v, down ? 1 : 0);
}
// Totono's isUp() (0x4e1730 -> 0x44e570 with 1): the left button or Return / keypad Enter released.
// The exe also asks a per-sprite state (+0x158) whose writer is not found; Nyanroid asks that the
// pointer be over the sprite instead (Guess).
SQInteger isUp(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    if (!o->img || !pointerOver(h, *o->img)) return pushInt(v, 0);
    const auto& kb = h.input.keyboard;
    bool up = h.input.mouse.buttonState(Mouse::Left) == 1 || kb.isUp(0x1c) || kb.isUp(0x9c);
    return pushInt(v, up ? 1 : 0);
}
SQInteger setChangeCursor(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) o->img->changeCursor = argI(v, 2);
    return 0;
}
SQInteger setUVOffset(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->uvOffsetX = argFloat(v, 2);
    o->img->uvOffsetY = argFloat(v, 3);
    return 0;
}
SQInteger getUVOffset(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img) host(v).setResult(o->img->uvOffsetX, o->img->uvOffsetY);
    return 0;
}
// addSoundGroup(name, priority, factor) (0x5104c0 -> CSprite 0x45b4b0) and setVolumeControlFadeTime(in,
// inCurve, out, outCurve) (0x510520): on a movie's sound record (CSprite +0x524), as SQSound's: the
// group only if the manager defines it; nothing without a movie.
SQInteger addSoundGroup(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->movie) return 0;
    std::string name = argString(v, 2);
    if (host(v).groupVolumes.find(name) == host(v).groupVolumes.end()) return 0;
    o->movie->groups.push_back({name, argI(v, 3), argFloat(v, 4)});
    o->movie->dirty = true;
    return 0;
}
SQInteger setVolumeControlFadeTime(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->movie) return 0;
    o->movie->fadeIn = argI(v, 2);
    o->movie->fadeInCurve = argI(v, 3);
    o->movie->fadeOut = argI(v, 4);
    o->movie->fadeOutCurve = argI(v, 5);
    return 0;
}
// isDeletedTexture() (0x510570, CSprite +0xc08): the device lost the texture; never here.
SQInteger isDeletedTexture(HSQUIRRELVM v) { return pushInt(v, 0); }

// --- Movies --------------------------------------------------------------------------------------
// On a sprite without a movie, play and stop work a screen capture (+0xd0): play turns it on
// (+0xcc), stop off, taking the picture once more first (+0xc8); CNutSprite.setCapture calls them.

// play(loop) (0x50f2b0 -> CSprite 0x459170): 1 plays for ever (-1), other counts as they are.
SQInteger play(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->movie) {
        o->movie->play(argI(v, 2));
    } else if (o->img && o->img->source == SpriteImage::Source::Screen && o->screenCapture) {
        o->capture = 1;
    }
    return 0;
}
SQInteger stop(HSQUIRRELVM v) {  // 0x50f310
    SPRITE_SELF
    if (o->movie) {
        o->movie->stop();
    } else if (o->img && o->img->source == SpriteImage::Source::Screen && o->screenCapture) {
        o->capture = 0;  // as setCapture(0): the picture stays
    }
    return 0;
}
SQInteger pause(HSQUIRRELVM v) {  // 0x50f370
    SPRITE_SELF
    if (o->movie) o->movie->pause();
    return 0;
}
SQInteger onResume(HSQUIRRELVM v) {  // 0x50f3d0 -> CSprite 0x459230
    SPRITE_SELF
    if (o->movie) o->movie->resume();
    return 0;
}
SQInteger isPlaying(HSQUIRRELVM v) {  // 0x50f8a0: Mware +0x34
    SPRITE_SELF
    return pushInt(v, o->movie && o->movie->clock.playing ? 1 : 0);
}
SQInteger isPaused(HSQUIRRELVM v) {  // 0x50f8c0: Mware +0x38
    SPRITE_SELF
    return pushInt(v, o->movie && o->movie->clock.paused ? 1 : 0);
}
// setLoop(n) (0x50f490 -> Mware +0x40): the plays left, as given.
SQInteger setLoop(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->movie) o->movie->clock.setLoop(argI(v, 2));
    return 0;
}
SQInteger seek(HSQUIRRELVM v) {  // 0x50f4d0 -> Mware +0x20
    SPRITE_SELF
    if (o->movie) o->movie->seek(argI(v, 2));
    return 0;
}
// getDurationTime() / getTotalTime() (0x50f780) and getRemainTime() (0x50f7b0): milliseconds.
SQInteger getDurationTime(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->movie ? o->movie->clock.durationMs() : 0);
}
SQInteger getRemainTime(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->movie) return pushInt(v, 0);
    return pushInt(v, o->movie->clock.durationMs() - o->movie->clock.positionMs());
}

// --- Render targets and masks ---------------------------------------------------------------------

// The drawable a name gives (the task's find, CTask +0xb4 vtable +0x4c).
Drawable* namedDrawable(ScriptHost::Impl& h, const std::string& name) {
    if (name.empty()) return nullptr;
    FrameTask* t = h.findTask(name);
    return t ? t->drawable() : nullptr;
}
}  // namespace

// setSurface(name) (SQSprite 0x50ee00, SQSpriteMask 0x517380, SQSpriteRenderTarget 0x51c1c0, SQText
// 0x522390): the object draws into the named object's render target (vtable +0x98), or into the
// task's (the screen) for a name that finds none.
void setDrawableSurface(ScriptHost::Impl& h, Drawable& d, const std::string& name) {
    Drawable* t = namedDrawable(h, name);
    d.surface = t && !t->isMask() ? t->targetId() : 0;
}
// setMask(name) (SQSprite 0x50ed00 / 0x4589f0): the named mask's texture masks the object.
void setDrawableMask(ScriptHost::Impl& h, Drawable& d, const std::string& name) {
    Drawable* t = namedDrawable(h, name);
    d.mask = t && t->isMask() ? t->targetId() : 0;
}

namespace {

SQInteger setSurface(HSQUIRRELVM v) {
    SPRITE_SELF
    setDrawableSurface(host(v), *o, argString(v, 2));
    return 0;
}
// setMask(name); a mask given a mask also goes just above it (SQSpriteMask 0x517220).
SQInteger setMask(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    std::string name = argString(v, 2);
    setDrawableMask(h, *o, name);
    if (o->kind == SpriteObject::Kind::Mask) {
        if (Drawable* t = namedDrawable(h, name)) h.setDrawPriority(o, h.drawPriority(t) + 1);
    }
    return 0;
}
SQInteger setDiffuseOnly(HSQUIRRELVM v) {  // SQSpriteMask 0x517710 (ISpriteMask +0xd4)
    SPRITE_SELF
    o->diffuseOnly = argI(v, 2);
    return 0;
}
SQInteger setInverseAlpha(HSQUIRRELVM v) {  // SQSpriteMask 0x517730 (ISpriteMask +0xd8)
    SPRITE_SELF
    o->inverseAlpha = argI(v, 2);
    return 0;
}

// create(w, h, flag): int (SQSpriteRenderTarget 0x51bcf0 -> 0x485790): a render target of that size,
// capturing (+0xcc 1); -1 if it cannot be made.
SQInteger createTarget(HSQUIRRELVM v) {
    SPRITE_SELF
    o->release();
    int w = argI(v, 2), ht = argI(v, 3);
    if (w <= 0 || ht <= 0) return pushInt(v, -1);
    SpriteImage& im = o->fresh();
    im.source = SpriteImage::Source::Target;
    // Named "suf://" + its name when it has one (0x485790); a name in use gives that texture (and
    // its size).
    if (!o->taskName.empty()) {
        auto s = host(v).namedSurface("suf://" + o->taskName, NamedSurface::Kind::Target, o->ownTarget, w, ht);
        if (s->kind == NamedSurface::Kind::Target) {
            im.surface = s;
            o->ownTarget = s->id;
            w = s->width, ht = s->height;
        }
    }
    im.targetId = o->ownTarget;
    im.textureWidth = im.srcW = w;
    im.textureHeight = im.srcH = ht;
    o->capture = 1;
    return pushInt(v, 0);
}
// SQDisplacementMap (muramasa_en.exe natives 0x531320 ff.; CSpriteDisplacementMap 0x566f00).
// The size its target gets: that of the render target it is on (0x567310: the surface's texture),
// else the screen's.
std::array<int, 2> displacementMapSize(ScriptHost::Impl& h, const SpriteObject& o) {
    if (o.surface) {
        for (const auto& e : h.drawList) {
            auto* t = dynamic_cast<SpriteObject*>(e.object);
            if (t && t->targetId() == o.surface && t->img) return {t->img->srcW, t->img->srcH};
        }
    }
    return {h.screenWidth(), h.screenHeight()};
}
// create(sx, sy) (0x531320 -> CSpriteDisplacementMap 0x567110): a new map (the old one released;
// objects holding it keep it): Mware's CDisplacementMap (CGraphics +0xbc, 0x10017a40) takes (sx, sy)
// as its sampling scale and makes its render target (the device's size: the screen's here) with the
// target's clear colour 0xff808080 (CSpriteDisplacementMap's constructor), so an untouched map moves
// nothing much ((128 / 255) x 2 - 1 = 1 / 255 of the scale). Its picture is not drawn (the target's
// texture sprite is never made, +0xc0 0; the sprite +0x14e0 that shows the map is faded to 0). 0.
SQInteger createDisplacementMap(HSQUIRRELVM v) {
    SPRITE_SELF
    auto& h = host(v);
    o->release();
    SpriteImage& im = o->fresh();
    o->map = std::make_shared<DisplacementMap>();
    o->map->target = h.nextTargetId++;
    o->map->scaleX = argFloat(v, 2);
    o->map->scaleY = argFloat(v, 3);
    im.source = SpriteImage::Source::Target;
    im.targetId = o->map->target;
    im.textureWidth = im.srcW = h.screenWidth();
    im.textureHeight = im.srcH = h.screenHeight();
    o->capture = 1;
    const float grey = 128.0f / 255.0f;
    o->clearColor = {grey, grey, grey, 1.0f};
    return pushInt(v, 0);
}
// setSurface(name) (0x531430): the map, then the object, on the named object's render target or the
// screen's (CSpriteDisplacementMap vtable +0x4c, 0x567310: the map's target takes that target's size).
// Without create() the exe has no map to tell (it crashes): nothing here.
SQInteger setSurfaceDisplacementMap(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->img || !o->map) return 0;
    setDrawableSurface(host(v), *o, argString(v, 2));
    auto size = displacementMapSize(host(v), *o);
    o->img->textureWidth = o->img->srcW = size[0];
    o->img->textureHeight = o->img->srcH = size[1];
    return 0;
}
// getSize() (0x531550): the map's texture size in the vector result.
SQInteger getSizeDisplacementMap(HSQUIRRELVM v) {
    SPRITE_SELF
    if (o->img && o->map) host(v).setResult(static_cast<float>(o->img->srcW), static_cast<float>(o->img->srcH));
    return 0;
}

// setCapture(on) (0x51c810): off, the picture stays as it is (Fitted: what is drawn into it then is
// not); on again, it is reset each frame unless it copies another target.
SQInteger setCapture(HSQUIRRELVM v) {
    SPRITE_SELF
    o->capture = argI(v, 2);
    return 0;
}
SQInteger isCaptureEnabled(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->capture);
}
// setCopyTarget(name) (0x51c890): the named target's picture is copied in before each frame's drawing.
SQInteger setCopyTarget(HSQUIRRELVM v) {
    SPRITE_SELF
    Drawable* t = namedDrawable(host(v), argString(v, 2));
    o->copyFrom = t && !t->isMask() ? t->targetId() : 0;
    return 0;
}
SQInteger setCaptureTarget(HSQUIRRELVM v) {  // 0x51c9a0: the name kept
    SPRITE_SELF
    o->captureTarget = argString(v, 2);
    return 0;
}
SQInteger setDelayRendering(HSQUIRRELVM v) {  // 0x51caa0: CSprite +0xcf0 (its use not followed)
    SPRITE_SELF
    o->delayRendering = argI(v, 2);
    return 0;
}
SQInteger isDelayedRendering(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->delayRendering);
}
SQInteger clearTarget(HSQUIRRELVM v) {  // 0x51cb70: cleared at the next draw (0x4ca550)
    SPRITE_SELF
    o->clearRequested = true;
    return 0;
}
// createCustomEffect(file, parameters) (0x51d380, totono_en.exe 0x4ef8d0, phantom.exe 0x14014d4b0):
// -1 in every exe, none of which builds an effect: Muramasa's and Phantom's build the file's name
// and drop it; Totono's hands it to CSpriteRenderTarget vtable +0x3c8 (0x49c3c0), which answers -1.
// Parameters given go first to setCustomEffectParameter, which answers -1 as well (an effect is
// never made).
SQInteger createCustomEffect(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, -1);
}
// setCustomEffectParameter(table) (0x51d440, totono_en.exe 0x4ef9b0, phantom.exe 0x14014d600): hands
// the table's named ints, floats, bools and strings to the target's custom effect (CRenderTarget
// +0x14d8, Totono's +0xbf0, Phantom's +0x16b0) and answers 0; without one -1. Nothing in the exes
// sets that effect (their constructors' 0 only), so it is always -1.
SQInteger setCustomEffectParameter(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, -1);
}

// --- Transitions ---------------------------------------------------------------------------------

// transition(src, dst, rate, rule, time, curve) (0x50e010): the rule picture loaded (0x458e00: the same
// name keeps it; one that does not load leaves none), the threshold tween from src to dst over the time
// along the curve (0x459090), the softness kept. The picture applies from then on, the tween running
// or not, until a transition names another (or none).
SQInteger transition(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& h = host(v);
    SpriteImage& im = *o->img;
    std::string rule = argString(v, 5);
    if (rule != im.transitionFile) {
        int w = 0, ht = 0;
        im.transitionFile = !rule.empty() && h.textureSize(rule, w, ht) ? rule : std::string();
    }
    im.transition.set({argFloat(v, 2), 0.0f, 0.0f}, {argFloat(v, 3), 0.0f, 0.0f}, argI(v, 6), argI(v, 7));
    im.transitionRate = argFloat(v, 4);
    return 0;
}
// setDither(on, x, y, file) (0x50eff0 -> CSprite 0x459d30; SQSpriteMask and SQSpriteRenderTarget the
// same): on, the pattern texture is the file if it is named and loads, else the graphics' default
// one (vtable +0x80); handed with the offset (x, y) to the Mware sprite drawing the image (the
// primitive +0xcc or the texture sprite +0xc8); off, none.
SQInteger setDither(HSQUIRRELVM v) {
    SPRITE_IMAGE
    SpriteImage& im = *o->img;
    im.dither = argI(v, 2) != 0;
    std::string file = argString(v, 5);
    int w = 0, ht = 0;
    im.ditherFile = im.dither && !file.empty() && host(v).textureSize(file, w, ht) ? file : std::string();
    im.ditherX = static_cast<float>(argI(v, 3));
    im.ditherY = static_cast<float>(argI(v, 4));
    return 0;
}

// setDisplacementMap(name) (SQSprite 0x50ef00, SQSpriteMask 0x5174a0, SQSpriteRenderTarget 0x51c2f0
// -> CSprite 0x459cb0): the map held before goes (and the texture sprite's); a named object's map
// (the task's +0xf8, an SQDisplacementMap's; the exe reads that field of whatever the name finds, so
// only maps are taken here) is held and handed to the texture sprite.
SQInteger setDisplacementMap(HSQUIRRELVM v) {
    SPRITE_IMAGE
    SpriteImage& im = *o->img;
    im.displacement.reset();
    std::string name = argString(v, 2);
    FrameTask* t = name.empty() ? nullptr : host(v).findTask(name);
    if (t) im.displacement = displacementMapOf(*t);
    return 0;
}

// isTransitioning() (0x50f870, CSprite +0x4c8): the threshold tween runs.
SQInteger isTransitioning(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img && o->img->transition.active ? 1 : 0);
}
SQInteger getTransitionFilename(HSQUIRRELVM v) {  // 0x50f920
    SPRITE_SELF
    std::string name = o->img ? o->img->transitionFile : std::string();
    sq_pushstring(v, name.c_str(), -1);
    return 1;
}
SQInteger getTransitionTimer(HSQUIRRELVM v) {  // 0x510d80
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->transition.timer : 0.0f);
}
// setTransitionTimer(ms) (0x5113d0 -> 0x45b5f0): for a sprite with a picture or a primitive.
SQInteger setTransitionTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    if (o->img->source == SpriteImage::Source::None) return 0;
    o->img->transition.setTimer(argFloat(v, 2));
    return 0;
}

// --- Beziers ---------------------------------------------------------------------------------------

// resetBezier() (0x50f500): the points dropped; a running path ends where it was going.
SQInteger resetBezier(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& b = o->img->bezier;
    b.points.clear();
    if (b.t.active) {
        b.t.active = 0;
        b.t.nowNoPower[0] = b.t.dst[0];
        b.t.now[0] = b.t.power * b.t.dst[0];
    }
    return 0;
}
// setBezier(x, y, z) (0x50f540 -> 0x45ec00): a control point added.
SQInteger setBezier(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->bezier.points.push_back({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)});
    return 0;
}
// beginBezier(time, curve) (0x50f5c0 -> CBezier +4, 0x45ecf0): with a whole number of four-point
// segments the path runs from 0 to 1 over the time along the curve, at the sprite's time scale, from
// its first point; otherwise the point is the last one given and nothing runs.
SQInteger beginBezier(HSQUIRRELVM v) {
    SPRITE_IMAGE
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
SQInteger getBezierTimer(HSQUIRRELVM v) {  // 0x511040
    SPRITE_SELF
    return pushFloat(v, o->img ? o->img->bezier.t.timer : 0.0f);
}
// setBezierTimer(ms) (0x511500 -> 0x45b9a0): the path's timer set, its point made the position.
SQInteger setBezierTimer(HSQUIRRELVM v) {
    SPRITE_IMAGE
    SpriteImage& im = *o->img;
    if (im.bezier.points.empty()) return 0;
    im.bezier.t.setTimer(argFloat(v, 2));
    im.bezier.evaluate();
    im.move.nowNoPower = im.bezier.point;
    for (size_t i = 0; i < 3; ++i) im.move.now[i] = im.bezier.point[i] * im.move.power;
    return 0;
}

// --- Blur ------------------------------------------------------------------------------------------

// setBlur(on, levels, power, interval, order, fade) (0x50f6c0 -> 0x45b240): the trail set afresh.
SQInteger setBlur(HSQUIRRELVM v) {
    SPRITE_IMAGE
    auto& b = o->img->blur;
    b.history.clear();
    b.levels = argI(v, 2) ? std::max(argI(v, 3), 0) : 0;
    b.power = argFloat(v, 4);
    b.interval = std::max(static_cast<uint32_t>(argI(v, 5)), 16u);
    b.order = argI(v, 6);
    b.fade = argI(v, 7);
    return 0;
}
SQInteger setBlurMode(HSQUIRRELVM v) {  // 0x50f6f0: +0x698 and +0x674
    SPRITE_IMAGE
    o->img->blur.mode = argI(v, 2);
    return 0;
}

// --- Shakes ----------------------------------------------------------------------------------------

// shake(start, end, frequency, interpolation, time, tempo, beatTempo) (0x50e0e0 -> CSprite 0x4592d0)
// and randomShake (0x50e110 -> 0x459380): a new shake replaces the old, with the image's time scale.
// start and end are Vector3D (SqPlus refuses anything else). Phantom of Inferno's (phantom.exe
// 0x140143700) takes only the five ints and starts and ends at SQMATH_V3_1 and SQMATH_V3_2.
SQInteger startShake(HSQUIRRELVM v, bool random) {
    SPRITE_SELF
    std::unique_ptr<Shake> s;
    SQInteger r = shakeFromArgs(v, random, o->img ? o->img->timeScale : 1.0f, s);
    if (r < 0) return r;
    if (o->img) o->img->shake = std::move(s);
    return 0;
}
SQInteger shake(HSQUIRRELVM v) { return startShake(v, false); }
SQInteger randomShake(HSQUIRRELVM v) { return startShake(v, true); }
// isShaking() (0x50f880): the shake's master timer runs.
SQInteger isShaking(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->img && o->img->shake && o->img->shake->shaking() ? 1 : 0);
}
SQInteger getShakeTimer(HSQUIRRELVM v) {  // 0x510da0
    SPRITE_SELF
    return pushFloat(v, o->img && o->img->shake ? o->img->shake->timer() : 0.0f);
}
// setShakeTimer(ms) (0x5113f0): within [0, the shake's time].
SQInteger setShakeTimer(HSQUIRRELVM v) {
    SPRITE_SELF
    if (!o->img || !o->img->shake) return 0;
    float t = argFloat(v, 2);
    float time = o->img->shake->time();
    o->img->shake->setTimer(t >= 0.0f ? (time < t ? time : t) : 0.0f);
    return 0;
}

}  // namespace

SQInteger shakeFromArgs(HSQUIRRELVM v, bool random, float timeScale, std::unique_ptr<Shake>& out) {
    auto& h = host(v);
    std::optional<std::array<float, 4>> start, end;
    SQInteger at = 4;  // the first int
    if (h.mathClasses) {
        start = argMath(v, 2, "Vector3D"), end = argMath(v, 3, "Vector3D");
        if (!start || !end) return sq_throwerror(v, "shake: start and end must be Vector3D");
    } else {
        start = std::array<float, 4>{h.mathVars[0], h.mathVars[1], h.mathVars[2], 0.0f};
        end = std::array<float, 4>{h.mathVars[3], h.mathVars[4], h.mathVars[5], 0.0f};
        at = 2;
    }
    // CRandom() seeds from timeGetTime (Fitted: the host's seed plus the frame clock's time).
    auto s = std::make_unique<Shake>(random, h.randomSeed + static_cast<int32_t>(h.clock.milliTime));
    s->set({(*start)[0], (*start)[1], (*start)[2]}, {(*end)[0], (*end)[1], (*end)[2]},
           static_cast<uint32_t>(argI(v, at)), argI(v, at + 1), static_cast<uint32_t>(argI(v, at + 2)), argI(v, at + 3),
           argI(v, at + 4));
    s->setTimeScale(timeScale);
    out = std::move(s);
    return 0;
}

namespace {

// --- Page turns ------------------------------------------------------------------------------------

// The page turn's state (SQSprite 0x510140 ff. -> CSprite +0xa94 ff.): no scene turns a page, but
// the saves read its timers. setPageAction(file, mode) (0x45b310) hands a texture to the material
// (kept by name); setPage<X>(value(s), time, curve) tween from where they are; beginPageAction(start,
// end, time, curve) (0x45b380) runs the page's move from start to end; getPageBackColor (0x45b460)
// puts r, g, b and 1 in SQMATH_RET_1..4; the timers answer 0 without an image (0x511100).
SQInteger setPageAction(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->pageActionFile = argString(v, 2);
    o->img->pageActionMode = argI(v, 3);
    return 0;
}
#define PAGE_TWEEN1(Set, Get, member)                                          \
    SQInteger Set(HSQUIRRELVM v) {                                             \
        SPRITE_IMAGE                                                           \
        o->img->member.moveTo({argFloat(v, 2), 0.0f, 0.0f}, argI(v, 3), argI(v, 4)); \
        return 0;                                                              \
    }                                                                          \
    SQInteger Get(HSQUIRRELVM v) {                                             \
        SPRITE_IMAGE                                                           \
        return pushFloat(v, o->img->member.now[0]);                            \
    }
PAGE_TWEEN1(setPageDirection, getPageDirection, pageDirection)
PAGE_TWEEN1(setPageRadius, getPageRadius, pageRadius)
PAGE_TWEEN1(setPageBackAlpha, getPageBackAlpha, pageBackAlpha)
#undef PAGE_TWEEN1
SQInteger setPageBackColor(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->pageBackColor.moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argI(v, 6));
    return 0;
}
SQInteger getPageBackColor(HSQUIRRELVM v) {
    SPRITE_IMAGE
    const auto& c = o->img->pageBackColor.now;
    host(v).setResult(c[0], c[1], c[2], 1.0f);
    return 0;
}
SQInteger beginPageAction(HSQUIRRELVM v) {
    SPRITE_IMAGE
    o->img->pageMove.set({argFloat(v, 2), 0.0f, 0.0f}, {argFloat(v, 3), 0.0f, 0.0f}, argI(v, 4), argI(v, 5));
    return 0;
}
SQInteger getPageMove(HSQUIRRELVM v) {
    SPRITE_IMAGE
    return pushFloat(v, o->img->pageMove.now[0]);
}
#define PAGE_TIMER(Get, Set, member)                                \
    SQInteger Get(HSQUIRRELVM v) {                                  \
        SPRITE_SELF                                                 \
        return pushFloat(v, o->img ? o->img->member.timer : 0.0f);  \
    }                                                               \
    SQInteger Set(HSQUIRRELVM v) {                                  \
        SPRITE_IMAGE                                                \
        o->img->member.setTimer(argFloat(v, 2));                    \
        return 0;                                                   \
    }
PAGE_TIMER(getPageDirectionTimer, setPageDirectionTimer, pageDirection)
PAGE_TIMER(getPageActionTimer, setPageActionTimer, pageMove)
PAGE_TIMER(getPageRadiusTimer, setPageRadiusTimer, pageRadius)
PAGE_TIMER(getPageBackColorTimer, setPageBackColorTimer, pageBackColor)
PAGE_TIMER(getPageBackAlphaTimer, setPageBackAlphaTimer, pageBackAlpha)
#undef PAGE_TIMER

// --- Totono's flags -------------------------------------------------------------------------------

SQInteger setLock(HSQUIRRELVM v) {
    SPRITE_SELF
    o->lock = argI(v, 2);
    return 0;
}
SQInteger getLock(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->lock);
}
SQInteger setDisused(HSQUIRRELVM v) {
    SPRITE_SELF
    o->disused = argI(v, 2);
    return 0;
}
SQInteger getDisused(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->disused);
}
SQInteger setHidable(HSQUIRRELVM v) {
    SPRITE_SELF
    o->hidable = argI(v, 2);
    return 0;
}
SQInteger getHidable(HSQUIRRELVM v) {
    SPRITE_SELF
    return pushInt(v, o->hidable);
}

}  // namespace

// serialize(file) / deserialize(file): the object's engine state in the save the scripts opened
// under that name (natives_vmstate.cpp); 1 if done.
EngineRecord engineRecordKind(const SpriteObject& o) {
    if (o.kind == SpriteObject::Kind::Mask) return EngineRecord::Mask;
    if (o.kind == SpriteObject::Kind::Target) return EngineRecord::Target;
    return EngineRecord::Sprite;
}
EngineState engineState(SpriteObject& o) {
    EngineState st;
    st.hasImage = o.img != nullptr;
    st.blur = o.img ? &o.img->blur : nullptr;
    st.shake = o.img ? &o.img->shake : nullptr;
    st.capture = &o.capture;
    if (o.img && o.img->source == SpriteImage::Source::Screen) {
        st.screen = o.img->screenId;
        st.picture = &o.img->screenPicture;
    }
    return st;
}
SQInteger serializeSprite(HSQUIRRELVM v) {
    SPRITE_SELF
    bool ok = writeEngineRecord(*o->host, argString(v, 2), engineRecordKind(*o), engineState(*o));
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}
SQInteger deserializeSprite(HSQUIRRELVM v) {
    SPRITE_SELF
    bool ok = readEngineRecord(*o->host, argString(v, 2), engineRecordKind(*o), engineState(*o));
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}

ClassNatives spriteNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {
        {"setSurface", setSurface},
        {"setMask", setMask},
        {"create", create},
        {"createColor", createColor},
        {"createScreen", createScreen},
        {"createMovie", createMovie},
        {"link", link},
        {"setPriority", setPriority},
        {"getPriority", getPriority},
        {"setErase", setErase},
        {"getErase", getErase},
        {"fade", fade},
        {"getFade", getFade},
        {"isFading", isFading},
        {"getFadeTimer", getFadeTimer},
        {"setFadeTimer", setFadeTimer},
        {"move", move},
        {"move3D", move3D},
        {"getMove", getMove},
        {"isMoving", isMoving},
        {"getMoveTimer", getMoveTimer},
        {"setMoveTimer", setMoveTimer},
        {"moveVertex", moveVertex},
        {"getVertexMove", getVertexMove},
        {"getMoveVertexTimer", getMoveVertexTimer},
        {"setMoveVertexTimer", setMoveVertexTimer},
        {"scale", scale},
        {"getScale", getScale},
        {"isScaling", isScaling},
        {"getScaleTimer", getScaleTimer},
        {"setScaleTimer", setScaleTimer},
        {"rotate", rotate},
        {"getRotate", getRotate},
        {"isRotating", isRotating},
        {"getRotateTimer", getRotateTimer},
        {"setRotateTimer", setRotateTimer},
        {"scroll", scroll},
        {"getScroll", getScroll},
        {"getScrollTimer", getScrollTimer},
        {"setScrollTimer", setScrollTimer},
        {"setColor", setColor},
        {"getColor", getColor},
        {"getColorTimer", getColorTimer},
        {"setColorTimer", setColorTimer},
        {"setColorBalance", setColorBalance},
        {"getColorBalance", getColorBalance},
        {"getColorBalanceTimer", getColorBalanceTimer},
        {"setColorBalanceTimer", setColorBalanceTimer},
        {"setColorHue", setColorHue},
        {"getColorHue", getColorHue},
        {"getColorHueTimer", getColorHueTimer},
        {"setColorHueTimer", setColorHueTimer},
        {"setColorSaturation", setColorSaturation},
        {"getColorSaturation", getColorSaturation},
        {"getColorSaturationTimer", getColorSaturationTimer},
        {"setColorSaturationTimer", setColorSaturationTimer},
        {"setColorBrightness", setColorBrightness},
        {"getColorBrightness", getColorBrightness},
        {"getColorBrightnessTimer", getColorBrightnessTimer},
        {"setColorBrightnessTimer", setColorBrightnessTimer},
        {"setColorGlow", setColorGlow},
        {"getColorGlow", getColorGlow},
        {"getColorGlowTimer", getColorGlowTimer},
        {"setColorGlowTimer", setColorGlowTimer},
        {"setNegaposiPower", setNegaposiPower},
        {"getNegaposiPower", getNegaposiPower},
        {"getNegaposiPowerTimer", getNegaposiPowerTimer},
        {"setNegaposiPowerTimer", setNegaposiPowerTimer},
        {"setSepiaPower", setSepiaPower},
        {"getSepiaPower", getSepiaPower},
        {"getSepiaPowerTimer", getSepiaPowerTimer},
        {"setSepiaPowerTimer", setSepiaPowerTimer},
        {"setRadialBlur", setRadialBlur},
        {"getRadialBlurPower", getRadialBlurPower},
        {"getRadialBlurCenter", getRadialBlurCenter},
        {"getRadialBlurTimer", getRadialBlurTimer},
        {"setRadialBlurTimer", setRadialBlurTimer},
        {"setDirectionalBlur", setDirectionalBlur},
        {"getDirectionalBlurPower", getDirectionalBlurPower},
        {"getDirectionalBlurDirection", getDirectionalBlurDirection},
        {"getDirectionalBlurTimer", getDirectionalBlurTimer},
        {"setDirectionalBlurTimer", setDirectionalBlurTimer},
        {"setRipple", setRipple},
        {"getRippleCenter", getRippleCenter},
        {"getRipplePower", getRipplePower},
        {"getRippleFrequency", getRippleFrequency},
        {"getRippleSpeed", getRippleSpeed},
        {"getRipplePhase", getRipplePhase},
        {"setRipplePhase", setRipplePhase},
        {"getRippleTimer", getRippleTimer},
        {"setRippleTimer", setRippleTimer},
        {"setRaster", setRaster},
        {"getRasterRotate", getRasterRotate},
        {"getRasterPower", getRasterPower},
        {"getRasterFrequency", getRasterFrequency},
        {"getRasterSpeed", getRasterSpeed},
        {"getRasterPhase", getRasterPhase},
        {"setRasterPhase", setRasterPhase},
        {"getRasterTimer", getRasterTimer},
        {"setRasterTimer", setRasterTimer},
        {"setLens", setLens},
        {"getLensCenter", getLensCenter},
        {"getLensPower", getLensPower},
        {"getLensRadius", getLensRadius},
        {"getLensTimer", getLensTimer},
        {"setLensTimer", setLensTimer},
        {"resetSpline", resetSpline},
        {"setSpline", setSpline},
        {"beginSpline", beginSpline},
        {"splitFace", splitFace},
        {"clearSplitAction", clearSplitAction},
        {"pushSplitAction", pushSplitAction},
        {"beginSplitAction", beginSplitAction},
        {"getSplitActionTimer", getSplitActionTimer},
        {"setSplitActionTimer", setSplitActionTimer},
        {"getSplineTimer", getSplineTimer},
        {"setSplineTimer", setSplineTimer},
        {"setCamera", setCamera},
        {"setShadingParameter", setShadingParameter},
        {"getShadingLevel", getShadingLevel},
        {"getShadingDensity", getShadingDensity},
        {"setShadingPower", setShadingPower},
        {"getShadingPower", getShadingPower},
        {"getShadingPowerTimer", getShadingPowerTimer},
        {"setShadingPowerTimer", setShadingPowerTimer},
        {"setMonochromePower", setMonochromePower},
        {"getMonochromePower", getMonochromePowerOnly},
        {"getMonochromeCb", getMonochromeCb},
        {"getMonochromeCr", getMonochromeCr},
        {"getMonochromePowerTimer", getMonochromePowerTimer},
        {"setMonochromePowerTimer", setMonochromePowerTimer},
        {"setColorLevel", setColorLevel},
        {"getColorLevelMin", getColorLevelMin},
        {"getColorLevelMax", getColorLevelMax},
        {"getColorLevelPower", getColorLevelPower},
        {"getColorLevelTimer", getColorLevelTimer},
        {"setColorLevelTimer", setColorLevelTimer},
        {"setVolume", setVolume},
        {"getVolume", getVolume},
        {"getVolumeTimer", getVolumeTimer},
        {"setVolumeTimer", setVolumeTimer},
        {"setCenter", setCenter},
        {"setPivot", setPivot},
        {"getSize", getSize},
        {"setBlendMode", setBlendMode},
        {"setTimeScale", setTimeScale},
        {"setActionCompatibleMode", setActionCompatibleMode},
        {"setTextureFiltering", setTextureFiltering},
        {"setClickable", setClickable},
        {"setChangeCursor", setChangeCursor},
        {"addSoundGroup", addSoundGroup},
        {"setVolumeControlFadeTime", setVolumeControlFadeTime},
        {"isOver", isOver},
        {"isDown", isDown},
        {"isUp", isUp},
        {"setUVOffset", setUVOffset},
        {"serialize", serializeSprite},
        {"deserialize", deserializeSprite},
        {"setCapture", setCaptureScreen},
        {"isCaptureEnabled", isCaptureEnabledScreen},
        {"saveImage", saveImage},
        {"getUVOffset", getUVOffset},
        {"isDeletedTexture", isDeletedTexture},
        {"resetBezier", resetBezier},
        {"setBezier", setBezier},
        {"beginBezier", beginBezier},
        {"getBezierTimer", getBezierTimer},
        {"setBezierTimer", setBezierTimer},
        {"setBlur", setBlur},
        {"setBlurMode", setBlurMode},
        {"transition", transition},
        {"setDither", setDither},
        {"setDisplacementMap", setDisplacementMap},
        {"createSubresource", createSubresource},
        {"createScene", createScene},
        {"isTransitioning", isTransitioning},
        {"getTransitionFilename", getTransitionFilename},
        {"getTransitionTimer", getTransitionTimer},
        {"setTransitionTimer", setTransitionTimer},
        {"shake", shake},
        {"randomShake", randomShake},
        {"isShaking", isShaking},
        {"getShakeTimer", getShakeTimer},
        {"setShakeTimer", setShakeTimer},
        {"setPageAction", setPageAction},
        {"setPageDirection", setPageDirection},
        {"setPageRadius", setPageRadius},
        {"setPageBackColor", setPageBackColor},
        {"setPageBackAlpha", setPageBackAlpha},
        {"beginPageAction", beginPageAction},
        {"getPageDirection", getPageDirection},
        {"getPageMove", getPageMove},
        {"getPageRadius", getPageRadius},
        {"getPageBackColor", getPageBackColor},
        {"getPageBackAlpha", getPageBackAlpha},
        {"getPageDirectionTimer", getPageDirectionTimer},
        {"getPageRadiusTimer", getPageRadiusTimer},
        {"getPageBackColorTimer", getPageBackColorTimer},
        {"getPageBackAlphaTimer", getPageBackAlphaTimer},
        {"getPageActionTimer", getPageActionTimer},
        {"setPageDirectionTimer", setPageDirectionTimer},
        {"setPageRadiusTimer", setPageRadiusTimer},
        {"setPageBackColorTimer", setPageBackColorTimer},
        {"setPageBackAlphaTimer", setPageBackAlphaTimer},
        {"setPageActionTimer", setPageActionTimer},
        {"play", play},
        {"stop", stop},
        {"pause", pause},
        {"onResume", onResume},
        {"isPlaying", isPlaying},
        {"isPaused", isPaused},
        {"setLoop", setLoop},
        {"seek", seek},
        {"getDurationTime", getDurationTime},
        {"getTotalTime", getDurationTime},
        {"getRemainTime", getRemainTime},
        {"setLock", setLock},
        {"getLock", getLock},
        {"setDisused", setDisused},
        {"getDisused", getDisused},
        {"setHidable", setHidable},
        {"getHidable", getHidable},
    };
    return c;
}

ClassNatives spriteMaskNatives() {
    ClassNatives c = spriteNatives();
    c.make = makeMask;
    c.methods.push_back({"setDiffuseOnly", setDiffuseOnly});
    c.methods.push_back({"setInverseAlpha", setInverseAlpha});
    return c;
}

ClassNatives spriteRenderTargetNatives() {
    ClassNatives c = spriteNatives();
    c.make = makeTarget;
    for (auto& m : c.methods) {
        if (std::string(m.name) == "create") m.fn = createTarget;
    }
    for (auto& m : c.methods) {
        if (std::string(m.name) == "setCapture") m.fn = setCapture;
        if (std::string(m.name) == "isCaptureEnabled") m.fn = isCaptureEnabled;
    }
    c.methods.push_back({"setCopyTarget", setCopyTarget});
    c.methods.push_back({"setCaptureTarget", setCaptureTarget});
    c.methods.push_back({"setDelayRendering", setDelayRendering});
    c.methods.push_back({"isDelayedRendering", isDelayedRendering});
    c.methods.push_back({"clear", clearTarget});
    c.methods.push_back({"createCustomEffect", createCustomEffect});
    c.methods.push_back({"setCustomEffectParameter", setCustomEffectParameter});
    return c;
}

std::shared_ptr<const DisplacementMap> displacementMapOf(FrameTask& task) {
    auto* m = dynamic_cast<SpriteObject*>(&task);
    if (m && m->kind == SpriteObject::Kind::Displacement && m->img) return m->map;
    return nullptr;
}

ClassNatives displacementMapNatives() {
    ClassNatives c;
    c.make = makeDisplacementMap;
    for (const auto& m : spriteNatives().methods) {
        std::string n = m.name;
        if (n == "setPriority" || n == "getPriority" || n == "setErase" || n == "getErase" || n == "setLock" ||
            n == "getLock" || n == "setDisused" || n == "getDisused" || n == "setHidable" || n == "getHidable") {
            c.methods.push_back(m);
        }
    }
    c.methods.push_back({"create", createDisplacementMap});
    c.methods.push_back({"setSurface", setSurfaceDisplacementMap});
    c.methods.push_back({"getSize", getSizeDisplacementMap});
    return c;
}

// The draw list (CTask +0xc0).

void ScriptHost::Impl::addDrawable(Drawable* d, int32_t priority, uint32_t created) {
    DrawEntry e{created, priority, d};
    auto at = std::upper_bound(drawList.begin(), drawList.end(), e, [](const DrawEntry& a, const DrawEntry& b) {
        return a.priority < b.priority || (a.priority == b.priority && a.created < b.created);
    });
    drawList.insert(at, e);
}

void ScriptHost::Impl::removeDrawable(Drawable* d) {
    std::erase_if(drawList, [d](const DrawEntry& e) { return e.object == d; });
}

void ScriptHost::Impl::setDrawPriority(Drawable* d, int32_t priority) {
    for (auto it = drawList.begin(); it != drawList.end(); ++it) {
        if (it->object != d) continue;
        if (it->priority == priority) return;
        DrawEntry e = *it;
        drawList.erase(it);
        addDrawable(d, priority, e.created);
        return;
    }
}

int32_t ScriptHost::Impl::drawPriority(const Drawable* d) const {
    for (const auto& e : drawList) {
        if (e.object == d) return e.priority;
    }
    return 0x7fffffff;
}

const ScriptHost::Impl::AlphaPlane* ScriptHost::Impl::textureAlpha(const std::string& path) {
    auto it = textureAlphas.find(path);
    if (it == textureAlphas.end()) {
        std::shared_ptr<AlphaPlane> plane;
        if (auto data = readAnyFile(path)) {
            try {
                Image img = decodeImage(*data);
                plane = std::make_shared<AlphaPlane>();
                plane->width = img.width;
                plane->height = img.height;
                plane->alpha.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
                for (size_t i = 0; i < plane->alpha.size(); ++i) plane->alpha[i] = img.rgba[i * 4 + 3];
            } catch (const std::exception& e) {
                if (log) log("texture " + path + ": " + e.what());
                plane.reset();
            }
        }
        it = textureAlphas.emplace(path, std::move(plane)).first;
    }
    return it->second.get();
}

namespace {

bool sameFrame(const SpriteImage::BlurFrame& a, const SpriteImage::BlurFrame& b) {
    return a.quad.corner == b.quad.corner && a.rgb == b.rgb;
}

}  // namespace

// A sprite with a trail (0x4a4160, 0x4a4320, 0x456400): this frame's quad joins the history (the
// oldest leaves once it holds more than (levels + 1 for mode 0) × interval × 0.06 frames); the copies
// drawn are those `levels` × interval ms back and nearer (mode 1: the oldest kept when the history
// is short, copies like this frame dropped; mode 0: those it has, the oldest frame first if short,
// copies like their next dropped), with this frame first and the copies in age order after it
// (order 0) or the oldest first and this frame last. Each copy keeps its corner colours with the
// alpha the fade's times power (times 1 − its place / their number with `fade`); this frame keeps
// its own.
void ScriptHost::Impl::emitBlur(SpriteImage& im, const DrawItem& d, std::vector<DrawItem>& out) {
    auto& b = im.blur;
    SpriteImage::BlurFrame now;
    now.quad = d.quad;
    for (size_t i = 0; i < 4; ++i) now.rgb[i] = {d.color[i][0], d.color[i][1], d.color[i][2]};
    b.history.push_back(now);
    if (b.history.size() > blurHistoryLimit(b) && b.history.size() > 1) b.history.pop_front();
    size_t size = b.history.size();
    auto pick = blurPicks(b, size, [&b](size_t i, size_t j) { return sameFrame(b.history[i], b.history[j]); });
    float alpha = im.fade.now[0] * im.fade.power;
    for (size_t i = 0; i < pick.size(); ++i) {
        DrawItem copy = d;
        size_t idx = pick[i];
        if (idx != size - 1) {
            float a = std::clamp(alpha * blurWeight(b, i, pick.size()), 0.0f, 1.0f);
            copy.quad = b.history[idx].quad;
            for (size_t c = 0; c < 4; ++c) {
                copy.color[c] = {b.history[idx].rgb[c][0], b.history[idx].rgb[c][1], b.history[idx].rgb[c][2], a};
            }
        }
        out.push_back(std::move(copy));
    }
}

// What the material holds for the texture filters after the update handed it the tweens' values
// (0x453280; the material's setters 0x10023970 ff.: the powers scaled, the direction normalised).
static TextureFilter textureFilter(const SpriteImage& im) {
    TextureFilter f;
    f.radialCenter = im.radialBlurCenter;
    f.radialPower = im.radialBlurPower.now[0] * 10.0f;
    float dx = im.directionalBlurDirection[0], dy = im.directionalBlurDirection[1];
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.0f) f.directional = {dx / len, dy / len};
    f.directionalPower = im.directionalBlurPower.now[0] * 3.0f;
    f.rippleCenter = im.rippleCenter;
    f.ripplePhase = im.ripplePhase;
    f.rippleFrequency = im.rippleFrequency.now[0];
    f.ripplePower = im.ripplePower.now[0];
    f.rasterRotate = im.rasterRotate.now[0];
    f.rasterPhase = im.rasterPhase;
    f.rasterFrequency = im.rasterFrequency.now[0];
    f.rasterPower = im.rasterPower.now[0];
    f.lensCenter = {im.lensCenter.now[0], im.lensCenter.now[1]};
    f.lensRadius = im.lensRadius.now[0];
    f.lensDistance = im.lensPower.now[0];
    // 0x10023000 wraps when the UV offset (+0xa0, the scroll added into it by the update) or the pair at
    // +0x154 is non-zero; no CSprite or SQSprite code sets that pair (only the constructor's 0).
    f.wrap = im.uvOffsetX != 0.0f || im.uvOffsetY != 0.0f;
    // The colour filters: the tweens' values (0x453280 hands them on: material +0x14 .. +0x58).
    f.shadingPower = std::clamp(im.shading.now[0], 0.0f, 1.0f);
    f.shadingLevel = im.shadingLevel;
    f.shadingDensity = im.shadingDensity * 256.0f;
    f.negaposi = im.negaposi.now[0];
    f.monochrome = im.monochrome.now;
    f.sepia = im.sepia.now[0];
    f.colorLevel = im.colorLevel.now;
    f.hsv = {im.colorHue.now[0], im.colorSaturation.now[0], im.colorBrightness.now[0], im.colorGlow.now[0]};
    return f;
}

// The draw (CTask's draw list, 0x42e7e0; the passes 0x4861b0): each object not erased lays its quad
// out where the frame shows it, on the target it draws into (a render target, a mask's texture or the
// screen: its quads are in that target's pixels); the hit tests use these quads until the next draw.
// Then the frame's render targets and masks, for the renderer to draw first.
void ScriptHost::Impl::layout() {
    drawItems.clear();
    renderTargets.clear();
    int sw = screenWidth(), sh = screenHeight();
    // The targets' sizes: a render target's own; a mask's, that of the target it is on (0x4a6ce0).
    std::map<uint32_t, std::array<int, 2>> sizes;
    std::map<uint32_t, bool> inverse;
    for (const auto& e : drawList) {
        uint32_t id = e.object->targetId();
        if (id && !e.object->isMask()) sizes[id] = {e.object->image()->srcW, e.object->image()->srcH};
    }
    for (const auto& e : drawList) {
        uint32_t id = e.object->targetId();
        if (!id || !e.object->isMask()) continue;
        auto on = sizes.find(e.object->surface);
        sizes[id] = on != sizes.end() ? on->second : std::array<int, 2>{sw, sh};
        inverse[id] = static_cast<const SpriteObject*>(static_cast<const Drawable*>(e.object))->inverseAlpha != 0;
    }
    for (const auto& e : drawList) {
        Drawable* obj = e.object;
        // A mask draws into its own texture; others into their surface (gone: nowhere seen).
        uint32_t target = obj->isMask() ? obj->targetId() : obj->surface;
        int tw = sw, th = sh;
        bool drawn = true;
        if (target) {
            auto it = sizes.find(target);
            if (it == sizes.end()) drawn = false;
            else tw = it->second[0], th = it->second[1];
        }
        layoutWidth = tw, layoutHeight = th;
        size_t first = drawItems.size();
        SpriteImage* im = obj->image();
        // A displacement map's picture is not drawn (its target's texture sprite is never made).
        auto* so = dynamic_cast<SpriteObject*>(obj);
        const bool shown = !so || so->kind != SpriteObject::Kind::Displacement;
        if (!im) {
            if (!obj->erase && drawn) obj->emit(*this, drawItems);
        } else if (!drawn || !shown) {
            im->drawnQuad.reset();
        } else if (obj->erase) {
            // An erased sprite is not drawn, but a clickable one still has its vertex build run
            // (SQSprite's draw: muramasa_en.exe 0x50d2c0, totono_en.exe 0x4defc0), so its hit tests
            // go on: Muramasa's quick menu opens over an erased strip at the screen's right edge.
            if (im->clickable) im->drawnQuad = spriteQuad(*im, tw, th);
            else im->drawnQuad.reset();
        } else {
            im->drawnQuad = spriteQuad(*im, tw, th);
            if (im->drawnQuad && im->source != SpriteImage::Source::None && im->source != SpriteImage::Source::Scene) {
                DrawItem d;
                switch (im->source) {
                case SpriteImage::Source::Color: d.kind = DrawItem::Kind::Color; break;
                case SpriteImage::Source::Screen: d.kind = DrawItem::Kind::Screen; break;
                case SpriteImage::Source::Movie: d.kind = DrawItem::Kind::Movie; break;
                case SpriteImage::Source::Target: d.kind = DrawItem::Kind::Target; break;
                default: d.kind = DrawItem::Kind::Texture; break;
                }
                d.texture = im->path;
                d.picture = im->picture;
                if (im->subresource) d.subresource = true, d.subRect = *im->subresource;
                d.textureWidth = im->textureWidth;
                d.textureHeight = im->textureHeight;
                d.srcX = im->srcX, d.srcY = im->srcY, d.srcW = im->srcW, d.srcH = im->srcH;
                d.quad = *im->drawnQuad;
                // The alpha is the fade's (now times power: 0x456400); the corners' colours clamp to 0..1.
                float alpha = std::clamp(im->fade.now[0] * im->fade.power, 0.0f, 1.0f);
                for (size_t i = 0; i < 4; ++i) {
                    for (size_t c = 0; c < 3; ++c) d.color[i][c] = std::clamp(im->color[i].now[c], 0.0f, 1.0f);
                    d.color[i][3] = alpha;
                }
                d.blendMode = im->blendMode;
                d.filtering = im->textureFiltering != 0;
                d.uvOffsetX = im->uvOffsetX;
                d.uvOffsetY = im->uvOffsetY;
                if (d.kind != DrawItem::Kind::Color) d.filter = textureFilter(*im);
                if (im->source == SpriteImage::Source::Screen) {
                    d.screen = im->screenId;
                    auto* so = dynamic_cast<SpriteObject*>(obj);
                    d.capture = !so || (so->screenCapture != 0 && so->capture != 0);
                    d.screenPicture = im->screenPicture;
                }
                // The material gets the threshold (now, clamped to 0..1: CSprite2D 0x10022730) and the
                // softness whenever they change; a sprite without a rule picture has no transition.
                if (!im->transitionFile.empty()) {
                    d.transitionRule = im->transitionFile;
                    d.transitionNow = std::clamp(im->transition.now[0], 0.0f, 1.0f);
                    d.transitionRate = im->transitionRate;
                }
                d.dither = im->dither;
                d.ditherTexture = im->ditherFile;
                d.ditherX = im->ditherX;
                d.ditherY = im->ditherY;
                if (im->displacement && d.kind != DrawItem::Kind::Color) {
                    d.displacementMap = im->displacement->target;
                    d.displacementScaleX = im->displacement->scaleX;
                    d.displacementScaleY = im->displacement->scaleY;
                }
                if (im->source == SpriteImage::Source::Movie) {
                    d.movie = im->movieId;
                    d.movieTime = im->movieTime;
                    d.moviePass = im->moviePass;
                    d.movieAlpha = im->movieAlpha;
                }
                if (im->source == SpriteImage::Source::Target) d.renderTarget = im->targetId;
                if (!im->split.pieces.empty()) {
                    // A split sprite draws its pieces (0x456400's split branch): each corner's
                    // colour the corners' colours mixed by where it is in the picture.
                    float w = static_cast<float>(im->srcW), h = static_cast<float>(im->srcH);
                    for (const auto& pq : splitPieceQuads(*im, tw, th)) {
                        DrawItem p = d;
                        p.quad = pq.quad;
                        p.partUv = true;
                        for (size_t k = 0; k < 4; ++k) {
                            float u = w > 0.0f ? pq.pixel[k][0] / w : 0.0f, v = h > 0.0f ? pq.pixel[k][1] / h : 0.0f;
                            p.uv[k] = {u, v};
                            for (size_t c = 0; c < 3; ++c) {
                                float left = (im->color[2].now[c] - im->color[0].now[c]) * v + im->color[0].now[c];
                                float right = (im->color[3].now[c] - im->color[1].now[c]) * v + im->color[1].now[c];
                                p.color[k][c] = std::clamp((right - left) * u + left, 0.0f, 1.0f);
                            }
                        }
                        drawItems.push_back(std::move(p));
                    }
                } else if (im->blur.levels > 0) {
                    emitBlur(*im, d, drawItems);
                } else {
                    im->blur.history.clear();
                    drawItems.push_back(std::move(d));
                }
            }
        }
        // Everything the object drew goes to its target, through its mask.
        auto m = obj->mask ? inverse.find(obj->mask) : inverse.end();
        for (size_t i = first; i < drawItems.size(); ++i) {
            drawItems[i].target = target;
            if (m != inverse.end()) {
                drawItems[i].mask = obj->mask;
                drawItems[i].maskInverse = m->second;
            }
        }
    }
    // The targets: a render target resets each frame while capturing without a copy source (CSprite
    // +0xcf8 clear), its picture kept otherwise; clear() clears it once (0x51bc10). A mask redraws
    // (cleared) while it is drawn (0x4a6970).
    std::set<uint32_t> listed;
    for (const auto& e : drawList) {
        uint32_t id = e.object->targetId();
        if (!id || !listed.insert(id).second) continue;  // two targets under one name share one
        auto* o = static_cast<SpriteObject*>(e.object);
        RenderTarget t;
        t.id = id;
        t.width = sizes[id][0];
        t.height = sizes[id][1];
        t.mask = o->isMask();
        if (t.mask) {
            t.draw = !o->erase;
            t.reset = t.draw;
        } else {
            t.draw = o->capture != 0;
            t.delayed = o->delayRendering != 0;
            t.copyFrom = o->capture ? o->copyFrom : 0;
            t.reset = o->capture && !o->copyFrom;
            t.clear = o->clearRequested && !o->erase;
            t.clearColor = o->clearColor;
            if (t.clear) o->clearRequested = false;
        }
        renderTargets.push_back(t);
    }
    // The app's screen fade over everything, while its alpha is above 0 (0x42e7e0); a capture asked
    // for is taken by this draw whether or not it shows.
    auto& f = screenFade;
    bool take = f.captureRequested;
    if (take) {
        f.captureRequested = false;
        f.colour = false;
        if (!f.screenId) f.screenId = nextScreenId++;
    }
    if (f.fade.now[0] > 0.0f && (f.colour || f.screenId)) {
        DrawItem d;
        d.kind = f.colour ? DrawItem::Kind::Color : DrawItem::Kind::Screen;
        d.screen = f.colour ? 0 : f.screenId;
        d.capture = false;
        d.takeLastFrame = take && !f.colour;
        d.textureWidth = d.srcW = sw;
        d.textureHeight = d.srcH = sh;
        const float w = static_cast<float>(sw), hgt = static_cast<float>(sh);
        d.quad.corner = {{{0.0f, 0.0f, 1.0f}, {w, 0.0f, 1.0f}, {0.0f, hgt, 1.0f}, {w, hgt, 1.0f}}};
        float alpha = std::clamp(f.fade.now[0] * f.fade.power, 0.0f, 1.0f);
        for (auto& c : d.color) c = {f.colour ? f.rgb[0] : 1.0f, f.colour ? f.rgb[1] : 1.0f, f.colour ? f.rgb[2] : 1.0f, alpha};
        d.blendMode = 1;
        drawItems.push_back(std::move(d));
    }
}

bool ScriptHost::Impl::textureSize(const std::string& path, int& w, int& h) {
    auto it = textureSizes.find(path);
    if (it == textureSizes.end()) {
        auto data = readAnyFile(path);
        int tw = 0, th = 0;
        if (!data || !nyan::diesel::imageSize(*data, tw, th)) {
            note(missing, path);
            return false;
        }
        it = textureSizes.emplace(path, std::make_pair(tw, th)).first;
    }
    w = it->second.first;
    h = it->second.second;
    return true;
}

}  // namespace nyan::diesel
