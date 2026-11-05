#pragma once

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "nyan/wa2/save.hpp"

namespace nyan::wa2 {

class Game;
class Gfx;

// The message window (WA2.exe: open 0x402cd0, close 0x403240, names 0x402890, new / append message
// 0x40e420 / 0x40e5d0, the per-frame update 0x410300 with its states, novel mode 0x403400 / 0x403290,
// the backlog 0x4034c0, the window buttons 0x40a180, choices 0x4046a0 / 0x413400). Text is drawn by
// the text objects of the sprite engine (text records 0 = message, 1 / 2 = names, 0x10.. choices).
class Message {
public:
    explicit Message(Game& game);

    // The ini's window settings (0x432840; defaults there).
    struct Config {
        float speed = 1.0f;          // msg_wait / 2 (0x4be010): 0 instant, 0.5, 1.0, 2.0
        int autoMax = 137;           // auto_max (0x4be05c): frames before auto advances
        int winAlpha = 0xcf;         // win_alpha (0x4be07c)
        int winAlphaVis = 0x86;      // win_alpha_vis (0x4be080): over an event CG
        int winAlphaNovel = 0x86;    // win_alpha_novel (0x4be084)
        int winAlphaCommand = 0x86;  // win_alpha_command (0x4be088): the button bar
        int winDisp = 1;             // win_disp (0x4be08c): draw the window art
        int winColor = 0;            // win_color (0x4be078): sys_N.... digit
        int rightClick = 0;          // 0x4be094: 0 hides the window, 1 options, 2 save
    } config;

    // The window's pictures and page cursors (0x40a3a0, at the scene's start).
    void begin();
    // Window, names, text and backlog cleared (0x4032f0 and the window's state).
    void reset();

    // SetMessage / SetMessageE / SetMessage2's first run: mode 1 a new message, 0 appended after a
    // `\k`; kind 1 continues when shown, 2 waits for a click. Marks `line` read.
    void say(const std::string& text, int line, int mode, int kind);
    bool done() const { return state_ == 4; }  // 0x4037b0
    // 0x40ae10: a click, skip, wheel-down, or the auto / demo counter (p == 0 counts auto).
    bool advanceInput(int p);
    void openWindow(int nameIndex, int style, int fade);  // 0x402cd0 (W, WN)
    void closeWindow(int duration, int style);             // 0x403240 (WR)
    void releaseText();                                    // 0x403780: the text object and its slot
    // WN(name) (0x402cb0 + 0x402cd0): the speaker by name.
    void speak(const std::string& name, int speaker);
    // 0x402890: mode 0 a speaker, 1 / 2 backlog browsing; null clears.
    void setName(const std::string* name, int mode, int speaker);
    void novelMode(int on);                  // 0x403400 (NevelMode)
    void novelFade(int frames, int hidden);  // 0x403290 (NB: hidden 1, NBR / messages: 0)
    bool novelFading() const;                // 0x4032d0
    int logOut = 0;                          // 0xa391dc (LogOut)

    // SetSelect's display (0x4046a0) for the choices the script added.
    void showChoices(int id, int scriptNumber);
    // The options a click can take now (shown, not grey), as indices among all of them.
    std::vector<int> selectableChoices() const;
    // A click on option `option` (0x413400): the backlog gets its text, the "chosen before" bit is
    // set, SetSelect returns it.
    void pick(int option);

    // The story's per-frame work (0x413c10 state 4): buttons (0x40a180), the window (0x410300),
    // choices (0x413400).
    void update();

    // 0x403f80: the window's objects at their places plus the window's share of a shake.
    void placeWindow();

    int state() const { return state_; }
    bool windowExists() const { return exists_; }
    bool novel() const { return novel_ != 0; }

    // The backlog (0x4d3294..): newest first, at most 255.
    struct Voice {
        int scene = -1, num = -1, chara = -1, vol = -1;
    };
    struct LogEntry {
        std::string text, name;
        bool novel = false;
        int speaker = -1;
        std::array<Voice, 10> voices;
    };
    const std::deque<LogEntry>& log() const { return log_; }

    // The message buffer (0x4d2d3c): the last message as given, for the save's excerpt.
    const std::string& text() const { return buffer_; }
    bool choicesShown() const { return choiceShown_; }  // 0xa37ff8
    int choicePhase() const { return choicePhase_; }    // 0xa38004
    void resetAutoCount() { autoCount_ = 0; }           // 0x4be064
    void clearChoices();                                // 0x404d10
    // The window art after the win_disp option changed: removed, or made again unless in novel mode.
    void applyWindowArt();
    void clearBacklog();                                // 0x4032f0
    // Saves: 0x4037c0 keeps the window, the text and the name; 0x40e830 opens the window again at once
    // with the text at its saved page, then clears the backlog.
    void capture(SaveRecord::Message& m) const;
    void restore(const SaveRecord::Message& m);

private:
    int colorIndex(int speaker) const;  // 0x4027a0
    int pushLog(const std::string& text, bool noName);  // 0x4034c0
    void newMessage(const std::string& text, int kind);    // 0x40e420
    void appendMessage(const std::string& text, int kind); // 0x40e5d0
    int revealStep();                                       // 0x40b210
    void showWindow(bool on);                               // 0x40e220
    void nameFrame();                                       // top of 0x410300
    void stateOpenClose();                                  // 1 / 2
    void stateReveal();                                     // 0
    void stateWait();                                       // 3
    void stateDone();                                       // 4
    void stateBacklog();                                    // 5
    void stateHidden();                                     // 6
    void buttons(bool takeInput);                           // the bar, icons, tooltip (state 3)
    void openBacklog();                                     // 0x410300 state 3's part
    void closeBacklog();                                    // 0x403e50
    void backlogFrame();                                    // 0x4038a0
    void backlogTexts(int hover);                           // the entries' texts made again
    void storyInput();                                      // 0x40a180
    void quickPreview();                                    // 0x409e80
    void choiceFrame();                                     // 0x413400
    void hideCursors();

    Game& game_;
    Gfx& gfx_;
    // Window (0x4d2d00..).
    bool exists_ = false;   // 0x4d2d00
    int state_ = 0;         // 0x4d2d04
    int style_ = 0;         // 0x4d2d08
    int openCount_ = 0;     // 0x4d2d0c
    int openFrames_ = 0;    // 0x4d2d10
    int page_ = 0;          // 0x4d2d14
    int kind_ = 1;          // 0x4d2d18
    int reveal_ = 0;        // 0x4d2d1c
    int novel_ = 0;         // 0x4d2d24
    bool novelFading_ = false;  // 0x4d2d28
    int novelHidden_ = 0;   // 0x4d2d2c
    int novelCount_ = 0, novelFrames_ = 0;  // 0x4d2d30 / 0x4d2d34
    int voiceSlot_ = 0;     // 0x4d2d38
    std::string buffer_;    // 0x4d2d3c
    int cursorCount_ = 0;   // 0x52169c
    // Name (0x4d3140..).
    std::string name_;      // 0x4d3140
    int nameCount_ = 0;     // 0x4d3180
    bool nameShown_ = false;  // 0x4d3184
    bool nameChanged_ = false;  // 0x4d318c
    int nameBuf_ = 0;       // 0x4d3190
    int speaker_ = -1;      // 0x4d313c
    std::string logName_;   // 0x5e76b0
    // Button bar.
    int barCount_ = 0;
    int tipCount_ = 0, tipButton_ = -1;
    // Backlog.
    std::deque<LogEntry> log_;
    int logScroll_ = 0;     // 0x521694
    int replayRow_ = 0, replayStep_ = 0;  // 0xa39250 / 0xa3924c: a backlog voice replay
    int backlogHover_ = -1;  // 0x4bf794
    // Counters.
    int autoCount_ = 0;     // 0x4be064
    int demoCount_ = 0;     // 0x4be040
    // Choices (0xa37ff8..).
    bool choiceShown_ = false;  // 0xa37ff8
    bool choiceActive_ = false; // 0xa38000
    int choicePhase_ = 0;       // 0xa38004: 1 fading in
    int choiceCount_ = 0;       // 0xa38008
    std::array<bool, 8> choiceDisabled_{};  // 0xa3909c
    int choiceHover_ = -1;      // 0x4bf798
    bool choiceBackFromHidden_ = false;  // 0xa39260
};

}  // namespace nyan::wa2
