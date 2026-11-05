#pragma once

#include <array>
#include <string>
#include <vector>

namespace nyan {
struct XmlNode;
}

namespace nyan::cs2 {

// The engine's input bindings (startup.xml <KEYCUSTOMIZE>): each engine action (key00..key28)
// is bound to Windows virtual-key codes and engine mouse codes, in priority slots.
class KeyMap {
public:
    // Engine action numbers (the keyNN index).
    enum Action : int {
        Confirm = 0,
        Cancel = 1,
        AdvanceText = 2,
        HideWindow = 3,
        CursorUp = 4,
        CursorDown = 5,
        CursorLeft = 6,
        CursorRight = 7,
        ScrollUp = 8,
        ScrollDown = 9,
        PageUp = 10,
        PageDown = 11,
        ForceSkip = 12,
        SkipMode = 13,
        AutoMode = 14,
        MessageLog = 15,
        NextOption = 16,
        PrevOption = 17,
        ReplayVoice = 18,
        Options = 19,
        QuickSave = 21,
        QuickLoad = 22,
        OptionsInGame = 23,
        AutoSpeedUp = 24,
        AutoSpeedDown = 25,
        ScreenMode = 26,
        Minimize = 27,
        Quit = 28,
        Count = 29,
    };
    // Engine mouse codes.
    enum Mouse : int { LeftButton = 1, RightButton = 3, WheelUp = 6, WheelDown = 7 };

    // The engine's default table (as shipped by the Grisaia games) if the game has none.
    KeyMap();
    // Reads <KEYCUSTOMIZE> from startup.xml's <document>; keeps the defaults if it is missing.
    static KeyMap parse(const XmlNode* document);

    // Priority slot (1 = first binding) of a key/mouse code for an action, or 0 if unbound.
    int keySlot(Action a, int vk) const;
    int mouseSlot(Action a, int code) const;

    // Of `candidates`, the action this input triggers: lowest slot wins, then list order. -1 if none.
    int pick(const std::vector<Action>& candidates, int code, bool mouse) const;

private:
    struct Binding {
        std::vector<int> keys, mice;
    };
    std::array<Binding, Count> actions_;
};

}  // namespace nyan::cs2
