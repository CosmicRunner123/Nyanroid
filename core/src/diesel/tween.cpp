#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

namespace {

float unsignedToFloat(int32_t v) { return static_cast<float>(static_cast<double>(v) + (v < 0 ? 4294967296.0 : 0.0)); }

}  // namespace

Tween::Tween(int components, float x, float y, float z) : n(components) {
    std::array<float, 3> v{x, y, z};
    src = dst = nowNoPower = v;
    for (int i = 0; i < 3; ++i) now[i] = power * v[i];
}

void Tween::set(const std::array<float, 3>& from, const std::array<float, 3>& to, int32_t timeMs, int32_t curve) {
    nowNoPower = from;
    for (int i = 0; i < n; ++i) now[i] = power * nowNoPower[i];
    src = from;
    dst = to;
    lastStamp = 0.0f;
    timer = 0.0f;
    actionTime = unsignedToFloat(timeMs);
    tempo = curve > 0xa0 ? 0 : curve;
    if (actionTime == 0.0f) {
        active = 0;
        nowNoPower = to;
        for (int i = 0; i < n; ++i) now[i] = power * to[i];
        src = to;
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (src[i] != dst[i]) {
            active = 1;
            return;
        }
    }
}

void Tween::refresh() {
    if (!active) return;
    for (int i = 0; i < n; ++i) {
        nowNoPower[i] = easing(tempo, src[i], dst[i], actionTime, timer);
        now[i] = power * nowNoPower[i];
    }
}

bool Tween::step(const FrameClock& clock) {
    if (!active) return false;
    float stamp = static_cast<float>(clock.milliTime);
    if (lastStamp != stamp) {
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
    for (int i = 0; i < n; ++i) {
        nowNoPower[i] = easing(tempo, src[i], dst[i], actionTime, timer);
        now[i] = power * nowNoPower[i];
    }
    if (actionTime <= timer) {
        timer = actionTime;
        nowNoPower = dst;
        for (int i = 0; i < n; ++i) now[i] = power * dst[i];
        active = 0;
    }
    return true;
}

void Tween::setTimer(float ms) {
    float t = ms >= 0.0f ? (ms <= actionTime ? ms : actionTime) : 0.0f;
    timer = t >= 0.0f ? (t <= actionTime ? t : actionTime) : 0.0f;
    if (actionTime > 0.0f) active = 1;
    refresh();
}

}  // namespace nyan::diesel
