#pragma once

namespace nyan::wa2 {

class Game;

// Music Mode (WA2.exe 0x428250, from the title's Specials): two pages of 31 tracks in the music
// room's order (0x4c1dd0; without the introductory chapter the first page follows 0x4c1ed0), each
// shown once heard (game flag track + 100); track 21's entry plays its other version (35) once flag
// 135 is set. A click plays a track (cut in when none plays, else crossfaded over 60 frames), a click
// on the playing one stops it; the strip at the bottom names it. Arrows and the wheel turn the page.
class MusicRoom {
public:
    explicit MusicRoom(Game& game);
    // Once per frame with the caller's objects, slots, priority and hit-area group (the title passes
    // 0x4a4, 10, 0x1050, 3): 0 while open, 1 when closed.
    int step(int obj, int slot, int prio, int group);

private:
    // The track of item i on the current page.
    int track(int i) const;
    bool heard(int i) const;
    int titleRow(int i) const;  // the item's row in sys_04101
    void pageItems(int obj, int slot, int prio, int group, bool visible);

    Game& game_;
    int state_ = 0;      // 0xa72f24
    int counter_ = 0;    // 0xa72f1c
    int page_ = 0;       // 0xa72f20
    int slide_ = 0;      // 0xa72f14
    int closed_ = 0;     // 0xa72f18
    int playing_ = -1;   // 0x4c2200: the playing item
    int playingPage_ = -1;  // 0x4c21fc
};

}  // namespace nyan::wa2
