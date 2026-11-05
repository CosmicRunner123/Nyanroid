#include "nyan/wa2/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// Slots and objects (WA2.exe RT mode).
constexpr int kBgSlot = 0x71;        // + buffer
constexpr int kMaskSlot = 0x73;
constexpr int kCharSlot = 0x75;      // + 2i + k
constexpr int kTargetSlot = 0x105;   // + buffer
constexpr int kBgObj = 0x371;        // + buffer
constexpr int kCharObj = 0x379;      // + 2i + k
constexpr int kDisplayObj = 0x409;   // + buffer
constexpr int kNoChar = 8;

// The character file prefixes (0x415920; strings 0x4a2904..0x4a2988).
constexpr const char* kPrefix[38] = {"har", "kaz", "set", "koh", "izu", "mar", "a",   "a",   "a",   "a",
                                     "tak", "ioo", "chi", "pap", "mam", "oto", "you", "tan", "shi", "tom",
                                     "sat", "hon", "nak", "say", "aco", "mih", "mhh", "ueh", "yos", "tan",
                                     "ham", "mat", "kiz", "suz", "saw", "miy", "yan", "mas"};
// Position offsets of the sprite centre from the screen centre (0x4be0bc).
constexpr int kPosition[11] = {-288, 0, 288, -384, 384, -480, 480, -480, -160, 160, 480};
// The time of day's colour table for characters (0x4be328).
constexpr const char* kTimeAmp[4] = {nullptr, "evening.amp", "night.amp", "indoor.AMP"};

// 0x401920.
float ease(int type, float t) {
    switch (type) {
    case 1: return t * t;
    case 2: return 1.0f - (1.0f - t) * (1.0f - t);
    case 3:
        if (t >= 0.5f) {
            float f = 1.0f - ((t - 0.5f) + (t - 0.5f));
            return (1.0f - f * f) * 0.5f + 0.5f;
        }
        return (t + t) * (t + t) * 0.5f;
    case 4:
        if (t < 0.5f) {
            float f = 1.0f - (t + t);
            return (1.0f - f * f) * 0.5f;
        } else {
            float f = (t - 0.5f) + (t - 0.5f);
            return f * f * 0.5f + 0.5f;
        }
    default: return t;
    }
}

std::string format(const char* fmt, int a, int b = 0, int c = 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, a, b, c);
    return buf;
}

int ftol(float f) {
    return static_cast<int>(f);
}

}  // namespace

Scene::Scene(Game& game) : game_(game), gfx_(game.gfx()) {}

void Scene::begin() {
    // 0x413c10's first frame: the system pictures (0x40a3a0 and 0x1ba belong to the message window) and
    // the scene buffers.
    gfx_.load(0x1ba, "bitmap.tga", 4, false, "", "", 0x100, "");
    gfx_.makeTarget(kTargetSlot, 0x800, 0x400);
    gfx_.makeTarget(kTargetSlot + 1, 0x800, 0x400);
}

void Scene::reset() {
    // From 0x413ac0: no pictures, characters or transitions; colour settings, the tone, the global
    // tint, and object 1, a black full-screen quad.
    chars_ = {};
    buf_ = {};
    cur_ = 0;
    shown_ = false;
    lastNumber_ = lastKind_ = keepChars_ = 0;
    windowFade_ = 0;
    transState_ = transType_ = transCounter_ = transDuration_ = transSoft_ = 0;
    picW_ = picH_ = 0;
    srcW_ = 1280;
    srcH_ = 720;
    bgAmp_.clear();
    cameraPaused_ = false;
    pauseLength_ = pauseCounter_ = 0;
    toneOn_ = false;
    toneMode_ = toneIn_ = toneOut_ = toneCounter_ = 0;
    toneTarget_ = {128, 128, 128};
    cBusyFlag = false;
    shakeX = shakeY = shakeRotation = 0;
    shakeScale = 1;
    windowShown = false;
    bgEffect.clear();
    charEffect.clear();
    bgLevel = charLevel = 0x100;
    timeMode = 0;
    toneStart_ = toneCur_ = {128, 128, 128};
    gfx_.setTint(0x80, 0x80, 0x80, 0);
    gfx_.makePolygon(1, 4, 0, true);
    gfx_.setDest(1, 0, 0, 1280, 720);
    gfx_.setColor(1, 0xff, 0, 0, 0);
}

void Scene::setChroma(int level, int which) {
    if (which == 0) bgLevel = charLevel = level;
    else if (which == 1) bgLevel = level;
    else if (which == 2) charLevel = level;
}

void Scene::setEffect(const std::string& name, int which) {
    if (which == 0) bgEffect = charEffect = name;
    else if (which == 1) bgEffect = name;
    else if (which == 2) charEffect = name;
}

std::string Scene::characterAmp() const {
    if (game_.assets().exists("grp", bgAmp_)) return bgAmp_;
    const char* t = kTimeAmp[timeMode & 3];
    return t ? t : "";
}

// ---- Backgrounds -------------------------------------------------------------------------------------

void Scene::show(int num, int trans, int keepChars, int kind, int frames, int soft, int x, int y, float zx,
                 float zy, float rot, int raw) {
    // 0x416650
    game_.fr(frames);
    cur_ = cur_ == 0 ? 1 : 0;
    if (keepChars != 0) {
        if (num >= 0) lastNumber_ = num;
        if (kind == 0) bgAmp_ = format("B%04d%1d%1d.amp", lastNumber_ / 10, lastNumber_ % 10, timeMode);
        charsToNewBuffer();
    }
    if (num == -10) return fadeOut(trans, frames);
    int cur = cur_, old = cur_ == 0 ? 1 : 0;
    if (num == -20) {
        gfx_.copySlot(kBgSlot + cur, kBgSlot + old);
        num = lastNumber_;
    } else if (num >= 0) {
        std::string file, group;
        if (kind == 1) {
            game_.system.setGalleryFlag(0, num, 1);
            game_.writeSystem();
            file = format("v%06d.tga", num);
        } else if (kind == 2) {
            game_.system.setGalleryFlag(1, num, 1);
            game_.writeSystem();
            file = format("h%06d.tga", num);
        } else {
            file = format("B%04d%1d%1d.tga", num / 10, num % 10, timeMode);
            bgAmp_ = format("B%04d%1d%1d.amp", num / 10, num % 10, timeMode);
            group = "bak";
        }
        if (!gfx_.load(kBgSlot + cur, file, 2, false, bgEffect, "", bgLevel, group)) {
            game_.noteMissing("picture " + file);
            return;  // cur stays toggled (as the engine's)
        }
    } else if (timeMode == 0) {
        return;  // other negative numbers: the engine tests a stale variable, here the time mode
    }
    // The window's opacity follows the kind; it crossfades (0x410b08) when the window is shown.
    windowFade_ = (lastKind_ == 0) == (kind == 0) || !windowShown ? 0x20 : 0;
    int w = 0, h = 0;
    gfx_.pictureSize(kBgSlot + cur, w, h);
    lastKind_ = kind;
    keepChars_ = keepChars;
    picW_ = w;
    picH_ = h;
    shown_ = true;
    lastNumber_ = num;
    Buffer& c = buf_[static_cast<size_t>(cur)];
    const Buffer& o = buf_[static_cast<size_t>(old)];
    c.panOn = false;
    c.panEase = c.panCounter = c.panDuration = 0;
    // The source offset: given, or the old buffer's (a running pan continues).
    auto offset = [&](int v, int screen, int size, int Buffer::*rest, int Buffer::*target, bool isX) {
        if (v != 10000) {
            c.panOn = false;
            if (raw == 0) v = (screen - size) / 2 + v;
            c.*rest = c.*target = v;
            return;
        }
        if (!o.panOn) {
            v = o.*rest;
            c.*rest = c.*target = v;
            return;
        }
        int n = game_.fr(o.panDuration);
        if (o.panCounter < n) {
            float e = ease(o.panEase, static_cast<float>(o.panCounter) / static_cast<float>(n));
            v = ftol(static_cast<float>(o.*rest) * (1.0f - e) + static_cast<float>(o.*target) * e);
            c.panOn = o.panOn;
            c.panEase = o.panEase;
            c.panCounter = o.panCounter;
            c.panDuration = o.panDuration;
            c.*target = o.*target;
            c.*rest = o.*rest;
            (void)isX;
            return;
        }
        v = o.*target;
        c.*rest = c.*target = v;
    };
    offset(x, 1280, w, &Buffer::panX, &Buffer::panTargetX, true);
    x = c.panX;
    offset(y, 720, h, &Buffer::panY, &Buffer::panTargetY, false);
    y = c.panY;
    // Zoom, per axis; a running zoom of the old buffer continues (its pivot is not copied).
    auto zoomAxis = [&](float z, float Buffer::*start, float Buffer::*target, int Buffer::*pivot, int centre) -> float {
        if (z < 10000.0f) {
            c.zoomOn = false;
            c.*start = c.*target = z;
            c.*pivot = centre;
            return z;
        }
        if (!o.zoomOn) {
            c.*start = c.*target = o.*start;
            return o.*start;
        }
        int n = game_.fr(o.zoomDuration);
        if (o.zoomCounter < n) {
            float e = ease(o.zoomEase, static_cast<float>(o.zoomCounter) / static_cast<float>(n));
            float now = (1.0f - e) * (o.*start) + (o.*target) * e;
            c.zoomOn = o.zoomOn;
            c.zoomEase = o.zoomEase;
            c.zoomCounter = o.zoomCounter;
            c.zoomDuration = o.zoomDuration;
            c.*target = o.*target;
            c.*start = o.*start;
            return now;
        }
        c.*start = c.*target = o.*target;
        return o.*target;
    };
    float sx = zoomAxis(zx, &Buffer::zoomX, &Buffer::zoomTargetX, &Buffer::pivotX, 640);
    float sy = zoomAxis(zy, &Buffer::zoomY, &Buffer::zoomTargetY, &Buffer::pivotY, 360);
    if (rot < 10000.0f) c.rotOn = false;
    else rot = o.rot;
    c.rot = c.rotTarget = rot;

    int display = kDisplayObj + cur, bg = kBgObj + cur;
    srcW_ = 1280;
    srcH_ = 720;
    gfx_.makeImage(display, kTargetSlot + cur, 0x1036, true);
    gfx_.setMask(display, 2);
    gfx_.makeImage(bg, kBgSlot + cur, 0, true);
    gfx_.setTarget(bg, kTargetSlot + cur);
    gfx_.setMask(bg, 2);
    gfx_.setDest(bg, static_cast<float>((1280 - w) / 2), static_cast<float>((720 - h) / 2), static_cast<float>(w),
                 static_cast<float>(h));
    gfx_.setSource(bg, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
    gfx_.setFilter(bg, 2);
    srcW_ = w;
    srcH_ = h;
    gfx_.setRotation(bg, c.rot);
    gfx_.setScale(bg, sx, sy);
    gfx_.setPivot(bg, static_cast<float>(c.pivotX), static_cast<float>(c.pivotY));

    int d = game_.fr(frames);
    if (trans != -1 && d != 0) {
        switch (trans) {
        case 1: case 0xb: case 0xc: case 0xd: case 0xe: case 0x10: case 0x11: case 0x12: case 0x13: case 0x18:
            break;
        default:
            if (trans < 0x80) {
                game_.log("B: transition type " + std::to_string(trans) + " is not one of the engine's");
            } else {
                // Pattern fade: the mask f0nnn.bmp on the single-texture path (2048 x 1024).
                uint32_t flags = 3;
                if (trans & 0x800) flags |= 0x100;
                if (trans & 0x1000) flags |= 0x200;
                if (trans & 0x100) flags |= 0x400;
                std::string mask = format("f0%03d.bmp", trans & 0x7f);
                if (!gfx_.load(kMaskSlot, mask, flags, false, "", "", 0x100, "", 0x100, true)) {
                    game_.noteMissing("picture " + mask);
                }
                gfx_.setMaskSlot(display, kMaskSlot, 0);
                transSoft_ = soft;
            }
        }
        game_.clearScreen = true;  // 0x4c223c: the next frame is drawn
        transState_ = 1;
        transType_ = trans;
        transCounter_ = 0;
        transDuration_ = frames;
        return;
    }
    transDuration_ = 1;
    transCounter_ = 0;
    transType_ = 1;
    transState_ = 1;
}

void Scene::fadeOut(int trans, int frames) {
    // 0x40b820 (N = -10, unused by the scripts).
    if (keepChars_ == 0) charsAtEnd();
    if (!shown_) return;
    lastNumber_ = 0;
    lastKind_ = 0;
    Buffer& c = buf_[static_cast<size_t>(cur_)];
    c.panOn = false;
    c.panEase = c.panCounter = c.panDuration = 0;
    c.zoomX = c.zoomY = 1;
    transType_ = trans;
    transDuration_ = trans != -1 ? frames : 0;
    transState_ = 2;
    transCounter_ = 0;
}

void Scene::transitionStep() {
    // 0x40b8d0
    int d = game_.fr(transDuration_);
    int cur = cur_, old = cur_ == 0 ? 1 : 0;
    if (transState_ == 0) {
        // The display objects follow the shake.
        for (int b = 0; b < 2; ++b) {
            int obj = kDisplayObj + b;
            gfx_.setRect(obj, shakeX, shakeY, 0, 0, 1280, 720);
            gfx_.setPivot(obj, static_cast<float>(buf_[static_cast<size_t>(b)].pivotX),
                          static_cast<float>(buf_[static_cast<size_t>(b)].pivotY));
            gfx_.setFilter(obj, shakeScale == 1.0f && shakeRotation == 0.0f ? 0 : 2);
            gfx_.setScale(obj, shakeScale);
            gfx_.setRotation(obj, shakeRotation);
        }
    } else if (++transCounter_ >= d) {
        // 0x4026c0 reads the keepChars of the last show().
        charsAtEnd();
        pauseCamera(0, 0);
        if (onTransitionEnd) onTransitionEnd(keepChars_ != 0);
        bool toNew = transState_ == 1;
        transState_ = 0;
        if (toNew) {
            int bg = kBgObj + cur;
            gfx_.clearMaskSlot(bg);
            gfx_.setAlpha(bg, 0xff);
            gfx_.setPriority(bg, 0);
            gfx_.setSource(bg, static_cast<float>(buf_[static_cast<size_t>(cur)].panX),
                           static_cast<float>(buf_[static_cast<size_t>(cur)].panY), static_cast<float>(srcW_),
                           static_cast<float>(srcH_));
            gfx_.setRotation(bg, buf_[static_cast<size_t>(cur)].rot);
            gfx_.setScale(bg, buf_[static_cast<size_t>(cur)].zoomX, buf_[static_cast<size_t>(cur)].zoomY);
            int display = kDisplayObj + cur;
            gfx_.clearMaskSlot(display);
            gfx_.setAlpha(display, 0xff);
            gfx_.setPriority(display, 0x1035);
            gfx_.clearMaskSlot(kDisplayObj + old);
            gfx_.remove(kDisplayObj + old);
            gfx_.remove(kBgObj + old);
            gfx_.release(kBgSlot + old);
            gfx_.release(kMaskSlot);
            game_.clearScreen = true;
        } else {
            shown_ = false;
            gfx_.remove(kBgObj);
            gfx_.release(kBgSlot);
            gfx_.remove(kBgObj + 1);
            gfx_.release(kBgSlot + 1);
            gfx_.release(kMaskSlot);
        }
    } else {
        int p = transState_ == 1 ? transCounter_ : d - transCounter_;
        int a = p * 0xff / d;
        int incoming = kDisplayObj + cur, outgoing = kDisplayObj + old;
        float t = static_cast<float>(p) / static_cast<float>(d);
        switch (transType_) {
        case -1:
        case 0x18:
            break;
        case 1:
            gfx_.setAlpha(incoming, a);
            break;
        case 0xb: {
            gfx_.setAlpha(incoming, a);
            float q = static_cast<float>(d) - static_cast<float>(p);
            gfx_.setPivot(incoming, 640, 360);
            gfx_.setScale(incoming, q * q / static_cast<float>(d * d) + 1.0f);
            break;
        }
        case 0xc: {
            gfx_.setAlpha(incoming, a);
            float f = 1.0f - t;
            gfx_.setPivot(outgoing, 640, 360);
            gfx_.setScale(outgoing, (1.0f - f * f) + 1.0f);
            gfx_.setFilter(outgoing, 2);
            break;
        }
        case 0xd: {
            gfx_.setAlpha(incoming, a);
            float f = (1.0f - t) * (1.0f - t);
            gfx_.setPivot(incoming, 640, 360);
            gfx_.setScale(incoming, f + 1.0f);
            gfx_.setFilter(incoming, 2);
            gfx_.setPivot(outgoing, 640, 360);
            gfx_.setScale(outgoing, 2.0f - f);
            gfx_.setFilter(outgoing, 2);
            break;
        }
        case 0xe:
            // The engine's scale here goes through a pow() whose argument is not read yet (unused by
            // the scripts): a crossfade.
            gfx_.setAlpha(incoming, a);
            break;
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13: {
            gfx_.setAlpha(incoming, a);
            gfx_.setAlpha(outgoing, a);
            int q = (0x100 - a) * (0x100 - a);
            int vy = q * 0x2d0;
            int dy = -((vy + ((vy >> 31) & 0xffff)) >> 16);
            int vx = q * 0x500;
            int dx = -((vx + ((vx >> 31) & 0xffff)) >> 16);
            int oy = dy + 0x2d0, ox = dx + 0x500;
            switch (transType_) {
            case 0x10:
                gfx_.moveTo(incoming, 0, static_cast<float>(dy));
                gfx_.moveTo(outgoing, 0, static_cast<float>(oy));
                break;
            case 0x11:
                gfx_.moveTo(incoming, 0, static_cast<float>(0x2d0 - oy));
                gfx_.moveTo(outgoing, 0, static_cast<float>(-oy));
                break;
            case 0x12:
                gfx_.moveTo(incoming, static_cast<float>(0x500 - ox), 0);
                gfx_.moveTo(outgoing, static_cast<float>(-ox), 0);
                break;
            default:
                gfx_.moveTo(incoming, static_cast<float>(dx), 0);
                gfx_.moveTo(outgoing, static_cast<float>(ox), 0);
                break;
            }
            break;
        }
        default:
            if (transType_ >= 0x80 && transSoft_ >= 0 && transSoft_ <= 3) {
                static constexpr float kSoft[4] = {2.0f, 1.5f, 1.25f, 1.125f};
                float af = static_cast<float>(a) * 0.00390625f;
                float c0 = transSoft_ == 0 ? (af + af) - 1.0f : af * kSoft[transSoft_] - 1.0f;
                gfx_.setShader(incoming, transSoft_, c0);
            }
            break;
        }
    }
    ++windowFade_;
    cameraStep();
}

void Scene::cameraStep() {
    // Bottom of 0x40b8d0: in the story's normal state, unless the message window is in state 5 or 6.
    if (cameraPaused_ && ++pauseCounter_ >= pauseLength_) {
        cameraPaused_ = false;
        pauseLength_ = pauseCounter_ = 0;
    }
    for (int b = 0; b < 2; ++b) {
        Buffer& s = buf_[static_cast<size_t>(b)];
        int obj = kBgObj + b;
        if (s.panOn) {
            int n = Game::frP(s.panDuration);
            if (!cameraPaused_) ++s.panCounter;
            if (s.panCounter < n) {
                float e = ease(s.panEase, static_cast<float>(s.panCounter) / static_cast<float>(n));
                gfx_.setSource(obj, (1.0f - e) * static_cast<float>(s.panX) + static_cast<float>(s.panTargetX) * e,
                               static_cast<float>(s.panY) * (1.0f - e) + static_cast<float>(s.panTargetY) * e,
                               static_cast<float>(srcW_), static_cast<float>(srcH_));
            } else {
                s.panOn = false;
                s.panX = s.panTargetX;
                s.panY = s.panTargetY;
                gfx_.setSource(obj, static_cast<float>(s.panX), static_cast<float>(s.panTargetY),
                               static_cast<float>(srcW_), static_cast<float>(srcH_));
                game_.clearScreen = true;
            }
        }
        if (s.zoomOn) {
            int n = Game::frP(s.zoomDuration);
            if (!cameraPaused_) ++s.zoomCounter;
            gfx_.setPivot(obj, static_cast<float>(s.pivotX), static_cast<float>(s.pivotY));
            if (s.zoomCounter < n) {
                float e = ease(s.zoomEase, static_cast<float>(s.zoomCounter) / static_cast<float>(n));
                gfx_.setScale(obj, s.zoomX * (1.0f - e) + s.zoomTargetX * e, (1.0f - e) * s.zoomY + s.zoomTargetY * e);
                gfx_.setFilter(obj, 2);
            } else {
                s.zoomOn = false;
                s.zoomX = s.zoomTargetX;
                s.zoomY = s.zoomTargetY;
                gfx_.setScale(obj, s.zoomX, s.zoomY);
                game_.clearScreen = true;
            }
        }
        if (s.rotOn) {
            int n = Game::frP(s.rotDuration);
            if (!cameraPaused_) ++s.rotCounter;
            gfx_.setPivot(obj, static_cast<float>(s.pivotX), static_cast<float>(s.pivotY));
            if (s.rotCounter < n) {
                float e = ease(s.rotEase, static_cast<float>(s.rotCounter) / static_cast<float>(n));
                gfx_.setRotation(obj, (1.0f - e) * s.rot + e * s.rotTarget);
                gfx_.setFilter(obj, 2);
            } else {
                s.rotOn = false;
                s.rot = s.rotTarget;
                gfx_.setRotation(obj, s.rot);
                game_.clearScreen = true;
            }
        }
    }
}

// ---- Camera ------------------------------------------------------------------------------------------

void Scene::pan(int x, int y, int frames, int easeType) {
    Buffer& c = buf_[static_cast<size_t>(cur_)];
    if (c.panOn && c.panDuration != 0) {
        float e = ease(c.panEase, static_cast<float>(c.panCounter) / static_cast<float>(c.panDuration));
        c.panX = ftol(static_cast<float>(c.panX) * (1.0f - e) + static_cast<float>(c.panTargetX) * e);
        c.panY = ftol(static_cast<float>(c.panY) * (1.0f - e) + static_cast<float>(c.panTargetY) * e);
    }
    c.panOn = true;
    c.panTargetX = (1280 - picW_) / 2 + x;
    c.panTargetY = (720 - picH_) / 2 + y;
    c.panEase = easeType;
    c.panCounter = 0;
    c.panDuration = frames;
}

void Scene::zoom(float zx, float zy, int px, int py, int frames, int easeType) {
    Buffer& c = buf_[static_cast<size_t>(cur_)];
    if (c.zoomOn && c.zoomDuration != 0) {
        float e = ease(c.zoomEase, static_cast<float>(c.zoomCounter) / static_cast<float>(c.zoomDuration));
        c.zoomX = c.zoomX * (1.0f - e) + c.zoomTargetX * e;
        c.zoomY = (1.0f - e) * c.zoomY + c.zoomTargetY * e;
    }
    c.zoomOn = true;
    c.zoomTargetX = zx;
    c.zoomTargetY = zy;
    c.zoomEase = easeType;
    c.zoomCounter = 0;
    c.zoomDuration = frames;
    c.pivotX = px;
    c.pivotY = py;
}

void Scene::rotate(float angle, int px, int py, int frames, int easeType) {
    Buffer& c = buf_[static_cast<size_t>(cur_)];
    if (c.rotOn && c.rotDuration != 0) {
        float e = ease(c.rotEase, static_cast<float>(c.rotCounter) / static_cast<float>(c.rotDuration));
        c.rot = (1.0f - e) * c.rot + e * c.rotTarget;
    }
    c.rotOn = true;
    c.rotTarget = angle;
    c.rotEase = easeType;
    c.rotCounter = 0;
    c.rotDuration = frames;
    c.pivotX = px;
    c.pivotY = py;
}

void Scene::pauseCamera(int on, int frames) {
    cameraPaused_ = on != 0;
    pauseLength_ = frames;
    pauseCounter_ = 0;
}

bool Scene::cameraMoving() const {
    for (const Buffer& b : buf_) {
        if (b.panOn || b.zoomOn || b.rotOn) return true;
    }
    return false;
}

// ---- Tone --------------------------------------------------------------------------------------------

void Scene::tone(int mode, int fin, int fout, int r, int g, int b) {
    // 0x401de0: a new fade starts from the last finished tone.
    toneMode_ = mode;
    toneIn_ = fin;
    toneOut_ = fout;
    toneTarget_ = {r, g, b};
    toneCur_ = toneStart_;
    toneOn_ = true;
    toneCounter_ = 0;
}

void Scene::toneStep() {
    // 0x40d280
    int d = game_.frT(toneIn_);
    game_.frT(toneOut_);
    if (toneOn_) {
        if (toneCounter_ < d) {
            ++toneCounter_;
            int k = d - toneCounter_;
            for (size_t i = 0; i < 3; ++i) toneCur_[i] = k * toneStart_[i] / d + toneTarget_[i] * toneCounter_ / d;
        } else {
            toneCounter_ = 0;
            toneStart_ = toneCur_ = toneTarget_;
            if (toneMode_ == 1) {
                toneMode_ = 0;
                toneIn_ = toneOut_;
                toneTarget_ = {0x80, 0x80, 0x80};
            } else {
                toneOn_ = false;
            }
        }
    }
    gfx_.setTone(kDisplayObj, toneCur_[0], toneCur_[1], toneCur_[2]);
    gfx_.setTone(kDisplayObj + 1, toneCur_[0], toneCur_[1], toneCur_[2]);
}

// ---- Characters --------------------------------------------------------------------------------------

int Scene::charIndex(int id) const {
    for (int i = 0; i < 8; ++i) {
        if (chars_[static_cast<size_t>(i)].used && chars_[static_cast<size_t>(i)].id == id) return i;
    }
    return kNoChar;
}

bool Scene::charExists(int id) const {
    return charIndex(id) != kNoChar;
}

bool Scene::charBusy(int id) const {
    int i = charIndex(id);
    return i != kNoChar && chars_[static_cast<size_t>(i)].state != 0;
}

bool Scene::charsIdle() const {
    for (const Char& c : chars_) {
        if (c.used && c.state != 0) return false;
    }
    return true;
}

float Scene::charX(int w, int i, bool previous) const {
    // 0x402270
    const Char& c = chars_[static_cast<size_t>(i)];
    int base = (1280 - w) / 2;
    bool isFloat = previous ? c.prevPosFloat : c.posFloat;
    if (isFloat) return static_cast<float>(base) + (previous ? c.prevPos : c.pos);
    int idx = previous ? c.prevPosIndex : c.posIndex;
    int off = idx >= 0 && idx < 11 ? kPosition[idx] : 0;  // the engine reads past its table
    return static_cast<float>(off + base);
}

void Scene::charLoad(int i, int prioExtra, int& w, int& h, bool toggle) {
    // 0x415920
    Char& c = chars_[static_cast<size_t>(i)];
    if (toggle) c.k = c.k == 0 ? 1 : 0;
    game_.clearScreen = true;
    const char* prefix = c.id >= 0 && c.id < 38 ? kPrefix[c.id] : "";
    std::string file = std::string(prefix) + format("%06d.tga", c.pic);
    // The background's own colour table for characters, else the time of day's (the existence check
    // does not look in en.pak).
    std::string amp1 = characterAmp();
    int slot = kCharSlot + 2 * i + c.k;
    w = h = 0;
    if (!gfx_.load(slot, file, 0x802, false, amp1, charEffect, charLevel, "char", 0x80)) {
        game_.noteMissing("picture " + file);
        return;
    }
    gfx_.pictureSize(slot, w, h);
    int obj = kCharObj + 2 * i + c.k;
    gfx_.makeImage(obj, slot, c.z + 2 + prioExtra, true);
    float x = charX(w, i, false);
    gfx_.moveTo(obj, x, static_cast<float>(720 - h));
    gfx_.setMask(obj, 2);
    gfx_.setTone(obj, c.r, c.g, c.b);
    gfx_.setAlpha(obj, c.alpha);
    gfx_.setFilter(obj, 0);
    int buffer = c.mode == 3 ? (cur_ == 0 ? 1 : 0) : cur_;
    gfx_.setTarget(obj, kTargetSlot + buffer);
}

void Scene::charRelease(int i, bool oldOnly) {
    // 0x402580
    Char& c = chars_[static_cast<size_t>(i)];
    if (oldOnly) {
        int other = c.k == 0 ? 1 : 0;
        gfx_.release(kCharSlot + 2 * i + other);
        gfx_.remove(kCharObj + 2 * i + other);
        return;
    }
    for (int k = 0; k < 2; ++k) {
        gfx_.release(kCharSlot + 2 * i + k);
        gfx_.remove(kCharObj + 2 * i + k);
    }
    c = Char{};
}

void Scene::charsAtEnd() {
    // 0x4026c0
    if (keepChars_ == 0) {
        for (int i = 0; i < 8; ++i) charRelease(i, false);
        return;
    }
    for (int i = 0; i < 8; ++i) {
        Char& c = chars_[static_cast<size_t>(i)];
        if (!c.used) continue;
        if (c.state == 8) {
            charRelease(i, false);
        } else if (c.state == 0 && c.releaseOld) {
            c.releaseOld = false;
            charRelease(i, true);
        }
    }
}

void Scene::charsToNewBuffer() {
    // 0x4163c0: at a show() that keeps the characters, each idle one is loaded again into the new buffer
    // and deferred changes happen there.
    for (int i = 0; i < 8; ++i) {
        Char& c = chars_[static_cast<size_t>(i)];
        if (!c.used) continue;
        int w = 0, h = 0;
        if (c.state == 7) {
            c.mode = 0;
            c.state = 0;
            c.counter = c.duration = 30;
            charLoad(i, 0, w, h, true);
            c.w = w;
            c.h = h;
            gfx_.setAlpha(kCharObj + 2 * i + c.k, c.alpha);
            gfx_.setVisible(kCharObj + 2 * i + c.k, true);
        } else if (c.state == 0) {
            if (!c.posFloat) c.prevPosIndex = c.posIndex;
            else c.prevPos = c.pos;
            c.prevPosFloat = c.posFloat;
            charLoad(i, 0, w, h, true);
            c.prevW = c.w;
            c.w = w;
            c.prevH = c.h;
            c.h = h;
            c.releaseOld = true;
        }
        if (c.state == 9) {
            c.state = 0;
            c.counter = 30;
            c.mode = 0;
            c.duration = 30;
            c.k = c.k == 0 ? 1 : 0;
            charLoad(i, 0, w, h, false);
            c.prevW = c.w;
            c.releaseOld = true;
            c.w = w;
            c.prevH = c.h;
            c.h = h;
        }
    }
}

void Scene::charSet(int id, int pic, int posIndex, float posFloat, bool posIsFloat, int z, int mode, int r, int g,
                    int b, int alpha, int frames) {
    // 0x415f50
    int i = charIndex(id);
    if (i == kNoChar) {
        if (mode == -1 || mode == -2) {
            mode = 0;
            frames = 1;
        }
        i = kNoChar;
        for (int k = 0; k < 8; ++k) {
            if (!chars_[static_cast<size_t>(k)].used) {
                i = k;
                break;
            }
        }
        if (i == kNoChar) return game_.log("C: no room for another character");
        Char& c = chars_[static_cast<size_t>(i)];
        c.used = true;
        c.state = mode == 3 ? 7 : 1;
        c.mode = mode;
        c.id = id;
        c.pic = pic;
        c.z = z;
        c.posFloat = posIsFloat;
        if (posIsFloat) c.pos = posFloat;
        else c.posIndex = static_cast<int16_t>(posIndex);
        c.r = r & 0xff;
        c.g = g & 0xff;
        c.b = b & 0xff;
        c.alpha = static_cast<int16_t>(alpha);
        c.k = 0;
        c.counter = 0;
        c.duration = static_cast<int16_t>(frames);
        if (mode == 3) {
            gfx_.setVisible(kCharObj + 2 * i, false);
            return;
        }
        int w = 0, h = 0;
        charLoad(i, 0, w, h, false);
        c.w = w;
        c.h = h;
        return;
    }
    Char& c = chars_[static_cast<size_t>(i)];
    bool samePos;
    if (!c.posFloat) samePos = posIsFloat ? static_cast<float>(c.posIndex) == posFloat : c.posIndex == static_cast<int16_t>(posIndex);
    else samePos = posIsFloat ? c.pos == posFloat : static_cast<float>(posIndex) == c.pos;
    if (c.z == z && samePos && c.r == (r & 0xff) && c.g == (g & 0xff) && c.b == (b & 0xff) &&
        c.alpha == static_cast<int16_t>(alpha) && c.pic == pic) {
        return;
    }
    if (mode == 0) {
        mode = 3;
        c.state = 9;
        c.mode = 3;
    } else {
        if (mode == -1 || mode == -2) {
            mode = 0;
            frames = 1;
            c.state = 3;
            c.mode = 0;
        } else if (mode == 3) {
            c.state = 9;
            c.mode = 3;
        } else {
            c.state = 3;
            c.mode = mode;
        }
    }
    c.z = z;
    if (!c.posFloat) c.prevPosIndex = c.posIndex;
    else c.prevPos = c.pos;
    c.prevPosFloat = c.posFloat;
    c.posFloat = posIsFloat;
    if (posIsFloat) c.pos = posFloat;
    else c.posIndex = static_cast<int16_t>(posIndex);
    c.prevR = c.r;
    c.prevG = c.g;
    c.prevB = c.b;
    c.r = r & 0xff;
    c.g = g & 0xff;
    c.b = b & 0xff;
    c.prevAlpha = c.alpha;
    c.alpha = static_cast<int16_t>(alpha);
    c.counter = 0;
    c.duration = static_cast<int16_t>(frames);
    if (pic == c.pic) {
        c.samePic = true;
        return;
    }
    c.pic = pic;
    if (mode != 3) {
        c.k = c.k == 0 ? 1 : 0;
        int w = 0, h = 0;
        charLoad(i, 0, w, h, false);
        c.prevW = c.w;
        c.w = w;
        c.prevH = c.h;
        c.h = h;
    }
    c.samePic = false;
}

void Scene::charRemove(int id, int mode, int frames) {
    // 0x402610
    int i = charIndex(id);
    if (i == kNoChar) return;
    Char& c = chars_[static_cast<size_t>(i)];
    c.state = mode == 3 ? 8 : 2;
    c.mode = mode;
    c.counter = 0;
    c.duration = static_cast<int16_t>(frames);
}

void Scene::charMove(int id, float x) {
    // 0x402730
    int i = charIndex(id);
    if (i == kNoChar) return;
    Char& c = chars_[static_cast<size_t>(i)];
    c.pos = c.prevPos = x;
    c.posFloat = c.prevPosFloat = true;
}

void Scene::charStep() {
    // 0x40d440
    for (int i = 0; i < 8; ++i) {
        Char& c = chars_[static_cast<size_t>(i)];
        if (!c.used) continue;
        int d = game_.fr(c.duration);
        float x = charX(c.w, i, false);
        float xPrev = charX(c.prevW, i, true);
        int h = c.h, hPrev = c.prevH;
        int curObj = kCharObj + 2 * i + c.k, othObj = kCharObj + 2 * i + (c.k == 0 ? 1 : 0);
        auto A = static_cast<float>(c.alpha), Ap = static_cast<float>(c.prevAlpha);
        if (c.state == 1 && c.mode == 3) {
            c.state = 7;
        } else if (c.state == 1 || c.state == 2) {
            if (c.mode == 3) {
                c.state = 8;
            } else if (++c.counter >= d) {
                if (c.state == 1) {
                    gfx_.setAlpha(curObj, c.alpha);
                    gfx_.setScale(curObj, 1.0f);
                    gfx_.setFilter(curObj, 0);
                    c.state = 0;
                } else {
                    charRelease(i, false);
                }
            } else {
                float f = static_cast<float>(c.counter) / static_cast<float>(d);
                if (c.state != 1) f = 1.0f - f;
                gfx_.setAlpha(curObj, ftol(A * f));
                switch (c.mode) {
                case 1:
                case 2: {
                    float e = 1.0f - (1.0f - f) * (1.0f - f);
                    x = c.mode == 1 ? (1.0f - e) * 1280.0f + e * x : e * x - (1.0f - e) * 768.0f;
                    break;
                }
                case 6:
                    gfx_.setPivot(curObj, static_cast<float>(c.w / 2) + x, 720.0f);
                    gfx_.setScale(curObj, 1.0f - (1.0f - f) * 0.25f);
                    gfx_.setFilter(curObj, 2);
                    break;
                case 7:
                    gfx_.setPivot(curObj, static_cast<float>(c.w / 2) + x, static_cast<float>(c.h / 2));
                    gfx_.setScale(curObj, (1.0f - f) * 0.5f + 1.0f);
                    gfx_.setFilter(curObj, 2);
                    break;
                default:
                    break;
                }
            }
        } else if (c.state == 3) {
            if (++c.counter >= d) {
                gfx_.moveTo(curObj, x, static_cast<float>(720 - c.h));
                gfx_.setPivot(curObj, static_cast<float>(c.w / 2) + x, 720.0f);
                gfx_.setAlpha(curObj, c.alpha);
                gfx_.setTone(curObj, c.r, c.g, c.b);
                gfx_.setScale(curObj, 1.0f);
                gfx_.setFilter(curObj, 0);
                c.state = 0;
                game_.clearScreen = true;
                if (c.mode == 0 || c.mode == 6 || c.mode == 7 || c.mode == 2 || c.mode == 1) {
                    gfx_.remove(othObj);
                    gfx_.release(kCharSlot + 2 * i + (c.k == 0 ? 1 : 0));
                }
            } else {
                float t = static_cast<float>(c.counter) / static_cast<float>(d);
                if (c.mode == 6 || c.mode == 7) {
                    float now = c.posFloat ? c.pos : static_cast<float>(c.posIndex);
                    float before = c.prevPosFloat ? c.prevPos : static_cast<float>(c.prevPosIndex);
                    if (before == now) {
                        gfx_.setAlpha(curObj, ftol(A * (1.0f - (1.0f - t) * (1.0f - t))));
                        gfx_.setAlpha(othObj, ftol(Ap * (1.0f - t * t)));
                        x = charX(c.w, i, false);
                        xPrev = charX(c.prevW, i, true);
                        if (c.mode == 7) {
                            gfx_.setPivot(othObj, static_cast<float>(c.prevW / 2) + xPrev, 720.0f);
                            gfx_.setScale(othObj, 1.0f - t * 0.25f);
                        } else {
                            gfx_.setPivot(othObj, static_cast<float>(c.prevW / 2) + xPrev, static_cast<float>(c.prevH / 2));
                            gfx_.setScale(othObj, t * 0.5f + 1.0f);
                        }
                    } else {
                        float u = 1.0f - t;
                        x = charX(c.w, i, false) * t + charX(c.w, i, true) * u;
                        xPrev = t * charX(c.prevW, i, false) + charX(c.prevW, i, true) * u;
                        gfx_.setAlpha(curObj, ftol(A * t));
                        gfx_.setAlpha(othObj, ftol(Ap * u));
                        float pivot = static_cast<float>(c.prevW / 2) + xPrev;
                        if (c.mode == 7) {
                            gfx_.setPivot(othObj, pivot, 720.0f);
                            gfx_.setScale(othObj, 1.0f - t * 0.25f);
                            gfx_.setPivot(curObj, pivot, static_cast<float>(c.prevH / 2));
                            gfx_.setScale(curObj, u * 0.5f + 1.0f);
                        } else {
                            gfx_.setPivot(othObj, pivot, static_cast<float>(c.prevH / 2));
                            gfx_.setScale(othObj, t * 0.5f + 1.0f);
                            gfx_.setPivot(curObj, pivot, 720.0f);
                            gfx_.setScale(curObj, 1.0f - u * 0.25f);
                        }
                        gfx_.setFilter(curObj, 2);
                        gfx_.setFilter(othObj, 2);
                    }
                } else if (c.mode == 0) {
                    if (c.prevPosIndex == c.posIndex) {
                        gfx_.setAlpha(curObj, ftol(A * (1.0f - (1.0f - t) * (1.0f - t))));
                        gfx_.setAlpha(othObj, ftol(Ap * (1.0f - t * t)));
                    } else {
                        x = charX(c.w, i, false);
                        xPrev = charX(c.prevW, i, true);
                        gfx_.setAlpha(curObj, ftol(A * t));
                        gfx_.setAlpha(othObj, ftol(Ap * (1.0f - t)));
                    }
                } else {
                    float e = 1.0f - (1.0f - t) * (1.0f - t), u = 1.0f - e;
                    x = charX(c.w, i, false) * e + charX(c.w, i, true) * u;
                    xPrev = charX(c.prevW, i, false) * e + charX(c.prevW, i, true) * u;
                    gfx_.moveTo(curObj, x, static_cast<float>(720 - c.h));
                    gfx_.moveTo(othObj, xPrev, static_cast<float>(720 - c.prevH));
                    if (!c.samePic) {
                        gfx_.setAlpha(curObj, ftol(A * e));
                        gfx_.setAlpha(othObj, ftol(Ap * u));
                    }
                    auto mix = [&](int now, int before) {
                        return static_cast<int>(std::nearbyint(static_cast<float>(now) * e + static_cast<float>(before) * u)) & 0xff;
                    };
                    gfx_.setTone(curObj, mix(c.r, c.prevR), mix(c.g, c.prevG), mix(c.b, c.prevB));
                }
            }
        }
        if (!c.used) continue;  // released this frame
        gfx_.moveTo(curObj, x, static_cast<float>(720 - h));
        gfx_.moveTo(othObj, xPrev, static_cast<float>(720 - hPrev));
        gfx_.setPriority(curObj, c.z + 2);
    }
}

// ---- Saves ------------------------------------------------------------------------------------------

void Scene::capture(SaveRecord::Scene& s) const {
    // 0x4234c0 [6..0x27], 0x401e70, [0x440..0x442]
    const Buffer& b = buf_[static_cast<size_t>(cur_)];
    s.shown = shown_;
    s.number = lastNumber_;
    s.kind = lastKind_;
    s.panX = b.panX;
    s.panY = b.panY;
    s.zoomX = b.zoomX;
    s.zoomY = b.zoomY;
    s.rotation = static_cast<int>(b.rot);
    s.timeMode = timeMode;
    s.bgLevel = bgLevel;
    s.charLevel = charLevel;
    s.bgAmp = bgAmp_.substr(0, 0x1f);
    s.bgEffect = bgEffect.substr(0, 0x1f);
    s.charEffect = charEffect.substr(0, 0x1f);
    s.tone = toneStart_;
    for (size_t i = 0; i < 8; ++i) {
        const Char& c = chars_[i];
        s.chars[i] = {c.used, c.id, c.pic, c.posFloat, c.posIndex, c.pos, c.z, c.r, c.alpha};
    }
}

void Scene::restore(const SaveRecord::Scene& s) {
    // 0x423d10: the colour settings; every character released, the saved ones set again in mode 3 (all
    // three channels from the saved red); the tone; the background shown again (transition -1, the
    // characters kept), or faded out when none was shown.
    charLevel = s.charLevel;
    timeMode = s.timeMode;
    bgLevel = s.bgLevel;
    bgAmp_ = s.bgAmp;
    bgEffect = s.bgEffect;
    charEffect = s.charEffect;
    for (int i = 0; i < 8; ++i) charRelease(i, false);
    for (const auto& c : s.chars) {
        if (!c.used) continue;
        charSet(c.id, c.pic, c.posIndex, c.pos, c.posFloat, c.z, 3, c.r, c.r, c.r, c.alpha, 0);
    }
    toneStart_ = toneCur_ = s.tone;
    shown_ = s.shown;
    if (shown_) {
        show(s.number, -1, 1, s.kind, 0, 0, s.panX, s.panY, s.zoomX, s.zoomY, static_cast<float>(s.rotation), 1);
    } else {
        fadeOut(-1, 0);
    }
}

}  // namespace nyan::wa2
