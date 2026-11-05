#include "nyan/cs2/stage.hpp"

#include <cmath>
#include <random>
#include <utility>

namespace nyan::cs2 {

int32_t randomBetween(int32_t lo, int32_t hi) {
    static std::mt19937 rng{std::random_device{}()};
    if (lo > hi) std::swap(lo, hi);
    return std::uniform_int_distribution<int32_t>(lo, hi)(rng);
}

namespace {
constexpr float kPi = 3.14159265358979f;
}  // namespace

float easeCurve(float p, int ease) {
    if (ease == 1) return std::sin(kPi / 2 * p);
    if (ease == 2) return 1 - std::cos(kPi / 2 * p);
    if (ease == 3) return (1 - std::cos(kPi * p)) / 2;
    return p;
}

float addedValue(const std::vector<Motion>& motions, Motion::Kind kind, int axis, uint64_t now) {
    float sum = 0;
    for (const Motion& m : motions) {
        if (m.kind == kind) sum += m.a[axis] * (easeCurve(m.phase(now), static_cast<int>(m.a[5])) - 1.0f);
    }
    return sum;
}

namespace {

// The motion scripts' triangle wave (native 42): over each period 0 -> 1 -> 0 -> -1 -> 0.
float triangle(float x, float period) {
    x = std::fabs(x);
    period = std::fabs(period);
    if (period <= 0) return 0;
    float m = x > 0 ? std::fmod(x, period) : 0;
    float q = period * 0.25f;
    float s = m > 0 ? std::fmod(m, q) / q : 0;
    if (q > m) return s;
    if (2 * q > m) return 1 - s;
    if (3 * q > m) return -s;
    return s - 1;
}

// The engine's spline (sscript native 320, kcs motSpline/mcSpline): Forsythe-Malcolm-Moler's
// cubic spline through (x[i], y[i]) -- end conditions from the cubics through the four end
// points, plain when there are three points, a line for two -- evaluated as
// y[i] + b[i] u + c[i] u^2 + d[i] u^3 with u = x - x[i] on the segment the time falls in.
float fmmSpline(const std::vector<float>& x, const std::vector<float>& y, float t) {
    const size_t n = x.size();
    if (n == 0) return 0;
    if (n == 1) return y[0];
    std::vector<float> b(n), c(n), d(n);
    if (n < 3) {
        b[0] = b[1] = (x[1] != x[0]) ? (y[1] - y[0]) / (x[1] - x[0]) : 0;
    } else {
        const size_t nm1 = n - 1;
        d[0] = x[1] - x[0];
        c[1] = d[0] != 0 ? (y[1] - y[0]) / d[0] : 0;
        for (size_t i = 1; i < nm1; ++i) {
            d[i] = x[i + 1] - x[i];
            b[i] = 2 * (d[i - 1] + d[i]);
            c[i + 1] = d[i] != 0 ? (y[i + 1] - y[i]) / d[i] : 0;
            c[i] = c[i + 1] - c[i];
        }
        b[0] = -d[0];
        b[nm1] = -d[n - 2];
        c[0] = c[nm1] = 0;
        if (n > 3) {
            auto q = [](float a, float s) { return s != 0 ? a / s : 0.0f; };
            c[0] = q(c[2], x[3] - x[1]) - q(c[1], x[2] - x[0]);
            c[nm1] = q(c[n - 2], x[nm1] - x[n - 3]) - q(c[n - 3], x[n - 2] - x[n - 4]);
            c[0] = q(c[0] * d[0] * d[0], x[3] - x[0]);
            c[nm1] = q(-c[nm1] * d[n - 2] * d[n - 2], x[nm1] - x[n - 4]);
        }
        for (size_t i = 1; i < n; ++i) {
            float tt = b[i - 1] != 0 ? d[i - 1] / b[i - 1] : 0;
            b[i] -= tt * d[i - 1];
            c[i] -= tt * c[i - 1];
        }
        c[nm1] = b[nm1] != 0 ? c[nm1] / b[nm1] : 0;
        for (size_t k = n - 1; k-- > 0;) c[k] = b[k] != 0 ? (c[k] - d[k] * c[k + 1]) / b[k] : 0;
        b[nm1] = d[n - 2] != 0 ? (y[nm1] - y[n - 2]) / d[n - 2] + d[n - 2] * (c[n - 2] + 2 * c[nm1]) : 0;
        for (size_t i = 0; i < nm1; ++i) {
            b[i] = d[i] != 0 ? (y[i + 1] - y[i]) / d[i] - d[i] * (c[i + 1] + 2 * c[i]) : 0;
            d[i] = d[i] != 0 ? (c[i + 1] - c[i]) / d[i] : 0;
            c[i] = 3 * c[i];
        }
        c[nm1] = 3 * c[nm1];
        d[nm1] = d[n - 2];
    }
    // motSpline: the first key past t, then back one; past the end, the last key.
    size_t i = 0;
    while (i < n && !(t < x[i])) ++i;
    if (i >= n) return y[n - 1];
    if (i > 0) --i;
    float u = t - x[i];
    return y[i] + u * (b[i] + u * (c[i] + u * d[i]));
}
}

std::array<float, 4> rotation2x2(float degX, float degY, float degZ) {
    constexpr float kRad = kPi / 180.0f;
    float cx = std::cos(degX * kRad), sx = std::sin(degX * kRad);
    float cy = std::cos(degY * kRad), sy = std::sin(degY * kRad);
    float cz = std::cos(degZ * kRad), sz = std::sin(degZ * kRad);
    // Rows 0 and 1 of Rx * Ry (Rx = [1 0 0; 0 cx sx; 0 -sx cx], Ry = [cy 0 -sy; 0 1 0; sy 0 cy]),
    // times the first two columns of Rz = [cz sz 0; -sz cz 0; 0 0 1].
    float r00 = cy, r01 = 0, r10 = sx * sy, r11 = cx;
    return {r00 * cz - r01 * sz, r00 * sz + r01 * cz, r10 * cz - r11 * sz, r10 * sz + r11 * cz};
}

float defaultDepth(Layer layer, int32_t i, int32_t mplFaceFrom) {
    switch (layer) {
    case Layer::Bg: return 10.0f * static_cast<float>(i);
    case Layer::Cg: return 10.0f * static_cast<float>(i * 3 + 6);
    case Layer::Fg: return 10.0f * static_cast<float>(i * 3 + 7);
    case Layer::Eg: return 10.0f * static_cast<float>(i * 3 + 8);
    case Layer::Fw: return 10.0f * static_cast<float>(i + 99);
    // E-mote bank n: a layer of its own among the planes, just under plane n; the window face banks
    // over the window (sscript 0x0c559b / Nekopara 0x0c0d61).
    case Layer::Mpl: return i < mplFaceFrom ? 4500.0f + 1000.0f * static_cast<float>(i) : 60100.0f;
    }
    return 0;
}

float Motion::phase(uint64_t now) const {
    if (now <= start || period == 0) return 0;
    uint64_t t = now - start;
    if (loop) t %= period;
    else if (t >= period) return 1;
    return static_cast<float>(t) / static_cast<float>(period);
}

MotionState evaluate(const std::vector<Motion>& motions, uint64_t now) {
    MotionState s;
    for (const Motion& m : motions) {
        if (m.kind == Motion::Kind::None) continue;
        // Not started yet (`delay`): only a settle (the eased tail of mrot/mscale) and the relative
        // motions already show, holding the old value.
        if (now < m.start && m.kind != Motion::Kind::Settle && m.kind != Motion::Kind::AddPos &&
            m.kind != Motion::Kind::AddScale) {
            continue;
        }
        // Finished one-shot motions hold their end state (phase clamps to 1): a 0->90 degree
        // arc leaves the object raised, while full arcs and shakes end back at zero offset.
        float p = m.phase(now);
        switch (m.kind) {
        case Motion::Kind::Move:
            s.hasPos = true;
            s.x = m.a[0] + (m.a[2] - m.a[0]) * p;
            s.y = m.a[1] + (m.a[3] - m.a[1]) * p;
            break;
        case Motion::Kind::Arc: {
            // motArc: along a circle from the start angle, the angle running linearly to the end:
            // x + rx (cos a - cos a0), y - ry (sin a - sin a0).
            float a0 = m.a[0] * kPi / 180.0f;
            float a = (m.a[0] + (m.a[1] - m.a[0]) * p) * kPi / 180.0f;
            s.dx += m.a[2] * (std::cos(a) - std::cos(a0));
            s.dy -= m.a[3] * (std::sin(a) - std::sin(a0));
            break;
        }
        case Motion::Kind::ScaleCos: {
            // One full cosine cycle per period: from -> to -> from. Paired planes started half a
            // path apart (Fruit's highway lamp posts) then match in size at the same screen spot.
            float k = (1.0f - std::cos(2.0f * kPi * p)) / 2.0f;
            s.scaleX *= (m.a[0] + (m.a[2] - m.a[0]) * k) / 100.0f;
            s.scaleY *= (m.a[1] + (m.a[3] - m.a[1]) * k) / 100.0f;
            break;
        }
        case Motion::Kind::Rotate: {
            // mcRot: one full turn every |period| frames on each axis, the period's sign giving the
            // direction.
            auto turn = [&](float signedPeriod) {
                float period = std::fabs(signedPeriod);
                if (period < 1) return 0.0f;
                float t = std::fmod(static_cast<float>(now - m.start), period);
                return 360.0f * t / period * (signedPeriod < 0 ? -1.0f : 1.0f);
            };
            s.rotation += turn(m.a[0]);
            s.rotX += turn(m.a[1]);
            s.rotY += turn(m.a[2]);
            break;
        }
        case Motion::Kind::Shake: {
            // motShake: the motion engine's triangle wave (native 42) with period 2 * frames / count,
            // so <count> straight half-swings out and back over the shake (a sharp jolt, not a sine).
            float w = m.a[2] > 0 ? triangle(p * static_cast<float>(m.period), 2.0f * static_cast<float>(m.period) / m.a[2]) : 0.0f;
            s.dx += m.a[0] * w;
            s.dy += m.a[1] * w;
            break;
        }
        case Motion::Kind::AddPos:
        case Motion::Kind::AddScale: {
            float k = easeCurve(p, static_cast<int>(m.a[5])) - 1.0f;
            if (m.kind == Motion::Kind::AddPos) {
                s.dx += m.a[0] * k;
                s.dy += m.a[1] * k;
            } else {
                s.addScaleX += m.a[0] * k;
                s.addScaleY += m.a[1] * k;
            }
            break;
        }
        case Motion::Kind::Settle: {
            // The curves of Tween::ease (motAScaleN as motAMoveN).
            float e = easeCurve(p, static_cast<int>(m.a[5]));
            s.scaleX *= m.a[0] + (1.0f - m.a[0]) * e;
            s.scaleY *= m.a[1] + (1.0f - m.a[1]) * e;
            s.rotation += m.a[2] * (1.0f - p);
            s.rotX += m.a[3] * (1.0f - p);
            s.rotY += m.a[4] * (1.0f - p);
            break;
        }
        case Motion::Kind::Path: {
            if (m.keys.empty()) break;
            uint64_t total = 0;
            for (const auto& k : m.keys) total += static_cast<uint64_t>(std::max(0.0f, k[0]));
            uint64_t t = now - m.start;
            if (m.loop && total > 0) t %= total;
            bool spline = m.a[0] != 0;
            if (spline) {
                // motSpline: from where the object was (time 0) through each key at its running
                // time. mcSpline: the loop's points twice over (from the last key at time 0), run
                // through once, then round its second lap: past x[N-2], t = t mod P + P with P the
                // time of the middle point.
                std::vector<float> xs, px, py;
                auto add = [&](float x, const std::array<float, 3>& k) { xs.push_back(x), px.push_back(k[1]), py.push_back(k[2]); };
                float tx = 0;
                if (m.loop) {
                    add(0, m.keys.back());
                    for (int lap = 0; lap < 2; ++lap) {
                        for (const auto& k : m.keys) add(tx += std::max(0.0f, k[0]), k);
                    }
                } else {
                    add(0, {0, m.a[1], m.a[2]});
                    for (const auto& k : m.keys) add(tx += std::max(0.0f, k[0]), k);
                }
                float tf = static_cast<float>(now - m.start);
                if (m.loop && xs.size() >= 3) {
                    float lapEnd = xs[xs.size() - 2], period = xs[(xs.size() - 1) / 2];
                    if (period > 0 && tf >= std::floor(lapEnd)) tf = std::fmod(tf, period) + period;
                }
                if (m.a[3] != 0) {
                    s.setScaleX = fmmSpline(xs, px, tf) / 100.0f;
                    s.setScaleY = fmmSpline(xs, py, tf) / 100.0f;
                    break;
                }
                s.hasPos = true;
                s.x = fmmSpline(xs, px, tf);
                s.y = fmmSpline(xs, py, tf);
                break;
            }
            // Walk the steps: a spline goes from the previous point to its own over its frames; a
            // quake sits at each point for its frames.
            auto point = [&](size_t i) {
                size_t n = m.keys.size();
                return std::array<float, 2>{m.keys[(i + n) % n][1], m.keys[(i + n) % n][2]};
            };
            std::array<float, 2> from = spline ? point(m.keys.size() - 1) : std::array<float, 2>{0, 0};
            std::array<float, 2> at = spline ? point(m.keys.size() - 1) : std::array<float, 2>{0, 0};
            bool done = true;
            for (size_t i = 0; i < m.keys.size(); ++i) {
                uint64_t f = static_cast<uint64_t>(std::max(0.0f, m.keys[i][0]));
                auto to = point(i);
                if (t < f) {
                    float u = static_cast<float>(t) / static_cast<float>(f);
                    if (spline) {
                        // Catmull-Rom through the loop's points.
                        auto p0 = point(static_cast<long>(i) - 2), p1 = from, p2 = to, p3 = point(i + 1);
                        for (int c = 0; c < 2; ++c) {
                            at[c] = 0.5f * (2 * p1[c] + (-p0[c] + p2[c]) * u + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * u * u +
                                            (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * u * u * u);
                        }
                    } else {
                        // motQuake holds each key's offset for its frames: jumps, not slides.
                        at = to;
                    }
                    done = false;
                    break;
                }
                t -= f;
                from = to;
                at = to;
            }
            (void)done;
            if (spline) {
                s.hasPos = true;
                s.x = at[0];
                s.y = at[1];
            } else {
                s.dx += at[0];
                s.dy += at[1];
            }
            break;
        }
        case Motion::Kind::Pendulum: {
            // motPend runs `period` frames and then ends; mcPend repeats.
            uint64_t t = now - m.start;
            if (!m.loop && t >= m.period) break;
            float tm = static_cast<float>(m.loop ? t % static_cast<uint64_t>(std::max(1.0f, m.a[0])) : t);
            s.rotation += std::sin(triangle(tm, m.a[0]) * kPi / 2) * m.a[1];
            break;
        }
        case Motion::Kind::ScaleSaw: {
            auto saw = [&](float from, float to, float len) {
                uint64_t cycle = static_cast<uint64_t>(std::max(1.0f, len));
                float t = static_cast<float>((now - m.start) % cycle);
                return (from + (to - from) * t / static_cast<float>(cycle)) / 100.0f;
            };
            s.setScaleX = saw(m.a[0], m.a[2], m.a[4]);
            s.setScaleY = saw(m.a[1], m.a[3], m.a[5]);
            break;
        }
        case Motion::Kind::Fade: {
            // mcFade: t = elapsed mod (P + hold); k = |triangle(t, 2P)| (0 -> 1 -> 0 over P) while
            // t < P, else 0; opacity round(from + (to - from) k), clamped.
            uint64_t cycle = static_cast<uint64_t>(std::max<uint32_t>(1, m.period)) + static_cast<uint64_t>(std::max(0.0f, m.a[2]));
            uint64_t ti = (now - m.start) % cycle;
            float k = ti < m.period ? std::fabs(triangle(static_cast<float>(ti), 2.0f * static_cast<float>(m.period))) : 0.0f;
            s.setAlpha = std::clamp(static_cast<int32_t>(std::lround(m.a[0] + (m.a[1] - m.a[0]) * k)), 0, 255);
            break;
        }
        case Motion::Kind::CFade: {
            // mcCFade (mot.int): t = elapsed mod P; k = |triangle(t, 2P)|; each byte of the
            // object's colour (blue, green, red, alpha) round(from + (to - from) k), clamped. The
            // alpha byte is the opacity mcFade and pAlpha write.
            const uint64_t ti = (now - m.start) % std::max<uint64_t>(1, m.period);
            const float k = std::fabs(triangle(static_cast<float>(ti), 2.0f * static_cast<float>(m.period)));
            uint32_t c = 0;
            for (size_t b = 0; b < 4 && b < m.keys.size(); ++b) {
                const auto& ch = m.keys[b];
                const int32_t v = std::clamp(static_cast<int32_t>(std::lround(ch[0] + (ch[1] - ch[0]) * k)), 0, 255);
                c |= static_cast<uint32_t>(v) << (8 * b);
            }
            s.hasColor = true;
            s.color = c;
            s.setAlpha = static_cast<int32_t>(c >> 24);
            break;
        }
        case Motion::Kind::None: break;
        }
    }
    return s;
}

void setImageMode(StageImage& img, uint8_t mode, bool eachFrame) {
    img.filter = mode;
    // Only an object the slot has loaded (record +288) is marked.
    if (eachFrame && (!img.parts.empty() || img.solid)) img.filterPending = true;
}

void loadImageMode(StageImage& img) {
    img.filterApplied = img.filter ? std::string(1, static_cast<char>('0' + img.filter)) : std::string();
    img.filterPending = false;
}

void stepImageMode(StageImage& img) {
    if (!std::exchange(img.filterPending, false)) return;
    if (img.filter == 0) loadImageMode(img);
    else if (!img.parts.empty()) img.filterApplied += static_cast<char>('0' + img.filter);
}

}  // namespace nyan::cs2
