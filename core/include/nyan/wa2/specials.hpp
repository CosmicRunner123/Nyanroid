#pragma once

#include <array>

namespace nyan::wa2 {

class Game;

// Specials' Voice Message (WA2.exe 0x42a520): the five heroines side by side (0x4c21a8); a click shows
// her picture (sys_072NN) and plays her message (0x4062c0); its end, Return or a right click comes
// back to the five.
class VoiceMessage {
public:
    explicit VoiceMessage(Game& game);
    // Once per frame with the caller's objects, slots, priority and hit-area group (the title passes
    // 0x4a4, 10, 0x1050, 3): 0 while open, 1 when closed.
    int step(int obj, int slot, int prio, int group);

private:
    Game& game_;
    int state_ = 0;     // 0xa72f30
    int counter_ = 0;   // 0xa72f2c
    int closed_ = 0;    // 0xa72f28
    int playing_ = -1;  // 0x4c2208
};

// Specials' CG Mode (WA2.exe 0x42b300): fourteen pages of twelve CGs (0x4c01b0; pages 0..2 the
// introductory chapter's, shown once flag 0 is set with it bundled; 9..13 coda's, once flag 5 is),
// thumbnails of the seen ones (`tv%06d`, else a placeholder by part); a click zooms into the CG and
// further clicks go through its seen variants; oversized pictures scroll at the screen's edges.
class CgMode {
public:
    explicit CgMode(Game& game);
    // As VoiceMessage::step (the title passes 0x4a4, 10, 0x1050, 3).
    int step(int obj, int slot, int prio, int group);

private:
    void thumbnails(int obj, int slot, int prio, int group, bool visible);
    bool seen(int entry) const;                 // 0x428070
    int firstSeen(int entry) const;             // 0x428150
    int seenVariant(int entry, int k) const;    // 0x4281a0: the k-th seen variant, -1 if none

    Game& game_;
    int state_ = 0;       // 0xa72f60
    int counter_ = 0;     // 0xa72f50
    int viewCounter_ = 0; // 0xa72f58
    int page_ = 0;        // 0xa72f64
    int prevPage_ = 0;    // 0xa72f4c
    int slide_ = 0;       // 0xa72f48
    int selected_ = 0;    // 0xa72f40: page * 12 + item
    int variant_ = 0;     // 0xa72f44: the next variant to show
    int buffer_ = 0;      // 0xa72f5c: which of the two view objects is current
    int scrollX_ = 0, scrollY_ = 0;  // 0xa72f38 / 0xa72f34
    int closed_ = 0;      // 0xa72f54
    int lastHover_ = -1;  // 0x4c220c
};

// Specials' Scene Replay (WA2.exe 0x42d910): two pages of twelve scenes (0x4c1bf4: the game flag that
// unlocks each, its CG and its part), the CG's thumbnail for an unlocked one, else a placeholder by
// part. A click sets the replay number (0x4be048, 1..24) and closes into script 9999, which loads the
// scene; the scene's GoTitle comes back to this menu.
class SceneReplay {
public:
    explicit SceneReplay(Game& game);
    // As VoiceMessage::step (the title passes 0x4a4, 10, 0x1050, 3); 5 when a scene was picked (the
    // title starts script 9999).
    int step(int obj, int slot, int prio, int group);

private:
    void items(int obj, int slot, int prio, int group, bool visible);
    bool unlocked(int entry) const;  // the flag set, and the introductory chapter there for part 0

    Game& game_;
    int state_ = 0;       // 0xa72f78
    int counter_ = 0;     // 0xa72f6c
    int page_ = 0;        // 0xa72f74
    int slide_ = 0;       // 0xa72f68
    int closed_ = 0;      // 0xa72f70
    int lastHover_ = -1;  // 0x4c2210
};

}  // namespace nyan::wa2
