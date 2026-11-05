#pragma once

#include <array>
#include <string>

namespace nyan::wa2 {

class Game;

// The title screen (WA2.exe 0x425850, the engine's state 2): the logo movie and, once story movies
// have been seen, an opening movie; the background scrolling in, the logo and the menu rows fading in
// with snow (T0000..T0201 from grp.pak, the patch's English rows from en.pak); the menu and its three
// sub-menus, the chapters, Specials and the Digital Novels. A chosen chapter starts its script.
class Title {
public:
    explicit Title(Game& game);

    // One logic frame: -1 while the title runs, else the engine state that follows (3 the story, which
    // the title has started unless a save was loaded; 1 quit).
    int step();
    // The state (0xa72f10): 0..3 movies, 4 set-up, 6 / 7 fade-ins, 0x10 the menu, 0x11 Specials,
    // 0x12 the chapters, 0x13 the Digital Novels, 8 / 9 fade-outs, 5 the end, 10..15 the menus the
    // title opens (load, options, CG, scene replay, music, voice message).
    int state() const { return state_; }
    // The script a chapter starts (0xa3a158 as the title writes it).
    const std::string& script() const { return script_; }

    // The menus the title opens. Each returns 0 while it runs, 1 to come back to the title, more for
    // what it starts (the load menu: a save was loaded; the scene replay: the engine state + 2).
    int loadMenu();     // 0x41a6d0 (opened by 0x41a5e0)
    int optionsMenu();  // 0x41f800 (opened by 0x419140); 2: to the load menu
    int cgMenu();       // 0x42b300
    int replayMenu();   // 0x42d910
    int musicMenu();    // 0x428250
    int voiceMenu();    // 0x42a520

private:
    // The menu rows' objects: label 0xb + i (T0100), its plate 0x15 + i and highlight 0x1f + i (T0101);
    // the header 0x29 (T0201) and the hovered row's description 0x2a (T0200).
    static constexpr int kLabel = 0xb, kPlate = 0x15, kHighlight = 0x1f, kHeader = 0x29, kNote = 0x2a;

    void setUp();                            // state 4
    void specialsAreas();                    // the Specials rows' hit areas (group 1)
    void chapterAreas();                     // the chapters' (group 1)
    void showRows(const int* rows, int group, int headerV);  // a sub-menu's frame (0x11 / 0x13 / 0x12 B)
    void hover(int row, const int* rows, int labelObj, int highlightObj);
    void back(int toState, int group, int fromState);   // a sub-menu's Back
    void start(const char* script);          // a chapter: fade out, then the story
    bool cpuFast() const;                    // 0xb9191c >= 1700 MHz
    void startWeather(int fastFlags, int slowFlags);

    // Special Contents' title (WA2_Special.exe 0x424cb0; title_special.cpp): the states it changed.
    // Returns false for a state it leaves as WA2's (0, 1, 3, 5, 6, 8).
    bool stepSpecial(int& result);
    void setUpSpecial();                     // state 4
    void specialMenuObjects();               // the main menu's eight rows and their hit areas (group 0)
    void specialNovelObjects();              // the Digital Novels' two columns
    void specialNovelAreas();                // their hit areas (group 2)
    void specialAreas(const int* widths);    // a sub-menu's eight rows' hit areas (group 2)
    void specialRows(const int* rows, int headerV);  // a sub-menu's frame (0x10, 0x14, 0x16)
    void specialHover(int labelObj, int row, int highlightObj);
    void specialBack(int fromState);         // Return / a right click: to the main menu

    Game& game_;
    int state_ = 0;        // 0xa72f10
    int count_ = 0;        // 0xa72f0c
    float scroll_ = 0;     // 0xa72f00: the background's v, 0..136
    int result_ = 3;       // 0x4c0080: the state the title ends in
    int lastHover_ = -1;   // 0x4c01a8
    int backState_ = -1;   // 0x4c01ac: the menu state a sub-menu was opened from
    bool loaded_ = false;  // 0xa72e9c: the load menu loaded a save
    std::array<int, 8> hoverCount_{};  // 0xa72ea0 (Special Contents: eight rows)
    std::string script_;
};

}  // namespace nyan::wa2
