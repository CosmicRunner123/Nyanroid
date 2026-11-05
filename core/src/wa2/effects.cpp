#include "nyan/wa2/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// 4096 * sin(2 pi i / 256), as the exe's table at 0x4a5b20 has it.
constexpr int16_t kSin[256] = {
    0, 100, 200, 301, 401, 501, 601, 700, 799, 897, 995, 1092, 1189, 1284, 1379, 1474, 1567, 1659, 1751, 1841, 1930, 2018,
    2105, 2191, 2275, 2358, 2439, 2519, 2598, 2675, 2750, 2824, 2896, 2966, 3034, 3101, 3166, 3229, 3289, 3348, 3405, 3460,
    3513, 3563, 3612, 3658, 3702, 3744, 3784, 3821, 3856, 3889, 3919, 3947, 3973, 3996, 4017, 4035, 4051, 4065, 4076, 4084,
    4091, 4094, 4096, 4094, 4091, 4084, 4076, 4065, 4051, 4035, 4017, 3996, 3973, 3947, 3919, 3889, 3856, 3821, 3784, 3744,
    3702, 3658, 3612, 3563, 3513, 3460, 3405, 3348, 3289, 3229, 3166, 3101, 3034, 2966, 2896, 2824, 2750, 2675, 2598, 2519,
    2439, 2358, 2275, 2191, 2105, 2018, 1930, 1841, 1751, 1659, 1567, 1474, 1379, 1284, 1189, 1092, 995, 897, 799, 700,
    601, 501, 401, 301, 200, 100, 0, -100, -200, -301, -401, -501, -601, -700, -799, -897, -995, -1092, -1189, -1284,
    -1379, -1474, -1567, -1659, -1751, -1841, -1930, -2018, -2105, -2191, -2275, -2358, -2439, -2519, -2598, -2675, -2750,
    -2824, -2896, -2966, -3034, -3101, -3166, -3229, -3289, -3348, -3405, -3460, -3513, -3563, -3612, -3658, -3702, -3744,
    -3784, -3821, -3856, -3889, -3919, -3947, -3973, -3996, -4017, -4035, -4051, -4065, -4076, -4084, -4091, -4094, -4096,
    -4094, -4091, -4084, -4076, -4065, -4051, -4035, -4017, -3996, -3973, -3947, -3919, -3889, -3856, -3821, -3784, -3744,
    -3702, -3658, -3612, -3563, -3513, -3460, -3405, -3348, -3289, -3229, -3166, -3101, -3034, -2966, -2896, -2824, -2750,
    -2675, -2598, -2519, -2439, -2358, -2275, -2191, -2105, -2018, -1930, -1841, -1751, -1659, -1567, -1474, -1379, -1284,
    -1189, -1092, -995, -897, -799, -700, -601, -501, -401, -301, -200, -100};

// C `%` (remainder with the dividend's sign) into the table.
int sinAt(int i) {
    int m = i % 256;
    if (m < 0) m += 256;  // the exe would read before its table here (§12.3); keep inside
    return kSin[m];
}

int triangle(int n, int p) {
    // 0x44eb70
    if (p == 0) return 0;
    n %= p * 2;
    return n >= p ? p * 2 - n : n;
}

// Direction signs for the offset shakes (types 0, 1, 9).
constexpr int kDirX[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int kDirY[8] = {1, 1, 0, -1, -1, -1, 0, 1};

constexpr int kShakeBack = 0x370;
constexpr int kParticle = 0x51, kParticleCopy = 0x1e1;
constexpr int kPicImage = 0x4e, kPicObj = 0x11, kPicTwin = 0x31, kPicAni = 0xfb;
constexpr int kTargetSlot = 0x105;

}  // namespace

Effects::Effects(Game& game) : game_(game), gfx_(game.gfx()) {}

void Effects::reset() {
    stopShake();
    resetWeather(true);
    for (int i = 0; i < 32; ++i) releasePicture(i);
    if (calPhase_ >= 0) {
        for (int obj = 0x53a; obj <= 0x549; ++obj) gfx_.remove(obj);
        for (int s = 0x195; s <= 0x198; ++s) gfx_.release(s);
    }
    calPhase_ = -1;
}

// ---- Shake -------------------------------------------------------------------------------------------

void Effects::shake(int typeFlags, int amp, int dur, int dir, int speed) {
    shakeType_ = typeFlags;
    shakeAmp_ = amp;
    shakeDur_ = dur;
    shakeDir_ = dir;
    shakeSpeed_ = speed;
    shakeCount_ = 0;
    shakeOn_ = true;
    placeShake(0, 0, 1, 0, 0x200);
}

void Effects::stopShake() {
    gfx_.remove(kShakeBack);
    placeShake(0, 0, 1, 0, 0x200);
    shakeOn_ = false;
    game_.message().placeWindow();
}

void Effects::endMessageShake() {
    if (shakeOn_ && shakeDur_ != 0) stopShake();
}

void Effects::placeShake(float x, float y, float zoom, float angle, int flags) {
    Scene& s = game_.scene();
    bool screen = flags == 0 || flags == 0x200 || !(flags & 0x100);
    bool window = flags == 0x200 || (flags & 0x100);
    if (screen) {
        s.shakeX = x;
        s.shakeY = y;
        s.shakeScale = zoom;
        s.shakeRotation = angle;
    }
    if (window) {
        winX = x;
        winY = y;
        winZoom = zoom;
        winAngle = angle;
    }
}

void Effects::shakeStep() {
    // 0x40c780
    if (!shakeOn_) return;
    int D = game_.frT(shakeDur_);
    ++shakeCount_;
    if (D <= shakeCount_ && shakeDur_ != 0) {
        stopShake();
        return;
    }
    gfx_.makePolygon(kShakeBack, 4, 0x102b, true);
    gfx_.setColor(kShakeBack, 0xff, 0, 0, 0);
    int c = shakeCount_, amp = shakeAmp_, type = shakeType_ & 0xff;
    float ox = 0, oy = 0, zoom = 1, angle = 0;
    auto offset = [&](int v, bool decay) {
        int dir = ((shakeDir_ % 8) + 8) % 8;
        int vx = kDirX[dir] * v, vy = kDirY[dir] * v;
        if (decay && D != 0) {
            vx = (D - c) * vx / D;
            vy = (D - c) * vy / D;
        }
        ox = static_cast<float>(vx);
        oy = static_cast<float>(vy);
    };
    switch (type) {
    case 0:
        offset(sinAt((shakeSpeed_ * c) / 8) * amp / 4096, true);
        game_.clearScreen = true;
        break;
    case 1:
        offset(((c % 8) / 4 * 2 - 1) * amp, false);
        game_.clearScreen = true;
        break;
    case 2: {
        int t = triangle(2 * c, amp);
        zoom = amp != 0 ? static_cast<float>(t * t) / static_cast<float>(amp * amp) + 1.0f : 1.0f;
        break;
    }
    case 9:
        joltToggle_ = !joltToggle_;
        if (joltToggle_) {
            int r;
            do r = game_.rand.next() % 8;
            while (r == shakeDir_);
            shakeDir_ = r;
        }
        offset(amp, false);
        game_.clearScreen = true;
        break;
    case 0xc:
        if (D != 0) {
            int a = 360 - c * 360 / D;
            int e = 360 - a * a / 360;
            angle = static_cast<float>(shakeDir_ % 2 == 0 ? 360 - ((e * amp) / 2) % 360 : ((e * amp) / 2) % 360);
        } else {
            angle = static_cast<float>(c * amp) / 100.0f * (shakeDir_ % 2 == 0 ? -1.0f : 1.0f);
            zoom = static_cast<float>(shakeSpeed_) / 4096.0f;
        }
        break;
    case 0xd: {
        int v = -(sinAt((shakeSpeed_ * c) / 8) * amp / 4096);
        if (D != 0) v = (D - c) * v / D;
        angle = static_cast<float>(v);
        break;
    }
    case 0xe: {
        int ph = (c / 2) % 4;
        angle = static_cast<float>(ph == 0 ? -amp : ph == 2 ? amp : 0);
        break;
    }
    case 0xf: {
        int t = triangle(2 * c, amp);
        zoom = amp != 0 ? static_cast<float>(t * t) * (static_cast<float>(shakeSpeed_ - 100) / 100.0f) /
                                  static_cast<float>(amp * amp) + 1.0f
                        : 1.0f;
        break;
    }
    default:
        break;
    }
    placeShake(ox, oy, zoom, angle, shakeType_ & 0xff00);
}

// ---- Weather -----------------------------------------------------------------------------------------

void Effects::resetWeather(bool full) {
    // 0x406e00
    if (!full) {
        count_ = 0;
        weatherFade_ = true;
        return;
    }
    if (weatherMode_ == 2) {
        weatherMode_ = 0;
        return;
    }
    for (int obj = kParticle; obj <= 0x370; ++obj) {
        if (obj == kShakeBack && shakeOn_) continue;
        gfx_.remove(obj);
    }
    gfx_.release(0x6e);
    gfx_.release(0x6f);
    game_.ani().release(0);
    weatherOn_ = false;
    weatherType_ = weatherFlags_ = weatherLayer_ = weatherMode_ = 0;
    wind_ = fall_ = 0;
    windTarget_ = fallTarget_ = count_ = live_ = frame_ = margin_ = marginTarget_ = 0;
    weatherFade_ = prefillDone_ = false;
    particles_ = {};
}

bool Effects::weatherGone() const {
    // 0x406f60
    for (int i = 0; i < live_ && i < 400; ++i) {
        if (particles_[static_cast<size_t>(i)].active) return false;
    }
    return true;
}

void Effects::weather(int typeFlags, int wind, int fall, int margin, int count, int layer, int mode) {
    // 0x40fd20
    resetWeather(true);
    weatherFlags_ = typeFlags;
    weatherType_ = typeFlags & 0xff;
    weatherLayer_ = layer;
    weatherMode_ = mode;
    weatherOn_ = true;
    if (weatherType_ == 0) {
        // Rain: an animation on the screen.
        game_.ani().load(0, "\x89\x4a\x36\x34\x30\x78\x33\x36\x30.ani", 1, "", "", 0x100, "grp");  // 雨640x360.ani
        game_.ani().makeObject(kParticle, 0, 0, true, 0x1043, true, 1, 0, 0);
        gfx_.moveTo(kParticle, 640, 360);
        gfx_.setPivot(kParticle, 640, 360);
        gfx_.setScale(kParticle, 2.0f);
        return;
    }
    if (weatherType_ == 1) {
        gfx_.load(0x6e, "sakura1_2.tga", 2, false, "", "", 0x100, "grp");
        gfx_.load(0x6f, "sakura2.tga", 2, false, "", "", 0x100, "grp");
        gfx_.load(0x70, "sakura3.tga", 2, false, "", "", 0x100, "grp");
    } else {
        gfx_.load(0x6e, "weather.tga", 2, false, "", "", 0x100, "grp");
        margin_ = margin;
    }
    windTarget_ = wind;
    fallTarget_ = fall;
    wind_ = static_cast<float>(wind);
    fall_ = static_cast<float>(fall);
    count_ = std::clamp(count, 0, 400);
    live_ = 0;
}

void Effects::changeWeather(int wind, int fall, int count, int layer, int mode, int margin) {
    if (wind != -1000) windTarget_ = wind;
    if (fall != -1000) fallTarget_ = fall;
    if (count != -1000) count_ = std::clamp(count, 0, 400);
    if (layer != -1000) weatherLayer_ = layer;
    if (mode != -1000) weatherMode_ = mode;
    marginTarget_ = margin;
}

void Effects::transitionEnded(bool keepChars) {
    if (!keepChars && weatherMode_ == 0 && weatherOn_) resetWeather(true);
}

void Effects::snowFlake(int i, bool prefill) {
    // 0x4071e0
    Particle& p = particles_[static_cast<size_t>(i)];
    auto& rnd = game_.rand;
    p.sprite = 0;
    int r = rnd.next() % 32;
    int k;
    bool mid = false, far = false;
    if (r == 0) {
        if (weatherFlags_ & 0x200) {
            mid = true;
        } else {
            k = rnd.next() % 16;
            rnd.next();
            rnd.next();
            p.sx = static_cast<float>(rnd.next() % 8 + 10);
            p.sy = static_cast<float>(rnd.next() % 8 + 10);
        }
    } else if (r <= 2) {
        mid = true;
    } else {
        far = true;
    }
    if (mid) {
        if (weatherFlags_ & 0x400) {
            far = true;
        } else {
            k = rnd.next() % 48 + 192;
            p.sx = static_cast<float>(rnd.next() % 4 + 5);
            p.sy = static_cast<float>(rnd.next() % 4 + 5);
        }
    }
    if (far) {
        if (!(weatherFlags_ & 0x800)) {
            k = rnd.next() % 8 + 248;
            int range = (256 - k) * 100;
            p.sx = (static_cast<float>(range > 0 ? rnd.next() % range : 0) / 100.0f + 1.0f) * 0.5f;
            p.sy = (static_cast<float>(range > 0 ? rnd.next() % range : 0) / 100.0f + 1.0f) * 0.5f;
        } else {
            p.sprite = rnd.next() % 4 + 1;
            k = rnd.next() % 8 + 248;
            p.sx = static_cast<float>(rnd.next() % 3 + 1);
            p.sy = static_cast<float>(rnd.next() % 3 + 513 - 2 * k) / 2.0f;
        }
    }
    p.k = k;
    p.x = static_cast<float>(rnd.next() % ((576 - k) * 4) - (256 - k) * 2);
    if (prefill) p.y = static_cast<float>(rnd.next() % ((436 - k) * 4) - (256 - k) * 2);
    else p.y = static_cast<float>(-(rnd.next() % 100) - (256 - k) * 2);
    p.x0 = p.x;
    p.y0 = p.y;
    p.z = static_cast<float>(k);
    p.phase = rnd.next() & 0xff;
    p.active = true;
}

void Effects::burstParticle(int i, bool prefill, bool inward) {
    // 0x4075f0: x holds the base angle, y the radius, z the radius scale, sx the angle, sy the spin;
    // x0 / y0 the spiral's distance and velocity.
    Particle& p = particles_[static_cast<size_t>(i)];
    auto& rnd = game_.rand;
    p.k = rnd.next() % 4;
    p.x = static_cast<float>(rnd.next() % 360) / 3.141592f;
    if (!prefill) {
        p.y = static_cast<float>(rnd.next() % 100 + (inward ? 1300 : 100));
        p.phase = 0;
        p.x0 = 1.0f;
        p.y0 = 0;
    } else {
        p.y = static_cast<float>(rnd.next() % 1300 + 100);
        p.phase = rnd.next() % 360;
        p.x0 = static_cast<float>(rnd.next() % 360);
    }
    float kappa = static_cast<float>(rnd.next() % 30) / 10.0f - 1.0f;
    if (kappa >= 0 && kappa < 0.4f) kappa = 0.4f;
    else if (kappa < 0 && kappa > -0.4f) kappa = -0.4f;
    p.z = kappa;
    p.sx = static_cast<float>(rnd.next() % 360);
    p.sy = (static_cast<float>(rnd.next() % ((4 - p.k) * 100)) / 100.0f + 1.0f) * 0.5f;
    p.active = true;
    if (weatherType_ == 6) {
        p.x0 = 1.0f;
        p.y0 = 0;
        if (prefill) {
            for (int n = 0; n < p.phase; ++n) {
                p.y0 += fall_ * (static_cast<float>(p.k + 2) / 1000.0f) / 10.0f;
                p.x0 += p.y0;
            }
        }
    }
}

void Effects::weatherSpawn(int i, bool prefill) {
    auto& rnd = game_.rand;
    Particle& p = particles_[static_cast<size_t>(i)];
    switch (weatherType_) {
    case 1:
        if (prefill) {
            p.k = rnd.next() % 20 == 0 ? 0 : 1 + rnd.next() % 2;
            p.x = static_cast<float>(rnd.next() % 1280);
            p.y = static_cast<float>(rnd.next() % 720);
        } else {
            p.x = static_cast<float>(rnd.next() % 1280);
            p.y = static_cast<float>(-40 - rnd.next() % 100);
            p.k = rnd.next() % 20 == 0 ? 0 : 1 + rnd.next() % 2;
        }
        if (p.k == 0) {
            p.sx = p.sy = 8.0f;
        } else {
            p.sx = (static_cast<float>(rnd.next() % ((3 - p.k) * 100)) / 100.0f + 1.0f) * 0.5f;
            p.sy = (static_cast<float>(rnd.next() % ((3 - p.k) * 100)) / 100.0f + 1.0f) * 0.5f;
        }
        p.phase = rnd.next() & 0xff;
        p.active = true;
        break;
    case 2:
        p.k = rnd.next() % 6;
        p.x = static_cast<float>(rnd.next() % 1280);
        p.y = static_cast<float>(720 + rnd.next() % 100);
        p.sx = (static_cast<float>(rnd.next() % ((6 - p.k) * 100)) / 100.0f + 1.0f) * 0.5f;
        p.sy = (static_cast<float>(rnd.next() % ((6 - p.k) * 100)) / 100.0f + 1.0f) * 0.5f;
        p.phase = rnd.next() & 0xff;
        p.active = true;
        break;
    case 3: snowFlake(i, prefill); break;
    case 4: burstParticle(i, prefill, false); break;
    case 5: burstParticle(i, prefill, !prefill); break;
    case 6: burstParticle(i, prefill, false); break;
    default: break;
    }
}

void Effects::retire(int i) {
    Particle& p = particles_[static_cast<size_t>(i)];
    if (i >= count_) {
        p.active = false;
        --live_;
        return;
    }
    // Wrap / respawn as the type says (the callers handle the wrap cases).
    weatherSpawn(i, false);
}

void Effects::weatherStep() {
    // 0x407900
    if (!weatherOn_ || weatherType_ == 0) return;
    auto ramp = [](float& cur, int target) {
        auto t = static_cast<float>(target);
        if (cur < t) cur = std::min(cur + 1.0f, t);
        else if (cur > t) cur = std::max(cur - 1.0f, t);
    };
    ramp(wind_, windTarget_);
    ramp(fall_, fallTarget_);
    if (margin_ < marginTarget_) ++margin_;
    ++frame_;
    auto& rnd = game_.rand;
    // Spawning.
    if (!prefillDone_ && (weatherFlags_ & 0x100)) {
        int n = count_ * 4 / 5;
        for (int i = 0; i < n; ++i) weatherSpawn(i, true);
        live_ = std::max(live_, n);
    }
    prefillDone_ = true;
    int interval = std::max(4, static_cast<int>(100.0 / std::max(1.0, std::sqrt(static_cast<double>(count_)))));
    if (weatherType_ == 2) interval = 5;
    if (live_ < count_ && frame_ % interval == 0) {
        if (weatherType_ == 1) {
            int free = -1;
            for (int i = 0; i < count_; ++i) {
                if (!particles_[static_cast<size_t>(i)].active) {
                    free = i;
                    break;
                }
            }
            if (free == -1) return;  // the exe returns from the whole update (§12.2)
            weatherSpawn(free, false);
            live_ = std::max(live_, free + 1);
        } else if (weatherType_ == 6) {
            int free = count_;
            for (int i = 0; i < count_; ++i) {
                if (!particles_[static_cast<size_t>(i)].active) {
                    free = i;
                    break;
                }
            }
            if (free < 400) {
                weatherSpawn(free, false);
                live_ = std::max(live_, free + 1);
            }
        } else if (live_ < 400) {
            weatherSpawn(live_, false);
            ++live_;
        }
    }
    for (int obj = kParticle; obj < kParticleCopy + 400 && obj <= 0x370; ++obj) {
        if (obj == kShakeBack) continue;
        gfx_.remove(obj);
    }
    Scene& scene = game_.scene();
    int cur = scene.currentBuffer(), other = cur == 0 ? 1 : 0;
    bool inTransition = scene.transitioning();
    int bound = weatherType_ == 6 ? count_ : live_;
    float m = static_cast<float>(margin_);
    for (int i = 0; i < bound && i < 400; ++i) {
        Particle& p = particles_[static_cast<size_t>(i)];
        if (!p.active) continue;
        int obj = kParticle + i;
        float dx = 0, dy = 0, size = 0;
        int alpha = 0xff, image = 0x6e;
        float su = 0, sv = 0, ss = 0;
        int prio = 10;
        bool twoCopies = false;
        switch (weatherType_) {
        case 1: {
            p.x += p.sx * (static_cast<float>(sinAt(p.phase + 64)) / 4096.0f + wind_ / 10.0f);
            p.y += p.sy * fall_ / 10.0f;
            bool gone = false;
            if (p.y > 720) gone = true, p.y -= 752;
            if (p.x >= 1280) gone = true, p.x -= 1312;
            if (p.x < -32) gone = true, p.x += 1312;
            if (gone && i >= count_) {
                p.active = false;
                --live_;
                continue;
            }
            static constexpr int kCell[3] = {128, 32, 16};
            static constexpr int kDiv[3] = {4, 3, 2};
            int cell = kCell[p.k];
            int f = (p.phase / kDiv[p.k]) % 24;
            image = 0x6e + p.k;
            su = static_cast<float>((f % 12) * cell);
            sv = static_cast<float>((f / 12) * cell);
            ss = static_cast<float>(cell);
            dx = std::trunc(p.x);
            dy = std::trunc(p.y);
            size = static_cast<float>(cell);
            prio = p.k == 2 ? 1 : 10;
            twoCopies = true;
            break;
        }
        case 2: {
            p.x += p.sx * (static_cast<float>(sinAt(p.phase + 64)) / 4096.0f + wind_ / 10.0f);
            p.y -= p.sy * fall_ / 10.0f;
            bool gone = false;
            if (p.y < -32) gone = true, p.y += 752;
            if (p.x >= 1280) gone = true, p.x -= 1312;
            if (p.x < -32) gone = true, p.x += 1312;
            if (gone && i >= count_) {
                p.active = false;
                --live_;
                continue;
            }
            static constexpr int kPeriod[3] = {23, 20, 17};
            int k3 = p.k % 3;
            int gp = triangle(p.phase, kPeriod[k3]) * 64 / kPeriod[k3];
            gfx_.makeImage(obj, 0x6e, 0, true);
            size = p.k < 3 ? 16.0f : 8.0f;
            su = k3 == 0 ? 32.0f : k3 == 1 ? 32.0f : 0.0f;
            sv = k3 == 0 ? 32.0f : 0.0f;
            gfx_.setRect(obj, std::trunc(p.x), std::trunc(p.y), su, sv, 32, 32);
            gfx_.setDest(obj, std::trunc(p.x), std::trunc(p.y), size, size);
            gfx_.setColor(obj, 0xff, gp + 128, gp + 64, gp + 64);
            gfx_.setBlend(obj, 3);
            gfx_.setFilter(obj, 1);
            gfx_.setMask(obj, 2);
            static constexpr int kLayers[3] = {0x1034, 0x1039, 0x1043};
            if (weatherLayer_ >= 0 && weatherLayer_ < 3) gfx_.setPriority(obj, kLayers[weatherLayer_]);
            ++p.phase;
            continue;
        }
        case 3: {
            bool wide = (weatherFlags_ & 0x800) != 0;
            p.x += p.sx * wind_ / 10.0f + static_cast<float>(sinAt(p.phase + 64)) * p.sx * fall_ / (4096.0f * 40.0f);
            if (wide) p.x += (p.x0 - 640.0f) / (5120.0f / (257.0f - p.z));
            p.y += p.sy * fall_ / 10.0f;
            int k = p.k;
            if (p.y > 720) {
                if (i >= count_) {
                    p.active = false;
                    --live_;
                    continue;
                }
                snowFlake(i, false);
            }
            float right = static_cast<float>((896 - k) * 2), left = static_cast<float>(4 * k - 1024);
            if (p.x >= right) {
                if (i >= count_) {
                    p.active = false;
                    --live_;
                    continue;
                }
                if (wide) snowFlake(i, false);
                else p.x -= right;
            } else if (p.x < left) {
                if (i >= count_) {
                    p.active = false;
                    --live_;
                    continue;
                }
                if (wide) snowFlake(i, false);
                else p.x += right;
            }
            k = p.k;
            float z = p.z;
            if (wide) alpha = 0xc0;
            else if (z <= 16) alpha = 0x20;
            else if (z <= 240) alpha = 0x80;
            else alpha = static_cast<int>((256.0f - z) * 8.0f + 32.0f);
            if (k > 240) size = (257.0f - z) * (wide ? 2.0f : 4.0f);
            else size = (257.0f - z) * 2.0f;
            if (k <= 240) {
                dx = std::trunc(p.x);
                dy = std::trunc(p.y);
            } else {
                int ph = p.phase % 4096;
                dx = std::trunc(p.x + static_cast<float>(sinAt(ph)) * static_cast<float>(p.phase) * m / 409600.0f);
                dy = std::trunc(p.y + static_cast<float>(sinAt(ph + 64)) * static_cast<float>(p.phase) * m / 409600.0f);
            }
            su = 0;
            sv = 32;
            ss = 32;
            prio = k < 0xf1 ? 10 : 1;
            twoCopies = true;
            break;
        }
        case 4:
        case 5: {
            if (p.sx < 360) p.sx += 0.2f;
            else p.sx = 0;
            if (weatherType_ == 4) {
                p.y += (p.y + 100.0f + fall_) / 400.0f;
                if (p.y >= 1400) {
                    if (i >= count_) {
                        p.active = false;
                        --live_;
                        continue;
                    }
                    burstParticle(i, false, true);
                    p.y = static_cast<float>(rnd.next() % 100 + 100);
                }
            } else {
                p.y -= (p.y + 100.0f + fall_) / 400.0f;
                if (p.y <= 100) {
                    if (i >= count_) {
                        p.active = false;
                        --live_;
                        continue;
                    }
                    burstParticle(i, false, true);
                }
            }
            float r = p.y;
            double ang = static_cast<double>(p.x) + std::sin(static_cast<double>(p.sx) / 3.141592) / (r * 0.5 + 1.0);
            int ph = p.phase % 4096;
            dx = std::trunc(std::trunc(640.0f + static_cast<float>(std::cos(ang)) * r * p.z) +
                            static_cast<float>(sinAt(ph)) * static_cast<float>(p.phase) * m / 409600.0f);
            dy = std::trunc(std::trunc(360.0f + static_cast<float>(std::sin(ang)) * r * p.z) +
                            static_cast<float>(sinAt(ph + 64)) * static_cast<float>(p.phase) * m / 409600.0f);
            float a;
            if (r < 450) a = (r - 200.0f) / 2.0f;
            else if (r < 1300) a = std::clamp(125.0f - (r - 450.0f) / 4.0f, 32.0f, 255.0f);
            else a = 32.0f - 0.32f * (r - 1300.0f);
            alpha = std::clamp(static_cast<int>(a), 0, 255);
            size = static_cast<float>(5 - p.k) * r / (200.0f * std::abs(p.z));
            su = 0;
            sv = 32;
            ss = 32;
            break;
        }
        case 6: {
            if (p.sx < 360) p.sx += static_cast<float>(p.k + 2) / 10.0f * wind_ / 10.0f;
            else p.sx = 0;
            float v = p.sx + p.sy * static_cast<float>(p.phase) / 5.0f * wind_ / 10.0f;
            v = v - std::trunc(v / 360.0f) * 360.0f;
            double th = static_cast<double>(v) * 3.141592653589793 / 180.0;
            int ph = p.phase % 4096;
            dx = std::trunc(std::trunc(640.0f - static_cast<float>(std::cos(th)) * p.x0 * fall_ / 2.0f) +
                            static_cast<float>(sinAt(ph)) * static_cast<float>(p.phase) * m / 409600.0f);
            dy = std::trunc(std::trunc(360.0f + static_cast<float>(std::sin(th)) * p.x0 * fall_ / 2.0f) +
                            static_cast<float>(sinAt(ph + 64)) * static_cast<float>(p.phase) * m / 409600.0f);
            if (dx < (-150.0f - m) * 2 || dx >= 2 * m + 1580 || dy < (-150.0f - m) * 2 || dy >= 2 * m + 1020) {
                p.active = false;
                --live_;
                continue;
            }
            p.y0 += static_cast<float>(p.k + 2) / 1000.0f * fall_ / 20.0f;
            p.x0 += p.y0;
            float q = p.x0 * fall_;
            alpha = q < 450 ? std::clamp(static_cast<int>(q - 30.0f), 0, 200) : std::clamp(static_cast<int>(200.0f - (q - 450.0f) / 5.0f), 0, 200);
            size = static_cast<float>(5 - p.k) * std::max(p.x0, 1.0f) / 4.0f;
            su = 0;
            sv = 32;
            ss = 32;
            break;
        }
        default:
            continue;
        }
        // Placement (§5.4).
        auto place = [&](int o, int target, int priority) {
            gfx_.makeImage(o, image, priority, true);
            gfx_.setSource(o, su, sv, ss, ss);
            gfx_.setDest(o, dx, dy, size, size);
            gfx_.setAlpha(o, alpha);
            gfx_.setFilter(o, 1);
            gfx_.setMask(o, 2);
            if (target >= 0) gfx_.setTarget(o, target);
        };
        if (weatherMode_ == 0) {
            if (twoCopies) {
                place(obj, kTargetSlot + other, prio);
                bool drop = inTransition && (weatherType_ != 3 || !scene.keepsCharacters());
                if (!drop) place(obj + 400, kTargetSlot + cur, prio);
            } else {
                place(obj, kTargetSlot + (inTransition ? other : cur), 10);
            }
        } else {
            static constexpr int kLayers[3] = {0x1034, 0x1039, 0x1043};
            int priority = weatherType_ == 1 || weatherType_ == 3 ? 0x1043
                           : weatherLayer_ >= 0 && weatherLayer_ < 3 ? kLayers[weatherLayer_] : 0x1043;
            place(obj, -1, priority);
            const auto& t = scene.tone();
            gfx_.setTone(obj, t[0], t[1], t[2]);
        }
        ++p.phase;
    }
}

// ---- Free pictures -----------------------------------------------------------------------------------

template <typename F>
void Effects::bothObjects(int slot, F f) {
    f(kPicObj + slot);
    if (pictures_[static_cast<size_t>(slot)].fixed == 0) f(kPicTwin + slot);
}

void Effects::loadPicture(int slot, const std::string& file, int layer, int colourMode, const std::string& amp, int level,
                          const std::string& group, int fixed) {
    // 0x424f90
    if (slot < 0 || slot >= 32) return;
    if (!file.empty() && (file[0] == 'v' || file[0] == 'V' || file[0] == 'h' || file[0] == 'H')) {
        int n = std::atoi(file.c_str() + 1);
        game_.system.setGalleryFlag(file[0] == 'v' || file[0] == 'V' ? 0 : 1, n, 1);
        game_.writeSystem();
    }
    releasePicture(slot);
    Scene& scene = game_.scene();
    std::string amp1, amp2;
    int lv = level;
    if (colourMode == 0) {
        amp1 = scene.bgEffect;
        lv = scene.bgLevel;
    } else if (colourMode == 1) {
        amp1 = scene.characterAmp();
        amp2 = scene.charEffect;
        lv = scene.charLevel;
    } else {
        amp1 = amp;
    }
    int image = kPicImage + slot;
    if (!gfx_.load(image, file, 2, false, amp1, amp2, lv, group)) {
        game_.noteMissing("picture " + file);
        return;
    }
    int w = 0, h = 0;
    gfx_.pictureSize(image, w, h);
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p = Picture{};
    p.used = true;
    p.disp = true;
    p.layer = layer;
    p.fixed = fixed;
    p.w = p.fullW = w;
    p.h = p.fullH = h;
    p.file = file;
    p.colourMode = colourMode;
    p.amp = amp;
    p.group = group;
    p.level = level;
    int cur = scene.currentBuffer();
    auto make = [&](int obj, int prio, int target) {
        gfx_.makeImage(obj, image, prio, true);
        gfx_.setFilter(obj, 1);
        gfx_.setRect(obj, 0, 0, 0, 0, static_cast<float>(w), static_cast<float>(h));
        if (target >= 0) gfx_.setTarget(obj, target);
    };
    if (fixed != 0) {
        make(kPicObj + slot, 0x1035 + layer, -1);
    } else {
        make(kPicObj + slot, layer, kTargetSlot + cur);
        make(kPicTwin + slot, layer, kTargetSlot + (cur == 0 ? 1 : 0));
    }
}

void Effects::loadAnime(int slot, const std::string& file, int layer, const std::string& amp, int level, const std::string& group,
                        int fixed) {
    // 0x4252d0
    if (slot < 0 || slot >= 32) return;
    releasePicture(slot);
    if (!game_.ani().load(kPicAni + slot, file, 1, amp, "", level, group)) {
        game_.noteMissing("animation " + file);
        return;
    }
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p = Picture{};
    p.used = true;
    p.disp = true;
    p.kind = 1;
    p.layer = layer;
    p.fixed = fixed;
    p.mode = 1;
    p.file = file;
    p.amp = amp;
    p.group = group;
    p.level = level;
    game_.ani().makeObject(kPicObj + slot, kPicAni + slot, 0, true, 0x1035 + layer, true, 0, 1, 0);
    gfx_.setFilter(kPicObj + slot, 1);
}

void Effects::releasePicture(int slot) {
    if (slot < 0 || slot >= 32) return;
    Picture& p = pictures_[static_cast<size_t>(slot)];
    if (!p.used) return;
    if (p.kind == 1) {
        game_.ani().releaseInstance(gfx_.objects[static_cast<size_t>(kPicObj + slot)].slot);
        game_.ani().release(kPicAni + slot);
    }
    gfx_.release(kPicImage + slot);
    gfx_.remove(kPicObj + slot);
    gfx_.remove(kPicTwin + slot);
    p = Picture{};
}

void Effects::pictureVisible(int slot, bool on) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) { gfx_.setVisible(o, on); });
    pictures_[static_cast<size_t>(slot)].disp = on;
}

void Effects::pictureLayer(int slot, int layer) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) { gfx_.setPriority(o, 0x1035 + layer); });
    pictures_[static_cast<size_t>(slot)].layer = layer;
}

void Effects::pictureParam(int slot, int blend, int alpha, int frames) {
    if (slot < 0 || slot >= 32) return;
    Picture& p = pictures_[static_cast<size_t>(slot)];
    if (!p.used) return;
    bothObjects(slot, [&](int o) { gfx_.setBlend(o, static_cast<uint32_t>(blend)); });
    p.blend = blend;
    if (frames == 0) {
        p.alpha = std::clamp(alpha, 0, 255);
        bothObjects(slot, [&](int o) { gfx_.setAlpha(o, p.alpha); });
        p.frames = 0;
    } else {
        p.start = p.alpha;
        p.target = alpha;
        p.counter = 0;
        p.frames = frames;
    }
}

void Effects::pictureMirror(int slot, int flags) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) { gfx_.setMirror(o, static_cast<uint32_t>(flags)); });
    pictures_[static_cast<size_t>(slot)].mirror = flags;
}

void Effects::pictureBright(int slot, int r, int g, int b) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) { gfx_.setTone(o, r, g, b); });
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p.r = r, p.g = g, p.b = b;
}

void Effects::pictureMove(int slot, int x, int y) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) { gfx_.moveTo(o, static_cast<float>(x), static_cast<float>(y)); });
    pictures_[static_cast<size_t>(slot)].x = x;
    pictures_[static_cast<size_t>(slot)].y = y;
}

void Effects::picturePos(int slot, int x, int y, int sx, int sy, int w, int h) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) {
        gfx_.setRect(o, static_cast<float>(x), static_cast<float>(y), static_cast<float>(sx), static_cast<float>(sy),
                     static_cast<float>(w), static_cast<float>(h));
    });
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p.x = x, p.y = y, p.sx = sx, p.sy = sy, p.w = w, p.h = h;
}

void Effects::pictureZoom(int slot, int x, int y, int w, int h) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) {
        gfx_.setDest(o, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
    });
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p.x = x, p.y = y, p.w = w, p.h = h;
}

void Effects::pictureZoom2(int slot, int cx, int cy, float zoom) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    bothObjects(slot, [&](int o) {
        gfx_.setPivot(o, static_cast<float>(cx), static_cast<float>(cy));
        gfx_.setScale(o, zoom);
    });
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p.cx = cx, p.cy = cy, p.zoom = zoom;
}

void Effects::pictureRoll(int slot, int cx, int cy, int a4096) {
    if (slot < 0 || slot >= 32 || !pictures_[static_cast<size_t>(slot)].used) return;
    float angle = static_cast<float>(a4096) * 360.0f / 4096.0f;
    bothObjects(slot, [&](int o) {
        gfx_.setPivot(o, static_cast<float>(cx), static_cast<float>(cy));
        gfx_.setRotation(o, angle);
    });
    Picture& p = pictures_[static_cast<size_t>(slot)];
    p.cx = cx, p.cy = cy, p.angle = angle;
}

void Effects::animePlay(int slot, int seq, int play, int mode) {
    if (slot < 0 || slot >= 32 || pictures_[static_cast<size_t>(slot)].kind != 1) return;
    game_.ani().setPlay(gfx_.objects[static_cast<size_t>(kPicObj + slot)].slot, kPicAni + slot, seq, play != 0, 1, mode, 0);
    pictures_[static_cast<size_t>(slot)].seq = seq;
    pictures_[static_cast<size_t>(slot)].mode = mode;
}

void Effects::picturesStep() {
    // 0x424c30: fades only (the engine never interpolates moves or zooms).
    for (int slot = 0; slot < 32; ++slot) {
        Picture& p = pictures_[static_cast<size_t>(slot)];
        if (!p.used || p.frames == 0) continue;
        int n = game_.fr(p.frames);
        int a;
        if (p.counter < n) {
            a = ((n - p.counter) * p.start + p.target * p.counter) / n;
            ++p.counter;
        } else {
            a = p.target;
            p.alpha = p.target;
            p.counter = 0;
            p.frames = 0;
        }
        bothObjects(slot, [&](int o) { gfx_.setAlpha(o, a); });
    }
}

// ---- The date card -----------------------------------------------------------------------------------

void Effects::calendar(int year, int month, int day, int weekday, int show) {
    // 0x401320
    if (weekday == -1) {
        int y = year, m = month;
        if (month <= 2) {
            --y;
            m += 12;
        }
        weekday = (y / 4 + (13 * m + 8) / 5 + y / 400 - y / 100 + y + day) % 7;
    }
    year_ = year;
    month_ = month;
    day_ = day;
    weekday_ = weekday;
    if (show == 0) return;
    // The card's art: calender_01 when the script global FLG_00 is 2.
    int flg = 0;
    for (const GlobalVar& g : game_.globals()) {
        if (g.name == "FLG_00") flg = g.i;
    }
    // Special Contents (0x401300) names the translators' English card calender_0e.
    const char* plain = game_.assets().build() == Build::Special ? "calender_0e.tga" : "calender_00.tga";
    gfx_.load(0x195, flg == 2 ? "calender_01.tga" : plain, 2, false, "", "", 0x100, "grp");
    gfx_.load(0x197, "calender_10.tga", 2, false, "", "", 0x100, "grp");
    gfx_.load(0x198, "calender_11.tga", 2, false, "", "", 0x100, "grp");
    gfx_.makeImage(0x53a, 0x195, 0x1061, false);
    gfx_.setRect(0x53a, 0, 0, 0, 0, 1280, 720);
    for (int obj : {0x541, 0x542}) {
        gfx_.makePolygon(obj, 4, 0x1062, false);
        gfx_.setColor(obj, 0x80, 0x15, 0x25, 0x53);
    }
    for (int obj = 0x544; obj <= 0x549; ++obj) gfx_.makeImage(obj, obj == 0x549 ? 0x198 : 0x197, 0x1062, false);
    calPhase_ = 0;
    calA_ = calB_ = calT_ = 0;
}

void Effects::calendarStep() {
    // 0x40b380
    if (calPhase_ < 0) return;
    int D = game_.skipActive() ? 60 / 3 : 60;  // 0x40b050
    int s = calT_ / 2;
    auto digits = [&](int a) {
        auto cell = [](int d) { return static_cast<float>(d * 40); };
        int mt = month_ / 10, mu = month_ % 10, dt = day_ / 10, du = day_ % 10;
        auto place = [&](int obj, float u, float w, float x, float y, float h) {
            gfx_.setRect(obj, x - static_cast<float>(s), y, u, 0, w, h);
            gfx_.setVisible(obj, true);
            gfx_.setAlpha(obj, a);
        };
        place(0x544, month_ < 10 ? 400.0f : cell(mt), 40, 276, 176, 48);
        place(0x545, cell(mu), 40, 316, 176, 48);
        place(0x546, 440, 32, 356, 176, 48);
        place(0x547, day_ < 10 ? cell(du) : cell(dt), 40, 388, 176, 48);
        place(0x548, day_ < 10 ? 400.0f : cell(du), 40, 428, 176, 48);
        place(0x549, static_cast<float>(((weekday_ % 7) + 7) % 7 * 104), 104, 476, 192, 32);
    };
    int half = D / 2;
    switch (calPhase_) {
    case 0:
        digits(0xff);
        calPhase_ = 1;
        calA_ = calB_ = 0;
        [[fallthrough]];
    case 1:
        gfx_.setVisible(0x53a, true);
        if (calA_ < half) {
            gfx_.setAlpha(0x53a, calA_ * 256 / half);
        } else {
            gfx_.setAlpha(0x53a, 0xff);
            calPhase_ = 2;
            calA_ = calB_ = 0;
        }
        digits(0xff);
        break;
    case 2: {
        gfx_.setVisible(0x541, true);
        gfx_.setVisible(0x542, true);
        int L = calA_ >= half ? 1280 : calB_ * 1280 / half;
        gfx_.setDest(0x541, 167, 0, 1, static_cast<float>(calA_ >= half ? 720 : L));
        gfx_.setDest(0x542, 0, 235, static_cast<float>(L), 1);
        digits(0xff);
        if (calA_ >= half) {
            calPhase_ = 3;
            calA_ = calB_ = 0;
        }
        break;
    }
    case 3:
        digits(0xff);
        if (calA_ >= D) {
            calPhase_ = 4;
            calA_ = calB_ = 0;
        }
        break;
    case 4: {
        int f = calB_ * 1280 / half;
        gfx_.setDest(0x541, 167, static_cast<float>(f), 1, 720);
        gfx_.setDest(0x542, static_cast<float>(f), 235, 1280, 1);
        gfx_.setAlpha(0x53a, std::clamp(256 - calA_ * 256 / D, 0, 255));
        digits(std::clamp(256 - calA_ * 512 / D, 0, 255));
        if (calA_ >= D) {
            for (int obj = 0x53a; obj <= 0x542; ++obj) gfx_.remove(obj);
            for (int obj = 0x544; obj <= 0x549; ++obj) gfx_.remove(obj);
            calPhase_ = 5;
            calA_ = calB_ = 0;
        }
        break;
    }
    case 5:
        for (int sl = 0x195; sl <= 0x198; ++sl) gfx_.release(sl);
        for (int obj = 0x53a; obj <= 0x543; ++obj) gfx_.remove(obj);
        calPhase_ = -1;
        return;
    default:
        break;
    }
    ++calA_;
    ++calB_;
    ++calT_;
}

// ---- Saves ------------------------------------------------------------------------------------------

void Effects::captureShake(SaveRecord::Shake& s) const {
    // 0x4234c0: nothing unless a shake runs without an end.
    s = {};
    if (!shakeOn_ || shakeDur_ != 0) return;
    s = {true, shakeDir_, shakeAmp_, shakeType_, shakeSpeed_};
}

void Effects::captureWeather(SaveRecord::Weather& w) const {
    w = {weatherOn_, weatherType_, windTarget_, fallTarget_, count_, weatherMode_};
}

void Effects::capturePictures(std::array<SaveRecord::Picture, 32>& out) const {
    for (size_t i = 0; i < 32; ++i) {
        const Picture& p = pictures_[i];
        SaveRecord::Picture& r = out[i];
        r = {};
        if (!p.used) continue;
        r.used = true;
        r.disp = p.disp;
        r.kind = p.kind;
        r.file = p.file;
        r.amp = p.amp;
        r.group = p.group;
        r.colourMode = p.colourMode;
        r.level = p.level;
        r.blend = p.blend;
        r.mirror = p.mirror;
        r.alpha = p.alpha;
        r.r = p.r;
        r.g = p.g;
        r.b = p.b;
        r.layer = p.layer;
        r.fixed = p.fixed;
        r.x = p.x, r.y = p.y, r.w = p.w, r.h = p.h, r.sx = p.sx, r.sy = p.sy, r.fullW = p.fullW, r.fullH = p.fullH;
        r.zoom = p.zoom;
        r.cx = p.cx, r.cy = p.cy;
    }
}

void Effects::captureCalendar(int& year, int& month, int& day, int& weekday) const {
    year = year_, month = month_, day = day_, weekday = weekday_;
}

void Effects::restorePicture(int slot, const SaveRecord::Picture& p) {
    // 0x425640
    if (!p.used || (p.kind != 0 && p.kind != 1)) return releasePicture(slot);
    if (p.kind == 0) loadPicture(slot, p.file, p.layer, p.colourMode, p.amp, p.level, p.group, p.fixed);
    else loadAnime(slot, p.file, p.layer, p.amp, p.level, p.group, p.fixed);
    pictureVisible(slot, p.disp);
    pictureParam(slot, p.blend, p.alpha, 0);
    pictureMirror(slot, p.mirror);
    pictureBright(slot, p.r, p.g, p.b);
    picturePos(slot, p.x, p.y, p.sx, p.sy, p.w, p.h);
    if (p.zoom != 0) return pictureZoom2(slot, p.cx, p.cy, p.zoom);
    if (p.w != p.fullW || p.h != p.fullH) pictureZoom(slot, p.x, p.y, p.w, p.h);
}

}  // namespace nyan::wa2
