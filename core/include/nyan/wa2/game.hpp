#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nyan/wa2/ani.hpp"
#include "nyan/wa2/assets.hpp"
#include "nyan/wa2/effects.hpp"
#include "nyan/wa2/gfx.hpp"
#include "nyan/wa2/message.hpp"
#include "nyan/wa2/music_room.hpp"
#include "nyan/wa2/specials.hpp"
#include "nyan/wa2/voice_drama.hpp"
#include "nyan/wa2/options.hpp"
#include "nyan/wa2/scene.hpp"
#include "nyan/wa2/input.hpp"
#include "nyan/wa2/save.hpp"
#include "nyan/wa2/save_menu.hpp"
#include "nyan/wa2/script_vm.hpp"
#include "nyan/wa2/sound.hpp"
#include "nyan/wa2/subtitles.hpp"
#include "nyan/wa2/system_data.hpp"
#include "nyan/wa2/title.hpp"

namespace nyan::audio {
class Mixer;
}

namespace nyan::wa2 {

// The C runtime's rand() (MSVC: a linear congruential generator), which the engine seeds with the
// time at startup (WA2.exe 0x432840 -> srand(timeGetTime())).
class CrtRand {
public:
    void seed(uint32_t s) { state_ = s; }
    int next() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7fff);
    }

private:
    uint32_t state_ = 1;
};

// A running White Album 2: its files, the script VM and the engine state the scripts change. Steps at
// the engine's logic rate, 60 per second (WA2.exe 0x418c00 runs 0x418560 every 1000/60 ms).
class Game : public ScriptHost {
public:
    static constexpr int kTicksPerSecond = 60;

    // Without a mixer the sounds run silently on the game's clock.
    explicit Game(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer = nullptr);
    ~Game() override;

    Assets& assets() { return assets_; }
    // The engine's input, polled at the start of each logic frame.
    Input input;
    // What the input means to the engine this frame (0x4181c0 -> 0xa39770..0xa397d8).
    struct Keys {
        bool advance = false;        // 0xa39770: Enter or the left button pressed
        bool advanceRepeat = false;  // 0xa39774
        bool cancel = false;         // 0xa39778: Escape, Backspace or the right button pressed
        bool forceSkip = false;      // 0xa39780: Ctrl or the middle button held
        bool shift = false;          // 0xa39784
        bool space = false;          // 0xa39788
        bool up = false, down = false, left = false, right = false;  // 0xa3978c..: arrows (repeat)
        bool end = false;            // 0xa3979c
        bool home = false;           // 0xa397a0
        bool scrollUp = false;       // 0xa397a4: wheel up or Page Up (repeat)
        bool scrollDown = false;     // 0xa397a8: wheel down or Page Down (repeat)
        std::array<bool, 10> digits{};  // 0xa397ac..: number keys, either row
    } keys;
    // 0x4181c0: the keys from this frame's poll.
    void updateKeys();

    // The engine's state (0x418560's switch on 0xa3a364): Boot loads the system pictures and goes to
    // the title, Title runs the title screen, Story the scripts; Quit asks the platform to close.
    enum class Mode { Boot = 0, Quit = 1, Title = 2, Story = 3 };
    Mode mode() const { return mode_; }
    // For tests and tools: Quit leaves a frame with only the input, the animations and the sound.
    void setMode(Mode m) { mode_ = m; }
    // Starts the story at a script and label (0x4178b0: globals from Global.vrb, the VM, the label).
    void startStory(const std::string& script, int label = 0);
    // 0x413ac0 (GoTitle, the window menu's Return To Title): the story ends, the title follows two
    // frames later.
    // A system message as the build's exe writes it: the English patch's `<F24text>`, Special
    // Contents' `<F20text>` (0x41e140, 0x419c40, 0x41eb90).
    std::string systemMessage(const char* text) {
        return std::string(assets_.build() == Build::Special ? "<F20" : "<F24") + text + ">";
    }
    void goTitle();
    // 0x413ac0 (Special Contents: 0x4134d0, after its fade): the story's things released.
    void goTitleCleanup();
    // Whether Return To Title may be used: the story runs and no menu or movie is open.
    bool canGoTitle() const;
    // One logic frame (0x418560: the input, the .ani tick, the state's work, the sound).
    void tick();
    uint64_t frame() const { return frame_; }
    // The engine's millisecond clock (timeGetTime), from the frame count.
    uint32_t milliseconds() const { return static_cast<uint32_t>(frame_ * 1000 / kTicksPerSecond); }
    bool storyRunning() const { return vm_ && vm_->state() == ScriptVm::Running; }
    // GoTitle ran: the story is over and the engine shows its title.
    bool titleRequested() const { return titleRequested_; }
    const ScriptVm* vm() const { return vm_.get(); }
    // The innermost running script's name (0x417390).
    std::string currentScript() const;

    // The engine's settings (the ini's SYSTEM section, read at startup 0x432840; defaults as there and
    // in the config's defaults button 0x41a160). Sound keys from WA2.exe's sound code.
    struct Settings {
        int allSound = 1;  // all_sound 0x4be01c
        int bgm = 1;       // 0x4be020
        int se = 1;        // 0x4be024
        int voice = 1;     // 0x4be028
        int allVol = 195;  // all_vol 0x4be02c, 0..256
        int bgmVol = 134;  // bgm_vol 0x4be030
        int seVol = 195;   // se_vol 0x4be034
        int voiceVol = 195;  // voice_vol 0x4be038
        std::array<int, 10> charVoice{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // char_voice0..9 0x566330
        int eroVoice = 0;  // ero_voice 0x4be068
        int pageVoice = 0;  // page_voice 0x4be0a0
        int movieLevel = 2;  // mov_lv 0x4be04c
        float speed = 1.0f;  // wait / 2 (0x4be004): scales BGM and SE fades
        int confirm = 1;     // yes_no 0x4be090: ask before a slot is saved over or loaded
        int windowMode = 0;  // window_mode 0xb343fc: 0 full screen (the platform's choice here)
        int wheel = 0;       // wheel 0x4be008
    } settings;

    // Reading modes and input as the engine keeps them.
    struct Modes {
        bool skip = false;          // 0x4be018: skip mode (menu, Ctrl+...)
        bool forceSkip = false;     // 0xa39780: the force-skip key held
        bool skipUnread = false;    // config msg_cut_optin bit 0 (0x4be00c)
        int demo = 0;               // 0x4be03c: SetDemoMode
        int frameOverride = 0;      // 0x4c2240
        int replay = 0;             // 0x4be048: scene replay from the gallery (non-zero: number)
        int demoArg = 0;            // 0x4be044: SetDemoMode's second argument
        bool autoMode = false;      // 0x4be060
    } modes;
    // Skip as the scripts see it (GetSkip, 0x40ad50; GetSkip2, 0x40adb0).
    bool skipActive();
    bool skipActive2();
    // Whether the current message counts as read (0x405250): 1 read (or a replay), 2 IfSkip set, 0 not.
    int currentTextRead();
    // Durations in frames (-1 = 15, -2 = 30): fr (0x40aed0) scaled by the effect speed, 0 while
    // skipping, plain in demo mode; frT (0x40b100) unscaled, 0 while skipping; frP (0x401260) plain.
    int fr(int n);
    int frT(int n);
    static int frP(int n);

    // Diagnostics for the headless runs.
    std::function<void(const std::string&)> logSink;
    const std::map<std::string, size_t>& unhandled() const { return unhandled_; }
    const std::map<std::string, size_t>& missing() const { return missing_; }
    void noteMissing(const std::string& what) { ++missing_[what]; }
    // The headless runner's hooks: a click answers what waits for one (filled in by the systems).
    std::function<int(const std::vector<std::string>&)> choose;  // a choice: the option to pick

    // ScriptHost
    std::optional<Bytes> readScriptFile(const std::string& group, const std::string& name) override;
    const FunctionTable& functions() const override { return functions_; }
    std::vector<GlobalVar>& globals() override { return globals_; }
    int callNative(ScriptVm& vm, int id, CallContext& ctx) override;
    void log(const std::string& message) override;

    // BGM, sound effects and voices.
    Sound& sound() { return *sound_; }
    // The image slots and sprite objects.
    Gfx& gfx() { return gfx_; }
    // Backgrounds, characters, tone.
    Scene& scene() { return scene_; }
    // The .ani animations.
    AniPlayer& ani() { return ani_; }
    // The message window, names, backlog, choices.
    Message& message() { return message_; }
    // Shakes, weather, free pictures, the date card.
    Effects& effects() { return effects_; }
    // The title screen.
    Title& title() { return title_; }
    // The save and load screens, the quick save / load notice.
    SaveMenu& saveMenu() { return saveMenu_; }
    // The options screen.
    Options& options() { return options_; }
    // Specials' Music Mode.
    MusicRoom& musicRoom() { return musicRoom_; }
    VoiceMessage& voiceMessage() { return voiceMessage_; }
    CgMode& cgMode() { return cgMode_; }
    SceneReplay& sceneReplay() { return sceneReplay_; }
    VoiceDrama& voiceDrama() { return voiceDrama_; }
    // The settings as the ini's SYSTEM keys (settings.ini in the save folder; the engine writes them at
    // exit, Nyanroid when the options close and when the app is suspended).
    void loadSettings();
    void saveSettings();
    // 0x413c10's state (0xa39204): 4 the story, 5 options, 6 save / load, 7 the quick notice; 1 a load at
    // the story's start; 2 / 3 on the way to the title.
    void setStoryPhase(int phase) { storyPhase_ = phase; }
    int storyPhase() const { return storyPhase_; }
    // 0x4509c0: a script global's value by name (0 if there is none).
    int globalInt(const std::string& name) const;
    // 0x428210: a BGM track's place in the music room's order (0x4c1dd0), 64 if it has none.
    static int musicIndex(int track);
    // 0x409e00: the scene is at rest (no transition, tone, endless... shake, moving character or
    // fading choice) and the window waits: the window's buttons and the menu item work only then.
    bool sceneAtRest() const;
    // 0x419140(0): the options screen over the story (state 5).
    void openOptions();

    // Movies (SetMovie, 0x4138a0 / 0x430b30). The game does not tick while one plays.
    struct MovieRequest {
        std::string file;  // in the game folder, e.g. "IC/mv020.pak"
        int id = 0;
        int volume = 0;    // 0..255: linear volume / 256
    };
    // The platform's player: starts `request` and returns true, or false if it cannot play it (the
    // movie then ends at once, as a missing file does). Without a player (nyan-cli) a movie counts
    // as watched at once.
    std::function<bool(const MovieRequest&)> playMovie;
    // 0x4138a0: starts movie `name` (without its quality digit and extension) with its id.
    void startMovie(const std::string& name, int id);
    bool moviePlaying() const { return movieActive_; }
    // A click or a decision key may end it: ids below 200 always, story movies once seen.
    bool movieSkippable() const;
    // 0x430b30: the end (completed or skipped): skip off; story movies are flagged as seen.
    void endMovie();
    // The frame draw's clear request (0x4c221c: set by the background code, reset after each draw).
    bool clearScreen = true;
    // The patch's subtitles (its d3d9.dll), when the install has them.
    Subtitles subtitles;

    // System data (Sys.sav): read text, gallery flags, game flags.
    SystemData system;
    // The current script as the read-text and choice code knows it (0x40e960).
    std::string scriptName;       // 0xa3a158
    int scriptNumber = 0;         // 0xa391e0
    int scriptIndexNow = 0;       // 0xa391e4: its block of read-text bits
    int chapterDigit = 0;         // 0xa391f0: the name's first digit
    int currentLine = -1;         // 0xa391e8: the last SetMessage's line
    int ifSkip = 0;               // 0xa391f4: IfSkip
    // 0x40e960: a script becomes the current one; leaving one marks the line after its last as read.
    void enterScript(const std::string& name);
    // 0x405180: a message line is shown; it counts as read from now on.
    void markLine(int line);
    // Where the saves and the system data live (empty: nothing is kept). Loads the system data.
    void setSaveDirectory(const std::string& dir);
    // Writes the system data if it changed (the engine writes Sys.sav at once on flag changes and
    // every line number that is a multiple of 100).
    void writeSystem();

    // Saves (save.cpp): slot 0..99, or -1 for the quick save. Nyanroid's own files in the save folder
    // (save_NN.nsv, save_Q.nsv); nothing is kept without one.
    std::string savePath(int slot) const;
    // 0x424260: the header, the engine record (0x4234c0) and the scripts. False without a story or a
    // save folder, or if the file cannot be written.
    bool writeSave(int slot);
    // 0x423330: the header only (the thumbnail when asked for).
    std::optional<SaveInfo> readSaveInfo(int slot, bool thumbnail = true) const;
    // 0x424310: the record restored (0x423d10), the scripts resumed where they were.
    bool loadSave(int slot);
    // 0x423060's sort key from the globals and the date card.
    int saveSortKey() const;
    // The scene's thumbnail for the next save (0xa61a90, 128 x 128).
    RgbaImage thumbnail;
    SaveRecord captureRecord() const;
    void restoreRecord(const SaveRecord& rec);

    // Choices (SetSelectMess / SetSelect, 0x404530 / 0x4046a0 / 0x405150).
    struct ChoiceOption {
        std::string text;
        int flag = 0, value = 0, always = 0;
    };
    std::vector<ChoiceOption> choiceOptions;  // up to 8
    std::vector<int> visibleChoices;          // indices of the options shown
    int choiceResult = -1;                    // 0xa39014: -1 until the player picks
    int choicePick = -1;                      // 0xa39018
    bool choiceShown = false;
    int choiceId = 0;                         // SetSelect's argument (0xa390bc)
    int choiceScript = 0;                     // the script's number (0xa390c0)
    // 0x404530 (SetSelectMess): an option added; shown when `always` or game flag `flag` is `value`.
    void addChoice(const std::string& text, int flag, int value, int always);
    // Picks option `option` (an index among all the options), as the click handler 0x413400 does.
    void pickChoice(int option);
    // The game flag holding the "chosen before" bits of the current choice (0xa390c4).
    int choiceFlagIndex() const;
    // Scripts whose choices keep those bits in flags of their own (WA2.exe 0x4be4d0, 23 entries).
    static constexpr std::array<int, 23> chosenTable = {2003, 2004, 2005, 2007, 2009, 2011, 2013, 2014,
                                                        2015, 2016, 2017, 2018, 2019, 2024, 2503, 3003,
                                                        3005, 3009, 3012, 3013, 3014, 3016, 3904};


    // Timers (StartTimer / GetTimer / WaitTimer: 0x401300 / 0x401310 / 0x40b330).
    uint32_t timerStart = 0;
    // Wait / Wait2 (0x40af60): one counter shared by every wait.
    int waitCounter = 0;
    uint32_t waitTarget = 0;
    int runEx = 0;  // 0x4c223c: runEX

    CrtRand rand;

private:
    using Native = int (Game::*)(ScriptVm&, CallContext&);
    void registerNatives();
    void registerNative(const char* name, Native fn);
    void loadGlobals();
    void loadFonts();
    std::optional<Bytes> readSaveFile(int slot) const;
    void boot();        // 0x418560's first frame
    void storyFrame();  // 0x418560 state 3 and 0x413c10
    void optionsFrame();  // 0x41f800(0)
    int unhandledNative(const std::string& name);

    // Flow and system natives (natives_flow.cpp).
    int nRun(ScriptVm&, CallContext&);
    int nPrint(ScriptVm&, CallContext&);
    int nRet(ScriptVm&, CallContext&);
    int nInt(ScriptVm&, CallContext&);
    int nFloat(ScriptVm&, CallContext&);
    int nRand(ScriptVm&, CallContext&);
    int nMath(ScriptVm&, CallContext&);
    int nTimeGetTime(ScriptVm&, CallContext&);
    int nWait(ScriptVm&, CallContext&);
    int nWait2(ScriptVm&, CallContext&);
    int nStartTimer(ScriptVm&, CallContext&);
    int nGetTimer(ScriptVm&, CallContext&);
    int nWaitTimer(ScriptVm&, CallContext&);
    int nGetGameFlag(ScriptVm&, CallContext&);
    int nSetGameFlag(ScriptVm&, CallContext&);
    int nVFlag(ScriptVm&, CallContext&);
    int nHFlag(ScriptVm&, CallContext&);
    int nGetSkip(ScriptVm&, CallContext&);
    int nGetSkip2(ScriptVm&, CallContext&);
    int nIfSkip(ScriptVm&, CallContext&);
    int nSkipOff(ScriptVm&, CallContext&);
    int nRunEx(ScriptVm&, CallContext&);
    int nGetReplayMode(ScriptVm&, CallContext&);
    int nSetDemoMode(ScriptVm&, CallContext&);
    int nSetSelectMess(ScriptVm&, CallContext&);
    int nSetSelect(ScriptVm&, CallContext&);
    int nGoTitle(ScriptVm&, CallContext&);
    // Special Contents' own functions (WA2_Special.exe 0x458590..0x458620).
    int nVWait(ScriptVm&, CallContext&);
    int nDebugBox(ScriptVm&, CallContext&);
    int nGetTrialFlag(ScriptVm&, CallContext&);

    // Picture natives (natives_scene.cpp).
    void registerSceneNatives();
    int cgNumber(int a1, int a2);
    int nB(ScriptVm&, CallContext&);
    int nB2(ScriptVm&, CallContext&);
    int nC(ScriptVm&, CallContext&);
    int nCS(ScriptVm&, CallContext&);
    int nCW(ScriptVm&, CallContext&);
    int nCR(ScriptVm&, CallContext&);
    int nCRW(ScriptVm&, CallContext&);
    int nCM(ScriptVm&, CallContext&);
    int nF(ScriptVm&, CallContext&);
    int nSetTimeMode(ScriptVm&, CallContext&);
    int nSetChromaMode(ScriptVm&, CallContext&);
    int nSetEffctMode(ScriptVm&, CallContext&);
    int nS(ScriptVm&, CallContext&);
    int nZ(ScriptVm&, CallContext&);
    int nR(ScriptVm&, CallContext&);
    int nWSZ(ScriptVm&, CallContext&);
    int nStopSZR(ScriptVm&, CallContext&);

    // Message natives (natives_message.cpp).
    void registerMessageNatives();
    int nSetMessage(ScriptVm&, CallContext&);
    int nSetMessage2(ScriptVm&, CallContext&);
    int nWaitMessage(ScriptVm&, CallContext&);
    int nEndMessage(ScriptVm&, CallContext&);
    int nK(ScriptVm&, CallContext&);
    int nWN(ScriptVm&, CallContext&);
    int nWN2(ScriptVm&, CallContext&);
    int nW(ScriptVm&, CallContext&);
    int nWR(ScriptVm&, CallContext&);
    int nNB(ScriptVm&, CallContext&);
    int nNevelMode(ScriptVm&, CallContext&);
    int nLogOut(ScriptVm&, CallContext&);
    int nGetClick(ScriptVm&, CallContext&);
    int nSetMovie(ScriptVm&, CallContext&);

    // Effect natives (natives_effects.cpp).
    void registerEffectNatives();
    int nSetShake(ScriptVm&, CallContext&);
    int nStopShake(ScriptVm&, CallContext&);
    int nSetWeather(ScriptVm&, CallContext&);
    int nChangeWeather(ScriptVm&, CallContext&);
    int nResetWeather(ScriptVm&, CallContext&);
    int nLoadBmp(ScriptVm&, CallContext&);
    int nLoadBmpAnime(ScriptVm&, CallContext&);
    int nSetBmpAvi(ScriptVm&, CallContext&);
    int nWaitBmpAvi(ScriptVm&, CallContext&);
    int nWaitBmpAnime(ScriptVm&, CallContext&);
    int nReleaseBmp(ScriptVm&, CallContext&);
    int nSetBmp(ScriptVm&, CallContext&);
    int nCalender(ScriptVm&, CallContext&);

    // Sound natives (natives_sound.cpp).
    void registerSoundNatives();
    int nM(ScriptVm&, CallContext&);
    int nMS(ScriptVm&, CallContext&);
    int nMP(ScriptVm&, CallContext&);
    int nMV(ScriptVm&, CallContext&);
    int nMW(ScriptVm&, CallContext&);
    int nMLW(ScriptVm&, CallContext&);
    int nSE(ScriptVm&, CallContext&);
    int nSEP(ScriptVm&, CallContext&);
    int nSES(ScriptVm&, CallContext&);
    int nSEV(ScriptVm&, CallContext&);
    int nSEW(ScriptVm&, CallContext&);
    int nSEVW(ScriptVm&, CallContext&);
    int nVI(ScriptVm&, CallContext&);
    int nVV(ScriptVm&, CallContext&);
    int nVX(ScriptVm&, CallContext&);
    int nVW(ScriptVm&, CallContext&);
    int nVS(ScriptVm&, CallContext&);
    int nVA(ScriptVm&, CallContext&);
    int nVXV(ScriptVm&, CallContext&);
    int nEroMode(ScriptVm&, CallContext&);
    bool waitFrames(uint32_t amount, int milliseconds, int noSkip, int extra);  // 0x40af60

    Assets assets_;
    Gfx gfx_{assets_};
    AniPlayer ani_{gfx_};
    Scene scene_{*this};
    Message message_{*this};
    Effects effects_{*this};
    Title title_{*this};
    SaveMenu saveMenu_{*this};
    Options options_{*this};
    MusicRoom musicRoom_{*this};
    VoiceMessage voiceMessage_{*this};
    CgMode cgMode_{*this};
    SceneReplay sceneReplay_{*this};
    VoiceDrama voiceDrama_{*this};
    std::unique_ptr<SoundOut> soundOut_;
    std::unique_ptr<Sound> sound_;
    FunctionTable functions_;
    std::vector<GlobalVar> globals_;
    std::unique_ptr<ScriptVm> vm_;
    std::array<Native, 0x100> natives_{};
    std::array<std::string, 0x100> nativeNames_;
    uint64_t frame_ = 0;
    Mode mode_ = Mode::Boot;
    int titleFade_ = 0;   // Special Contents (0xa361a8): the fade before the title
    int storyPhase_ = 0;  // 0xa39204: 0x413c10's state (2, 3: going to the title; 4 running)
    bool vmHeld_ = false;  // 0xa3a360: 0x413c10 returned 1 (a menu) last frame: no VM run
    bool titleRequested_ = false;
    bool movieActive_ = false;
    int movieId_ = 0;
    std::string lastScript_;  // 0xa3a260
    std::string saveDir_;
    std::map<std::string, size_t> unhandled_;
    std::map<std::string, size_t> missing_;
};

}  // namespace nyan::wa2
