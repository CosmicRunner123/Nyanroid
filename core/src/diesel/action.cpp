#include "nyan/diesel/action.hpp"

#include <cmath>

namespace nyan::diesel {

namespace {

// MSVC's SSE2 float routines the curves call (powf 0x5bf280, sinf 0x5bf933, cosf 0x5befdc); the
// host's give the same values to within rounding.
inline float powf(float a, float b) { return std::pow(a, b); }
inline float sinf(float a) { return std::sin(a); }
inline float cosf(float a) { return std::cos(a); }

#include "easing_lifted.inc"

constexpr int kCurves = sizeof(kEasing) / sizeof(kEasing[0]);

// An unsigned 32-bit value as the exes convert it to float (int, plus 2^32 when negative).
float unsignedToFloat(int32_t v) { return static_cast<float>(static_cast<double>(v) + (v < 0 ? 4294967296.0 : 0.0)); }

}  // namespace

float easing(int32_t index, float src, float dst, float duration, float time) {
    if (index < 0 || index >= kCurves) index = 0;
    return kEasing[index](src, dst, duration, time);
}

void FrameClock::beginFrame() {
    uint32_t now = milliTime;
    float elapsed = unsignedToFloat(static_cast<int32_t>(now - lastMilli));
    float speed = elapsed * 0.06f;
    frameSpeed = speed < 0.0f ? 0.0f : (speed >= 5.0f ? 5.0f : speed);
    maxFrameTime = elapsed < 0.0f ? 0.0f : (elapsed >= 83.33333587646484f ? 83.33333587646484f : elapsed);
    lastMilli = now;
}

float Action1D::curve() const { return easing(tempo, src, dst, actionTime, timer); }

void Action1D::refresh() {
    if (active) {
        nowNoPower = curve();
        now = power * nowNoPower;
    }
}

void Action1D::set(float from, float to, int32_t timeMs, int32_t curveIndex) {
    lastStamp = 0;  // +0x08 (0x54d950 clears it)
    nowNoPower = from;
    now = power * from;
    timer = 0;
    src = from;
    dst = to;
    if (curveIndex > 0xa0) curveIndex = 0;
    tempo = curveIndex;
    actionTime = unsignedToFloat(timeMs);
    if (actionTime == 0.0f) {
        active = 0;
        nowNoPower = to;
        src = to;
        now = power * to;
    } else if (from != to) {
        active = 1;
    }
}

bool Action1D::step(const FrameClock& clock) {
    if (!active) return false;
    float stamp = static_cast<float>(clock.milliTime);
    if (lastStamp != stamp) {
        // frame speed * 16.666666 (ms per frame at 60 Hz) * time scale, within [0, max frame time].
        float dt = clock.frameSpeed * 16.66666603088379f * timeScale;
        float step = 0.0f;
        if (dt >= 0.0f) {
            step = clock.maxFrameTime;
            if (step <= 0.0f) step = 0.0f;
            if (dt <= step) step = dt;
        }
        timer = timer + step;
        lastStamp = stamp;
    }
    nowNoPower = curve();
    now = power * nowNoPower;
    if (actionTime <= timer) {
        nowNoPower = dst;
        active = 0;
        timer = actionTime;
        now = dst * power;
    }
    return true;
}

bool Action1D::finish() {
    if (!active) return false;
    active = 0;
    nowNoPower = dst;
    now = power * dst;
    return true;
}

void Action1D::setTimer(float ms) {
    timer = ms;
    timer = ms >= 0.0f ? (ms <= actionTime ? ms : actionTime) : 0.0f;
    if (actionTime > 0.0f) active = 1;
    refresh();
}

void Action1D::resetTimer() {
    // 0x54dec0: timer = min(actionTime, 0) (0 for any real duration); active again if it has one.
    timer = actionTime >= 0.0f ? 0.0f : actionTime;
    if (actionTime > 0.0f) active = 1;
    refresh();
    refresh();
}

void Action1D::setActionTime(int32_t ms) {
    actionTime = unsignedToFloat(ms);
    refresh();
}

void Action1D::setSrc(float v) {
    src = v;
    refresh();
}

void Action1D::setDst(float v) {
    dst = v;
    refresh();
}

void Action1D::setNow(float v) {
    nowNoPower = v;
    now = power * v;
}

void Action1D::setPower(float v) {
    power = v;
    if (active) {
        refresh();
    } else {
        nowNoPower = dst;
        now = dst * v;
    }
}

void Action1D::setTempo(int32_t t) {
    tempo = t;
    refresh();
}

}  // namespace nyan::diesel
