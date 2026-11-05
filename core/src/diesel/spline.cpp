#include "nyan/diesel/spline.hpp"

#include <cmath>

namespace nyan::diesel {

namespace {

using V3 = std::array<float, 3>;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 plus(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float lengthSq(const V3& a) { return a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; }
// Normalised when its squared length is at least 1e-5 (0x5eedd0), else zero.
V3 direction(const V3& a) {
    float l2 = lengthSq(a);
    if (l2 < 9.99999975e-06f) return {};
    return mul(a, 1.0f / std::sqrt(l2));
}

constexpr float kMs = 0.001f;  // 0x5eedd8

}  // namespace

void TimedSpline::add(const std::array<float, 3>& p, int32_t timeMs, float weight) {
    keys_.push_back({p, static_cast<float>(timeMs), weight});
}

void TimedSpline::reset() {
    keys_.clear();
    entries_.clear();
    segment_ = -1;
    if (t.active) {
        t.active = 0;
        t.nowNoPower[0] = t.dst[0];
        t.now[0] = t.power * t.dst[0];
    }
}

void TimedSpline::restart(int32_t timeMs, int32_t curve) {
    segment_ = 1;
    t.set({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, timeMs, curve);
}

// 0x461460: the tangent leaving key i towards i + 1 that makes the segment's end natural:
// (3 (P[i+1] − P[i]) / dt − T[i+1]) / 2, times key i's weight (dt: the segment in seconds).
std::array<float, 3> TimedSpline::startTangent(size_t i) const {
    const Entry& a = entries_[i];
    const Entry& b = entries_[i + 1];
    float f = 3.0f / ((b.t - a.t) * (last_ - first_) * kMs);
    V3 v = mul(sub(b.pos, a.pos), f);
    v = sub(v, b.out);
    v = mul(v, 0.5f);
    return mul(v, a.weight);
}

// 0x4615c0: the tangent arriving at key i from i − 1, likewise.
std::array<float, 3> TimedSpline::endTangent(size_t i) const {
    const Entry& a = entries_[i - 1];
    const Entry& b = entries_[i];
    float f = 3.0f / ((b.t - a.t) * (last_ - first_) * kMs);
    V3 v = mul(sub(b.pos, a.pos), f);
    v = sub(v, a.out);
    v = mul(v, 0.5f);
    return mul(v, b.weight);
}

void TimedSpline::begin(int32_t timeMs, int32_t curve) {
    restart(timeMs, curve);
    if (!keys_.empty()) point = keys_.front().pos;
    if (keys_.size() < 2) {
        entries_.assign(keys_.size(), Entry{});
        for (size_t i = 0; i < keys_.size(); ++i) entries_[i].pos = keys_[i].pos;
        return;
    }
    // A key at the same time as the one before it replaces that one.
    for (size_t i = 1; i < keys_.size();) {
        if (keys_[i].time - keys_[i - 1].time == 0.0f) keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        else ++i;
    }
    first_ = keys_.front().time;
    last_ = keys_.back().time;
    size_t n = keys_.size();
    entries_.assign(n, Entry{});
    for (size_t i = 0; i < n; ++i) {
        entries_[i].pos = keys_[i].pos;
        entries_[i].weight = keys_[i].weight;
        // Nyanroid: one key left (all at one time) stays at 0 rather than dividing by zero.
        entries_[i].t = last_ != first_ ? (keys_[i].time - first_) / (last_ - first_) : 0.0f;
    }
    if (n <= 2) return;

    // A path ending where it starts is closed: the key before the last goes in front, the second
    // after the end, with their spacing in time, for the tangents; they go again at the end.
    bool closed = entries_.front().pos == entries_.back().pos;
    if (closed) {
        Entry before = entries_[n - 2];
        before.t = entries_[0].t - (entries_[n - 1].t - entries_[n - 2].t);
        Entry after = entries_[1];
        after.t = (entries_[1].t - entries_[0].t) + entries_[n - 1].t;
        entries_.push_back(after);
        entries_.insert(entries_.begin(), before);
    }
    size_t m = entries_.size();
    const float total = last_ - first_;
    // Inner keys: the direction halfway between their neighbours'.
    for (size_t i = 1; i + 1 < m; ++i) {
        V3 a = direction(sub(entries_[i + 1].pos, entries_[i].pos));
        V3 b = direction(sub(entries_[i - 1].pos, entries_[i].pos));
        entries_[i].out = direction(sub(a, b));
    }
    entries_[0].out = startTangent(0);
    entries_[m - 1].out = endTangent(m - 1);
    // Three passes: each inner tangent the mean of the natural ones from either side, weighted by
    // the segments' lengths in seconds and damped where the speeds into and out of the key differ
    // (4 a b / (a + b)²); written one key late, so every key reads the last pass's neighbours.
    for (int pass = 0; pass < 3; ++pass) {
        V3 previous = startTangent(0);
        for (size_t i = 1; i + 1 < m; ++i) {
            float next = (entries_[i + 1].t - entries_[i].t) * total * kMs;
            V3 a = mul(endTangent(i), next);
            float before = (entries_[i].t - entries_[i - 1].t) * total * kMs;
            V3 b = mul(startTangent(i), before);
            V3 c = mul(plus(a, b), 1.0f / (before + next));
            float speedIn = std::sqrt(lengthSq(sub(entries_[i - 1].pos, entries_[i].pos))) / next;
            float speedOut = std::sqrt(lengthSq(sub(entries_[i + 1].pos, entries_[i].pos))) / before;
            if (speedIn > 9.99999975e-06f && speedOut > 9.99999975e-06f) {
                float s = speedIn + speedOut;
                c = mul(c, (speedIn * 4.0f * speedOut) / (s * s));
            }
            entries_[i - 1].out = previous;
            previous = c;
        }
        V3 end = endTangent(m - 1);
        entries_[m - 1].out = end;
        entries_[m - 2].out = previous;
    }
    // Each key's incoming tangent is its predecessor's outgoing one; both scaled by the segment's
    // length in seconds.
    for (size_t i = 1; i < m; ++i) entries_[i].in = entries_[i - 1].out;
    for (size_t i = 1; i < m; ++i) {
        float s = (entries_[i].t - entries_[i - 1].t) * total * kMs;
        entries_[i].in = mul(entries_[i].in, s);
        entries_[i].out = mul(entries_[i].out, s);
    }
    if (closed) {
        entries_.erase(entries_.begin());
        entries_.pop_back();
    }
    restart(timeMs, curve);
    point = entries_.front().pos;
}

void TimedSpline::findSegment() {
    if (segment_ < 0) return;
    size_t m = entries_.size();
    if (m < 3 || !t.active) {
        if (!t.active) segment_ = -1;
        return;
    }
    float now = t.now[0];
    segment_ = 1;
    if (entries_[0].t <= now) {
        for (; static_cast<size_t>(segment_) != m; ++segment_) {
            if (entries_[static_cast<size_t>(segment_) - 1].t <= now && now < entries_[static_cast<size_t>(segment_)].t) return;
        }
        segment_ = static_cast<int32_t>(m) - 1;
    }
}

bool TimedSpline::evaluate() {
    if (segment_ < 0) {
        if (!entries_.empty()) point = entries_.back().pos;
        return false;
    }
    size_t m = entries_.size();
    findSegment();
    if (m < 3) {
        if (m != 2) return true;
        if (segment_ != -1) {
            const Entry& a = entries_[static_cast<size_t>(segment_) - 1];
            const Entry& b = entries_[static_cast<size_t>(segment_)];
            float s = (t.now[0] - a.t) / (b.t - a.t);
            point = plus(entries_[0].pos, mul(sub(entries_[1].pos, entries_[0].pos), s));
            return true;
        }
    } else if (segment_ != -1) {
        // Mware's Vector3D::Hermite (DirectXMath's), as the exe passes it: the previous key, this
        // key's incoming tangent (+0xc), this key, its outgoing tangent (+0x18).
        const Entry& p = entries_[static_cast<size_t>(segment_) - 1];
        const Entry& c = entries_[static_cast<size_t>(segment_)];
        float s = (t.now[0] - p.t) / (c.t - p.t);
        float s2 = s * s, s3 = s2 * s;
        float h00 = ((s3 + s3) - s2 * 3.0f) + 1.0f;
        float h10 = (s3 - s2 * 2.0f) + s;
        float h01 = s2 * 3.0f - (s3 + s3);
        float h11 = s3 - s2;
        for (size_t k = 0; k < 3; ++k) point[k] = p.pos[k] * h00 + h10 * c.in[k] + h01 * c.pos[k] + h11 * c.out[k];
        return true;
    }
    point = entries_.back().pos;
    return true;
}

bool TimedSpline::step(const FrameClock& clock) {
    if (segment_ < 0) return false;
    bool ran = t.step(clock);
    evaluate();
    return ran;
}

void TimedSpline::setTimer(float ms) {
    t.setTimer(ms);
    segment_ = 1;
    findSegment();
    evaluate();
}

}  // namespace nyan::diesel
