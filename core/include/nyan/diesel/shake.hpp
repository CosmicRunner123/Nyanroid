#pragma once

#include <array>
#include <cstdint>

#include "nyan/diesel/random.hpp"
#include "nyan/diesel/tween.hpp"

namespace nyan::diesel {

// A sprite's shake: Diesel::CEffectShake (muramasa_en.exe 0x4a6d90, 0x100 bytes; set 0x4a7030,
// update 0x4a7300, setTimer 0x4a7850) and Diesel::CEffectRandomShake (0x4a9a70; set 0x4a9ab0,
// update 0x4a9c70, setTimer 0x4a9ea0). CSprite keeps one at +0x658 and adds its offset to the
// sprite's position in the local matrix (0x455b10).
//
// Three tweens: the master timer (+0x24, 0 to 1 over the shake's time; `isShaking` is its activity),
// the amplitude (+0x50, from `start` to `end` over the time along `tempo`) and the swing (+0x9c,
// along `beatTempo`). The plain shake swings from side to side: first from 0 to -amplitude over half
// of the time's remainder by the frequency (or of the frequency), then from one side to the other
// every `frequency` ms, the sides changing sign; a last, shorter swing when little time is left; and
// back to 0 over what remains. The random shake swings to the amplitude times a random number in
// [-1, 1] per axis every `frequency` ms (shorter swings at the end), then back to 0. Frequency 0
// swings every frame. With `interpolation` 0 the offset is the swing's target, not its way there.
class Shake {
public:
    explicit Shake(bool random, int32_t seed = 0) : random_(random), rng_(seed) {}

    // set(start, end, frequency, interpolation, time, tempo, beatTempo): vtable +0x18.
    void set(const std::array<float, 3>& start, const std::array<float, 3>& end, uint32_t frequency,
             int32_t interpolation, uint32_t time, int32_t tempo, int32_t beatTempo);
    // The frame's step (vtable +0x1c), which CSprite runs while it shakes.
    void update(const FrameClock& clock);
    // The timer moved to `ms` and the swing worked out for it (vtable +0x40; setShakeTimer).
    void setTimer(float ms);
    void setTimeScale(float s);  // vtable +0x44: the three tweens'

    bool shaking() const { return master_.active != 0; }  // vtable +0x20
    float timer() const { return master_.timer; }          // vtable +0x3c
    float time() const { return master_.actionTime; }      // vtable +0x38
    bool random() const { return random_; }
    // What the sprite's position gets (vtable +0x24): the swing now, or its target.
    std::array<float, 3> offset() const { return interpolate_ ? swing_.now : swing_.dst; }

    // What its record keeps (CEffectShake 0x4a7dc0 writes, 0x4a81e0 reads, in this order), and the
    // random shake's generator (0x4aa270 / 0x4aa3b0).
    struct Saved {
        Tween master{1, 0.0f}, amplitude{3, 0.0f, 0.0f, 0.0f}, swing{3, 0.0f, 0.0f, 0.0f};
        int32_t interpolate = 0, beatTempo = 0;
        float sign = 1.0f;
        int32_t final = 0;
        uint32_t frequency = 0, edge = 0;
    };
    Saved saved() const {
        return {master_, amplitude_, swing_, interpolate_ ? 1 : 0, beatTempo_, sign_, final_ ? 1 : 0, frequency_, edge_};
    }
    void restore(const Saved& s) {
        master_ = s.master, amplitude_ = s.amplitude, swing_ = s.swing;
        interpolate_ = s.interpolate != 0, beatTempo_ = s.beatTempo, sign_ = s.sign, final_ = s.final != 0;
        frequency_ = s.frequency, edge_ = s.edge;
    }
    MwareRandom& generator() { return rng_; }

private:
    void swingTo(const std::array<float, 3>& from, const std::array<float, 3>& to, uint32_t ms);
    std::array<float, 3> randomTarget();

    bool random_;
    MwareRandom rng_;       // +0x100 (random only): seeded like CRandom() from timeGetTime
    Tween master_{1, 0.0f};  // +0x24
    Tween amplitude_{3, 0.0f, 0.0f, 0.0f};  // +0x50
    Tween swing_{3, 0.0f, 0.0f, 0.0f};      // +0x9c
    bool interpolate_ = false;  // +0xe8
    int32_t beatTempo_ = 0;     // +0xec
    float sign_ = 1.0f;         // +0xf0
    bool final_ = false;        // +0xf4: the swing back to 0 has begun
    uint32_t frequency_ = 0;    // +0xf8
    uint32_t edge_ = 0;         // +0xfc: plain: the first swing's time; random: time % frequency
};

}  // namespace nyan::diesel
