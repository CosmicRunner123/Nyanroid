#include "nyan/diesel/shake.hpp"

#include <cmath>

namespace nyan::diesel {

namespace {

using Vec = std::array<float, 3>;

// An unsigned int as the exe makes it a float (through a double, 2^32 added when the top bit is set).
float u2f(uint32_t v) { return static_cast<float>(static_cast<double>(v)); }
// A float time to the unsigned milliseconds a tween takes (0x5be400: truncated).
uint32_t toUint(float v) { return v > 0.0f ? static_cast<uint32_t>(v) : 0u; }

// Mware::Vector3D's operators as the exe chains them: -v, * s, * a, / b.
Vec swingTarget(const Vec& amplitude, float sign, float a, float b) {
    Vec out{};
    for (size_t i = 0; i < 3; ++i) out[i] = -amplitude[i] * sign * a / b;
    return out;
}

constexpr Vec kZero{};

}  // namespace

void Shake::swingTo(const Vec& from, const Vec& to, uint32_t ms) {
    swing_.set(from, to, static_cast<int32_t>(ms), beatTempo_);
}

Vec Shake::randomTarget() {
    Vec r = amplitude_.now;
    for (auto& c : r) c = rng_.nextFloat(-1.0f, 1.0f) * c;
    return r;
}

void Shake::set(const Vec& start, const Vec& end, uint32_t frequency, int32_t interpolation, uint32_t time,
                int32_t tempo, int32_t beatTempo) {
    // The master timer: 0 to 1 over the time, linear; no time: done at once.
    master_.nowNoPower[0] = 0.0f;
    master_.src[0] = 0.0f;
    master_.dst[0] = 1.0f;
    master_.lastStamp = 0.0f;
    master_.now[0] = master_.power * 0.0f;
    master_.timer = 0.0f;
    master_.tempo = 0;
    master_.actionTime = u2f(time);
    if (master_.actionTime == 0.0f) {
        master_.active = 0;
        master_.nowNoPower[0] = 1.0f;
        master_.now[0] = master_.power;
        master_.src[0] = 1.0f;
    } else {
        master_.active = 1;
    }
    amplitude_.set(start, end, static_cast<int32_t>(time), tempo);
    beatTempo_ = beatTempo;
    if (random_) {
        Vec r = randomTarget();
        if (frequency != 0) edge_ = time % frequency;
        swingTo(swing_.now, r, frequency);
    } else if (frequency == 0) {
        Vec a = amplitude_.now;
        swingTo(a, swingTarget(a, 1.0f, 1.0f, 1.0f), 0);
    } else {
        // The first swing, from 0 to the far side, takes half the remainder (or half a swing), the
        // amplitude taken where it will be then.
        uint32_t r = time % frequency;
        if (r == 0) r = frequency;
        edge_ = r >> 1;
        amplitude_.setTimer(u2f(edge_));
        Vec target = swingTarget(amplitude_.now, sign_, u2f(edge_ * 2), u2f(r));
        swingTo(kZero, target, edge_);
        amplitude_.setTimer(0.0f);
    }
    interpolate_ = interpolation != 0;
    frequency_ = frequency;
    final_ = false;
}

void Shake::update(const FrameClock& clock) {
    master_.step(clock);
    amplitude_.step(clock);
    swing_.step(clock);
    float left = master_.actionTime - master_.timer;
    if (random_) {
        if (!final_ && left <= u2f(edge_)) {
            final_ = true;
            swingTo(swing_.now, kZero, toUint(left));
        }
        if (swing_.active) return;
        Vec r = randomTarget();
        if (left <= u2f(frequency_ + edge_)) {
            float k = u2f(edge_) / u2f(frequency_);
            for (auto& c : r) c = k * c;
        }
        swingTo(swing_.now, r, toUint(swing_.actionTime));
        return;
    }
    if (final_) return;
    if (u2f(edge_) >= left) {
        final_ = true;
        swingTo(swing_.now, kZero, toUint(left));
        return;
    }
    if (swing_.active) return;
    sign_ = -sign_;
    if (u2f(frequency_ + edge_) >= left) {
        // The last swing: shortened as the first was.
        swingTo(swing_.now, swingTarget(amplitude_.now, sign_, u2f(edge_ * 2), u2f(frequency_)), frequency_);
    } else {
        Vec from{};
        for (size_t i = 0; i < 3; ++i) from[i] = amplitude_.now[i] * sign_;
        swingTo(from, swingTarget(amplitude_.now, sign_, 1.0f, 1.0f), frequency_);
    }
}

void Shake::setTimer(float t) {
    float f = u2f(frequency_);
    float total = master_.actionTime;
    if (f == 0.0f || total == 0.0f) return;
    float edge = u2f(edge_);
    if (random_) {
        // Which swing the old and the new timer fall in; a new one gets a new random target.
        int32_t was = 0, now = 0;
        float within;
        if (edge <= 0.0f) {
            was = static_cast<int32_t>((1.0f / f) * master_.timer);
            now = static_cast<int32_t>((1.0f / f) * t);
            within = std::fmod(t, f);
        } else {
            float inv = 1.0f / f;
            int32_t n = static_cast<int32_t>((total - (edge + edge)) * inv);
            within = t;
            if (edge < t) {
                if (t < total - edge) {
                    float x = t - edge;
                    now = static_cast<int32_t>(x * inv);
                    within = std::fmod(x, f);
                } else {
                    now = n + 1;
                    within = t - (total - edge);
                }
            }
            float m = master_.timer;
            if (edge < m) was = m < total - edge ? static_cast<int32_t>((m - edge) * inv) : n + 1;
        }
        master_.setTimer(t);
        amplitude_.setTimer(t);
        if (was != now) {
            float left = total - master_.timer;
            if (edge < left) {
                Vec r = randomTarget();
                if (left <= u2f(edge_ + frequency_)) {
                    float k = edge / f;
                    for (auto& c : r) c = k * c;
                }
                swingTo(swing_.now, r, toUint(swing_.actionTime));
            } else {
                final_ = true;
                swingTo(swing_.now, kZero, toUint(left));
            }
        }
        swing_.setTimer(within);
        return;
    }
    master_.setTimer(t);
    amplitude_.setTimer(t);
    // Which swing the timer falls in (its parity gives the side) and how far into it.
    float within = 0.0f;
    uint32_t swingIndex = 0;
    if (edge > 0.0f) {
        uint32_t n = static_cast<uint32_t>(static_cast<int32_t>(total - (edge + edge))) / frequency_;
        if (edge >= t) {
            within = t;
        } else if (t >= total - edge) {
            within = t - (total - edge);
            swingIndex = n + 1;
        } else {
            float x = t - edge;
            swingIndex = static_cast<uint32_t>(static_cast<int32_t>(x / f));
            within = std::fmod(x, f);
        }
    } else {
        swingIndex = static_cast<uint32_t>(static_cast<int32_t>(t / f));
    }
    sign_ = (swingIndex & 1) ? -1.0f : 1.0f;
    swing_.setTimer(within);
    float left = total - master_.timer;
    if (edge >= left) {
        final_ = true;
        swingTo(swing_.now, kZero, toUint(left));
        swing_.setTimer(within);
        return;
    }
    final_ = false;
    if (u2f(frequency_ + edge_) >= left) {
        swingTo(swing_.now, swingTarget(amplitude_.now, sign_, u2f(edge_ * 2), f), frequency_);
        swing_.setTimer(within);
    } else {
        // The exe starts the first swing again here, from 0, for the amplitude at its end.
        amplitude_.setTimer(edge);
        Vec target = swingTarget(amplitude_.now, sign_, u2f(edge_ * 2), f);
        swing_.set(kZero, target, static_cast<int32_t>(edge_), beatTempo_);
        amplitude_.setTimer(t);
    }
}

void Shake::setTimeScale(float s) {
    master_.timeScale = s;
    amplitude_.timeScale = s;
    swing_.timeScale = s;
}

}  // namespace nyan::diesel
