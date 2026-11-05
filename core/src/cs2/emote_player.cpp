#include "nyan/cs2/emote_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nyan::cs2 {

EmotePlayer::EmotePlayer(std::shared_ptr<const EmoteModel> model, uint32_t seed) : model_(std::move(model)) {
    // The DLL's generator: XorShift128 with the classic first three words; the fourth is the time
    // there (a per-bank seed here).
    rng_[0] = 123456789, rng_[1] = 362436069, rng_[2] = 521288629, rng_[3] = seed;
    if (const auto& b = model_->blink()) {
        blinkPhase_ = static_cast<float>(b->beginFrame);
        blinkCount_ = b->intervalMin + (b->intervalMax - b->intervalMin) * random01();
    }
    // Physics starts from the editor's saved state (EPBustControl / EPPendControl, set from
    // `param` by 0x1000e3d0 and 0x1002e320).
    for (const auto& b : model_->busts()) {
        BustState s;
        s.c = &b;
        s.P = b.op, s.q = b.p, s.v = b.pv;
        busts_.push_back(s);
        anchors_.insert(b.layer);
    }
    // Selectors start on option 0 (EPSelectorControl's constructor, 0x1000cec0 -> 0x1000d060).
    for (const auto& sel : model_->selectors()) {
        SelectorState s;
        s.s = &sel;
        selectors_.push_back(s);
        applySelector(selectors_.back(), 0);
    }
    for (const auto* list : {&model_->hairs(), &model_->parts()}) {
        for (const auto& h : *list) {
            PendState s;
            s.c = &h;
            s.A = h.op;
            for (int k = 0; k < 2; ++k) s.p[k] = h.p[k], s.v[k] = h.pv[k];
            s.phase = h.bendR, s.bend = h.bendS;
            pends_.push_back(s);
            anchors_.insert(h.layer);
        }
    }
}

namespace {
// The physics' soft limit: atan(x 1.15 k) / k, k = pi / 80 (0x1008bf3c, 0x1008bf18), in double as
// the DLL calls atan.
float softLimit(float x) {
    const float k = 0.0392699f;
    return static_cast<float>(std::atan(static_cast<double>(x * 1.15f * k))) / k;
}
}  // namespace

void EmotePlayer::bustUpdate(BustState& s, float x, float y, float dt, float& lr, float& ud) const {
    // EPBustControl 0x10028b90 (outer rotation 0: gravity straight down).
    const auto& c = *s.c;
    if (s.ctlFirst) {
        s.ctlFirst = false;
        s.offX = s.P[0] - x, s.offY = s.P[1] - y;
    } else {
        s.P[0] = s.offX + x, s.P[1] = s.offY + y;
    }
    const float g[3] = {0, 1, 0};
    float k = c.spring * dt;
    for (int i = 0; i < 3; ++i) s.v[i] += k * (s.P[i] - s.q[i]);
    k = c.gravity * dt;
    for (int i = 0; i < 3; ++i) s.v[i] += k * g[i];
    k = c.friction * dt;
    for (int i = 0; i < 3; ++i) s.v[i] -= s.v[i] * k;
    for (int i = 0; i < 3; ++i) s.q[i] += s.v[i] * dt;
    lr = softLimit(-(c.scaleX * ((s.P[0] - s.q[0]) * 1.0f)));
    ud = softLimit((-((s.P[1] - s.q[1]) * 1.0f) - c.ofs) * c.scaleY);
}

void EmotePlayer::pendUpdate(PendState& s, float x, float y, float dt, float out[3]) const {
    // EPPendControl 0x1002e3c0 (no wind, outer rotation 0).
    const auto& c = *s.c;
    if (s.ctlFirst) {
        s.ctlFirst = false;
        s.offX = s.A[0] - x, s.offY = s.A[1] - y;
    } else {
        s.A[0] = s.offX + x, s.A[1] = s.offY + y;
    }
    std::array<float, 3> bp[2];
    bp[0] = {s.A[0], s.A[1] + c.length[0], s.A[2]};
    bp[1] = {bp[0][0], bp[0][1] + c.length[1], bp[0][2]};
    const float g[3] = {0, 1, 0};
    for (int i = 0; i < 2; ++i) {
        const float L = c.length[i];
        const auto& parent = i == 0 ? s.A : s.p[0];
        float d[3] = {parent[0] - s.p[i][0], parent[1] - s.p[i][1], parent[2] - s.p[i][2]};
        float d2 = d[1] * d[1] + d[0] * d[0] + d[2] * d[2];
        if (d2 > L * L) {
            float dist = static_cast<float>(std::sqrt(static_cast<double>(d2)));
            if (dist > 0.015625f) {
                float inv = 1.0f / dist;
                float n[3] = {inv * d[0], inv * d[1], d[2] * inv};
                float stretch = dist - L;
                if (i == 1) {
                    for (int a = 0; a < 3; ++a) s.p[1][a] += stretch * n[a];
                    float kk = -((s.v[1][1] * n[1] + s.v[1][0] * n[0] + s.v[1][2] * n[2]) * c.vBound * dt);
                    for (int a = 0; a < 3; ++a) s.v[1][a] += kk * n[a];
                } else {
                    float kk = stretch * c.bRate * dt;
                    for (int a = 0; a < 3; ++a) s.v[0][a] += kk * n[a];
                }
            }
        }
        float k = c.gravity * dt;
        for (int a = 0; a < 3; ++a) s.v[i][a] += k * g[a];
        s.v[i][0] -= s.v[i][0] * c.frictionX * dt;
        s.v[i][1] -= s.v[i][1] * c.frictionY * dt;
        for (int a = 0; a < 3; ++a) s.p[i][a] += s.v[i][a] * dt;
        const float ex = bp[i][0] - s.p[i][0], ey = bp[i][1] - s.p[i][1];
        out[i] = softLimit(-(c.scaleX[i] * ex * 1.0f));
        if (i == c.udEft) out[2] = softLimit((c.ofs - ey) * c.scaleY[i] * 1.0f);
    }
}

void EmotePlayer::physics(float dt, float x, float y) {
    // 0x1000ec10 (bust) and 0x100101b0 (hair, parts), after the pose is evaluated.
    if (busts_.empty() && pends_.empty()) return;
    const auto pos = model_->layerPositions(values(), anchors_);
    auto anchor = [&](const std::string& layer) {
        // 0x1000be30: an anchor layer not in the pose leaves (0, 0).
        auto it = pos.find(layer);
        return it == pos.end() ? std::array<float, 2>{x, y} : std::array<float, 2>{it->second[0] + x, it->second[1] + y};
    };
    // Steps of at most 1.1 frames over dt, the position moving from last frame's (0x1008bf38).
    auto pieces = [&](float prevX, float prevY, float px, float py, auto&& update) {
        const float rest = dt - 1.19209e-07f;
        float t = 0;
        if (rest <= 0) return;
        do {
            float step = std::min(1.1f, dt - t);
            float t2 = step + t;
            float f = t2 / dt;
            update((1.0f - f) * prevX + f * px, (1.0f - f) * prevY + f * py, step);
            t = t2;
        } while (rest > t);
    };
    for (auto& s : busts_) {
        auto [px, py] = anchor(s.c->layer);
        float lr = 0, ud = 0;
        if (s.first) {
            s.first = false;
            bustUpdate(s, px, py, 0, lr, ud);
        } else {
            pieces(s.prevX, s.prevY, px, py, [&](float ix, float iy, float step) { bustUpdate(s, ix, iy, step, lr, ud); });
        }
        s.prevX = px, s.prevY = py;
        if (std::fabs(lr) <= 0.01f && std::fabs(ud) <= 0.01f) lr = ud = 0;
        values_[s.c->varLR] = lr;
        values_[s.c->varUD] = ud;
    }
    for (auto& s : pends_) {
        auto [px, py] = anchor(s.c->layer);
        float out[3] = {0, 0, 0};
        auto bendStep = [&](float step) {
            // 0x100102b5: the bend grows while |UD| > 28, sways LR and LRM apart.
            if (std::fabs(out[2]) > 28.0f) s.bend = std::min(1.0f, s.bend + step * 0.03125f);
            else s.bend = std::max(0.0f, s.bend - step * 0.03125f);
            s.phase = static_cast<float>(std::fmod(static_cast<double>(s.c->bendSpd * s.bend * step + s.phase), 6.283185307179586));
            float sway = static_cast<float>(std::sin(static_cast<double>(s.phase))) * s.bend * s.c->bendVol;
            out[0] -= sway;
            out[1] += sway;
        };
        if (s.first) {
            s.first = false;
            pendUpdate(s, px, py, 0, out);
            bendStep(0);
        } else {
            pieces(s.prevX, s.prevY, px, py, [&](float ix, float iy, float step) {
                pendUpdate(s, ix, iy, step, out);
                bendStep(step);
            });
        }
        s.prevX = px, s.prevY = py;
        if (std::fabs(out[1]) <= 0.01f && std::fabs(out[0]) <= 0.01f && std::fabs(out[2]) <= 0.01f) out[0] = out[1] = out[2] = 0;
        values_[s.c->varLR] = out[1];
        values_[s.c->varLRM] = out[0];
        values_[s.c->varUD] = out[2];
    }
}

float EmotePlayer::random01() {
    uint32_t t = rng_[0] ^ (rng_[0] << 11);
    rng_[0] = rng_[1], rng_[1] = rng_[2], rng_[2] = rng_[3];
    rng_[3] = rng_[3] ^ (rng_[3] >> 19) ^ t ^ (t >> 8);
    // A float in [1, 2) from the low 23 bits, less one.
    uint32_t bits = (rng_[3] & 0x7FFFFFu) | 0x3F800000u;
    float f;
    std::memcpy(&f, &bits, 4);
    return f - 1.0f;
}

float EmotePlayer::variable(const std::string& name) const {
    auto it = values_.find(name);
    return it == values_.end() ? 0.0f : it->second;
}

void EmotePlayer::applySelector(SelectorState& s, int32_t index) {
    // 0x1000d060: the index kept; each option's variable to onValue if chosen, else offValue.
    s.index = index;
    values_[s.s->label] = static_cast<float>(index);
    for (size_t i = 0; i < s.s->options.size(); ++i) {
        const auto& o = s.s->options[i];
        setVariable(o.variable, static_cast<int32_t>(i) == index ? o.onValue : o.offValue);
    }
}

void EmotePlayer::setVariable(const std::string& name, float value, float frames, float easing) {
    // A selector's own variable (SetVariable type 8, 0x1000d260): at once without frames, else
    // queued (a new request replaces what waits) and taken by the step, which then waits that many
    // frames before the next.
    for (auto& s : selectors_) {
        if (s.s->label != name) continue;
        if (frames <= 0) {
            s.queue.clear();
            s.wait = 0;
            applySelector(s, static_cast<int32_t>(value));
        } else {
            s.queue.clear();
            s.queue.emplace_back(value, frames);
        }
        return;
    }
    if (sendTransition(transitions_[name], value, frames, easing)) values_[name] = value;
}

bool EmotePlayer::sendTransition(Transition& tr, float to, float frames, float easing) {
    if (frames <= 0) {
        tr.active = false;
        tr.queue.clear();
        return true;
    }
    float power = easing == 0 ? 1.0f : easing > 0 ? easing + 1.0f : 1.0f / (1.0f - easing);
    // Already on its way to this value and the new request would take longer: leave it.
    if (tr.active && tr.to == to && tr.speed > 0 && frames > (1.0f - tr.progress) / tr.speed) return false;
    tr.active = false;
    tr.queue.clear();
    tr.queue.push_back({to, {frames, power}});
    return false;
}

void EmotePlayer::stepTransition(Transition& tr, float& value, float dt) {
    for (int guard = 0; guard < 64; ++guard) {
        if (tr.active) {
            tr.progress += tr.speed * dt;
            if (tr.progress - 1.0f >= -1e-4f) {
                value = tr.to;
                tr.progress = 1;
                tr.active = false;
                continue;
            }
            value = tr.from + (tr.to - tr.from) * std::pow(tr.progress, tr.power);
            break;
        }
        if (tr.queue.empty()) break;
        auto [to, fp] = tr.queue.front();
        tr.queue.pop_front();
        tr.from = value;
        tr.to = to;
        tr.speed = 1.0f / fp.first;
        tr.power = fp.second;
        tr.progress = 0;
        tr.active = true;
    }
}

void EmotePlayer::sendKey(Playing& p, size_t i, float value, float frames, float easing) {
    if (!p.diff[i].on) {
        setVariable(p.tl->tracks[i].first, value, frames, easing);
        return;
    }
    auto& d = p.diff[i];
    if (sendTransition(d.tr, value, frames, easing)) d.value = value;
}

void EmotePlayer::playTimeline(const std::string& label, uint32_t flags) {
    // 0x1001d720: without the parallel flag everything playing stops first, known label or not.
    if (!(flags & kTimelineParallel)) playing_.clear();
    const EmoteModel::Timeline* tl = model_->timeline(label);
    if (!tl) return;
    stopTimeline(label);
    Playing p;
    p.tl = tl;
    p.label = label;
    p.flags = flags;
    p.index.assign(tl->tracks.size(), 0);
    p.diff.resize(tl->tracks.size());
    if (flags & kTimelineDifference) {
        for (size_t i = 0; i < tl->tracks.size(); ++i) p.diff[i].on = !model_->instantVariable(tl->tracks[i].first);
    }
    resetTimeline(p, 0);
    playing_.push_back(std::move(p));
}

bool EmotePlayer::animating() const {
    std::set<std::string> listed;
    for (const auto& p : playing_) {
        if (p.tl->loopBegin < 0 || p.tl->loopEnd <= p.tl->loopBegin) return true;
        // The timeline's tracks with keys (the loader, 0x1001e6b0, keeps only those in the list
        // IsAnimating reads; a label without keys doesn't count).
        for (const auto& [name, keys] : p.tl->tracks) listed.insert(name);
    }
    for (const auto& [name, tr] : transitions_) {
        if ((tr.active || !tr.queue.empty()) && !listed.count(name)) return true;
    }
    for (const auto& [name, h] : hostTweens_) {
        if (!listed.count(name)) return true;
    }
    return false;
}

void EmotePlayer::skip() {
    for (const auto& [name, h] : hostTweens_) setVariable(name, h.to);
    hostTweens_.clear();
    for (size_t i = 0; i < playing_.size();) {
        Playing& p = playing_[i];
        if (p.tl->loopBegin < 0 || p.tl->loopEnd <= p.tl->loopBegin) {
            advanceTimeline(p, p.tl->lastTime >= 0 ? p.tl->lastTime : p.tl->maxTime, true);
            playing_.erase(playing_.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
    for (auto& [name, tr] : transitions_) {
        if (!tr.active && tr.queue.empty()) continue;
        values_[name] = tr.queue.empty() ? tr.to : tr.queue.back().first;
        tr.active = false;
        tr.queue.clear();
    }
    for (auto& s : selectors_) {
        if (!s.queue.empty()) applySelector(s, static_cast<int32_t>(s.queue.back().first));
        s.queue.clear();
        s.wait = 0;
    }
}

void EmotePlayer::stopTimeline(const std::string& label) {
    std::erase_if(playing_, [&](const Playing& p) { return p.label == label; });
}

void EmotePlayer::resetTimeline(Playing& p, float time) {
    // Each track: the segment holding `time`, and the variable sent to the value of the last key
    // at or before it (end keys skipped) over the frames to the key after that.
    for (size_t i = 0; i < p.tl->tracks.size(); ++i) {
        const auto& [name, keys] = p.tl->tracks[i];
        const size_t n = keys.size();
        size_t k = 0;
        long last = -1;
        while (k + 1 < n) {
            if (!keys[k].end) last = static_cast<long>(k);
            if (keys[k].time <= time && keys[k + 1].time > time) break;
            ++k;
        }
        p.index[i] = k;
        if (last >= 0) {
            const auto& key = keys[static_cast<size_t>(last)];
            float next = static_cast<size_t>(last) + 1 < n ? keys[static_cast<size_t>(last) + 1].time : key.time;
            sendKey(p, i, key.value, std::max(0.0f, next - time - 1.0f), key.easing);
        }
    }
    p.time = time;
}

void EmotePlayer::advanceTimeline(Playing& p, float time, bool inclusive) {
    // Keys passed on the way to `time` each send their variable to their value, over the frames
    // left to the following key.
    for (size_t i = 0; i < p.tl->tracks.size(); ++i) {
        const auto& [name, keys] = p.tl->tracks[i];
        const size_t n = keys.size();
        size_t k = p.index[i];
        while (k + 1 < n) {
            float t1 = keys[k + 1].time;
            if (inclusive ? time < t1 : time <= t1) break;
            ++k;
            if (keys[k].end) continue;
            float next = k + 1 < n ? keys[k + 1].time : keys[k].time;
            sendKey(p, i, keys[k].value, std::max(0.0f, next - time - 1.0f), keys[k].easing);
        }
        p.index[i] = k;
    }
    p.time = time;
}

void EmotePlayer::setHostBlinks(std::vector<HostBlink> blinks) {
    hostBlinks_.clear();
    for (auto& b : blinks) {
        values_[b.variable] = b.rest;
        hostBlinks_.push_back({std::move(b), 0, 0});
    }
}

void EmotePlayer::setBlink(bool on) {
    // 0x4e2b00: off puts each blink's variable at rest, on restarts the waits.
    if (on == blinkOn_) return;
    blinkOn_ = on;
    for (auto& h : hostBlinks_) {
        if (!on) values_[h.b.variable] = h.b.rest;
        h.startAt = h.endAt = 0;
    }
}

void EmotePlayer::stepHostBlinks() {
    // cs2.exe 0x4e2410 (first loop), with the bank's clock for its frame count.
    if (!blinkOn_) return;
    const uint64_t now = clock_;
    for (auto& h : hostBlinks_) {
        if (h.startAt == h.endAt) {
            uint32_t r = static_cast<uint32_t>(random01() * 8388608.0f);
            h.startAt = static_cast<uint64_t>(h.b.min) + r % static_cast<uint32_t>(std::max(1, h.b.range)) + now;
            h.endAt = static_cast<uint64_t>(std::max(0, h.b.frames)) + h.startAt;
        }
        if (h.startAt > now) continue;
        if (h.endAt <= now) {
            values_[h.b.variable] = h.b.rest;
            h.endAt = h.startAt;
            continue;
        }
        float p = static_cast<float>(now - h.startAt) / static_cast<float>(std::max(1, h.b.frames));
        p += p;
        values_[h.b.variable] = p <= 1.0f ? (h.b.end - h.b.start) * p + h.b.start : (p - 1.0f) * (h.b.start - h.b.end) + h.b.end;
    }
}

void EmotePlayer::hostTween(const std::string& name, float to, int32_t frames, uint8_t curve) {
    if (frames <= 0) {
        hostTweens_.erase(name);
        setVariable(name, to);
        return;
    }
    hostTweens_[name] = HostTween{variable(name), to, clock_, frames, curve};
}

void EmotePlayer::stepHostTweens() {
    constexpr float kPi = 3.14159265358979f;
    for (auto it = hostTweens_.begin(); it != hostTweens_.end();) {
        const HostTween& h = it->second;
        float p = std::min(1.0f, static_cast<float>(clock_ - h.start) / static_cast<float>(std::max(1, h.frames)));
        float e = p;
        if (h.curve == 1) e = std::sin(kPi / 2 * p);
        else if (h.curve == 2) e = 1 - std::cos(kPi / 2 * p);
        else if (h.curve == 3) e = (1 - std::cos(kPi * p)) / 2;
        setVariable(it->first, h.from + (h.to - h.from) * e);
        it = p >= 1.0f ? hostTweens_.erase(it) : std::next(it);
    }
}

void EmotePlayer::step(float dt, float x, float y) {
    clock_ += static_cast<uint64_t>(std::max(0.0f, dt));
    // Selectors (0x1000d360): after the wait, the next queued choice at once, then its frames.
    for (auto& s : selectors_) {
        if (s.wait > 0) s.wait -= dt;
        while (s.wait <= 0 && !s.queue.empty()) {
            auto [v, f] = s.queue.front();
            s.queue.pop_front();
            applySelector(s, static_cast<int32_t>(v));
            s.wait += f;
        }
    }
    stepHostBlinks();
    stepHostTweens();
    // Timelines.
    for (size_t i = 0; i < playing_.size();) {
        Playing& p = playing_[i];
        const auto* tl = p.tl;
        if (tl->loopBegin < 0 || tl->loopEnd <= tl->loopBegin) {
            advanceTimeline(p, p.time + dt, true);
            float end = tl->lastTime >= 0 ? tl->lastTime : tl->maxTime;
            if (p.time >= end) {
                playing_.erase(playing_.begin() + static_cast<long>(i));
                continue;
            }
        } else {
            float rest = dt;
            while (p.time + rest >= tl->loopEnd) {
                float before = p.time;
                advanceTimeline(p, tl->loopEnd, false);
                rest -= tl->loopEnd - before;
                resetTimeline(p, tl->loopBegin);
            }
            advanceTimeline(p, p.time + std::max(0.0f, rest), true);
        }
        // A difference timeline's own values, after its keys (0x1001f8c0).
        if (p.flags & kTimelineDifference) {
            for (auto& d : p.diff) {
                if (d.on) stepTransition(d.tr, d.value, dt);
            }
        }
        ++i;
    }

    // Variable transitions.
    for (auto& [name, tr] : transitions_) {
        if (tr.active || !tr.queue.empty()) stepTransition(tr, values_[name], dt);
    }

    // Blinking.
    stepBlink(dt);
    if (dt != 0) physics(dt, x, y);
}

void EmotePlayer::stepBlink(float dt) {
    const auto& b = model_->blink();
    if (!b) return;
    const float begin = static_cast<float>(b->beginFrame), end = static_cast<float>(b->endFrame);
    const float rate = dt * 2.5f / b->frames * (end - begin);
    for (int guard = 0; guard < 8; ++guard) {
        if (blinkState_ == 0) {
            if (!b->enabled || !blinkOn_ || static_cast<int>(blinkPhase_) != b->beginFrame) break;
            blinkCount_ -= dt;
            if (blinkCount_ > 0) break;
            blinkState_ = 10;
        } else if (blinkState_ == 10) {
            blinkPhase_ += rate;
            if (blinkPhase_ < end) break;
            blinkPhase_ = end;
            blinkCount_ = b->frames / 5.0f;
            blinkState_ = 11;
        } else if (blinkState_ == 11) {
            blinkCount_ -= dt;
            if (blinkCount_ > 0) break;
            blinkState_ = 12;
            blinkCount_ = b->intervalMin + (b->intervalMax - b->intervalMin) * random01();
        } else {
            blinkPhase_ -= rate;
            if (blinkPhase_ > begin) break;
            blinkPhase_ = begin;
            blinkState_ = 0;
        }
    }
}

std::map<std::string, float> EmotePlayer::values() const {
    std::map<std::string, float> out = values_;
    // Difference timelines' values on top (0x10020170; blend ratio 1, cs2 never changes it).
    for (const auto& p : playing_) {
        for (size_t i = 0; i < p.diff.size(); ++i) {
            if (p.diff[i].on) out[p.tl->tracks[i].first] += p.diff[i].value;
        }
    }
    for (const auto& [name, add] : offsets_) {
        if (add != 0) out[name] += add;
    }
    if (const auto& b = model_->blink(); b && b->endFrame != b->beginFrame) {
        const float begin = static_cast<float>(b->beginFrame), end = static_cast<float>(b->endFrame);
        float v = variable(b->variable);
        if (v >= begin && v <= end) out[b->variable] = v + (blinkPhase_ - begin) * (end - v) / (end - begin);
    }
    // Clamps (0x10015d70): float arithmetic with the DLL's double sqrt / trig calls.
    const float pi = 3.14159274f;
    for (const auto& c : model_->clamps()) {
        auto value = [&](const std::string& name) {
            auto it = out.find(name);
            return it == out.end() ? 0.0f : it->second;
        };
        const float range = c.max - c.min;
        float x = (value(c.varLR) - c.min) / range, y = (value(c.varUD) - c.min) / range;
        x = (x + x) - 1.0f, y = (y + y) - 1.0f;
        if (x == 0 || y == 0 || std::isnan(x) || std::isnan(y)) continue;
        if (c.type == 1) {
            float r = static_cast<float>(std::sqrt(static_cast<double>(y * y + x * x)));
            if (r > 1.0f) {
                float a = static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x)));
                x = static_cast<float>(std::cos(static_cast<double>(a)));
                y = static_cast<float>(std::sin(static_cast<double>(a)));
            }
        } else if (c.type == 0) {
            float t = std::fabs(x / y);
            if (t > 1.0f) t = 1.0f / t;
            float s = 1.0f / static_cast<float>(std::sqrt(static_cast<double>(t * t + 1.0f)));
            const float X = s * x, Y = s * y;
            const float r = static_cast<float>(std::sqrt(static_cast<double>(Y * Y + X * X)));
            const float c1 = 1.0f - static_cast<float>(std::cos(static_cast<double>(t * pi * 0.5f)));
            const float f = (static_cast<float>(std::sin(static_cast<double>(r * pi * 0.5f))) / r - 1.0f) * c1 + 1.0f;
            x = f * X, y = f * Y;
        } else {
            continue;
        }
        out[c.varLR] = (x + 1.0f) * 0.5f * range + c.min;
        out[c.varUD] = (y + 1.0f) * 0.5f * range + c.min;
    }
    return out;
}

}  // namespace nyan::cs2
