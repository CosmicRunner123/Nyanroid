#include "nyan/render/diesel_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "diesel/renderer.hpp"
#include "nyan/common/error.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/game_data.hpp"
#include "nyan/diesel/save_store.hpp"
#include "nyan/diesel/script_host.hpp"
#include "nyan/diesel/sound_player.hpp"

namespace nyan::render {

#include "diesel/hand_cursor.inc"

namespace {

// Development aid: NYAN_DIESEL_EVAL="F:code" (several joined by ";;") runs Squirrel on the game's VM
// before frame F (ScriptHost::evaluate), as nyan-cli boot --eval does, for captures of what the
// scripts reach only late. On Android, where an app gets no environment, the property
// debug.nyanroid.eval instead (adb shell setprop; at most 91 bytes, enough to start a scene).
struct DevEval {
    uint64_t frame = 0;
    std::string code;
};
std::vector<DevEval> devEvals() {
    std::vector<DevEval> out;
    std::string all;
    if (const char* env = std::getenv("NYAN_DIESEL_EVAL")) all = env;
#if defined(__ANDROID__)
    char prop[PROP_VALUE_MAX] = {};
    if (all.empty() && __system_property_get("debug.nyanroid.eval", prop) > 0) all = prop;
#endif
    if (all.empty()) return out;
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(";;", at);
        std::string one = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t colon = one.find(':');
        if (colon != std::string::npos) out.push_back({std::strtoull(one.c_str(), nullptr, 10), one.substr(colon + 1)});
        if (end == std::string::npos) break;
        at = end + 2;
    }
    return out;
}

// A Diesel Engine game's Session: the exe's MAIN task (the scripts on Squirrel) stepped at the frame
// rate the game sets, its draw list (sprites, text, movies) drawn each frame, its sounds and movies'
// sound through the mixer, the platform's input fed to Mware's keyboard and mouse, the app menu's
// entries through the game's own keys.
class DieselSession final : public Session {
public:
    DieselSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer, Options options)
        : folder_(std::move(folder)), options_(std::move(options)) {
        game_ = std::make_unique<diesel::GameData>(diesel::GameData::open(*folder_));
        saves_ = options_.saveDirectory.empty() ? diesel::makeMemorySaveStore()
                                                : diesel::makeDirectorySaveStore(options_.saveDirectory);
        if (mixer) {
            audio_ = std::make_unique<diesel::SoundPlayer>(
                mixer, [this](const std::string& path) { return host_->readGameFile(path); },
                [this](const std::string& path) { return host_->openGameFile(path); });
        }
        startHost();
        if (host_->state() == diesel::ScriptHost::State::Failed) fail("Diesel: " + host_->lastError());
        last_ = std::chrono::steady_clock::now();
    }

    Engine engine() const override { return Engine::Diesel; }
    std::string title() const override {
        return host_->windowTitle().empty() ? game_->windowName() : host_->windowTitle();
    }
    int screenWidth() const override { return game_->screenWidth(); }
    int screenHeight() const override { return game_->screenHeight(); }
    void setLog(std::function<void(const std::string&)> log) override { log_ = std::move(log); }

    void frame(int width, int height, GLuint target) override {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        int rate = std::max(1, host_->frameRate());
        accumulator_ += dt;
        int steps = static_cast<int>(accumulator_ * rate);
        accumulator_ -= steps / static_cast<double>(rate);
        if (steps > 4) {
            steps = 4;  // after a stall, do not try to catch up
            accumulator_ = 0;
        }
        for (int i = 0; i < steps; ++i) step();
        renderFrame(width, height, target);
    }

    void step() override {
        // The window closed for a restart (closeForReboot): nothing runs until the game starts again.
        if (rebootSteps_ > 0) {
            ++frameBase_;
            if (--rebootSteps_ == 0) reboot();
            return;
        }
        if (host_->state() != diesel::ScriptHost::State::Suspended) return;
        for (const auto& e : evals_) {
            if (e.frame == frameNumber()) log("eval " + e.code + " -> " + host_->evaluate(e.code));
        }
        host_->frame();
        if (host_->quitRequested() && host_->rebootRequested()) {
            closeForReboot();
            return;
        }
        // A pointer that moved with nothing pressed and stayed for a frame is a mouse's or the
        // trackpad's hover, which touches never give.
        if ((sentX_ != seenX_ || sentY_ != seenY_) && !leftHeld_ && !hold_.active) hoveredSinceTouch_ = true;
        seenX_ = sentX_, seenY_ = sentY_;
        flushQueued();
        // A finger left down past the app's own hold gesture (500 ms) with nothing taking it over:
        // a press held down after all.
        if (hold_.active && frameNumber() >= hold_.frame + holdFrames()) {
            hold_.active = false;
            pressHeld(hold_.lastGx, hold_.lastGy);
        }
        // A key the app menu pressed is let go after the frame that saw it down.
        if (menuKey_ && --menuKeyFrames_ <= 0) {
            host_->keyEvent(menuKey_, false);
            menuKey_ = 0;
        }
        // What the frame told the sound device goes to the mixer.
        auto events = host_->takeSoundEvents();
        if (audio_) audio_->apply(events, host_->soundVoices());
        if (host_->state() == diesel::ScriptHost::State::Failed && !reportedFailure_) {
            reportedFailure_ = true;
            log("script error: " + host_->lastError());
        }
    }

    void renderFrame(int width, int height, GLuint target) override {
        if (freshRenderer_) {  // a new process's device: no textures, render targets or captures
            renderer_.reset();
            freshRenderer_ = false;
        }
        if (!renderer_) {
            renderer_ = std::make_unique<dieselr::Renderer>(
                screenWidth(), screenHeight(), [this](const std::string& path) { return host_->readGameFile(path); },
                [this](const std::string& s) { log(s); },
                [this](const std::string& path) { return host_->openGameFile(path); });
        }
        if (rebootSteps_ > 0) renderer_->draw({}, {});  // no window: black
        else renderer_->draw(host_->drawItems(), host_->renderTargets());
        renderer_->present(width, height, target);
        viewW_ = width;
        viewH_ = height;
    }

    // Counted on through a restart (the frames of the earlier runs and of the wait between them).
    uint64_t frameNumber() const override { return frameBase_ + host_->frameNumber(); }

    // The platform's pointer and buttons, fed to Mware's mouse (Fitted: what a touch screen gives is
    // made to read as a mouse would).
    // - A left press that comes with the pointer jumping to it (a touch: a mouse or the trackpad's
    //   cursor rests where it clicks) is held back until it is known to be a press: the finger lifts
    //   (a tap: press, then release), drags sideways (a slider; a vertical move may still be the app's
    //   swipe), or stays down past the app's hold gesture; a gesture taking it over (cancelPress: a
    //   second finger, a swipe, a hold) drops it unseen. Muramasa's text box advances on the press
    //   (isMouseDownL), so a press handed on at once advanced the text under every gesture.
    // - The scripts' buttons find the pointer over them from the frame before (CNutUIContainer's
    //   focus, the CG thumbnails' hover), so a press where the last frame did not see the pointer
    //   waits a frame with the pointer there, and what comes after it waits behind it.
    void pointerMove(float x, float y) override {
        int gx, gy;
        toGame(x, y, gx, gy);
        if (hold_.active) {
            hold_.lastX = x, hold_.lastY = y, hold_.lastGx = gx, hold_.lastGy = gy;
            if (isDrag(x, y)) {
                hold_.active = false;
                pressHeld(gx, gy);
            }
            return;
        }
        if (queueing_) queued_.push_back({Input::Move, gx, gy, 0, false});
        else sendMove(gx, gy);
    }
    void mouseButton(int button, bool down, float x, float y) override {
        int gx, gy;
        toGame(x, y, gx, gy);
        int b = button == 1 ? 0 : button == 3 ? 1 : 2;
        if (b == 0 && !down && hold_.active) {  // a tap
            hold_.active = false;
            bool slid = std::fabs(x - hold_.x) > slop() || std::fabs(y - hold_.y) > slop();
            pressHeld(hold_.gx, hold_.gy);
            if (slid) queued_.push_back({Input::Move, gx, gy, 0, false}), queueing_ = true;
            if (queueing_) queued_.push_back({Input::Button, gx, gy, 0, false});
            else sendButton(0, false);
            return;
        }
        if (b == 0 && down && !queueing_ && !leftHeld_) {
            bool jumped = gx != seenX_ || gy != seenY_;
            bool touch = jumped || (lastPressTouch_ && !hoveredSinceTouch_);
            lastPressTouch_ = touch;
            hoveredSinceTouch_ = false;
            if (touch) {
                if (gx != sentX_ || gy != sentY_) sendMove(gx, gy);
                hold_ = Hold{true, x, y, gx, gy, x, y, gx, gy, frameNumber()};
                return;
            }
        }
        if (queueing_) {
            queued_.push_back({Input::Move, gx, gy, 0, false});
            queued_.push_back({Input::Button, gx, gy, b, down});
            return;
        }
        sendMove(gx, gy);
        sendButton(b, down);
    }
    // WM_MOUSEWHEEL's delta: 120 a notch, > 0 away from the user.
    void wheel(int steps) override { host_->wheelEvent(steps * 120); }
    void key(int vk, bool down) override { host_->keyEvent(vk, down); }
    // A touch gesture took the press over (a second finger, a swipe, a hold): a left press the game
    // has not seen is dropped; one it has is let go with the pointer off the window, as a finger slid
    // off before lifting would leave it, so no button under it takes the release as a click, and the
    // pointer comes back after the frame that saw the release.
    void cancelPress() override {
        if (hold_.active) {
            hold_.active = false;
            return;
        }
        for (size_t i = 0; i < queued_.size(); ++i) {
            if (queued_[i].kind == Input::Button && queued_[i].button == 0 && queued_[i].down) {
                queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(i));
                for (size_t j = i; j < queued_.size(); ++j) {
                    if (queued_[j].kind == Input::Button && queued_[j].button == 0 && !queued_[j].down) {
                        queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(j));
                        break;
                    }
                }
                return;
            }
        }
        if (!leftHeld_) return;
        int backX = sentX_, backY = sentY_;
        for (const Input& in : queued_) {
            if (in.kind == Input::Move) backX = in.x, backY = in.y;
        }
        host_->pointerEvent(-1, -1);
        sentX_ = -1, sentY_ = -1;
        host_->mouseButtonEvent(0, false);
        leftHeld_ = false;
        queued_.insert(queued_.begin(), {Input::Move, backX, backY, 0, false});
        queueing_ = true;
    }

    // SQApp.showMessageBox's box (the game waits in the call; ScriptHost::messageBox). The platform's
    // dialog has two buttons at most: OK alone; Yes / No (also for Yes / No / Cancel); otherwise the
    // box's first and last buttons as OK / Cancel. Closing the dialog gives the second (MB_OK: IDOK,
    // as closing a Windows box does). Every script of the three games asks for MB_OK.
    std::optional<Dialog> dialog() override {
        auto m = host_->messageBox();
        if (!m) return std::nullopt;
        Dialog d;
        d.serial = m->serial;
        d.title = m->caption;
        d.message = m->text;
        int buttons = m->type & 0xf;
        d.type = buttons == 0 ? 0 : (buttons == 3 || buttons == 4) ? 2 : 1;
        return d;
    }
    void answerDialog(bool ok, const std::string&) override {
        auto m = host_->messageBox();
        if (!m) return;
        int32_t button = 1;
        switch (m->type & 0xf) {
        case 0: button = 1; break;                // IDOK
        case 1: button = ok ? 1 : 2; break;       // IDOK / IDCANCEL
        case 2: button = ok ? 3 : 5; break;       // IDABORT / IDIGNORE
        case 3: case 4: button = ok ? 6 : 7; break;  // IDYES / IDNO
        case 5: button = ok ? 4 : 2; break;       // IDRETRY / IDCANCEL
        case 6: button = ok ? 2 : 11; break;      // IDCANCEL / IDCONTINUE
        default: break;
        }
        host_->answerMessageBox(button);
    }

    void setSkip(bool) override {}
    void setAuto(bool) override {}
    // The held-key skip is the scripts' own (Ctrl).
    void setForceSkip(bool on) override { host_->keyEvent(0x11, on); }

    // The app's menu. The story runs while the framework's in-story key handler does (the script
    // process KeyCheck: on the title only SystemKeyCheck runs); Return to Title and Settings press
    // its keys, T (the scripts ask first) and C, for a frame.
    bool inStory() const override {
        if (rebootSteps_ > 0 || host_->state() != diesel::ScriptHost::State::Suspended) return false;
        return host_->evaluate("foreach (p in ::g_ScriptManager.m_ScriptProcess) { if (p.name == \"KeyCheck\") "
                               "return 1; } return 0") == "1";
    }
    bool returnToTitle() override { return storyKey('T'); }
    bool openSettings() override { return storyKey('C'); }
    // The app may be ended while in the background (the PC's never is): the global data (text read,
    // settings, play time) is written as the scripts write it at their saves (SaveGlobalVars, to
    // val.npf), on the first command running (the boot script's). The story's place is kept only by the
    // player's saves, as on the PC.
    // After the game quit (exit(): the PC's process ends there) nothing more is written.
    void suspend() override {
        if (host_->state() != diesel::ScriptHost::State::Suspended || host_->quitRequested()) return;
        std::string r = host_->evaluate(
            "foreach (p in ::g_ScriptManager.m_ScriptProcess) { if (p.val instanceof ::CCommand) { "
            "p.val.SaveGlobalVars(); return 0; } } return 1");
        if (r != "0") log("suspend: global data not written (" + r + ")");
    }
    void releaseHeld() override {
        queued_.clear();
        queueing_ = false;
        leftHeld_ = false;
        hold_.active = false;
        host_->releaseInput();
    }
    // A quit that restarts the game (Totono's Reboot) does not end the app (closeForReboot).
    bool quitRequested() const override { return host_->quitRequested() && !host_->rebootRequested(); }

    std::vector<std::string> languages() const override { return {}; }
    int language() const override { return 0; }
    void setLanguage(int) override {}

    // The pointer as the exe's window shows it after the last frame (ScriptHost::cursor): Windows'
    // arrow for shape 1 (null: the front end's own arrow), its hand for 7 (Nyanroid's drawing,
    // hand_cursor.inc); hidden after showCursor(false) (no scene calls it).
    const Cursor* cursor() override {
        if (!host_->cursorVisible()) {
            if (hidden_.argb.empty()) hidden_ = Cursor{2, 1, 1, {0u}, 0, 0};
            return &hidden_;
        }
        if (host_->cursor() != 7) return nullptr;
        if (hand_.argb.empty()) {
            hand_cursor::Picture p = hand_cursor::draw();
            hand_ = Cursor{1, p.width, p.height, std::move(p.argb), p.hotX, p.hotY};
        }
        return &hand_;
    }

private:
    struct Input {
        enum Kind { Move, Button } kind;
        int x, y;
        int button;  // Mware's: 0 left, 1 right, 2 middle
        bool down;
    };
    void sendMove(int gx, int gy) {
        host_->pointerEvent(gx, gy);
        sentX_ = gx, sentY_ = gy;
    }
    void sendButton(int b, bool down) {
        if (b == 0 && down && (sentX_ != seenX_ || sentY_ != seenY_)) {
            queued_.push_back({Input::Button, sentX_, sentY_, b, down});
            queueing_ = true;
            return;
        }
        // A release in the frame its press went in waits for a frame: a tap (whose press goes in as the
        // finger lifts) or a trackpad tap is a quick click, as a mouse's is, never shorter than a
        // frame. Mware's queue (Mouse) keeps a press and release given between two updates both, and
        // then reports the release for two updates, so a button that opens a menu on the release
        // would have the menu see the same release and close it again (Mojika's settings).
        if (!down && b >= 0 && b < 3 && pressFrame_[b] == frameNumber()) {
            queued_.push_back({Input::Button, sentX_, sentY_, b, down});
            queueing_ = true;
            return;
        }
        host_->mouseButtonEvent(b, down);
        if (down && b >= 0 && b < 3) pressFrame_[b] = frameNumber();
        if (b == 0) leftHeld_ = down;
    }
    // A held touch press handed on: down where the finger landed (seen there since), then the pointer
    // where the finger is now, after a frame.
    void pressHeld(int nowGx, int nowGy) {
        if (queueing_) queued_.push_back({Input::Button, hold_.gx, hold_.gy, 0, true});
        else sendButton(0, true);
        if (nowGx != hold_.gx || nowGy != hold_.gy) {
            queued_.push_back({Input::Move, nowGx, nowGy, 0, false});
            queueing_ = true;
        }
    }
    // How far a finger goes before it is a drag (about Android's touch slop: 1.5 % of the view's
    // shorter side, 24 pixels on a 1600-pixel-high tablet), in view pixels; a drag is one not twice
    // as tall as wide, which the app may yet take for a swipe (80 dp, two to one).
    float slop() const { return std::max(8.0f, 0.015f * static_cast<float>(std::min(viewW_, viewH_))); }
    bool isDrag(float x, float y) const {
        float dx = std::fabs(x - hold_.x), dy = std::fabs(y - hold_.y);
        if (dx <= slop() && dy <= slop()) return false;
        return dy <= 2.0f * dx;
    }
    // The app's hold gesture fires at 500 ms: a press still held at 600 ms has not been taken over.
    uint64_t holdFrames() const { return static_cast<uint64_t>(std::max(1, host_->frameRate()) * 6 / 10 + 1); }

    // After a frame: what waited goes in, up to the next press that has to wait again.
    void flushQueued() {
        if (!queueing_) return;
        queueing_ = false;
        std::vector<Input> pending = std::move(queued_);
        queued_.clear();
        for (size_t i = 0; i < pending.size(); ++i) {
            if (queueing_) {
                queued_.push_back(pending[i]);
                continue;
            }
            if (pending[i].kind == Input::Move) sendMove(pending[i].x, pending[i].y);
            else sendButton(pending[i].button, pending[i].down);
        }
    }

    bool storyKey(int vk) {
        if (!inStory()) return false;
        if (menuKey_) host_->keyEvent(menuKey_, false);
        menuKey_ = vk;
        menuKeyFrames_ = 2;
        host_->keyEvent(vk, true);
        return true;
    }

    // The exe's start: a fresh VM over the game's archives and the save folder, its scripts started.
    void startHost() {
        host_ = std::make_unique<diesel::ScriptHost>(*game_, [this](const std::string& s) { log(s); }, saves_.get());
        addSystemFonts();
        // The exes seed their generator with time(0) (muramasa_en.exe 0x490aa7).
        host_->seedRandom(static_cast<int32_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())));
        // Screen captures' pictures come from the renderer (save thumbnails, frozen captures in saves).
        host_->setScreenReader([this](uint32_t screen, int w, int h, std::vector<uint8_t>& rgba) {
            return renderer_ && !freshRenderer_ && renderer_->readScreen(screen, w, h, rgba);
        });
        host_->setTargetReader([this](uint32_t target, int w, int h, std::vector<uint8_t>& rgba) {
            return renderer_ && !freshRenderer_ && renderer_->readTarget(target, w, h, rgba);
        });
        host_->start();
    }

    // Totono's Reboot (a script function): SaveGlobalVars, SaveSystemConfig, RebootRequest, exit().
    // RebootRequest (totono_en.exe 0x4019d0) has Windows open kmsdll.exe with "<the exe>" 3000
    // --nodlg (ShellExecuteW), and exit() closes the window, ending the process (ScriptHost::frame).
    // kmsdll.exe is in none of the installs, so what it does is read from its arguments (Guess): it
    // waits 3000 ms (the exe's TotonoMutex, 0x401bb3, turns a second copy away while the first is
    // still open) and starts the exe again with --nodlg, which skips the launcher (0x401dda, read at
    // 0x4081a1), which Nyanroid has none of. Meanwhile there is no window: the screen black, the
    // sound gone with the process. Then the game boots as when first opened, from what the scripts
    // saved (the global data, system.dat: SetClearTrue's option names the window anew).
    void closeForReboot() {
        if (audio_) audio_->stopAll();
        releaseHeld();
        menuKey_ = 0;
        rebootSteps_ = 3 * std::max(1, host_->frameRate());
        log("reboot: the game starts again in 3 s");
    }
    void reboot() {
        frameBase_ += host_->frameNumber();
        host_.reset();  // the old process is gone before the new one starts
        freshRenderer_ = true;
        reportedFailure_ = false;
        startHost();
        // The pointer where it is (the new window reads the cursor's place).
        if (sentX_ >= 0 && sentY_ >= 0) host_->pointerEvent(sentX_, sentY_);
        if (host_->state() == diesel::ScriptHost::State::Failed) {
            reportedFailure_ = true;
            log("script error: " + host_->lastError());
        }
    }

    // The host's fonts stand for the Windows fonts a game names by family (MS PGothic, ...).
    void addSystemFonts() {
        if (!options_.fallbackFont.empty()) host_->addSystemFont(std::make_shared<const Bytes>(options_.fallbackFont));
        for (const auto& dir : options_.fontDirectories) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                std::string ext = entry.path().extension().string();
                for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
                if (auto f = FdFile::open(entry.path().string())) {
                    host_->addSystemFont(std::make_shared<const Bytes>(f->readAll()));
                }
            }
        }
    }
    void log(const std::string& s) {
        if (log_) log_(s);
    }
    // Framebuffer pixels to game-screen pixels, through the letterbox.
    void toGame(float x, float y, int& gx, int& gy) const {
        if (!renderer_ || viewW_ <= 0 || viewH_ <= 0) {
            gx = static_cast<int>(x), gy = static_cast<int>(y);
            return;
        }
        auto vp = renderer_->viewport(viewW_, viewH_);
        float sx = vp[2] > 0 ? static_cast<float>(screenWidth()) / vp[2] : 1.0f;
        float sy = vp[3] > 0 ? static_cast<float>(screenHeight()) / vp[3] : 1.0f;
        gx = static_cast<int>((x - vp[0]) * sx);
        gy = static_cast<int>((y - vp[1]) * sy);
    }

    int menuKey_ = 0, menuKeyFrames_ = 0;  // a key the app menu holds down
    std::unique_ptr<GameFolder> folder_;
    Options options_;
    std::unique_ptr<diesel::GameData> game_;
    std::unique_ptr<diesel::SaveStore> saves_;
    std::unique_ptr<diesel::ScriptHost> host_;
    std::unique_ptr<dieselr::Renderer> renderer_;
    bool freshRenderer_ = false;  // the game restarted: the next draw makes a new renderer
    std::unique_ptr<diesel::SoundPlayer> audio_;
    // A restart (closeForReboot): the frames left with no window, and the frames run before the
    // current host's (earlier runs and the waits between them).
    int rebootSteps_ = 0;
    uint64_t frameBase_ = 0;
    std::vector<DevEval> evals_ = devEvals();
    std::function<void(const std::string&)> log_;
    std::chrono::steady_clock::time_point last_;
    double accumulator_ = 0;
    int viewW_ = 0, viewH_ = 0;
    bool reportedFailure_ = false;
    Cursor hand_, hidden_;
    // The input the session holds back (see mouseButton): the pointer as last handed to the host and
    // as the last frame saw it, whether the host holds the left button, and what waits for a frame.
    int sentX_ = 0, sentY_ = 0, seenX_ = 0, seenY_ = 0;
    bool leftHeld_ = false;
    std::array<uint64_t, 3> pressFrame_{UINT64_MAX, UINT64_MAX, UINT64_MAX};  // the frame each button's press went in
    bool queueing_ = false;
    std::vector<Input> queued_;
    // A touch's left press held back (see mouseButton): where the finger landed and is now (view and
    // game pixels) and the frame it landed in; whether the last press was a touch's, and whether a
    // mouse-like hover came since (a press with no jump after a touch's is a touch's again).
    struct Hold {
        bool active = false;
        float x = 0, y = 0;
        int gx = 0, gy = 0;
        float lastX = 0, lastY = 0;
        int lastGx = 0, lastGy = 0;
        uint64_t frame = 0;
    } hold_;
    bool lastPressTouch_ = false, hoveredSinceTouch_ = false;
};

}  // namespace

std::unique_ptr<Session> createDieselSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                             Session::Options options) {
    return std::make_unique<DieselSession>(std::move(folder), mixer, std::move(options));
}

}  // namespace nyan::render
