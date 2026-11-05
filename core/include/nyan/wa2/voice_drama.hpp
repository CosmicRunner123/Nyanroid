#pragma once

namespace nyan::wa2 {

class Game;

// Special Contents' voice drama player (WA2_Special.exe 0x429770, the music room reworked): the
// drama's picture (VD%02d) and title (VD_Title) beside a list of its parts (0x4bd208: up to seven
// rows of sys_e4101); a click plays the part (BGM 80 + the drama, the part's letter in the file name:
// BGM_080_c for part 2; cut in when none plays, else crossfaded over 60 frames), a click on the
// playing one stops it; Back or a right click returns to the dramas' menu with the title's
// background (Te000) again.
class VoiceDrama {
public:
    explicit VoiceDrama(Game& game);
    void select(int drama) { drama_ = drama; }  // 0xa6fde8, set by the title
    // Once per frame with the caller's objects, slots, priority and hit-area group (the title passes
    // 0x4a4, 10, 0x1050, 3): 0 while open, 1 when closed.
    int step(int obj, int slot, int prio, int group);

private:
    int part(int i) const;  // the drama's part i (its row in sys_e4101), -1 past the last

    Game& game_;
    int drama_ = 0;
    int state_ = 0;         // 0xa6fe74
    int counter_ = 0;       // 0xa6fe6c
    int closed_ = 0;        // 0xa6fe68
    int playing_ = -1;      // 0x4bf2d0: the playing part
    int playingPage_ = -1;  // 0x4bf32c
    int lastHover_ = -1;    // 0x4bf328
};

}  // namespace nyan::wa2
