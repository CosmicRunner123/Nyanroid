#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nyan/wa2/gfx.hpp"

namespace nyan::wa2 {

// The `.ani` sprite animations (INAS files; WA2.exe loader 0x448bd0, instances 0x448520, tick 0x448180
// with the interpreter 0x447ef0, draw 0x447e50 / 0x4478e0): frames of parts cut from sheets, played by
// sequences of commands (show a frame for n ticks, loops, end). An object of kind 1 shows an instance.
class AniPlayer {
public:
    explicit AniPlayer(Gfx& gfx);

    // 0x448bd0 via 0x449470: `file` from `group` into definition `slot` (0..0x1ff); its sheets go to the
    // first free image slots from 0x200 with the given picture parameters.
    bool load(int slot, const std::string& file, uint32_t flags, const std::string& amp1, const std::string& amp2,
              int level, const std::string& group);
    void release(int slot);  // 0x448400
    void reset();            // 0x448e80: every definition and instance
    // 0x441c40: object `obj` (kind 1) playing sequence `seq` of definition `slot`.
    bool makeObject(int obj, int slot, int seq, bool playing, int prio, bool visible, int hold, int mode, int xfade);
    void releaseInstance(int instance);  // 0x448630
    // 0x448670 (SetBmpAnimePlay): restart or change what an instance plays.
    void setPlay(int instance, int slot, int seq, bool playing, int hold, int mode, int xfade);
    // One logic tick (0x448180, before the VM): every playing instance moves on.
    void tick(int fps);
    // The kind-1 draw (0x447e50): the object's current frame, part by part.
    void draw(const SpriteObject& obj, const Gfx::DrawObject& emit) const;

    // An instance's state, for tests and saves.
    struct Instance {
        bool used = false;
        int def = 0, seq = 0;
        int depth = 0;
        std::array<uint8_t, 16> stack{};
        int tick = 0, index = 0, acc = 0;
        int16_t breakFlag = 0, breakNext = 0, mark = 0;  // +0x28 / +0x2a / +0x2c
        int lookahead = 0;  // +0x30
        int xfade = 0, playing = 0, hold = 0, mode = 0, speed = 100, paused = 0;
    };
    const Instance& instance(int i) const { return instances_[static_cast<size_t>(i)]; }
    // The frame an instance shows (the current command's first argument).
    int currentFrame(int i) const;

private:
    struct Part {
        bool used = false;
        int image = 0;
        uint32_t flags = 0;
        int32_t blend = 0;
        int sx = 0, sy = 0, w = 0, h = 0, ox = 0, oy = 0;
    };
    struct Frame {
        bool used = false;
        int selector = 0;
        uint32_t flags = 0;
        std::vector<Part> parts;
    };
    struct Command {
        uint16_t op = 0, a = 0, b = 0, c = 0;
    };
    struct Def {
        bool used = false;
        int rate = 60;
        std::vector<Frame> frames;
        std::vector<std::vector<Command>> seqs;
        std::vector<int> images;  // image slots
    };
    void step(Instance& in, const std::vector<Command>& cmds);  // 0x447ef0
    void drawPart(const Part& part, const Frame& frame, const Def& def, const Instance& in, const SpriteObject& obj,
                  const Gfx::DrawObject& emit) const;  // 0x4478e0

    Gfx& gfx_;
    std::array<Def, 0x200> defs_;
    std::array<Instance, 0x100> instances_;
};

}  // namespace nyan::wa2
