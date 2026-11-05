#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nyan/cs2/emote.hpp"

namespace nyan::cs2 {

// The variable side of an E-mote character, as the SDK's player (emotedriver.dll 3.9.7) runs it,
// stepped once per frame. Read from the DLL:
//
// - SetVariable(name, value, frames, easing) (0x1000c370 / 0x1002a3a0): frames <= 0 sets the value
//   at once; otherwise a transition from the current value, value = from + (to - from) * p^power,
//   p advancing 1/frames per frame, power = 1 for easing 0, easing + 1 above, 1 / (1 - easing)
//   below. A transition already heading for the same value is kept when the new one would last
//   longer than what it has left.
// - Timelines (0x100196b0, 0x10019450, 0x10019180): playing does not interpolate keys itself. On
//   start, and each time playback passes a key, the variable is sent to that key's value over the
//   frames left until the next key, less one, with that key's easing. A looping timeline runs to
//   loopEnd, restarts at loopBegin, and carries the rest of the step over; a one-shot one ends at
//   its last key. PlayTimeline's flags (0x1001d720): without 1 (parallel) every playing timeline
//   stops first. With 2 (difference; Nekopara 0x1001f080 / 0x1001f1c0 / 0x1001f5c0, Amakano
//   0x10019070 / 0x10019180) each track with keys, unless its variable is in the model's
//   instantVariableList, moves a value of its own instead of the variable (the same transitions,
//   starting from 0, stepped after the keys, 0x1001f8c0); the pose gets the variable plus those
//   values times the timeline's blend ratio, 1 (0x10013530 -> 0x10020170). GetVariable, and so
//   cs2's inertia, sees the variable alone. cs2 plays the idle with 2 (Nekopara 0x571f88, Amakano
//   0x4d2126) and `m2intmot` with 1 (Amakano 0x4d5436).
// - Blinking (eyeControl, 0x1002dbb0), only when the model's blinkEnabled is set (nothing in the API
//   turns it on; every Amakano 2 model has 0): a phase goes beginFrame -> endFrame at 2.5 x (end - begin) /
//   blinkFrameCount per frame, holds blinkFrameCount / 5 frames, comes back at the same rate, then
//   waits a random blinkIntervalMin..Max frames (XorShift128, the DLL's generator). The eye
//   variable v shows v + (phase - begin) * (end - v) / (end - begin) while begin <= v <= end.
// - Physics (after the pose is evaluated, 0x1000b6d0 -> 0x1000ec10 bust, 0x100101b0 hair / parts):
//   each control follows its base layer's position, in model units (layer position plus where the
//   host puts the character, over its scale: the DLL's flag 0x200, scale factor from 0x10014c00),
//   relative to where it was first seen. The step is cut into pieces of at most 1.1 frames with
//   the position interpolated from the last frame's. Outputs go straight into the variables (so
//   the next frame's pose uses them); values all within 0.01 become 0.
//   - Bust (EPBustControl 0x10028b90): a particle q on a spring to the anchor P, v += spring dt
//     (P - q) + gravity dt g, v -= v friction dt, q += v dt; g = (-sin a, cos a) for the outer
//     rotation a. LR = atan(-(scaleX (P - q).x) 1.15 k) / k, UD = atan((-(P - q).y - ofs) scaleY
//     1.15 k) / k with k = pi / 80 (a soft limit of 40).
//   - Hair / parts (EPPendControl 0x1002e3c0): two links of length[0], length[1] from the anchor.
//     A stretched first link pulls its point back (v += n stretch bRate dt); a stretched second
//     link is put back at its length and loses vBound of its outward speed. Gravity and friction
//     (x, y separately) as for the bust. Each point's x offset from its rest point (straight
//     down) gives its angle (tip -> varLR, middle -> varLRM), the udEft point's y offset gives
//     varUD. Past |UD| 28 a bend grows (1/32 per frame, to 1) that sways LR and LRM in opposite
//     ways: phase += bendSpd bend dt, sway = sin(phase) bend bendVol.
// - Clamps (clampControl, 0x10015d70, after every variable is handed to the pose at 0x1000c860):
//   each pair, as x, y in -1..1 over min..max, is limited before the pose uses it; the variables
//   keep their own values. Unchanged if x or y is 0. Type 1: a point past the unit circle goes
//   onto it along its angle. Type 0: t = |x / y| (or its inverse, at most 1), (x, y) /= sqrt(1 +
//   t^2), then times 1 + (sin(r pi/2) / r - 1)(1 - cos(t pi/2)) for its length r: the square's
//   corners land on the circle, the axes stay put.
//   Wind and outer forces (StartWind / SetOuterForce / SetOuterRot) are never called by cs2.exe;
//   the hair, parts and bust scales stay at 1.
class EmotePlayer {
public:
    explicit EmotePlayer(std::shared_ptr<const EmoteModel> model, uint32_t seed = 88675123);

    const EmoteModel& model() const { return *model_; }
    std::shared_ptr<const EmoteModel> modelPtr() const { return model_; }

    void setVariable(const std::string& name, float value, float frames = 0, float easing = 0);
    float variable(const std::string& name) const;
    // A host's addition to a variable as drawn (cs2.exe's inertia, 0x5948f0), set each frame.
    void setOffset(const std::string& name, float value) { offsets_[name] = value; }

    // The DLL's IsAnimating (emotedriver 0x10011dc0, which cs2's sender asks, 0x56d280): a playing
    // timeline that doesn't loop, or a variable still moving that no playing timeline has keys for.
    // A looping idle alone is not animating.
    bool animating() const;
    // The DLL's Skip as cs2 sends it when a click ends `wait mpl` (message 0x20f -> 0x57c1e0: the
    // host's queued moves sent, Skip, Progress(1)): the host's moves, the variable transitions and
    // the one-shot timelines end where they were going; looping timelines play on.
    void skip();

    // Starts a timeline from its beginning (a running one with the same label restarts).
    static constexpr uint32_t kTimelineParallel = 1, kTimelineDifference = 2;
    void playTimeline(const std::string& label, uint32_t flags);
    void stopTimeline(const std::string& label);

    // The host's blinking (cs2.exe 0x4e2410, set up from the profile's #blink rows at 0x4d7610):
    // per entry, the next blink starts min + rand % range frames after the last ended; it runs the
    // variable start -> end -> start over `frames` (a triangle), then back to the rest value.
    // Setting the entries (a spec applied, 0x4d3273) puts each variable at rest and restarts the
    // waits; switching blinking off (`m2enable_blink 0`, 0x4e2b00) puts them at rest too.
    struct HostBlink {
        std::string variable;
        float rest = 0, start = 0, end = 0;
        int32_t frames = 1, min = 0, range = 1;
    };
    void setHostBlinks(std::vector<HostBlink> blinks);
    void setBlink(bool on);

    // The host's own variable moves (cs2.exe 0x5766e0 -> 0x56cfa0: a profile digit changing along its
    // #chain, as `m2body` does): the variable goes from its value now to `to` over `frames`, along
    // Tween::ease's curve `curve` (0 linear, 1 sin_fs, 2 sin_sf, 3 sin_ss), set by the host each
    // frame before the update; a new one for the same variable takes over from where it is.
    void hostTween(const std::string& name, float to, int32_t frames, uint8_t curve);

    // One frame. (x, y): where the host has the character's origin, in model units (moving it
    // swings the physics).
    void step(float dt = 1, float x = 0, float y = 0);

    // The values the pose is drawn with (blinking applied to the eye variable, pairs clamped).
    std::map<std::string, float> values() const;

private:
    // Selectors (EPSelectorControl): the chosen option, the changes waiting (value, frames) and the
    // frames left before the next one is taken (0x1000d360).
    struct SelectorState {
        const EmoteModel::Selector* s = nullptr;
        int32_t index = 0;
        std::deque<std::pair<float, float>> queue;
        float wait = 0;
    };
    std::vector<SelectorState> selectors_;
    void applySelector(SelectorState& s, int32_t index);
    struct Transition {
        float from = 0, to = 0, progress = 0, speed = 0, power = 1;
        bool active = false;
        std::deque<std::pair<float, std::pair<float, float>>> queue;  // to, (frames, power)
    };
    struct Playing {
        const EmoteModel::Timeline* tl = nullptr;
        std::string label;
        uint32_t flags = 0;
        float time = 0;
        std::vector<size_t> index;  // per track: the key last passed
        // Per track of a difference timeline: whether it moves its own value, and that value.
        struct Diff {
            bool on = false;
            Transition tr;
            float value = 0;
        };
        std::vector<Diff> diff;
    };

    struct BustState {
        const EmoteModel::Bust* c = nullptr;
        bool first = true, ctlFirst = true;
        std::array<float, 3> P{}, q{}, v{};
        float offX = 0, offY = 0, prevX = 0, prevY = 0;
    };
    struct PendState {
        const EmoteModel::Pend* c = nullptr;
        bool first = true, ctlFirst = true;
        std::array<float, 3> A{};
        std::array<float, 3> p[2]{}, v[2]{};
        float offX = 0, offY = 0, prevX = 0, prevY = 0, phase = 0, bend = 0;
    };
    void bustUpdate(BustState& s, float x, float y, float dt, float& lr, float& ud) const;
    void pendUpdate(PendState& s, float x, float y, float dt, float out[3]) const;
    void physics(float dt, float x, float y);
    void stepBlink(float dt);

    // A transition sent towards `to` (0x1000da10); true: no frames, the value is to be set at once.
    static bool sendTransition(Transition& tr, float to, float frames, float easing);
    // One frame of a transition (0x1000dbb0); a queued one starts from `value`.
    static void stepTransition(Transition& tr, float& value, float dt);
    // A key reached by track `i`: its variable, or the track's own value in a difference timeline.
    void sendKey(Playing& p, size_t i, float value, float frames, float easing);
    void resetTimeline(Playing& p, float time);
    void advanceTimeline(Playing& p, float time, bool inclusive);
    float random01();

    std::shared_ptr<const EmoteModel> model_;
    std::map<std::string, float> values_;
    std::map<std::string, float> offsets_;  // setOffset
    std::map<std::string, Transition> transitions_;
    std::vector<Playing> playing_;

    std::vector<BustState> busts_;
    std::vector<PendState> pends_;  // hair, then parts
    std::set<std::string> anchors_;

    struct HostBlinkState {
        HostBlink b;
        uint64_t startAt = 0, endAt = 0;
    };
    std::vector<HostBlinkState> hostBlinks_;
    uint64_t clock_ = 0;  // frames stepped
    void stepHostBlinks();
    struct HostTween {
        float from = 0, to = 0;
        uint64_t start = 0;
        int32_t frames = 1;
        uint8_t curve = 0;
    };
    std::map<std::string, HostTween> hostTweens_;
    void stepHostTweens();

    bool blinkOn_ = true;
    int blinkState_ = 0;
    float blinkPhase_ = 0, blinkCount_ = 0;
    uint32_t rng_[4];
};

}  // namespace nyan::cs2
