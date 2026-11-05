#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nyan::wa2 {

// The engine's input as it polls it once per logic frame (WA2.exe 0x44a600 mouse, 0x44a350 keys):
// 74 keys in a fixed order with held / pressed / repeat tables, three mouse buttons likewise plus a
// released edge, the wheel, the pointer and the hit areas menus test it against.
class Input {
public:
    // The keys the engine polls, in its table order (0x449ba0).
    enum Key : int {
        Up, Down, Left, Right, Enter, Space, Back, Ctrl, Shift, Alt, Tab, Escape, PageUp, PageDown, End, Home,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        Digit0, Num0 = Digit0 + 10, LetterA = Num0 + 10,
        KeyCount = LetterA + 26,
    };
    enum Button : int { LeftButton = 0, RightButton = 1, MiddleButton = 2 };

    // The table index of a Windows virtual-key code; -1 if the engine does not poll it.
    static int keyIndex(int vk);

    // Device state from the platform (any time).
    void setKey(int vk, bool down);
    void setButton(int button, bool down);
    void setPointer(float x, float y);  // virtual screen pixels (1280 x 720)
    void addWheel(int steps);           // > 0 away from the user
    void releaseAll();

    // One logic frame's poll: the tables below change only here.
    void poll();

    bool held(int key) const { return valid(key) && held_[static_cast<size_t>(key)]; }
    bool pressed(int key) const { return valid(key) && pressed_[static_cast<size_t>(key)]; }
    bool repeat(int key) const { return valid(key) && repeat_[static_cast<size_t>(key)]; }
    bool buttonHeld(int b) const { return b >= 0 && b < 3 && buttonHeld_[static_cast<size_t>(b)]; }
    bool buttonPressed(int b) const { return b >= 0 && b < 3 && buttonPressed_[static_cast<size_t>(b)]; }
    bool buttonReleased(int b) const { return b >= 0 && b < 3 && buttonReleased_[static_cast<size_t>(b)]; }
    bool buttonRepeat(int b) const { return b >= 0 && b < 3 && buttonRepeat_[static_cast<size_t>(b)]; }
    int wheel() const { return wheel_; }  // this frame's steps (0xb601c0)
    int pointerX() const { return pointerX_; }
    int pointerY() const { return pointerY_; }
    // Letters typed, newest first (0x44a1c0 keeps the last 256 in 0xb60038).
    const std::string& typed() const { return typed_; }

    // Hit areas (0xb601f0: groups of 128 rectangles, 10 bytes each); the area under the pointer in
    // the current group, -1 if none (0xb601c4).
    void setArea(int group, int index, int x, int y, int w, int h, bool active);
    void clearAreas(int group);                          // 0x44b330
    void clearAllAreas();                                // 0x44b350
    void enableArea(int group, int index, bool active);  // 0x44b310
    bool areaActive(int group, int index) const;         // 0x44b040
    // 0x44b0d0: the pointer onto an area's centre (keyboard choice moves).
    void pointAt(int group, int index);
    void setGroup(int group) { group_ = group >= 0 && group < kGroups ? group : 0; }  // 0x44b070
    int hoveredArea() const { return hovered_; }

private:
    static bool valid(int k) { return k >= 0 && k < KeyCount; }
    struct Area {
        bool active = false;
        int16_t x = 0, y = 0, w = 0, h = 0;
    };

    std::array<bool, KeyCount> down_{};  // what the platform says now
    std::array<bool, KeyCount> held_{}, pressed_{}, repeat_{};
    std::array<int, KeyCount> holdCount_{};
    std::array<bool, 3> buttonDown_{};
    std::array<bool, 3> buttonHeld_{}, buttonPressed_{}, buttonReleased_{}, buttonRepeat_{};
    std::array<int, 3> buttonCount_{};
    int pendingWheel_ = 0, wheel_ = 0;
    float rawX_ = 0, rawY_ = 0;
    int pointerX_ = 0, pointerY_ = 0;
    std::string typed_;
    static constexpr int kGroups = 0x30;
    std::array<std::array<Area, 128>, kGroups> areas_{};
    int group_ = 0;
    int hovered_ = -1;
};

}  // namespace nyan::wa2
