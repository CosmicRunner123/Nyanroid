#include "nyan/cs2/emote_inertia.hpp"

#include <algorithm>

#include "nyan/cs2/emote_player.hpp"

namespace nyan::cs2 {

EmoteInertia::EmoteInertia(const EmoteProfiles::Resolved& profile) {
    // Every #material variable gets a spring; the min / max rows start as copies of it (0x591eb0).
    for (const auto& [name, m] : profile.materials) {
        Spring s;
        s.base = s.min = s.max = m;
        if (auto it = profile.materialMin.find(name); it != profile.materialMin.end()) s.min = it->second;
        if (auto it = profile.materialMax.find(name); it != profile.materialMax.end()) s.max = it->second;
        if (auto it = profile.levelGroup.find(name); it != profile.levelGroup.end()) s.levelGroup = it->second;
        if (auto it = profile.enableGroup.find(name); it != profile.enableGroup.end()) s.enableGroup = it->second;
        blend(s, 1.0f);
        springs_.emplace(name, s);
    }
    ud_ = profile.inertiaUD;
    lr_ = profile.inertiaLR;
    for (const auto& d : profile.inertiaDifs) {
        if (!springs_.count(d.target)) continue;
        difs_.push_back({d.source, d.target, d.power, d.range != 0 ? 1.0f / d.range : 0.0f});
    }
}

void EmoteInertia::blend(Spring& s, float level) const {
    // 0x593c60: the level clamped to 0-2; 1 is #material itself.
    level = std::clamp(level, 0.0f, 2.0f);
    auto mix = [](const Material& a, const Material& b, float t, Spring& out) {
        out.dt = (b.dt - a.dt) * t + a.dt;
        out.spring = (b.spring - a.spring) * t + a.spring;
        out.mass = (b.mass - a.mass) * t + a.mass;
        out.resistance = (b.resistance - a.resistance) * t + a.resistance;
        out.maxMove = (b.maxMove - a.maxMove) * t + a.maxMove;
    };
    if (level == 1.0f) mix(s.base, s.base, 0, s);
    else if (level > 1.0f) mix(s.base, s.max, level - 1.0f, s);
    else mix(s.min, s.base, level, s);
}

void EmoteInertia::setLevel(const std::string& group, int32_t level) {
    level = std::clamp(level, 0, 200);  // 0x581240
    auto [it, added] = levels_.try_emplace(group, 100);
    if (!added && it->second == level) return;
    if (added && level == 100) return;
    it->second = level;
    for (auto& [name, s] : springs_) {
        if (s.levelGroup != group) continue;
        blend(s, static_cast<float>(level) * 0.01f);
        s.x = s.v = s.out = 0;
    }
    first_ = true;  // the inputs restart from where things are
    for (auto& d : difs_) d.started = false;
}

void EmoteInertia::setEnabled(const std::string& group, bool on) {
    auto [it, added] = enables_.try_emplace(group, 1);
    if (!added && it->second == (on ? 1 : 0)) return;
    it->second = on ? 1 : 0;
    for (auto& [name, s] : springs_) {
        if (s.enableGroup == group) s.enabled = on;
    }
}

void EmoteInertia::nudge(const std::vector<std::string>& sources) {
    for (auto& d : difs_) {
        if (std::find(sources.begin(), sources.end(), d.source) != sources.end()) d.nudge = true;
    }
}

void EmoteInertia::setPaused(const std::vector<std::string>& names) {
    for (auto& [name, s] : springs_) {
        const bool p = std::find(names.begin(), names.end(), name) != names.end();
        if (p != s.paused) {
            s.paused = p;
            s.x = s.v = s.out = 0;
        }
    }
}

void EmoteInertia::step(float x, float y, EmotePlayer& player, bool active) {
    if (!active) {
        for (auto& [name, s] : springs_) {
            s.x = s.v = s.out = 0;
            player.setOffset(name, 0);
        }
        first_ = true;
        for (auto& d : difs_) d.started = false;
        return;
    }
    // The bank's move since the last frame pushes the springs the other way (0x5930c0).
    if (!first_) {
        for (const auto& name : lr_) {
            if (auto it = springs_.find(name); it != springs_.end()) it->second.x -= x - px_;
        }
        for (const auto& name : ud_) {
            if (auto it = springs_.find(name); it != springs_.end()) it->second.x -= y - py_;
        }
    }
    first_ = false;
    px_ = x, py_ = y;
    // Expression changes (0x594530): (source now - before) / range x the target's maximum move x power.
    for (auto& d : difs_) {
        const float now = player.variable(d.source);
        if (d.nudge) {
            // 0x594530 with the flag (+0x44): before := now, the flag cleared, and the target pushed
            // by its maximum move over the range (no power, no change).
            d.nudge = false;
            d.cur = d.prev = now;
            d.started = true;
            Spring& t = springs_.at(d.target);
            t.x -= t.maxMove * d.invRange;
            continue;
        }
        if (!d.started) {
            d.cur = d.prev = now;
            d.started = true;
            continue;
        }
        d.prev = d.cur;
        d.cur = now;
        if (d.cur == d.prev) continue;
        Spring& t = springs_.at(d.target);
        t.x -= (d.cur - d.prev) * d.invRange * t.maxMove * d.power;
    }
    // One step per frame (0x594760), then the outputs (0x5948f0).
    for (auto& [name, s] : springs_) {
        if (!s.enabled) {
            s.x = s.v = s.out = 0;
        } else if (!s.paused) {
            s.x = std::clamp(s.x, -s.maxMove, s.maxMove);
            s.v = ((-(s.spring * s.x)) - s.resistance * s.v) / s.mass * s.dt + s.v;
            s.x = s.dt * s.v + s.x;
            s.out = s.maxMove != 0 ? s.x / s.maxMove : 0.0f;
        }
        float value = s.out * s.base.scale;
        if (s.base.invert) value = -value;
        player.setOffset(name, value);
    }
}

}  // namespace nyan::cs2
