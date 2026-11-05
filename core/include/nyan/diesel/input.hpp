#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>

namespace nyan::diesel {

// Mware's key code (DirectInput's DIK_* numbering) for a Windows virtual-key code, as Mware's
// window procedure maps WM_KEYDOWN / WM_KEYUP (Mware.dll 0x100369f0); 0 for keys it does not map.
// VK_MENU gives 0xb8 (right Alt) and VK_CONTROL 0x1d (left Ctrl), as there.
int keyCodeForVk(int vk);

// Mware's keyboard (Mware.dll CKeyboard). Each key that has events keeps a queue of them, each a
// state (1 down, 3 up) and a count of frame updates seen; the front event is the key's state. Window
// messages add events; once a frame, before the scripts run, update() ages and retires them.
class Keyboard {
public:
    void keyDown(int code);  // 0x10043de0
    void keyUp(int code);    // 0x10043ed0
    void releaseAll();       // 0x10043d10 (the window lost focus)
    void update();           // 0x10043960

    // Down (pressed or held); with justPressed, only in the frames right after the press
    // (0x10043b60). Right-hand Ctrl / Alt / Win / Shift codes ask about the left-hand key.
    bool isDown(int code, bool justPressed) const;
    // Released this frame (0x10043c60).
    bool isUp(int code) const;
    // 0 no event, 1 just pressed, 2 held, 3 released (0x10043d40; the code as given).
    int state(int code) const;

private:
    struct Event {
        int32_t count = 0;
        int32_t state = 0;
    };
    std::map<int, std::deque<Event>> keys_;
};

// Mware's mouse (Mware.dll CMouse), positions in game-screen pixels. Buttons keep event queues as
// the keyboard's keys do, with states 2 down and 1 up; the wheel keeps notches (5 toward the user,
// 6 away).
class Mouse {
public:
    enum Button { Left = 0, Right = 1, Middle = 2 };

    void button(Button b, bool down);  // 0x10010e80 (left), 0x10010f00 (right), 0x10010f90 (middle)
    void wheel(int delta);             // 0x10010dd0 (WM_MOUSEWHEEL's delta)
    void moveTo(int x, int y);         // where the pointer is now
    void releaseAll();                 // 0x10011560
    void clear();                      // 0x10011550: buttons read as none until all are retired
    void update();                     // 0x100106a0

    // 0 none, 1 released, 2 just pressed, 3 held (0x100110d0 / 0x10011160 / 0x100111f0).
    int buttonState(Button b) const;
    // 4 none, 5 or 6 a notch (0x10011290).
    int wheelState() const;
    int x() const { return x_; }
    int y() const { return y_; }
    int moveX() const { return moveX_; }  // since the last update
    int moveY() const { return moveY_; }

private:
    struct Event {
        int32_t count = 0;
        int32_t state = 0;
    };
    std::array<std::deque<Event>, 3> buttons_;
    std::deque<Event> wheel_;
    bool cleared_ = false;
    int pointerX_ = 0, pointerY_ = 0;  // as last reported
    int x_ = 0, y_ = 0, lastX_ = 0, lastY_ = 0, moveX_ = 0, moveY_ = 0;
};

// The task's key repeat (muramasa_en.exe CTask: 0x485e00 asks, 0x4860f0 keeps the times):
// SQApp.isKeyDown(key, interval) answers true for a held key at most once per interval.
class KeyRepeat {
public:
    static constexpr int kKeys = 0xdc;
    // 0x485e00 with mode 0: false within `interval` ms of the last true for this key.
    bool isKeyDown(const Keyboard& keyboard, int code, uint32_t now, uint32_t interval);
    // 0x4860f0, each frame before the script: the keys answered true get the time; released keys
    // are cleared (0x7fffffff, so the next press answers at once).
    void update(const Keyboard& keyboard, uint32_t now);

private:
    std::array<int32_t, kKeys> answered_{};
    std::array<uint32_t, kKeys> last_{};
};

// The input devices as the scripts' task sees them.
struct Input {
    Keyboard keyboard;
    Mouse mouse;
    KeyRepeat repeat;
};

}  // namespace nyan::diesel
