#pragma once

#include <cstdint>

namespace nyan::diesel {

// The app's frame clock (muramasa_en.exe: the core's time stamp, app+0x100, app+0x108, set at the
// start of each frame by 0x42edb0): milliseconds since start, and from the time since the last frame
// the frame speed (elapsed * 0.06, within [0, 5]) and the longest step a tween may take this frame
// (elapsed, within [0, 83.33]; SQApp.setMaxFrameTime changes it for the frame).
struct FrameClock {
    uint32_t milliTime = 0;     // the timer's milliseconds (SQApp.getMilliTime)
    float frameSpeed = 0.0f;    // app+0x100 (SQApp.getFrameSpeed)
    float maxFrameTime = 0.0f;  // app+0x108 (SQApp.setMaxFrameTime changes it)
    uint32_t lastMilli = 0;     // app+0xd0

    // The timer reads `now` (ms) from here on.
    void setTime(uint32_t now) { milliTime = now; }
    // At the start of a frame (0x42edb0): the frame speed and the step limit from the time since the
    // last frame began.
    void beginFrame();
};

// The engine's 1D tween (a 0x2c-byte struct embedded in SQAction1D at +0xb8 and in the sprites'
// animated properties): a value going from src to dst over actionTime milliseconds along one of
// 161 curves ("tempo", muramasa_en.exe table 0x5e57a8), scaled by power.
struct Action1D {
    float actionTime = 0.0f;  // +0x00, ms
    float timer = 0.0f;       // +0x04, ms elapsed
    float lastStamp = 0.0f;   // +0x08, the milli time of the last step
    float src = 0.0f;         // +0x0c
    float dst = 0.0f;         // +0x10
    float now = 0.0f;         // +0x14, nowNoPower * power
    float nowNoPower = 0.0f;  // +0x18
    int32_t tempo = 0;        // +0x1c, curve index
    int32_t active = 0;       // +0x20
    float power = 1.0f;       // +0x24
    float timeScale = 1.0f;   // +0x28

    // The curve's value at the current timer (the table's function for `tempo`).
    float curve() const;
    // Recomputes now / nowNoPower from the curve if active (what most setters do after a change).
    void refresh();
    // setAction(src, dst, time, tempo) (0x54d950): starts a tween; a zero time jumps to dst.
    void set(float from, float to, int32_t timeMs, int32_t curveIndex);
    // One frame's step (0x42f990): advances the timer once per milli time, then evaluates; at the end
    // snaps to dst and stops. Returns whether it was active.
    bool step(const FrameClock& clock);
    // finish() (0x54da20): jumps to dst; true if it was active.
    bool finish();
    void setTimer(float ms);         // 0x54dac0
    void resetTimer();               // 0x54dec0
    void setActionTime(int32_t ms);  // 0x54db40 (the int read as unsigned)
    void setSrc(float v);            // 0x54dbc0
    void setDst(float v);            // 0x54dc20
    void setNow(float v);            // 0x54dc80
    void setPower(float v);          // 0x54ddb0
    void setTempo(int32_t t);        // 0x54de30
};

// The curve function `index` (0..160) at (src, dst, duration, time); out-of-range indexes use 0.
float easing(int32_t index, float src, float dst, float duration, float time);

}  // namespace nyan::diesel
