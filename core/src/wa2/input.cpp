#include "nyan/wa2/input.hpp"

#include <cmath>

namespace nyan::wa2 {

namespace {

// The engine's repeat rule (0x44a290, and for the mouse 0x44a600): on the first frame held, then on
// every frame once held 16 frames (the count stops at 16, a multiple of 4).
bool repeats(int count) {
    return count == 1 || (count > 15 && count % 4 == 0);
}

}  // namespace

int Input::keyIndex(int vk) {
    static const int fixed[] = {0x26, 0x28, 0x25, 0x27, 0x0d, 0x20, 0x08, 0x11, 0x10, 0x12, 0x09, 0x1b, 0x21, 0x22, 0x23, 0x24};
    for (int i = 0; i < 16; ++i) {
        if (fixed[i] == vk) return i;
    }
    if (vk >= 0x70 && vk <= 0x7b) return F1 + (vk - 0x70);
    if (vk >= 0x30 && vk <= 0x39) return Digit0 + (vk - 0x30);
    if (vk >= 0x60 && vk <= 0x69) return Num0 + (vk - 0x60);
    if (vk >= 0x41 && vk <= 0x5a) return LetterA + (vk - 0x41);
    // Left / right variants of the modifiers count as the modifier (GetAsyncKeyState of 0x10-0x12).
    if (vk == 0xa0 || vk == 0xa1) return Shift;
    if (vk == 0xa2 || vk == 0xa3) return Ctrl;
    if (vk == 0xa4 || vk == 0xa5) return Alt;
    return -1;
}

void Input::setKey(int vk, bool down) {
    int k = keyIndex(vk);
    if (k >= 0) down_[static_cast<size_t>(k)] = down;
}

void Input::setButton(int button, bool down) {
    if (button >= 0 && button < 3) buttonDown_[static_cast<size_t>(button)] = down;
}

void Input::setPointer(float x, float y) {
    rawX_ = x;
    rawY_ = y;
}

void Input::addWheel(int steps) {
    pendingWheel_ += steps;
}

void Input::releaseAll() {
    down_ = {};
    buttonDown_ = {};
}

void Input::setArea(int group, int index, int x, int y, int w, int h, bool active) {
    if (group < 0 || group >= kGroups || index < 0 || index >= 128) return;
    areas_[static_cast<size_t>(group)][static_cast<size_t>(index)] =
        Area{active, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

void Input::clearAreas(int group) {
    // 0x44b330: only the active flags are cleared.
    if (group < 0 || group >= kGroups) return;
    for (Area& a : areas_[static_cast<size_t>(group)]) a.active = false;
}

void Input::clearAllAreas() {
    for (int g = 0; g < kGroups; ++g) clearAreas(g);
}

bool Input::areaActive(int group, int index) const {
    if (group < 0 || group >= kGroups || index < 0 || index >= 128) return false;
    return areas_[static_cast<size_t>(group)][static_cast<size_t>(index)].active;
}

void Input::enableArea(int group, int index, bool active) {
    if (group < 0 || group >= kGroups || index < 0 || index >= 128) return;
    areas_[static_cast<size_t>(group)][static_cast<size_t>(index)].active = active;
}

void Input::pointAt(int group, int index) {
    if (group < 0 || group >= kGroups || index < 0 || index >= 128) return;
    const Area& a = areas_[static_cast<size_t>(group)][static_cast<size_t>(index)];
    setPointer(static_cast<float>(a.x + a.w / 2), static_cast<float>(a.y + a.h / 2));
}

void Input::poll() {
    // Keys (0x44a350): held now, pressed = held and not held last frame, repeat by the count.
    for (int k = 0; k < KeyCount; ++k) {
        auto i = static_cast<size_t>(k);
        bool was = held_[i];
        held_[i] = down_[i];
        pressed_[i] = held_[i] && !was;
        holdCount_[i] = held_[i] ? std::min(holdCount_[i] + 1, 16) : 0;
        repeat_[i] = held_[i] && repeats(holdCount_[i]);
    }
    // Letters pressed this frame go to the front of the typed history (0x44a1c0: the last one of the
    // frame in table order).
    int letter = -1;
    for (int k = LetterA; k < LetterA + 26; ++k) {
        if (pressed_[static_cast<size_t>(k)]) letter = k - LetterA;
    }
    if (letter >= 0) {
        typed_.insert(typed_.begin(), static_cast<char>('a' + letter));
        if (typed_.size() > 256) typed_.resize(256);
    }
    // Mouse (0x44a600): held, pressed and released edges, repeat by the count.
    for (size_t b = 0; b < 3; ++b) {
        bool was = buttonHeld_[b];
        buttonHeld_[b] = buttonDown_[b];
        buttonPressed_[b] = buttonHeld_[b] && !was;
        buttonReleased_[b] = !buttonHeld_[b] && was;
        buttonCount_[b] = buttonHeld_[b] ? buttonCount_[b] + 1 : 0;
        buttonRepeat_[b] = buttonHeld_[b] && repeats(buttonCount_[b]);
    }
    wheel_ = pendingWheel_;
    pendingWheel_ = 0;
    pointerX_ = static_cast<int>(std::lround(rawX_));
    pointerY_ = static_cast<int>(std::lround(rawY_));
    hovered_ = -1;
    if (pointerX_ >= 1 && pointerY_ >= 1 && pointerX_ <= 1280 && pointerY_ <= 720) {
        const auto& g = areas_[static_cast<size_t>(group_)];
        for (int i = 0; i < 128; ++i) {
            const Area& a = g[static_cast<size_t>(i)];
            if (a.active && a.x <= pointerX_ && a.y <= pointerY_ && pointerX_ < a.x + a.w && pointerY_ < a.y + a.h) {
                hovered_ = i;
                break;
            }
        }
    }
}

}  // namespace nyan::wa2
