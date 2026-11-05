#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nyan::wa2 {

class Game;

// The save and load screens (WA2.exe 0x41a6d0, opened by 0x41a5e0) and the quick save / load notice
// (0x41ee10, opened by 0x41edf0), which share their state (0xa3a438..): ten pages of ten slots with
// the scene's thumbnail, the chapter and date the save was made at, the time and the message's start;
// the quick slot (load only); a confirmation when the "confirm" setting is on; a notice when done.
class SaveMenu {
public:
    explicit SaveMenu(Game& game);

    // 0x41a5e0: mode 0 save, 1 load (from the story), 2 load (from the title), 10 the quick save (only
    // the thumbnail). `thumbnail`: draw the scene into the 128 x 128 target for the save's picture.
    void open(int mode, bool thumbnail);
    // 0x41edf0: the quick notice; mode 0 "saved", 1 the quick load.
    void openQuick(int mode);
    // 0x41a6d0 / 0x41ee10, once per frame: 0 while open (or closed in the story), 1 back to the title,
    // 2 a save was loaded from the title (the story then starts by loading slot()).
    int step();
    int stepQuick();

    int mode() const { return mode_; }
    bool thumbnailPending() const { return thumbPending_; }  // 0xa3a454
    // 0x4391f0: the target's texels into the game's thumbnail; the object and the target released.
    void takeThumbnail();
    int slot() const { return selected_; }         // 0xa3a450
    int folder() const { return loadFolder_; }     // 0xa3a494

private:
    // Objects: 0x4a4 top bar, 0x4a5 bottom bar, 0x4a7 the chapter art, 0x4a8 the shade, 0x4a9 the
    // cursor, 0x4aa "new", 0x4ab / 0x4ac page arrows, 0x4ad Back, slot i: plate 0x4ae, thumbnail 0x4b8,
    // chapter 0x4c2, number 0x4cc, text 0x4d6, month / route 0x508, day 0x512; tabs 0x4e0..; the prompt
    // 0x4ea..0x4f1; the quick slot 0x4f4..0x4f9; the notice 0x500 / 0x501.
    void setUp();                 // the default state
    void fillSlot(int i, bool setup);
    void fadeChrome(float t, bool finished);  // states 2 and 3: bars, art, slots
    void slidePage(float t, bool out);         // states 5 and 6
    void promptAlpha(float t);
    void close();                              // state 1
    int finishFrame(int result, int hover);
    std::string slotLabel(int n) const;        // 0x423470

    Game& game_;
    int state_ = 0;            // 0xa3a498
    int mode_ = 0;             // 0xa3a43c
    std::array<int, 2> page_{};      // 0xa3a448
    int selected_ = 0;         // 0xa3a450
    bool thumbPending_ = false;  // 0xa3a454
    bool loaded_ = false;      // 0xa3a458
    bool saved_ = false;       // 0xa3a45c
    std::array<int, 2> latest_{-1, -1};  // 0xa3a488
    int loadFolder_ = 0;       // 0xa3a494
    int folder_ = 0;           // 0xa3a4a8
    std::array<int, 10> keys_{};  // 0xa3a4ac: the shown slots' sort keys
    int counter_ = 0;          // 0xa3a4d4
    int quickCounter_ = 0;     // 0xa3a4d8
    int lastHover_ = -1;       // 0x4c006c
    int slide_ = 0;            // 0x4c0070: the page slide's direction
    int quickLastHover_ = -1;  // 0x4c0074
    // The header last read (0xa4e5c0's time, 0xa4e5d0's text), as the slot labels use it.
    std::array<uint16_t, 8> time_{};
};

}  // namespace nyan::wa2
