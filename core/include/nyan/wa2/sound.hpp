#pragma once

#include <array>
#include <functional>

#include "nyan/wa2/save.hpp"

#include "nyan/wa2/sound_out.hpp"

namespace nyan::wa2 {

class Game;

// The game's sound: BGM, sound-effect and voice channels with their frame-based fades, as WA2.exe's
// game code keeps them (BGM 0x40fa50.., SE 0x4064a0 / 0x40f2d0.., voices 0x40ecc0.., per-frame update
// 0x40fb80 then 0x40f630). Levels go to SoundOut once per frame.
class Sound {
public:
    Sound(Game& game, SoundOut& out);

    // BGM (M, M2, MS, MV, MW, MLW).
    void playBgm(int num, int fade, int loop, int vol, int force, int variant);  // 0x40fa50
    void stopBgm(int fade);                                                       // MS, 0x406d50
    void bgmVolume(int vol, int fade);                                            // MV, 0x406de0
    bool bgmSteady() const;    // MW's test (0x406da0): the current channel's state is 0 or 1
    bool bgmStopped() const;   // MLW's test (0x406d80): its state is 0

    // Sound effects (SE, SEP, SES, SEV, SEW, SEVW).
    void se(int num, int vol);                                            // 0x4064a0
    void sep(int ch, int num, int fade, int loop, int vol, int mode);     // 0x40f2d0
    void ses(int ch, int fade);                                           // 0x40f460
    void sev(int ch, int vol, int fade);                                  // 0x4067f0
    bool sewWaiting(int ch, int mode);                                    // 0x40f4c0
    bool sevwSame(int ch) const;                                          // 0x4068d0: cur == target
    void stopAllSe();                                                     // 0x4137f0
    int bgmTrack() const { return bgmTrack_; }                            // 0x4be52c
    bool charVoiceOff(int chara) const;                                   // 0x4063d0 / 0x406460
    // The menus' sounds, loaded once at startup (0x405e10: SE 9211..9214, 9015): 0 the hover
    // (0x405af0), 1 a decision (0x405b90), 2 a cancel (0x405c30), 3, 4.
    enum SystemSe { Hover = 0, Decide = 1, Cancel = 2, Buzz = 3, Page = 4 };
    void systemSe(int which);

    // Voices (VI, VV, VX, VW, VS, VA, VXV).
    void setScene(int scene);  // VI, 0x406190
    void addOffset(int n);     // VA, 0x406170
    void playVoice(int ch, int chara, int scene, int num, int vol, int loop, int noRecord);  // 0x40ecc0
    bool voiceDone(int ch, int mode, int force) const;   // VW, 0x40ec40
    void stopVoice(int ch, int fade);                     // VS, 0x40eb80
    void fadeVoice(int ch, int frames, int target);       // VXV, 0x4061b0
    void restoreDucked();                                 // 0x40eb50 (page advance)
    // 0x4062c0: Voice Message's line for a heroine, on channel 0 whatever the voice settings.
    void playVoiceMessage(int chara, int vol);
    int sceneOfVi() const { return viScene_; }
    int numberOffset() const { return vaOffset_; }
    bool eroMode = false;      // EroMode (0x4d2d20)
    int currentSpeaker = 0;    // 0x4d313c: the character a voice of character -1 takes
    struct VoiceRecord {
        int scene = 0, num = 0, chara = 0, vol = 0;
    } lastVoice;               // 0x4033c0: attached to the backlog page
    int voiceSpeaker = -1;     // 0x4be344: the character of the last recorded voice (100: a group name)
    // Special Contents (0x40e700): the engine's clock when a recorded voice last started on channel 0
    // (0xa36160), which VWait measures from.
    uint32_t voiceStartTime = 0;
    // Special Contents (0xa6fde4): the voice drama part the player chose; BGM 80 and up open
    // BGM_nnn_<a + part>.ogg (0x460bb0).
    int dramaPart = 0;

    // Return to title (0x413ac0): MS(0), effects and voices stopped.
    void stopAll();
    // The options' switches: `all` is the all-sound switch's call (0x40faa0 BGM, 0x40f510 SE, 0x40eb10
    // voices); otherwise the type's own switch, which stores the setting.
    void switchBgm(bool on, bool all);
    void switchSe(bool on, bool all);
    void switchVoice(bool on, bool all);
    // Saves: the current BGM, the looping SEP channels and voices (0x4234c0); the restore starts them
    // again (0x423d10: BGM with fade -2, SEPs loaded then started, voices looped and unrecorded).
    void capture(SaveRecord& r) const;
    void restore(const SaveRecord& r);
    // One logic frame: BGM channels, then voices, one-shot effects and SEP channels.
    void update();

    // The patch's d3d9.dll hooks: called after the SE play core (0x405940) and the voice play
    // (0x40ecc0) whether or not a sound plays.
    std::function<void(int key, int type)> onTrigger;

private:
    struct BgmChannel {
        int track = -1, handle = -1, state = 0, c = 0, d = 0, loop = 0, cur = 0, start = 0, target = 0;
    };
    struct SepSlot {
        bool inUse = false, stopAfter = false, preloaded = false;
        int num = 0, handle = -1, c = 0, d = 0, loop = 0, cur = 0, start = 0, target = 0;
    };
    struct VoiceChannel {
        int handle = -1, chara = 0, scene = 0, num = 0, cur = 0, loop = 0, c = 0, d = 0, start = 0, target = 0, base = 0;
        bool stopAfter = false;
    };

    int fadeFrames(int n) const;       // 0x40aed0
    int voiceFrames(int n) const;      // 0x40b100
    bool skipping() const;
    int level(int cur, int typeVol) const;
    void bgmPlay(int num, int fade, int loop, int vol, int force, int always, int variant);  // 0x40f970
    void bgmStart(int ch, int num, int fade, int loop, int vol, int force, int variant);     // 0x4068f0
    void bgmFadeOut(int ch, int fade);                                                       // 0x406b90
    int playSe(int num, int vol, int loop);                                                  // 0x405940
    int loadSe(int num, int loop, int vol, int mode);                                        // 0x406620
    void freeSound(int& handle);
    void clearVoice(int ch);
    Game& game_;
    SoundOut& out_;
    std::array<BgmChannel, 4> bgm_;
    int bgmCur_ = 0;          // 0xa391f8
    int bgmTrack_ = -1;       // 0x4be52c
    int bgmVolume_ = 255;     // 0x4bf634
    int bgmLoop_ = 1;         // 0x4bf6e8
    std::array<int, 8> oneShots_;  // 0x4bf6ec
    std::array<int, 5> systemSe_;  // 0x4bf70c
    std::array<SepSlot, 16> sep_;  // 0x5e7360
    std::array<VoiceChannel, 4> voice_;  // 0xa390c8
    int viScene_ = -1;        // 0x4be4cc
    int vaOffset_ = 0;        // 0xa391ec
};

}  // namespace nyan::wa2
