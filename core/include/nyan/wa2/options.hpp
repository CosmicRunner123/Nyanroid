#pragma once

namespace nyan::wa2 {

class Game;

// The options screen (WA2.exe 0x41f800, opened by 0x419140; pages built by 0x419170, defaults
// 0x41a160): three pages of settings, the ini's SYSTEM keys. Page 0: window / full screen (shown,
// the platform decides), message speed, skip unread text, auto speed, effect speed, the window's four
// opacities, the window art. Page 1: confirmations, the right click, the four sound switches and
// volumes with a test sound and voice, the page voice, the heroine-voice option. Page 2: the ten
// characters' voices. Tabs, arrows and the wheel turn pages; Restore Defaults asks first.
class Options {
public:
    explicit Options(Game& game);

    // 0x419140: `fromTitle` 0 over the story (state 5), 1 from the title.
    void open(int fromTitle);
    // 0x41f800, once per frame: 0 while open; 1 closed (2 would open the load screen; unused here).
    int step();

private:
    void build();                      // 0x419170: the page's cells, sliders and marks
    void setDefaults();                // 0x41a160
    void fade(float t, bool opening);  // states 2 / 3
    void pageZoom(float t, float zoom);  // states 5 / 6
    void close();                      // state 1
    void applyWindow();                // the window's opacities after a change
    bool hoverChoice(int row, int col, int w, int h, bool current);  // the highlight on a cell

    Game& game_;
    int fromTitle_ = 0;   // 0x41f800's argument
    int state_ = 0;       // 0xa3a498
    int page_ = 0;        // 0xa3a4a4
    int counter_ = 0;     // 0xa3a4dc
    int lastHover_ = -1;  // 0x4c0078
};

}  // namespace nyan::wa2
