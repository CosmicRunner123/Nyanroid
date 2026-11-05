#include "nyan/diesel/sprite.hpp"

#include <cmath>

#include <algorithm>

namespace nyan::diesel {

SpriteImage::~SpriteImage() {
    setParent(nullptr);
    for (SpriteImage* c : children) c->parent = nullptr;
}

void SpriteImage::setTimeScale(float s) {
    timeScale = s;
    forEachTween([s](Tween& t) { t.timeScale = s; });
    if (shake) shake->setTimeScale(s);
}

void SpriteImage::update(const FrameClock& clock) {
    volumeMoved = volume.step(clock);
    forEachTween([this, &clock](Tween& t) {
        if (&t != &volume && &t != &bezier.t && &t != &spline.t && &t != &split.t) t.step(clock);
    });
    // The texture scroll (0x453280, after the scroll's tween steps): while its value is non-zero,
    // value x the tween's time scale (+0x634) is added to the UV offset (+0x5f4) once per update.
    if (scroll.now[0] != 0.0f || scroll.now[1] != 0.0f) {
        uvOffsetX = scroll.now[0] * scroll.timeScale + uvOffsetX;
        uvOffsetY = scroll.now[1] * scroll.timeScale + uvOffsetY;
    }
    // The ripple's and raster's phases run while their power, frequency and speed are all non-zero
    // (0x453280): by the frame speed x speed x time scale, then back by their whole part once that
    // reaches 1 either way.
    auto advance = [&clock, this](float& phase, const Tween& power, const Tween& frequency, const Tween& speed) {
        if (power.now[0] == 0.0f || frequency.now[0] == 0.0f || speed.now[0] == 0.0f) return;
        phase = clock.frameSpeed * speed.now[0] * timeScale + phase;
        if (std::fabs(phase) >= 1.0f) phase -= static_cast<float>(static_cast<int32_t>(phase));
    };
    advance(ripplePhase, ripplePower, rippleFrequency, rippleSpeed);
    advance(rasterPhase, rasterPower, rasterFrequency, rasterSpeed);
    // The bezier puts the position where its path is (0x453280).
    if (bezier.step(clock)) move.set(bezier.point, bezier.point, 0, 0);
    if (spline.step(clock)) move.set(spline.point, spline.point, 0, 0);
    // The split's tween, or its run past the end (0x453280); then the pieces' places (0x453960).
    split.step(clock, timeScale);
    split.place();
    // The shake steps while it shakes (0x453280).
    if (shake && shake->shaking()) shake->update(clock);
}

// CBezier vtable +0 (0x45ebe0): the tween steps; while it ran, the point is worked out again.
bool SpriteImage::Bezier::step(const FrameClock& clock) {
    bool ran = t.step(clock);
    if (ran) evaluate();
    return ran;
}

// 0x4611e0: with a whole number of four-point segments, the point of the tween's value (now: the
// last point when it is not running, or at 1): segment k = t / (1 / segments), u its fraction, the
// cubic's weights v³, 3v²u, 3vu², u³ (v = 1 − u) in the exe's order of operations.
void SpriteImage::Bezier::evaluate() {
    size_t n = points.size();
    if (n < 4 || n % 4 != 0) return;
    if (!t.active) {
        point = points.back();
        return;
    }
    float segs = static_cast<float>(n / 4);
    float tv = t.now[0];
    int k = static_cast<int>(tv / (1.0f / segs));
    if (k >= static_cast<int>(n / 4)) {
        point = points.back();
        return;
    }
    float u = std::fmod(tv, 1.0f / segs) * segs;
    u = u >= 0.0f ? (u <= 1.0f ? u : 1.0f) : 0.0f;
    float v = 1.0f - u;
    float w0 = v * v * v;
    float v3 = v * 3.0f;
    float w1 = v3 * v * u;
    float w2 = v3 * u * u;
    float w3 = u * u * u;
    const auto& p0 = points[static_cast<size_t>(k) * 4];
    const auto& p1 = points[static_cast<size_t>(k) * 4 + 1];
    const auto& p2 = points[static_cast<size_t>(k) * 4 + 2];
    const auto& p3 = points[static_cast<size_t>(k) * 4 + 3];
    for (size_t i = 0; i < 3; ++i) {
        float c = p0[i] * w0;
        c = p1[i] * w1 + c;
        c = p2[i] * w2 + c;
        c = p3[i] * w3 + c;
        point[i] = c;
    }
}

void SpriteImage::setParent(SpriteImage* p) {
    if (parent) std::erase(parent->children, this);
    parent = p;
    if (p && std::find(p->children.begin(), p->children.end(), this) == p->children.end()) p->children.push_back(this);
}

size_t blurHistoryLimit(const SpriteImage::Blur& b) {
    return static_cast<uint32_t>(static_cast<float>(b.levels + (b.mode == 0 ? 1 : 0)) * static_cast<float>(b.interval) * 0.06f);
}

std::vector<size_t> blurPicks(const SpriteImage::Blur& b, size_t size, const std::function<bool(size_t, size_t)>& same) {
    std::vector<int64_t> pick;
    for (int32_t i = 0; i < b.levels; ++i) {
        auto k = static_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(b.levels - i) * b.interval) * 0.06f);
        if (b.mode == 1) pick.push_back(k < size ? static_cast<int64_t>(size - k - 1) : 0);
        else if (k < size) pick.push_back(static_cast<int64_t>(size - k - 1));
    }
    if (b.mode == 1) {
        for (auto& p : pick) {
            if (same(static_cast<size_t>(p), size - 1)) p = -1;
        }
    } else if (!pick.empty()) {
        if (pick.size() < static_cast<size_t>(b.levels) && pick.front() != 0) pick.insert(pick.begin(), 0);
        for (size_t j = 0; j + 1 < pick.size(); ++j) {
            if (same(static_cast<size_t>(pick[j + 1]), static_cast<size_t>(pick[j]))) pick[j] = -1;
        }
    }
    std::erase(pick, -1);
    std::vector<size_t> out;
    if (b.order) {
        for (auto p : pick) out.push_back(static_cast<size_t>(p));
        out.push_back(size - 1);
    } else {
        out.push_back(size - 1);
        for (auto it = pick.rbegin(); it != pick.rend(); ++it) out.push_back(static_cast<size_t>(*it));
    }
    return out;
}

float blurWeight(const SpriteImage::Blur& b, size_t i, size_t count) {
    if (!b.fade) return b.power;
    float n = static_cast<float>(count);
    float place = b.order == 0 ? static_cast<float>(static_cast<int64_t>(i) - 1) : static_cast<float>(count - i - 1);
    return (1.0f - place / n) * b.power;
}

}  // namespace nyan::diesel
