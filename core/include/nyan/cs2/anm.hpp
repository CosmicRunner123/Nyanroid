#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/cs2/mersenne_twister.hpp"

namespace nyan::cs2 {

// An ANM animation script (`<image>.anm` next to a multi-frame image): "ANM\0", a word, the line
// count, padding to 0x20, then 68-byte lines: an opcode and eight operands, each a type (0 or 2 a
// number, 1 a variable's number) and a value (cs2.exe's loader 0x849490).
struct AnmScript {
    struct Operand {
        int32_t type = 0;
        int32_t value = 0;
    };
    struct Line {
        int32_t op = 0;
        std::array<Operand, 8> p{};
    };
    std::vector<Line> lines;

    static std::optional<AnmScript> parse(const Bytes& data);
};

// One running ANM script: cs2.exe's ANM object (0x849340, run by 0x849840 each time its image is
// updated). It keeps a line counter, the current line's wait, 64 variables and the state the lines
// set (frame, blend, display, position). Opcodes: 0 ID frame min max (show a frame for a wait), 1 SET
// var min max, 2 LOOP var line (jump when the variable is below 1, else count it down), 3 JUMP line,
// 4 IF value line (jump when above 0), 5-10 IFE / IFN / IFG / IFS / IFGE / IFSE a b line, 11 MAX var
// (the image's highest frame id), 12 BLEND, 13 DISP, 14 POS x y, 15 WAIT min max, 16 ADD var n,
// 17 SUB var n. A wait or a SET from min to max is min when they are equal, else a random number
// from min up to (not including) max (the engine's MT19937, 0x4c82e0).
class AnmPlayer {
public:
    AnmPlayer(std::shared_ptr<const AnmScript> script, uint32_t seed);

    // The highest frame id of the image (MAX's value; cs2.exe 0x650560 -> 0x849800).
    void setMaxFrame(int32_t id) { maxFrame_ = id; }
    // A variable (0-63), as the scene sets them (lip sync, `anm set`; 0x8497b0).
    void setVar(int32_t index, int32_t value);
    int32_t var(int32_t index) const;

    // Runs the script up to `now` (ticks, 1/60 s). The first call starts it at that time (the image's
    // first update after a load, 0x6506a0 -> 0x849630).
    void advance(uint64_t now);
    // Starts the script over at its first line at the next advance (`anm reset`: 0x849650).
    void restart();

    int32_t frame() const { return frame_; }  // -1 until a line shows one
    int32_t blend() const { return blend_; }
    bool shown() const { return disp_ != 0; }
    int32_t posX() const { return posX_; }
    int32_t posY() const { return posY_; }
    bool ended() const { return pc_ >= static_cast<int32_t>(script_->lines.size()); }
    // The frames shown, newest first, each once: an image keeps the last frame it has when a line
    // names one it lacks (0x6500e0 changes frames only for an id it finds).
    const std::vector<int32_t>& history() const { return history_; }

private:
    int32_t value(const AnmScript::Operand& o) const;
    int32_t& slot(int32_t index);
    int32_t range(const AnmScript::Operand& lo, const AnmScript::Operand& hi);

    std::shared_ptr<const AnmScript> script_;
    MersenneTwister rng_;
    bool started_ = false;
    uint64_t base_ = 0;  // when the current line's wait began
    int64_t wait_ = 0;
    int32_t pc_ = -1;
    std::array<int32_t, 64> vars_{};
    int32_t junk_ = 0;  // where an out-of-range variable number writes
    int32_t maxFrame_ = 0;
    int32_t frame_ = -1, blend_ = 255, disp_ = 1, posX_ = 0, posY_ = 0;
    std::vector<int32_t> history_;
};

}  // namespace nyan::cs2
