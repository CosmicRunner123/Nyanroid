#include "nyan/diesel/input.hpp"

#include <vector>

namespace nyan::diesel {

namespace {

// Mware.dll 0x100369f0's table, in its order (the same for key down and key up).
constexpr struct {
    uint8_t vk, code;
} kVkCodes[] = {
    {0x08, 0x0e}, {0x09, 0x0f}, {0x0d, 0x1c}, {0x10, 0x2a}, {0x11, 0x1d}, {0x12, 0xb8}, {0x13, 0xc5}, {0x14, 0x3a},
    {0x1b, 0x01}, {0x20, 0x39}, {0x21, 0xc9}, {0x22, 0xd1}, {0x23, 0xcf}, {0x24, 0xc7}, {0x25, 0xcb}, {0x26, 0xc8},
    {0x27, 0xcd}, {0x28, 0xd0}, {0x2a, 0xb7}, {0x2d, 0xd2}, {0x2e, 0xd3}, {0x5b, 0xdb}, {0x5c, 0xdc}, {0x60, 0x52},
    {0x61, 0x4f}, {0x62, 0x50}, {0x63, 0x51}, {0x64, 0x4b}, {0x65, 0x4c}, {0x66, 0x4d}, {0x67, 0x47}, {0x68, 0x48},
    {0x69, 0x49}, {0x6a, 0x37}, {0x6b, 0x4e}, {0x6c, 0x9c}, {0x6d, 0x4a}, {0x6e, 0x53}, {0x6f, 0xb5}, {0x70, 0x3b},
    {0x71, 0x3c}, {0x72, 0x3d}, {0x73, 0x3e}, {0x74, 0x3f}, {0x75, 0x40}, {0x76, 0x41}, {0x77, 0x42}, {0x78, 0x43},
    {0x79, 0x44}, {0x7a, 0x57}, {0x7b, 0x58}, {0x90, 0x45}, {0x91, 0x46}, {0x31, 0x02}, {0x32, 0x03}, {0x33, 0x04},
    {0x34, 0x05}, {0x35, 0x06}, {0x36, 0x07}, {0x37, 0x08}, {0x38, 0x09}, {0x39, 0x0a}, {0x30, 0x0b}, {0x51, 0x10},
    {0x57, 0x11}, {0x45, 0x12}, {0x52, 0x13}, {0x54, 0x14}, {0x59, 0x15}, {0x55, 0x16}, {0x49, 0x17}, {0x4f, 0x18},
    {0x50, 0x19}, {0x41, 0x1e}, {0x53, 0x1f}, {0x44, 0x20}, {0x46, 0x21}, {0x47, 0x22}, {0x48, 0x23}, {0x4a, 0x24},
    {0x4b, 0x25}, {0x4c, 0x26}, {0x5a, 0x2c}, {0x58, 0x2d}, {0x43, 0x2e}, {0x56, 0x2f}, {0x42, 0x30}, {0x4e, 0x31},
    {0x4d, 0x32},
};

// The queries' folding of right-hand modifier keys onto the left-hand ones (0x10043b60).
int leftHand(int code) {
    switch (code) {
        case 0x9d: return 0x1d;  // right Ctrl
        case 0xb8: return 0x38;  // right Alt
        case 0xdc: return 0xdb;  // right Win
        case 0x36: return 0x2a;  // right Shift
        default: return code;
    }
}

template <typename Event>
void popFront(std::deque<Event>& q) {
    q.pop_front();
}

// One update of an event queue (the keyboard's and the mouse buttons' are the same apart from the
// state numbers): a down event counts up to 2 and stays while it is the last; an up event counts to
// 1 and then leaves.
template <typename Event>
void age(std::deque<Event>& q, int32_t down, int32_t up) {
    if (q.empty()) return;
    Event& f = q.front();
    if (f.state == down && f.count <= 1) {
        ++f.count;
    } else if (f.state == up && f.count < 1) {
        ++f.count;
    } else if (f.state == up) {
        popFront(q);
    } else if (f.state == down && q.size() > 1) {
        popFront(q);
    }
}

}  // namespace

int keyCodeForVk(int vk) {
    for (const auto& e : kVkCodes) {
        if (e.vk == vk) return e.code;
    }
    return 0;
}

// --- Keyboard -------------------------------------------------------------------------------------

void Keyboard::keyDown(int code) {
    // A key with events already keeps them (a repeat while held adds nothing).
    if (keys_.count(code)) return;
    keys_[code].push_back({0, 1});
}

void Keyboard::keyUp(int code) {
    auto it = keys_.find(code);
    if (it == keys_.end() || it->second.empty()) return;
    auto& q = it->second;
    if (q.back().state != 1) return;
    // A press the scripts may have seen gives way to the release; one not yet aged stays a frame.
    if (q.front().count > 0) q.pop_back();
    q.push_back({0, 3});
}

void Keyboard::releaseAll() {
    std::vector<int> codes;
    for (const auto& [code, q] : keys_) codes.push_back(code);
    for (int c : codes) keyUp(c);
}

void Keyboard::update() {
    for (auto it = keys_.begin(); it != keys_.end();) {
        age(it->second, 1, 3);
        if (it->second.empty()) {
            it = keys_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Keyboard::isDown(int code, bool justPressed) const {
    auto it = keys_.find(leftHand(code));
    if (it == keys_.end() || it->second.empty()) return false;
    const Event& f = it->second.front();
    if (f.state != 1) return false;
    return !justPressed || f.count < 2;
}

bool Keyboard::isUp(int code) const {
    auto it = keys_.find(leftHand(code));
    return it != keys_.end() && !it->second.empty() && it->second.front().state == 3;
}

int Keyboard::state(int code) const {
    auto it = keys_.find(code);
    if (it == keys_.end() || it->second.empty()) return 0;
    const Event& f = it->second.front();
    if (f.state == 1) return f.count > 1 ? 2 : 1;
    return f.state;
}

// --- Mouse ----------------------------------------------------------------------------------------

void Mouse::button(Button b, bool down) {
    auto& q = buttons_[b];
    if (down) {
        if (q.empty()) q.push_back({0, 2});
        return;
    }
    if (q.empty() || q.back().state != 2) return;
    if (q.front().count > 0) q.pop_back();
    q.push_back({0, 1});
}

void Mouse::wheel(int delta) {
    int16_t d = static_cast<int16_t>(delta);
    if (d == 0) {
        wheel_.clear();
        return;
    }
    int32_t dir = d > 0 ? 6 : 5;
    // Notches the same way as the last one waiting restart it.
    if (!wheel_.empty() && wheel_.back().state == dir) {
        wheel_.back().count = 0;
        return;
    }
    wheel_.push_back({0, dir});
}

void Mouse::moveTo(int x, int y) {
    pointerX_ = x;
    pointerY_ = y;
}

void Mouse::releaseAll() {
    button(Left, false);
    button(Right, false);
    button(Middle, false);
    wheel(0);
}

void Mouse::clear() { cleared_ = true; }

void Mouse::update() {
    bool none = true;
    for (auto& q : buttons_) {
        if (q.empty()) continue;
        none = false;
        age(q, 2, 1);
    }
    if (wheel_.empty()) {
        if (none) cleared_ = false;
    } else if (wheel_.front().count < 1) {
        ++wheel_.front().count;
    } else {
        wheel_.pop_front();
    }
    // The pointer: where it is now, and how far it went since the last update.
    moveX_ = pointerX_ - lastX_;
    moveY_ = pointerY_ - lastY_;
    lastX_ = pointerX_;
    lastY_ = pointerY_;
    x_ = pointerX_;
    y_ = pointerY_;
}

int Mouse::buttonState(Button b) const {
    const auto& q = buttons_[b];
    if (cleared_ || q.empty()) return 0;
    const Event& f = q.front();
    if (f.state == 2) return f.count > 1 ? 3 : 2;
    return f.state;
}

int Mouse::wheelState() const {
    if (cleared_ || wheel_.empty()) return 4;
    return wheel_.front().state;
}

// --- KeyRepeat ------------------------------------------------------------------------------------

bool KeyRepeat::isKeyDown(const Keyboard& keyboard, int code, uint32_t now, uint32_t interval) {
    if (code < 0 || code >= kKeys) return keyboard.isDown(code, false);
    if (now - last_[code] < interval) return false;
    if (!keyboard.isDown(code, false)) return false;
    answered_[code] = 1;
    return true;
}

void KeyRepeat::update(const Keyboard& keyboard, uint32_t now) {
    for (int k = 0; k < kKeys; ++k) {
        if (answered_[k]) last_[k] = now;
        answered_[k] = 0;
        if (keyboard.isUp(k)) last_[k] = 0x7fffffff;
    }
}

}  // namespace nyan::diesel
