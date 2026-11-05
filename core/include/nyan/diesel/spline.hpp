#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

// Diesel::CTimedNouniformSpline<Mware::Vector3D> (muramasa_en.exe vtable 0x5e6658: step 0x45f9c0,
// begin 0x45edb0): the path setSpline / beginSpline give a sprite (CSprite +0x594) or a text
// (CText +0x470). Each key is a point with a time (ms) and a weight; begin makes a 0-to-1 tween over
// the given time and, with three keys or more, a Hermite curve through them whose tangents come
// from the keys' spacing in time (three smoothing passes; a path that ends where it starts is
// closed); two keys are joined by a straight line. While the tween runs, the owner's update puts
// its position at `point`.
class TimedSpline {
public:
    // setSpline(x, y, z, time, weight) (0x45fa50): a key at the end.
    void add(const std::array<float, 3>& p, int32_t timeMs, float weight);
    // resetSpline (0x4597c0, 0x4d8ee0): no keys; a running tween ends where it is going.
    void reset();
    // vtable +4 (0x45edb0): the curve from the keys, run over `timeMs` along `curve`.
    void begin(int32_t timeMs, int32_t curve);
    // vtable +0 (0x45f9c0): the tween steps and the point is worked out; true if it ran.
    bool step(const FrameClock& clock);
    // setSplineTimer (0x4602a0): the tween's timer, then the point.
    void setTimer(float ms);
    // Whether begin made a curve (setSplineTimer works only then: +0xc != +0x10).
    bool built() const { return !entries_.empty(); }

    Tween t{1, 0.0f};             // +0x1c
    std::array<float, 3> point{}; // +0x48

private:
    struct Key {
        std::array<float, 3> pos{};
        float time = 0.0f;   // +0x14
        float weight = 0.0f; // +0x1c
    };
    // The curve's keys (0x34 bytes each): the point, the tangents into it and out of it (scaled by
    // the segment's length in seconds), its time as a fraction of the whole, its weight.
    struct Entry {
        std::array<float, 3> pos{};
        std::array<float, 3> in{};   // +0xc
        std::array<float, 3> out{};  // +0x18
        float t = 0.0f;              // +0x24
        float weight = 0.0f;         // +0x2c
    };
    void restart(int32_t timeMs, int32_t curve);
    std::array<float, 3> startTangent(size_t i) const;  // 0x461460
    std::array<float, 3> endTangent(size_t i) const;    // 0x4615c0
    void findSegment();                                 // 0x461720
    bool evaluate();                                    // 0x4617e0

    std::vector<Key> keys_;       // +4 (a list)
    std::vector<Entry> entries_;  // +0xc
    int32_t segment_ = -1;        // +0x18: the segment the tween is in (-1: not running)
    float first_ = 0.0f, last_ = 0.0f;  // +0x54 / +0x58: the first and last keys' times
};

}  // namespace nyan::diesel
