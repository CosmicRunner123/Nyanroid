#include "nyan/diesel/split.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

#include "nyan/diesel/random.hpp"

namespace nyan::diesel {

namespace {

using V3 = SplitFace::V3;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// XMVector3Normalize: a zero vector stays zero.
V3 normalized(const V3& a) {
    float l = std::sqrt(dot(a, a));
    return l > 0.0f ? scale(a, 1.0f / l) : V3{};
}

// The cut sizes along one side (0x454550): split the largest part (sorted largest first) n - 1
// times at a random place at least `minFirst` and 5 pixels from its ends, drop empty parts, fold
// tail parts under 5 pixels into the one before, shuffle.
std::vector<int32_t> randomCuts(MwareRandom& r, int32_t total, int32_t n, int32_t minFirst) {
    std::vector<int32_t> s(static_cast<size_t>(std::max(n, 1)), 0);
    s[0] = total;
    for (int32_t i = 0; i < n - 1; ++i) {
        int32_t cut = r.nextInt(minFirst, s[0] - 5);
        s[static_cast<size_t>(i) + 1] = s[0] - cut;
        s[0] = cut;
        std::sort(s.begin(), s.end(), std::greater<int32_t>());
    }
    std::erase(s, 0);
    for (size_t k = s.size(); k > 1 && s[k - 1] < 5; --k) {
        s[k - 2] += s[k - 1];
        s[k - 1] = 0;
    }
    std::erase(s, 0);
    // std::random_shuffle with Mware's generator (0x4745a0).
    for (size_t i = 1; i < s.size(); ++i) {
        auto j = static_cast<size_t>(r.nextInt(0, static_cast<int32_t>(i)));
        std::swap(s[i], s[j]);
    }
    return s;
}

// Even cuts (no distortion): the size divided, the remainder one pixel each from the first.
std::vector<int32_t> evenCuts(int32_t total, int32_t n) {
    std::vector<int32_t> s(static_cast<size_t>(std::max(n, 0)), n > 0 ? total / n : 0);
    if (n > 0) {
        for (int32_t u = 0; u < total % n; ++u) s[static_cast<size_t>(u % n)] += 1;
    }
    return s;
}

// A group centre's place for a term (0x45a8d0 / 0x45ade0): from the term's centre, in units of the
// picture's smaller side (or each side with `aspect`), doubled; its squared distances along the
// direction and across it.
struct Placed {
    V3 p{};
    float along2 = 0.0f, across2 = 0.0f;
};
Placed placeFor(const V3& c, const V3& center, const V3& dir, int32_t aspect, int width, int height) {
    Placed o;
    V3 p = c;
    p[2] = p[2] - center[2];
    if (!aspect) {
        float s = 1.0f / static_cast<float>(std::min(width, height));
        p[2] = p[2] * s;
        p[0] = (p[0] - center[0]) * s;
        p[1] = (p[1] - center[1]) * s;
    } else {
        p[0] = (p[0] - center[0]) / static_cast<float>(width);
        p[1] = (p[1] - center[1]) / static_cast<float>(height);
    }
    p = scale(p, 2.0f);
    V3 across = cross(dir, {0.0f, 0.0f, 1.0f});
    float a = std::fabs(dot(dir, p)), b = std::fabs(dot(across, p));
    o.p = p;
    o.along2 = a * a;
    o.across2 = b * b;
    return o;
}

// A motion term for one group (0x45a8d0): the outward direction (z rising as on a dome), each axis
// sized by e^-(across^2 side + along^2 front) (or one minus it) between dist_min and dist_max plus
// its random part, plus random offsets, times the mask.
V3 evalTerm(const SplitFace::Term& t, const V3& c, int width, int height, MwareRandom& r) {
    Placed pl = placeFor(c, t.center, t.dir, t.aspect, width, height);
    V3 out = normalized(pl.p);
    out[2] = std::sqrt(std::max(1.0f - dot(pl.p, pl.p), 0.0f));
    for (size_t i = 0; i < 3; ++i) {
        float f = std::exp(-(pl.across2 * t.edge[1] + pl.along2 * t.edge[0]));
        if (t.inv) f = 1.0f - f;
        float base = t.distMin;
        if (t.distRandMin < t.distRandMax) base = r.nextFloat(t.distRandMin, t.distRandMax) + base;
        out[i] = (f * (t.distMax - t.distMin) + base) * out[i];
    }
    if (t.randMin[0] < t.randMax[0]) out[0] = r.nextFloat(t.randMin[0], t.randMax[0]) + out[0];
    else if (t.randMin[0] == t.randMax[0]) out[0] = t.randMin[0] + out[0];
    if (t.randMin[1] < t.randMax[1]) out[1] = r.nextFloat(t.randMin[1], t.randMax[1]) + out[1];
    else if (t.randMin[1] == t.randMax[1]) out[1] = t.randMin[1] + out[1];
    // (The exe tests y's range for z's fixed offset.)
    if (t.randMin[2] < t.randMax[2]) out[2] = r.nextFloat(t.randMin[2], t.randMax[2]) + out[2];
    else if (t.randMin[1] == t.randMax[1]) out[2] = t.randMin[2] + out[2];
    for (size_t i = 0; i < 3; ++i) out[i] = t.mask[i] * out[i];
    return out;
}

float evalDelay(const SplitFace::DelayTerm& t, const V3& c, int width, int height, MwareRandom& r) {
    Placed pl = placeFor(c, t.center, t.dir, t.aspect, width, height);
    float f = std::exp(-(t.edge[1] * pl.across2 + t.edge[0] * pl.along2));
    if (t.inv) f = 1.0f - f;
    float base = t.distMin;
    if (t.distRandMin < t.distRandMax) base = r.nextFloat(t.distRandMin, t.distRandMax) + base;
    float v = f * (t.distMax - t.distMin) + base;
    if (t.randMin < t.randMax) v = r.nextFloat(t.randMin, t.randMax) + v;
    return t.mask * v;
}

// A state's motion t seconds in (0x450390 / 0x44ff50): with resistance k the velocity tends to
// a k (v = a k + (v0 - a k) e^-kt, the place adding a k t + (v0 - a k)(1 - e^-kt) / k to its
// start); without, v0 t + a t^2 / 2 (from 0: the exe leaves the start out there).
void motion(const V3& start, const V3& v0, const V3& a, float k, float t, V3& pos, V3* vel) {
    if (k <= 0.0f) {
        pos = add(scale(a, t * t * 0.5f), scale(v0, t));
        if (vel) *vel = add(scale(a, t), v0);
        return;
    }
    V3 ak = scale(a, k);
    float e = std::exp(-(k * t));
    V3 d = sub(v0, ak);
    pos = add(add(start, scale(ak, t)), scale(d, (1.0f - e) / k));
    if (vel) *vel = add(ak, scale(d, e));
}

}  // namespace

void SplitFace::split(int32_t r, int32_t c, float link, int32_t s, int32_t noDist, int width, int height) {
    if (rows < r && cols < c) {
        // (+0xc54: the larger grid's flag; nothing here reads it.)
    }
    if (rows == r && cols == c && linkRate == link && seed == s && noDistortion == noDist) return;
    rows = r, cols = c, linkRate = link, seed = s, noDistortion = noDist;
    overflow = 0.0f;
    // The cells (0x454550).
    pieces.clear();
    groups.clear();
    actions.clear();
    if ((rows <= 1 && cols <= 1) || width <= 0 || height <= 0) return;
    if (cols <= 0 || rows <= 0) return;
    if (width / cols < 15) cols = width / 15;
    if (height / rows < 15) rows = height / 15;
    if (cols <= 0 || rows <= 0) return;
    MwareRandom rnd(seed);
    std::vector<V2> jag(static_cast<size_t>((cols + 1) * (rows + 1)), V2{});
    std::vector<int32_t> widths, heights;
    if (!noDistortion) {
        widths = randomCuts(rnd, width, cols, 5);
        cols = static_cast<int32_t>(widths.size());
        heights = randomCuts(rnd, height, rows, 1);
        rows = static_cast<int32_t>(heights.size());
        jag.assign(static_cast<size_t>((cols + 1) * (rows + 1)), V2{});
        for (int32_t j = 0; j <= rows; ++j) {
            for (int32_t i = 0; i <= cols; ++i) {
                V2& v = jag[static_cast<size_t>((cols + 1) * j + i)];
                v[0] = (i == 0 || i == cols)
                           ? 0.0f
                           : static_cast<float>(rnd.nextInt(static_cast<int32_t>(static_cast<float>(widths[static_cast<size_t>(i) - 1]) * -0.4f),
                                                            static_cast<int32_t>(static_cast<float>(widths[static_cast<size_t>(i)]) * 0.4f)));
                v[1] = (j == 0 || j == rows)
                           ? 0.0f
                           : static_cast<float>(rnd.nextInt(static_cast<int32_t>(static_cast<float>(heights[static_cast<size_t>(j) - 1]) * -0.4f),
                                                            static_cast<int32_t>(static_cast<float>(heights[static_cast<size_t>(j)]) * 0.4f)));
            }
        }
    } else {
        widths = evenCuts(width, cols);
        heights = evenCuts(height, rows);
    }
    pieces.resize(static_cast<size_t>(cols * rows));
    float y = 0.0f;
    for (int32_t j = 0; j < rows; ++j) {
        float h = static_cast<float>(heights[static_cast<size_t>(j)]);
        float cy = h * 0.5f + y;
        float x = 0.0f;
        for (int32_t i = 0; i < cols; ++i) {
            Piece& p = pieces[static_cast<size_t>(cols * j + i)];
            float w = static_cast<float>(widths[static_cast<size_t>(i)]);
            p.parent = -1, p.group = -1, p.count = 1;
            p.h = h, p.x = x, p.w = w, p.y = y;
            x += w;
            p.own = {w * 0.5f + p.x - static_cast<float>(width) * 0.5f, cy - static_cast<float>(height) * 0.5f};
            p.center = {0.0f, 0.0f};
            p.jag[0] = jag[static_cast<size_t>((cols + 1) * j + i)];
            p.jag[1] = jag[static_cast<size_t>((cols + 1) * j + i + 1)];
            p.jag[2] = jag[static_cast<size_t>((cols + 1) * (j + 1) + i)];
            p.jag[3] = jag[static_cast<size_t>((cols + 1) * (j + 1) + i + 1)];
            p.offset = {}, p.angle = {};
        }
        y += h;
    }
    // Groups: (1 - link) of the pieces, chosen at random, start them; every other piece joins a
    // random started neighbour (left, right, above, below), pass after pass until all have joined.
    size_t n = pieces.size();
    float rate = std::clamp(linkRate, 0.0f, 1.0f);
    auto roots = static_cast<size_t>(static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(n)) * (1.0f - rate)));
    std::vector<int32_t> order(n);
    for (size_t k = 0; k < n; ++k) order[k] = static_cast<int32_t>(k);
    for (size_t i = 1; i < n; ++i) {
        auto j = static_cast<size_t>(rnd.nextInt(0, static_cast<int32_t>(i)));
        std::swap(order[i], order[j]);
    }
    std::vector<bool> marked(n, false);
    size_t started = 0;
    for (size_t k = 0; k < roots && k < n; ++k) {
        marked[static_cast<size_t>(order[k])] = true;
        ++started;
    }
    if (started == 0) {
        marked[0] = true;
        started = 1;
    }
    if (started < n) {
        auto remaining = static_cast<int32_t>(n - started);
        std::vector<int32_t> cand;
        while (remaining > 0) {
            for (size_t idx = 0; idx < n; ++idx) {
                if (marked[idx]) continue;
                auto row = static_cast<int32_t>(idx) / cols, col = static_cast<int32_t>(idx) % cols;
                auto i = static_cast<int32_t>(idx);
                cand.clear();
                if (col > 0 && marked[static_cast<size_t>(i - 1)]) cand.push_back(i - 1);
                if (col < cols - 1 && marked[static_cast<size_t>(i + 1)]) cand.push_back(i + 1);
                if (row > 0 && marked[static_cast<size_t>(i - cols)]) cand.push_back(i - cols);
                if (row < rows - 1 && marked[static_cast<size_t>(i + cols)]) cand.push_back(i + cols);
                if (cand.empty()) continue;
                int32_t pick = cand.size() > 1 ? cand[static_cast<size_t>(rnd.nextInt(0, static_cast<int32_t>(cand.size()) - 1))] : cand[0];
                pieces[idx].parent = pick;
                --remaining;
                marked[idx] = true;
                if (remaining < 0) break;
            }
        }
    }
    // Each group's first piece gets the next group index (in the pieces' order).
    for (size_t k = 0; k < n; ++k) {
        if (pieces[k].parent < 0) {
            pieces[k].group = static_cast<int32_t>(groups.size());
            groups.push_back(static_cast<int32_t>(k));
        }
    }
    for (auto& p : pieces) {
        if (p.parent < 0) continue;
        int32_t root = p.parent;
        for (int guard = 0; pieces[static_cast<size_t>(root)].parent >= 0 && guard < static_cast<int>(n); ++guard) {
            root = pieces[static_cast<size_t>(root)].parent;
        }
        p.parent = root;
        Piece& r0 = pieces[static_cast<size_t>(root)];
        p.group = r0.group;
        r0.count += 1;
        r0.center[0] += p.own[0];
        r0.center[1] += p.own[1];
    }
    for (auto& p : pieces) {
        if (p.parent >= 0) continue;
        p.center[0] += p.own[0];
        p.center[1] += p.own[1];
        p.center[0] /= static_cast<float>(p.count);
        p.center[1] /= static_cast<float>(p.count);
    }
    for (auto& p : pieces) {
        if (p.parent < 0) continue;
        p.center = pieces[static_cast<size_t>(p.parent)].center;
        p.count = pieces[static_cast<size_t>(p.parent)].count;
    }
}

void SplitFace::clearActions() {
    overflow = 0.0f;
    actions.clear();
}

void SplitFace::push(const ActionSpec& a, int width, int height) {
    if (groups.empty()) return;
    Action act;
    act.time = a.time;
    act.states.assign(groups.size(), State{});
    actions.push_back(std::move(act));
    Action& now = actions.back();
    const Action* prev = actions.size() > 1 ? &actions[actions.size() - 2] : nullptr;
    MwareRandom rnd(a.seed);
    for (size_t g = 0; g < groups.size(); ++g) {
        State& s = now.states[g];
        if (prev) s = prev->states[g];
        s.regist = a.regist;
        s.angRegist = a.angRegist;
        const Piece& head = pieces[static_cast<size_t>(groups[g])];
        V3 c{head.center[0], head.center[1], 0.0f};
        if (a.vel.on) s.vel = evalTerm(a.vel, c, width, height, rnd);
        if (a.acc.on) s.acc = evalTerm(a.acc, c, width, height, rnd);
        if (a.angVel.on) s.angVel = evalTerm(a.angVel, c, width, height, rnd);
        if (a.angAcc.on) s.angAcc = evalTerm(a.angAcc, c, width, height, rnd);
        if (a.delay.on) s.delay = evalDelay(a.delay, c, width, height, rnd);
        s.delay = s.delay >= 0.0f ? std::min(s.delay, a.time) : 0.0f;
    }
    if (!prev) {
        // A group whose delay outlasts the action does not move in it.
        for (auto& s : now.states) {
            if (now.time <= s.delay) s.vel = s.acc = s.angVel = s.angAcc = V3{};
        }
        return;
    }
    // Later actions start where the one before has taken each group (at the time it starts for
    // that group), with its velocities where the action gives none.
    for (size_t g = 0; g < groups.size(); ++g) {
        State& s = now.states[g];
        const State& p = prev->states[g];
        if (now.time <= s.delay) {
            s.angVel = p.angVel, s.angAcc = p.angAcc;
            s.acc = p.acc, s.vel = p.vel;
        }
        float t = s.delay + prev->time - p.delay;
        V3 vel{}, angVel{};
        motion(p.pos, p.vel, p.acc, p.regist, t, s.pos, &vel);
        motion(p.angle, p.angVel, p.angAcc, p.angRegist, t, s.angle, &angVel);
        if (!a.vel.on) s.vel = vel;
        if (!a.angVel.on) s.angVel = angVel;
    }
}

void SplitFace::begin(float start, float end, int32_t timeMs, int32_t curve, int32_t keep) {
    float total = 0.0f;
    for (const auto& a : actions) total = a.time * 1000.0f + total;
    t.set({total * start, 0.0f, 0.0f}, {total * end, 0.0f, 0.0f}, timeMs, curve);
    keepGoing = keep;
}

void SplitFace::setTimer(float ms) {
    t.setTimer(ms);
    float over = ms - t.timer;
    overflow = over >= 0.0f ? over : 0.0f;
    dirty = 1;
    if (!keepGoing) overflow = 0.0f;
}

void SplitFace::step(const FrameClock& clock, float timeScale) {
    if (t.step(clock)) {
        overflow = 0.0f;
        dirty = 1;
    } else if (keepGoing) {
        overflow = clock.frameSpeed * 16.0f * timeScale * (t.dst[0] - t.src[0]) / t.actionTime + overflow;
        dirty = 1;
    }
}

void SplitFace::place() {
    if (pieces.empty() || actions.empty() || !dirty) return;
    float time = (t.now[0] + overflow) * 0.001f;
    size_t k = 0;
    while (k < actions.size() && actions[k].time < time) {
        time = time - actions[k].time;
        ++k;
    }
    if (k >= actions.size()) {
        k = actions.size() - 1;
        time = actions[k].time + time;
    }
    for (auto& p : pieces) {
        if (p.group < 0) continue;
        auto g = static_cast<size_t>(p.group);
        size_t use = k;
        float local = time - actions[k].states[g].delay;
        if (local < 0.0f && k > 0) {
            use = k - 1;
            local = actions[use].time + time - actions[use].states[g].delay;
        }
        local = std::max(local, 0.0f);
        const State& s = actions[use].states[g];
        motion(s.pos, s.vel, s.acc, s.regist, local, p.offset, nullptr);
        motion(s.angle, s.angVel, s.angAcc, s.angRegist, local, p.angle, nullptr);
    }
}

}  // namespace nyan::diesel
