#include "nyan/wa2/ani.hpp"

#include <cstring>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

namespace {

constexpr int kFirstImageSlot = 0x200;

int16_t s16(const Bytes& d, size_t at) {
    return static_cast<int16_t>(readLE16(&d[at]));
}

}  // namespace

AniPlayer::AniPlayer(Gfx& gfx) : gfx_(gfx) {}

bool AniPlayer::load(int slot, const std::string& file, uint32_t flags, const std::string& amp1, const std::string& amp2,
                     int level, const std::string& group) {
    if (slot < 0 || slot >= static_cast<int>(defs_.size())) return false;
    release(slot);
    std::string g = group.empty() ? std::string("grp") : group;
    auto data = gfx_.assets().read(g, file);
    if (!data) return false;
    const Bytes& d = *data;
    if (d.size() < 0x24 || std::memcmp(d.data(), "INAS", 4) != 0) return false;  // "未知のフォーマット"
    Def def;
    def.rate = static_cast<int>(readLE32(&d[0x14]));
    uint32_t frames = readLE32(&d[0x18]), seqs = readLE32(&d[0x1c]), images = readLE32(&d[0x20]);
    size_t at = 0x24;
    auto need = [&](size_t n) { return at + n <= d.size(); };
    for (uint32_t f = 0; f < frames; ++f) {
        if (!need(0x18)) return false;
        Frame fr;
        fr.used = d[at] != 0;
        fr.selector = readLE16(&d[at + 4]);
        fr.flags = readLE32(&d[at + 8]);
        uint32_t parts = readLE32(&d[at + 0x14]);
        at += 0x18;
        for (uint32_t p = 0; p < parts; ++p) {
            if (!need(0x28)) return false;
            Part pt;
            pt.used = d[at] != 0;
            pt.image = d[at + 4];
            pt.flags = readLE32(&d[at + 0xc]);
            pt.blend = static_cast<int32_t>(readLE32(&d[at + 0x14]));
            pt.sx = s16(d, at + 0x18);
            pt.sy = s16(d, at + 0x1a);
            pt.w = s16(d, at + 0x1c);
            pt.h = s16(d, at + 0x1e);
            pt.ox = s16(d, at + 0x20);
            pt.oy = s16(d, at + 0x22);
            fr.parts.push_back(pt);
            at += 0x28;
        }
        def.frames.push_back(std::move(fr));
    }
    for (uint32_t s = 0; s < seqs; ++s) {
        if (!need(8)) return false;
        uint32_t m = readLE32(&d[at + 4]);
        at += 8;
        std::vector<Command> cmds;
        for (uint32_t k = 0; k <= m; ++k) {
            if (!need(8)) return false;
            cmds.push_back({readLE16(&d[at]), readLE16(&d[at + 2]), readLE16(&d[at + 4]), readLE16(&d[at + 6])});
            at += 8;
        }
        def.seqs.push_back(std::move(cmds));
    }
    for (uint32_t i = 0; i < images; ++i) {
        size_t end = at;
        while (end < d.size() && d[end] != 0) ++end;
        std::string name(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(end));
        at = end + 1;
        // 0x448340: the first free image slot from 0x200.
        int image = -1;
        for (int s = kFirstImageSlot; s < kSlotCount; ++s) {
            if (!gfx_.slots[static_cast<size_t>(s)].loaded) {
                image = s;
                break;
            }
        }
        if (image != -1 && !gfx_.load(image, name, flags, false, amp1, amp2, level, g)) image = -1;
        def.images.push_back(image);
    }
    def.used = true;
    defs_[static_cast<size_t>(slot)] = std::move(def);
    return true;
}

void AniPlayer::release(int slot) {
    if (slot < 0 || slot >= static_cast<int>(defs_.size())) return;
    for (int image : defs_[static_cast<size_t>(slot)].images) {
        if (image >= 0) gfx_.release(image);
    }
    defs_[static_cast<size_t>(slot)] = Def{};
}

void AniPlayer::reset() {
    for (int s = 0; s < static_cast<int>(defs_.size()); ++s) {
        if (defs_[static_cast<size_t>(s)].used) release(s);
    }
    instances_.fill(Instance{});
}

bool AniPlayer::makeObject(int obj, int slot, int seq, bool playing, int prio, bool visible, int hold, int mode,
                           int xfade) {
    // 0x441c40 with 0x448520.
    gfx_.remove(obj);
    if (obj < 0 || obj >= kObjectCount) return false;
    int found = -1;
    for (int i = 0; i < static_cast<int>(instances_.size()); ++i) {
        if (!instances_[static_cast<size_t>(i)].used) {
            found = i;
            break;
        }
    }
    if (found == -1 || slot < 0 || slot >= static_cast<int>(defs_.size()) || !defs_[static_cast<size_t>(slot)].used ||
        seq < 0 || seq >= static_cast<int>(defs_[static_cast<size_t>(slot)].seqs.size())) {
        return false;
    }
    Instance in;
    in.used = true;
    in.def = slot;
    in.seq = seq;
    in.playing = playing ? 1 : 0;
    in.xfade = xfade;
    in.hold = hold;
    in.mode = mode;
    instances_[static_cast<size_t>(found)] = in;
    SpriteObject o;
    o.used = true;
    o.visible = visible;
    o.priority = static_cast<int16_t>(prio);
    o.kind = 1;
    o.blend = 0;
    o.slot = static_cast<int16_t>(found);
    gfx_.objects[static_cast<size_t>(obj)] = o;
    return true;
}

void AniPlayer::releaseInstance(int instance) {
    if (instance >= 0 && instance < static_cast<int>(instances_.size())) instances_[static_cast<size_t>(instance)] = Instance{};
}

void AniPlayer::setPlay(int instance, int slot, int seq, bool playing, int hold, int mode, int xfade) {
    // 0x448670: a different animation, mode 0, or a finished play-once starts over.
    if (instance < 0 || instance >= static_cast<int>(instances_.size())) return;
    Instance& in = instances_[static_cast<size_t>(instance)];
    if (in.def != slot || in.seq != seq || mode == 0 || (in.mode == 1 && in.playing == 0)) {
        in = Instance{};
        in.used = true;
        in.def = slot;
        in.seq = seq;
    }
    in.playing = playing ? 1 : 0;
    in.hold = hold;
    in.mode = mode;
    in.xfade = xfade;
    in.speed = 100;
}

int AniPlayer::currentFrame(int i) const {
    const Instance& in = instances_[static_cast<size_t>(i)];
    const Def& def = defs_[static_cast<size_t>(in.def)];
    if (!in.used || !def.used || in.seq >= static_cast<int>(def.seqs.size())) return 0;
    const auto& cmds = def.seqs[static_cast<size_t>(in.seq)];
    return in.index >= 0 && in.index < static_cast<int>(cmds.size()) ? cmds[static_cast<size_t>(in.index)].a : 0;
}

void AniPlayer::step(Instance& in, const std::vector<Command>& cmds) {
    // 0x447ef0: interpret while the accumulator has a frame's worth (100) left.
    auto op = [&](int i) -> const Command& {
        static const Command end{};
        return i >= 0 && i < static_cast<int>(cmds.size()) ? cmds[static_cast<size_t>(i)] : end;
    };
    int guard = 0;
    while (in.acc >= 1 && ++guard < 100000) {
        const Command& c = op(in.index);
        switch (c.op) {
        case 0:
            if (in.index == 0) in.acc -= 100;
            if (in.mode == 0) {
                in.index = 0;
            } else if (in.mode == 1) {
                if (in.hold == 0) {
                    in.playing = 0;
                    in.acc = 0;
                } else {
                    while (in.index > 0 && op(in.index).op != 1) --in.index;
                    in.playing = 0;
                    in.tick = 0;
                }
            } else {
                --in.mode;
                in.index = 0;
            }
            break;
        case 1:
            ++in.tick;
            if (c.b < in.tick) {
                ++in.index;
                in.tick = 0;
            } else {
                if (in.tick == 1 && c.c != 0) in.mark = static_cast<int16_t>(c.c);
                if (in.breakNext == 1 && c.b < in.tick + 1 && op(in.index + 1).op == 3) in.breakNext = 2;
                in.acc -= 100;
            }
            break;
        case 2:
            if (in.depth < static_cast<int>(in.stack.size())) in.stack[static_cast<size_t>(in.depth++)] = static_cast<uint8_t>(c.a == 0 ? 0 : c.a + 1);
            ++in.index;
            break;
        case 3: {
            int i = in.index;
            if (in.depth != 0) {
                if (in.breakFlag != 0) {
                    in.breakFlag = 0;
                    in.stack[static_cast<size_t>(--in.depth)] = 0;
                    ++in.index;
                    break;
                }
                uint8_t& top = in.stack[static_cast<size_t>(in.depth - 1)];
                if (top != 0) {
                    if (top == 1) {
                        top = 0;
                        --in.depth;
                        ++in.index;
                        break;
                    }
                    --top;
                }
                while (i > -1 && op(i).op != 2) --i;
            }
            in.index = i + 1;
            break;
        }
        default:
            ++in.index;
            break;
        }
    }
}

void AniPlayer::tick(int fps) {
    for (Instance& in : instances_) {
        if (!in.used || in.playing == 0 || in.paused != 0) continue;
        const Def& def = defs_[static_cast<size_t>(in.def)];
        if (!def.used || in.seq >= static_cast<int>(def.seqs.size())) continue;
        in.acc += in.speed * def.rate / fps;
        step(in, def.seqs[static_cast<size_t>(in.seq)]);
        // (0x4480a0, the cross-fade's look-ahead frame: no shipped animation uses it.)
        if (in.xfade != 0) in.lookahead = in.index;
    }
}

void AniPlayer::draw(const SpriteObject& obj, const Gfx::DrawObject& emit) const {
    // 0x447e50: an instance draws while it plays or holds its last frame.
    int i = obj.slot;
    if (i < 0 || i >= static_cast<int>(instances_.size())) return;
    const Instance& in = instances_[static_cast<size_t>(i)];
    if (!in.used) return;
    const Def& def = defs_[static_cast<size_t>(in.def)];
    if (!def.used || (in.playing == 0 && in.hold == 0) || def.frames.empty() ||
        in.seq >= static_cast<int>(def.seqs.size())) {
        return;
    }
    int f = currentFrame(i);
    if (f < 0 || f >= static_cast<int>(def.frames.size())) return;
    const Frame& frame = def.frames[static_cast<size_t>(f)];
    if (!frame.used) return;
    for (const Part& part : frame.parts) drawPart(part, frame, def, in, obj, emit);
}

void AniPlayer::drawPart(const Part& part, const Frame& frame, const Def& def, const Instance&, const SpriteObject& obj,
                         const Gfx::DrawObject& emit) const {
    // 0x4478e0: a copy of the object on the part's sheet, always on the screen.
    if (!part.used) return;
    SpriteObject q = obj;
    q.used = q.visible = true;
    q.kind = 1;
    q.vertexCount = 4;
    q.shaderOn = false;
    if (q.blend == 0) {
        switch (frame.selector) {
        case 0: q.blend = part.blend == -2 ? 1 : 0; break;
        case 0xb: q.blend = 1; break;
        case 1: q.blend = 3; break;
        case 3: q.blend = 9; break;
        case 5: q.blend = 6; break;
        default: break;
        }
    }
    uint32_t flags = part.flags ^ obj.mirror ^ frame.flags;
    if (flags & 0x10) q.mirror |= 1;
    if (flags & 0x20) q.mirror |= 2;
    q.target = -1;
    q.slot = static_cast<int16_t>(part.image >= 0 && part.image < static_cast<int>(def.images.size())
                                      ? def.images[static_cast<size_t>(part.image)]
                                      : -1);
    if (q.slot < 0) return;
    q.maskSlot = -1;
    float x0 = static_cast<float>(part.ox) + obj.x[0], y0 = static_cast<float>(part.oy) + obj.y[0];
    float x1 = static_cast<float>(part.w) + x0, y1 = y0 + static_cast<float>(part.h);
    q.x = {x0, x1, x0, x1};
    q.y = {y0, y0, y1, y1};
    q.u0 = static_cast<float>(part.sx);
    q.u1 = static_cast<float>(part.sx + part.w);
    q.v0 = static_cast<float>(part.sy);
    q.v1 = static_cast<float>(part.sy + part.h);
    emit(q);
}

}  // namespace nyan::wa2
