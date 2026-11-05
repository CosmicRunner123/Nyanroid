#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/diesel/draw_item.hpp"
#include "nyan/diesel/game_data.hpp"
#include "nyan/diesel/save_store.hpp"
#include "nyan/diesel/sound_events.hpp"

namespace nyan::diesel {

// The exes' Squirrel VM with the engine's natives, running the game's scripts as the exe does:
// its MAIN task (muramasa_en.exe 0x47eae0) runs media/script/main.nut with the root table as
// `this`; the script suspends the VM (`suspend()`) at the end of each frame and the task wakes it the
// next frame.
class ScriptHost {
public:
    using Log = std::function<void(const std::string&)>;

    // `saves`: where the game's writes go (the PC's save folder); null: a store in memory.
    ScriptHost(const GameData& game, Log log, SaveStore* saves = nullptr);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    enum class State { Idle, Suspended, Finished, Failed };

    // Seeds RandomI / RandomF's generator (the exes seed it with time(0); 0 if not called).
    void seedRandom(int32_t seed);
    // Starts the MAIN task: runs main.nut until it suspends, returns or fails.
    void start();
    // One frame: wakes the suspended VM and runs it until it suspends again.
    void frame();
    // Runs Squirrel source text on a thread of the VM sharing its root table, as a debugger would
    // (tests, nyan-cli): what it returns, as text, or "error: ..." (the game's state is untouched).
    std::string evaluate(const std::string& source);
    State state() const;
    // The last script error (message and call stack), if the VM failed.
    const std::string& lastError() const;
    // Where the script threads other than the main one wait (their call stacks), for nyan-cli.
    std::string threadStacks() const;
    uint64_t frameNumber() const;

    // Input between frames, as the window's messages bring it (the next frame's input update ages
    // it): Windows virtual-key codes; mouse buttons 0 left, 1 right, 2 middle; the pointer in
    // game-screen pixels; wheel deltas as WM_MOUSEWHEEL's (120 a notch, > 0 away from the user).
    void keyEvent(int vk, bool down);
    void mouseButtonEvent(int button, bool down);
    void pointerEvent(int x, int y);
    void wheelEvent(int delta);
    // The window lost focus: held keys and buttons are released.
    void releaseInput();
    // The window's close button: the script's close callback, else quitting.
    void requestClose();
    // The game asked to quit (SQApp.exit, or closing without a callback).
    bool quitRequested() const;
    // The game asked to be started again (Totono's RebootRequest, before its exit()).
    bool rebootRequested() const;

    // SQApp.showMessageBox's dialog (MessageBoxW: on PC the frame loop stops until it is answered):
    // while one is open frame() does nothing; answerMessageBox gives the button the player chose
    // (IDOK 1, IDCANCEL 2, IDABORT 3, IDRETRY 4, IDIGNORE 5, IDYES 6, IDNO 7, IDTRYAGAIN 10,
    // IDCONTINUE 11) and the next frame() finishes the frame the box interrupted.
    struct MessageBox {
        uint32_t serial = 0;
        std::string caption, text;  // UTF-8
        int32_t type = 0;           // MB_*: the buttons in the low four bits, the icon above
    };
    std::optional<MessageBox> messageBox() const;
    void answerMessageBox(int32_t button);

    // What the window shows: its title (SQApp.setWindowTitle, UTF-8), the cursor's shape as the
    // window shows it after the last frame (1 Windows' arrow, 7 its hand) and whether it shows, and
    // the frames a second the game runs at.
    const std::string& windowTitle() const;
    int cursor() const;
    bool cursorVisible() const;
    int frameRate() const;
    // The last frame as drawn, in draw order (laid out after the scripts ran, before the task
    // objects stepped: what the PC shows for that frame), on the game screen's pixels.
    const std::vector<DrawItem>& drawItems() const;
    // The render targets and masks the draw items of the frame name.
    const std::vector<RenderTarget>& renderTargets() const;
    // A file as the scripts' paths name it ("media/cg/..."): the archives, then the writable files
    // and the install folder, as SQSprite.create finds a texture.
    std::optional<Bytes> readGameFile(const std::string& path) const;
    // The same file for reading piece by piece (movies stream from the archives); null if missing.
    std::shared_ptr<RandomAccessFile> openGameFile(const std::string& path) const;
    // A font file of the host's (the Windows fonts a game names by family, such as MS PGothic):
    // its faces join those the game's fonts are looked up in.
    void addSystemFont(std::shared_ptr<const Bytes> data);
    // The renderer's picture of a screen capture (DrawItem::screen; 0 or one not drawn yet: the last
    // frame), scaled to width x height, RGBA rows from the top: what SQSprite.saveImage (save
    // thumbnails) and serialize read. Without one (headless), those fail as with no picture.
    using ScreenReader = std::function<bool(uint32_t screen, int width, int height, std::vector<uint8_t>& rgba)>;
    void setScreenReader(ScreenReader reader);
    // The renderer's picture of a render target (RenderTarget::id) as last drawn, scaled the same
    // way: what SQSprite.saveImage reads of a render target's sprite (Mojika's save thumbnails).
    void setTargetReader(ScreenReader reader);
    // The sound device calls since the last take (plays, stops, pauses), in order, and the playing
    // voices' settings now (a voice that ends by itself just leaves the list): a host plays the
    // audio from these.
    std::vector<SoundEvent> takeSoundEvents();
    std::vector<SoundVoice> soundVoices() const;

    // What the scripts reached that Nyanroid lacks: natives not implemented ("SQSprite.setPos"),
    // files not found, with counts; for nyan-cli's reports.
    const std::map<std::string, int>& unhandled() const;
    const std::map<std::string, int>& missing() const;
    // Every native registered (globals by name, methods as "Class.method") and whether Nyanroid
    // implements it (false: the stub that notes it as unhandled); with NYAN_DIESEL_COUNT_NATIVES set
    // when the host is made, the implemented ones' calls by name (the stubs' are in unhandled()).
    const std::map<std::string, bool>& natives() const;
    const std::map<std::string, uint64_t>& nativeCalls() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nyan::diesel
