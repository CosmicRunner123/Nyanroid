#pragma once

#include <chrono>
#include <memory>

#include "nyan/audio/mixer.hpp"
#include "nyan/cs2/game.hpp"
#include "nyan/render/session.hpp"
#include "nyan/render/stage_renderer.hpp"

namespace nyan::render {

// CatSystem2's Session: a running cs2::Game plus its renderer. Steps the 60 Hz simulation against
// real time, turns pointer input into advance/choose actions, and draws.
class GameSession final : public Session {
public:
    GameSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer, Options options);

    cs2::Game& game() { return *game_; }
    StageRenderer& renderer() { return *renderer_; }

    Engine engine() const override { return Engine::CatSystem2; }
    // startup.xml's title and screen size.
    std::string title() const override { return game_->config().title; }
    int screenWidth() const override { return game_->config().width; }
    int screenHeight() const override { return game_->config().height; }
    void setLog(std::function<void(const std::string&)> log) override { game_->player().log = std::move(log); }

    void frame(int width, int height, GLuint target = 0) override;
    void step() override;
    void renderFrame(int width, int height, GLuint target = 0) override;
    uint64_t frameNumber() const override { return game_->player().frame(); }

    // Input goes through the game's own key bindings (startup.xml KEYCUSTOMIZE).
    void pointerMove(float x, float y) override;
    const Cursor* cursor() override;
    void mouseButton(int button, bool down, float x, float y) override;
    void wheel(int steps) override;
    void key(int vk, bool down) override;
    void pointerUp(float x, float y);   // a left click (press + release)
    void cancelPress() override;
    void advance();
    void choose(size_t index);
    // The dialog of the FE screen that has the input (`input_text`, `messagebox`).
    std::optional<Dialog> dialog() override;
    void answerDialog(bool ok, const std::string& valueUtf8 = {}) override;

    // True once the game's flow script has finished (Quit Game on the title screen).
    bool quitRequested() const override { return game_->quitRequested(); }

    void setSkip(bool on) override { skipping_ = on; }
    void setForceSkip(bool on) override { forceSkip_ = on; }
    void setAuto(bool on) override { auto_ = on; }

    // The story is the scene engine running (not the flow's title and menu screens).
    bool inStory() const override { return game_->sceneRunning(); }
    // System > Return to Title: the story ends and the game's flow shows its title.
    bool returnToTitle() override;
    void releaseHeld() override;
    // Autosaves the story's place and writes global data.
    void suspend() override;
    // The game's in-game system menu (where its options are).
    bool openSettings() override;
    bool skipping() const { return skipping_ || forceSkip_ || jumping_; }

    // startup.xml <language>. The one in use is the story text's (`lang_text_main`); choosing one
    // sets both languages and writes global data.
    std::vector<std::string> languages() const override;
    int language() const override { return game_->conf("lang_text_main", 0); }
    void setLanguage(int index) override;

private:
    // The cursor() handed out, and the game's picture it was made from.
    Cursor cursor_;
    const cs2::CursorImage* cursorFrom_ = nullptr;
    // Where input goes: an engine/menu FE screen, or the story (message window and scene).
    cs2::FesInstance* activeScreen();
    void storyAction(int action, bool down);
    void storyInput(int code, bool mouse, bool down);
    void systemRequest(int32_t n);
    // The Auto button / key: starts auto, or speeds it up (adv.xml etc/automag), or stops it.
    void autoRequest();
    // Skip and auto around a screen the scene runs (`fes` / `fesjs` / `fex`), after each tick.
    void screenModes();
    // Grabs the scene for the next save's thumbnail (before a menu covers it).
    void captureThumbnail();

    std::unique_ptr<cs2::Game> game_;
    std::unique_ptr<StageRenderer> renderer_;
    bool skipping_ = false;
    bool auto_ = false;
    bool forceSkip_ = false;   // Force Skip key held
    // What a `fex` screen keeps for its return (sscript's m[0x84454] skip, m[0x2ba38] auto, the
    // auto level m[0x84444]).
    bool fexSkip_ = false, fexAuto_ = false;
    int32_t fexAutoLevel_ = 1;
    bool thumbnailHeld_ = false;  // a screen's `_SYS_FEXSEL_CAPTURE` picture waits for the save
    // A click (or a pick) waiting for the next step, which runs it before the tick (see step()).
    bool advancePending_ = false;
    int64_t choicePending_ = -1;
    // The renderer's window settings from the window script, after the step's lines have run.
    void syncRenderer();
    bool jumping_ = false;     // Skip to Next Option running
    // Skip to Next Option as sscript runs it (0x091b9e): the screen holds 16 frames, fades to black
    // over 16, the story runs on to the stop, and the screen fades back in over 16.
    enum class JumpPhase { None, Hold, Out, Run, In };
    static constexpr uint32_t kJumpFadeFrames = 16;
    JumpPhase jumpPhase_ = JumpPhase::None;
    uint32_t jumpFrame_ = 0;
    void stepJump();
    float jumpFade() const;
    bool windowHidden_ = false;
    float mouseVX_ = 0, mouseVY_ = 0;
    int mousePending_ = 0;
    uint32_t skipWait_ = 0;  // frames until the next skipped message  // story: button pressed, acting on its release
    std::chrono::steady_clock::time_point last_;
    double accumulator_ = 0;
};

}  // namespace nyan::render
