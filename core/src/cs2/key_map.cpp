#include "nyan/cs2/key_map.hpp"

#include <cstdio>

#include "nyan/common/xml.hpp"

namespace nyan::cs2 {

KeyMap::KeyMap() {
    auto set = [&](Action a, std::vector<int> keys, std::vector<int> mice = {}) {
        actions_[a] = {std::move(keys), std::move(mice)};
    };
    set(Confirm, {13}, {LeftButton});
    set(Cancel, {27}, {RightButton});
    set(AdvanceText, {13, 34}, {LeftButton, WheelDown});
    set(HideWindow, {32, 27}, {0, RightButton});
    set(CursorUp, {38, 104});
    set(CursorDown, {40, 98});
    set(CursorLeft, {37, 100});
    set(CursorRight, {39, 102});
    set(ScrollUp, {38, 104}, {WheelUp});
    set(ScrollDown, {40, 98}, {WheelDown});
    set(PageUp, {33}, {WheelUp});
    set(PageDown, {34}, {WheelDown});
    set(ForceSkip, {17});
    set(SkipMode, {114});
    set(AutoMode, {115, 90});
    set(MessageLog, {119, 9, 33}, {WheelUp});
    set(NextOption, {118});
    set(PrevOption, {117});
    set(Options, {27}, {RightButton});
    set(QuickSave, {112});
    set(QuickLoad, {113});
    set(OptionsInGame, {27}, {RightButton});
    set(AutoSpeedUp, {115, 90});
    set(AutoSpeedDown, {116, 88});
    set(ScreenMode, {122});
    set(Minimize, {123});
    set(Quit, {120});
}

KeyMap KeyMap::parse(const XmlNode* doc) {
    KeyMap m;
    const XmlNode* table = doc ? doc->child("KEYCUSTOMIZE") : nullptr;
    if (!table) return m;
    for (int a = 0; a < Count; ++a) {
        char name[8];
        std::snprintf(name, sizeof name, "key%02d", a);
        const XmlNode* k = table->child(name);
        if (!k) continue;
        Binding b;
        // Slots keyboard1..N / mouse1..N; 0 marks an empty slot (kept so priorities stay positional).
        for (int i = 1;; ++i) {
            auto v = k->value("keyboard" + std::to_string(i));
            if (!v) break;
            b.keys.push_back(std::atoi(v->c_str()));
        }
        for (int i = 1;; ++i) {
            auto v = k->value("mouse" + std::to_string(i));
            if (!v) break;
            b.mice.push_back(std::atoi(v->c_str()));
        }
        m.actions_[a] = std::move(b);
    }
    return m;
}

namespace {
int slotIn(const std::vector<int>& codes, int code) {
    if (code == 0) return 0;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (codes[i] == code) return static_cast<int>(i) + 1;
    }
    return 0;
}
}  // namespace

int KeyMap::keySlot(Action a, int vk) const {
    return slotIn(actions_[a].keys, vk);
}

int KeyMap::mouseSlot(Action a, int code) const {
    return slotIn(actions_[a].mice, code);
}

int KeyMap::pick(const std::vector<Action>& candidates, int code, bool mouse) const {
    int best = -1, bestSlot = 0;
    for (Action a : candidates) {
        int s = mouse ? mouseSlot(a, code) : keySlot(a, code);
        if (s > 0 && (best < 0 || s < bestSlot)) {
            best = a;
            bestSlot = s;
        }
    }
    return best;
}

}  // namespace nyan::cs2
