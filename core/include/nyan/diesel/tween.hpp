#pragma once

#include <array>
#include <cstdint>

#include "nyan/diesel/action.hpp"

namespace nyan::diesel {

// The engine's tweens of one to three components (the sprites' properties: muramasa_en.exe's
// 0x2c-byte scalar, 0x3c-byte Vector2D and 0x4c-byte Vector3D tweens). They work as Action1D does:
// one timer advanced once per milli time by frame speed * 16.666666 * time scale within the frame's
// step limit, each component eased from src to dst by the same curve, scaled by power, snapping to
// dst at the end. Nyanroid eases every component with the scalar curve (the exe's vector tables,
// 0x5e6910 and 0x5e6688, apply the same formulas to each component; not compared bit for bit).
struct Tween {
    int n = 1;                 // components
    float actionTime = 0.0f;   // ms
    float timer = 0.0f;
    float lastStamp = 0.0f;
    std::array<float, 3> src{}, dst{}, now{}, nowNoPower{};
    int32_t tempo = 0;
    int32_t active = 0;
    float power = 1.0f;
    float timeScale = 1.0f;

    Tween() = default;
    // A tween resting at `v` (the constructors' set with time 0).
    Tween(int components, float x, float y = 0.0f, float z = 0.0f);

    // Starts from `from` towards `to` over `timeMs` (read as unsigned) along `curve` (above 160: 0);
    // a zero time jumps to `to`; equal ends leave `active` as it was (0x42fa80, 0x45d2e0, 0x45d720).
    void set(const std::array<float, 3>& from, const std::array<float, 3>& to, int32_t timeMs, int32_t curve);
    // From where it is now (nowNoPower) to `to`: what the sprites' setters do.
    void moveTo(const std::array<float, 3>& to, int32_t timeMs, int32_t curve) { set(nowNoPower, to, timeMs, curve); }
    // One frame (0x42f990, 0x45d1a0, 0x45d5c0); true if it was active.
    bool step(const FrameClock& clock);
    // setXTimer (0x45b550, 0x5112d0): the timer within [0, actionTime], active if it has a duration,
    // re-evaluated.
    void setTimer(float ms);
    // Re-evaluates now from the timer if active (0x45def0, 0x45df70).
    void refresh();
};

}  // namespace nyan::diesel
