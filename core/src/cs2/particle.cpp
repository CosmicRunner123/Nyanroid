#include "nyan/cs2/particle.hpp"

#include "nyan/common/xml.hpp"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace nyan::cs2 {
namespace {

using Mat = std::array<float, 16>;  // row-major; vectors are rows (D3DX)

Mat identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat mul(const Mat& a, const Mat& b) {
    Mat out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
    return out;
}

Mat scaling(float x, float y, float z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}; }
Mat rotationX(float c, float s) { return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1}; }
Mat rotationY(float c, float s) { return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}; }
Mat rotationZ(float c, float s) { return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat translation(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }

// The plugins' rotations (0x100016b0 / 0x100017d0 / 0x100018f0) keep three decimals of the angle and
// take the CRT's double cos / sin; each operation multiplies in front (M = op * M, 0x10001290).
float plugAngle(float a) { return static_cast<float>(static_cast<int32_t>(a * 1000.0f)) / 1000.0f; }
void plugRotate(Mat& m, float ax, float ay, float az) {
    float x = plugAngle(ax), y = plugAngle(ay), z = plugAngle(az);
    m = mul(rotationX(static_cast<float>(std::cos(static_cast<double>(x))), static_cast<float>(std::sin(static_cast<double>(x)))), m);
    m = mul(rotationY(static_cast<float>(std::cos(static_cast<double>(y))), static_cast<float>(std::sin(static_cast<double>(y)))), m);
    m = mul(rotationZ(static_cast<float>(std::cos(static_cast<double>(z))), static_cast<float>(std::sin(static_cast<double>(z)))), m);
}

std::array<float, 3> transform(const std::array<float, 3>& v, const Mat& m) {
    return {v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12], v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
            v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14]};
}

// x87Note. CatSystem2 creates its Direct3D 9 device without D3DCREATE_FPU_PRESERVE (Happiness
// 0x575854 / 0x575882: behaviour flags 0x44 / 0x24; Amakano 2's and Nekopara 3's cs2.exe the
// same), and Direct3D then sets the creating thread's x87 control word to single precision; the
// only precision change after the CRT's start-up _PC_53 (0x6acfd5) comes before that. Plugins run
// on that thread, so every x87 operation in them (the VS2008 builds throughout, the newer builds
// where they work on a host function's ST0) rounds its result to 24 bits: float arithmetic, step
// by step, a double constant taking part at its own value. The CRT's sqrt / sin / cos take their
// SSE2 paths (in double) whatever the precision.

// The engine's linear map for plugins (vtable +0x74, 0x5eb6a0): x from [lo, hi] onto [a, b], held
// at a below lo and at b above hi.
float mapRange(float a, float b, float lo, float hi, float x) {
    if (lo >= x) return a;
    if (x >= hi) return b;
    return a + (x - lo) / (hi - lo) * (b - a);
}

// A record moved with its plane: most plugins add the plane's move (ParticleEffect::planeMove) to
// a record's position, x then y, once an update, where their branches meet.
void addMove(ParticleRecord& r, const std::array<float, 2>& m) {
    r.pos[0] = r.pos[0] + m[0];
    r.pos[1] = m[1] + r.pos[1];
}

void setAlpha(ParticleRecord& r, int32_t a) {
    for (auto& v : r.v) v.alpha = static_cast<uint8_t>(a);
}

void setUv(ParticleRecord& r, float u, float v, float du, float dv) {
    r.v[0].u = u, r.v[0].v = v;
    r.v[1].u = u + du, r.v[1].v = v;
    r.v[2].u = u, r.v[2].v = v + dv;
    r.v[3].u = u + du, r.v[3].v = v + dv;
}

// matrix_rain.ppi (Amakano 2): streaks flying at the camera. Init 0x10001d60, Update 0x10001f60,
// spawn 0x10002620 (at the back) / 0x10002880 (anywhere along the way, for a running start).
//   f0 f1: emitter (screen); f2 f3: overall x / y scale (percent); f4 + rand * f5: speed per update;
//   f6 f7: spread in x / y; f8-f10: overall rotation (hundredths of a radian); f11: start half the
//   particles already in flight; f12: additive (the layer's blend, blend()).
class MatrixRain : public ParticleEffect {
public:
    MatrixRain(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        if (static_cast<int32_t>(f[12]) != 0) blend_ = 1;  // Init 0x10001e68: additive
        trans_ = {f[0], f[1], 0};
        scale_ = {f[2] / 100.0f, f[3] / 100.0f, 1};
        rot_ = {f[8] / 100.0f, f[9] / 100.0f, f[10] / 100.0f};
        speedBase_ = f[4], speedRand_ = f[5], spreadX_ = f[6], spreadY_ = f[7];
        prefill_ = static_cast<int32_t>(f[11]) != 0;
        state_.resize(records_.size());
        size_t n = records_.size();
        if (prefill_) {
            active_ = n / 2;
            for (size_t i = 0; i < active_; ++i) spawn(i, true);
            for (size_t i = active_; i < n; ++i) spawn(i, false);
        } else {
            for (size_t i = 0; i < n; ++i) spawn(i, false);
        }
    }

    void step(int32_t frames) override {
        // The emitter follows the plane: the update's f0 / f1 plus the plane's move (0x10001fd6).
        const auto mv = planeMove();
        trans_[0] = params_.f[0] + mv[0];
        trans_[1] = params_.f[1] + mv[1];
        Mat g = identity();
        g = mul(scaling(scale_[0], scale_[1], scale_[2]), g);
        plugRotate(g, rot_[0], rot_[1], rot_[2]);
        g[12] += trans_[0], g[13] += trans_[1], g[14] += trans_[2];
        active_ = std::min(active_ + 1, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st == 0) {
                // Fading in over 10 frames, to 20.
                setAlpha(r, static_cast<int32_t>(mapRange(0, 20, 0, 10, static_cast<float>(s.counter))));
                s.pos[2] += s.speed;
                r.pos[2] = s.pos[2];
                if (s.counter > 10) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                // Ones heading for the middle of the screen fade out on the way.
                bool middle = s.pos[0] > 320 && 960 > s.pos[0] && s.pos[1] > 180 && 560 > s.pos[1];
                setAlpha(r, middle ? static_cast<int32_t>(mapRange(20, 0, -2000, -300, s.pos[2])) : 20);
                s.pos[2] += s.speed;
                r.pos[2] = s.pos[2];
                if (s.pos[2] > 300) s.st = 3, s.start = s.counter;
            } else {
                spawn(i, false);
            }
            Mat m = identity();
            m = mul(scaling(s.scale[0], s.scale[1], s.scale[2]), m);
            plugRotate(m, s.rot[0], s.rot[1], s.rot[2]);
            m[12] += s.pos[0], m[13] += s.pos[1], m[14] += s.pos[2];
            Mat fm = mul(m, g);
            for (int k = 0; k < 4; ++k) {
                auto v = transform(s.corner[static_cast<size_t>(k)], fm);
                r.v[static_cast<size_t>(k)].x = v[0], r.v[static_cast<size_t>(k)].y = v[1], r.v[static_cast<size_t>(k)].z = v[2];
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 3> pos{}, rot{}, scale{1, 1, 1};
        float speed = 0;
        std::array<std::array<float, 3>, 4> corner{};
    };

    void spawn(size_t i, bool inFlight) {
        static constexpr float kU[4] = {0.0f, 0.25f, 0.5f, 0.75f};  // .data 0x100180b0
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        uint32_t k = rng_.next() & 3;
        setUv(r, kU[k], 0, 0.25f, 1.0f);
        setAlpha(r, 0);
        float w = params_.texW, h = params_.texH;
        s.corner = {{{0, 0, 0}, {w, 0, 0}, {0, h, 0}, {w, h, 0}}};
        s.pos[0] = (rng_.unit() * 2.0f - 1.0f) * spreadX_;
        s.pos[1] = (rng_.unit() * 2.0f - 1.0f) * spreadY_;
        s.counter = 0;
        if (inFlight) {
            uint32_t at = rng_.next() % 1000;
            s.pos[2] = mapRange(-2000, 300, 0, 999, static_cast<float>(at));
            s.counter = static_cast<int32_t>(at);
        } else {
            s.pos[2] = -2000;
        }
        r.pos = s.pos;
        s.rot = {300.0f, 350.0f, 3.12f};
        s.scale = {1, 1, 1};
        r.scale = {1, s.scale[1], s.scale[2]};
        s.speed = rng_.unit() * speedRand_ + speedBase_;
        s.start = 0, s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> trans_{}, scale_{}, rot_{};
    float speedBase_ = 0, speedRand_ = 0, spreadX_ = 0, spreadY_ = 0;
    bool prefill_ = false;
    size_t active_ = 0;
};

// cs2ParticleBaseEx's path (add 0x10001780, tangents 0x100018f0, position 0x10001c40 in each
// plugin): up to 20 points, each with its tangent and its distance from the one before.
struct SplinePath {
    struct Node {
        std::array<float, 3> p{}, t{};
        float len = 0;
    };
    std::array<Node, 20> nodes{};
    size_t count = 0;
    float total = 0, duration = 0;

    void add(const std::array<float, 3>& p) {
        float len = 0;
        if (count > 0) {
            const auto& q = nodes[count - 1].p;
            float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            len = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
        }
        if (count < nodes.size()) nodes[count] = {p, {0, 0, 0}, len};
        total += len;
        if (++count > nodes.size()) count = 0;
    }
    static std::array<float, 3> unit(std::array<float, 3> v, float k) {
        float l = static_cast<float>(std::sqrt(static_cast<double>(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
        return {v[0] / l * k, v[1] / l * k, v[2] / l * k};
    }
    static std::array<float, 3> sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }
    void finish(float d, float k) {
        if (d == 0 || count < 2) return;
        // An inner point's direction is (next - this) - (previous - this), as the DLLs work it out
        // (sakura 0x100018f0, steam 0x100019d0).
        for (size_t i = 1; i + 1 < count; ++i)
            nodes[i].t = unit(sub(sub(nodes[i + 1].p, nodes[i].p), sub(nodes[i - 1].p, nodes[i].p)), k);
        nodes[0].t = unit(sub(nodes[1].p, nodes[0].p), k);
        nodes[count - 1].t = unit(sub(nodes[count - 1].p, nodes[count - 2].p), k);
        duration = d;
    }
    // The tangents as energy's copy works them out (0x100018c0): the length is the mean of the
    // segments' (each |next - this|, in order), the inner points' direction ((next - this) -
    // (previous - this)) and the ends' their segment's; two points share one tangent.
    void finishMean(float d) {
        if (d == 0 || count < 2) return;
        auto length = [](const std::array<float, 3>& v) {
            return static_cast<float>(std::sqrt(static_cast<double>(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
        };
        auto scaled = [](const std::array<float, 3>& v, float l, float k) {
            return std::array<float, 3>{v[0] / l * k, v[1] / l * k, v[2] / l * k};
        };
        const size_t n = count - 1;
        float sum = 0;
        for (size_t i = 0; i < n; ++i) sum = length(sub(nodes[i + 1].p, nodes[i].p)) + sum;
        const float mean = sum / static_cast<float>(static_cast<double>(n));
        if (count == 2) {
            const auto v = sub(nodes[1].p, nodes[0].p);
            nodes[0].t = nodes[1].t = scaled(v, length(v), mean);
            duration = d;
            return;
        }
        for (size_t i = 1; i + 1 < count; ++i) {
            const auto v = sub(sub(nodes[i + 1].p, nodes[i].p), sub(nodes[i - 1].p, nodes[i].p));
            nodes[i].t = scaled(v, length(v), mean);
        }
        auto v = sub(nodes[1].p, nodes[0].p);
        nodes[0].t = scaled(v, length(v), mean);
        v = sub(nodes[count - 1].p, nodes[count - 2].p);
        nodes[count - 1].t = scaled(v, length(v), mean);
        duration = d;
    }
    std::array<float, 3> at(float t) const {
        if (count < 2 || 0 >= t) return nodes[0].p;
        if (t >= duration) t = duration;
        float s = t / duration * total, acc = 0;
        size_t i = 1;
        for (; i < count; ++i) {
            if (acc + nodes[i].len > s) break;
            acc += nodes[i].len;
        }
        if (i == count) return nodes[count - 1].p;
        float u = (s - acc) / nodes[i].len, u2 = u * u, u3 = u2 * u;
        const Node &a = nodes[i - 1], &b = nodes[i];
        std::array<float, 3> out{};
        for (size_t c = 0; c < 3; ++c) {
            // Hermite basis (.data 0x100160d0): rows 2 -2 1 1 / -3 3 -2 -1 / 0 0 1 0 / 1 0 0 0.
            float k3 = 2 * a.p[c] - 2 * b.p[c] + a.t[c] + b.t[c];
            float k2 = -3 * a.p[c] + 3 * b.p[c] - 2 * a.t[c] - b.t[c];
            out[c] = u3 * k3 + u2 * k2 + u * a.t[c] + a.p[c];
        }
        return out;
    }
};


// sakura.ppi (Amakano 2's endings): petals blown down and to the left. Init 0x10001ff0, Update
// 0x10002160, spawn 0x10002800. Each petal follows its own Hermite path (0x10001780 adds a point,
// 0x100018f0 sets the tangents, 0x10001c40 evaluates), turning a little more every frame, and fades
// over the last tenth of its life. It does not read the plane fields: the petals stay put when
// their plane moves.
//   f0: group size (1: single petals; groups' followers are not reimplemented); f1: size
//   (hundredths of the texture); f2: path step; f3: life (frames); f4: start with half of them
//   already on their way.
class Sakura : public ParticleEffect {
public:
    Sakura(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        int32_t group = static_cast<int32_t>(f[0]);
        size_t groups = group > 0 ? records_.size() / static_cast<size_t>(group) : records_.size();
        state_.resize(groups);
        size_ = f[1] / 100.0f;
        stepLen_ = f[2];
        life_ = f[3];
        bool prefill = static_cast<int32_t>(f[4]) != 0;
        if (prefill) {
            active_ = groups / 2;
            for (size_t i = 0; i < active_; ++i) {
                spawn(i);
                State& s = state_[i];
                s.counter = static_cast<int32_t>(rng_.unit() * life_);
                if (static_cast<float>(s.counter) > life_ * 0.9f) s.st = 1, s.start = s.counter;
            }
            for (size_t i = active_; i < groups; ++i) spawn(i);
        } else {
            active_ = 1;
            for (size_t i = 0; i < groups; ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (rng_.next() % 10 == 0) active_ += 10;
        active_ = std::min(active_, state_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st == 0) {
                auto p = s.path.at(static_cast<float>(s.counter));
                r.pos[0] = p[0], r.pos[1] = p[1];
                setAlpha(r, 255);
                turn(r);
                // The engine's round (vtable +0x50: floor(x + 0.5)).
                if (s.counter > static_cast<int32_t>(std::floor(life_ * 0.9f + 0.5f))) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                auto p = s.path.at(static_cast<float>(s.counter));
                float a = mapRange(255, 0, static_cast<float>(s.start), life_, static_cast<float>(s.counter));
                r.pos[0] = p[0], r.pos[1] = p[1];
                setAlpha(r, static_cast<int32_t>(a));
                turn(r);
                if (static_cast<float>(s.counter) > life_) s.st = 2, s.start = s.counter;
            } else {
                spawn(i);
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        SplinePath path;
    };

    void turn(ParticleRecord& r) {
        for (float& a : r.rot) a += rng_.unit() / 20.0f;
    }

    float stepLength() { return rng_.unit() * stepLen_ * 0.2f + stepLen_ * 0.8f; }

    void spawn(size_t i) {
        static constexpr float kU[2] = {0.0f, 0.5f};  // .data 0x100160b0: the top two petals
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        r.pivot[0] = params_.texW * 0.5f;
        r.pivot[1] = params_.texH * 0.5f;
        uint32_t k = rng_.next() & 1;
        setUv(r, kU[k], 0, 0.5f, 0.5f);
        float sz = rng_.unit() * size_ * 0.6f + size_ * 0.4f;
        r.scale = {sz, sz * 1.3f, 1};
        setAlpha(r, 0);
        r.pos = {static_cast<float>(rng_.next() % 1280) + 480.0f, -50.0f, 0};
        constexpr float kPi = 3.14159274f;
        r.rot[0] = rng_.unit() * kPi;
        r.rot[1] = rng_.unit() * kPi;
        r.rot[2] = rng_.unit() * kPi;
        s.counter = 0, s.st = 0;
        s.path = SplinePath{};
        s.path.add(r.pos);
        // A first step down and to the left; then up to 12 more, each turned at random within
        // about 30 degrees of the first, ending early one time in a hundred.
        std::array<float, 3> dir{-rng_.unit(), 0, 0};
        dir[1] = rng_.unit();
        if (float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]); l > 0) dir = {dir[0] / l, dir[1] / l, 0};
        float len = stepLength();
        std::array<float, 3> base{r.pos[0] + dir[0] * len, r.pos[1] + dir[1] * len, r.pos[2] + dir[2] * len};
        s.path.add(base);
        std::array<float, 3> d = SplinePath::sub(base, r.pos);
        if (float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); l > 0) d = {d[0] / l, d[1] / l, d[2] / l};
        const float c30 = static_cast<float>(std::cos(static_cast<double>(30.0f * kPi / 180.0f)));
        const float s30 = static_cast<float>(std::sin(static_cast<double>(30.0f * kPi / 180.0f)));
        const float x1 = c30 * d[0] + s30 * d[1], y1 = c30 * d[1] - s30 * d[0];  // turned by -30
        const float x2 = c30 * d[0] - s30 * d[1], y2 = c30 * d[1] + s30 * d[0];  // by +30
        for (int j = 0; j < 12; ++j) {
            float w = std::fabs(x2 - x1);
            float nx = d[0] + (rng_.unit() * w - w * 0.5f);
            float wy = std::fabs(y2 - y1);
            float ny = d[1] + (rng_.unit() * wy - wy * 0.5f);
            if (float l = std::sqrt(nx * nx + ny * ny); l > 0) nx /= l, ny /= l;
            float step = stepLength();
            base = {base[0] + nx * step, base[1] + ny * step, base[2]};
            s.path.add(base);
            if (rng_.next() % 100 == 0) break;
        }
        s.path.finish(life_, life_ / 5.0f);
    }

    std::vector<State> state_;
    float size_ = 0, stepLen_ = 0, life_ = 0;
    size_t active_ = 0;
};

// The host's helpers as the plugins call them (cs2.exe's kcTaskParticlePluginFunc table): +0x10
// degrees to radians (Happiness 0x55f840: x87 fld, fmul pi as a float, fdiv 180.0f), each step in
// the FPU's single precision (x87Note); +0x18 / +0x1c the CRT's sin / cos of that float (SSE2, in
// double), as a float.
float plugRad(float deg) { return deg * 3.14159274f / 180.0f; }
float plugSin(float r) { return static_cast<float>(std::sin(static_cast<double>(r))); }
float plugCos(float r) { return static_cast<float>(std::cos(static_cast<double>(r))); }

// em_note.ppi (Nekopara 3): notes rising beside a character's head. Init 0x10002070, Update
// 0x100021c0, spawn 0x100024e0; the path is cs2ParticleBaseEx's (SplinePath).
//   f0: records per note (only the first moves); f1 f2: emitter; f3: 0 heads up-left, else
//   up-right; f4: size (percent); f5 f6 f7: fade-in, hold and fade-out frames (each 50-100% of
//   that, at random).
// Each note starts at the emitter's colour (white, alpha 0), scaled 60-100% of the size and
// turned up to a quarter radian either way; its path starts 1-2 steps out along the heading and
// takes four more steps, each within 50 degrees of it; it fades in, holds and fades out over the
// path's whole time, then starts again. One note at first; each update one more joins with
// chance 1 in 51.
class EmNote : public ParticleEffect {
public:
    EmNote(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        const int32_t per = static_cast<int32_t>(f[0]);
        groups_ = per != 0 ? static_cast<size_t>(std::max(0, p.count) / per) : static_cast<size_t>(std::max(0, p.count));
        groups_ = std::min(groups_, records_.size());
        emitter_ = {f[1], f[2], 0};
        right_ = static_cast<int32_t>(f[3]) != 0;
        size_ = f[4] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        fadeIn_ = static_cast<int32_t>(f[5]);
        hold_ = static_cast<int32_t>(f[6]);
        fadeOut_ = static_cast<int32_t>(f[7]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(groups_);
        for (size_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (rng_.next() % 51 == 0) ++active_;
        active_ = std::min(active_, groups_);
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter - s.start);
            if (s.st == 0) {
                place(s, r);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(s.fadeIn), t)));
                if (s.fadeIn < s.counter - s.start) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                place(s, r);
                if (s.hold < s.counter - s.start) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                place(s, r);
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(s.fadeOut), t)));
                if (s.fadeOut < s.counter - s.start) s.st = 10, s.start = s.counter;
            } else {
                spawn(i);
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x1000247f)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        int32_t fadeIn = 0, hold = 0, fadeOut = 0;
        SplinePath path;
    };

    static void place(const State& s, ParticleRecord& r) {
        auto p = s.path.at(static_cast<float>(s.counter));
        r.pos[0] = p[0], r.pos[1] = p[1];
    }

    int32_t spread(int32_t frames) {
        const float a = static_cast<float>(frames);
        return static_cast<int32_t>(rng_.unit() * a * 0.5f + a * 0.5f);
    }

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        r.v[0].x = -hw, r.v[0].y = -hh;
        r.v[1].x = hw, r.v[1].y = -hh;
        r.v[2].x = -hw, r.v[2].y = hh;
        r.v[3].x = hw, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        const float sc = static_cast<float>(static_cast<double>(rng_.unit() * size_) * 0.4) + size_ * 0.6f;  // x87Note
        r.scale = {sc, sc, 1};
        s.fadeIn = spread(fadeIn_);
        s.hold = spread(hold_);
        s.fadeOut = spread(fadeOut_);
        r.rot[2] = rng_.unit() - 0.5f;
        setAlpha(r, 0);
        // The heading: up and to one side.
        float dx = right_ ? 1.0f : -1.0f, dy = -1.0f;
        if (float l = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + 1.0f))); l > 0) dx /= l, dy /= l;
        const float step = size_ * 80.0f * k_;
        float len = rng_.unit() * step + step;
        std::array<float, 3> cur{emitter_[0] + len * dx, emitter_[1] + len * dy, emitter_[2]};
        float ux = cur[0] - emitter_[0], uy = cur[1] - emitter_[1], uz = cur[2] - emitter_[2];
        if (float l = static_cast<float>(std::sqrt(static_cast<double>(ux * ux + uy * uy + uz * uz))); l > 0) ux /= l, uy /= l;
        s.path = SplinePath{};
        const float c1 = plugCos(plugRad(-50)), s1 = plugSin(plugRad(-50));
        const float c2 = plugCos(plugRad(50)), s2 = plugSin(plugRad(50));
        for (int j = 0; j < 4; ++j) {
            const float x1 = c1 * ux - s1 * uy, y1 = c1 * uy + s1 * ux;
            const float x2 = c2 * ux - s2 * uy, y2 = c2 * uy + s2 * ux;
            const float w = std::fabs(x1 - x2);  // sqrt(pow(d, 2)) through the host
            float nx = rng_.unit() * w - w * 0.5f + ux;
            const float wy = std::fabs(y1 - y2);
            float ny = rng_.unit() * wy - wy * 0.5f + uy;
            if (float l = static_cast<float>(std::sqrt(static_cast<double>(nx * nx + ny * ny))); l > 0) nx /= l, ny /= l;
            len = rng_.unit() * step + step;
            cur = {cur[0] + len * nx, cur[1] + len * ny, cur[2]};
            s.path.add(cur);
        }
        s.path.finish(static_cast<float>(s.fadeIn + s.hold + s.fadeOut), size_ * 120.0f * k_);
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    size_t groups_ = 0, active_ = 1;
    std::array<float, 3> emitter_{};
    bool right_ = false;
    float size_ = 1, k_ = 0.5f;
    int32_t fadeIn_ = 0, hold_ = 0, fadeOut_ = 0;
};

// em_sweat.ppi (Nekopara 3): a drop of sweat sliding down beside a character's head. Init
// 0x10002010, Update 0x10002130, spawn 0x10002560. One group: the drop (records[0]) and the rest as
// its trail.
//   f0 f1: emitter; f2: size (percent); f3: fade-in frames; f4: frames it keeps sliding after that.
// The drop's path runs from the emitter straight down by 150 * size * k (k: 0.5 for a texture up
// to 256 high, else 1) over fade-in plus hold frames; it fades in, then stays where the path ends.
// While it slides, each update leaves a copy of it one unit behind, at alpha 30 fading to 0 over
// 22 frames and freed after 25.
class EmSweat : public ParticleEffect {
public:
    EmSweat(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        fadeIn_ = static_cast<int32_t>(f[3]);
        hold_ = static_cast<int32_t>(f[4]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        trail_.resize(records_.empty() ? 0 : records_.size() - 1);
        if (!records_.empty()) spawn();
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        ParticleRecord& r = records_[0];
        const int32_t t = counter_ - start_;
        if (st_ == 0) {
            place(r);
            setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(fadeIn_), static_cast<float>(t))));
            if (fadeIn_ < t) st_ = 1, start_ = counter_;
        } else if (st_ == 1) {
            place(r);
            if (hold_ < t) st_ = 10, start_ = counter_;
        } else {
            place(r);
        }
        // While sliding, the first free trail record takes the drop's place.
        for (size_t j = 0; j < trail_.size() && st_ <= 1; ++j) {
            Trail& tr = trail_[j];
            if (tr.active) continue;
            ParticleRecord& c = records_[j + 1];
            tr.pos = r.pos;
            c.pos = r.pos;
            c.pos[2] -= 1.0f;
            tr.active = true;
            break;
        }
        for (size_t j = 0; j < trail_.size(); ++j) {
            Trail& tr = trail_[j];
            if (!tr.active) continue;
            ParticleRecord& c = records_[j + 1];
            c.pos[0] = tr.pos[0], c.pos[1] = tr.pos[1];
            setAlpha(c, static_cast<int32_t>(mapRange(30, 0, 0, 22, static_cast<float>(tr.counter))));
            tr.counter += frames;
            if (tr.counter > 25) setAlpha(c, 0), tr.counter = 0, tr.active = false;
            addMove(c, planeMove());  // each trail piece that was out (0x100024bf)
        }
        addMove(r, planeMove());  // then the drop (0x10002501)
        counter_ += frames;
    }

private:
    struct Trail {
        int32_t counter = 0;
        bool active = false;
        std::array<float, 3> pos{};
    };

    void place(ParticleRecord& r) const {
        auto p = path_.at(static_cast<float>(counter_));
        r.pos[0] = p[0], r.pos[1] = p[1];
    }

    void spawn() {
        ParticleRecord& r = records_[0];
        r.pivot[0] = params_.texW * 0.5f;
        r.pivot[1] = params_.texH * 0.5f;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
        counter_ = start_ = st_ = 0;
        path_ = SplinePath{};
        path_.add(emitter_);
        path_.add({emitter_[0], size_ * 150.0f * k_ + emitter_[1], 0});
        path_.finish(static_cast<float>(hold_ + fadeIn_), 10.0f);
        for (size_t j = 0; j < trail_.size(); ++j) {
            trail_[j] = Trail{};
            ParticleRecord& c = records_[j + 1];
            c.pivot = r.pivot;
            c.pos = r.pos;
            c.scale = r.scale;
            setAlpha(c, 0);
        }
    }

    std::vector<Trail> trail_;
    SplinePath path_;
    int32_t counter_ = 0, start_ = 0, st_ = 0;
    std::array<float, 3> emitter_{};
    float size_ = 1, k_ = 0.5f;
    int32_t fadeIn_ = 0, hold_ = 0;
};

// The host's triangle wave (vtable +0x78, 0x65b990): |x| through a period of |p|, rising 0 to 1
// over the first quarter, back to 0, down to -1 and back; each remainder by the CRT's fmod.
float plugTriangle(float x, float p) {
    const float a = std::fabs(x);
    p = std::fabs(p);
    const float r = a > 0 ? static_cast<float>(std::fmod(static_cast<double>(a), static_cast<double>(p))) : 0.0f;
    const float q = p * 0.25f;
    float f = r > 0 ? static_cast<float>(std::fmod(static_cast<double>(r), static_cast<double>(q))) : 0.0f;
    f /= q;
    if (q > r) return f;
    if (q * 2.0f > r) return 1.0f - f;
    if (q * 3.0f > r) return -f;
    return f - 1.0f;
}

// em_question.ppi / em_question_a.ppi (Nekopara 3): a question mark that pops up beside a head.
// Init 0x10001390, Update 0x100014c0, spawn 0x100019c0 (0x100019a0 in _a). One record per mark.
//   f0 f1: emitter; f2: leans right (else left) by half a radian; f3: size (percent); f4 f5 f6:
//   the pop's three steps (frames); f7: still frames between wobbles; f8: a wobble's frames.
// The mark stands on its bottom edge. It fades in to half while stretching to 1.5 times tall and
// 0.4 wide, squashes to half tall and 2.4 wide while fading in fully, and settles to its size.
// em_question then waits and wobbles (each update its turn moves by cos(2 pi t / f8) / 50,
// put back at the end) for good; em_question_a, which has no waiting state, stays still.
class EmQuestion : public ParticleEffect {
public:
    EmQuestion(const ParticleParams& p, uint32_t seed, bool wobble) : ParticleEffect(p, seed), wobble_(wobble) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        right_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        for (size_t k = 0; k < 5; ++k) frames_[k] = static_cast<int32_t>(f[4 + k]);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const float s = size_;
        for (size_t i = 0; i < records_.size(); ++i) {
            State& st = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = st.counter - st.start;
            const float ft = static_cast<float>(t);
            switch (st.st) {
            case 0: {
                const float hi = static_cast<float>(frames_[0]);
                r.scale[1] = mapRange(s, s * 1.5f, 0, hi, ft);
                r.scale[0] = mapRange(s, s * 0.4f, 0, hi, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 128, 0, hi, ft)));
                if (frames_[0] < t) st.st = 1, st.start = st.counter;
                break;
            }
            case 1: {
                const float hi = static_cast<float>(frames_[1]);
                r.scale[1] = mapRange(s * 1.5f, s * 0.5f, 0, hi, ft);
                r.scale[0] = mapRange(static_cast<float>(static_cast<double>(s) * 0.4), s * 2.4f, 0, hi, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(128, 255, 0, hi, ft)));
                if (frames_[1] < t) st.st = 2, st.start = st.counter;
                break;
            }
            case 2: {
                const float hi = static_cast<float>(frames_[2]);
                r.scale[1] = mapRange(s * 0.5f, s, 0, hi, ft);
                r.scale[0] = mapRange(s * 2.4f, s, 0, hi, ft);
                if (frames_[2] < t) st.st = 3, st.start = st.counter;
                break;
            }
            case 3:
                if (wobble_ && frames_[3] < t) st.st = 4, st.start = st.counter;
                break;
            case 4: {
                const float c = plugCos(6.2831855f / static_cast<float>(frames_[4]) * ft);
                r.rot[2] += c / 50.0f;
                if (frames_[4] < t) st.st = 3, st.start = st.counter, r.rot[2] = st.lean;
                break;
            }
            default:
                break;
            }
            // The emitter plus the plane's move, where the branches meet (0x10001950).
            const auto mv = planeMove();
            r.pos[0] = emitter_[0] + mv[0], r.pos[1] = mv[1] + emitter_[1];
            st.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float lean = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        // As the plugin writes them: the third corner's y is the half width.
        r.v[0].x = -hw, r.v[0].y = -hh;
        r.v[1].x = hw, r.v[1].y = -hh;
        r.v[2].x = -hw, r.v[2].y = hw;
        r.v[3].x = hw, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pivot[1] = hh;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        state_[i] = State{};
        state_[i].lean = right_ ? 0.5f : -0.5f;
        r.rot[2] = state_[i].lean;
        setAlpha(r, 0);
    }

    std::vector<State> state_;
    bool wobble_ = true, right_ = false;
    std::array<float, 3> emitter_{};
    float size_ = 1;
    std::array<int32_t, 5> frames_{};
};

// em_double.ppi (Nekopara 3): a mark that drops in and shakes. Init 0x10001420, Update 0x10001580,
// spawn 0x10001900. One record per mark.
//   f0 f1: emitter; f2: leans right (else left) by 0.4 radian; f3: size (percent); f4: frames to
//   shrink from three times its size while fading in; f5: frames the shake takes to die down.
// The shake jumps between the four diagonals, one a frame, by up to 35 * size * k (k: 0.5 for a
// texture up to 256 high, else 1); its state never ends, so the mark then stays put. (The state
// after it, f6 frames, and the offset the spawn works out are never reached or read.)
class EmDouble : public ParticleEffect {
public:
    EmDouble(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        right_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        fadeIn_ = static_cast<int32_t>(f[4]);
        shake_ = static_cast<int32_t>(f[5]);
        rest_ = static_cast<int32_t>(f[6]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& st = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = st.counter - st.start;
            if (st.st == 0) {
                r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
                const float hi = static_cast<float>(fadeIn_);
                r.scale[1] = mapRange(size_ * 3.0f, size_, 0, hi, static_cast<float>(t));
                r.scale[0] = mapRange(size_ * 3.0f, size_, 0, hi, static_cast<float>(t));
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, hi, static_cast<float>(t))));
                if (fadeIn_ < t) st.st = 1, st.start = st.counter;
            } else if (st.st == 1) {
                const float a = mapRange(size_ * 35.0f * k_, 0, 0, static_cast<float>(shake_), static_cast<float>(t));
                static constexpr float kDx[4] = {-1, 1, 1, -1}, kDy[4] = {-1, 1, -1, 1};
                const int32_t m = t % 4;
                if (m >= 0) r.pos[0] = emitter_[0] + kDx[m] * a, r.pos[1] = emitter_[1] + kDy[m] * a;
            } else if (st.st == 2) {
                r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
                if (rest_ < t) st.st = 1, st.start = st.counter;
            } else {
                spawn(i);
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001888)
            st.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        r.v[0].x = -hw, r.v[0].y = -hh;
        r.v[1].x = hw, r.v[1].y = -hh;
        r.v[2].x = -hw, r.v[2].y = hw;  // as written: the half width
        r.v[3].x = hw, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        r.scale = {size_ * 3.0f, size_ * 3.0f, 1};
        r.rot[2] = right_ ? 0.4f : -0.4f;
        setAlpha(r, 0);
        state_[i] = State{};
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    bool right_ = false;
    float size_ = 1, k_ = 0.5f;
    int32_t fadeIn_ = 0, shake_ = 0, rest_ = 0;
};

// em_angry.ppi (Nekopara 3): an anger mark that bursts out and throbs. Init 0x100013c0, Update
// 0x10001510, spawn 0x10001cc0. One group: the mark (records[0]) and the rest as its trail.
//   f0 f1: emitter; f2: size (percent); f3: frames to grow from 0.1 to 1.4 times the size while
//   fading in; f4: frames to settle back to the size (its turn goes 0 to 1 radian over f4 from
//   the start, then 1 to 0.3 while settling); f5: frames of plain jitter; f6 f7: a throb's swell
//   to 1.1 and its fall from 1.2 back to the size, the jitter going on, then f5 again, and so on.
// The jitter steps x through 0, +A, 0, -A (A = 4 * size * k; k 0.5 for a texture up to 256 high,
// else 1). Every update leaves a copy behind, one unit back, at alpha 30 fading to 0 over 29
// frames and growing by 1% a frame; it is freed after 29.
class EmAngry : public ParticleEffect {
public:
    EmAngry(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        for (size_t k = 0; k < 5; ++k) frames_[k] = static_cast<int32_t>(f[3 + k]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        trail_.resize(records_.empty() ? 0 : records_.size() - 1);
        if (!records_.empty()) spawn();
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        ParticleRecord& r = records_[0];
        const int32_t t = counter_ - start_;
        const float ft = static_cast<float>(t), s = size_;
        auto jitter = [&] {
            r.pos[0] = emitter_[0] + plugTriangle(ft, 4.0f) * s * 4.0f * k_;
            r.pos[1] = emitter_[1];
        };
        switch (st_) {
        case 0: {
            r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            const float sc = mapRange(s * 0.1f, s * 1.4f, 0, static_cast<float>(frames_[0]), ft);
            r.scale[0] = r.scale[1] = sc;
            r.rot[2] = mapRange(0, 1, 0, static_cast<float>(frames_[1]), ft);
            setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(frames_[0]), ft)));
            if (frames_[0] < t) st_ = 1, start_ = counter_;
            break;
        }
        case 1: {
            r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            const float sc = mapRange(s * 1.4f, s, 0, static_cast<float>(frames_[1]), ft);
            r.scale[0] = r.scale[1] = sc;
            r.rot[2] = mapRange(1, 0.3f, 0, static_cast<float>(frames_[1]), ft);
            if (frames_[1] < t) st_ = 2, start_ = counter_;
            break;
        }
        case 2:
            jitter();
            if (frames_[2] < t) st_ = 3, start_ = counter_;
            break;
        case 3: {
            jitter();
            const float sc = mapRange(s, s * 1.1f, 0, static_cast<float>(frames_[3]), ft);
            r.scale[0] = r.scale[1] = sc;
            if (frames_[3] < t) st_ = 4, start_ = counter_;
            break;
        }
        case 4: {
            jitter();
            const float sc = mapRange(s * 1.2f, s, 0, static_cast<float>(frames_[4]), ft);
            r.scale[0] = r.scale[1] = sc;
            if (frames_[4] < t) st_ = 2, start_ = counter_;
            break;
        }
        default:
            r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
        }
        for (size_t j = 0; j < trail_.size(); ++j) {
            Trail& tr = trail_[j];
            if (tr.active) continue;
            ParticleRecord& c = records_[j + 1];
            tr.pos = r.pos;
            c.pos = r.pos;
            c.pos[2] -= 1.0f;
            c.rot = r.rot;
            c.scale = r.scale;
            tr.active = true;
            break;
        }
        for (size_t j = 0; j < trail_.size(); ++j) {
            Trail& tr = trail_[j];
            if (!tr.active) continue;
            ParticleRecord& c = records_[j + 1];
            c.pos[0] = tr.pos[0], c.pos[1] = tr.pos[1];
            setAlpha(c, static_cast<int32_t>(mapRange(30, 0, 0, 29, static_cast<float>(tr.counter))));
            c.scale[0] *= 1.01f;
            c.scale[1] = c.scale[0];
            tr.counter += frames;
            if (tr.counter > 29) setAlpha(c, 0), tr.counter = 0, tr.active = false;
            addMove(c, planeMove());  // each trail piece that was out (0x10001c0b)
        }
        addMove(r, planeMove());  // then the mark (0x10001c4d)
        counter_ += frames;
    }

private:
    struct Trail {
        int32_t counter = 0;
        bool active = false;
        std::array<float, 3> pos{};
    };

    void spawn() {
        ParticleRecord& r = records_[0];
        r.pivot[0] = params_.texW * 0.5f;
        r.pivot[1] = params_.texH * 0.5f;
        r.pos = emitter_;
        r.scale = {size_ * 0.1f, size_ * 0.1f, 1};
        setAlpha(r, 0);
        counter_ = start_ = st_ = 0;
        for (size_t j = 0; j < trail_.size(); ++j) {
            trail_[j] = Trail{};
            ParticleRecord& c = records_[j + 1];
            c.pivot = r.pivot;
            c.pos = r.pos;
            c.scale = r.scale;
            setAlpha(c, 0);
        }
    }

    std::vector<Trail> trail_;
    int32_t counter_ = 0, start_ = 0, st_ = 0;
    std::array<float, 3> emitter_{};
    float size_ = 1, k_ = 0.5f;
    std::array<int32_t, 5> frames_{};
};

// em_notice.ppi (Happiness 2): a mark that blinks. Init 0x100013b0, Update 0x100014f0, spawn
// 0x10001700. One record per mark.
//   f0 f1: emitter; f2: mirrored (non-zero); f3: size (percent); f4-f8: frames shown, hidden,
//   shown, hidden, then shown for good.
// The quad is the texture's size about its centre (the second corner of its lower edge at y = half
// the width, as the DLL writes it), the mirror swapping left and right.
class EmNotice : public ParticleEffect {
public:
    EmNotice(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        mirror_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        for (size_t k = 0; k < 5; ++k) frames_[k] = static_cast<int32_t>(f[4 + k]);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& st = state_[i];
            ParticleRecord& r = records_[i];
            r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];  // in every branch
            if (st.st < 5) {
                setAlpha(r, st.st % 2 == 0 ? 255 : 0);
                if (frames_[static_cast<size_t>(st.st)] < st.counter - st.start) ++st.st, st.start = st.counter;
            }
            addMove(r, planeMove());  // where the branches meet (0x1000169f)
            st.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        const float l = mirror_ ? hw : -hw, rt = mirror_ ? -hw : hw;
        r.v[0].x = l, r.v[0].y = -hh;
        r.v[1].x = rt, r.v[1].y = -hh;
        r.v[2].x = l, r.v[2].y = hw;  // as written: the half width
        r.v[3].x = rt, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
        state_[i] = State{};
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    bool mirror_ = false;
    float size_ = 1;
    std::array<int32_t, 5> frames_{};
};

// em_oko.ppi (Happiness 2): anger marks puffing out on both sides. Init 0x10002080, Update
// 0x100021d0, spawn 0x100024f0. One record per mark.
//   f0 f1: emitter; f2: size (percent); f3 f4 f5: fade-in, hold and fade-out frames (each 50-100%
//   of that, at random).
// One mark at first; each update one more joins with chance 1 in 3. A mark starts 350-550 *
// size * k left of the emitter (even ones; odd ones right) and 120-250 * size * k above it,
// scaled 60-100% of the size and leaning 0.5-1 radian outwards (k: 0.5 for a texture up to 256
// high, else 1); it moves 100-200 * size * k along its lean over fade-in plus hold (a two-point
// Hermite path, tangents 10 long), fading in, holding and fading out, then starts again.
class EmOko : public ParticleEffect {
public:
    EmOko(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        fadeIn_ = static_cast<int32_t>(f[3]);
        hold_ = static_cast<int32_t>(f[4]);
        fadeOut_ = static_cast<int32_t>(f[5]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (rng_.next() % 3 == 0) ++active_;
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter - s.start);
            if (s.st <= 2) {
                auto at = s.path.at(static_cast<float>(s.counter));
                r.pos[0] = at[0], r.pos[1] = at[1];
            }
            if (s.st == 0) {
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(s.fadeIn), t)));
                if (s.fadeIn < s.counter - s.start) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                if (s.hold < s.counter - s.start) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(s.fadeOut), t)));
                if (s.fadeOut < s.counter - s.start) s.st = 10, s.start = s.counter;
            } else {
                spawn(i);
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x1000248f)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        int32_t fadeIn = 0, hold = 0, fadeOut = 0;
        SplinePath path;
    };

    int32_t spread(int32_t frames) {
        const float a = static_cast<float>(frames);
        return static_cast<int32_t>(rng_.unit() * a * 0.5f + a * 0.5f);
    }

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        const float sc = static_cast<float>(static_cast<double>(rng_.unit() * size_) * 0.4) + size_ * 0.6f;  // x87Note
        r.scale = {sc, sc, 1};
        s.fadeIn = spread(fadeIn_);
        s.hold = spread(hold_);
        s.fadeOut = spread(fadeOut_);
        float lean = 0.5f + rng_.unit() * 0.5f;
        if (i % 2 == 0) {
            lean = -lean;
            r.pos[0] = emitter_[0] - (rng_.unit() * 200.0f + 350.0f) * size_ * k_;
        } else {
            r.pos[0] = (rng_.unit() * 200.0f + 350.0f) * size_ * k_ + emitter_[0];
        }
        r.pos[1] = emitter_[1] - (rng_.unit() * 130.0f + 120.0f) * size_ * k_;
        r.pos[2] = 0;
        r.rot[2] = lean;
        setAlpha(r, 0);
        const float dist = (rng_.unit() * 100.0f + 100.0f) * k_ * size_;
        s.path = SplinePath{};
        s.path.add(r.pos);
        const float a = static_cast<float>(static_cast<int32_t>(lean * 1000.0f)) / 1000.0f;
        const float dx = plugSin(a), dy = -plugCos(a);
        s.path.add({r.pos[0] + dx * dist, r.pos[1] + dy * dist, r.pos[2]});
        s.path.finish(static_cast<float>(s.fadeIn + s.hold), 10.0f);
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    size_t active_ = 1;
    std::array<float, 3> emitter_{};
    float size_ = 1, k_ = 0.5f;
    int32_t fadeIn_ = 0, hold_ = 0, fadeOut_ = 0;
};

// em_common.ppi (Happiness 2): a mark that springs out and settles. Init 0x10001420, Update
// 0x10001550, spawn 0x100019e0. One record per mark.
//   f0 f1: emitter; f2: leans right (else left) by half a radian; f3: size (percent); f4: frames
//   to spring out, f5: frames to settle.
// It flies from the emitter to 300 * size * k along its lean while growing to 1.3 times and
// fading in to half, then comes back to 100 * size * k, shrinking to its size and fading in the
// rest, and stays (k: 0.5 for a texture up to 256 high, else 1). The lean's direction is (sin a,
// sin a * sin a - cos a), as the DLL works it out.
class EmCommon : public ParticleEffect {
public:
    EmCommon(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        right_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        out_ = static_cast<int32_t>(f[4]);
        settle_ = static_cast<int32_t>(f[5]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter - s.start);
            if (s.st == 0) {
                const float hi = static_cast<float>(out_);
                r.pos[0] = mapRange(emitter_[0], s.a[0], 0, hi, t);
                r.pos[1] = mapRange(emitter_[1], s.a[1], 0, hi, t);
                r.scale[1] = mapRange(size_, size_ * 1.3f, 0, hi, t);
                r.scale[0] = mapRange(size_, size_ * 1.3f, 0, hi, t);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 128, 0, hi, t)));
                if (out_ < s.counter - s.start) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                const float hi = static_cast<float>(settle_);
                r.pos[0] = mapRange(s.a[0], s.b[0], 0, hi, t);
                r.pos[1] = mapRange(s.a[1], s.b[1], 0, hi, t);
                r.scale[1] = mapRange(size_ * 1.3f, size_, 0, hi, t);
                r.scale[0] = mapRange(size_ * 1.3f, size_, 0, hi, t);
                setAlpha(r, static_cast<int32_t>(mapRange(128, 255, 0, hi, t)));
                if (settle_ < s.counter - s.start) s.st = 10, s.start = s.counter;
            } else {
                r.pos[0] = s.b[0], r.pos[1] = s.b[1];
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001988)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 3> a{}, b{};  // where it springs out to, where it settles
    };

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        r.v[0].x = -hw, r.v[0].y = -hh;
        r.v[1].x = hw, r.v[1].y = -hh;
        r.v[2].x = -hw, r.v[2].y = hw;  // as written: the half width
        r.v[3].x = hw, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        const float lean = right_ ? 0.5f : -0.5f;
        r.rot[2] = lean;
        const float a = static_cast<float>(static_cast<int32_t>(lean * 1000.0f)) / 1000.0f;
        const float c = plugCos(a), sn = plugSin(a);
        const float dx = sn, dy = dx * sn - c;
        for (int j = 0; j < 2; ++j) {
            const float d = size_ * (j == 0 ? 300.0f : 100.0f) * k_;
            auto& to = j == 0 ? s.a : s.b;
            to = {dx * d + emitter_[0], dy * d + emitter_[1], emitter_[2]};
        }
        setAlpha(r, 0);
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    bool right_ = false;
    float size_ = 1, k_ = 0.5f;
    int32_t out_ = 0, settle_ = 0;
};

// em_dis.ppi (Happiness 2): gloom lines and their flicker. Init 0x10001420, Update 0x10001540,
// spawn 0x10001bf0. The texture holds two marks side by side (each a quarter: its top half's left
// and right), each record showing one on the texture-sized quad about its centre.
//   f0 f1: emitter; f2: mirrored (non-zero); f3: size (percent); f4: frames out; f5: frames back;
//   f6: unused; f7: frames between flickers.
// Record 0 shows the left mark, the others the right one. Each slides 50 * size out along a lean
// of 0.7 radian (to the right when mirrored, else the left) while growing to 1.2 times and fading
// in to 190, then back while settling and fading in the rest. Then record 1 flickers: every f6
// frames it jumps to within 3 pixels of the emitter, turned at random and its mark flipped one of
// four ways; the others stay.
class EmDis : public ParticleEffect {
public:
    EmDis(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        mirror_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        out_ = static_cast<int32_t>(f[4]);
        back_ = static_cast<int32_t>(f[5]);
        flicker_ = static_cast<int32_t>(f[7]);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            const float ft = static_cast<float>(t);
            if (s.st == 0) {
                const float hi = static_cast<float>(out_);
                r.pos[0] = mapRange(emitter_[0], s.to[0], 0, hi, ft);
                r.pos[1] = mapRange(emitter_[1], s.to[1], 0, hi, ft);
                const float sc = mapRange(size_, size_ * 1.2f, 0, hi, ft);
                r.scale[0] = r.scale[1] = sc;
                setAlpha(r, static_cast<int32_t>(mapRange(0, 190, 0, hi, ft)));
                if (out_ < t) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                const float hi = static_cast<float>(back_);
                r.pos[0] = mapRange(s.to[0], emitter_[0], 0, hi, ft);
                r.pos[1] = mapRange(s.to[1], emitter_[1], 0, hi, ft);
                const float sc = mapRange(size_ * 1.2f, size_, 0, hi, ft);
                r.scale[0] = r.scale[1] = sc;
                setAlpha(r, static_cast<int32_t>(mapRange(190, 255, 0, hi, ft)));
                if (back_ < t) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                s.at = {emitter_[0], emitter_[1]};
                if (i == 1) {
                    s.at[0] = rng_.unit() * 6.0f - 3.0f + emitter_[0];
                    s.at[1] = rng_.unit() * 6.0f - 3.0f + emitter_[1];
                    rng_.unit();
                    r.rot[2] = rng_.unit() * 6.2831854820251465f;
                    const uint32_t q = rng_.next() & 3;
                    const bool fx = q == 1 || q == 3, fy = q >= 2;
                    quarter(r, 0.5f, fx, fy);
                }
                r.pos[0] = s.at[0], r.pos[1] = s.at[1];
                s.st = 3, s.start = s.counter;
            } else if (s.st == 3) {
                r.pos[0] = s.at[0], r.pos[1] = s.at[1];
                if (flicker_ < t) s.st = 2, s.start = s.counter;
            } else {
                r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001b8f)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 3> to{};
        std::array<float, 2> at{};
    };

    // The quarter at (u, 0), flipped across or down.
    static void quarter(ParticleRecord& r, float u, bool fx, bool fy) {
        const float u0 = fx ? u + 0.5f : u, u1 = fx ? u : u + 0.5f;
        const float v0 = fy ? 0.5f : 0.0f, v1 = fy ? 0.0f : 0.5f;
        r.v[0].u = u0, r.v[0].v = v0;
        r.v[1].u = u1, r.v[1].v = v0;
        r.v[2].u = u0, r.v[2].v = v1;
        r.v[3].u = u1, r.v[3].v = v1;
    }

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        quarter(r, i == 0 ? 0.0f : 0.5f, mirror_, false);
        const float lean = mirror_ ? 0.7f : -0.7f;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        const float a = static_cast<float>(static_cast<int32_t>(lean * 1000.0f)) / 1000.0f;
        const float d = size_ * 50.0f;
        s.to = {plugSin(a) * d + emitter_[0], -plugCos(a) * d + emitter_[1], emitter_[2]};
        setAlpha(r, 0);
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    bool mirror_ = false;
    float size_ = 1;
    int32_t out_ = 0, back_ = 0, flicker_ = 0;
};

// em_suprise.ppi (Happiness 2): a mark that spins in and shakes. Init 0x100013b0, Update
// 0x100014f0, spawn 0x10001870. One record per mark.
//   f0 f1: emitter; f2: mirrored (non-zero; 1 also spins the other way); f3: size (percent);
//   f4: frames to spin in; f5: frames the shake takes to die down.
// It turns two whole turns while shrinking from three times its size and fading in, then jumps
// between the four diagonals, one a frame, by up to 35 * size * k (k: 0.5 for a texture up to 256
// high, else 1), and stays. The quad is as em_notice's.
class EmSuprise : public ParticleEffect {
public:
    EmSuprise(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        side_ = static_cast<int32_t>(f[2]);
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        spin_ = static_cast<int32_t>(f[4]);
        shake_ = static_cast<int32_t>(f[5]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            const float ft = static_cast<float>(t);
            r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            if (s.st == 0) {
                const float hi = static_cast<float>(spin_);
                float rz = mapRange(0, 12.566371f, 0, hi, ft);
                if (side_ == 1) rz *= -1.0f;
                r.rot[2] = rz;
                const float sc = mapRange(size_ * 3.0f, size_, 0, hi, ft);
                r.scale[1] = sc;
                r.scale[0] = mapRange(size_ * 3.0f, size_, 0, hi, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, hi, ft)));
                if (spin_ < t) s.st = 1, r.rot[2] = 0, s.start = s.counter;
            } else if (s.st == 1) {
                const float a = mapRange(size_ * 35.0f * k_, 0, 0, static_cast<float>(shake_), ft);
                static constexpr float kDx[4] = {-1, 1, 1, -1}, kDy[4] = {-1, 1, -1, 1};
                const int32_t m = t % 4;
                if (m >= 0) r.pos[0] = emitter_[0] + kDx[m] * a, r.pos[1] = emitter_[1] + kDy[m] * a;
                if (shake_ < t) s.st = 10, s.start = s.counter;
            }
            addMove(r, planeMove());  // where the branches meet (0x1000180b)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        const bool mirror = side_ != 0;
        const float l = mirror ? hw : -hw, rt = mirror ? -hw : hw;
        r.v[0].x = l, r.v[0].y = -hh;
        r.v[1].x = rt, r.v[1].y = -hh;
        r.v[2].x = l, r.v[2].y = hw;  // as written: the half width
        r.v[3].x = rt, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        r.scale = {size_ * 3.0f, size_ * 3.0f, 1};
        setAlpha(r, 0);
        state_[i] = State{};
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    int32_t side_ = 0;
    float size_ = 1, k_ = 0.5f;
    int32_t spin_ = 0, shake_ = 0;
};

// The host's acos (kcTaskParticlePluginFunc +0x28, after sin, cos, tan and asin): of the float,
// as a float.
float plugAcos(float x) { return static_cast<float>(std::acos(static_cast<double>(x))); }

// em_sigh.ppi (Happiness 2): a sigh's puff and its trail. Init 0x100020e0, Update 0x100021f0,
// spawn 0x100028c0. One group: the puff (records[0], the texture's top-left quarter) and the rest
// its trail (the top-right quarter).
//   f0 f1: emitter; f2: to the right (else the left; the quarters mirrored); f3: size (percent);
//   f4: frames to fade in; f5: frames more along the path.
// The path (a Hermite path, tangents 25 long, over f4 + f5 frames) runs from the emitter 100 *
// size along a lean of 2.2 radian (down and out), then 100 * size / 0.99 along 0.85 of that. The
// puff turns along its way: acos of its direction from the emitter's y, negative while it heads
// right, less 0.8 radian (plus it when to the right); it fades in over f4 and stays where the path
// ends. While it moves, each update leaves a trail copy where it is, one unit back, at alpha 30
// fading to 0 over 20 frames and growing 0.5% a frame; a copy is free again after 20.
class EmSigh : public ParticleEffect {
public:
    EmSigh(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        right_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        fadeIn_ = static_cast<int32_t>(f[4]);
        more_ = static_cast<int32_t>(f[5]);
        trail_.resize(records_.empty() ? 0 : records_.size() - 1);
        if (!records_.empty()) spawn();
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        ParticleRecord& r = records_[0];
        const float t = static_cast<float>(counter_ - start_);
        if (st_ <= 1) {
            auto at = path_.at(static_cast<float>(counter_));
            r.pos[0] = at[0], r.pos[1] = at[1];
            if (st_ == 0) setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(fadeIn_), t)));
            float dx = r.pos[0] - emitter_[0], dy = r.pos[1] - emitter_[1], dz = r.pos[2] - emitter_[2];
            if (float l = static_cast<float>(std::sqrt(static_cast<double>(dy * dy + dx * dx + dz * dz))); 0.0f < l) dx /= l, dy /= l;
            float a = plugAcos(dx * 0.0f + dy + dz * 0.0f);
            if (0.0f < dy * 0.0f + dx) a *= -1.0f;
            r.rot[2] = right_ ? a + 0.8f : a - 0.8f;
            if (st_ == 0 && fadeIn_ < counter_ - start_) st_ = 1, start_ = counter_;
            else if (st_ == 1 && more_ < counter_ - start_) st_ = 10, start_ = counter_;
        } else {
            auto at = path_.at(static_cast<float>(counter_));
            r.pos[0] = at[0], r.pos[1] = at[1];
        }
        // A copy left behind (the first free one), then the copies fading.
        if (st_ != 10) {
            for (size_t k = 0; k < trail_.size(); ++k) {
                if (trail_[k].on) continue;
                ParticleRecord& c = records_[k + 1];
                trail_[k].at = r.pos;
                c.pos = r.pos;
                c.pos[2] -= 1.0f;
                c.rot = r.rot;
                c.scale = r.scale;
                trail_[k].on = true;
                break;
            }
        }
        for (size_t k = 0; k < trail_.size(); ++k) {
            Trail& tr = trail_[k];
            if (!tr.on) continue;
            ParticleRecord& c = records_[k + 1];
            c.pos[0] = tr.at[0], c.pos[1] = tr.at[1];
            setAlpha(c, static_cast<int32_t>(mapRange(30, 0, 0, 20, static_cast<float>(tr.counter))));
            c.scale[0] *= 1.005f;
            c.scale[1] = c.scale[0];
            tr.counter += frames;
            if (20 < tr.counter) setAlpha(c, 0), tr.counter = 0, tr.on = false;
            addMove(c, planeMove());  // each copy that was out (0x10002820)
        }
        addMove(r, planeMove());  // then the puff (0x10002862)
        counter_ += frames;
    }

private:
    struct Trail {
        int32_t counter = 0;
        bool on = false;
        std::array<float, 3> at{};
    };

    static void quarter(ParticleRecord& r, float u, bool mirror) {
        const float u0 = mirror ? u + 0.5f : u, u1 = mirror ? u : u + 0.5f;
        r.v[0].u = u0, r.v[0].v = 0;
        r.v[1].u = u1, r.v[1].v = 0;
        r.v[2].u = u0, r.v[2].v = 0.5f;
        r.v[3].u = u1, r.v[3].v = 0.5f;
    }

    void spawn() {
        ParticleRecord& r = records_[0];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        quarter(r, 0, right_);
        const float lean = right_ ? 2.2f : -2.2f;
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
        r.pos = emitter_;
        const float len = size_ * 100.0f;
        path_ = SplinePath{};
        path_.add(r.pos);
        const float a1 = static_cast<float>(static_cast<int32_t>(lean * 1000.0f)) / 1000.0f;
        const std::array<float, 3> p1{plugSin(a1) * len + r.pos[0], -plugCos(a1) * len + r.pos[1], r.pos[2]};
        path_.add(p1);
        const float a2 = static_cast<float>(static_cast<int32_t>(lean * 0.85f * 1000.0f)) / 1000.0f;
        path_.add({plugSin(a2) * len / 0.99f + p1[0], -plugCos(a2) * len / 0.99f + p1[1], p1[2]});
        path_.finish(static_cast<float>(fadeIn_ + more_), 25.0f);
        counter_ = start_ = st_ = 0;
        for (size_t k = 0; k < trail_.size(); ++k) {
            ParticleRecord& c = records_[k + 1];
            trail_[k] = Trail{};
            c.pivot = r.pivot;
            c.pos = r.pos;
            c.scale = r.scale;
            quarter(c, 0.5f, right_);
            setAlpha(c, 0);
        }
    }

    std::vector<Trail> trail_;
    SplinePath path_;
    int32_t counter_ = 0, start_ = 0, st_ = 0;
    std::array<float, 3> emitter_{};
    bool right_ = false;
    float size_ = 1;
    int32_t fadeIn_ = 0, more_ = 0;
};

// em_down.ppi (Happiness 2): a gloomy cloud and the lines hanging from it. Init 0x100013b0,
// Update 0x10001520, cloud 0x100015e0 / 0x10001c70, lines 0x100018d0 / 0x10001d80. Record 0 is the
// cloud (the texture's top half, from a quarter across), the rest are lines (its left quarter).
//   f0 f1: emitter; f2: size (percent); f3 f4: the cloud's offset from it; f5: frames to grow; f6:
//   frames to settle (f7-f10 unused).
// The cloud grows from 1% to 250% across and 180% down of the size, and fades in to half, over
// f5 + f6 frames from the start (it stops at f5). Line i (from 1, one more every six updates)
// hangs from 50 * i * size right of the emitter and up to 20 * size below it, by its top: it
// widens from 2.5% to 25% and lengthens from 10% to 1.2 times the size while fading in over f5,
// then shortens to 80-110% of the size over f6, and stays.
class EmDown : public ParticleEffect {
public:
    EmDown(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        offX_ = f[3], offY_ = f[4];
        grow_ = static_cast<int32_t>(f[5]);
        settle_ = static_cast<int32_t>(f[6]);
        state_.resize(records_.size());
        if (records_.empty()) return;
        cloud();
        for (size_t i = 1; i < records_.size(); ++i) line(i);
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        if (5 < timer_) {
            ++active_;
            timer_ = 0;
            active_ = std::min(active_, records_.size());
        }
        {
            State& s = state_[0];
            ParticleRecord& r = records_[0];
            if (s.st <= 1) {
                r.pos[0] = s.at[0], r.pos[1] = s.at[1];
                const float hi = static_cast<float>(grow_ + settle_), c = static_cast<float>(s.counter);
                r.scale[0] = mapRange(size_ * 0.01f, size_ * 2.5f, 0, hi, c);
                r.scale[1] = mapRange(size_ * 0.01f, size_ * 1.8f, 0, hi, c);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 128, 0, hi, c)));
                if (s.st == 0 && grow_ < s.counter) s.st = 1, s.start = s.counter;
                else if (s.st == 1 && settle_ < s.counter) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                r.pos[0] = s.at[0], r.pos[1] = s.at[1];
            } else {
                r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            }
            addMove(r, planeMove());  // the cloud, at the end of its update (0x1000188d)
            s.counter += frames;
        }
        for (size_t i = 1; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            const float ft = static_cast<float>(t);
            r.pos[0] = s.at[0], r.pos[1] = s.at[1];
            if (s.st == 0) {
                const float hi = static_cast<float>(grow_);
                r.scale[0] = mapRange(size_ * 0.025f, size_ * 0.25f, 0, hi, ft);
                r.scale[1] = mapRange(size_ * 0.1f, s.length * 1.2f, 0, hi, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, hi, ft)));
                if (grow_ < t) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                r.scale[1] = mapRange(size_ * 1.2f, s.length, 0, static_cast<float>(settle_), ft);
                if (settle_ < t) s.st = 2, s.start = s.counter;
            }
            addMove(r, planeMove());  // each line, at the end of its update (0x10001c16)
            s.counter += frames;
        }
        ++timer_;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 2> at{};
        float length = 0;
    };

    void cloud() {
        ParticleRecord& r = records_[0];
        State& s = state_[0];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        r.v[0].u = 0.25f, r.v[0].v = 0;
        r.v[1].u = 1.0f, r.v[1].v = 0;
        r.v[2].u = 0.25f, r.v[2].v = 0.5f;
        r.v[3].u = 1.0f, r.v[3].v = 0.5f;
        s.at = {offX_ + emitter_[0], offY_ + emitter_[1]};
        r.pos = {s.at[0], s.at[1], 0};
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
    }

    void line(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot = {params_.texW * 0.5f, 0, 0};  // its top
        r.v[0].u = 0, r.v[0].v = 0;
        r.v[1].u = 0.25f, r.v[1].v = 0;
        r.v[2].u = 0, r.v[2].v = 1.0f;
        r.v[3].u = 0.25f, r.v[3].v = 1.0f;
        s.at[0] = static_cast<float>(static_cast<int32_t>(i) * 50) * size_ + emitter_[0];
        s.at[1] = rng_.unit() * (size_ * 20.0f) + emitter_[1];
        r.pos = {s.at[0], s.at[1], static_cast<float>(i)};
        s.length = (rng_.unit() * 0.3f + 0.8f) * size_;
        r.scale = {size_ * 0.025f, size_ * 0.1f, 1};
        setAlpha(r, 0);
    }

    std::vector<State> state_;
    size_t active_ = 1;
    int32_t timer_ = 0;
    std::array<float, 3> emitter_{};
    float size_ = 1, offX_ = 0, offY_ = 0;
    int32_t grow_ = 0, settle_ = 0;
};

// em_silent.ppi (Happiness 2): a speech balloon and its dots, one by one. Init 0x100013a0, Update
// 0x10001500, balloon 0x100015b0 / 0x10001bf0, dots 0x10001880 / 0x10001d70. Record 0 is the
// balloon (the texture's top three quarters), records 1-3 the dots (quarters of its bottom row,
// the second to the fourth).
//   f0 f1: emitter; f2: mirrored (non-zero; the balloon only); f3: size (percent); f4: frames to
//   pop up; f5: frames to settle (f6-f9 unused).
// The balloon grows from 1% to 270% across and 220% down of the size while fading in over f4
// frames from the start, then to 250% / 200% by f5 (from the start too), and stays. Dot i (one
// more every 31 updates) sits at (300 i - 600) * size * k across and -10 * size * k down from the
// emitter (k: 0.5 for a texture up to 256 high, else 1), growing from 10% to 1.2 times the size
// while fading in over f4, then to 70% over f5.
class EmSilent : public ParticleEffect {
public:
    EmSilent(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        mirror_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        pop_ = static_cast<int32_t>(f[4]);
        settle_ = static_cast<int32_t>(f[5]);
        k_ = p.texH <= 256.0f ? 0.5f : 1.0f;
        state_.resize(records_.size());
        if (records_.empty()) return;
        balloon();
        for (size_t i = 1; i < records_.size(); ++i) dot(i);
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        if (30 < timer_) {
            ++active_;
            timer_ = 0;
            active_ = std::min(active_, records_.size());
        }
        {
            State& s = state_[0];
            ParticleRecord& r = records_[0];
            const float c = static_cast<float>(s.counter);
            if (s.st <= 2) r.pos[0] = s.at[0], r.pos[1] = s.at[1];
            else r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
            if (s.st == 0) {
                const float hi = static_cast<float>(pop_);
                r.scale[0] = mapRange(size_ * 0.01f, size_ * 2.7f, 0, hi, c);
                r.scale[1] = mapRange(size_ * 0.01f, size_ * 2.2f, 0, hi, c);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, hi, c)));
                if (pop_ < s.counter) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                const float hi = static_cast<float>(settle_);
                r.scale[0] = mapRange(size_ * 2.7f, size_ * 2.5f, 0, hi, c);
                r.scale[1] = mapRange(size_ * 2.2f, size_ * 2.0f, 0, hi, c);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, hi, c)));
                if (settle_ < s.counter) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                if (settle_ < s.counter) s.start = s.counter;
            }
            addMove(r, planeMove());  // the balloon, at the end of its update (0x1000183f)
            s.counter += frames;
        }
        for (size_t i = 1; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            const float ft = static_cast<float>(t);
            r.pos[0] = s.at[0], r.pos[1] = s.at[1];
            if (s.st == 0) {
                const float sc = mapRange(size_ * 0.1f, size_ * 1.2f, 0, static_cast<float>(pop_), ft);
                r.scale[0] = r.scale[1] = sc;
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(pop_), ft)));
                if (pop_ < t) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                const float sc = mapRange(size_ * 1.2f, size_ * 0.7f, 0, static_cast<float>(settle_), ft);
                r.scale[0] = r.scale[1] = sc;
                if (settle_ < t) s.st = 2, s.start = s.counter;
            }
            addMove(r, planeMove());  // each dot, at the end of its update (0x10001b92)
            s.counter += frames;
        }
        ++timer_;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 2> at{};
    };

    void balloon() {
        ParticleRecord& r = records_[0];
        State& s = state_[0];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        const float u0 = mirror_ ? 1.0f : 0.0f, u1 = mirror_ ? 0.0f : 1.0f;
        r.v[0].u = u0, r.v[0].v = 0;
        r.v[1].u = u1, r.v[1].v = 0;
        r.v[2].u = u0, r.v[2].v = 0.75f;
        r.v[3].u = u1, r.v[3].v = 0.75f;
        s.at = {emitter_[0], emitter_[1]};
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
    }

    void dot(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        // The DLL's table of each record's quarter (.data 0x100140b0); past the fourth it reads on.
        static constexpr float kUv[6][2] = {{0, 0}, {0.25f, 0.75f}, {0.5f, 0.75f}, {0.75f, 0.75f}, {2.0f, -2.0f}, {1.0f, 1.0f}};
        const float u = kUv[std::min<size_t>(i, 5)][0], v = kUv[std::min<size_t>(i, 5)][1];
        r.v[0].u = u, r.v[0].v = v;
        r.v[1].u = u + 0.25f, r.v[1].v = v;
        r.v[2].u = u, r.v[2].v = v + 0.25f;
        r.v[3].u = u + 0.25f, r.v[3].v = v + 0.25f;
        s.at[0] = (emitter_[0] - size_ * 600.0f * k_) + static_cast<float>(static_cast<int32_t>(i) * 300) * size_ * k_;
        s.at[1] = size_ * -10.0f * k_ + emitter_[1];
        r.pos = {s.at[0], s.at[1], static_cast<float>(i)};
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
    }

    std::vector<State> state_;
    size_t active_ = 1;
    int32_t timer_ = 0;
    std::array<float, 3> emitter_{};
    bool mirror_ = false;
    float size_ = 1, k_ = 0.5f;
    int32_t pop_ = 0, settle_ = 0;
};

// em_exclamation.ppi (Happiness 2): a "!" that jumps, squashes and wobbles. Init 0x10001420,
// Update 0x10001550, spawn 0x10001c70. One record per mark; the quad as em_common's.
//   f0 f1: emitter; f2: leans right (else left) by 0.4 radian; f3: size (percent); f4: frames out
//   (and the move of the third step); f5: frames back; f6: frames to settle; f7: frames of a
//   wobble; f8: frames between wobbles (and the wobble's half period).
// Out to 100 along the lean (direction as em_common's) while stretching to 60% across and 150%
// down and fading in to half; back to the emitter squashing to 180% / 50% and fading in the rest;
// then out to 40 while settling to its size. Then it waits f8 frames and wobbles for f7 (its turn
// growing by sin(pi t / f8) / 2 a frame), back to upright, and so on.
class EmExclamation : public ParticleEffect {
public:
    EmExclamation(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        right_ = static_cast<int32_t>(f[2]) != 0;
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        for (size_t k = 0; k < 5; ++k) frames_[k] = static_cast<int32_t>(f[4 + k]);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            const float ft = static_cast<float>(t);
            const float f0 = static_cast<float>(frames_[0]), f1 = static_cast<float>(frames_[1]), f2 = static_cast<float>(frames_[2]);
            switch (s.st) {
            case 0:
                r.pos[0] = mapRange(emitter_[0], s.a[0], 0, f0, ft);
                r.pos[1] = mapRange(emitter_[1], s.a[1], 0, f0, ft);
                r.scale[1] = mapRange(size_, size_ * 1.5f, 0, f0, ft);
                r.scale[0] = mapRange(size_, size_ * 0.6f, 0, f0, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(0, 128, 0, f0, ft)));
                if (frames_[0] < t) s.st = 1, s.start = s.counter;
                break;
            case 1:
                r.pos[0] = mapRange(s.a[0], emitter_[0], 0, f1, ft);
                r.pos[1] = mapRange(s.a[1], emitter_[1], 0, f1, ft);
                r.scale[1] = mapRange(size_ * 1.5f, size_ * 0.5f, 0, f1, ft);
                r.scale[0] = mapRange(static_cast<float>(static_cast<double>(size_) * 0.6), size_ * 1.8f, 0, f1, ft);
                setAlpha(r, static_cast<int32_t>(mapRange(128, 255, 0, f1, ft)));
                if (frames_[1] < t) s.st = 2, s.start = s.counter;
                break;
            case 2:
                r.pos[0] = mapRange(emitter_[0], s.b[0], 0, f0, ft);
                r.pos[1] = mapRange(emitter_[1], s.b[1], 0, f0, ft);
                r.scale[1] = mapRange(size_ * 0.5f, size_, 0, f2, ft);
                r.scale[0] = mapRange(size_ * 1.8f, size_, 0, f2, ft);
                if (frames_[2] < t) s.st = 3, s.start = s.counter;
                break;
            case 3:
                r.pos[0] = s.b[0], r.pos[1] = s.b[1];
                if (frames_[4] < t) s.st = 4, s.start = s.counter;
                break;
            case 4:
                r.pos[0] = s.b[0], r.pos[1] = s.b[1];
                r.rot[2] += plugSin(3.1415927f / static_cast<float>(frames_[4]) * ft) * 0.5f;
                if (frames_[3] < t) s.st = 3, r.rot[2] = 0, s.start = s.counter;
                break;
            default:
                spawn(i);
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001bfd)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 3> a{}, b{};
    };

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        const float hw = params_.texW * 0.5f, hh = params_.texH * 0.5f;
        r.v[0].x = -hw, r.v[0].y = -hh;
        r.v[1].x = hw, r.v[1].y = -hh;
        r.v[2].x = -hw, r.v[2].y = hw;  // as written: the half width
        r.v[3].x = hw, r.v[3].y = hh;
        for (auto& v : r.v) v.z = 0;
        r.pos = emitter_;
        r.scale = {size_, size_, 1};
        const float lean = right_ ? 0.4f : -0.4f;
        r.rot[2] = lean;
        const float a = static_cast<float>(static_cast<int32_t>(lean * 1000.0f)) / 1000.0f;
        const float c = plugCos(a), sn = plugSin(a);
        const float dx = sn, dy = dx * sn - c;
        s.a = {dx * 100.0f + emitter_[0], dy * 100.0f + emitter_[1], emitter_[2]};
        s.b = {dx * 40.0f + emitter_[0], dy * 40.0f + emitter_[1], emitter_[2]};
        setAlpha(r, 0);
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    bool right_ = false;
    float size_ = 1;
    std::array<int32_t, 5> frames_{};
};

// em_kyupin.ppi (Happiness 2): a mark that pops and pulses, with sparkles. Init 0x10001410,
// Update 0x10001550, spawn 0x10001e30, the sparkles' curve 0x100020b0. One group: the mark
// (records[0]) and the rest its sparkles.
//   f0 f1: emitter; f2: (unused); f3: size (percent); f4: frames to pop up; f5: frames of each
//   swell and fall; f6: frames between pulses; f7, f8: when a pulse's swell and fall end.
// The mark grows from 10% to 110% of the size while fading in, falls back to its size, then
// pulses: every f6 frames it swells to 1.05 times (until f7) and falls back (until f8). Each free
// sparkle starts with chance 1 in 300 an update, from the emitter's 31 r1 * the mark's scale along
// a random diagonal-ish direction to 31 r2 * that further out; it moves out along the host's curve
// (x by cos from 90 to 0 degrees, y by sin from 180 to 270) and fades over as many frames as there
// are sparkles.
class EmKyupin : public ParticleEffect {
public:
    EmKyupin(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        for (size_t k = 0; k < 5; ++k) frames_[k] = static_cast<int32_t>(f[4 + k]);
        sparks_.resize(records_.empty() ? 0 : records_.size() - 1);
        if (!records_.empty()) spawn();
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        ParticleRecord& r = records_[0];
        const int32_t t = counter_ - start_;
        const float ft = static_cast<float>(t), swell = static_cast<float>(frames_[1]);
        r.pos[0] = emitter_[0], r.pos[1] = emitter_[1];
        auto both = [&](float v) { r.scale[0] = v, r.scale[1] = v; };
        switch (st_) {
        case 0:
            both(mapRange(size_ * 0.1f, size_ * 1.1f, 0, static_cast<float>(frames_[0]), ft));
            setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(frames_[0]), ft)));
            if (frames_[0] < t) r.rot[2] = 0, st_ = 1, start_ = counter_;
            break;
        case 1:
            both(mapRange(size_ * 1.1f, size_, 0, swell, ft));
            if (frames_[1] < t) st_ = 2, start_ = counter_;
            break;
        case 2:
            if (frames_[2] < t) st_ = 3, start_ = counter_;
            break;
        case 3:
            both(mapRange(size_, size_ * 1.05f, 0, swell, ft));
            if (frames_[3] < t) st_ = 4, start_ = counter_;
            break;
        case 4:
            both(mapRange(size_ * 1.05f, size_, 0, swell, ft));
            if (frames_[4] < t) st_ = 2, start_ = counter_;
            break;
        default:
            break;
        }
        // A sparkle starting (the first free one that draws it), then the sparkles moving.
        for (size_t k = 0; k < sparks_.size(); ++k) {
            Spark& sp = sparks_[k];
            if (sp.on || rng_.next() % 300 != 0) continue;
            float dx = rng_.unit(), dy = rng_.unit(), dz = 0;
            if (float l = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz))); 0.0f < l) dx /= l, dy /= l;
            dx *= static_cast<float>((rng_.next() & 1) != 0 ? 1 : -1);
            dy *= static_cast<float>((rng_.next() & 1) != 0 ? 1 : -1);
            float r1 = rng_.unit();
            r1 = r1 + 30.0f * r1;
            sp.a = {dx * r1 * r.scale[0] + emitter_[0], dy * r1 * r.scale[0] + emitter_[1], dz * r1 * r.scale[0] + emitter_[2]};
            float r2 = rng_.unit();
            r2 = r2 + 30.0f * r2;
            sp.b = {dx * r2 * r.scale[0] + sp.a[0], dy * r2 * r.scale[0] + sp.a[1], dz * r2 * r.scale[0] + sp.a[2]};
            ParticleRecord& c = records_[k + 1];
            c.pivot = r.pivot;
            c.pos = r.pos;
            c.scale[0] = c.scale[1] = size_ * 0.1f;
            sp.on = true;
            break;
        }
        const float dur = static_cast<float>(sparks_.size());
        for (size_t k = 0; k < sparks_.size(); ++k) {
            Spark& sp = sparks_[k];
            if (!sp.on) continue;
            ParticleRecord& c = records_[k + 1];
            auto at = curve(sp.a, sp.b, static_cast<float>(sp.counter), dur);
            c.pos[0] = at[0], c.pos[1] = at[1];
            setAlpha(c, static_cast<int32_t>(mapRange(255, 0, 0, dur, static_cast<float>(sp.counter))));
            sp.counter += frames;
            if (static_cast<int32_t>(sparks_.size()) <= sp.counter) sp.counter = 0, sp.on = false, setAlpha(c, 0);
            addMove(c, planeMove());  // each sparkle that was out (0x10001d63)
        }
        addMove(r, planeMove());  // then the mark (0x10001da5)
        counter_ += frames;
    }

private:
    struct Spark {
        int32_t counter = 0;
        bool on = false;
        std::array<float, 3> a{}, b{};
    };

    // 0x100020b0: from a to b, x and z easing out (cos of 90 to 0 degrees), y easing in (sin of
    // 180 to 270), over d frames; b once t reaches d.
    std::array<float, 3> curve(const std::array<float, 3>& a, const std::array<float, 3>& b, float t, float d) const {
        if (t < 1.0f) t = 1.0f;
        if (d <= static_cast<float>(static_cast<int32_t>(t))) return b;
        const float r90 = plugRad(90), r0 = plugRad(0), r180 = plugRad(180), r270 = plugRad(270);
        std::array<float, 3> out{};
        const float mx = mapRange(r90, r0, 0, d, t);
        out[0] = (plugCos(mx) - plugCos(r90)) * (b[0] - a[0]) + a[0];
        const float my = mapRange(r180, r270, 0, d, t);
        out[1] = a[1] - (plugSin(my) - plugSin(r180)) * (b[1] - a[1]);
        out[2] = (plugCos(mx) - plugCos(r90)) * (b[2] - a[2]) + a[2];
        return out;
    }

    void spawn() {
        ParticleRecord& r = records_[0];
        r.pivot = {params_.texW * 0.5f, params_.texH * 0.5f, 0};
        r.pos = emitter_;
        r.scale = {size_ * 0.1f, size_ * 0.1f, 1};
        setAlpha(r, 0);
        for (size_t k = 0; k < sparks_.size(); ++k) {
            ParticleRecord& c = records_[k + 1];
            sparks_[k] = Spark{};
            c.pos = r.pos;
            c.pivot = r.pivot;
            setAlpha(c, 0);
        }
        counter_ = start_ = st_ = 0;
    }

    std::vector<Spark> sparks_;
    int32_t counter_ = 0, start_ = 0, st_ = 0;
    std::array<float, 3> emitter_{};
    float size_ = 1;
    std::array<int32_t, 5> frames_{};
};

// asease.ppi (Happiness 2, 800 uses): sweat drops popping out beside a head, by turns left and
// right. Init 0x100013d0, Update 0x100014c0, spawn 0x10001760. Every record is a drop (the base's
// group size is 0, as in em_shy); none is out at first, and each update one more comes out when
// the random number is even. A drop grows from nothing to its size while fading in, swells to 1.3
// times and settles back, then starts again.
//   f0 f1: emitter; f2: size (percent; each drop 80-100% of it); f3: distance (each 0-(f3 + 1));
//   f4: half the spread (degrees); f5: frames to grow; f6: frames to settle.
// The direction (the spawn's x87 code, step by step): the two edges of the spread, (±1, 0) turned
// by -f4 and f4 degrees through the host's cos / sin (+0x1c / +0x18 of +0x10's radians), and a
// random point between them, but x is ±1 plus a random offset across their x gap (their x is the
// same) and y one across their y gap, each gap the host's sqrt of its pow 2 (+0x44 / +0x48); then
// made unit length (the CRT's sqrt). The distance is one random number r times f3, plus r.
class Asease : public ParticleEffect {
public:
    Asease(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        dist_ = f[3];
        spread_ = f[4];
        grow_ = f[5];
        settle_ = f[6];
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        if ((rng_.next() & 1) == 0) ++active_;
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st == 0) {
                r.pos = s.pos;
                const float sc = mapRange(0, s.size, 0, grow_, static_cast<float>(s.counter));
                r.scale = {sc, sc, 1};
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, grow_, static_cast<float>(s.counter))));
                if (grow_ < static_cast<float>(s.counter)) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                r.pos = s.pos;
                const float sc = mapRange(s.size * 1.3f, s.size, 0, settle_, static_cast<float>(s.counter - s.start));
                r.scale = {sc, sc, 1};
                if (settle_ < static_cast<float>(s.counter - s.start)) s.st = 10, s.start = s.counter;
            } else {
                spawn(i);
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x100016fd)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float size = 0;
        std::array<float, 3> pos{};
    };

    // The host's pow (+0x48) and sqrt (+0x44): the CRT's, of doubles, kept as floats.
    static float hostPow(float x, float y) { return static_cast<float>(std::pow(static_cast<double>(x), static_cast<double>(y))); }
    static float hostSqrt(float x) { return static_cast<float>(std::sqrt(static_cast<double>(x))); }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        s.size = rng_.unit() * size_ * 0.2f + size_ * 0.8f;
        r.scale = {0, 0, 1};
        rng_.unit();  // 50-100% of the grow and settle frames, kept but not used
        rng_.unit();
        setAlpha(r, 0);
        const float sign = i % 2 == 0 ? 1.0f : -1.0f;
        const float a1 = plugRad(-spread_), a2 = plugRad(spread_);
        const float x1 = sign * plugCos(a1), y1 = sign * plugSin(a1);
        const float x2 = sign * plugCos(a2), y2 = sign * plugSin(a2);
        const float dx = hostSqrt(hostPow(x2 - x1, 2.0f));
        const float xo = rng_.unit() * dx - dx * 0.5f + sign;
        const float dy = hostSqrt(hostPow(y2 - y1, 2.0f));
        const float yo = rng_.unit() * dy - dy * 0.5f + 0.0f;
        std::array<float, 3> d{xo, yo, 0};
        const float len = static_cast<float>(std::sqrt(static_cast<double>(yo * yo + xo * xo + 0.0f)));
        if (0.0f < len) d = {xo / len, yo / len, 0.0f / len};
        const float rr = rng_.unit();
        const float dist = static_cast<float>(static_cast<long double>(rr) + static_cast<long double>(dist_) * rr);
        s.pos = {emitter_[0] + d[0] * dist, emitter_[1] + d[1] * dist, emitter_[2] + d[2] * dist};
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    size_t active_ = 0;
    float size_ = 0, dist_ = 0, spread_ = 0, grow_ = 0, settle_ = 0;
};

// ohanaburst.ppi (Happiness 2, 188 uses): flowers popping out around a point, then bursting.
// Init 0x10001380, Update 0x100014a0, spawn 0x10001b10. Every record is a flower (group size 0);
// none is out at first, one more each update with an even random number. A flower (one of the
// texture's top-left, top-right and bottom-left quarters) grows to 1.3 times its size while fading
// in, settles to its size turning 0.1 radian a frame, waits, turns into the burst (the bottom-right
// quarter) which swells from 0.8 to 1.2 times while fading in, then shrinks to 0.1 times while
// fading from 170 and turning 0.05 a frame, and starts again.
//   f0 f1: centre; f2: size (percent; each 50-150% of it); f3 f4: reach in x / y; f5 f6: the least
//   part of the reach (percent); f7 f8 f9 f10 f11: frames to grow, settle, wait, burst, fade.
class Ohanaburst : public ParticleEffect {
public:
    Ohanaburst(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        centre_ = {f[0], f[1]};
        size_ = f[2] / 100.0f;
        reach_ = {f[3], f[4]};
        least_ = {f[5] / 100.0f, f[6] / 100.0f};
        for (int k = 0; k < 5; ++k) frames_[static_cast<size_t>(k)] = f[static_cast<size_t>(7 + k)];
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        if ((rng_.next() & 1) == 0) ++active_;
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float c = static_cast<float>(s.counter), t = static_cast<float>(s.counter - s.start);
            auto scale = [&](float v) { r.scale = {v, v, 1}; };
            auto next = [&](int32_t st) { s.st = st, s.start = s.counter; };
            if (s.st <= 4) r.pos = s.pos;
            switch (s.st) {
            case 0:
                scale(mapRange(0, s.size * 1.3f, 0, frames_[0], c));
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, frames_[0], c)));
                if (frames_[0] < c) next(1);
                break;
            case 1:
                r.rot[2] += 0.1f;
                scale(mapRange(s.size * 1.3f, s.size, 0, frames_[1], t));
                if (frames_[1] < t) next(2);
                break;
            case 2:
                if (frames_[2] < t) {
                    setUv(r, 0.5f, 0.5f, 0.5f, 0.5f);
                    setAlpha(r, 0);
                    next(3);
                }
                break;
            case 3:
                scale(mapRange(s.size * 0.8f, s.size * 1.2f, 0, frames_[3], t));
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, frames_[3], t)));
                if (frames_[3] < t) next(4);
                break;
            case 4:
                r.rot[2] += 0.05f;
                scale(mapRange(s.size * 1.2f, s.size * 0.1f, 0, frames_[4], t));
                setAlpha(r, static_cast<int32_t>(mapRange(170, 0, 0, frames_[4], t)));
                if (frames_[4] < t) next(5);
                break;
            default:
                spawn(i);
                break;
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001a92)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float size = 0;
        std::array<float, 3> pos{};
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        static constexpr float kFlower[3][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}};
        const uint32_t q = rng_.next() % 3;
        setUv(r, kFlower[q][0], kFlower[q][1], 0.5f, 0.5f);
        s.size = static_cast<float>(static_cast<long double>(rng_.unit()) * size_ + static_cast<long double>(size_ * 0.5f));
        r.scale = {0, 0, 1};
        const uint32_t side = rng_.next();
        const float angle = (rng_.unit() * 0.5f + 0.5f) * static_cast<float>((side & 1) != 0 ? 1 : -1);
        r.rot = {0, 0, angle};
        setAlpha(r, 0);
        float dx = static_cast<float>(static_cast<long double>(rng_.unit()) - 0.5L);
        float dy = static_cast<float>(static_cast<long double>(rng_.unit()) - 0.5L);
        const float len = static_cast<float>(std::sqrt(static_cast<double>(dy * dy + dx * dx + 0.0f)));
        if (0.0f < len) dx /= len, dy /= len;
        const float spanX = 1.0f - least_[0], spanY = 1.0f - least_[1];
        s.pos[0] = (rng_.unit() * spanX + least_[0]) * reach_[0] * dx + centre_[0];
        s.pos[1] = (rng_.unit() * spanY + least_[1]) * reach_[1] * dy + centre_[1];
        s.pos[2] = s.size + 0.0f;
        r.pos = s.pos;
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    std::array<float, 2> centre_{}, reach_{}, least_{};
    std::array<float, 5> frames_{};
    float size_ = 0;
    size_t active_ = 0;
};

// em_shy.ppi (Happiness 2, 280 uses): a general emitter, here the blush lines. Init 0x10002370,
// Update 0x10002860, spawn 0x10002e10. Every record is a particle of its own (the base's group
// size comes in ECX straight after the CRT memset, 0, so no trail copies); one is out at the
// start, and each update with a random number divisible by the rate brings out more.
//   f0 + f1: distance (base, at least 10, and range); f2: direction and f3: its spread (hundredths
//   of a radian); f4 f5: size and its range (percent); f6 f7: alpha and its range; f8 f9 f10:
//   frames to fade in, hold, fade out; f11 f12 - f13 f14: the line particles start on (at a random
//   point of a path along it, 100 long); f15: rate (one emission every 61 - f15 updates, 1-60);
//   f16: particles per emission (at least 1).
// A particle takes a random quarter of the texture and follows its own Hermite path (tangents 0.4
// of its life long, over its life): from its start, one leg along the line's normal turned by the
// direction plus up to a quarter turn either way, then up to three shorter legs (0.9, 0.81, 0.72
// of a distance, each kept 4 times in 5) within the spread. It rises 0.1 px a frame on top.
class EmShy : public ParticleEffect {
public:
    EmShy(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        blend_ = 1;  // Init 0x100023d5: additive
        constexpr float kPi = 3.14159265f;
        auto clampPi = [&](float a) { return a > kPi ? kPi : a < -kPi ? -kPi : a; };
        dist_ = f[0] < 10.0f ? 10.0f : f[0];
        distRange_ = std::max(f[1], 0.0f);
        dir_ = clampPi(f[2] / 100.0f);
        spread_ = clampPi(f[3] / 100.0f);
        size_ = std::max(f[4] / 100.0f, 0.01f);
        sizeRange_ = std::max(f[5] / 100.0f, 0.0f);
        alpha_ = std::max(f[6], 0.0f);
        alphaRange_ = std::max(f[7], 0.0f);
        fadeIn_ = std::max(static_cast<int32_t>(f[8]), 0);
        hold_ = std::max(static_cast<int32_t>(f[9]), 0);
        fadeOut_ = std::max(static_cast<int32_t>(f[10]), 0);
        line_.add({f[11], f[12], 0});
        line_.add({f[13], f[14], 0});
        line_.finish(100.0f, 1.0f);
        // The legs' base direction: the line turned a quarter turn, then made unit length.
        std::array<float, 3> d{f[11] - f[13], f[12] - f[14], 0};
        if (static_cast<float>(std::sqrt(static_cast<double>(d[1] * d[1] + d[0] * d[0] + d[2] * d[2]))) == 0.0f) d = {0, -1, 0};
        normal_ = turn(d, 3.14159265f / 2.0f);
        const float l = static_cast<float>(std::sqrt(static_cast<double>(normal_[0] * normal_[0] + normal_[1] * normal_[1] + normal_[2] * normal_[2])));
        if (0.0f < l) normal_ = {normal_[0] / l, normal_[1] / l, normal_[2] / l};
        rate_ = std::clamp(61 - static_cast<int32_t>(f[15]), 1, 60);
        perEmission_ = std::max(static_cast<int32_t>(f[16]), 1);
        state_.resize(records_.size());
        if (!records_.empty()) {
            active_ = 1;
            for (size_t i = 0; i < records_.size(); ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        if (rng_.next() % static_cast<uint32_t>(rate_) == 0) active_ += static_cast<size_t>(perEmission_);
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter);
            if (s.st == 0) {
                put(r, s, mapRange(0, s.alpha, 0, static_cast<float>(fadeIn_), t));
                if (fadeIn_ <= s.counter) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                put(r, s, s.alpha);
                if (hold_ <= s.counter - s.start) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                put(r, s, mapRange(s.alpha, 0, 0, static_cast<float>(fadeOut_), static_cast<float>(s.counter - s.start)));
                if (fadeOut_ < s.counter - s.start) s.st = 3, s.start = s.counter;
            }
            addMove(r, planeMove());  // every particle out, after its update (0x10002d81)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float alpha = 0;
        SplinePath path;
    };

    // The plugin's z turn (0x100016b0: the angle to three decimals, the CRT's cos / sin) of a
    // direction, as a row vector.
    static std::array<float, 3> turn(const std::array<float, 3>& v, float angle) {
        Mat m = rotationZ(plugCos(plugAngle(angle)), plugSin(plugAngle(angle)));
        return transform(v, m);
    }

    void put(ParticleRecord& r, const State& s, float alpha) {
        setAlpha(r, static_cast<int32_t>(alpha));
        const auto at = s.path.at(static_cast<float>(s.counter));
        r.pos = {at[0], at[1] - static_cast<float>(s.counter) * 0.1f, at[2]};
    }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};
        const uint32_t q = rng_.next() & 3;
        setUv(r, kQuarter[q][0], kQuarter[q][1], 0.5f, 0.5f);
        const float sc = rng_.unit() * sizeRange_ - sizeRange_ * 0.5f + size_;
        r.scale = {sc, sc, 1};
        setAlpha(r, 0);
        const float u = rng_.unit() * 100.0f;
        std::array<float, 3> p = line_.at(u);
        s.path = SplinePath{};
        s.path.add(p);
        r.pos = p;
        const float a = rng_.unit() * 3.14159265f - 3.14159265f / 2.0f + dir_;
        const float d = rng_.unit() * distRange_ + distRange_ * 0.5f + dist_;
        auto leg = turn(normal_, a);
        p = {leg[0] * d + p[0], leg[1] * d + p[1], leg[2] * d + p[2]};
        s.path.add(p);
        for (int32_t k = 10; k > 7; --k) {
            if (rng_.next() % 5 == 0) continue;
            const float a2 = rng_.unit() * spread_ - spread_ * 0.5f + dir_;
            const float base = rng_.unit() * distRange_ + distRange_ * 0.5f + dist_;
            const float d2 = base * static_cast<float>(static_cast<double>(k) * 0.09);  // x87Note
            leg = turn(normal_, a2);
            p = {p[0] + leg[0] * d2, p[1] + leg[1] * d2, p[2] + leg[2] * d2};
            s.path.add(p);
        }
        const float life = static_cast<float>(fadeIn_ + hold_ + fadeOut_);
        s.path.finish(life, life * 0.4f);
        s.alpha = rng_.unit() * alphaRange_ - alphaRange_ * 0.5f + alpha_;
        s.counter = s.start = s.st = 0;
    }

    std::vector<State> state_;
    SplinePath line_;
    std::array<float, 3> normal_{};
    size_t active_ = 0;
    float dist_ = 10, distRange_ = 0, dir_ = 0, spread_ = 0, size_ = 1, sizeRange_ = 0, alpha_ = 0, alphaRange_ = 0;
    int32_t fadeIn_ = 0, hold_ = 0, fadeOut_ = 0, rate_ = 1, perEmission_ = 1;
};

// kirakira.ppi (Happiness 2, 253 uses) and warukira.ppi (92): sparkles drifting out from a point,
// twinkling while they fade in and out. Init 0x10002040, Update 0x10002180, spawn 0x100028b0
// (warukira's 0x10002590). One is out at first; each update one more joins when a random number
// divides by 2 (warukira: 5).
//   f0 f1: emitter; f2: size (percent, at least 0.001); f3: distance (each r + f3 * r);
//   f4: half the spread (degrees); f5 f6 f7: frames before, fading in, fading out.
// A sparkle's path (cs2ParticleBaseEx's, tangents 1 long, over f5 + f6 + f7 frames) runs from the
// emitter straight out to its distance, to the right or the left within the spread, its direction
// worked out as asease's drops. Its clock starts at f5, so it appears that far along the path and
// eases to its end while it fades in and out. While fading it twinkles: its x scale is its size
// plus tri(t, 30) / 20 and its y scale its size minus tri / 40 (warukira: / 5 and / 10; tri is the
// host's triangle wave, +0x78), t the clock.
// kirakira's base (0x10001120) pairs the records: the first half are sparkles (the texture's
// bottom-left quarter, .data 0x100160b8), each with a glow in the second half (the top-right
// quarter, 0x100160c0) at its place and scale, 1 nearer, at its alpha while it fades in and half
// that after. Its sizes are 1-101% of f2, and an odd random number sends it left. warukira's
// records are all sparkles (the whole texture), 50-100% of f2, every other one to the left.
// kirakira's Init sets +0x10, the layer's blend, to additive (as em_shy's and concent's do);
// warukira's leaves it at 0.
class Kirakira : public ParticleEffect {
public:
    Kirakira(const ParticleParams& p, uint32_t seed, bool glow) : ParticleEffect(p, seed), glow_(glow) {
        const auto& f = p.f;
        if (glow) blend_ = 1;  // kirakira's Init: additive; warukira's leaves +0x10 at 0
        emitter_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        dist_ = f[3];
        spread_ = f[4];
        before_ = std::max(static_cast<int32_t>(f[5]), 0);
        fadeIn_ = std::max(static_cast<int32_t>(f[6]), 0);
        fadeOut_ = std::max(static_cast<int32_t>(f[7]), 0);
        groups_ = glow_ ? records_.size() / 2 : records_.size();
        state_.resize(groups_);
        active_ = 1;
        for (size_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (groups_ == 0) return;
        if (rng_.next() % (glow_ ? 2u : 5u) == 0) ++active_;
        active_ = std::min(active_, groups_);
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st <= 2) r.pos = s.path.at(static_cast<float>(s.counter));
            if (s.st == 0) {
                if (before_ < s.counter) s.st = 1, s.start = s.counter;
            } else if (s.st == 1 || s.st == 2) {
                twinkle(r, s);
                const int32_t t = s.counter - s.start, len = s.st == 1 ? fadeIn_ : fadeOut_;
                const float a = s.st == 1 ? mapRange(0, 255, 0, static_cast<float>(len), static_cast<float>(t))
                                          : mapRange(255, 0, 0, static_cast<float>(len), static_cast<float>(t));
                setAlpha(r, static_cast<int32_t>(a));
                if (len < t) ++s.st, s.start = s.counter;
            } else {
                spawn(i);
            }
            if (glow_) {
                ParticleRecord& g = records_[groups_ + i];
                if (!s.glowSet) {
                    g.pivot = r.pivot;
                    setAlpha(g, 0);
                    setUv(g, 0.5f, 0.0f, 0.5f, 0.5f);
                    s.glowSet = true;
                }
                g.pos[0] = r.pos[0], g.pos[1] = r.pos[1];
                g.scale = r.scale;
                setAlpha(g, s.st == 1 ? r.v[3].alpha : r.v[0].alpha >> 1);
                addMove(g, planeMove());  // the glow (kirakira 0x10002812)
            }
            addMove(r, planeMove());  // then the sparkle (0x10002854; warukira 0x10002525)
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float size = 0;
        bool glowSet = false;
        SplinePath path;
    };

    void twinkle(ParticleRecord& r, const State& s) const {
        const double tri = plugTriangle(static_cast<float>(s.counter), 30.0f);
        r.scale = {static_cast<float>(tri / (glow_ ? 20.0 : 5.0) + s.size),
                   static_cast<float>(s.size - tri / (glow_ ? 40.0 : 10.0)), 1};
    }

    static float hostPow(float x, float y) { return static_cast<float>(std::pow(static_cast<double>(x), static_cast<double>(y))); }
    static float hostSqrt(float x) { return static_cast<float>(std::sqrt(static_cast<double>(x))); }

    void spawn(size_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        if (glow_) setUv(r, 0.0f, 0.5f, 0.5f, 0.5f);
        r.pos = emitter_;
        const float u = rng_.unit();
        s.size = glow_ ? u * size_ + size_ * 0.01f  // x87Note
                       : u * size_ * 0.5f + size_ * 0.5f;
        r.scale = {s.size, s.size, 1};
        setAlpha(r, 0);
        const float sign = glow_ ? ((rng_.next() & 1) != 0 ? -1.0f : 1.0f) : (i % 2 == 0 ? 1.0f : -1.0f);
        const float a1 = plugRad(-spread_), a2 = plugRad(spread_);
        const float x1 = sign * plugCos(a1), y1 = sign * plugSin(a1);
        const float x2 = sign * plugCos(a2), y2 = sign * plugSin(a2);
        const float dx = hostSqrt(hostPow(x2 - x1, 2.0f));
        const float xo = rng_.unit() * dx - dx * 0.5f + sign;
        const float dy = hostSqrt(hostPow(y2 - y1, 2.0f));
        const float yo = rng_.unit() * dy - dy * 0.5f + 0.0f;
        std::array<float, 3> d{xo, yo, 0};
        const float len = static_cast<float>(std::sqrt(static_cast<double>(yo * yo + xo * xo + 0.0f)));
        if (0.0f < len) d = {xo / len, yo / len, 0.0f / len};
        const float rr = rng_.unit();
        const float dist = static_cast<float>(static_cast<long double>(rr) + static_cast<long double>(dist_) * rr);
        s.path = SplinePath{};
        s.path.add(emitter_);
        s.path.add({emitter_[0] + d[0] * dist, emitter_[1] + d[1] * dist, d[2] * dist + emitter_[2]});
        const float life = static_cast<float>(before_ + fadeIn_ + fadeOut_);
        s.path.finish(life, 1.0f);
        if (glow_) {
            ParticleRecord& g = records_[groups_ + i];
            g.pos = {r.pos[0], r.pos[1], r.pos[2] + 1.0f};
            setAlpha(g, 0);
            s.glowSet = false;
        }
        s.counter = before_, s.start = 0, s.st = 0;
    }

    bool glow_ = true;
    std::vector<State> state_;
    std::array<float, 3> emitter_{};
    size_t groups_ = 0, active_ = 0;
    float size_ = 1, dist_ = 0, spread_ = 0;
    int32_t before_ = 0, fadeIn_ = 0, fadeOut_ = 0;
};

// A piece of a trail for a head's step from prev to r.pos, as concent's update (0x1000221d-
// 0x100024b4) and radiate's (0x10002398-0x1000262b) both place it.
void placeStreak(const ParticleRecord& r, const std::array<float, 3>& prev, ParticleRecord& c) {
    const float dx = r.pos[0] - prev[0], dy = r.pos[1] - prev[1], dz = r.pos[2] - prev[2];
    const float len = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));  // host +0x44
    if (len == 0.0f) {
        c.pos = r.pos;
    } else {
        // Host +0x70 (0x55fad0 -> D3DXVec4Normalize of (d, 0)).
        const float n = std::sqrt(dx * dx + dy * dy + dz * dz + 0.0f * 0.0f);
        const float ny = dy / n, nx = dx / n, nz = dz / n;
        float turn = plugAcos(nx * 0.0f + ny + nz * 0.0f);  // host +0x28
        if (dy * 0.0f + dx + dz * 0.0f > 0.0f) turn *= -1.0f;
        c.rot[2] = turn;
        // Host +0x6c (0x55fa70 -> 0x583960): the distance, its squares in float.
        const float ey = r.pos[1] - prev[1], ex = r.pos[0] - prev[0], ez = r.pos[2] - prev[2];
        const float dist = static_cast<float>(std::sqrt(static_cast<double>(ey * ey + ex * ex + ez * ez)));
        c.pos = {(prev[0] + r.pos[0]) * 0.5f, (prev[1] + r.pos[1]) * 0.5f, (prev[2] + r.pos[2]) * 0.5f};
        c.pivot = r.pivot;
        c.scale = r.scale;
        c.scale[1] = dist * 0.7f * c.scale[1];
        return;
    }
    c.pivot = r.pivot;
    c.scale = r.scale;
}

// concent.ppi (Happiness 2, 44 uses): speed lines rushing in on a point (a manga's 集中線). Init
// 0x10001fd0, Update 0x10002090, spawn 0x10002660. The base (0x10001120) makes count / f0 groups,
// each a head (records [0, groups)) and f0 - 1 pieces of its trail (the records after the heads,
// group by group). It does not read the plane fields: the lines stay put when their plane moves.
//   f0: records per group; f1: frames along the path; f2: size (thousandths of the texture, each
//   80-100% of it); f3 f4: the point; f5 f6: the box the heads start in, centred on the point.
// A head starts at a random whole point of the box (a random integer modulo the box's width, less
// half of it, then the same for y), 500 nearer, and runs a two-point path (cs2ParticleBaseEx's,
// tangents 100 long, over f1 frames) to the point at that depth; after 60-80% of f1 frames it
// starts again. The head itself stays clear. Each update while it runs, the first free piece of its
// trail goes halfway between where the head was and where it is, turned along that step about z
// (the acos of its unit vector's y, negated while it heads right; the host's normalize is
// D3DXVec4Normalize, here x / its length) and stretched along it to 0.7 of the step's length times
// the head's scale; the piece fades from 255 to 0 over f0 - 1 frames and is free again. A step of
// 0 (a head's first update) leaves a piece at the head at the head's scale, keeping its turn.
// One group runs at first; each update ten more join when a random number divides by 10, up to
// all of them. That number is a global of the DLL (0x100150ac, 1 in the file; nothing else
// writes it), so it starts afresh with each load of the DLL and layers running concent at once
// share it (PluginGlobals). The pieces keep the record defaults' texture coordinates (the whole
// texture).
class Concent : public ParticleEffect {
public:
    struct Globals : PluginGlobals {
        uint32_t active = 1;
    };

    Concent(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        blend_ = 1;  // Init 0x10002025: additive
        const auto& f = p.f;
        // The group size as the base takes it (CVTTSS2SI, then unsigned).
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        move_ = f[1];
        size_ = f[2] / 1000.0f;
        point_ = {f[3], f[4]};
        box_ = {f[5], f[6]};
        heads_.resize(groups_);
        pieces_.resize(static_cast<size_t>(groups_) * trail_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (rng_.next() % 10 == 0) dll_->active += 10;
        dll_->active = std::min(dll_->active, groups_);
        for (uint32_t i = 0; i < dll_->active; ++i) {
            Head& h = heads_[i];
            ParticleRecord& r = records_[i];
            if (h.st == 0) {
                r.pos = h.path.at(static_cast<float>(h.counter));
                if (static_cast<float>(h.counter) > h.life) h.start = h.counter, h.st = 2;
            } else if (h.st == 1) {  // not reached: the DLL goes from 0 to 2
                r.pos = h.path.at(static_cast<float>(h.counter));
                if (static_cast<float>(h.counter) > move_) h.start = h.counter, h.st = 2;
            } else {
                spawn(i);
            }
            if (h.st == 0) {
                for (uint32_t k = 0; k < trail_; ++k) {
                    Piece& pc = pieces_[i * trail_ + k];
                    if (pc.on) continue;
                    place(r, h, records_[groups_ + i * trail_ + k]);
                    pc.on = true;
                    break;
                }
            }
            for (uint32_t k = 0; k < trail_; ++k) {
                Piece& pc = pieces_[i * trail_ + k];
                if (!pc.on) continue;
                ParticleRecord& c = records_[groups_ + i * trail_ + k];
                setAlpha(c, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(trail_), static_cast<float>(pc.counter))));
                pc.counter += frames;
                if (static_cast<uint32_t>(pc.counter) >= trail_) pc.counter = 0, pc.on = false, setAlpha(c, 0);
            }
            h.prev = r.pos;
            h.counter += frames;
        }
    }

private:
    struct Head {
        int32_t counter = 0, start = 0, st = 0;
        SplinePath path;
        std::array<float, 3> prev{};
        float life = 0;
    };
    struct Piece {
        int32_t counter = 0;
        bool on = false;
    };

    static void place(const ParticleRecord& r, const Head& h, ParticleRecord& c) { placeStreak(r, h.prev, c); }

    // Spawn 0x10002660: random numbers in the order the DLL takes them (scale, x, y, life).
    void spawn(uint32_t i) {
        ParticleRecord& r = records_[i];
        Head& h = heads_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const float sc = rng_.unit() * size_ * 0.2f + size_ * 0.8f;
        r.scale = {sc, sc, 1};
        setAlpha(r, 0);
        r.pos[0] = boxPoint(box_[0]) - box_[0] * 0.5f + point_[0];
        r.pos[1] = boxPoint(box_[1]) - box_[1] * 0.5f + point_[1];
        r.pos[2] = 500;
        h.counter = 0, h.st = 0;
        h.prev = r.pos;
        h.path = SplinePath{};
        h.path.add(r.pos);
        h.path.add({point_[0], point_[1], 500.0f});
        h.path.finish(move_, 100.0f);
        const float u = rng_.unit();
        h.life = u * static_cast<float>(static_cast<double>(move_) * 0.2) + move_ * 0.6f;  // x87Note
        for (uint32_t k = 0; k < trail_; ++k) {
            pieces_[i * trail_ + k] = Piece{};
            ParticleRecord& c = records_[groups_ + i * trail_ + k];
            c.pos = r.pos;
            setAlpha(c, 0);
        }
    }

    // A random integer modulo the box's size (CVTTSS2SI, unsigned DIV), as a float. The DLL would
    // fault on a size under 1; none of the scenes gives one.
    float boxPoint(float size) {
        const uint32_t d = static_cast<uint32_t>(static_cast<int32_t>(size));
        const uint32_t v = rng_.next();
        return static_cast<float>(static_cast<double>(d == 0 ? 0 : v % d));
    }

    Globals* dll_ = nullptr;
    std::vector<Head> heads_;
    std::vector<Piece> pieces_;
    uint32_t groups_ = 0, trail_ = 0;
    float move_ = 0, size_ = 0;
    std::array<float, 2> point_{}, box_{};
};

// digi.ppi (Happiness 2, 27 uses, with digiring in the costume changes): squares of light thrown
// off an emitter that sweeps across the screen. Init 0x100013e0, Update 0x10001550, spawn
// 0x100017d0. Every record is a particle of its own: Init passes the base (0x10001120) ECX as the
// group size, which the CRT's memset just before leaves at 0 on its `rep stosb` path (the one
// taken on CPUs with fast strings; its SSE path would leave the buffer's end, and no groups).
//   f0 f1 -> f2 f3: the emitter's sweep, over f4 frames (the host's linear map of the layer
//   clock); f5 f6: speed and its spread (hundredths per frame); f7 f8: size and its spread
//   (thousandths of the texture); f9 f10: how far from the emitter a particle starts in x / y;
//   f11: life (frames, 0-2x); f12: alpha (0-2x, held to 255); f13: additive (non-zero).
// A particle takes one of the texture's quarters (a random number modulo 10: 0 the top-left, 1 the
// top-right, 2 the bottom-right, the rest the bottom-left), a colour of 225-255 in each channel,
// its size, a place round the emitter, a direction (x and y each 0-1 with a random sign, made unit
// length) times its speed, its alpha and its life. Each update it moves by its velocity and fades
// from its alpha to 0 over its life, then waits; it starts again only while the emitter still
// sweeps. Spreads are (u * 2 - 1) * spread + base.
class Digi : public ParticleEffect {
public:
    Digi(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        from_ = {f[0], f[1]};
        emitter_ = from_;
        to_ = {f[2], f[3]};
        sweep_ = static_cast<int32_t>(f[4]);
        speed_ = f[5] / 100.0f, speedRange_ = f[6] / 100.0f;
        size_ = f[7] / 1000.0f, sizeRange_ = f[8] / 1000.0f;
        spread_ = {f[9], f[10]};
        life_ = static_cast<int32_t>(f[11]);
        alpha_ = static_cast<int32_t>(f[12]);
        if (f[13] != 0.0f) blend_ = 1;  // additive
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const float fr = static_cast<float>(frames);
        emitter_[0] = mapRange(from_[0], to_[0], 0, static_cast<float>(sweep_), clock_);
        emitter_[1] = mapRange(from_[1], to_[1], 0, static_cast<float>(sweep_), clock_);
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st != 0) {
                if (static_cast<float>(sweep_) > clock_) spawn(i);
            } else {
                s.pos[1] = s.vel[1] + s.pos[1];
                s.pos[0] = s.vel[0] + s.pos[0];
                r.pos[0] = s.pos[0], r.pos[1] = s.pos[1];
                const int32_t age = s.counter - s.start;
                setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(s.alpha), 0, 0, s.life, static_cast<float>(age))));
                if (static_cast<float>(age) > s.life) s.st = 3, s.start = s.counter;
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x1000174b)
            s.counter += static_cast<int32_t>(fr);
        }
        clock_ += fr;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 2> pos{}, vel{};
        int32_t alpha = 0;
        float life = 0;
    };

    float spread(float range, float base) { return (rng_.unit() * 2.0f - 1.0f) * range + base; }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        // .data 0x100150b0: the quarters' corners.
        static const float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};
        const uint32_t q = rng_.next() % 10;
        const int k = q == 0 ? 0 : q == 1 ? 1 : q == 2 ? 3 : 2;
        setUv(r, kQuarter[k][0], kQuarter[k][1], 0.5f, 0.5f);
        const float sc = spread(sizeRange_, size_);
        r.scale = {sc, sc, 1};
        // Bytes 0-2 of the D3DCOLOR (blue, green, red), each 225 + a random number modulo 31.
        const uint8_t b = static_cast<uint8_t>(rng_.next() % 31 + 225);
        const uint8_t g = static_cast<uint8_t>(rng_.next() % 31 + 225);
        const uint8_t red = static_cast<uint8_t>(rng_.next() % 31 + 225);
        for (auto& v : r.v) v.r = red, v.g = g, v.b = b, v.alpha = 0;
        s.pos[0] = spread(spread_[0], emitter_[0]);
        s.pos[1] = spread(spread_[1], emitter_[1]);
        rng_.unit();  // * 5, kept but not used
        float vx = rng_.unit();
        if ((rng_.next() & 1) == 0) vx *= -1.0f;
        float vy = rng_.unit();
        if ((rng_.next() & 1) == 0) vy *= -1.0f;
        const float vz = 0;
        const float len = static_cast<float>(std::sqrt(static_cast<double>(vx * vx + vy * vy + vz * vz)));
        if (0.0f < len) vx = vx / len, vy = vy / len;
        const float sp = spread(speedRange_, speed_);
        s.vel = {vx * sp, vy * sp};
        const float a = spread(static_cast<float>(alpha_), static_cast<float>(alpha_));
        s.alpha = static_cast<int32_t>(0.0f <= a ? (a < 255.0f ? a : 255.0f) : 0.0f);
        s.counter = s.start = s.st = 0;
        s.life = spread(static_cast<float>(life_), static_cast<float>(life_));
    }

    std::vector<State> state_;
    std::array<float, 2> from_{}, to_{}, emitter_{}, spread_{};
    int32_t sweep_ = 0, life_ = 0, alpha_ = 0;
    float speed_ = 0, speedRange_ = 0, size_ = 0, sizeRange_ = 0, clock_ = 0;
};

// digiring.ppi (Happiness 2, 27 uses, with digi): squares of light circling the same sweeping
// emitter in a ring seen from the side. Init 0x100013e0, Update 0x10001560, spawn 0x100018e0;
// every record is a particle of its own (the group size as digi's).
//   f0 f1 -> f2 f3 over f4 frames: the sweep, as digi's; f7 f8: size and its spread
//   (thousandths); f10: the ring's height (each 0-2x); f11: its radius (each +-50); f12: life
//   (frames, 0-2x); f13: alpha (0-2x, held to 255); f14: additive (non-zero); f5 f6 f9 are
//   read but not used.
// A particle takes a quarter and a colour as digi's, a size, a start angle (a random number of
// degrees to 360, through the host's radians, kept to thousandths of a radian), a turn per frame
// of 0-0.1 radian either way, its radius and height, its alpha and life. Each update it goes on
// round: x = the emitter's x - sin(angle) * radius, z = cos(angle) * radius (so the depth makes
// the near side larger), y = the emitter's y plus its height; it is turned about y to face the
// ring's centre (the acos of the unit direction's -z, negated when that direction's -x is under
// 0) and fades from its alpha to 0 over its life; it starts again only while the emitter sweeps.
// The x87 parts (the speed, the start angle, x) are float steps (x87Note).
class DigiRing : public ParticleEffect {
public:
    DigiRing(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        from_ = {f[0], f[1]};
        emitter_ = from_;
        to_ = {f[2], f[3]};
        sweep_ = static_cast<int32_t>(f[4]);
        size_ = f[7] / 1000.0f, sizeRange_ = f[8] / 1000.0f;
        height_ = f[10];
        radius_ = f[11];
        life_ = static_cast<int32_t>(f[12]);
        alpha_ = static_cast<int32_t>(f[13]);
        if (static_cast<int32_t>(f[14]) != 0) blend_ = 1;  // additive
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const float fr = static_cast<float>(frames);
        emitter_[0] = mapRange(from_[0], to_[0], 0, static_cast<float>(sweep_), clock_);
        emitter_[1] = mapRange(from_[1], to_[1], 0, static_cast<float>(sweep_), clock_);
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.st != 0) {
                if (static_cast<float>(sweep_) > clock_) spawn(i);
            } else {
                const int32_t age = s.counter - s.start;
                s.angle = s.speed + s.angle;
                const float sn = plugSin(s.angle);  // host +0x18
                r.pos[0] = sn * -s.radius + emitter_[0];
                r.pos[1] = s.height + emitter_[1];
                r.pos[2] = plugCos(s.angle) * s.radius;  // host +0x1c
                // Host +0x70 (D3DXVec4Normalize) of the way to the centre (the emitter's z is 0).
                const float dx = emitter_[0] - r.pos[0], dy = 0.0f, dz = 0.0f - r.pos[2];
                const float n = std::sqrt(dx * dx + dy * dy + dz * dz + 0.0f * 0.0f);
                const float nx = dx / n, ny = dy / n, nz = dz / n;
                float turn = plugAcos(ny * 0.0f + nx * 0.0f - nz * 1.0f);  // host +0x28
                if (0.0f > ny * 0.0f - nx * 1.0f + nz * 0.0f) turn *= -1.0f;
                r.rot[1] = turn;
                setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(s.alpha), 0, 0, static_cast<float>(s.life), static_cast<float>(age))));
                if (age > s.life) s.st = 3, s.start = s.counter;
            }
            addMove(r, planeMove());  // every record, where the branches meet (0x10001852)
            s.counter += static_cast<int32_t>(fr);
        }
        clock_ += fr;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        float height = 0, radius = 0, angle = 0, speed = 0;
        int32_t alpha = 0, life = 0;
    };

    float spread(float range, float base) { return (rng_.unit() * 2.0f - 1.0f) * range + base; }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        static const float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x100140b0
        const uint32_t q = rng_.next() % 10;
        const int k = q == 0 ? 0 : q == 1 ? 1 : q == 2 ? 3 : 2;
        setUv(r, kQuarter[k][0], kQuarter[k][1], 0.5f, 0.5f);
        const float sc = spread(sizeRange_, size_);
        r.scale = {sc, sc, 1};
        const uint8_t b = static_cast<uint8_t>(rng_.next() % 31 + 225);
        const uint8_t g = static_cast<uint8_t>(rng_.next() % 31 + 225);
        const uint8_t red = static_cast<uint8_t>(rng_.next() % 31 + 225);
        for (auto& v : r.v) v.r = red, v.g = g, v.b = b, v.alpha = 0;
        // The start angle: degrees (x87, stored as a float), the host's radians (+0x10), times
        // 1000 kept whole.
        const float deg = rng_.unit() * 360.0f;
        const float milli = plugRad(deg) * 1000.0f;
        s.angle = static_cast<float>(static_cast<int32_t>(milli)) / 1000.0f;
        const float t = rng_.unit() * 2.0f - 1.0f;
        s.speed = rng_.unit() * 0.1f * t;
        s.radius = (rng_.unit() * 2.0f - 1.0f) * 50.0f + radius_;
        s.height = spread(height_, height_);
        const float a = spread(static_cast<float>(alpha_), static_cast<float>(alpha_));
        s.alpha = static_cast<int32_t>(0.0f > a ? 0.0f : (255.0f > a ? a : 255.0f));
        s.counter = s.start = s.st = 0;
        s.life = static_cast<int32_t>(spread(static_cast<float>(life_), static_cast<float>(life_)));
    }

    std::vector<State> state_;
    std::array<float, 2> from_{}, to_{}, emitter_{};
    int32_t sweep_ = 0, life_ = 0, alpha_ = 0;
    float size_ = 0, sizeRange_ = 0, height_ = 0, radius_ = 0, clock_ = 0;
};

// dark_tri.ppi (Happiness 2, 26 uses, two layers at once): a mass of darkness, smoke puffs drifting
// off three points. Init 0x10001500, Update 0x100016c0 -> 0x10001780, spawn 0x10001cc0 (0x10002030, a second
// spawn, is for records past the active limit, which Init sets to the count, so it never runs).
// Every record is a particle of its own: this DLL's base (0x10001120) gives no trails.
//   f0 f1, f2 f3, f4 f5: the three emitters (particle i uses emitter i % 3); f6: fall per frame
//   per frame (thousandths; added to the y speed each update); f7: alpha at full; f8: size
//   (percent of the texture); f9: additive (non-zero).
// Update takes the floats the engine passes each time (the layer's, which `particle3 <n> set`
// replaces, setFloats()): when f8 there differs from the one it holds, it takes the new size and
// the six emitter numbers.
// One particle more moves every fourth update from the fifth (the first at once; the count
// +0x90 goes back to 0 and up by one in the same update). A particle takes one of the
// texture's quarters (a random number & 3; .data 0x100160b0), an angle about z (0-2 pi) and a turn
// per update (+-0.01), a direction (x and y each the product of two random numbers, one made
// -1..1, then unit length) times 0.75-0.775, and three spans: fading in to f7 over 15-45 updates,
// holding 30, fading out over 40-80. Each update it moves off its emitter by its speed, turns, and
// grows from the size to twice it over the three spans; after them it starts again from its
// emitter. The x87 parts (the angle, the speed's factor) are float steps (x87Note).
// wc_dark.ppi (Happiness 2, 2 loads: a pink mist) is an older build of the same (plugin
// version 0x20001; Init 0x100013c0, Update 0x10001530 -> 0x100015b0, spawn 0x10001a50, the same
// steps and constants) with one emitter: f0 f1 the emitter, f2 the fall, f3 the alpha, f4 the
// size, f5 additive. It adds the plane's move (+0x24 / +0x28 less half the screen, +0x90 / +0x94)
// rather than the plane's position, and takes no new floats.
class DarkTri : public ParticleEffect {
public:
    DarkTri(const ParticleParams& p, uint32_t seed, bool wcDark = false) : ParticleEffect(p, seed), wcDark_(wcDark) {
        const auto& f = p.f;
        const int b = wcDark ? 2 : 6;  // the first float after the emitters
        for (int k = 0; k < 3; ++k) emitter_[k] = wcDark ? std::array<float, 2>{f[0], f[1]} : std::array<float, 2>{f[k * 2], f[k * 2 + 1]};
        fall_ = f[b] / 1000.0f;
        alphaMax_ = static_cast<int32_t>(f[b + 1]);
        size_ = f[b + 2] / 100.0f;
        sizeKey_ = f[b + 2];
        if (f[b + 3] != 0.0f) blend_ = 1;  // additive
        state_.resize(records_.size());
        active_ = std::min<size_t>(1, records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const auto& f = params_.f;  // the floats of this update
        if (!wcDark_ && sizeKey_ != f[8]) {
            sizeKey_ = f[8];
            size_ = f[8] / 100.0f;
            for (int k = 0; k < 3; ++k) emitter_[k] = {f[k * 2], f[k * 2 + 1]};
        }
        if (tick_ > 3) {
            active_ = std::min(active_ + 1, records_.size());
            tick_ = 0;
        }
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const auto& e = emitter_[i % 3];
            const int32_t age = s.counter - s.start;
            auto move = [&] {
                s.off[0] = s.vel[0] + s.off[0];
                s.vel[1] = fall_ + s.vel[1];
                s.off[1] = s.vel[1] + s.off[1];
                r.pos[0] = e[0] + s.off[0];
                r.pos[1] = e[1] + s.off[1];
                r.pos[2] = 0;  // the emitters' z
            };
            auto grow = [&] {
                const float sc = mapRange(s.size, s.size * 2.0f, 0, static_cast<float>(s.total), static_cast<float>(s.counter));
                r.scale[0] = r.scale[1] = sc;
            };
            if (s.st == 0) {
                s.size = size_;
                move();
                grow();
                setAlpha(r, static_cast<int32_t>(mapRange(0, static_cast<float>(alphaMax_), 0, static_cast<float>(s.in), static_cast<float>(age))));
                r.rot[2] = s.turn + r.rot[2];
                if (age > s.in) s.st = 1, s.start = s.counter;
            } else if (s.st == 1) {
                move();
                r.rot[2] = s.turn + r.rot[2];
                grow();
                if (age > s.hold) s.st = 2, s.start = s.counter;
            } else if (s.st == 2) {
                move();
                grow();
                setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(alphaMax_), 0, 0, static_cast<float>(s.out), static_cast<float>(age))));
                r.rot[2] = s.turn + r.rot[2];
                if (age > s.out) s.st = 3, s.start = s.counter;
            } else {
                r.pos[0] = e[0], r.pos[1] = e[1];
                spawn(i);
            }
            // Plus the plane's position (+0x24 / +0x28, copied to +0xb0 / +0xb4 at the update's
            // start) as it is, not less the screen's centre: 0 on planes 30 and 31, which Happiness
            // 2's vram.xml leaves out, and on plane 1 after the scripts' `pl 1 pos 0 0`. wc_dark
            // adds the move instead.
            const std::array<float, 2> add = wcDark_ ? planeMove() : planePos_;
            r.pos[0] = r.pos[0] + add[0];
            r.pos[1] = r.pos[1] + add[1];
            s.counter += frames;
        }
        ++tick_;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, st = 0;
        std::array<float, 2> vel{}, off{};
        int32_t in = 0, hold = 0, out = 0, total = 0;
        float turn = 0, size = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        static const float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};
        const uint32_t q = rng_.next() & 3;
        setUv(r, kQuarter[q][0], kQuarter[q][1], 0.5f, 0.5f);
        s.off = {0, 0};
        r.scale = {size_, size_, 1};
        r.rot[2] = rng_.unit() * 6.2831854820251465f;  // 2 pi (float)
        s.turn = (rng_.unit() * 2.0f - 1.0f) * 0.01f;
        float a = rng_.unit() * 2.0f - 1.0f;
        float vx = rng_.unit() * a;
        a = rng_.unit() * 2.0f - 1.0f;
        float vy = rng_.unit() * a;
        float vz = 0;
        const float len = static_cast<float>(std::sqrt(static_cast<double>(vx * vx + vy * vy + vz * vz)));
        if (0.0f < len) vx = vx / len, vy = vy / len, vz = vz / len;
        const float k = rng_.unit() * 0.025f + 0.75f;
        s.vel = {vx * k, k * vy};
        s.st = s.counter = s.start = 0;
        s.in = static_cast<int32_t>((rng_.unit() * 2.0f - 1.0f) * 15.0f + 30.0f);
        s.hold = 30;
        s.out = static_cast<int32_t>((rng_.unit() * 2.0f - 1.0f) * 20.0f + 60.0f);
        s.total = s.in + s.out + s.hold;
        // The D3DCOLOR 0x00ffffff: white, alpha 0.
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;
    }

    std::vector<State> state_;
    bool wcDark_ = false;
    std::array<std::array<float, 2>, 3> emitter_{};
    float fall_ = 0, size_ = 0, sizeKey_ = 0;
    int32_t alphaMax_ = 0;
    size_t active_ = 0;
    int32_t tick_ = 0;
};

// filmnoise.ppi (Happiness 2; wc_filmnoise.ppi and Nekopara 3's filmnoise.ppi are the same file):
// an old film's grain, a scratch and dust. Init 0x10001470, Update 0x10001520; it reads none of
// the script's floats nor the plane fields. The texture (tex_filmnoise, 1024 x 1024) holds two
// grain patches, a scratch and a speck; the records keep the engine's texture-sized quads, about
// their centre (the canvas x 0.5).
//   Records 0 and 1 (spawn 0x10001840, update 0x100015e0): the grain patches (.data 0x100150b0 /
//   0x100150b8: 0.625 x 0.3125 of the texture at (0, 0) / (0, 0.5)) over the screen, at (640,
//   360, 20) scaled 1.25 x 0.8, alpha 5. Each holds a phase for more than one frame of its clock
//   and then turns to the other, drawn in one of them (record 0 when the phase is 0, record 1
//   when it is 1; record 0 starts in phase 1, record 1 in 0, so both show and hide together).
//   Record 2 (0x10001c20 / 0x10001780): the scratch (0.25 x 0.5 at (0.75, 0.5)) at a random x,
//   y 200, z 10, x scale 0.1-0.2, each corner a random alpha (a random number's low byte), shown
//   for more than 1-5 frames, then put somewhere else in the next update.
//   Records 3 on (0x10001a60 / 0x100016d0): specks (0.25 x 0.25 at (0.75, 0)) anywhere on the
//   screen, z = the record's number less 2, turned 0-pi, scaled 0.005-0.015, each corner's alpha
//   a random number % 200; hidden as they start, shown from the next update for more than 0-7
//   frames (random & 7), then started again.
// Init starts the first three; each update first adds one more when a random number % 30 is 0
// (up to the count) and moves only the started ones, so the specks join one by one. Each record
// counts the layer clock (+= the update's frames). The x87 parts (x, y, the turn, the specks'
// scale) are float steps (x87Note); the scratch's x scale adds a double 0.1.
class FilmNoise : public ParticleEffect {
public:
    FilmNoise(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (rng_.next() % 30 == 0) ++active_;
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (i < 2) {
                if (s.phase == 0 || s.phase == 1) {
                    r.draw = (s.phase == 0) == (s.which == 0) ? 1 : 0;
                    if (s.counter - s.start > 1) s.phase ^= 1, s.start = s.counter;
                } else {
                    spawn(i);
                }
            } else if (s.phase == 0) {
                if (i > 2) r.draw = 1;
                if (static_cast<float>(s.counter - s.start) > s.span) s.phase = 1, s.start = s.counter;
            } else {
                spawn(i);
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0, which = 0;
        float span = 0;
    };

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        if (i < 2) {
            setUv(r, 0.0f, i == 0 ? 0.0f : 0.5f, 0.625f, 0.3125f);
            r.pos = {640, 360, 20};
            r.scale = {1.25f, 0.8f, 0};
            for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 5;  // 0x05ffffff
            s.which = static_cast<int32_t>(i);
            s.phase = i == 0 ? 1 : 0;
            r.draw = 0;
            s.counter = 0;
            return;
        }
        if (i == 2) {
            setUv(r, 0.75f, 0.5f, 0.25f, 0.5f);
            r.scale = {1, 1, 1};
            r.pos[0] = rng_.unit() * 1280.0f;
            r.pos[1] = 200, r.pos[2] = 10;
            r.scale[0] = static_cast<float>(static_cast<double>(rng_.unit() * 0.1f) + 0.1);
            for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = static_cast<uint8_t>(rng_.next());
            s.span = static_cast<float>(rng_.next() % 5 + 1);
            r.draw = 1;
        } else {
            setUv(r, 0.75f, 0.0f, 0.25f, 0.25f);
            r.pos[0] = rng_.unit() * 1280.0f;
            r.pos[1] = rng_.unit() * 720.0f;
            r.pos[2] = static_cast<float>(static_cast<int32_t>(i) - 2);
            r.rot[2] = rng_.unit() * 3.1415927410125732f;
            r.scale[2] = 1;
            const float k = rng_.unit() * 0.01f + 0.005f;
            r.scale[0] = r.scale[1] = k;
            for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = static_cast<uint8_t>(rng_.next() % 200);
            s.span = static_cast<float>(rng_.next() & 7);
            r.draw = 0;
        }
        s.counter = s.start = s.phase = 0;
    }

    std::vector<State> state_;
    size_t active_ = 3;
};

// energy.ppi (Happiness 2, 22 loads): a burst of sparks streaming from one point to another, once.
// Init 0x10002120, Update 0x100022a0, spawn 0x100026c0; its path is its own copy of
// cs2ParticleBaseEx's (SplinePath, tangents finishMean). Additive (+0x10 = 1).
//   f0 f1: start; f2 f3: end; f4: size (percent; at least 0.001 as a scale); f5: its spread
//   (percent, either way; the same floor); f6: the bend's reach; f8 f9 f10: wait, fade-in and
//   fade-out frames (whole, at least 0). f7, f11 and f12 are kept and not used; the plane's move
//   is worked out each update (+0x7c / +0x80) and not used.
// Init sets every spark up at once: one of the texture's three quarters (rand % 3: .data
// 0x100150b0), the start, a path start -> a bend -> the end, the bend out from the start along a
// random direction (x and y each 2 rand - 1, made unit length) by reach x (1-2) in x and reach x
// (0.5-1.5) in y, run over wait + in + out frames. Each update a spark sits on its path at its
// clock, which starts at the wait; every corner takes a new colour (each of b, g, r 56 + rand %
// 200). It is unseen for the wait, fades in, then fades out shrinking by 0.997 an update, and stays
// out: nothing starts it again.
class Energy : public ParticleEffect {
public:
    Energy(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        from_ = {f[0], f[1], 0};
        to_ = {f[2], f[3], 0};
        size_ = f[4] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        spread_ = f[5] / 100.0f;
        if (spread_ < 0.001f) spread_ = 0.001f;
        reach_ = f[6];
        auto whole = [](float v) { return std::max(0, static_cast<int32_t>(v)); };
        wait_ = whole(f[8]), in_ = whole(f[9]), out_ = whole(f[10]);
        blend_ = 1;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.phase > 2) continue;
            const int32_t age = s.counter - s.start;
            r.pos = s.path.at(static_cast<float>(s.counter));
            if (s.phase == 0) {
                colour(r);
                if (s.counter > wait_) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(in_), static_cast<float>(age))));
                colour(r);
                if (age > in_) s.phase = 2, s.start = s.counter;
            } else {
                r.scale[0] = r.scale[0] * 0.997f;
                r.scale[1] = r.scale[1] * 0.997f;
                r.scale[2] = 1;
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(out_), static_cast<float>(age))));
                colour(r);
                if (age > out_) s.phase = 3, s.start = s.counter;
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        SplinePath path;
    };

    void colour(ParticleRecord& r) {
        for (auto& v : r.v) {
            v.b = static_cast<uint8_t>(rng_.next() % 200 + 0x38);
            v.g = static_cast<uint8_t>(rng_.next() % 200 + 0x38);
            v.r = static_cast<uint8_t>(rng_.next() % 200 + 0x38);
        }
    }

    void spawn(size_t i) {
        static constexpr float kQuarter[3][2] = {{0, 0}, {0, 0.5f}, {0.5f, 0}};
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const uint32_t q = rng_.next() % 3;
        setUv(r, kQuarter[q][0], kQuarter[q][1], 0.5f, 0.5f);
        r.pos = from_;
        s.path = SplinePath{};
        s.path.add(from_);
        const float sz = (rng_.unit() * 2.0f - 1.0f) * spread_ + size_;
        r.scale = {sz, sz, 1};
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;  // 0x00ffffff
        // The x87 parts (2 rand - 1) round once, as float arithmetic does here.
        const float a = rng_.unit() * 2.0f - 1.0f;
        const float b = rng_.unit() * 2.0f - 1.0f;
        float dx = a, dy = b, dz = 0;
        const float len = static_cast<float>(std::sqrt(static_cast<double>(b * b + a * a + 0.0f)));
        if (0.0f < len) dx = a / len, dz = 0.0f / len, dy = b / len;
        const float ox = (rng_.unit() * reach_ + reach_) * dx;
        const float oy = (rng_.unit() * reach_ + reach_ * 0.5f) * dy;
        s.path.add({from_[0] + ox, from_[1] + oy, dz + from_[2]});
        s.path.add(to_);
        s.path.finishMean(static_cast<float>(out_ + in_ + wait_));
        s.counter = wait_, s.start = 0, s.phase = 0;
    }

    std::vector<State> state_;
    std::array<float, 3> from_{}, to_{};
    float size_ = 0, spread_ = 0, reach_ = 0;
    int32_t wait_ = 0, in_ = 0, out_ = 0;
};

// steam.ppi (Happiness 2, 20 loads: sand and dust rising along the ground): puffs that start
// anywhere along a line and drift off it. Init 0x100021e0, Update 0x100026e0, spawn 0x10002c20,
// head-start spawn 0x10003320. Additive (+0x10 = 1). The groups' followers (cs2ParticleBaseEx) are
// never given any (0x10001120 sets every group's count to 0), so their code does nothing.
//   f0: step length (at least 10); f1: its spread (at least 0); f2: turn off the direction and f3
//   its spread (hundredths of a radian, each held to +-pi); f4: size (percent, at least 1) and f5
//   its spread (percent); f6: alpha and f7 its spread; f8 f9 f10: fade-in, hold, fade-out frames;
//   f11 f12 -> f13 f14: the line; f15: one start in 61 - f15 updates (1-60); f16: how many then (at
//   least 1); f17: start half of them part-way through.
// The line is a two-point path (tangents of length 1, run over 100); the direction is the line's
// start less its end turned a quarter (as the plugins turn: plugAngle, then the CRT's cos / sin),
// made unit length. A puff takes a quarter of the texture (rand & 3), a size (base + spread x
// (rand - 0.5)), a point on the line (at rand x 100), then a path from it: a first step and up to
// three more (each skipped one time in five), each along the direction turned by (turn + spread x
// (rand - 0.5)) and as long as (length + spread x (rand + 0.5)); run over fade-in + hold + fade-out,
// tangents 0.4 of that. Its alpha (alpha + spread x (rand - 0.5)) fades in by its clock, holds and
// fades out; then it starts again.
// wc_steam.ppi (Happiness 2, one load: a pink mist) is a later build of the same source
// (Init 0x10002770, Update 0x10002c90, spawn 0x100035d0, head-start spawn 0x10003cd0, which is
// steam's). It differs in three things: it starts alpha-blended, f18 not 0 making it additive; a
// puff's path takes energy's tangents (0x10001e50, SplinePath::finishMean, over the same frames);
// and f19 not 0 turns each puff to its way every update (face). Its line's finish (0x100019d0)
// also keeps a running sum and mean of the lengths, which only the finishes read.
class Steam : public ParticleEffect {
public:
    Steam(const ParticleParams& p, uint32_t seed, bool wc = false) : ParticleEffect(p, seed), wc_(wc) {
        const auto& f = p.f;
        if (!wc) blend_ = 1;
        constexpr float kPi = 3.14159274f;
        auto angle = [&](float v) {
            v = v / 100.0f;
            if (kPi < v) v = kPi;
            if (v < -kPi) v = -kPi;
            return v;
        };
        step_ = f[0] < 10.0f ? 10.0f : f[0];
        stepSpread_ = f[1] < 0.0f ? 0.0f : f[1];
        turn_ = angle(f[2]);
        turnSpread_ = angle(f[3]);
        size_ = f[4] / 100.0f;
        if (size_ < 0.01f) size_ = 0.01f;
        sizeSpread_ = f[5] / 100.0f;
        if (sizeSpread_ < 0.0f) sizeSpread_ = 0;
        alpha_ = f[6] < 0.0f ? 0.0f : f[6];
        alphaSpread_ = f[7] < 0.0f ? 0.0f : f[7];
        auto whole = [](float v) { return std::max(0, static_cast<int32_t>(v)); };
        in_ = whole(f[8]), hold_ = whole(f[9]), out_ = whole(f[10]);
        const std::array<float, 3> a{f[11], f[12], 0}, b{f[13], f[14], 0};
        line_.add(a);
        line_.add(b);
        line_.finish(100.0f, 1.0f);
        dir_ = turned(SplinePath::sub(a, b), 1.57079637f);
        const float len = static_cast<float>(std::sqrt(static_cast<double>(dir_[0] * dir_[0] + dir_[1] * dir_[1] + dir_[2] * dir_[2])));
        if (0.0f < len) dir_ = {dir_[0] / len, dir_[1] / len, dir_[2] / len};
        rate_ = std::clamp(61 - static_cast<int32_t>(f[15]), 1, 60);
        amount_ = std::max(1, static_cast<int32_t>(f[16]));
        if (wc) {
            if (static_cast<int32_t>(f[18]) != 0) blend_ = 1;
            face_ = static_cast<int32_t>(f[19]) != 0;
        }
        state_.resize(records_.size());
        if (static_cast<int32_t>(f[17]) == 0) {
            active_ = 1;
            for (size_t i = 0; i < records_.size(); ++i) spawn(i);
        } else {
            active_ = records_.size() >> 1;
            for (size_t i = 0; i < active_; ++i) headStart(i);
            for (size_t i = active_; i < records_.size(); ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (rng_.next() % static_cast<uint32_t>(rate_) == 0) active_ += static_cast<size_t>(amount_);
        active_ = std::min(active_, records_.size());
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t age = s.counter - s.start;
            if (s.phase == 0) {
                setAlpha(r, static_cast<int32_t>(mapRange(0, s.alpha, 0, static_cast<float>(in_), static_cast<float>(s.counter))));
                r.pos = s.path.at(static_cast<float>(s.counter));
                if (face_) face(r, s.last);
                if (in_ <= s.counter) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                setAlpha(r, static_cast<int32_t>(s.alpha));
                r.pos = s.path.at(static_cast<float>(s.counter));
                if (face_) face(r, s.last);
                if (hold_ <= age) s.phase = 2, s.start = s.counter;
            } else if (s.phase == 2) {
                setAlpha(r, static_cast<int32_t>(mapRange(s.alpha, 0, 0, static_cast<float>(out_), static_cast<float>(age))));
                r.pos = s.path.at(static_cast<float>(s.counter));
                if (face_) face(r, s.last);
                if (out_ < age) s.phase = 3, s.start = s.counter;
            } else {
                spawn(i);
            }
            s.last = r.pos;
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        float alpha = 0;
        SplinePath path;
        std::array<float, 3> last{};  // the record's position at the last update's end (+0x29c)
    };

    // wc_steam with f19: the record turned to its way, from its last position less this one
    // (0x100017d0), made unit length: acos of that's -y, negated when its x is under 0.
    static void face(ParticleRecord& r, const std::array<float, 3>& last) {
        float x = last[0] - r.pos[0], y = last[1] - r.pos[1], z = last[2] - r.pos[2];
        const float len = static_cast<float>(std::sqrt(static_cast<double>(x * x + y * y + z * z)));
        if (0.0f < len) x = x / len, z = z / len, y = y / len;
        float a = plugAcos((x * 0.0f - y * 1.0f) + z * 0.0f);
        if (0.0f < (y * 0.0f - x * 1.0f) + z * 0.0f) a = a * -1.0f;
        r.rot[2] = a;
    }

    // v through the plugins' z turn (0x100016b0 on an identity: plugAngle, then the CRT's cos /
    // sin), as a row vector, the matrix's terms summed in order.
    static std::array<float, 3> turned(const std::array<float, 3>& v, float angle) {
        Mat m = identity();
        plugRotate(m, 0, 0, angle);
        return {v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12], v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
                v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14]};
    }

    // One step: the direction turned by turn + spread x (rand - 0.5), as long as length + spread x
    // (rand + 0.5).
    std::array<float, 3> stepFrom(const std::array<float, 3>& p) {
        const float a = rng_.unit() * turnSpread_ - turnSpread_ * 0.5f + turn_;
        const float len = rng_.unit() * stepSpread_ + stepSpread_ * 0.5f + step_;
        const auto d = turned(dir_, a);
        return {d[0] * len + p[0], d[1] * len + p[1], d[2] * len + p[2]};
    }

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const uint32_t q = rng_.next() & 3;
        setUv(r, kQuarter[q][0], kQuarter[q][1], 0.5f, 0.5f);
        const float sz = rng_.unit() * sizeSpread_ - sizeSpread_ * 0.5f + size_;
        r.scale = {sz, sz, 1};
        setAlpha(r, 0);
        auto p = line_.at(rng_.unit() * 100.0f);
        s.path = SplinePath{};
        s.path.add(p);
        r.pos = p;
        p = stepFrom(p);
        s.path.add(p);
        for (int k = 0; k < 3; ++k) {
            if (rng_.next() % 5 == 0) continue;
            p = stepFrom(p);
            s.path.add(p);
        }
        const float d = static_cast<float>(out_ + hold_ + in_);
        if (wc_) {
            s.path.finishMean(d);  // 0x10001e50, energy's
        } else {
            s.path.finish(d, d * 0.4f);
        }
        s.alpha = rng_.unit() * alphaSpread_ - alphaSpread_ * 0.5f + alpha_;
        s.last = r.pos;
        s.counter = 0, s.phase = 0;
    }

    // 0x10003320: a puff part-way through its life (rand x its frames), its phase moved on to
    // match and, once past the fade-in, its alpha 0x80.
    void headStart(size_t i) {
        spawn(i);
        State& s = state_[i];
        s.counter = static_cast<int32_t>(rng_.unit() * static_cast<float>(out_ + hold_ + in_));
        if (s.counter > in_) {
            ++s.phase, s.start += in_;
            setAlpha(records_[i], 0x80);
        }
        if (s.counter > hold_) ++s.phase, s.start += hold_;
    }

    bool wc_ = false, face_ = false;
    std::vector<State> state_;
    SplinePath line_;
    std::array<float, 3> dir_{};
    float step_ = 0, stepSpread_ = 0, turn_ = 0, turnSpread_ = 0, size_ = 0, sizeSpread_ = 0, alpha_ = 0, alphaSpread_ = 0;
    int32_t in_ = 0, hold_ = 0, out_ = 0, rate_ = 1, amount_ = 1;
    size_t active_ = 0;
};

// dash.ppi (Happiness 2, 20 loads): speed lines streaking across the screen past a clear band
// through its middle. Init 0x10001390, Update 0x10001460, spawn 0x10001a00. Additive (+0x10 = 1).
// It does not read the plane fields.
//   f0: records per group (1 in every load: the base, 0x10001120, makes count / f0 groups, each a
//   head and f0 - 1 trail pieces); f1: speed; f2: width (percent of the texture); f3: direction
//   (degrees, the host's +0x10); f4: the half-length of the run and twice the band's half-width;
//   f5: how far the lines spread out from the band; f6: alpha.
// A line takes one of the texture's four columns (rand & 3), a width of (0.2-1.0) x f2 / 100, the
// speed, and an offset across the run built up over one to five steps (step k adds (0.1 + 0.9
// rand) x k x 0.1 x f5, the speed shrinks by 0.85-0.95 each, and a random number divisible by 5
// stops it); even lines then sit at f4 / 2 less it, odd ones at it less f4 / 2. It turns to f3
// (its record's z turn too) and runs from (offset, -f4) to (offset, f4 + the texture's height),
// both turned (the plugins' z turn: plugAngle, the CRT's cos / sin) about the screen's centre (640,
// 360), at its speed; once further than that from its start it starts again in the next update.
// One line more runs each update, up to all of them. That count is a global of the DLL
// (0x10017110, 1 in the file; nothing else writes it), so it starts afresh with each load of the
// DLL and layers running dash at once share it (PluginGlobals). The trail pieces (f0 above 1) are
// not reimplemented: they are left out of sight.
class Dash : public ParticleEffect {
public:
    struct Globals : PluginGlobals {
        uint32_t active = 1;
    };

    Dash(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        blend_ = 1;
        const auto& f = p.f;
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        speed_ = f[1];
        width_ = f[2] / 100.0f;
        angle_ = f[3];
        half_ = f[4];
        spread_ = f[5];
        alpha_ = static_cast<int32_t>(f[6]);
        heads_.resize(groups_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        dll_->active = std::min(dll_->active + 1, groups_);
        for (uint32_t i = 0; i < dll_->active; ++i) {
            Head& h = heads_[i];
            ParticleRecord& r = records_[i];
            if (h.phase == 0) {
                h.cur[0] = h.vel[0] + h.cur[0];
                h.cur[1] = h.vel[1] + h.cur[1];
                r.pos = h.cur;
                setAlpha(r, alpha_);
                const float dx = h.cur[0] - h.from[0], dy = h.cur[1] - h.from[1], dz = h.cur[2] - h.from[2];
                const float len = static_cast<float>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
                if (h.dist < len) h.phase = 2, h.start = h.counter;
            } else {
                spawn(i);
            }
            h.counter += frames;
        }
    }

private:
    struct Head {
        int32_t counter = 0, start = 0, phase = 0;
        float speed = 0, dist = 0;
        std::array<float, 3> from{}, vel{}, to{}, cur{};
    };

    void spawn(uint32_t i) {
        static constexpr float kColumn[4] = {0, 0.25f, 0.5f, 0.75f};  // .data 0x100170b0
        ParticleRecord& r = records_[i];
        Head& h = heads_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        setUv(r, kColumn[rng_.next() & 3], 0, 0.25f, 1.0f);
        const float u = rng_.unit();
        r.scale = {u * width_ * 0.8f + width_ * 0.2f, 1, 1};
        setAlpha(r, 0);
        h.speed = speed_;
        float off = 0;
        for (int k = 1; k < 6; ++k) {
            const float a = rng_.unit();
            off = (a * 0.9f + 0.1f) * (static_cast<float>(k) * 0.1f * spread_) + off;
            const float b = rng_.unit();
            h.speed = (b * 0.1f + 0.85f) * h.speed;
            if (rng_.next() % 5 == 0) break;
        }
        off = i % 2 == 0 ? half_ * 0.5f - off : off - half_ * 0.5f;
        const float rad = plugRad(angle_);
        r.rot[2] = rad;
        const float q = plugAngle(rad);
        const float c = plugCos(q), sn = plugSin(q);
        const float y0 = -half_, y1 = half_ + params_.texH;
        h.from = {-sn * y0 + c * off, y0 * c + sn * off, 0};
        h.to = {c * off + -sn * y1, sn * off + y1 * c, 0};
        h.from[0] = h.from[0] + 640.0f, h.from[1] = h.from[1] + 360.0f;
        h.to[0] = h.to[0] + 640.0f, h.to[1] = h.to[1] + 360.0f;
        const auto v = SplinePath::sub(h.to, h.from);
        const float len = static_cast<float>(std::sqrt(static_cast<double>(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
        h.dist = len;
        h.vel = v;
        if (0.0f < len) h.vel = {v[0] / len, v[1] / len, v[2] / len};
        h.vel = {h.vel[0] * h.speed, h.vel[1] * h.speed, h.vel[2] * h.speed};
        h.cur = h.from;
        h.counter = 0, h.phase = 0;
        for (uint32_t k = 0; k < trail_; ++k) {
            ParticleRecord& c2 = records_[groups_ + i * trail_ + k];
            c2.pos = r.pos;
            setAlpha(c2, 0);
        }
    }

    Globals* dll_ = nullptr;
    std::vector<Head> heads_;
    uint32_t groups_ = 0, trail_ = 0;
    float speed_ = 0, width_ = 0, angle_ = 0, half_ = 0, spread_ = 0;
    int32_t alpha_ = 0;
};

// rising4.ppi (Happiness 2, 16 loads): a spinning twinkle that throws off a trail of smaller
// ones. Init 0x10001fa0, Update 0x10002030, spawn 0x10002680. Additive (+0x10 = 1). It does not
// read the plane fields. Init gives the base (0x10001120) the count as the group size: one group,
// a head (record 0) and count - 1 trail pieces (the records after it).
//   f0 f1 f2: the head's place; f3: its size (percent of the texture). f5 (frames) and f6 f7 f8 (a
//   place to go to) belong to a second phase that nothing starts (the head's phase is never set
//   to 1), so the head stays where Init puts it.
// rising3.ppi (4 loads) is an older build of the same (Init 0x10001070, Update 0x10001100, spawn
// 0x10001720; x87 throughout, its constants doubles holding the same floats) whose head does go:
// once its clock passes f4 frames it moves from f0 f1 f2 to f6 f7 f8 over f5 frames of the update's
// floats (the host's linear map of each), turning on, then stays there. Its x87 code runs in single
// precision (x87Note), so its own arithmetic is float steps here; its path code orders its float
// steps differently from SplinePath's (a last bit's difference at most).
// The head is the texture's bottom-left quarter, white at 255; it spins by its turn (-pi/2 to
// pi/2 a frame, x87). Each update it hands its place, turn, pivot and scale to the first free
// piece, with a two-point path (cs2ParticleBaseEx's, tangents 100 long, over count - 1 frames)
// from there to a random point up to 250 away in x, y and z, and takes a new turn. A piece
// shrinks by 0.9 an update, follows its path by its clock and fades from 255 to 0 over count - 1
// frames, then is free again (keeping its last alpha until it is used). Its own turn is never set:
// it keeps the head's angle from when it was thrown.
class Rising4 : public ParticleEffect {
public:
    Rising4(const ParticleParams& p, uint32_t seed, bool rising3 = false) : ParticleEffect(p, seed), rising3_(rising3) {
        blend_ = 1;
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        trail_ = count != 0 ? count - 1 : 0;
        if (count == 0) return;
        pieces_.resize(trail_);
        ParticleRecord& r = records_[0];
        const auto& f = p.f;
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        setUv(r, 0.0f, 0.5f, 0.5f, 0.5f);  // .data 0x100150c0
        const float sz = f[3] / 100.0f;
        r.scale = {sz, sz, 1};
        setAlpha(r, 255);
        r.pos = {f[0], f[1], f[2]};
        spin_ = newSpin();
        for (uint32_t k = 0; k < trail_; ++k) {
            ParticleRecord& c = records_[1 + k];
            c.pos = r.pos;
            for (size_t v = 0; v < 4; ++v) {
                c.v[v].r = r.v[v].r, c.v[v].g = r.v[v].g, c.v[v].b = r.v[v].b, c.v[v].alpha = 0;
                c.v[v].u = r.v[v].u, c.v[v].v = r.v[v].v;
            }
            c.pivot[0] = r.pivot[0], c.pivot[1] = r.pivot[1];
        }
    }

    void step(int32_t frames) override {
        if (records_.empty()) return;
        ParticleRecord& r = records_[0];
        const auto& f = params_.f;  // the floats of this update
        if (phase_ == 0) {
            r.rot[2] = spin_ + r.rot[2];
            if (rising3_ && f[4] < static_cast<float>(counter_)) phase_ = 1, start_ = counter_;
        } else if (phase_ == 1) {
            const float lo = static_cast<float>(start_), hi = f[5] + lo, x = static_cast<float>(counter_);
            r.pos = {mapRange(f[0], f[6], lo, hi, x), mapRange(f[1], f[7], lo, hi, x), mapRange(f[2], f[8], lo, hi, x)};
            r.rot[2] = spin_ + r.rot[2];
            if (hi < x) phase_ = 2, start_ = counter_;
        }
        for (uint32_t k = 0; k < trail_; ++k) {
            Piece& pc = pieces_[k];
            if (pc.on) continue;
            ParticleRecord& c = records_[1 + k];
            c.pos = r.pos;
            c.rot[2] = r.rot[2];
            pc.path = SplinePath{};
            pc.path.add(c.pos);
            std::array<float, 3> to{};
            for (size_t a = 0; a < 3; ++a) {
                const float u = rng_.unit();
                to[a] = rising3_ ? u * 500.0f + c.pos[a] - 250.0f
                                 : u * 500.0f + c.pos[a] - 250.0f;
            }
            pc.path.add(to);
            pc.path.finish(static_cast<float>(trail_), 100.0f);
            c.pivot = r.pivot;
            c.scale = r.scale;
            spin_ = newSpin();
            pc.on = true;
            break;
        }
        for (uint32_t k = 0; k < trail_; ++k) {
            Piece& pc = pieces_[k];
            if (!pc.on) continue;
            ParticleRecord& c = records_[1 + k];
            c.scale[0] = c.scale[0] * 0.9f;
            c.scale[1] = c.scale[1] * 0.9f;
            c.pos = pc.path.at(static_cast<float>(pc.counter));
            setAlpha(c, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(trail_), static_cast<float>(pc.counter))));
            pc.counter += frames;
            if (pc.counter >= static_cast<int32_t>(trail_)) pc.counter = 0, pc.on = false;
        }
        counter_ += frames;
    }

private:
    struct Piece {
        int32_t counter = 0;
        bool on = false;
        SplinePath path;
    };

    // rand x pi - pi / 2 (x87, single precision: two float steps).
    float newSpin() {
        return rng_.unit() * 3.14159274f - 1.57079637f;
    }

    bool rising3_;
    std::vector<Piece> pieces_;
    uint32_t trail_ = 0;
    int32_t counter_ = 0, start_ = 0, phase_ = 0;  // the head's
    float spin_ = 0;
};

// ring.ppi (Happiness 2, 12 loads; wc_aoi.ppi is the same file, 6) and hadou.ppi (10): rings
// spreading from a point, each with fainter copies left behind. ring: Init 0x100013e0, Update
// 0x10001520; hadou: Init 0x100013e0, Update 0x10001530; spawn (the same code) 0x10001a30 /
// 0x10001a80. Additive (+0x10 = 1). The base (0x10001120) makes count / f0 groups, each a head
// (records [0, groups)) and f0 - 1 copies (the records after the heads, group by group).
//   ring: f0: records per group; f1 f2: the centre; f3 f4: scale from and to (percent, at least
//   0.001 as a scale); f5: one more group every f5 updates (a negative one is 1); f6 f7: fade-in
//   to 200 and fade-out frames.
//   hadou: the same to f5, then f6: alpha at full; f7 f8: fade-in and fade-out; f9: frames to wait
//   before it starts again. Its width is 1.1 times its height.
// A head sits at the centre (z 0), the texture's whole image, white at alpha 0; its scale goes from
// f3 to f4 over fade-in + fade-out frames of its clock while it fades in and out. A ring is then
// done (it stays at the centre, alpha where the fade left it); hadou waits and starts again. While
// a head fades, each update the first free copy takes its pivot, place and scale (z stays 1, 2, ...
// nearer than the head) at alpha 0, then every copy in use sits at the head's place and fades from
// 10 to 0 over 40 updates (its own count, one an update, not the clock); a copy is never freed.
// Heads move with their plane's move (+0x24 / +0x28 less half the screen) once an update, after
// their copies took their place: the copies stay at the centre.
class RingWave : public ParticleEffect {
public:
    RingWave(const ParticleParams& p, uint32_t seed, bool hadou) : ParticleEffect(p, seed), hadou_(hadou) {
        const auto& f = p.f;
        blend_ = 1;
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        centre_ = {f[1], f[2]};
        from_ = f[3] / 100.0f;
        if (from_ < 0.001f) from_ = 0.001f;
        to_ = f[4] / 100.0f;
        if (to_ < 0.001f) to_ = 0.001f;
        const int32_t rate = static_cast<int32_t>(f[5]);
        rate_ = static_cast<uint32_t>(rate < 0 ? 1 : rate);
        if (hadou_) {
            peak_ = static_cast<float>(static_cast<int32_t>(f[6]));
            in_ = static_cast<int32_t>(f[7]), out_ = static_cast<int32_t>(f[8]), wait_ = static_cast<int32_t>(f[9]);
        } else {
            peak_ = 200.0f;
            in_ = static_cast<int32_t>(f[6]), out_ = static_cast<int32_t>(f[7]);
        }
        heads_.resize(groups_);
        copies_.resize(static_cast<size_t>(groups_) * trail_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        // The DLL divides by the rate (an unsigned remainder); a rate of 0 would fault it.
        if (rate_ != 0 && tick_ % rate_ == 0) ++active_;
        ++tick_;
        active_ = std::min(active_, groups_);
        const auto move = planeMove();
        for (uint32_t i = 0; i < active_; ++i) {
            Head& h = heads_[i];
            ParticleRecord& r = records_[i];
            const int32_t age = h.counter - h.start;
            if (h.phase == 0 || h.phase == 1) {
                r.pos[0] = centre_[0], r.pos[1] = centre_[1];
                const float sc = mapRange(from_, to_, 0, static_cast<float>(in_ + out_), static_cast<float>(h.counter));
                r.scale[1] = sc;
                r.scale[0] = hadou_ ? sc * 1.1f : sc;
                if (h.phase == 0) {
                    setAlpha(r, static_cast<int32_t>(mapRange(0, peak_, 0, static_cast<float>(in_), static_cast<float>(age))));
                    if (in_ < age) h.phase = 1, h.start = h.counter;
                } else {
                    setAlpha(r, static_cast<int32_t>(mapRange(peak_, 0, 0, static_cast<float>(out_), static_cast<float>(age))));
                    if (out_ < age) h.phase = hadou_ ? 2 : 9, h.start = h.counter;
                }
            } else if (hadou_ && h.phase == 2) {
                if (wait_ < age) h.phase = 9, h.start = h.counter;
            } else if (hadou_) {
                spawn(i);
            } else {
                r.pos[0] = centre_[0], r.pos[1] = centre_[1];
            }
            if (h.phase < 4) {
                for (uint32_t k = 0; k < trail_; ++k) {
                    Copy& cp = copies_[i * trail_ + k];
                    if (cp.on) continue;
                    ParticleRecord& c = records_[groups_ + i * trail_ + k];
                    c.pivot = r.pivot;
                    c.pos[0] = r.pos[0], c.pos[1] = r.pos[1];
                    c.scale = r.scale;
                    setAlpha(c, 0);
                    cp.on = true, cp.counter = 0;
                    break;
                }
            }
            for (uint32_t k = 0; k < trail_; ++k) {
                Copy& cp = copies_[i * trail_ + k];
                if (!cp.on) continue;
                ParticleRecord& c = records_[groups_ + i * trail_ + k];
                c.pos[0] = r.pos[0], c.pos[1] = r.pos[1];
                setAlpha(c, static_cast<int32_t>(mapRange(10, 0, 0, 40, static_cast<float>(cp.counter))));
                if (++cp.counter > 39) setAlpha(c, 0);
            }
            addMove(r, move);
            h.counter += frames;
        }
    }

private:
    struct Head {
        int32_t counter = 0, start = 0, phase = 0;
    };
    struct Copy {
        int32_t counter = 0;
        bool on = false;
    };

    void spawn(uint32_t i) {
        ParticleRecord& r = records_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        r.pos = {centre_[0], centre_[1], 0};
        r.scale = {from_, from_, 1};
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;  // 0x00ffffff
        heads_[i] = Head{};
        for (uint32_t k = 0; k < trail_; ++k) {
            copies_[i * trail_ + k] = Copy{};
            ParticleRecord& c = records_[groups_ + i * trail_ + k];
            c.pivot[0] = r.pivot[0], c.pivot[1] = r.pivot[1];
            c.pos = r.pos;
            c.pos[2] = static_cast<float>(k + 1) + c.pos[2];
            for (auto& v : c.v) v.r = v.g = v.b = 255, v.alpha = 0;
        }
    }

    bool hadou_;
    std::vector<Head> heads_;
    std::vector<Copy> copies_;
    uint32_t groups_ = 0, trail_ = 0, rate_ = 1, tick_ = 0, active_ = 0;
    std::array<float, 2> centre_{};
    float from_ = 0, to_ = 0, peak_ = 0;
    int32_t in_ = 0, out_ = 0, wait_ = 0;
};

// wc_mirror.ppi (Happiness 2, 6 loads) and wc_prism.ppi (2): mirror shards circling a vertical
// axis, each turned to face it. Init 0x100013a0, Update 0x10001530, spawn 0x10001b20 (the two DLLs
// differ only there: wc_prism colours all four corners). As digi's, Init passes the base
// ECX as the group size, which the CRT's memset leaves at 0 on its `rep stosb` path: every record
// is a shard of its own.
//   f0 f1: the axis (its z 0); f3: the shards' tilt (degrees, rot x = -it, held to 0-pi/2); f6 f7:
//   size and its spread (thousandths of the texture, 6 times as tall); f8: radius (each 50-150%);
//   f9: alpha (0-2x, held to 255); f10 f11 f12: fade-in, hold, fade-out frames (50-150%); f13:
//   additive (non-zero). f2, f4 and f5 are kept and not read.
// A shard pivots on its bottom edge's middle; its top corners (wc_prism: every corner) take a
// colour strong in one channel (225-255, which one at random) and weak in the others (0-29), the
// others stay white; all start at alpha 0. It starts at a random angle (0-360 degrees, the plugins' three decimals) and
// turns by up to 0.01 of a radian an update either way; each update it sits on its circle (x =
// axis - sin x radius, x87; z = cos x radius) turned about y to face the axis (as digiring's), fades
// in, holds, fades out and starts again. One more shard runs each update, up to all of them. Shards
// add their plane's move once an update.
class WcMirror : public ParticleEffect {
public:
    WcMirror(const ParticleParams& p, uint32_t seed, int corners) : ParticleEffect(p, seed), corners_(corners) {
        const auto& f = p.f;
        axis_ = {f[0], f[1]};
        const float t = plugRad(f[3]);
        tilt_ = 0.0f;
        if (0.0f <= t) tilt_ = t < 1.57079637f ? t : 1.57079637f;
        size_ = f[6] / 1000.0f, sizeSpread_ = f[7] / 1000.0f;
        radius_ = f[8];
        alpha_ = static_cast<int32_t>(f[9]);
        in_ = static_cast<int32_t>(f[10]), hold_ = static_cast<int32_t>(f[11]), out_ = static_cast<int32_t>(f[12]);
        if (static_cast<int32_t>(f[13]) != 0) blend_ = 1;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        active_ = std::min(active_ + 1, records_.size());
        const auto move = planeMove();
        for (size_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.phase > 2) {
                spawn(i);
            } else {
                const int32_t age = s.counter - s.start;
                s.angle = s.speed + s.angle;
                const float sn = plugSin(s.angle);  // host +0x18
                r.pos[0] = sn * -s.radius + axis_[0];  // x87Note
                r.pos[1] = axis_[1];
                r.pos[2] = plugCos(s.angle) * s.radius;  // host +0x1c
                // Host +0x70 (D3DXVec4Normalize of (d, 0)) of the way to the axis, then +0x28 (acos).
                const float dx = axis_[0] - r.pos[0], dy = 0.0f, dz = 0.0f - r.pos[2];
                const float n = std::sqrt(dx * dx + dy * dy + dz * dz + 0.0f * 0.0f);
                const float nx = dx / n, ny = dy / n, nz = dz / n;
                float turn = plugAcos(ny * 0.0f + nx * 0.0f - nz * 1.0f);
                if (ny * 0.0f - nx * 1.0f + nz * 0.0f < 0.0f) turn *= -1.0f;
                r.rot[0] = -tilt_;
                r.rot[1] = turn;
                if (s.phase == 0) {
                    setAlpha(r, static_cast<int32_t>(mapRange(0, static_cast<float>(s.alpha), 0, static_cast<float>(s.in), static_cast<float>(age))));
                    if (s.in < age) s.phase = 1, s.start = s.counter;
                } else if (s.phase == 1) {
                    if (s.hold < age) s.phase = 2, s.start = s.counter;
                } else {
                    setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(s.alpha), 0, 0, static_cast<float>(s.out), static_cast<float>(age))));
                    if (s.out < age) s.phase = 3, s.start = s.counter;
                }
            }
            r.pos[0] = move[0] + r.pos[0];
            r.pos[1] = r.pos[1] + move[1];
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        float radius = 0, angle = 0, speed = 0;
        int32_t alpha = 0, in = 0, hold = 0, out = 0;
    };

    // (2 rand - 1) x spread x 0.5 + base, whole (CVTTSS2SI).
    int32_t around(int32_t base) {
        const float b = static_cast<float>(base);
        return static_cast<int32_t>((rng_.unit() * 2.0f - 1.0f) * b * 0.5f + b);
    }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH;
        const float sz = (rng_.unit() * 2.0f - 1.0f) * sizeSpread_ + size_;
        r.scale = {sz, sz * 6.0f, 1};
        for (int k = 0; k < corners_; ++k) {
            uint32_t b = 0, g = 0, red = 0;
            switch (rng_.next() % 3) {
            case 0:
                b = rng_.next() % 31 + 225, g = rng_.next() % 30, red = rng_.next() % 30;
                break;
            case 1:
                b = rng_.next() % 30, g = rng_.next() % 31 + 225, red = rng_.next() % 30;
                break;
            default:
                b = rng_.next() % 30, g = rng_.next() % 30, red = rng_.next() % 31 + 225;
                break;
            }
            r.v[k].b = static_cast<uint8_t>(b), r.v[k].g = static_cast<uint8_t>(g), r.v[k].r = static_cast<uint8_t>(red);
            r.v[k].alpha = 0;
        }
        if (corners_ == 2) r.v[2].alpha = r.v[3].alpha = 0;
        s.angle = plugAngle(plugRad(rng_.unit() * 360.0f));
        const float a = rng_.unit() * 2.0f - 1.0f;
        s.speed = rng_.unit() * 0.01f * a;  // x87Note
        s.radius = (rng_.unit() * 2.0f - 1.0f) * radius_ * 0.5f + radius_;
        const float al = static_cast<float>(alpha_);
        const float v = (rng_.unit() * 2.0f - 1.0f) * al + al;
        s.alpha = static_cast<int32_t>(0.0f > v ? 0.0f : (255.0f > v ? v : 255.0f));
        s.counter = s.start = s.phase = 0;
        s.in = around(in_), s.hold = around(hold_), s.out = around(out_);
    }

    int corners_;
    std::vector<State> state_;
    std::array<float, 2> axis_{};
    float tilt_ = 0, size_ = 0, sizeSpread_ = 0, radius_ = 0;
    int32_t alpha_ = 0, in_ = 0, hold_ = 0, out_ = 0;
    size_t active_ = 0;
};

// The CRT's rand() as a plugin DLL links it (MSVC's: holdrand x 214013 + 2531011, bits 16-30).
// Its state is the DLL's own per-thread data, which starts at 1 on the DLL's first use of it: it
// begins again with each load of the DLL and is shared by layers running it at once
// (PluginGlobals).
struct CrtRand {
    uint32_t holdrand = 1;
    int32_t rand() {
        holdrand = holdrand * 214013u + 2531011u;
        return static_cast<int32_t>(holdrand >> 16 & 0x7fff);
    }
};

// paper.ppi (Happiness 2, 4 loads: scraps of paper drifting down), sakura's sibling in an x87
// (VS2008) build. Init 0x100010b0, Update 0x10001230, spawn 0x10001860. Alpha blend. It does not
// read the plane fields.
//   f0: records per group (1 in every load; the trail pieces of more are not reimplemented); f1:
//   life (frames); f2: size (percent, plus up to 0.05 at random); f3: one start in 61 - f3 updates
//   (1-60); f4: how many then; f5: start half of them part-way (their clock rand x life); f6:
//   start again once done.
// A scrap takes one of seven colours (.data 0x10011d20, rand % 7), a place on a line above the
// screen (x 0-1280, y -50, z 0-100) and a turn of 0-pi about each axis, then a path (cs2Particle-
// BaseEx's, tangents 50 long, over its life) of thirteen steps: the first down and to either side
// (x rand - 0.5, y rand, unit length) as long as 50 + (rand() % 500) / 10 (the DLL's own CRT
// rand(), seeded 1 when the DLL loads, so layers running paper at once share it: PluginGlobals),
// then twelve each within 10 degrees of that first direction (the bounds turned -10 and +10, a
// random offset within their spread), 50-99.9 long. Each update it sits on its path by its clock,
// turns by rand / 10 about each axis, and over the last tenth of its life fades from 255 to 0.
// Its x87 arithmetic runs in single precision (x87Note): float steps here; its path code orders
// its float steps differently from SplinePath's (a last bit's difference at most).
class Paper : public ParticleEffect {
public:
    struct Globals : PluginGlobals, CrtRand {};

    Paper(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        const auto& f = p.f;
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        life_ = static_cast<int32_t>(f[1]);
        size_ = f[2] / 100.0f;
        rate_ = std::clamp(61 - static_cast<int32_t>(f[3]), 1, 60);
        amount_ = static_cast<int32_t>(f[4]);
        again_ = static_cast<int32_t>(f[6]) != 0;
        state_.resize(groups_);
        if (static_cast<int32_t>(f[5]) != 0) {
            active_ = static_cast<int32_t>(groups_ >> 1);
            for (int32_t i = 0; i < active_; ++i) {
                spawn(static_cast<size_t>(i));
                state_[static_cast<size_t>(i)].counter = static_cast<int32_t>(rng_.unit() * static_cast<float>(life_));
            }
            for (size_t i = static_cast<size_t>(active_); i < groups_; ++i) spawn(i);
        } else {
            active_ = 0;
            for (size_t i = 0; i < groups_; ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (rng_.next() % static_cast<uint32_t>(rate_) == 0) active_ += amount_;
        if (static_cast<int32_t>(groups_) < active_) active_ = static_cast<int32_t>(groups_);
        for (int32_t i = 0; i < active_; ++i) {
            State& s = state_[static_cast<size_t>(i)];
            ParticleRecord& r = records_[static_cast<size_t>(i)];
            if (s.phase == 0) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                setAlpha(r, 255);
                turn(r);
                // The host's round (+0x50) of life x 0.9 (a float).
                const float edge = static_cast<float>(life_) * 0.9f;
                if (static_cast<int32_t>(std::floor(edge + 0.5f)) < s.counter) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, static_cast<float>(s.start), static_cast<float>(life_), static_cast<float>(s.counter))));
                turn(r);
                if (life_ < s.counter) s.phase = 2, s.start = s.counter;
            } else if (again_) {
                spawn(static_cast<size_t>(i));
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        SplinePath path;
    };

    void turn(ParticleRecord& r) {
        for (float& a : r.rot) a = rng_.unit() / 10.0f + a;
    }

    // A unit vector as the DLL works it out: (y y + x x) + z z, its length the CRT's sqrt of that
    // float, each part divided by it; left as it is at length 0.
    static std::array<float, 3> unit3(std::array<float, 3> v) {
        const float sq = v[1] * v[1] + v[0] * v[0] + v[2] * v[2];
        const float len = static_cast<float>(std::sqrt(static_cast<double>(sq)));
        if (len > 0.0f) {
            for (float& c : v) c = c / len;
        }
        return v;
    }

    float stepLength(int32_t r) {
        return static_cast<float>(r % 500) / 10.0f + 50.0f;
    }

    void spawn(size_t i) {
        static constexpr uint8_t kColour[7][3] = {  // b, g, r
            {240, 240, 240}, {110, 255, 255}, {255, 110, 255}, {255, 255, 110}, {110, 110, 255}, {110, 255, 110}, {255, 110, 110}};
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const float sz = rng_.unit() * 0.05f + size_;
        r.scale = {sz, sz, 1};
        const auto& c = kColour[rng_.next() % 7];
        for (auto& v : r.v) v.b = c[0], v.g = c[1], v.r = c[2], v.alpha = 0;
        r.pos[0] = rng_.unit() * 1280.0f;
        r.pos[1] = -50.0f;
        r.pos[2] = rng_.unit() * 100.0f;
        for (float& a : r.rot) a = rng_.unit() * 3.14159274f;
        s.counter = 0, s.phase = 0;
        s.path = SplinePath{};
        s.path.add(r.pos);
        std::array<float, 3> d{rng_.unit() - 0.5f, 0, 0};
        d[1] = rng_.unit();
        d = unit3(d);
        float len = stepLength(dll_->rand());
        std::array<float, 3> p{r.pos[0] + len * d[0], r.pos[1] + len * d[1], r.pos[2] + len * d[2]};
        s.path.add(p);
        const auto base = unit3(SplinePath::sub(p, r.pos));
        // The bounds: the base turned by -10 and +10 degrees (host +0x10, +0x1c cos, +0x18 sin).
        auto bound = [&](float deg) {
            const float a = plugRad(deg), cs = plugCos(a), sn = plugSin(a);
            return std::array<float, 2>{cs * base[0] - sn * base[1], cs * base[1] + sn * base[0]};
        };
        const auto b1 = bound(-10.0f), b2 = bound(10.0f);
        const float wx = std::fabs(b2[0] - b1[0]), wy = std::fabs(b2[1] - b1[1]);  // host +0x44 of +0x48 (x, 2)
        for (int k = 0; k < 12; ++k) {
            auto spread = [&](float w) {
                return rng_.unit() * w - w * 0.5f;
            };
            std::array<float, 3> n{base[0] + spread(wx), 0, 0};
            n[1] = base[1] + spread(wy);
            n = unit3(n);
            len = stepLength(static_cast<int32_t>(rng_.next() % 500));
            p = {p[0] + len * n[0], p[1] + len * n[1], p[2] + len * n[2]};
            s.path.add(p);
        }
        s.path.finish(static_cast<float>(life_), 50.0f);
        for (uint32_t k = 0; k < trail_; ++k) setAlpha(records_[groups_ + i * trail_ + k], 0);
    }

    Globals* dll_ = nullptr;
    std::vector<State> state_;
    uint32_t groups_ = 0, trail_ = 0;
    int32_t life_ = 0, rate_ = 1, amount_ = 0, active_ = 0;
    float size_ = 0;
    bool again_ = false;
};

// wc_slash.ppi (Happiness 2, 4 loads: blades of wind sweeping past): a ribbon fanned
// out from a point. Init 0x100022d0, Update 0x10002640, spawns 0x10002ca0 / 0x10002d80. As digi's,
// Init passes the base the ECX the CRT's memset leaves (0): every record is a piece of its own. It
// does not use the plane fields (the move is worked out and not read).
//   f0-f2, f3-f5, f6-f8: a three-point path (cs2ParticleBaseEx's, tangents finishMean, over f9
//   frames, whole); f10 f11 f12: each piece's fade-in to f14, hold and fade-out (a negative
//   fade-out skips it); f13: size (percent); f15: additive (non-zero).
// The fan's point is the middle of the path's ends pushed half as far again away from its middle
// point (m + (m - f3..f5) / 2). Each update an emitter runs along the path by the layer's clock
// and leaves one piece per 40 units it went (the whole distance's whole part / 40), each a quad
// whose first two corners are on the path (that step split evenly by the clock) and whose others
// are both the fan's point; a step under 40 leaves one piece from the last place to this one. The
// corners are set as they are: the record keeps the engine's pivot (half the texture) and its
// place at 0, and is scaled by the size. Pieces fade in, hold, fade out and stay out; once every
// record is used no more are left (the DLL's code to free a piece is not reached).
class WcSlash : public ParticleEffect {
public:
    WcSlash(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        const std::array<float, 3> a{f[0], f[1], f[2]}, b{f[3], f[4], f[5]}, c{f[6], f[7], f[8]};
        const std::array<float, 3> m{(c[0] + a[0]) * 0.5f, (c[1] + a[1]) * 0.5f, (c[2] + a[2]) * 0.5f};
        for (size_t k = 0; k < 3; ++k) focus_[k] = m[k] + (m[k] - b[k]) * 0.5f;
        const float dur = static_cast<float>(static_cast<int32_t>(f[9]));
        in_ = static_cast<int32_t>(f[10]), hold_ = static_cast<int32_t>(f[11]), out_ = static_cast<int32_t>(f[12]);
        size_ = f[13] / 100.0f;
        alpha_ = static_cast<int32_t>(f[14]);
        if (static_cast<int32_t>(f[15]) != 0) blend_ = 1;
        path_.add(a);
        cur_ = prev_ = a;
        path_.add(b);
        path_.add(c);
        path_.finishMean(dur);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) reset(i);
    }

    void step(int32_t frames) override {
        cur_ = path_.at(static_cast<float>(clock_));
        const auto d = SplinePath::sub(prev_, cur_);
        const double dist = std::sqrt(static_cast<double>(d[1] * d[1] + d[0] * d[0] + d[2] * d[2]));
        const int32_t n = static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(dist)) / 40.0f);
        if (static_cast<uint32_t>(active_ + n) < records_.size()) {
            if (n == 0) {
                place(static_cast<size_t>(active_), prev_, cur_);
                ++active_;
            } else {
                const float span = static_cast<float>(clock_ - prevClock_) / static_cast<float>(n);
                const float from = static_cast<float>(prevClock_);
                for (int32_t k = 0; k < n; ++k)
                    place(static_cast<size_t>(active_ + k), path_.at(static_cast<float>(k) * span + from),
                          path_.at(static_cast<float>(k + 1) * span + from));
                active_ += n;
            }
        }
        for (int32_t i = 0; i < active_; ++i) {
            State& s = state_[static_cast<size_t>(i)];
            ParticleRecord& r = records_[static_cast<size_t>(i)];
            const int32_t age = static_cast<int32_t>(static_cast<float>(s.counter - s.start));
            if (s.phase == 0) {
                setAlpha(r, static_cast<int32_t>(mapRange(0, static_cast<float>(alpha_), 0, static_cast<float>(in_), static_cast<float>(s.counter))));
                if (in_ < age) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                if (hold_ < age) s.start = s.counter, s.phase = out_ < 0 ? 3 : 2;
            } else if (s.phase == 2) {
                setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(alpha_), 0, 0, static_cast<float>(out_), static_cast<float>(age))));
                if (out_ < age) s.phase = 3, s.start = s.counter;
            }
            s.counter += frames;
        }
        prevClock_ = clock_;
        clock_ += frames;
        prev_ = cur_;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
    };

    void reset(size_t i) {
        ParticleRecord& r = records_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        r.scale = {size_, size_, 1};
        setAlpha(r, 0);
        state_[i] = State{};
    }

    void place(size_t i, const std::array<float, 3>& a, const std::array<float, 3>& b) {
        reset(i);
        ParticleRecord& r = records_[i];
        r.v[0].x = a[0], r.v[0].y = a[1], r.v[0].z = a[2];
        r.v[1].x = b[0], r.v[1].y = b[1], r.v[1].z = b[2];
        for (size_t k = 2; k < 4; ++k) r.v[k].x = focus_[0], r.v[k].y = focus_[1], r.v[k].z = focus_[2];
    }

    SplinePath path_;
    std::vector<State> state_;
    std::array<float, 3> focus_{}, cur_{}, prev_{};
    int32_t in_ = 0, hold_ = 0, out_ = 0, alpha_ = 0, active_ = 0, clock_ = 0, prevClock_ = 0;
    float size_ = 1;
};

// rubble.ppi (Happiness 2, 4 loads): debris thrown up off a line, x87 (VS2008) build.
// Init 0x100011b0, Update 0x10001320, spawn 0x10001530. Alpha blend; it does not read the plane
// fields. The base (0x10001900) gives every record a piece of its own.
//   f0-f2 -> f3-f5: the line (a two-point path, tangents 1, over 100); f6: size (percent, each
//   50-100% of it); f7: life (each 50-100% of it); f8: pieces started each update (whole).
// Piece k of n sits at 100 k / n along the line, takes a quarter of the texture (rand & 3) and a
// spin of 0.02-0.04 a frame either way, and runs a two-point path from there to 100-300 to the
// right, 600-1200 up and 50-100 nearer or further, over its life (tangents 0.55 of it). It fades
// in over the first tenth of its life and, at its end, stays where it is: nothing starts it again.
// Its x87 arithmetic runs in single precision (x87Note): float steps here; its path code orders
// its float steps differently from SplinePath's (a last bit's difference at most).
class Rubble : public ParticleEffect {
public:
    Rubble(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        line_.add({f[0], f[1], f[2]});
        line_.add({f[3], f[4], f[5]});
        line_.finish(100.0f, 1.0f);
        size_ = f[6] / 100.0f;
        life_ = f[7];
        amount_ = static_cast<int32_t>(f[8]);
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        active_ = std::min(active_ + static_cast<uint32_t>(amount_), static_cast<uint32_t>(records_.size()));
        for (uint32_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.phase == 0) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                r.rot[2] = s.spin + r.rot[2];
                const float in = s.path.duration / 10.0f;
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, in, static_cast<float>(s.counter))));
                if (s.path.duration < static_cast<float>(s.counter)) s.phase = 3, s.start = s.counter;
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        float spin = 0;
        SplinePath path;
    };

    // (rand & 1 ? 1 : -1), the sign the DLL picks with an integer.
    float sign() { return (rng_.next() & 1) != 0 ? 1.0f : -1.0f; }

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x10011d10
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pos[2] = 0;
        const auto& q = kQuarter[rng_.next() & 3];
        setUv(r, q[0], q[1], 0.5f, 0.5f);
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const float h = size_ * 0.5f;
        const float sz = rng_.unit() * h + h;
        r.scale = {sz, sz, 1};
        const float t = 100.0f / static_cast<float>(records_.size()) * static_cast<float>(i);
        r.pos = line_.at(t);
        r.rot = {0, 0, 0};
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;
        const float sg = sign();
        s.spin = (rng_.unit() * 0.02f + 0.02f) * sg;
        s.counter = 0, s.phase = 0;
        s.path = SplinePath{};
        s.path.add(r.pos);
        const float x = rng_.unit() * 200.0f + 100.0f + r.pos[0];
        const float y0 = r.pos[1];
        const float y = y0 - (rng_.unit() * 600.0f + 600.0f);
        const float sz2 = sign();
        const float z = (rng_.unit() * 50.0f + 50.0f) * sz2 + r.pos[2];
        s.path.add({x, y, z});
        const float half = life_ * 0.5f;
        const float life = rng_.unit() * half + half;
        s.path.finish(life, 0.55f * life);
    }

    SplinePath line_;
    std::vector<State> state_;
    float size_ = 0, life_ = 0;
    int32_t amount_ = 0;
    uint32_t active_ = 0;
};

// wc_thunder.ppi (Happiness 2, 3 loads): lightning flashing at a point. Init 0x100013e0, Update
// 0x10001510, spawn 0x10001ab0; ring's framework (groups of f0 records, a head and copies). Additive.
//   f0: records per group (1 in every load; the copies of more are not reimplemented); f1 f2: the
//   place; f3: size (percent, at least 0.001 as a scale); f5 f6: fade-in and fade-out frames. f4 is
//   kept and not read.
// A bolt takes one of the texture's four columns (the DLL's own CRT rand() & 3), a width of the
// size x (0.7 + rand / 2) and a height of three times the size, white at alpha 0. One more bolt
// starts every second update; each fades in and out at the place (moved with its plane) and is
// done. Once all of them are done the plugin stops its layer's updates (host +4: the layer's
// +0x344, which the update, 0x4fd420, tests), so the records stay as they were.
class WcThunder : public ParticleEffect {
public:
    struct Globals : PluginGlobals, CrtRand {};

    WcThunder(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        blend_ = 1;
        const auto& f = p.f;
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        place_ = {f[1], f[2]};
        size_ = f[3] / 100.0f;
        if (size_ < 0.001f) size_ = 0.001f;
        in_ = static_cast<int32_t>(f[5]), out_ = static_cast<int32_t>(f[6]);
        state_.resize(groups_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (stopped_) return;
        if ((++tick_ & 1) == 0) ++active_;
        active_ = std::min(active_, groups_);
        const auto move = planeMove();
        for (uint32_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t age = s.counter - s.start;
            r.pos[0] = place_[0], r.pos[1] = place_[1];
            if (s.phase == 0) {
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(in_), static_cast<float>(age))));
                if (in_ < age) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(out_), static_cast<float>(age))));
                if (out_ < age) s.phase = 9, s.start = s.counter, ++done_;
            }
            r.pos[0] = r.pos[0] + move[0];
            r.pos[1] = r.pos[1] + move[1];
            s.counter += frames;
        }
        if (groups_ <= done_) stopped_ = true;
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
    };

    void spawn(uint32_t i) {
        ParticleRecord& r = records_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const float u = static_cast<float>(dll_->rand() & 3) * 0.25f;  // .data 0x100140b0
        setUv(r, u, 0, 0.25f, 1.0f);
        r.pos = {place_[0], place_[1], 0};
        r.scale[0] = rng_.unit() * 0.5f * size_ + static_cast<float>(static_cast<double>(size_) * 0.7);  // x87Note
        r.scale[1] = size_ * 3.0f;
        r.scale[2] = 1;
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;
        state_[i] = State{};
        for (uint32_t k = 0; k < trail_; ++k) setAlpha(records_[groups_ + i * trail_ + k], 0);
    }

    Globals* dll_ = nullptr;
    std::vector<State> state_;
    uint32_t groups_ = 0, trail_ = 0, tick_ = 0, active_ = 0, done_ = 0;
    std::array<float, 2> place_{};
    float size_ = 1;
    int32_t in_ = 0, out_ = 0;
    bool stopped_ = false;
};

// protect.ppi's tables (Protect, below). .data 0x10019560: each face's corners (x, y, z), in the
// record's corner order.
constexpr float kProtectMesh[100][3] = {
    {-250, -250, -50}, {-150, -250, -25}, {-250, -150, -50}, {-150, -150, -25},
    {-150, -250, -25}, {-50, -250, 0}, {-150, -150, -25}, {-50, -150, 0},
    {-50, -250, 0}, {50, -250, 0}, {-50, -150, 0}, {50, -150, 0},
    {50, -250, 0}, {150, -250, -25}, {50, -150, 0}, {150, -150, -25},
    {150, -250, -25}, {250, -250, -50}, {150, -150, -25}, {250, -150, -50},
    {-250, -150, -50}, {-150, -150, -25}, {-250, -50, -50}, {-150, -50, -25},
    {-150, -150, -25}, {-50, -150, 0}, {-150, -50, -25}, {-50, -50, 0},
    {-50, -150, 0}, {50, -150, 0}, {-50, -50, 0}, {50, -50, 0},
    {50, -150, 0}, {150, -150, -25}, {50, -50, 0}, {150, -50, -25},
    {150, -150, -25}, {250, -150, -50}, {150, -50, -25}, {250, -50, -50},
    {-250, -50, -50}, {-150, -50, -25}, {-250, 50, -50}, {-150, 50, -25},
    {-150, -50, -25}, {-50, -50, 0}, {-150, 50, -25}, {-50, 50, 0},
    {-50, -50, 0}, {50, -50, 0}, {-50, 50, 0}, {50, 50, 0},
    {50, -50, 0}, {150, -50, -25}, {50, 50, 0}, {150, 50, -25},
    {150, -50, -25}, {250, -50, -50}, {150, 50, -25}, {250, 50, -50},
    {-250, 50, -50}, {-150, 50, -25}, {-250, 150, -50}, {-150, 150, -25},
    {-150, 50, -25}, {-50, 50, 0}, {-150, 150, -25}, {-50, 150, 0},
    {-50, 50, 0}, {50, 50, 0}, {-50, 150, 0}, {50, 150, 0},
    {50, 50, 0}, {150, 50, -25}, {50, 150, 0}, {150, 150, -25},
    {150, 50, -25}, {250, 50, -50}, {150, 150, -25}, {250, 150, -50},
    {-250, 150, -50}, {-150, 150, -25}, {-250, 250, -50}, {-150, 250, -25},
    {-150, 150, -25}, {-50, 150, 0}, {-150, 250, -25}, {-50, 250, 0},
    {-50, 150, 0}, {50, 150, 0}, {-50, 250, 0}, {50, 250, 0},
    {50, 150, 0}, {150, 150, -25}, {50, 250, 0}, {150, 250, -25},
    {150, 150, -25}, {250, 150, -50}, {150, 250, -25}, {250, 250, -50},
};
// .data 0x100190b0: each corner's texture coordinates and alpha (0-255).
constexpr float kProtectUv[100][3] = {
    {0, 0, 0}, {0.2f, 0, 0}, {0, 0.2f, 0}, {0.2f, 0.2f, 32},
    {0.2f, 0, 0}, {0.4f, 0, 0}, {0.2f, 0.2f, 32}, {0.4f, 0.2f, 128},
    {0.4f, 0, 0}, {0.6f, 0, 0}, {0.4f, 0.2f, 128}, {0.6f, 0.2f, 128},
    {0.6f, 0, 0}, {0.8f, 0, 0}, {0.6f, 0.2f, 128}, {0.8f, 0.2f, 32},
    {0.8f, 0, 0}, {1, 0, 0}, {0.8f, 0.2f, 32}, {1, 0.2f, 0},
    {0, 0.2f, 0}, {0.2f, 0.2f, 32}, {0, 0.4f, 0}, {0.2f, 0.4f, 128},
    {0.2f, 0.2f, 32}, {0.4f, 0.2f, 128}, {0.2f, 0.4f, 128}, {0.4f, 0.4f, 255},
    {0.4f, 0.2f, 128}, {0.6f, 0.2f, 128}, {0.4f, 0.4f, 255}, {0.6f, 0.4f, 255},
    {0.6f, 0.2f, 128}, {0.8f, 0.2f, 32}, {0.6f, 0.4f, 255}, {0.8f, 0.4f, 128},
    {0.8f, 0.2f, 32}, {1, 0.2f, 0}, {0.8f, 0.4f, 128}, {1, 0.4f, 0},
    {0, 0.4f, 0}, {0.2f, 0.4f, 128}, {0, 0.6f, 0}, {0.2f, 0.6f, 128},
    {0.2f, 0.4f, 128}, {0.4f, 0.4f, 255}, {0.2f, 0.6f, 128}, {0.4f, 0.6f, 255},
    {0.4f, 0.4f, 255}, {0.6f, 0.4f, 255}, {0.4f, 0.6f, 255}, {0.6f, 0.6f, 255},
    {0.6f, 0.4f, 255}, {0.8f, 0.4f, 255}, {0.6f, 0.6f, 128}, {0.8f, 0.6f, 128},
    {0.8f, 0.4f, 128}, {1, 0.4f, 0}, {0.8f, 0.6f, 128}, {1, 0.6f, 0},
    {0, 0.6f, 0}, {0.2f, 0.6f, 128}, {0, 0.8f, 0}, {0.2f, 0.8f, 32},
    {0.2f, 0.6f, 128}, {0.4f, 0.6f, 255}, {0.2f, 0.8f, 32}, {0.4f, 0.8f, 128},
    {0.4f, 0.6f, 255}, {0.6f, 0.6f, 255}, {0.4f, 0.8f, 128}, {0.6f, 0.8f, 128},
    {0.6f, 0.6f, 255}, {0.8f, 0.6f, 128}, {0.6f, 0.8f, 128}, {0.8f, 0.8f, 32},
    {0.8f, 0.6f, 128}, {1, 0.6f, 0}, {0.8f, 0.8f, 32}, {1, 0.8f, 0},
    {0, 0.8f, 0}, {0.2f, 0.8f, 32}, {0, 1, 0}, {0.2f, 1, 0},
    {0.2f, 0.8f, 32}, {0.4f, 0.8f, 128}, {0.2f, 1, 0}, {0.4f, 1, 0},
    {0.4f, 0.8f, 128}, {0.6f, 0.8f, 128}, {0.4f, 1, 0}, {0.6f, 1, 0},
    {0.6f, 0.8f, 128}, {0.8f, 0.8f, 32}, {0.6f, 1, 0}, {0.8f, 1, 0},
    {0.8f, 0.8f, 32}, {1, 0.8f, 0}, {0.8f, 1, 0}, {1, 1, 0},
};

// protect.ppi (Happiness 2, 3 loads at once: a barrier going up): a bowed shield of
// 5 x 5 faces. Init 0x10001ce0, Update 0x10001e70, spawn 0x100028e0; its own framework
// (0x10001120): groups of 26 records (count / 26), the first `groups` records the heads, then 25
// faces for each.
//   f0 f1: the shield's centre; f2: size (percent, at least 0.001 as a scale, x and y); f3 f4:
//   the texture's scroll in u and v (/ 10000, per update); f5: additive from the start when not 0.
// The faces' corners and texture coordinates are the DLL's tables (kProtectMesh: a 500 x 500 grid
// bowed back at the sides; kProtectUv: a fifth of the texture each way, and an alpha for each
// corner falling off to the edges). Each update puts every corner through the head's matrix (its
// scale, turn and position: 1, 0, 0) and then the layer's (the size, the head's +0x64 turn, which
// stays 0, then the centre), shades the face by its normal (y + x - z mapped from [-1, 1] onto
// 255-60, grey) and gives the corners the head's alpha times the table's. The head fades in over
// 60 frames, holds 5 and fades out over 60, going additive as the fade-out starts. The texture
// scrolls by the f3 / f4 speed eased to nothing over the head's first 65 frames (0x10002ba0), and
// the update asks for it to wrap (host +0x88: ADDRESSU and ADDRESSV = WRAP).
// The head's own record is not drawn (spawn sets its alpha to 0). The spawn puts the faces'
// corners through the face's matrix, which is still the framework's zeros (and it reads the
// head's corner fields, also zero), so they start at 0 until the first update.
class Protect : public ParticleEffect {
public:
    Protect(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        groups_ = static_cast<uint32_t>(std::max(0, p.count)) / 26;
        centre_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (0.001f > size_) size_ = 0.001f;
        speed_ = {f[3] / 10000.0f, f[4] / 10000.0f};
        if (static_cast<int32_t>(f[5]) != 0) blend_ = 1;
        heads_.resize(groups_);
        faces_.resize(static_cast<size_t>(groups_) * 25);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        samplerStates_.clear();  // the engine's, before the update (0x4fd420)
        samplerStates_.push_back({1, 1});
        samplerStates_.push_back({2, 1});
        const float ft = static_cast<float>(frames);
        for (uint32_t i = 0; i < groups_; ++i) {
            Head& h = heads_[i];
            const float t = h.counter - h.start;
            if (h.phase == 0) {
                h.alpha = static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(kIn), t));
                if (t > static_cast<float>(kIn)) h.phase = 1, h.start = h.counter;
            } else if (h.phase == 1) {
                if (t > static_cast<float>(kHold)) h.phase = 2, h.start = h.counter, blend_ = 1;
            } else if (h.phase == 2) {
                blend_ = 1;
                h.alpha = static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(kOut), t));
                if (t > static_cast<float>(kOut)) h.phase = 9, h.start = h.counter;
            }
            // Each matrix starts as the identity, takes the scale (M = S M), the three turns and
            // then its position added to the fourth row.
            Mat layer = mul(scaling(size_, size_, 1.0f), identity());
            plugRotate(layer, 0, 0, 0);
            layer[12] = layer[12] + centre_[0], layer[13] = layer[13] + centre_[1], layer[14] = layer[14] + centre_[2];
            Mat head = mul(scaling(1, 1, 1), identity());
            plugRotate(head, 0, 0, 0);
            head[12] = head[12] + 0.0f, head[13] = head[13] + -0.0f, head[14] = head[14] + 0.0f;
            const Mat world = mul(head, layer);  // 0x100012a0(out, layer, head)
            for (uint32_t k = 0; k < 25; ++k) {
                Face& fc = faces_[static_cast<size_t>(i) * 25 + k];
                ParticleRecord& r = records_[groups_ + static_cast<size_t>(i) * 25 + k];
                for (uint32_t j = 0; j < 4; ++j) {
                    const float* c = kProtectMesh[k * 4 + j];
                    const auto v = transform({c[0], c[1], c[2]}, world);
                    r.v[j].x = v[0], r.v[j].y = v[1], r.v[j].z = v[2];
                }
                const auto& a = r.v[0];
                const auto& b = r.v[1];
                const auto& c = r.v[2];
                float nx = (c.z - a.z) * (b.y - a.y) - (c.y - a.y) * (b.z - a.z);
                float ny = (c.x - a.x) * (b.z - a.z) - (c.z - a.z) * (b.x - a.x);
                float nz = (c.y - a.y) * (b.x - a.x) - (c.x - a.x) * (b.y - a.y);
                const float len = static_cast<float>(std::sqrt(static_cast<double>(ny * ny + nx * nx + nz * nz)));
                if (0.0f < len) nx = nx / len, nz = nz / len, ny = ny / len;
                const float shade = mapRange(255, 60, -1, 1, (ny + nx) - nz * 1.0f);
                fc.u = fc.u + ease(speed_[0], h.counter);
                fc.v = fc.v + ease(speed_[1], h.counter);
                for (uint32_t j = 0; j < 4; ++j) {
                    auto& v = r.v[j];
                    v.r = v.g = v.b = static_cast<uint8_t>(static_cast<int32_t>(shade));
                    v.alpha = static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(h.alpha) / 255.0f * kProtectUv[k * 4 + j][2]));
                }
                setUv(r, fc.u, fc.v, 0.2f, 0.2f);
                fc.counter = ft + fc.counter;  // not read
            }
            h.counter = ft + h.counter;
        }
    }

private:
    static constexpr int32_t kIn = 60, kHold = 5, kOut = 60, kSpan = 65;  // Init: +0xa8 to +0xb4

    struct Head {
        float counter = 0, start = 0;
        int32_t phase = 0, alpha = 0;
    };
    struct Face {
        float u = 0, v = 0, counter = 0;
    };

    // 0x10002ba0's x part (the update reads no other): the speed `from` eased to 0 (`to`) over
    // kSpan frames of the head's clock (at least 1): the cosine of 90 degrees mapped down to 0.
    static float ease(float from, float counter) {
        const float t = 1.0f > counter ? 1.0f : counter;
        if (static_cast<float>(static_cast<double>(static_cast<uint32_t>(t))) >= static_cast<float>(kSpan)) return 0.0f;
        const float a = plugRad(90), b = plugRad(0);
        const float m = mapRange(a, b, 0, static_cast<float>(kSpan), t);
        return (plugCos(m) - plugCos(a)) * (0.0f - from) + from + 0.0f;
    }

    void spawn(uint32_t i) {
        setAlpha(records_[i], 0);
        heads_[i] = Head{};
        for (uint32_t k = 0; k < 25; ++k) {
            Face& fc = faces_[static_cast<size_t>(i) * 25 + k];
            ParticleRecord& r = records_[groups_ + static_cast<size_t>(i) * 25 + k];
            fc.u = kProtectUv[k * 4][0], fc.v = kProtectUv[k * 4][1];
            for (uint32_t j = 0; j < 4; ++j) {
                auto& v = r.v[j];
                v.x = v.y = v.z = 0;
                v.u = kProtectUv[k * 4 + j][0], v.v = kProtectUv[k * 4 + j][1];
                v.r = v.g = v.b = 255;
                v.alpha = static_cast<uint8_t>(static_cast<int32_t>(kProtectUv[k * 4 + j][2]));
            }
        }
    }

    uint32_t groups_ = 0;
    std::array<float, 3> centre_{};
    float size_ = 1;
    std::array<float, 2> speed_{};
    std::vector<Head> heads_;
    std::vector<Face> faces_;
};


// wc_roundz.ppi and wc_roundy.ppi (Happiness 2, one load each: a whirlwind of
// wind strips going up as Karen starts her spell): two builds of protect's framework (0x10001120:
// the first `groups` records the heads, then the faces of each). wc_roundz: groups of count / 6,
// 5 faces each; wc_roundy: count / 5, 4 faces. Init 0x10001ce0, Update 0x10001e30, spawn
// 0x10002780 in both. Additive (Init sets the blend).
//   f0 f1: the whirl's centre; f2: size (percent, at least 0.001), x and y.
// A head is a strip of faces from the DLL's tables (kRoundzMesh: five panels of a 250-wide band
// whose ends turn back, each a fifth of the texture across; kRoundyMesh: a 200 x 100 band bent
// round in four panels, the texture's middle 0.6 across and 0.05-0.95 down). The spawn sets a
// turn about y (0 to 2 pi), u = 0-100 and v = u / 100, and:
//   wc_roundz: position (0, -u, v x 100), turned -pi/2 about x, scaled (0.6, 1, 1), spinning by
//   (0.01 to 0.06) x (v / 2 + 0.5) an update.
//   wc_roundy: position (v x 100, -u, 0), turned (-0.1, pi, pi/2 - 0.5), scaled v x 0.3 + 0.7,
//   spinning by (0.01 to 0.11) x (v / 2 + 0.5).
// and fade-in, hold and fade-out spans of 10, 5 and 40 frames each +-50%. Each update grows the
// head's x by 1.001 and its y by 1.005 an update while it fades in (to 200; wc_roundy 100) and
// holds, then x by 1.005 and y by 1.001 while it fades out, the spin slowing by 0.98; it adds the
// spin to its turn and, past the fade-out, starts again. Every face's corners go through the
// head's matrix (scale, turn, position) and then the layer's (the size, the head's spinning turn,
// the centre), are shaded by the face's normal (y + x - z mapped from [-1, 1] onto 255-60, grey)
// and take the head's alpha (its low byte).
// As in protect, the head's record is not drawn, and the spawn puts the faces' corners through
// the face's last matrix while reading the head's corner fields (zero), so a new face sits at the
// old matrix's position until the update that follows. The update also works out the head's
// distance from (640, 360, 0) (0x10001b00) and the layer position less half the screen, and reads
// neither.
constexpr float kRoundzMesh[20][3] = {
    {-125, 5, 0}, {-75, -15, 0}, {-115, 55, 0}, {-75, 35, 0}, {-75, -15, 0}, {-25, -25, 0}, {-75, 35, 0},
    {-25, 25, 0}, {-25, -25, 0}, {25, -25, 0},  {-25, 25, 0}, {25, 25, 0},   {25, -25, 0},  {75, -15, 0},
    {25, 25, 0},  {75, 35, 0},   {75, -15, 0},  {125, 5, 0},  {75, 35, 0},   {115, 55, 0}};  // .data 0x100181a0
constexpr float kRoundzUv[20][2] = {{0, 0},    {0.2f, 0}, {0, 1},    {0.2f, 1}, {0.2f, 0}, {0.4f, 0}, {0.2f, 1},
                                    {0.4f, 1}, {0.4f, 0}, {0.6f, 0}, {0.4f, 1}, {0.6f, 1}, {0.6f, 0}, {0.8f, 0},
                                    {0.6f, 1}, {0.8f, 1}, {0.8f, 0}, {1, 0},    {0.8f, 1}, {1, 1}};  // 0x100180b0
constexpr float kRoundyMesh[16][3] = {{-100, 45, -51}, {100, 45, -51}, {-100, 50, -33}, {100, 50, -33},
                                      {-100, 50, -33}, {100, 50, -33}, {-100, 52, 0},   {100, 52, 0},
                                      {-100, 52, 0},   {100, 52, 0},   {-100, 50, 33},  {100, 50, 33},
                                      {-100, 50, 33},  {100, 50, 33},  {-100, 45, 51},  {100, 45, 51}};  // 0x10018290
constexpr float kRoundyUv[16][2] = {{0.2f, 0.05f}, {0.8f, 0.05f}, {0.2f, 0.25f}, {0.8f, 0.25f},
                                    {0.2f, 0.25f}, {0.8f, 0.25f}, {0.2f, 0.5f},  {0.8f, 0.5f},
                                    {0.2f, 0.5f},  {0.8f, 0.5f},  {0.2f, 0.75f}, {0.8f, 0.75f},
                                    {0.2f, 0.75f}, {0.8f, 0.75f}, {0.2f, 0.95f}, {0.8f, 0.95f}};  // 0x100180b0

class WcRound : public ParticleEffect {
public:
    WcRound(const ParticleParams& p, uint32_t seed, bool roundy) : ParticleEffect(p, seed), roundy_(roundy) {
        const auto& f = p.f;
        faces_ = roundy ? 4 : 5;
        groups_ = static_cast<uint32_t>(std::max(0, p.count)) / (faces_ + 1);
        centre_ = {f[0], f[1], 0};
        size_ = f[2] / 100.0f;
        if (0.001f > size_) size_ = 0.001f;
        blend_ = 1;
        heads_.resize(groups_);
        faceMats_.resize(static_cast<size_t>(groups_) * faces_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const float ft = static_cast<float>(frames);
        const float peak = roundy_ ? 100.0f : 200.0f;
        for (uint32_t i = 0; i < groups_; ++i) {
            Head& h = heads_[i];
            const float t = h.counter - h.start;
            if (h.phase == 0) {
                h.pos[0] = h.pos[0] * 1.001f, h.pos[1] = h.pos[1] * 1.005f;
                h.alpha = static_cast<int32_t>(mapRange(0, peak, 0, static_cast<float>(h.in), t));
                h.turn[1] = h.spin + h.turn[1];
                if (static_cast<float>(h.in) < t) h.phase = 1, h.start = h.counter;
            } else if (h.phase == 1) {
                h.pos[0] = h.pos[0] * 1.001f, h.pos[1] = h.pos[1] * 1.005f;
                h.turn[1] = h.spin + h.turn[1];
                if (static_cast<float>(h.hold) < t) h.phase = 2, h.start = h.counter;
            } else if (h.phase == 2) {
                h.pos[0] = h.pos[0] * 1.005f, h.pos[1] = h.pos[1] * 1.001f;
                h.alpha = static_cast<int32_t>(mapRange(peak, 0, 0, static_cast<float>(h.out), t));
                h.spin = h.spin * 0.98f;
                h.turn[1] = h.turn[1] + h.spin;
                if (static_cast<float>(h.out) < t) h.phase = 9, h.start = h.counter;
            } else {
                spawn(i);
            }
            Mat layer = mul(scaling(size_, size_, 1.0f), identity());
            plugRotate(layer, h.turn[0], h.turn[1], h.turn[2]);
            layer[12] = layer[12] + centre_[0], layer[13] = layer[13] + centre_[1], layer[14] = layer[14] + centre_[2];
            Mat head = mul(scaling(h.scale[0], h.scale[1], h.scale[2]), identity());
            plugRotate(head, h.rot[0], h.rot[1], h.rot[2]);
            head[12] = head[12] + h.pos[0], head[13] = head[13] + h.pos[1], head[14] = head[14] + h.pos[2];
            const Mat world = mul(head, layer);  // 0x100012a0(out, layer, head)
            const auto alpha = static_cast<uint8_t>(h.alpha);
            for (uint32_t k = 0; k < faces_; ++k) {
                ParticleRecord& r = records_[groups_ + static_cast<size_t>(i) * faces_ + k];
                for (auto& v : r.v) v.alpha = alpha;
                faceMats_[static_cast<size_t>(i) * faces_ + k] = world;
                for (uint32_t j = 0; j < 4; ++j) {
                    const float* c = roundy_ ? kRoundyMesh[k * 4 + j] : kRoundzMesh[k * 4 + j];
                    const auto v = transform({c[0], c[1], c[2]}, world);
                    r.v[j].x = v[0], r.v[j].y = v[1], r.v[j].z = v[2];
                }
                const auto& a = r.v[0];
                const auto& b = r.v[1];
                const auto& c = r.v[2];
                float nx = (c.z - a.z) * (b.y - a.y) - (c.y - a.y) * (b.z - a.z);
                float ny = (c.x - a.x) * (b.z - a.z) - (c.z - a.z) * (b.x - a.x);
                float nz = (c.y - a.y) * (b.x - a.x) - (c.x - a.x) * (b.y - a.y);
                const float len = static_cast<float>(std::sqrt(static_cast<double>(ny * ny + nx * nx + nz * nz)));
                if (0.0f < len) nx = nx / len, nz = nz / len, ny = ny / len;
                const auto shade = static_cast<uint8_t>(static_cast<int32_t>(mapRange(255, 60, -1, 1, (ny + nx) - nz * 1.0f)));
                for (auto& v : r.v) v.r = v.g = v.b = shade;
            }
            h.counter = ft + h.counter;
        }
    }

private:
    struct Head {
        std::array<float, 3> pos{}, scale{1, 1, 1}, rot{}, turn{};
        float spin = 0, counter = 0, start = 0;
        int32_t phase = 0, alpha = 0, in = 0, hold = 0, out = 0;
    };

    void spawn(uint32_t i) {
        setAlpha(records_[i], 0);
        Head& h = heads_[i];  // the alpha (+0x84) stays
        h.turn[0] = 0;
        const float a = rng_.unit() * 3.14159274f;
        h.turn[2] = 0;
        h.turn[1] = a + a;
        const float u = rng_.unit() * 100.0f;
        const float v = u / 100.0f;
        if (!roundy_) {
            h.pos = {0, -u, v * 100.0f};
            h.rot = {-1.57079637f, 0, 0};
            h.scale = {0.6f, 1, 1};
            h.spin = (rng_.unit() * 0.05f + 0.01f) * (v * 0.5f + 0.5f);
        } else {
            const float s = v * 0.3f + 0.7f;
            h.pos = {v * 100.0f, -u, 0};
            h.rot = {-0.1f, 3.14159274f, 1.07079637f};
            h.scale = {s, s, s};
            h.spin = (rng_.unit() * 0.1f + 0.01f) * (v * 0.5f + 0.5f);
        }
        h.counter = 0, h.start = 0, h.phase = 0;
        h.in = static_cast<int32_t>((rng_.unit() - 0.5f) * 10.0f + 10.0f);  // Init: +0xa0 to +0xa8
        h.hold = static_cast<int32_t>((rng_.unit() - 0.5f) * 5.0f + 5.0f);
        h.out = static_cast<int32_t>((rng_.unit() - 0.5f) * 40.0f + 40.0f);
        for (uint32_t k = 0; k < faces_; ++k) {
            const Mat& m = faceMats_[static_cast<size_t>(i) * faces_ + k];
            ParticleRecord& r = records_[groups_ + static_cast<size_t>(i) * faces_ + k];
            for (uint32_t j = 0; j < 4; ++j) {
                const float* uv = roundy_ ? kRoundyUv[k * 4 + j] : kRoundzUv[k * 4 + j];
                auto& vx = r.v[j];
                vx.x = m[12], vx.y = m[13], vx.z = m[14];
                vx.u = uv[0], vx.v = uv[1];
            }
        }
    }

    bool roundy_ = false;
    uint32_t groups_ = 0, faces_ = 5;
    std::array<float, 3> centre_{};
    float size_ = 1;
    std::vector<Head> heads_;
    std::vector<Mat> faceMats_;
};


// star.ppi and o_150102.ppi (Happiness 2: o_150102 3 loads, ice shards bursting;
// star 3 in a sample scene): seven streams of sparks fanned out from a point, one colour each,
// x87 (VS2008) builds of one source. star: Init 0x100010d0, Update 0x100011c0, spawn 0x10001cb0;
// o_150102 0x10 lower. cs2ParticleBaseEx's framework: groups of count / 7 records (so seven
// groups), a head and the rest its sparks. Additive.
//   f0 f1: the start; f2 f3: the end; f4: size (percent); f5: the heads' time to the end
//   (frames); star only: f6 the fan's angle (degrees; o_150102 140) and f7 the sparks a head
//   starts in one update, less one (o_150102 16).
// Each head (not drawn) takes the line from start to end turned by its share of the fan
// (mapRange(-angle / 2, angle / 2, 0, 100, group x 16.67)) and a path (SplinePath, tangents as
// long as the whole path, over f5 frames) out to the end's distance and three points just past it
// (x 1.01 each), so it slows to a stop. While it moves, each update starts sparks that are not
// running: each at a random point between the head's last two places, scattered within +-100 x
// the head's size (z 0 to 120 x), with a path of its own (over 200 frames; o_150102 120) to 50-100
// units back along the head's move (forward along its path while it has not moved) turned up to 30
// degrees either way, a quarter of the texture (.data 0x10012d10: one of its two lower quarters,
// the right one 3 times in 4, and drawn 0.7 as large), the group's colour (.data 0x10012d30), the
// head's size, a spin and a random alpha (0-199). A running spark spins by up to 0.04, shrinks by
// 0.99 an update, fades from its alpha to 0 over its life, flickering between 40% and 100% of
// that, and then is free to start again. The heads shrink by 0.99 an update (o_150102 0.94); once
// at the end they wait 60 frames (star: from arriving; o_150102: their clock) and then stop.
// Its x87 arithmetic runs in single precision (x87Note): float steps here, a double constant
// taking part at its own value; its path code orders its float steps differently from
// SplinePath's (a last bit's difference at most).
class Star : public ParticleEffect {
public:
    Star(const ParticleParams& p, uint32_t seed, bool o150102) : ParticleEffect(p, seed), o150102_(o150102) {
        blend_ = 1;
        const auto& f = p.f;
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        const uint32_t size = count / 7;
        groups_ = size != 0 ? count / size : count;
        sparks_ = size != 0 ? size - 1 : 0;
        start_ = {f[0], f[1], 0};
        end_ = {f[2], f[3], 0};
        size_ = f[4] / 100.0f;
        duration_ = static_cast<int32_t>(f[5]);
        spread_ = o150102 ? 140.0f : f[6];
        limit_ = o150102 ? 16 : static_cast<int32_t>(f[7]);
        heads_.resize(groups_);
        sparkState_.resize(static_cast<size_t>(groups_) * sparks_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const int32_t life = o150102_ ? 120 : 200;
        for (uint32_t i = 0; i < groups_; ++i) {
            Head& s = heads_[i];
            ParticleRecord& h = records_[i];
            if (s.phase == 0) {
                h.pos = s.path.at(static_cast<float>(s.counter));
                if (duration_ < s.counter) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                const int32_t t = o150102_ ? s.counter : s.counter - s.start;
                if (static_cast<double>(t) > 60.0) s.phase = 2, s.start = s.counter;
            }
            s.size = s.size * (o150102_ ? 0.94f : 0.99f);
            int32_t started = 0;
            for (uint32_t k = 0; k < sparks_ && s.phase == 0; ++k) {
                if (spark(i, k).phase != 0) continue;
                emit(i, k);
                if (++started > limit_) break;
            }
            for (uint32_t k = 0; k < sparks_; ++k) {
                Spark& f = spark(i, k);
                if (f.phase == 0) continue;
                ParticleRecord& r = sparkRecord(i, k);
                r.pos = f.path.at(static_cast<float>(f.counter));
                // star's 0.04f; o_150102's a double 0.04.
                f.spin = (o150102_ ? static_cast<float>(static_cast<double>(rng_.unit()) * 0.04) : rng_.unit() * 0.04f) + f.spin;
                r.rot[2] = f.spin;
                r.scale[0] = r.scale[0] * 0.99f;
                r.scale[1] = 0.99f * r.scale[1];
                const int32_t a = static_cast<int32_t>(mapRange(static_cast<float>(f.alpha), 0, 0, static_cast<float>(life), static_cast<float>(f.counter)));
                const float fa = static_cast<float>(a);
                setAlpha(r, static_cast<int32_t>(rng_.unit() * 0.6f * fa + fa * 0.4f));
                f.counter += frames;
                if (f.counter >= life) f.counter = 0, f.phase = 0, setAlpha(r, 0);
            }
            s.prev = h.pos;
            s.counter += frames;
        }
    }

private:
    using V3 = std::array<float, 3>;
    struct Head {
        int32_t counter = 0, start = 0, phase = 0;
        float size = 0;
        V3 prev{};
        SplinePath path;
    };
    struct Spark {
        int32_t counter = 0, phase = 0;
        float spin = 0;
        uint8_t alpha = 0;
        SplinePath path;
    };

    Spark& spark(uint32_t i, uint32_t k) { return sparkState_[static_cast<size_t>(i) * sparks_ + k]; }
    ParticleRecord& sparkRecord(uint32_t i, uint32_t k) { return records_[groups_ + static_cast<size_t>(i) * sparks_ + k]; }

    // The x87 builds' helpers, float steps (x87Note): |v| as (y y + x x) + z z and the CRT's sqrt
    // (0x10002ed0); a unit vector, each part divided by it, left as it is at length 0; a distance
    // as (x x + y y) + z z (0x10002f40); a turn about z of the angle kept to three decimals
    // (0x100026f0: (int)(a x 1000) / 1000, plugAngle), the vector through it.
    static float length(const V3& v) {
        const float sq = v[1] * v[1] + v[0] * v[0] + v[2] * v[2];
        return static_cast<float>(std::sqrt(static_cast<double>(sq)));
    }
    static V3 unit(V3 v, float len) {
        if (0.0f < len || std::isnan(len)) {
            for (float& c : v) c = c / len;
        }
        return v;
    }
    static float distance(const V3& a, const V3& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        const float sq = dx * dx + dy * dy + dz * dz;
        return static_cast<float>(std::sqrt(static_cast<double>(sq)));
    }
    static V3 turnZ(const V3& v, float angle) {
        const float q = plugAngle(angle);
        const Mat m = rotationZ(plugCos(q), plugSin(q));
        auto row = [&](int a, int b, int c, int d, float x, float y, float z) {
            return m[a] * x + m[b] * y + m[c] * z + m[d];
        };
        return {row(0, 4, 8, 12, v[0], v[1], v[2]), row(5, 1, 9, 13, v[1], v[0], v[2]), row(2, 6, 10, 14, v[0], v[1], v[2])};
    }
    static V3 along(const V3& from, const V3& d, float len) {
        return {from[0] + d[0] * len, from[1] + d[1] * len, from[2] + d[2] * len};
    }

    void spawn(uint32_t i) {
        static constexpr float kCell[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x10012d10
        static constexpr uint8_t kColour[7][3] = {  // .data 0x10012d30: b, g, r
            {240, 240, 240}, {10, 255, 255}, {255, 10, 255}, {255, 255, 10}, {10, 10, 255}, {10, 255, 10}, {255, 10, 10}};
        ParticleRecord& h = records_[i];
        Head& s = heads_[i];
        setAlpha(h, 0);
        h.pos = {start_[0], start_[1], 0};
        s.path = SplinePath{};
        s.path.add(start_);
        const V3 d = SplinePath::sub(end_, start_);
        const float a = plugRad(spread_);
        const float m = mapRange(-a * 0.5f, a * 0.5f, 0, 100, static_cast<float>(i) * 16.666666f);
        V3 dir = turnZ(d, m);
        dir = unit(dir, length(dir));
        float len = distance(start_, end_);
        s.path.add(along(start_, dir, len));
        for (int k = 0; k < 3; ++k) {
            len = len * 1.01f;
            s.path.add(along(start_, dir, len));
        }
        s.path.finish(static_cast<float>(duration_), s.path.total);
        s.size = size_;
        s.phase = s.counter = s.start = 0;
        s.prev = h.pos;
        const float px = params_.texW * 0.5f, py = 0.5f * params_.texH;
        for (uint32_t k = 0; k < sparks_; ++k) {
            Spark& f = spark(i, k);
            ParticleRecord& r = sparkRecord(i, k);
            f.counter = f.phase = 0;
            f.spin = rng_.unit() * 3.14159274f;
            const uint32_t cell = std::min<uint32_t>((rng_.next() & 3) + 2, 3);
            setUv(r, kCell[cell][0], kCell[cell][1], 0.5f, 0.5f);
            r.pivot[0] = px, r.pivot[1] = py;
            for (auto& v : r.v) v.b = kColour[i % 7][0], v.g = kColour[i % 7][1], v.r = kColour[i % 7][2], v.alpha = 0;
        }
    }

    void emit(uint32_t i, uint32_t k) {
        Head& s = heads_[i];
        const ParticleRecord& h = records_[i];
        Spark& f = spark(i, k);
        ParticleRecord& r = sparkRecord(i, k);
        const float x0 = mapRange(s.prev[0], h.pos[0], 0, 1, rng_.unit());
        const float y0 = mapRange(s.prev[1], h.pos[1], 0, 1, rng_.unit());
        const float twice = s.size + s.size;
        r.pos[0] = (rng_.unit() * 100.0f - 50.0f) * twice + x0;
        r.pos[1] = (rng_.unit() * 100.0f - 50.0f) * twice + y0;
        r.pos[2] = rng_.unit() * 60.0f * twice + h.pos[2];
        r.rot[2] = h.rot[2];
        f.path = SplinePath{};
        f.path.add(r.pos);
        V3 v = SplinePath::sub(s.prev, h.pos);
        if (length(v) < 0.001f) v = SplinePath::sub(s.path.at(static_cast<float>(f.counter + 1)), s.path.at(static_cast<float>(f.counter)));
        v = unit(v, length(v));
        const float a = plugRad(30);
        const float m = mapRange(-a, a, 0, 100, rng_.unit() * 100.0f);
        v = turnZ(v, m);
        v = unit(v, length(v));
        const float len = rng_.unit() * 50.0f + 50.0f;
        f.path.add(along(h.pos, v, len));
        f.path.finish(o150102_ ? 120.0f : 200.0f, f.path.total / 10.0f);
        r.scale = h.scale;
        if (r.v[0].u > 0.4f) {
            r.scale[0] = s.size * 0.7f;
            r.scale[1] = 0.7f * s.size;
        } else {
            r.scale[0] = r.scale[1] = s.size;
        }
        f.spin = rng_.unit() * 3.14159274f;
        f.alpha = static_cast<uint8_t>(rng_.next() % 200);
        f.phase = 1;
    }

    bool o150102_;
    uint32_t groups_ = 0, sparks_ = 0;
    V3 start_{}, end_{};
    float size_ = 0, spread_ = 0;
    int32_t duration_ = 0, limit_ = 0;
    std::vector<Head> heads_;
    std::vector<Spark> sparkState_;
};


// exprode.ppi (Happiness 2, 3 loads, and the sample scene's three sizes): a burst of smoke, x87
// (VS2008) build. Init 0x10001070, Update 0x10001160, spawn 0x10001660; every record a puff of
// its own (0x100018a0). Alpha blend.
//   f0 f1: the centre; f2: the spread (each puff within f2 / 2 of the centre, x and y); f3: size
//   (percent, up to twice that at random); f4 f5 f6: fade-in, hold and fade-out frames (each at
//   least 1).
// Ten more puffs join each update, counted in a DLL global (.data 0x1000fe18, 1 when the DLL
// loads, shared by layers running exprode at once, capped at this layer's count). A puff rises by
// rand x 5 / f4 an update while it fades in, rand x 5 / f5 while it holds, then by rand x 300 / f6
// while it spins (rand x 0.3), shrinks to a tenth and fades out. Its texture is a 5 x 5 sheet
// whose cells it shows in turn over the whole f4 + f5 + f6 frames (the host's round of the clock
// mapped onto 0-24); the cells' corners (.data 0x1000fd10) are column x 0.2f and row x 0.2f, all
// 25 checked.
class Exprode : public ParticleEffect {
public:
    struct Globals : PluginGlobals {
        uint32_t active = 1;
    };

    Exprode(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        const auto& f = p.f;
        centre_ = {f[0], f[1]};
        spread_ = f[2];
        size_ = f[3] / 100.0f;
        in_ = f[4], hold_ = f[5], out_ = f[6];
        if (1.0f > in_) in_ = 1.0f;
        if (1.0f > hold_) hold_ = 1.0f;
        if (1.0f > out_) out_ = 1.0f;
        total_ = hold_ + in_ + out_;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const uint32_t active = dll_->active + 10;
        dll_->active = active;
        if (records_.size() < active) dll_->active = static_cast<uint32_t>(records_.size());
        for (uint32_t i = 0; i < dll_->active; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter - s.start);
            if (s.phase == 0) {
                rise(r, rng_.unit() * (5.0f / in_));
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, in_, static_cast<float>(s.counter))));
                frame(s, r);
                if (in_ < static_cast<float>(s.counter)) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                rise(r, rng_.unit() * (5.0f / hold_));
                frame(s, r);
                if (hold_ < t) s.phase = 2, s.start = s.counter;
            } else if (s.phase == 2) {
                rise(r, rng_.unit() * 300.0f / out_);
                r.rot[2] = rng_.unit() * 0.3f + r.rot[2];
                const float sc = mapRange(s.size, s.size / 10.0f, 0, out_, t);
                r.scale[0] = r.scale[1] = sc;
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, out_, t)));
                frame(s, r);
                if (out_ < t) s.phase = 10, s.start = s.counter, setAlpha(r, 0);
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0, frame = 0;
        float size = 0;
    };

    static void rise(ParticleRecord& r, float by) { r.pos[1] = r.pos[1] - by; }

    // The sheet's cell for the clock: the host's round (+0x50, floor(x + 0.5)) of 0-24 over the
    // puff's whole time.
    void frame(State& s, ParticleRecord& r) const {
        s.frame = static_cast<int32_t>(std::floor(mapRange(0, 24, 0, total_, static_cast<float>(s.counter)) + 0.5f));
        setUv(r, static_cast<float>(s.frame % 5) * 0.2f, static_cast<float>(s.frame / 5) * 0.2f, 0.2f, 0.2f);
    }

    void spawn(size_t i) {
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f;
        r.pivot[1] = 0.5f * params_.texH;
        setUv(r, 0, 0, 0.2f, 0.2f);
        s.size = rng_.unit() * size_ + size_;
        r.scale = {s.size, s.size, 1};
        setAlpha(r, 0);
        const float half = spread_ * 0.5f;
        r.pos[0] = rng_.unit() * spread_ - half + centre_[0];
        r.pos[1] = rng_.unit() * spread_ - half + centre_[1];
        r.pos[2] = 0;
        s.phase = s.counter = s.start = 0;
    }

    Globals* dll_ = nullptr;
    std::vector<State> state_;
    std::array<float, 2> centre_{};
    float spread_ = 0, size_ = 0, in_ = 1, hold_ = 1, out_ = 1, total_ = 3;
};


// cmnptcl.ppi (Happiness 2, 3 loads): an emitter from a Particle Designer file, the
// Cocos2d particle system reworked to the layer clock. Init 0x10002ab0, Update 0x10003850, a
// particle's set-up 0x10004310; every record a particle of its own (0x10001190). SSE build.
//   f0 f1: the file, ptcl<f0><f1, three digits>.plist, read through the host (+0x80) from the
//   game's archives; f2: when not 0 every particle starts at the first emission.
// Init walks the plist's dict in order (boost's property tree), each <key> looked up in a map of
// its 52 names (0x10004d10) and the next node read as its value, converted to the layer's units:
// seconds x 60 (frames), speeds / 60, accelerations / 3600 (radial / 360000), turns per second
// x -1 / 60, sizes / 30, colours x 255, the angle and gravity's y x -1 and sourcePositiony from
// the screen's height (y runs down). Every particle is set up at Init (pivot half the texture,
// hidden), and emitting one is counting it: total / life particles a frame (the record count
// over particleLifespan), until the count is reached. A particle's life, place, colours, size,
// turns and direction each take (rand x 2 - 1) x their variance; colours and their per-frame
// change are bytes (the change truncated, then added modulo 256 each update). In gravity mode
// (all three files) its direction gains (pos - source) x radial accel., a tangent x tangential
// accel. (worked from the direction and the set-up's radial unit, 0x10003b7a) and gravity; in
// radial mode it circles the source. A particle whose life runs out is set up again and the last
// one moved into its place (records and states, as Cocos does), or, once duration frames have
// passed, hidden; when all are hidden the layer stops. Records then take the layer plane's move.
class Cmnptcl : public ParticleEffect {
public:
    Cmnptcl(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        state_.resize(count);
        all_ = static_cast<int32_t>(p.f[2]);
        char name[64];
        std::snprintf(name, sizeof name, "ptcl%d%03d.plist", static_cast<int32_t>(p.f[0]), static_cast<int32_t>(p.f[1]));
        const std::string text = p.readFile ? p.readFile(name) : std::string();
        params_.readFile = nullptr;
        if (text.empty()) return;  // the host's read gave 0: Init does nothing more
        auto root = parseXml(text);
        const XmlNode* dict = root ? root->find("plist/dict") : nullptr;
        if (!dict) return;
        int32_t pending = kNone;
        for (const auto& node : dict->children) {
            if (pending >= kNone || pending == 0x32 || pending == 0x33) {
                pending = key(node->text);
                continue;
            }
            value(pending, node->text);
            pending = kNone;
        }
        if (emitterType_ == 1) {
            radial_ = {maxRadius_, maxRadiusVar_, minRadius_, 0.0f, rotatePerSecond_, rotatePerSecondVar_};
        } else {
            gravity_ = {gravityX_, gravityY_, 0};
            speedG_ = speed_, speedVarG_ = speedVar_;
            radialG_ = radialAccel_, radialVarG_ = radialAccelVar_;
            tanG_ = tanAccel_, tanVarG_ = tanAccelVar_;
        }
        rate_ = static_cast<float>(static_cast<double>(count)) / life_;
        if (blendDst_ == 1) blend_ = 1;
        half_ = {p.screenW * 0.5f, p.screenH * 0.5f};
        for (uint32_t i = 0; i < count; ++i) setUp(i);
        active_ = true;
    }

    void step(int32_t frames) override {
        if (records_.empty() || !active_) return;
        const float dt = static_cast<float>(frames);
        const std::array<float, 2> move{planePos_[0] - half_[0], planePos_[1] - half_[1]};
        const uint32_t total = static_cast<uint32_t>(records_.size());
        if (!stopped_) {
            const float every = 1.0f / rate_;
            if (count_ < total) {
                emitted_ = emitted_ + dt;
                while (emitted_ > every) {
                    count_ = all_ != 0 ? total : count_ + 1;
                    emitted_ = emitted_ - every;
                    if (count_ >= total) break;
                }
            }
            time_ = time_ + dt;
            if (duration_ != -1.0f && time_ > duration_) stopped_ = true;
        }
        uint32_t i = 0;
        while (i < count_) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            s.life = s.life - dt;
            if (s.life > 0.0f) {
                r.draw = 1;
                if (emitterType_ == 1) {
                    s.angle = s.degPerSec * dt + s.angle;
                    s.radius = s.deltaRadius * dt + s.radius;
                    s.pos[0] = -(plugCos(s.angle) * s.radius);
                    s.pos[1] = -(plugSin(s.angle) * s.radius);
                    s.pos[0] = source_[0] + s.pos[0];
                    s.pos[1] = s.pos[1] + source_[1];
                    s.pos[2] = s.pos[2] + 0.0f;
                } else {
                    gravityStep(s, dt);
                }
                for (int c = 0; c < 4; ++c) s.colour[c] = static_cast<uint8_t>(cvtt(static_cast<float>(s.delta[c]) * dt + static_cast<float>(s.colour[c])));
                s.size = s.deltaSize * dt + s.size;
                if (!(s.size > 0.0f)) s.size = 0.0f;
                s.rot = s.deltaRot * dt + s.rot;
                r.pos = s.pos;
                r.rot[2] = s.rot;
                r.scale[0] = r.scale[1] = s.size;
                for (auto& v : r.v) v.b = s.colour[0], v.g = s.colour[1], v.r = s.colour[2], v.alpha = s.colour[3];
                ++i;
                r.pos[0] = move[0] + r.pos[0];
                r.pos[1] = move[1] + r.pos[1];
            } else if (stopped_) {
                if (!s.done) s.done = true, r.draw = 0, ++finished_;
                if (finished_ == count_) {
                    active_ = false;
                    time_ = duration_;
                    emitted_ = 0;
                    return;
                }
                ++i;
                r.pos[0] = move[0] + r.pos[0];
                r.pos[1] = move[1] + r.pos[1];
            } else {
                setUp(i);
                if (i != count_ - 1) {
                    records_[i] = records_[count_ - 1];
                    state_[i] = state_[count_ - 1];
                    setUp(count_ - 1);
                }
                --count_;
                r.pos[0] = move[0] + r.pos[0];
                r.pos[1] = move[1] + r.pos[1];
            }
        }
    }

private:
    using V3 = std::array<float, 3>;
    static constexpr int32_t kNone = 0x34;
    struct State {
        V3 pos{}, start{}, unit{}, dir{};
        std::array<uint8_t, 4> colour{}, delta{};  // b, g, r, a
        float size = 0, deltaSize = 0, rot = 0, deltaRot = 0, life = 0, lifeAtStart = 0;
        float radialAccel = 0, tangentialAccel = 0;            // gravity mode
        float angle = 0, degPerSec = 0, radius = 0, deltaRadius = 0;  // radial mode
        bool done = false;
    };

    // cvttss2si: out of range (and NaN) gives 0x80000000.
    static int32_t cvtt(float x) {
        if (!(x > -2147483904.0f && x < 2147483648.0f)) return INT32_MIN;
        return static_cast<int32_t>(x);
    }
    float rnd() { return rng_.unit() * 2.0f - 1.0f; }
    static V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
    static V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    static V3 scale(const V3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
    // |v| as the DLL works it (the squares summed as given, the CRT's sqrt in double); the unit
    // vector, or v as it is when the length is not above 0.
    static V3 unitOr(const V3& v, bool xFirst) {
        const float sq = xFirst ? (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2] : (v[1] * v[1] + v[0] * v[0]) + v[2] * v[2];
        const float len = static_cast<float>(std::sqrt(static_cast<double>(sq)));
        if (0.0f >= len) return v;
        return {v[0] / len, v[1] / len, v[2] / len};
    }

    static int32_t key(const std::string& k) {
        static const char* const kKeys[] = {
            "angle", "angleVariance", "blendFuncDestination", "blendFuncSource", "duration", "emitterType",
            "finishColorAlpha", "finishColorBlue", "finishColorGreen", "finishColorRed", "finishColorVarianceAlpha",
            "finishColorVarianceBlue", "finishColorVarianceGreen", "finishColorVarianceRed", "finishParticleSize",
            "finishParticleSizeVariance", "gravityx", "gravityy", "maxParticles", "maxRadius", "maxRadiusVariance",
            "minRadius", "particleLifespan", "particleLifespanVariance", "radialAccelVariance", "radialAcceleration",
            "rotatePerSecond", "rotatePerSecondVariance", "rotationEnd", "rotationEndVariance", "rotationStart",
            "rotationStartVariance", "sourcePositionVariancex", "sourcePositionVariancey", "sourcePositionx",
            "sourcePositiony", "speed", "speedVariance", "startColorAlpha", "startColorBlue", "startColorGreen",
            "startColorRed", "startColorVarianceAlpha", "startColorVarianceBlue", "startColorVarianceGreen",
            "startColorVarianceRed", "startParticleSize", "startParticleSizeVariance", "tangentialAccelVariance",
            "tangentialAcceleration", "textureFileName", "textureImageData"};
        for (int32_t i = 0; i < kNone; ++i) {
            if (k == kKeys[i]) return i;
        }
        return kNone;
    }

    // A value as boost's stream translator reads it (std::istream >> float / int).
    void value(int32_t k, const std::string& text) {
        if (k == 2 || k == 3 || k == 5) {
            const int32_t v = static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10));
            (k == 2 ? blendDst_ : k == 3 ? blendSrc_ : emitterType_) = v;
            return;
        }
        const float v = static_cast<float>(std::strtod(text.c_str(), nullptr));
        switch (k) {
        case 0x00: angle_ = v * -1.0f; break;
        case 0x01: angleVar_ = v; break;
        case 0x04: duration_ = v; if (0.0f < v) duration_ = v * 60.0f; break;
        case 0x06: endColour_[3] = v * 255.0f; break;
        case 0x07: endColour_[0] = v * 255.0f; break;
        case 0x08: endColour_[1] = v * 255.0f; break;
        case 0x09: endColour_[2] = v * 255.0f; break;
        case 0x0a: endColourVar_[3] = v * 255.0f; break;
        case 0x0b: endColourVar_[0] = v * 255.0f; break;
        case 0x0c: endColourVar_[1] = v * 255.0f; break;
        case 0x0d: endColourVar_[2] = v * 255.0f; break;
        case 0x0e: endSize_ = v / 30.0f; break;
        case 0x0f: endSizeVar_ = v / 30.0f; break;
        case 0x10: gravityX_ = v / 3600.0f; break;
        case 0x11: gravityY_ = v * -1.0f / 3600.0f; break;
        case 0x12: break;  // maxParticles (+0x84): kept, not read
        case 0x13: maxRadius_ = v; break;
        case 0x14: maxRadiusVar_ = v; break;
        case 0x15: minRadius_ = v; break;
        case 0x16: life_ = v * 60.0f; break;
        case 0x17: lifeVar_ = v * 60.0f; break;
        case 0x18: radialAccelVar_ = v / 360000.0f; break;
        case 0x19: radialAccel_ = v / 360000.0f; break;
        case 0x1a: rotatePerSecond_ = v * -1.0f / 60.0f; break;
        case 0x1b: rotatePerSecondVar_ = v / 60.0f; break;
        case 0x1c: rotEnd_ = v; break;
        case 0x1d: rotEndVar_ = v; break;
        case 0x1e: rotStart_ = v; break;
        case 0x1f: rotStartVar_ = v; break;
        case 0x20: sourceVar_[0] = v; break;
        case 0x21: sourceVar_[1] = v; break;
        case 0x22: source_[0] = v; break;
        case 0x23: source_[1] = params_.screenH - v; break;
        case 0x24: speed_ = v / 60.0f; break;
        case 0x25: speedVar_ = v / 60.0f; break;
        case 0x26: startColour_[3] = v * 255.0f; break;
        case 0x27: startColour_[0] = v * 255.0f; break;
        case 0x28: startColour_[1] = v * 255.0f; break;
        case 0x29: startColour_[2] = v * 255.0f; break;
        case 0x2a: startColourVar_[3] = v * 255.0f; break;
        case 0x2b: startColourVar_[0] = v * 255.0f; break;
        case 0x2c: startColourVar_[1] = v * 255.0f; break;
        case 0x2d: startColourVar_[2] = v * 255.0f; break;
        case 0x2e: startSize_ = v / 30.0f; break;
        case 0x2f: startSizeVar_ = v / 30.0f; break;
        case 0x30: tanAccelVar_ = v / 3600.0f; break;
        case 0x31: tanAccel_ = v / 3600.0f; break;
        default: break;
        }
    }

    // A colour channel at set-up: clamped to 0-255.
    float channel(float var, float base) {
        float v = rnd() * var + base;
        if (0.0f > v) return 0.0f;
        if (255.0f > v) return v;
        return 255.0f;
    }

    void setUp(uint32_t i) {
        State& s = state_[i];
        ParticleRecord& r = records_[i];
        r.draw = 0;
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        s.done = false;
        float life = rnd() * lifeVar_ + life_;
        if (!(life > 0.0f)) life = 0.0f;
        s.life = s.lifeAtStart = life;
        s.pos[0] = rnd() * sourceVar_[0] + source_[0];
        s.pos[1] = rnd() * sourceVar_[1] + source_[1];
        s.start = s.pos;
        s.unit = unitOr({s.pos[0] - source_[0], s.pos[1] - source_[1], 0}, true);
        // Start r, g, b, a, then the finish's, each with its variance (colours b, g, r, a).
        const float r0 = channel(startColourVar_[2], startColour_[2]), g0 = channel(startColourVar_[1], startColour_[1]);
        const float b0 = channel(startColourVar_[0], startColour_[0]), a0 = channel(startColourVar_[3], startColour_[3]);
        const float r1 = channel(endColourVar_[2], endColour_[2]), g1 = channel(endColourVar_[1], endColour_[1]);
        const float b1 = channel(endColourVar_[0], endColour_[0]), a1 = channel(endColourVar_[3], endColour_[3]);
        s.colour = {static_cast<uint8_t>(cvtt(b0)), static_cast<uint8_t>(cvtt(g0)), static_cast<uint8_t>(cvtt(r0)), static_cast<uint8_t>(cvtt(a0))};
        s.delta = {static_cast<uint8_t>(cvtt((b1 - b0) / life)), static_cast<uint8_t>(cvtt((g1 - g0) / life)),
                   static_cast<uint8_t>(cvtt((r1 - r0) / life)), static_cast<uint8_t>(cvtt((a1 - a0) / life))};
        float size0 = rnd() * startSizeVar_ + startSize_;
        if (!(size0 > 0.0f)) size0 = 0.0f;
        s.size = size0;
        float size1 = rnd() * endSizeVar_ + endSize_;
        if (!(size1 > 0.0f)) size1 = 0.0f;
        s.deltaSize = (size1 - size0) / life;
        const float rot0 = plugRad(rnd() * rotStartVar_ + rotStart_);
        const float rot1 = plugRad(rnd() * rotEndVar_ + rotEnd_);
        s.rot = rot0;
        s.deltaRot = (rot1 - rot0) / life;
        const float a = plugRad(rnd() * angleVar_ + angle_);
        if (emitterType_ == 1) {
            const float maxR = rnd() * radial_[1] + radial_[0];
            const float minR = rnd() * radial_[3] + radial_[2];
            s.radius = maxR;
            s.deltaRadius = (minR - maxR) / life;
            s.angle = a;
            s.degPerSec = plugRad(rnd() * radial_[5] + radial_[4]);
            return;
        }
        const V3 v{plugCos(a), plugSin(a), 0};
        s.dir = scale(v, rnd() * speedVarG_ + speedG_);
        s.radialAccel = rnd() * radialVarG_ + radialG_;
        s.tangentialAccel = rnd() * tanVarG_ + tanG_;
    }

    // Gravity mode (0x10003ac8): the radial term (the place less the source, not made a unit),
    // the tangent (from the set-up's radial unit turned a quarter and the direction, its sign by
    // the acceleration's), and gravity, added to the direction over dt; the place moves by it.
    void gravityStep(State& s, float dt) const {
        V3 radial{0, 0, 0};
        const float dx = s.pos[0] - source_[0], dy = s.pos[1] - source_[1];
        if (dx != 0.0f || dy != 0.0f) radial = {dx, dy, 0};
        radial = scale(radial, s.radialAccel);
        const V3 t0{-s.unit[1], s.unit[0], s.unit[2]};
        V3 t;
        if (s.tangentialAccel > 0.0f) {
            const V3 a = sub(s.dir, t0);
            t = unitOr(sub(s.dir, V3{-a[1], a[0], a[2]}), false);
        } else {
            const V3 a = sub(t0, s.dir);
            t = unitOr(sub(V3{-a[1], a[0], a[2]}, s.dir), false);
        }
        t = scale(t, s.tangentialAccel);
        const V3 tmp = scale(add(add(radial, t), gravity_), dt);
        s.dir = add(s.dir, tmp);
        s.pos = add(s.pos, scale(scale(s.dir, dt), 1.0f));
    }

    std::vector<State> state_;
    int32_t all_ = 0, blendDst_ = 0, blendSrc_ = 0, emitterType_ = 0;
    float angle_ = 0, angleVar_ = 0, duration_ = 0, endSize_ = 0, endSizeVar_ = 0, gravityX_ = 0, gravityY_ = 0;
    float maxRadius_ = 0, maxRadiusVar_ = 0, minRadius_ = 0, life_ = 0, lifeVar_ = 0, radialAccelVar_ = 0, radialAccel_ = 0;
    float rotatePerSecond_ = 0, rotatePerSecondVar_ = 0, rotEnd_ = 0, rotEndVar_ = 0, rotStart_ = 0, rotStartVar_ = 0;
    float speed_ = 0, speedVar_ = 0, startSize_ = 0, startSizeVar_ = 0, tanAccelVar_ = 0, tanAccel_ = 0;
    std::array<float, 4> startColour_{}, startColourVar_{}, endColour_{}, endColourVar_{};  // b, g, r, a
    std::array<float, 2> source_{}, sourceVar_{}, half_{};
    // The mode's copies (Init, after the dict): gravity +0x10c-0x130, radial +0x134-0x148.
    V3 gravity_{};
    float speedG_ = 0, speedVarG_ = 0, radialG_ = 0, radialVarG_ = 0, tanG_ = 0, tanVarG_ = 0;
    std::array<float, 6> radial_{};  // max, its variance, min, its variance (0), turn, its variance
    float rate_ = 0, emitted_ = 0, time_ = 0;
    uint32_t count_ = 0, finished_ = 0;
    bool stopped_ = false, active_ = false;
};


// radiate.ppi (Happiness 2, 2 loads, and one in a sample scene): streaks of light shooting out from
// a point. Init 0x100020e0, Update 0x100021b0, spawn 0x100027c0. The base (0x10001120) makes
// count / f0 groups, a head and f0 - 1 trail pieces each. Additive.
//   f0: records per group; f1: the heads' time (frames); f2 f3: the centre; f4 f5: the heads'
//   scale from and to (thousandths); f6: how far they go.
// A head (not drawn) starts within 100 of the centre and runs a two-point path (cs2ParticleBaseEx's)
// straight out from it to f6 away, over 30-100% of f1 frames (rand x 0.7 f1 + 0.3 f1; tangents f6
// long), growing from f4 to f5; then it starts again. One more head runs each update. While a head
// runs, each update its first free trail piece becomes a streak from where it was to where it is
// (placeStreak, as concent's), which fades from 255 to 0 over f0 - 1 frames and is free again.
// (A second phase, waiting f1 frames, is never entered: the run goes straight to the restart.)
class Radiate : public ParticleEffect {
public:
    Radiate(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        blend_ = 1;
        const auto& f = p.f;
        const uint32_t size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const uint32_t count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        hold_ = f[1];
        centre_ = {f[2], f[3], 0};
        from_ = f[4] / 1000.0f, to_ = f[5] / 1000.0f;
        dist_ = f[6];
        heads_.resize(groups_);
        pieces_.resize(static_cast<size_t>(groups_) * trail_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        active_ = std::min(active_ + 1, groups_);
        for (uint32_t i = 0; i < active_; ++i) {
            Head& h = heads_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(h.counter);
            if (h.phase == 0) {
                r.pos = h.path.at(t);
                const float sc = mapRange(from_, to_, 0, h.time, t);
                r.scale[0] = r.scale[1] = sc;
                if (t > h.time) h.start = h.counter, h.phase = 2;
            } else if (h.phase == 1) {
                r.pos = h.path.at(t);
                if (t > hold_) h.start = h.counter, h.phase = 2;
            } else {
                spawn(i);
            }
            for (uint32_t k = 0; k < trail_ && h.phase == 0; ++k) {
                Piece& c = piece(i, k);
                if (c.on) continue;
                placeStreak(r, h.prev, pieceRecord(i, k));
                c.on = true;
                break;
            }
            for (uint32_t k = 0; k < trail_; ++k) {
                Piece& c = piece(i, k);
                if (!c.on) continue;
                ParticleRecord& pr = pieceRecord(i, k);
                setAlpha(pr, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(trail_), static_cast<float>(c.counter))));
                c.counter += frames;
                if (static_cast<uint32_t>(c.counter) >= trail_) c.counter = 0, c.on = false, setAlpha(pr, 0);
            }
            h.prev = r.pos;
            h.counter += frames;
        }
    }

private:
    struct Head {
        int32_t counter = 0, start = 0, phase = 0;
        float time = 0;
        std::array<float, 3> prev{};
        SplinePath path;
    };
    struct Piece {
        int32_t counter = 0;
        bool on = false;
    };

    Piece& piece(uint32_t i, uint32_t k) { return pieces_[static_cast<size_t>(i) * trail_ + k]; }
    ParticleRecord& pieceRecord(uint32_t i, uint32_t k) { return records_[groups_ + static_cast<size_t>(i) * trail_ + k]; }

    void spawn(uint32_t i) {
        ParticleRecord& r = records_[i];
        Head& h = heads_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        r.scale = {from_, from_, 1};
        setAlpha(r, 0);
        r.pos[0] = (rng_.unit() * 2.0f - 1.0f) * 100.0f + centre_[0];
        r.pos[1] = (rng_.unit() * 2.0f - 1.0f) * 100.0f + centre_[1];
        r.pos[2] = 0;
        h.counter = 0, h.phase = 0;
        h.prev = r.pos;
        h.path = SplinePath{};
        h.path.add(r.pos);
        float dx = r.pos[0] - centre_[0], dy = r.pos[1] - centre_[1];
        const float len = static_cast<float>(std::sqrt(static_cast<double>(dy * dy + dx * dx + 0.0f)));
        if (!(0.0f >= len)) dx = dx / len, dy = dy / len;
        h.path.add({centre_[0] + dx * dist_, centre_[1] + dy * dist_, 0});
        // rand (the host's, in ST0) x 0.7 f1 + 0.3 f1: x87 steps (x87Note).
        h.time = rng_.unit() * (hold_ * 0.7f) + hold_ * 0.3f;
        h.path.finish(h.time, dist_);
        for (uint32_t k = 0; k < trail_; ++k) {
            piece(i, k) = Piece{};
            ParticleRecord& c = pieceRecord(i, k);
            c.pos = r.pos;
            setAlpha(c, 0);
        }
    }

    uint32_t groups_ = 0, trail_ = 0, active_ = 0;
    float hold_ = 0, from_ = 0, to_ = 0, dist_ = 0;
    std::array<float, 3> centre_{};
    std::vector<Head> heads_;
    std::vector<Piece> pieces_;
};


// rain.ppi (Happiness 2, 1 load, and the sample scenes): rain, x87 (VS2008) build.
// Init 0x10001070, Update 0x10001270, spawn 0x10001440 (0x100016f0 for a start part-way); every
// record a drop of its own (0x10001840). Alpha blend; it does not read the plane fields.
//   f0 + (0 to f1): a drop's frames to fall 1020 (each at least 1, f1 at least 0); f2: the slant
//   (hundredths of a radian, within +-1); f3: its spread (hundredths, within +-0.2); f4: one more
//   lot in 61 - f4 updates (1-60); f5: how many then; f6: start half of them part-way.
// A drop takes a quarter of the texture (rand & 3; .data 0x10011d10), a place on a line 100 above
// the screen 2.8 screens wide (x = rand x w - (w - screen) / 2, z 0-10), a slant (the spread about
// f2) and an alpha of 100-169; it falls along (0, 1) turned by its slant (the library's turn,
// three decimals: plugAngle) at 1020 / its frames a frame, fading in over 2 frames, and once 200
// below the screen it starts again. Its x87 arithmetic is single precision (x87Note).
class Rain : public ParticleEffect {
public:
    Rain(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        min_ = std::max(1, static_cast<int32_t>(f[0]));
        range_ = std::max(0, static_cast<int32_t>(f[1]));
        base_ = f[2] / 100.0f;
        if (1.0f < base_) base_ = 1.0f;
        else if (-1.0f > base_) base_ = -1.0f;
        spread_ = f[3] / 100.0f;
        if (spread_ > 0.2f) spread_ = 0.2f;
        if (spread_ < -0.2f) spread_ = -0.2f;
        rate_ = std::clamp(61 - static_cast<int32_t>(f[4]), 1, 60);
        amount_ = static_cast<int32_t>(f[5]);
        state_.resize(records_.size());
        if (static_cast<int32_t>(f[6]) == 0) {
            active_ = 0;
            for (size_t i = 0; i < records_.size(); ++i) spawn(i);
        } else {
            active_ = static_cast<uint32_t>(records_.size()) >> 1;
            for (uint32_t i = 0; i < active_; ++i) {
                spawn(i);
                State& s = state_[i];
                ParticleRecord& r = records_[i];
                s.counter = static_cast<int32_t>(rng_.unit() * s.frames);
                r.pos[0] = s.vel[0] * static_cast<float>(s.counter) + r.pos[0];
                r.pos[1] = s.vel[1] * static_cast<float>(s.counter) + r.pos[1];
            }
            for (size_t i = active_; i < records_.size(); ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (rng_.next() % static_cast<uint32_t>(rate_) == 0) active_ += static_cast<uint32_t>(amount_);
        active_ = std::min(active_, static_cast<uint32_t>(records_.size()));
        for (uint32_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            if (s.phase == 0) {
                r.pos[0] = s.vel[0] + r.pos[0];
                r.pos[1] = s.vel[1] + r.pos[1];
                setAlpha(r, static_cast<int32_t>(mapRange(0, s.alpha, 0, 2.0f, static_cast<float>(s.counter - s.start))));
                if (params_.screenH + 200.0f < r.pos[1]) s.phase = 2, s.start = s.counter;
            } else if (s.phase == 1) {
                r.pos = {s.vel[0] + r.pos[0], s.vel[1] + r.pos[1], s.vel[2] + r.pos[2]};
                if (s.counter > min_) s.phase = 10, s.start = s.counter;
            } else {
                spawn(i);
            }
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        std::array<float, 3> vel{};
        float frames = 0, alpha = 0;
    };

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x10011d10
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        const auto& q = kQuarter[rng_.next() & 3];
        setUv(r, q[0], q[1], 0.5f, 0.5f);
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = 0.5f * params_.texH;
        const float w = static_cast<float>(static_cast<int32_t>(params_.screenW * 2.8f));
        r.pos[0] = rng_.unit() * w - (w - params_.screenW) * 0.5f;
        r.pos[1] = -100.0f;
        r.pos[2] = rng_.unit() * 10.0f;
        setAlpha(r, 0);
        r.rot[0] = r.rot[1] = 0;
        r.rot[2] = rng_.unit() * spread_ - spread_ * 0.5f + base_;
        s.counter = s.start = s.phase = 0;
        s.frames = rng_.unit() * static_cast<float>(range_) + static_cast<float>(min_);
        s.alpha = static_cast<float>(rng_.next() % 70 + 100);
        // (0, 1, 0) through the turn about z: (-sin, cos, 0), times 1020 / its frames.
        const float a = plugAngle(r.rot[2]);
        const float k = 1020.0f / s.frames;
        s.vel = {-plugSin(a) * k, plugCos(a) * k, 0.0f * k};
    }

    std::vector<State> state_;
    int32_t min_ = 1, range_ = 0, rate_ = 1, amount_ = 0;
    float base_ = 0, spread_ = 0;
    uint32_t active_ = 0;
};


// spark2.ppi (Happiness 2, 2 loads: cherry petals on the wind). Init 0x10002130,
// Update 0x10002410, spawn 0x100029b0 (0x10003580 for a start part-way). Init gives the base
// (0x10001120) the ECX the CRT's memset leaves as the group size: 0 where the CPU has fast
// `rep stosb` (ERMS, the CRT's choice then), so every record is a petal of its own and the trail
// code never runs. (Without ERMS the SSE path leaves the buffer's end there and no group forms.)
//   f0 f1 -> f2 f3: the line the petals start on (a two-point path, tangents 1, over 100); f4: size
//   (percent, up to twice that at random; at least 0.01); f5: a step's length (f5 to 2 f5; the
//   path's tangents 1.25 f5); f6 f7 f8: fade-in, hold and fade-out frames; f9 f10: the first
//   step's x (rand x f10 - f9, hundredths); f11 f12: its y (1: -(rand x f12 + 0.05), 2: rand x
//   f12 - f12 / 2, else rand x f12 + 0.05; hundredths); f13: one more lot in 61 - f13 updates
//   (1-60); f14: how many then; f15: start half part-way; f16: additive; f17-f22: the turns a
//   frame about z, x and y (centre and spread, hundredths).
// A petal takes a quarter of the texture (rand & 3; .data 0x100170b0), a random point of the
// line, a turn about z (0-pi) and its three spins; its path runs the first step from there, then
// 1-4 more (another while a random number does not divide by 15), each turned from the first
// step's direction by a random amount within +-50 degrees (the bounds' gaps, the host's sqrt of
// pow 2) and as long as the first, over f6 + f7 + f8 frames. It fades in, holds and fades out on
// its path, spinning, and starts again.
class Spark2 : public ParticleEffect {
public:
    Spark2(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        const auto& f = p.f;
        size_ = f[4] / 100.0f;
        if (0.01f > size_) size_ = 0.01f;
        step_ = f[5];
        tangent_ = f[5] * 1.25f;
        in_ = std::max(0, static_cast<int32_t>(f[6]));
        hold_ = std::max(0, static_cast<int32_t>(f[7]));
        out_ = std::max(0, static_cast<int32_t>(f[8]));
        line_.add({f[0], f[1], 0});
        line_.add({f[2], f[3], 0});
        line_.finish(100.0f, 1.0f);
        x0_ = f[9] / 100.0f, x1_ = f[10] / 100.0f;
        mode_ = static_cast<int32_t>(f[11]);
        y_ = f[12] / 100.0f;
        rate_ = std::clamp(61 - static_cast<int32_t>(f[13]), 1, 60);
        amount_ = static_cast<int32_t>(f[14]);
        if (static_cast<int32_t>(f[16]) != 0) blend_ = 1;
        for (int k = 0; k < 6; ++k) spin_[k] = f[17 + k] / 100.0f;
        state_.resize(records_.size());
        if (static_cast<int32_t>(f[15]) == 0) {
            active_ = 1;
            for (size_t i = 0; i < records_.size(); ++i) spawn(i);
        } else {
            active_ = static_cast<uint32_t>(records_.size()) >> 1;
            for (uint32_t i = 0; i < active_; ++i) startPartWay(i);
            for (size_t i = active_; i < records_.size(); ++i) spawn(i);
        }
    }

    void step(int32_t frames) override {
        if (rng_.next() % static_cast<uint32_t>(rate_) == 0) active_ += static_cast<uint32_t>(amount_);
        active_ = std::min(active_, static_cast<uint32_t>(records_.size()));
        for (uint32_t i = 0; i < active_; ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            auto move = [&] {
                r.pos = s.path.at(static_cast<float>(s.counter));
                r.rot[2] = s.spin[2] + r.rot[2];
                r.rot[0] = s.spin[0] + r.rot[0];
                r.rot[1] = s.spin[1] + r.rot[1];
            };
            if (s.phase == 0) {
                move();
                setAlpha(r, static_cast<int32_t>(mapRange(0, 255, 0, static_cast<float>(in_), static_cast<float>(s.counter))));
                if (in_ < s.counter) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                move();
                if (hold_ < t) s.phase = 2, s.start = s.counter;
            } else if (s.phase == 2) {
                move();
                setAlpha(r, static_cast<int32_t>(mapRange(255, 0, 0, static_cast<float>(out_), static_cast<float>(t))));
                if (out_ < t) s.phase = 3, s.start = s.counter;
            } else {
                spawn(i);
            }
            s.counter += frames;
        }
    }

private:
    using V3 = std::array<float, 3>;
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        std::array<float, 3> spin{};  // x, y, z
        SplinePath path;
    };

    // The host's pow (+0x48) and sqrt (+0x44), SSE in double.
    static float gap(float d) {
        const float sq = static_cast<float>(std::pow(static_cast<double>(d), 2.0));
        return static_cast<float>(std::sqrt(static_cast<double>(sq)));
    }
    static V3 unit2(V3 v) {
        const float len = static_cast<float>(std::sqrt(static_cast<double>(v[1] * v[1] + v[0] * v[0] + 0.0f)));
        if (!(0.0f >= len)) v = {v[0] / len, v[1] / len, 0.0f / len};
        return v;
    }

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x100170b0
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const auto& q = kQuarter[rng_.next() & 3];
        setUv(r, q[0], q[1], 0.5f, 0.5f);
        const float sc = rng_.unit() * size_ + size_;  // x87Note
        r.scale = {sc, sc, 1};
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;
        r.pos = line_.at(rng_.unit() * 100.0f);
        r.rot[2] = rng_.unit() * 3.14159274f;
        s.counter = s.phase = 0;
        s.path = SplinePath{};
        s.path.add(r.pos);
        const V3 start = r.pos;
        auto spin = [&](float base, float k) {
            const float u = rng_.unit();
            return u * k + base - k * 0.5f;
        };
        s.spin[0] = spin(spin_[2], spin_[3]);
        s.spin[1] = spin(spin_[4], spin_[5]);
        s.spin[2] = spin(spin_[0], spin_[1]);
        V3 d{rng_.unit() * x1_ - x0_, 0, 0};
        if (mode_ == 1) {
            d[1] = (rng_.unit() * y_ + 0.05f) * -1.0f;
        } else if (mode_ == 2) {
            d[1] = rng_.unit() * y_ - y_ * 0.5f;
        } else {
            d[1] = rng_.unit() * y_ + 0.05f;
        }
        d = unit2(d);
        float len = rng_.unit() * step_ + step_;
        V3 prev{start[0] + len * d[0], start[1] + len * d[1], start[2] + len * d[2]};
        s.path.add(prev);
        const V3 base = unit2(SplinePath::sub(prev, start));
        // The bounds: the first direction turned by -50 and +50 degrees (host +0x10, +0x1c, +0x18).
        auto bound = [&](float deg) {
            const float c = plugCos(plugRad(deg)), sn = plugSin(plugRad(deg));
            return std::array<float, 2>{c * base[0] - sn * base[1], sn * base[0] + c * base[1]};
        };
        const auto b1 = bound(-50.0f), b2 = bound(50.0f);
        for (int k = 0;;) {
            const float gx = gap(b2[0] - b1[0]);
            V3 n{base[0] + (rng_.unit() * gx - gx * 0.5f), 0, 0};
            const float gy = gap(b2[1] - b1[1]);
            n[1] = base[1] + (rng_.unit() * gy - gy * 0.5f);
            n = unit2(n);
            len = rng_.unit() * step_ + step_;
            prev = {prev[0] + len * n[0], prev[1] + len * n[1], prev[2] + len * n[2]};
            s.path.add(prev);
            if (rng_.next() % 15 == 0 || ++k >= 4) break;
        }
        s.path.finish(static_cast<float>(out_ + hold_ + in_), tangent_);
    }

    // 0x10003580: a spawn whose clock starts at random within its time, its phase moved on to
    // match (an alpha of 255 once past the fade-in; the hold compared with the clock itself).
    void startPartWay(uint32_t i) {
        spawn(i);
        State& s = state_[i];
        s.counter = static_cast<int32_t>(rng_.unit() * static_cast<float>(out_ + hold_ + in_));
        if (in_ < s.counter) ++s.phase, s.start += in_, setAlpha(records_[i], 255);
        if (hold_ < s.counter) ++s.phase, s.start += hold_;
    }

    SplinePath line_;
    std::vector<State> state_;
    float size_ = 0, step_ = 0, tangent_ = 0, x0_ = 0, x1_ = 0, y_ = 0;
    std::array<float, 6> spin_{};  // f17-f22: z (centre, spread), x, y
    int32_t in_ = 0, hold_ = 0, out_ = 0, mode_ = 0, rate_ = 1, amount_ = 0;
    uint32_t active_ = 1;
};

// s_040101.ppi (Happiness 2, 2 loads: a sweep of twinkles along a curve), spark2's
// x87 (VS2008) sibling. Init 0x10001230, Update 0x10001560, spawn 0x10001a20; every record a
// twinkle of its own (0x10002580). Additive. Every twinkle runs from the first update (the active
// count Init keeps is never read) and runs once.
//   f1-f6 and f7-f12: two three-point curves (paths over 100; tangents f18 and 100); f13: size
//   (percent, up to twice that); f14: a step's length (f14 to 2 f14); f15 f16 f17: fade-in, hold
//   and fade-out frames (to alpha 128, .data 0x10010a58); f18: the twinkles' tangents; f19: start
//   half of them part-way. f0 is not read.
// A twinkle takes a quarter of the texture (rand & 3; .data 0x10012d10), a point between the
// curves at one random parameter (x and y each at a random share of the way from the first to
// the second), z 0-50 and a turn of 0-pi; its path takes a first step in a random direction
// ((-15..15, -15..15), unit) and 1-4 more within +-20 degrees of it (as spark2's), over
// f15 + f16 + f17 frames. It fades in, holds and fades out on its path and then stays done. Its x87
// arithmetic is single precision (x87Note).
class S040101 : public ParticleEffect {
public:
    S040101(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        blend_ = 1;
        const auto& f = p.f;
        for (int k = 0; k < 3; ++k) a_.add({f[1 + k * 2], f[2 + k * 2], 0});
        a_.finish(100.0f, f[18]);
        for (int k = 0; k < 3; ++k) b_.add({f[7 + k * 2], f[8 + k * 2], 0});
        b_.finish(100.0f, 100.0f);
        size_ = f[13] / 100.0f;
        step_ = f[14];
        in_ = static_cast<int32_t>(f[15]), hold_ = static_cast<int32_t>(f[16]), out_ = static_cast<int32_t>(f[17]);
        tangent_ = f[18];
        state_.resize(records_.size());
        size_t i = 0;
        if (static_cast<int32_t>(f[19]) != 0) {
            for (; i < (records_.size() >> 1); ++i) {
                spawn(i);
                state_[i].counter = static_cast<int32_t>(rng_.unit() * static_cast<float>(hold_ + out_ + in_));
            }
        }
        for (; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const int32_t t = s.counter - s.start;
            if (s.phase == 0) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                setAlpha(r, static_cast<int32_t>(mapRange(0, 128, 0, static_cast<float>(in_), static_cast<float>(s.counter))));
                if (in_ < s.counter) s.phase = 1, s.start = s.counter;
            } else if (s.phase == 1) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                if (hold_ < t) s.phase = 2, s.start = s.counter;
            } else if (s.phase == 2) {
                r.pos = s.path.at(static_cast<float>(s.counter));
                setAlpha(r, static_cast<int32_t>(mapRange(128, 0, 0, static_cast<float>(out_), static_cast<float>(t))));
                if (out_ < t) s.phase = 3, s.start = s.counter;
            }
            s.counter += frames;
        }
    }

private:
    using V3 = std::array<float, 3>;
    struct State {
        int32_t counter = 0, start = 0, phase = 0;
        SplinePath path;
    };

    static float gap(float d) {
        const float sq = static_cast<float>(std::pow(static_cast<double>(d), 2.0));  // host +0x48
        return static_cast<float>(std::sqrt(static_cast<double>(sq)));             // host +0x44
    }
    // (y y + x x) + z z, the CRT's sqrt; each part divided (0 / len for z) when above 0.
    static V3 unit(V3 v) {
        const float len = static_cast<float>(std::sqrt(static_cast<double>(v[1] * v[1] + v[0] * v[0] + v[2] * v[2])));
        if (!(0.0f >= len)) v = {v[0] / len, v[1] / len, v[2] / len};
        return v;
    }

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x10012d10
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        r.pivot[0] = params_.texW * 0.5f, r.pivot[1] = params_.texH * 0.5f;
        const auto& q = kQuarter[rng_.next() & 3];
        setUv(r, q[0], q[1], 0.5f, 0.5f);
        const float sc = rng_.unit() * size_ + size_;
        r.scale = {sc, sc, 1};
        for (auto& v : r.v) v.r = v.g = v.b = 255, v.alpha = 0;
        const float u = rng_.unit() * 100.0f;
        const V3 pa = a_.at(u), pb = b_.at(u);
        r.pos[0] = mapRange(pa[0], pb[0], 0, 100, rng_.unit() * 100.0f);
        r.pos[1] = mapRange(pa[1], pb[1], 0, 100, rng_.unit() * 100.0f);
        r.pos[2] = rng_.unit() * 50.0f;
        r.rot[2] = rng_.unit() * 3.14159274f;
        s.counter = s.phase = 0;
        s.path = SplinePath{};
        s.path.add(r.pos);
        V3 d{rng_.unit() * 30.0f - 15.0f, 0, 0};
        d[1] = rng_.unit() * 30.0f - 15.0f;
        d = unit({d[0], d[1], 0.0f});
        float len = rng_.unit() * step_ + step_;
        V3 prev{r.pos[0] + d[0] * len, r.pos[1] + d[1] * len, r.pos[2] + d[2] * len};
        s.path.add(prev);
        const V3 base = unit(SplinePath::sub(prev, r.pos));
        auto bound = [&](float deg) {
            const float c = plugCos(plugRad(deg)), sn = plugSin(plugRad(deg));
            return std::array<float, 2>{c * base[0] - sn * base[1], sn * base[0] + c * base[1]};
        };
        const auto b1 = bound(-20.0f), b2 = bound(20.0f);
        for (int k = 0;;) {
            const float gx = gap(b2[0] - b1[0]);
            V3 n{rng_.unit() * gx - gx * 0.5f + base[0], 0, 0};
            const float gy = gap(b2[1] - b1[1]);
            n[1] = rng_.unit() * gy - gy * 0.5f + base[1];
            n = unit(n);
            len = rng_.unit() * step_ + step_;
            prev = {prev[0] + n[0] * len, prev[1] + n[1] * len, prev[2] + n[2] * len};
            s.path.add(prev);
            if (rng_.next() % 15 == 0 || ++k >= 4) break;
        }
        s.path.finish(static_cast<float>(out_ + hold_ + in_), tangent_);
    }

    SplinePath a_, b_;
    std::vector<State> state_;
    float size_ = 0, step_ = 0, tangent_ = 0;
    int32_t in_ = 0, hold_ = 0, out_ = 0;
};


// swordspark.ppi (Happiness 2, one load: sparks flying from a clash of blades): an
// x87 (VS2008) build on cs2Particle's framework (0x10001e00: a group of one record each, no
// followers). Init 0x10001070, Update 0x10001110, spawn 0x10001860. Additive when f4 is under 1.
//   f0 f1: the centre; f2: size (thousandths of the texture); f3: speed; f4: alpha (64 x f4 + 191);
//   f5: 0 for the whole texture, else one of its first (int)f5 quarters (.data 0x10012d10: top
//   left, top right, bottom left, bottom right) at random; f6: frames at full speed; f7: the
//   frame the fade-out starts; f8: the frame it ends.
// The spawn sends record i out at (360 / count) x i degrees (the host's +0x10, then cut to
// hundredths of a radian, then cos +0x1c and sin +0x18): (-sin, cos, 0.5) made unit length (host
// +0x70) times a speed of f3 x (0.5 to 1.5), z three times that. It takes a size of (1 to 2) x
// f2 / 1000, turns of 0 to pi about x, y and z, each negated one time in two (rand & 1), and spins
// of 0.3 to 0.6 an update. Its corners are the texture's: +-half its width and height. Each update
// moves it by its speed and spin (after f6 frames by 0.3 of its speed and half its spin) and builds
// its matrix: the scale (x and y; 0x100024f0) after the turns (x, y, z: plugRotate's) with the
// position added, puts the corners through it, shades the quad by its normal (y + x - z mapped
// from [-1, 1] onto 255-128, grey) and sets its alpha: 64 x f4 + 191 until f7, then down to 0 by
// f8 (mapRange from the alpha it had). Then it stays out of sight: nothing starts it again.
// Its x87 arithmetic runs in single precision (x87Note): float steps here.
class SwordSpark : public ParticleEffect {
public:
    SwordSpark(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {
        if (p.f[4] < 1.0f) blend_ = 1;
        state_.resize(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) spawn(i);
    }

    void step(int32_t frames) override {
        const auto& f = params_.f;
        const auto alpha = static_cast<int32_t>(64.0f * f[4] + 191.0f);
        for (size_t i = 0; i < records_.size(); ++i) {
            State& s = state_[i];
            ParticleRecord& r = records_[i];
            const float t = static_cast<float>(s.counter);
            if (s.phase == 0) {
                for (int k = 0; k < 3; ++k) s.pos[k] = s.vel[k] + s.pos[k], s.rot[k] = s.spin[k] + s.rot[k];
                setAlpha(r, alpha);
                if (f[6] < t) s.phase = 1, s.from = r.v[0].alpha;
            } else if (s.phase == 1) {
                for (int k = 0; k < 3; ++k) s.pos[k] = s.vel[k] * 0.3f + s.pos[k], s.rot[k] = s.spin[k] * 0.5f + s.rot[k];
                setAlpha(r, alpha);
                if (f[7] < t) s.phase = 2, s.from = r.v[0].alpha;
            } else if (s.phase == 2) {
                for (int k = 0; k < 3; ++k) s.pos[k] = s.vel[k] * 0.3f + s.pos[k], s.rot[k] = s.spin[k] * 0.5f + s.rot[k];
                setAlpha(r, static_cast<int32_t>(mapRange(static_cast<float>(s.from), 0, f[7], f[8], t)));
                if (f[8] < t) s.phase = 3, s.from = s.counter, setAlpha(r, 0);
            }
            Mat m = identity();
            plugRotate(m, s.rot[0], s.rot[1], s.rot[2]);
            m[12] = m[12] + s.pos[0], m[13] = s.pos[1] + m[13], m[14] = s.pos[2] + m[14];
            m = mul(scaling(s.scale, s.scale, 1.0f), m);
            for (size_t j = 0; j < 4; ++j) {
                const auto v = transform(corner(j), m);
                r.v[j].x = v[0], r.v[j].y = v[1], r.v[j].z = v[2];
            }
            const float e1x = r.v[1].x - r.v[0].x, e1y = r.v[1].y - r.v[0].y, e1z = r.v[1].z - r.v[0].z;
            const float e2x = r.v[2].x - r.v[0].x, e2y = r.v[2].y - r.v[0].y, e2z = r.v[2].z - r.v[0].z;
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            const float len = static_cast<float>(std::sqrt(static_cast<double>(nx * nx + ny * ny + nz * nz)));
            if (!(len <= 0.0f)) nx = nx / len, ny = ny / len, nz = nz / len;
            const auto shade = static_cast<uint8_t>(static_cast<int32_t>(mapRange(255, 128, -1, 1, (ny + nx) - nz * 1.0f)));
            for (auto& v : r.v) v.r = v.g = v.b = shade;
            s.counter += frames;
        }
    }

private:
    struct State {
        int32_t counter = 0, from = 0, phase = 0;
        std::array<float, 3> vel{}, pos{}, rot{}, spin{};
        float scale = 1;
    };

    std::array<float, 3> corner(size_t j) const {
        const float x = params_.texW * 0.5f, y = params_.texH * 0.5f;
        return {(j & 1) ? x : -x, (j & 2) ? y : -y, 0};
    }

    void spawn(size_t i) {
        static constexpr float kQuarter[4][2] = {{0, 0}, {0.5f, 0}, {0, 0.5f}, {0.5f, 0.5f}};  // .data 0x10012d10
        const auto& f = params_.f;
        ParticleRecord& r = records_[i];
        State& s = state_[i];
        const auto k = static_cast<int32_t>(f[5]);
        if (k == 0) {
            setUv(r, kQuarter[0][0], kQuarter[0][1], 1.0f, 1.0f);
        } else {
            const uint32_t q = rng_.next() % static_cast<uint32_t>(k);
            setUv(r, kQuarter[q & 3][0], kQuarter[q & 3][1], 0.5f, 0.5f);
        }
        s.pos = {f[0], f[1], 0};
        const float size = f[2] / 1000.0f;
        s.scale = rng_.unit() * size + size;
        setAlpha(r, 0);
        s.counter = 0, s.phase = 0;
        const float deg = 360.0f / static_cast<float>(records_.size()) * static_cast<float>(i);
        const float a = static_cast<float>(static_cast<int32_t>(plugRad(deg) * 100.0f)) / 100.0f;
        const float c = plugCos(a), sn = plugSin(a);
        s.vel = {0.0f * c - sn, c + 0.0f * sn, 0.5f};
        // Host +0x70 (D3DXVec4Normalize of (v, 0)).
        const float n = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2] + 0.0f * 0.0f);
        s.vel = {s.vel[0] / n, s.vel[1] / n, s.vel[2] / n};
        const float speed = rng_.unit() * f[3] + f[3] * 0.5f;
        s.vel[0] = s.vel[0] * speed, s.vel[1] = s.vel[1] * speed, s.vel[2] = speed * 3.0f * s.vel[2];
        for (auto& v : s.rot) v = rng_.unit() * 3.14159274f;
        for (auto& v : s.spin) v = rng_.unit() * 0.3f + 0.3f;
        for (auto& v : s.rot) {
            if (rng_.next() & 1) v = v * -1.0f;
        }
        for (size_t j = 0; j < 4; ++j) {
            const auto c3 = corner(j);
            r.v[j].x = c3[0] + s.pos[0], r.v[j].y = c3[1] + s.pos[1], r.v[j].z = c3[2] + s.pos[2];
        }
    }

    std::vector<State> state_;
};

// s_arrow.ppi (Happiness 2, one load: a streak of light flying across the screen,
// shedding sparkles): an x87 (VS2008) build on cs2ParticleBaseEx's framework (0x10001af0: groups
// of (int)f0 records, a head and the rest its sparkles). Init 0x10001090, Update 0x10001150, spawn
// 0x10001790. Additive.
//   f0: records per group; f1 f2: the start; f3 f4: the end; f5: size (percent); f6: frames to
//   the end.
// The head is drawn: the texture's top half (.data 0x10010d10) about its centre, 2 x size wide
// and 0.1 x size high, alpha 0x80, turned to its way (acos of the unit way's y, host +0x70 then
// +0x28, negated while it heads right, plus pi/2). Each update moves it from the start to the end
// by mapRange over f6 frames; past that it waits until its clock passes 60 and then stays where it
// is. While it moves, each update starts sparkles that are not running, one after another until
// rand % 5 is 0: at the head, with a path (SplinePath, tangents 20, over as many frames as the
// group has sparkles) through three points each a random step on (0-60 right and 0-20 down, then
// 0-80 and 0-80 twice), one of the four cells of the texture's bottom-left quarter (rand & 3), the
// head's centre and a size of 0.1 x f5 / 100. A running sparkle follows its path, shrinks by 0.99
// an update and fades from 255 to 0 over the same frames; then it is free to start again. The
// start also writes rand x pi - pi/2 into the head's +0x2b8, which nothing reads.
// Only the first n groups update, n a global of the DLL (0x10010d78, 1 in the file) raised by one
// when rand % 10 is 0 and held to the group count; layers running s_arrow at once share it
// (PluginGlobals), as dash's. Its x87 arithmetic runs in single precision (x87Note): float steps
// here; its path code is star's (add 0x10001f40 and position 0x10002350 the same instructions;
// its finish 0x10002000 a last bit's difference from SplinePath's at most).
class SArrow : public ParticleEffect {
public:
    struct Globals : PluginGlobals {
        uint32_t active = 1;
    };

    SArrow(const ParticleParams& p, uint32_t seed, const std::shared_ptr<PluginGlobals>& shared) : ParticleEffect(p, seed) {
        auto g = std::dynamic_pointer_cast<Globals>(shared);
        if (!g) g = std::make_shared<Globals>();
        dll_ = g.get();
        globals_ = std::move(g);
        const auto& f = p.f;
        const auto size = static_cast<uint32_t>(static_cast<int32_t>(f[0]));
        const auto count = static_cast<uint32_t>(std::max(0, p.count));
        groups_ = size != 0 ? count / size : count;
        trail_ = size != 0 ? size - 1 : 0;
        blend_ = 1;
        from_ = {f[1], f[2], 0};
        to_ = {f[3], f[4], 0};
        size_ = f[5] / 100.0f;
        frames_ = static_cast<int32_t>(f[6]);
        heads_.resize(groups_);
        sparks_.resize(static_cast<size_t>(groups_) * trail_);
        for (uint32_t i = 0; i < groups_; ++i) spawn(i);
    }

    void step(int32_t frames) override {
        if (rng_.next() % 10 == 0) ++dll_->active;
        if (groups_ < dll_->active) dll_->active = groups_;
        const float lot = static_cast<float>(trail_);
        for (uint32_t i = 0; i < dll_->active; ++i) {
            Head& hs = heads_[i];
            ParticleRecord& h = records_[i];
            if (hs.phase == 0) {
                h.pos[0] = mapRange(from_[0], to_[0], 0, static_cast<float>(frames_), static_cast<float>(hs.counter));
                h.pos[1] = mapRange(from_[1], to_[1], 0, static_cast<float>(frames_), static_cast<float>(hs.counter));
                h.pos[2] = 0;
                if (hs.counter > frames_) hs.phase = 1, hs.start = hs.counter;
            } else if (hs.phase == 1 && static_cast<float>(hs.counter) > 60.0f) {
                hs.phase = 2, hs.start = hs.counter;
            }
            for (uint32_t k = 0; k < trail_ && hs.phase == 0; ++k) {
                Spark& s = spark(i, k);
                if (s.on) continue;
                ParticleRecord& c = sparkRecord(i, k);
                c.pos = h.pos;
                s.path = SplinePath{};
                s.path.add(c.pos);
                float x = rng_.unit() * 60.0f + h.pos[0];
                float y = rng_.unit() * 20.0f + h.pos[1];
                s.path.add({x, y, 0});
                for (int n = 0; n < 2; ++n) {
                    x = rng_.unit() * 80.0f + x;
                    y = rng_.unit() * 80.0f + y;
                    s.path.add({x, y, 0});
                }
                s.path.finish(lot, 20.0f);
                c.pivot = h.pivot;
                c.scale[0] = size_ * 0.1f, c.scale[1] = 0.1f * size_;
                hs.unread = rng_.unit() * 3.14159274f - 1.57079637f;
                s.on = true;
                if (rng_.next() % 5 == 0) break;
            }
            for (uint32_t k = 0; k < trail_; ++k) {
                Spark& s = spark(i, k);
                if (!s.on) continue;
                ParticleRecord& c = sparkRecord(i, k);
                c.pos = s.path.at(static_cast<float>(s.counter));
                c.scale[0] = c.scale[0] * 0.99f, c.scale[1] = 0.99f * c.scale[1];
                setAlpha(c, static_cast<int32_t>(mapRange(255, 0, 0, lot, static_cast<float>(s.counter))));
                s.counter += frames;
                if (s.counter >= static_cast<int32_t>(trail_)) s.counter = 0, s.on = false, setAlpha(c, 0);
            }
            hs.last = h.pos;  // +0x2a0, not read
            hs.counter += frames;
        }
    }

private:
    struct Head {
        int32_t counter = 0, start = 0, phase = 0;
        float unread = 0;
        std::array<float, 3> last{};
    };
    struct Spark {
        int32_t counter = 0;
        bool on = false;
        SplinePath path;
    };

    Spark& spark(uint32_t i, uint32_t k) { return sparks_[static_cast<size_t>(i) * trail_ + k]; }
    ParticleRecord& sparkRecord(uint32_t i, uint32_t k) { return records_[groups_ + static_cast<size_t>(i) * trail_ + k]; }

    void spawn(uint32_t i) {
        static constexpr float kUv[5][2] = {{0, 0}, {0, 0.5f}, {0.25f, 0.5f}, {0, 0.75f}, {0.25f, 0.75f}};  // .data 0x10010d10
        ParticleRecord& h = records_[i];
        setUv(h, kUv[0][0], kUv[0][1], 1.0f, 0.5f);
        const float px = params_.texW * 0.5f, py = 0.5f * params_.texH;
        h.pivot[0] = px, h.pivot[1] = py;
        h.scale = {size_ + size_, size_ * 0.1f, 1};
        setAlpha(h, 0x80);
        h.pos = from_;
        const float dx = to_[0] - h.pos[0], dy = to_[1] - h.pos[1], dz = 0.0f - h.pos[2];
        // Host +0x70 (D3DXVec4Normalize of (d, 0)), then +0x28 (acos) of its y.
        const float n = std::sqrt(dx * dx + dy * dy + dz * dz + 0.0f * 0.0f);
        const float nx = n > 0 ? dx / n : 0, ny = n > 0 ? dy / n : 0, nz = n > 0 ? dz / n : 0;
        float a = plugAcos((nx * 0.0f + ny) + nz * 0.0f);
        if ((dy * 0.0f + dx) + dz * 0.0f > 0.0f) a = a * -1.0f;
        h.rot[2] = a + 1.57079637f;
        heads_[i] = Head{};
        for (uint32_t k = 0; k < trail_; ++k) {
            spark(i, k) = Spark{};
            ParticleRecord& c = sparkRecord(i, k);
            const uint32_t q = (rng_.next() & 3) + 1;
            setUv(c, kUv[q][0], kUv[q][1], 0.25f, 0.25f);
            c.pivot[0] = px, c.pivot[1] = py;
            for (auto& v : c.v) v.r = v.g = v.b = 255, v.alpha = 0;
        }
    }

    Globals* dll_ = nullptr;
    uint32_t groups_ = 0, trail_ = 0;
    std::array<float, 3> from_{}, to_{};
    float size_ = 1;
    int32_t frames_ = 0;
    std::vector<Head> heads_;
    std::vector<Spark> sparks_;
};

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// The old `particle` (and `particle2`) command's layers: a compiled script, ptcl/<name>.kcs, which
// the engine runs on its script VM instead of a plugin (Happiness 0x4fd660: the layer's +0x84 is 0;
// VM 0x82e240, the script's version (its header's second word) picks the natives: 0x9951e8 for
// version 1, which every script reimplemented here is). Each update (0x4fd420) runs the script from
// its start once per record, the record's index in +0x354 (native 36), with the layer clock (+0x348,
// native 1; ticks since the first update, times the layer's speed) and the clock at the last update
// (+0x34c, native 2; -1 before the first). The script's own memory (its globals and arrays) lasts
// as long as the layer. The records start as the plugin layers' do (0x4fe1d0: the texture-sized
// quad, white, pivot and position 0, scale 1). The three scripts the stories run share one start:
// on the first update (clock 0) a record's init, afterwards its update, then its position and
// corners back to the layer (natives 27, 31).
class ScriptParticle : public ParticleEffect {
public:
    ScriptParticle(const ParticleParams& p, uint32_t seed) : ParticleEffect(p, seed) {}

    void step(int32_t frames) override {
        if (stopped_) return;  // native 32 clears the layer's +0x344: no update runs again
        if (!started_) started_ = true, clock_ = 0, last_ = -1;
        else clock_ += frames;
        for (size_t i = 0; i < records_.size(); ++i) {
            if (clock_ == 0) init(i);
            else update(last_, clock_, i);
        }
        last_ = clock_;
    }

protected:
    virtual void init(size_t i) = 0;
    virtual void update(int32_t last, int32_t now, size_t i) = 0;

    float p(size_t k) const { return params_.f[k]; }
    // Native 34 (0x544ac0 -> 0x4c7b00): a float from the engine's own MT19937 (u32 / (2^32 - 1)),
    // shared engine-wide; the layer's generator stands in for it.
    float rand() { return rng_.unit(); }
    // Natives 6 / 7 (CRT sin 0x809cf0 / cos 0x80a700 in double, stored as float).
    static float sine(float a) { return static_cast<float>(std::sin(static_cast<double>(a))); }
    static float cosine(float a) { return static_cast<float>(std::cos(static_cast<double>(a))); }
    // Native 19 (0x545990): degrees to radians, x * pi / 180 in float.
    static float radians(float d) { return d * 3.14159265f / 180.0f; }
    // Natives 22-24 (0x583c50 / 0x583ce0 / 0x583d70 on each corner): turns about x, y and z.
    static void turnX(ParticleRecord& r, float a) {
        const float s = sine(a), c = cosine(a);
        for (auto& v : r.v) {
            const float y = v.y, z = v.z;
            v.y = z * s + y * c;
            v.z = z * c - y * s;
        }
    }
    static void turnY(ParticleRecord& r, float a) {
        const float s = sine(a), c = cosine(a);
        for (auto& v : r.v) {
            const float x = v.x, z = v.z;
            v.x = x * c - z * s;
            v.z = z * c + x * s;
        }
    }
    static void turnZ(ParticleRecord& r, float a) {
        const float s = sine(a), c = cosine(a);
        for (auto& v : r.v) {
            const float x = v.x, y = v.y;
            v.x = x * c + y * s;
            v.y = y * c - x * s;
        }
    }
    // Native 25 (0x55f4e0): every corner moved; 26 (0x55f410): every corner scaled.
    static void shift(ParticleRecord& r, float x, float y, float z) {
        for (auto& v : r.v) v.x += x, v.y += y, v.z += z;
    }
    static void stretch(ParticleRecord& r, float x, float y, float z) {
        for (auto& v : r.v) v.x *= x, v.y *= y, v.z *= z;
    }
    // Native 38 (0x55eeb0): the screen's rectangle at depth z (x0, y0, x1, y1): half the screen
    // (whole pixels) through the inverse of the draw's perspective (0x57ca10 with 65000 - (depth +
    // z) against the layer's 65000 - depth - 1000: scale 1000 / (1000 - z), 0 past 1000), about its
    // centre.
    std::array<float, 4> screenAt(float z) const {
        const float cx = static_cast<float>(static_cast<uint32_t>(params_.screenW) >> 1);
        const float cy = static_cast<float>(static_cast<uint32_t>(params_.screenH) >> 1);
        const float d = (65000.0f - (params_.depth + z)) - (65000.0f - params_.depth - 1000.0f);
        const float s = d != 0.0f && 1000.0f / d >= 0.0f ? 1000.0f / d : 0.0f;
        const float hx = s != 0.0f ? cx / s : 0.0f, hy = s != 0.0f ? cy / s : 0.0f;  // a singular matrix keeps (cx, cy)
        return {cx - hx, cy - hy, hx + cx, hy + cy};
    }
    void stop() { stopped_ = true; }

private:
    bool started_ = false, stopped_ = false;
    int32_t clock_ = 0, last_ = -1;
};

// p_fall2.kcs (Love's Sweet Garnish: `particle 0 p_fall2 <texture> 70 6000 67 2 5`): petals
// drifting down, spinning. f0: size (percent of the texture); f1: speed (tenths of a pixel a tick,
// plus 0-3); f2: the wind (degrees). Records past 100 are left alone (its arrays hold 100).
// Init: the quad centred on its position and scaled by f0 / 100, then a start. A start: spins of
// 0-0.1 radian a tick about x, y and z; a fall at the speed along b, 0.5 pi (1 - a) + rand x a with
// a = 40 degrees: x (sin f2 + cos b) x speed, y sin b x speed; then the fall turned by c (+-pi /
// 200): y (cos f2 + cos c) y, z -sin c y; placed at x (2 rand - 0.5) x screen width, y -2 rand x
// screen height, z 0. Update: each tick the fall added and the spins turned in; once its y is past
// the screen's bottom at its depth by 100, a new start.
class PFall2 final : public ScriptParticle {
public:
    using ScriptParticle::ScriptParticle;

private:
    void init(size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        shift(r, -((r.v[1].x - r.v[0].x) / 2.0f), -((r.v[2].y - r.v[0].y) / 2.0f), 0.0f);
        const float s = p(0) * 0.01f;
        stretch(r, s, s, 1.0f);
        start(i);
    }

    void start(size_t i) {
        ParticleRecord& r = records_[i];
        const float wind = 3.14159265f * p(2) / 180.0f;
        const float a = 3.14159265f * 40.0f / 180.0f;
        for (float& v : spin_[i]) v = rand() * 0.1f;
        const float speed = rand() * 3.0f + p(1) * 0.1f;
        const float b = rand() * a + 3.14159265f * (0.5f * (1.0f - a));
        auto& f = fall_[i];
        f[0] = (cosine(-b) + sine(wind)) * speed;
        f[1] = -sine(-b) * speed;
        const float y = f[1];
        const float c = rand() * 0.0314159282f - 0.0157079641f;
        f[1] = (cosine(c) + cosine(wind)) * y;
        f[2] = -sine(c) * y;
        r.pos[0] = params_.screenW * (rand() * 2.0f - 0.5f);
        r.pos[1] = -rand() * params_.screenH * 2.0f;
        r.pos[2] = 0.0f;
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        for (int32_t t = last; t < now; ++t) {
            r.pos[0] += fall_[i][0], r.pos[1] += fall_[i][1], r.pos[2] += fall_[i][2];
            turnX(r, spin_[i][0]);
            turnY(r, spin_[i][1]);
            turnZ(r, spin_[i][2]);
        }
        if (r.pos[1] > screenAt(r.pos[2])[3] + 100.0f) start(i);
    }

    std::array<std::array<float, 3>, 100> spin_{}, fall_{};  // m[0x98], m[0x548]
};

// burst03.kcs (Happiness 2, 4 loads: `particle 0 burst03 <texture> 12 20500 340 60 10 50 95 6 0 50
// 360 360`): stars bursting from a point. f0 f1: the point; f2 f3: the speed's range (pixels a
// tick); f4: the speed kept each tick (percent); f5: alpha lost each tick; f6: z added each tick; f7:
// the ticks it runs; f8: the spread (degrees) about f9 - 90. Records past 100 are left alone.
// Init: speed f2 + (f3 - f2) rand along f9 - 90 + (rand - 0.5) f8 degrees (x cos, y -sin), the
// quad centred on the point and turned about z by 2 pi rand. Update, each tick: the move added (z by
// f6), the speed times f4 / 100, the alpha less f5 (not below 0) on every corner; then, once the
// clock reaches f7, the layer stops (native 32).
class Burst03 final : public ScriptParticle {
public:
    using ScriptParticle::ScriptParticle;

private:
    void init(size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        float speed = (p(3) - p(2)) * rand();
        speed += p(2);
        const float spread = radians(p(8));
        const float way = rand() * spread - spread * 0.5f + radians(p(9) - 90.0f);
        move_[i] = {cosine(way) * speed, -sine(way) * speed};
        const float w = r.v[1].x - r.v[0].x, h = r.v[2].y - r.v[0].y;
        shift(r, -w / 2.0f, -h / 2.0f, 0.0f);
        r.pos[0] = p(0), r.pos[1] = p(1);
        turnZ(r, rand() * 3.14159265f * 2.0f);
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        const int32_t fade = static_cast<int32_t>(p(5));  // opcode 0x38 (0x6ac450)
        for (int32_t t = last; t < now; ++t) {
            r.pos[0] += move_[i][0], r.pos[1] += move_[i][1], r.pos[2] += p(6);
            const float keep = p(4) * 0.01f;
            move_[i][0] *= keep, move_[i][1] *= keep;
            int32_t a = static_cast<int32_t>(r.v[0].alpha) - fade;
            if (a < 0) a = 0;
            for (auto& v : r.v) v.alpha = static_cast<uint8_t>(a);
        }
        if (now >= static_cast<int32_t>(p(7))) stop();
    }

    std::array<std::array<float, 2>, 100> move_{};  // m[0x98], m[0x228]
};

// wind2.kcs (Happiness 2, 2 scenes: `particle 0 wind2 <texture> 100 9500 10 30 0 1` and two more
// layers at +-15 degrees): motes carried up by the wind. f0 f1: the speed's range (pixels a tick);
// f2: the wind's way (degrees from straight up). Records past 300 are left alone.
// Init and each start: a depth z of 3000 rand - 2900 for every corner, the screen's rectangle at that
// depth kept, a place across it (x from x0 + (2 rand - 0.5) (x1 - x0), y one rectangle below a random
// height in it). Init also gives the speed (f0 + (f1 - f0) rand) and turns the quad (not centred)
// about z by f2. Update: the move for the ticks since the last update (x by sin f2, y by -cos f2,
// times the speed); once above its rectangle's top, a new start.
class Wind2 final : public ScriptParticle {
public:
    using ScriptParticle::ScriptParticle;

private:
    void init(size_t i) override {
        if (i >= 300) return;
        ParticleRecord& r = records_[i];
        speed_[i] = (p(1) - p(0)) * rand() + p(0);
        way_ = radians(p(2));
        start(i);
        turnZ(r, way_);
    }

    void start(size_t i) {
        ParticleRecord& r = records_[i];
        const float z = rand() * 3000.0f - 2900.0f;
        box_[i] = screenAt(z);
        const auto& b = box_[i];
        r.pos[0] = (rand() * 2.0f - 0.5f) * (b[2] - b[0]) + b[0];
        r.pos[1] = rand() * (b[3] - b[1]) + b[1];
        r.pos[1] += b[3] - b[1];
        for (auto& v : r.v) v.z = z;
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 300) return;
        ParticleRecord& r = records_[i];
        const float ticks = static_cast<float>(static_cast<uint32_t>(now - last));
        r.pos[0] += sine(way_) * (ticks * speed_[i]);
        r.pos[1] -= cosine(way_) * (ticks * speed_[i]);
        if (r.pos[1] < box_[i][1]) start(i);
    }

    float way_ = 0;                             // m[0x548]
    std::array<float, 300> speed_{};            // m[0x98]
    std::array<std::array<float, 4>, 300> box_{};  // m[0x54c] x0, m[0xeac] y0, m[0x9fc] x1, m[0x135c] y1
};

// The version-2 scripts (their header's second word 2: natives 0x9952a0, version 1's plus 44-50;
// Amakano 1's cs2.exe 0x7c0ec0, REBELLIONS's 0x710f90 the same). Natives they add: 16 fmod (CRT
// 0x80acca, its x87 remainder loop), 39 a record's draw flag, 40 the record count less 1, 41 a
// clamped lerp (a, b, t0, t1, t: a up to t0, b from t1), 49 the layer's plane node's position (0
// without a plane: Nyanroid's plane position, as the plugins get it), 50 the blend (0-5). `particle2`
// reads <count> <depth> <plane> before the floats in every sscript (Amakano 1 0x071ee4, REBELLIONS
// 0x064f41: one argument counter), so Amakano 1's snow lines, written without a plane, give the
// script their -2500 as the plane and their floats from the next one on.
class ScriptParticle2 : public ScriptParticle {
public:
    using ScriptParticle::ScriptParticle;

protected:
    static float fmodf32(float a, float b) { return static_cast<float>(std::fmod(static_cast<double>(a), static_cast<double>(b))); }
    void setBlend(int32_t b) { blend_ = b >= 1 && b <= 5 ? b : 0; }
};

// snow.kcs (Amakano 1, 8 loads: `particle2 0 snow <texture> 50 5000 -2500 10 5 10 1 5 20 40 2 2 300 300
// 1`): flakes falling and swaying. f0 f1: depth range; f2 f3: sway width; f4 f5: sway speed (degrees
// a tick, tenths); f6 f7: fall speed (tenths of a pixel a tick); f8: the wind (degrees); f9: how
// often a gust starts (the clock modulo the screen height below f9 (rand + 0.1) height / 1000);
// f10: a gust's push (tenths); f11: its ticks; f12 1: additive. Records past 300 are left alone.
// A start: width, sway speed and fall speed from their ranges, sway 0, a depth f0 + (f1 - f0) (0.5 +
// 0.5 cos 2 pi rand) on every corner, the screen's rectangle there kept, a place across it one
// rectangle above a random height, a random turn about z, the place's x kept as the flake's base.
// Init: a start, then the wind's turn about z and a calm (gust counter 0, a new threshold and push).
// Update: in a gust (counter > 0) the base moves by push x (0.5 - 0.5 cos (2 pi counter / f11)) and
// the counter by the ticks, until f11, then a calm; out of one, a gust starts when the clock modulo
// the height falls under the threshold. Then the base moves by sin(wind) x fall x ticks, the sway by
// its speed x ticks, x is the base plus width x sin(sway), y moves by cos(wind) x fall x ticks; x
// wraps over the screen widened by 800 each side; below its rectangle, a new start.
class SnowScript final : public ScriptParticle2 {
public:
    using ScriptParticle2::ScriptParticle2;

private:
    void init(size_t i) override {
        if (i >= 300) return;
        ParticleRecord& r = records_[i];
        if (p(12) == 1.0f && i == 0) setBlend(1);
        start(i);
        wind_ = 3.14159265f * p(8) / 180.0f;
        turnZ(r, wind_);
        calm(i);
    }

    void start(size_t i) {
        ParticleRecord& r = records_[i];
        width_[i] = (p(3) - p(2)) * rand() + p(2);
        swaySpeed_[i] = radians((p(5) - p(4)) * rand() + p(4)) / 10.0f;
        fall_[i] = ((p(7) - p(6)) * rand() + p(6)) / 10.0f;
        sway_[i] = 0.0f;
        const float a = rand() * 2.0f * 3.14159265f;
        const float k = cosine(a) * 0.5f + 0.5f;
        const float z = k * (p(1) - p(0)) + p(0);
        box_[i] = screenAt(z);
        const auto& b = box_[i];
        r.pos[0] = (rand() * 2.0f - 0.5f) * (b[2] - b[0]) + b[0];
        r.pos[1] = rand() * (b[3] - b[1]) + b[1];
        r.pos[1] -= b[3] - b[1];
        for (auto& v : r.v) v.z = z;
        turnZ(r, rand() * 2.0f * 3.14159265f);
        base_[i] = r.pos[0];
    }

    void calm(size_t i) {
        gust_[i] = 0;
        threshold_ = (rand() + 0.1f) * p(9) * params_.screenH / 1000.0f;
        push_[i] = (rand() * 0.2f + 0.8f) * p(10) / 10.0f;
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 300) return;
        ParticleRecord& r = records_[i];
        const float ticks = static_cast<float>(static_cast<uint32_t>(now - last));
        if (gust_[i] > 0) {
            if (static_cast<float>(gust_[i]) < p(11)) {
                const float t = static_cast<float>(gust_[i]) * 2.0f * 3.14159265f / p(11);
                base_[i] += (cosine(t) * -0.5f + 0.5f) * push_[i];
                gust_[i] += static_cast<int32_t>(ticks);
            } else {
                calm(i);
            }
        } else if (fmodf32(static_cast<float>(static_cast<uint32_t>(now)), params_.screenH) < threshold_) {
            gust_[i] = 1;
        }
        base_[i] += sine(wind_) * (ticks * fall_[i]);
        sway_[i] += swaySpeed_[i] * ticks;
        r.pos[0] = base_[i] + sine(sway_[i]) * width_[i];
        r.pos[1] += cosine(wind_) * (ticks * fall_[i]);
        r.pos[0] = fmodf32(r.pos[0] + 800.0f, params_.screenW + 1600.0f) - 800.0f;
        if (r.pos[0] < -800.0f) r.pos[0] += params_.screenW + 800.0f;
        if (r.pos[1] > box_[i][3]) start(i);
    }

    float wind_ = 0, threshold_ = 0;  // m[0x1358], m[0x135c]
    std::array<float, 300> width_{}, swaySpeed_{}, fall_{}, sway_{}, push_{}, base_{};  // m[0x98], 0x548, 0x9f8, 0xea8, 0x1810, 0x1cc0
    std::array<int32_t, 300> gust_{};                                                  // m[0x1360]
    std::array<std::array<float, 4>, 300> box_{};                                      // m[0x2170] on
};

// rain.kcs (REBELLIONS, 16 loads: `particle2 0 rain <texture> 100 4900 0 -850 -50 0 0 0 0 1200 1250 -10
// 0 0 0 1`): snow's sibling without the sway. f0 f1: depth range; f6 f7: fall speed (tenths); f8:
// the wind; f9-f11: gusts as snow's; f12 > 0: additive. Records past 1000 are left alone. Init: the
// screen's rectangle at the depth the last start chose (0 at first) kept in globals, a start, the
// quad stretched to twice its height (its lower corners' y doubled), the wind's turn about z, a calm.
// A start: fall speed, a depth for every corner (kept for the next init's rectangle), the globals'
// rectangle as the drop's (its bottom lowered by the quad's height), a place across it; the x kept
// as the base. Update: gusts as snow's; the base and y move with the wind and fall; x is the base
// wrapped as snow's; below the rectangle, a new start, then up by the rectangle's height.
class RainScript final : public ScriptParticle2 {
public:
    using ScriptParticle2::ScriptParticle2;

private:
    void init(size_t i) override {
        if (i >= 1000) return;
        ParticleRecord& r = records_[i];
        if (p(12) > 0.0f && i == 0) setBlend(1);
        box_ = screenAt(depth_);
        start(i);
        r.v[2].y *= 2.0f;
        r.v[3].y *= 2.0f;
        wind_ = 3.14159265f * p(8) / 180.0f;
        turnZ(r, wind_);
        calm(i);
    }

    void start(size_t i) {
        ParticleRecord& r = records_[i];
        fall_[i] = ((p(7) - p(6)) * rand() + p(6)) / 10.0f;
        depth_ = rand() * 2.0f * 3.14159265f;
        const float k = cosine(depth_) * 0.5f + 0.5f;
        depth_ = k * (p(1) - p(0)) + p(0);
        for (auto& v : r.v) v.z = depth_;
        x0_[i] = box_[0], y0_[i] = box_[1], x1_[i] = box_[2];
        y1_[i] = box_[3] + r.v[3].y;
        r.pos[0] = (rand() * 2.0f - 0.5f) * (box_[2] - box_[0]) + box_[0];
        r.pos[1] = rand() * (y1_[i] - box_[1]) + box_[1];
        base_[i] = r.pos[0];
    }

    void calm(size_t i) {
        gust_[i] = 0;
        threshold_ = (rand() + 0.1f) * p(9) * params_.screenH / 1000.0f;
        push_[i] = (rand() * 0.2f + 0.8f) * p(10) / 10.0f;
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 1000) return;
        ParticleRecord& r = records_[i];
        const float ticks = static_cast<float>(static_cast<uint32_t>(now - last));
        if (gust_[i] > 0) {
            if (static_cast<float>(gust_[i]) < p(11)) {
                const float t = static_cast<float>(gust_[i]) * 2.0f * 3.14159265f / p(11);
                base_[i] += (cosine(t) * -0.5f + 0.5f) * push_[i];
                gust_[i] += static_cast<int32_t>(ticks);
            } else {
                calm(i);
            }
        } else if (fmodf32(static_cast<float>(static_cast<uint32_t>(now)), params_.screenH) < threshold_) {
            gust_[i] = 1;
        }
        base_[i] += sine(wind_) * (ticks * fall_[i]);
        r.pos[1] += cosine(wind_) * (ticks * fall_[i]);
        r.pos[0] = fmodf32(base_[i] + 800.0f, params_.screenW + 1600.0f) - 800.0f;
        if (r.pos[0] < -800.0f) r.pos[0] += params_.screenW + 800.0f;
        if (r.pos[1] > y1_[i]) {
            start(i);
            r.pos[1] -= y1_[i] - box_[1];
        }
    }

    float wind_ = 0, threshold_ = 0, depth_ = 0;  // m[0x3f2c], m[0x3f30], m[0xa8]
    std::array<float, 4> box_{};                  // m[0x98] - m[0xa4]
    std::array<float, 1000> fall_{}, push_{}, base_{}, x0_{}, x1_{}, y0_{}, y1_{};  // m[8172], 20180, 24180, 28180, 32180, 36180, 40180
    std::array<int32_t, 1000> gust_{};                                              // m[16180]
};

// spark.kcs (Amakano 1, 1 load: `particle2 0 spark <texture> 10 22600 0 -500 500 1200 900 60 120 30 90
// 1500 -300 10 3 800 60 60 6 0 1 2 1 5 1 1`): sparks rising from the layer's plane. f0: the way
// (degrees, plus 0-180, less 90); f1: sideways spread and the rise; f2: the rise's ticks; f3: a
// spark's ticks. Additive. Records past 100 are left alone. Init: hidden (native 39), the quad
// centred, a start; its tick counter starts at -(2 f3 i / (count - 2)) (whole numbers), so the
// sparks come one after another. A start: a sideways place within +-f1 / 2, the plane's position, a
// wobble of up to f3 / 2 at 1.5-2.5 cycles a life, the way. Update: the counter moves by the ticks;
// once 0 or more the spark shows, and from f3 the counter wraps (modulo) and it starts again; its
// place is (sideways + wobble x sin(cycles x counter / f3), the rise lerped from 0 to -f1 over f2
// ticks) turned about z by the way (native 24 on that vector), from the plane's position; its alpha
// 255 down to 0 over f3 ticks.
class SparkScript final : public ScriptParticle2 {
public:
    using ScriptParticle2::ScriptParticle2;

private:
    void init(size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        if (i == 0) setBlend(1);
        const int32_t life = static_cast<int32_t>(p(3));
        const int32_t last = static_cast<int32_t>(records_.size()) - 1 - 1;
        tick_[i] = last != 0 ? -((life * 2 * static_cast<int32_t>(i)) / last) : 0;
        r.draw = 0;
        const float w = r.v[1].x - r.v[0].x, h = r.v[2].y - r.v[0].y;
        shift(r, -w / 2.0f, -h / 2.0f, 0.0f);
        start(i);
    }

    void start(size_t i) {
        side_[i] = p(1) * rand();
        side_[i] -= p(1) * 0.5f;
        from_[i] = {planePos_[0], planePos_[1]};
        wobble_[i] = p(3) * rand() * 0.5f;
        cycles_[i] = rand() + 1.5f;
        way_[i] = (radians(p(0)) + rand() * radians(180.0f)) - radians(90.0f);
    }

    static float lerp(float a, float b, float t0, float t1, float t) {
        if (t <= t0) return a;
        if (t1 <= t) return b;
        return (t - t0) / (t1 - t0) * (b - a) + a;
    }

    void update(int32_t last, int32_t now, size_t i) override {
        if (i >= 100) return;
        ParticleRecord& r = records_[i];
        tick_[i] += now - last;
        if (tick_[i] < 0) return;
        const int32_t life = static_cast<int32_t>(p(3));
        if (tick_[i] >= life) {
            if (life != 0) tick_[i] %= life;  // opcode 0x1b
            start(i);
        }
        r.draw = 1;
        const float t = static_cast<float>(tick_[i]);
        float x = sine(t / p(3) * cycles_[i]) * wobble_[i] + side_[i];
        float y = lerp(0.0f, -p(1), 0.0f, p(2), t);
        const float s = sine(way_[i]), c = cosine(way_[i]);
        const float tx = x * c + y * s, ty = y * c - x * s;  // native 24 on the vector
        r.pos[0] = from_[i][0] + tx;
        r.pos[1] = from_[i][1] + ty;
        r.pos[2] = 0.0f;
        const auto a = static_cast<uint8_t>(static_cast<int32_t>(lerp(255.0f, 0.0f, 0.0f, p(3), t)));
        for (auto& v : r.v) v.alpha = a;
    }

    std::array<int32_t, 100> tick_{};                        // m[0x548]
    std::array<float, 100> side_{}, wobble_{}, cycles_{}, way_{};  // m[0x3b8], 0x6d8, 0x868, 0x9f8
    std::array<std::array<float, 2>, 100> from_{};           // m[0x98], m[0x228]
};

}  // namespace

ParticleEffect::ParticleEffect(const ParticleParams& params, uint32_t seed)
    : params_(params), rng_(seed), planePos_(params.planePos) {
    // The engine's defaults for every record (0x587df0): the whole texture on a texture-sized quad,
    // white and opaque, untransformed.
    ParticleRecord r;
    float w = params.texW, h = params.texH;
    r.v[0] = {0, 0, 0, 255, 0, 0};
    r.v[1] = {w, 0, 0, 255, 1, 0};
    r.v[2] = {0, h, 0, 255, 0, 1};
    r.v[3] = {w, h, 0, 255, 1, 1};
    records_.assign(static_cast<size_t>(std::max(0, params.count)), r);
}

bool ParticleEffect::known(const std::string& name) {
    std::string n = lower(name);
    return n == "matrix_rain" || n == "sakura" || n == "em_note" || n == "em_sweat" || n == "em_question" ||
           n == "em_question_a" || n == "em_double" || n == "em_angry" || n == "em_notice" || n == "em_oko" || n == "em_common" || n == "em_dis" ||
           n == "em_suprise" || n == "em_sigh" || n == "em_down" ||
           n == "em_silent" || n == "em_exclamation" ||
           n == "em_kyupin" || n == "em_shy" || n == "asease" || n == "ohanaburst" || n == "kirakira" ||
           n == "warukira" || n == "concent" || n == "digi" || n == "digiring" || n == "dark_tri" || n == "wc_dark" || n == "filmnoise" ||
           n == "wc_filmnoise" || n == "energy" || n == "steam" || n == "wc_steam" || n == "dash" || n == "rising4" ||
           n == "ring" || n == "wc_aoi" || n == "hadou" || n == "wc_hadou" || n == "wc_mirror" ||
           n == "wc_prism" || n == "rising3" || n == "paper" || n == "wc_paper" ||
           n == "wc_slash" || n == "rubble" || n == "wc_thunder" || n == "protect" ||
           n == "star" || n == "o_150102" || n == "exprode" || n == "cmnptcl" || n == "radiate" || n == "rain" ||
           n == "spark2" || n == "s_040101" || n == "wc_roundz" || n == "wc_roundy" ||
           n == "swordspark" || n == "s_arrow";
}

bool ParticleEffect::knownScript(const std::string& name) {
    const std::string n = lower(name);
    return n == "p_fall2" || n == "burst03" || n == "wind2" || n == "snow" || n == "rain" || n == "spark";
}

std::unique_ptr<ParticleEffect> ParticleEffect::createScript(const std::string& name, const ParticleParams& params, uint32_t seed) {
    const std::string n = lower(name);
    if (n == "p_fall2") return std::make_unique<PFall2>(params, seed);
    if (n == "burst03") return std::make_unique<Burst03>(params, seed);
    if (n == "wind2") return std::make_unique<Wind2>(params, seed);
    if (n == "snow") return std::make_unique<SnowScript>(params, seed);
    if (n == "rain") return std::make_unique<RainScript>(params, seed);
    if (n == "spark") return std::make_unique<SparkScript>(params, seed);
    return nullptr;
}

std::unique_ptr<ParticleEffect> ParticleEffect::create(const std::string& name, const ParticleParams& params, uint32_t seed,
                                                       std::shared_ptr<PluginGlobals> shared) {
    std::string n = lower(name);
    if (n == "matrix_rain") return std::make_unique<MatrixRain>(params, seed);
    if (n == "sakura") return std::make_unique<Sakura>(params, seed);
    if (n == "em_note") return std::make_unique<EmNote>(params, seed);
    if (n == "em_sweat") return std::make_unique<EmSweat>(params, seed);
    if (n == "em_question") return std::make_unique<EmQuestion>(params, seed, true);
    if (n == "em_question_a") return std::make_unique<EmQuestion>(params, seed, false);
    if (n == "em_double") return std::make_unique<EmDouble>(params, seed);
    if (n == "em_angry") return std::make_unique<EmAngry>(params, seed);
    if (n == "em_notice") return std::make_unique<EmNotice>(params, seed);
    if (n == "em_oko") return std::make_unique<EmOko>(params, seed);
    if (n == "em_common") return std::make_unique<EmCommon>(params, seed);
    if (n == "em_dis") return std::make_unique<EmDis>(params, seed);
    if (n == "em_suprise") return std::make_unique<EmSuprise>(params, seed);
    if (n == "em_sigh") return std::make_unique<EmSigh>(params, seed);
    if (n == "em_down") return std::make_unique<EmDown>(params, seed);
    if (n == "em_silent") return std::make_unique<EmSilent>(params, seed);
    if (n == "em_exclamation") return std::make_unique<EmExclamation>(params, seed);
    if (n == "em_kyupin") return std::make_unique<EmKyupin>(params, seed);
    if (n == "em_shy") return std::make_unique<EmShy>(params, seed);
    if (n == "asease") return std::make_unique<Asease>(params, seed);
    if (n == "ohanaburst") return std::make_unique<Ohanaburst>(params, seed);
    if (n == "kirakira") return std::make_unique<Kirakira>(params, seed, true);
    if (n == "warukira") return std::make_unique<Kirakira>(params, seed, false);
    if (n == "concent") return std::make_unique<Concent>(params, seed, shared);
    if (n == "digi") return std::make_unique<Digi>(params, seed);
    if (n == "digiring") return std::make_unique<DigiRing>(params, seed);
    if (n == "dark_tri") return std::make_unique<DarkTri>(params, seed);
    if (n == "wc_dark") return std::make_unique<DarkTri>(params, seed, true);
    if (n == "filmnoise" || n == "wc_filmnoise") return std::make_unique<FilmNoise>(params, seed);
    if (n == "energy") return std::make_unique<Energy>(params, seed);
    if (n == "steam") return std::make_unique<Steam>(params, seed);
    if (n == "wc_steam") return std::make_unique<Steam>(params, seed, true);
    if (n == "dash") return std::make_unique<Dash>(params, seed, shared);
    if (n == "rising4") return std::make_unique<Rising4>(params, seed);
    if (n == "rising3") return std::make_unique<Rising4>(params, seed, true);
    if (n == "paper" || n == "wc_paper") return std::make_unique<Paper>(params, seed, shared);
    if (n == "wc_slash") return std::make_unique<WcSlash>(params, seed);
    if (n == "rubble") return std::make_unique<Rubble>(params, seed);
    if (n == "wc_thunder") return std::make_unique<WcThunder>(params, seed, shared);
    if (n == "protect") return std::make_unique<Protect>(params, seed);
    if (n == "star") return std::make_unique<Star>(params, seed, false);
    if (n == "o_150102") return std::make_unique<Star>(params, seed, true);
    if (n == "exprode") return std::make_unique<Exprode>(params, seed, shared);
    if (n == "cmnptcl") return std::make_unique<Cmnptcl>(params, seed);
    if (n == "radiate") return std::make_unique<Radiate>(params, seed);
    if (n == "rain") return std::make_unique<Rain>(params, seed);
    if (n == "spark2") return std::make_unique<Spark2>(params, seed);
    if (n == "s_040101") return std::make_unique<S040101>(params, seed);
    if (n == "wc_roundz") return std::make_unique<WcRound>(params, seed, false);
    if (n == "wc_roundy") return std::make_unique<WcRound>(params, seed, true);
    if (n == "swordspark") return std::make_unique<SwordSpark>(params, seed);
    if (n == "s_arrow") return std::make_unique<SArrow>(params, seed, shared);
    if (n == "ring" || n == "wc_aoi") return std::make_unique<RingWave>(params, seed, false);
    if (n == "hadou" || n == "wc_hadou") return std::make_unique<RingWave>(params, seed, true);
    if (n == "wc_mirror") return std::make_unique<WcMirror>(params, seed, 2);
    if (n == "wc_prism") return std::make_unique<WcMirror>(params, seed, 4);
    return nullptr;
}

namespace {

// cs2.exe's sort of the records' draw order (Happiness 0x6b1a50, Amakano 2 0x73d390): K&R's
// quicksort, the middle entry taken as the pivot and its place given the first's, the entries
// under it swapped forward one by one, then the left part by recursion and the right by the loop.
// It is not stable, so records at one depth are drawn in its order, not theirs. The comparator
// (0x4feca0 / 0x5888c0): -1 when the difference is below 0, 1 above, else 0.
void engineSort(std::vector<size_t>& a, const std::vector<float>& key, int lo, int hi) {
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const size_t pivot = a[static_cast<size_t>(mid)];
        a[static_cast<size_t>(mid)] = a[static_cast<size_t>(lo)];
        int last = lo;
        for (int i = lo + 1; i <= hi; ++i) {
            if (key[a[static_cast<size_t>(i)]] - key[pivot] < 0.0f) {
                ++last;
                std::swap(a[static_cast<size_t>(last)], a[static_cast<size_t>(i)]);
            }
        }
        a[static_cast<size_t>(lo)] = a[static_cast<size_t>(last)];
        a[static_cast<size_t>(last)] = pivot;
        engineSort(a, key, lo, last - 1);
        lo = last + 1;
    }
}

}  // namespace

std::vector<ParticleQuad> projectParticles(const ParticleEffect& effect, float screenW, float screenH, int32_t layerAlpha) {
    const auto& recs = effect.records();
    std::vector<size_t> order(recs.size());
    std::iota(order.begin(), order.end(), 0);
    // Each record's depth (Amakano 2 0x5871ed, SSE floats): pivot z + position z + corner 0's z.
    std::vector<float> key(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) key[i] = recs[i].pivot[2] + recs[i].pos[2] + recs[i].v[0].z;
    engineSort(order, key, 0, static_cast<int>(recs.size()) - 1);
    const float cx = screenW * 0.5f, cy = screenH * 0.5f;
    std::vector<ParticleQuad> out;
    out.reserve(recs.size());
    for (size_t i : order) {
        const ParticleRecord& r = recs[i];
        if (r.draw == 0) continue;  // the layer's +0x32c (Happiness 0x4fcf70)
        Mat m = identity();
        m = mul(m, scaling(r.scale[0], r.scale[1], r.scale[2]));
        m = mul(m, rotationX(std::cos(r.rot[0]), std::sin(r.rot[0])));
        m = mul(m, rotationY(std::cos(r.rot[1]), std::sin(r.rot[1])));
        m = mul(m, rotationZ(std::cos(r.rot[2]), std::sin(r.rot[2])));
        m = mul(m, translation(r.pos[0], r.pos[1], r.pos[2]));
        ParticleQuad q;
        for (size_t k = 0; k < 4; ++k) {
            auto v = transform({r.v[k].x - r.pivot[0], r.v[k].y - r.pivot[1], r.v[k].z - r.pivot[2]}, m);
            float den = 1000.0f - v[2];
            float s = std::max(0.0f, 1000.0f / (den == 0 ? 1.0f : den));
            q.c[k] = {(v[0] - cx) * s + cx, (v[1] - cy) * s + cy};
            q.alpha[k] = static_cast<uint8_t>((static_cast<int32_t>(r.v[k].alpha) * layerAlpha) >> 8);
            q.rgb[k] = static_cast<uint32_t>(r.v[k].r) << 16 | static_cast<uint32_t>(r.v[k].g) << 8 | r.v[k].b;
        }
        q.u0 = r.v[0].u, q.v0 = r.v[0].v, q.u1 = r.v[3].u, q.v1 = r.v[3].v;
        out.push_back(q);
    }
    return out;
}

}  // namespace nyan::cs2
