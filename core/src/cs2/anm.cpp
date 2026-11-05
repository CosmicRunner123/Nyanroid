#include "nyan/cs2/anm.hpp"

#include <algorithm>

namespace nyan::cs2 {
namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr size_t kLineSize = 68;
// A script whose lines loop without a wait would hang the engine; here one update runs at most
// this many lines.
constexpr int kMaxLinesPerAdvance = 100000;

}  // namespace

std::optional<AnmScript> AnmScript::parse(const Bytes& d) {
    if (d.size() < kHeaderSize || d[0] != 'A' || d[1] != 'N' || d[2] != 'M') return std::nullopt;
    const uint32_t count = readLE32(&d[8]);
    if (count == 0 || kHeaderSize + static_cast<size_t>(count) * kLineSize > d.size()) return std::nullopt;
    AnmScript s;
    s.lines.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* q = &d[kHeaderSize + i * kLineSize];
        auto& l = s.lines[i];
        l.op = static_cast<int32_t>(readLE32(q));
        for (int k = 0; k < 8; ++k) {
            l.p[static_cast<size_t>(k)].type = static_cast<int32_t>(readLE32(q + 4 + 8 * k));
            l.p[static_cast<size_t>(k)].value = static_cast<int32_t>(readLE32(q + 8 + 8 * k));
        }
    }
    return s;
}

AnmPlayer::AnmPlayer(std::shared_ptr<const AnmScript> script, uint32_t seed) : script_(std::move(script)), rng_(seed) {}

void AnmPlayer::setVar(int32_t index, int32_t value) {
    if (index >= 0 && index < 64) vars_[static_cast<size_t>(index)] = value;
}

int32_t AnmPlayer::var(int32_t index) const {
    return index >= 0 && index < 64 ? vars_[static_cast<size_t>(index)] : 0;
}

// An operand's value (0x84a0b0): type 1 reads the variable it names; 0 and 2 are the number itself.
int32_t AnmPlayer::value(const AnmScript::Operand& o) const {
    if (o.type == 1) return var(o.value);
    if (o.type == 0 || o.type == 2) return o.value;
    return 0;
}

int32_t& AnmPlayer::slot(int32_t index) {
    if (index >= 0 && index < 64) return vars_[static_cast<size_t>(index)];
    junk_ = 0;
    return junk_;
}

int32_t AnmPlayer::range(const AnmScript::Operand& lo, const AnmScript::Operand& hi) {
    const int32_t a = value(lo), b = value(hi);
    const uint32_t d = static_cast<uint32_t>(b) - static_cast<uint32_t>(a);
    if (d == 0) return a;
    return static_cast<int32_t>(rng_.next() % d) + a;
}

void AnmPlayer::restart() {
    started_ = false;
    pc_ = -1;
    wait_ = 0;
    frame_ = -1, blend_ = 255, disp_ = 1, posX_ = 0, posY_ = 0;
}

void AnmPlayer::advance(uint64_t now) {
    const auto& lines = script_->lines;
    const int32_t count = static_cast<int32_t>(lines.size());
    if (!started_) {
        started_ = true;
        base_ = now;
    }
    if (pc_ >= count) return;
    int64_t t = static_cast<int64_t>(now) - static_cast<int64_t>(base_);
    for (int guard = 0; wait_ <= t && guard < kMaxLinesPerAdvance; ++guard) {
        if (++pc_ >= count) break;
        const AnmScript::Line& l = lines[static_cast<size_t>(pc_)];
        const auto& p = l.p;
        auto jump = [&](const AnmScript::Operand& to) { pc_ = value(to) - 1; };
        switch (l.op) {
        case 0:  // ID frame min max
            t -= wait_;
            base_ += static_cast<uint64_t>(wait_);
            frame_ = value(p[0]);
            wait_ = range(p[1], p[2]);
            if (history_.empty() || history_.front() != frame_) {
                std::erase(history_, frame_);
                history_.insert(history_.begin(), frame_);
            }
            break;
        case 1:  // SET var min max
        {
            const int32_t v = range(p[1], p[2]);
            slot(value(p[0])) = v;
            break;
        }
        case 2:  // LOOP var line
        {
            int32_t& c = slot(value(p[0]));
            if (c < 1) jump(p[1]);
            else --c;
            break;
        }
        case 3: jump(p[0]); break;
        case 4: if (value(p[0]) > 0) jump(p[1]); break;
        case 5: if (value(p[0]) == value(p[1])) jump(p[2]); break;
        case 6: if (value(p[0]) != value(p[1])) jump(p[2]); break;
        case 7: if (value(p[0]) > value(p[1])) jump(p[2]); break;
        case 8: if (value(p[0]) < value(p[1])) jump(p[2]); break;
        case 9: if (value(p[0]) >= value(p[1])) jump(p[2]); break;
        case 10: if (value(p[0]) <= value(p[1])) jump(p[2]); break;
        case 11: slot(value(p[0])) = maxFrame_; break;
        case 12: blend_ = value(p[0]); break;
        case 13: disp_ = value(p[0]); break;
        case 14:
            posX_ = value(p[0]);
            posY_ = value(p[1]);
            break;
        case 15:  // WAIT min max
            t -= wait_;
            base_ += static_cast<uint64_t>(wait_);
            wait_ = range(p[0], p[1]);
            break;
        case 16: slot(value(p[0])) += value(p[1]); break;
        case 17: slot(value(p[0])) -= value(p[1]); break;
        default: return;  // the engine logs an unknown opcode and stops this update
        }
        if (history_.size() > 64) history_.resize(64);
    }
}

}  // namespace nyan::cs2
