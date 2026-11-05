#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/common/engine.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/render/gl.hpp"

namespace nyan::audio {
class Mixer;
}

namespace nyan::render {

// A running game as the front ends (desktop, Android) drive it, whatever its engine: steps it
// against real time and draws it, takes raw device input, and runs the reading modes and the app
// menu. Each engine implements it (CatSystem2: GameSession); createSession() makes the one for a
// game folder. GL-thread only (except the mixer, which is thread-safe).
class Session {
public:
    struct Options {
        size_t textureBudgetBytes = 384u * 1024 * 1024;
        Bytes fallbackFont;  // used if the game ships no font
        std::string saveDirectory;  // this game's saves; empty = not kept between runs
        std::vector<std::string> fontDirectories;  // extra fonts (Windows system fonts the user supplied)
    };

    // A dialog the game waits on (on PC a window of its own: text entry, a question), which the
    // platform shows.
    struct Dialog {
        uint32_t serial = 0;    // changes with each new dialog
        bool input = false;     // text entry (else a message)
        std::string title;      // text entry: the prompt
        std::string message;    // message: its text
        std::string value;      // text entry: the starting text
        int32_t maxChars = 0;   // text entry: length limit in characters (UTF-16 units; 0: none)
        int32_t type = 0;       // message: 0 OK only, 1 OK / Cancel, 2 yes / no
        bool cancel = true;     // text entry: whether it can be cancelled
    };

    virtual ~Session() = default;

    virtual Engine engine() const = 0;
    // The game's name (for the window title) and its virtual screen size.
    virtual std::string title() const = 0;
    virtual int screenWidth() const = 0;
    virtual int screenHeight() const = 0;
    // Where the engine's messages go (unhandled commands, missing assets, ...).
    virtual void setLog(std::function<void(const std::string&)> log) = 0;

    // Advances to the current real time (at most a few frames) and renders.
    virtual void frame(int width, int height, GLuint target = 0) = 0;
    // Advances exactly one simulation frame (for captures/tests).
    virtual void step() = 0;
    // Draws the current state without advancing time.
    virtual void renderFrame(int width, int height, GLuint target = 0) = 0;
    // The game's frame counter (60 Hz), which captures and scripted input are timed by.
    virtual uint64_t frameNumber() const = 0;

    // Raw device input, interpreted through the game's own key bindings. Coordinates are
    // framebuffer pixels. Mouse buttons use engine codes: 1 left, 3 right.
    virtual void pointerMove(float x, float y) = 0;
    virtual void mouseButton(int button, bool down, float x, float y) = 0;
    virtual void wheel(int steps) = 0;            // > 0 up (away from the user), < 0 down
    virtual void key(int vk, bool down) = 0;      // Windows virtual-key code
    virtual void cancelPress() = 0;               // drop a pressed button without clicking (touch gestures)
    // A dialog the game is waiting on, if any, and the player's answer to it (OK / yes, and for
    // text entry the text entered).
    virtual std::optional<Dialog> dialog() = 0;
    virtual void answerDialog(bool ok, const std::string& valueUtf8 = {}) = 0;

    // Reading modes: skip (read text), auto, and force skip (the held-key skip: unread text too).
    virtual void setSkip(bool on) = 0;
    virtual void setAuto(bool on) = 0;
    virtual void setForceSkip(bool on) = 0;

    // The app's menu (Android Back), standing in for the original's window menu bar.
    // True while the story runs, where Return to Title and Settings apply.
    virtual bool inStory() const = 0;
    // Return to Title: the story ends and the game shows its title. False if no story runs.
    virtual bool returnToTitle() = 0;
    // Settings: the game's own options screen, over the story. False if it can't.
    virtual bool openSettings() = 0;
    // The app is closing or going to the background: keep the story's place and the global data.
    virtual void suspend() = 0;
    // Forgets keys and buttons held when input went elsewhere (the app menu took over the touch).
    virtual void releaseHeld() = 0;
    // True once the game asked to quit (Quit Game on its title screen).
    virtual bool quitRequested() const = 0;

    // The languages the game ships, as it names them (empty for single-language games), the one
    // in use, and choosing one, which is kept as the game's own options screen would keep it.
    virtual std::vector<std::string> languages() const = 0;
    virtual int language() const = 0;
    virtual void setLanguage(int index) = 0;

    // The game's own mouse cursor as its engine would show it now, for a front end that draws a
    // pointer of its own (Android's virtual cursor): ARGB pixels (0xAARRGGBB, rows top-down) in the
    // game's virtual pixels and the hotspot; the serial changes with the picture. Null: the
    // system's arrow, which is also what an engine without cursors of its own gives.
    struct Cursor {
        uint32_t serial = 0;
        int width = 0, height = 0;
        std::vector<uint32_t> argb;
        int hotX = 0, hotY = 0;
    };
    virtual const Cursor* cursor() { return nullptr; }
};

// Creates the session for the game in `folder`, of the engine detectEngine() finds there.
// `mixer` may be null (no sound). Throws nyan::Error if there is no such engine or the game
// fails to load.
std::unique_ptr<Session> createSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                       Session::Options options);

// The fonts the game in `folder` names that are neither its own nor in `fontDirectories` (a
// Windows system font such as MS Gothic that the player has not added), so that the platform's
// font would stand in for them; empty when it has what it needs. Any thread; it does not start
// the game. Only CatSystem2 games name such fonts before they run.
std::vector<std::string> missingFonts(const GameFolder& folder, const std::vector<std::string>& fontDirectories);

}  // namespace nyan::render
