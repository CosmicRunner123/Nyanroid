#include "nyan/render/wa2_session.hpp"

#include <algorithm>
#include <chrono>

#include "nyan/audio/mixer.hpp"
#include "nyan/common/error.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/wa2/game.hpp"
#include "wa2/renderer.hpp"

namespace nyan::render {

namespace {

constexpr int kScreenW = 1280;  // WIN_SIZE_W / WIN_SIZE_H defaults (WA2.exe 0x4c2250)
constexpr int kScreenH = 720;

// White Album 2's Session: a wa2::Game stepped at its 60 logic frames per second, with the platform's
// input fed to the engine's own input layer.
class Wa2Session final : public Session {
public:
    Wa2Session(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer, Options options)
        : game_(std::make_unique<wa2::Game>(std::move(folder), mixer)), mixer_(mixer), options_(std::move(options)) {
        last_ = std::chrono::steady_clock::now();
        game_->setSaveDirectory(options_.saveDirectory);
        game_->rand.seed(static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count()));
        game_->playMovie = [this](const wa2::Game::MovieRequest& r) { return startMovie(r); };
        // 0x4391f0: the frame so far drawn now (the target may not have been drawn since its object
        // was made, when several logic frames run between draws), then the target read.
        game_->gfx().readTarget = [this](int slot, wa2::RgbaImage& out) {
            if (!renderer_) return false;
            auto commands = game_->gfx().buildFrame(false);
            renderer_->execute(commands, game_->gfx());
            return renderer_->readTarget(slot, out);
        };
    }

    ~Wa2Session() override {
        if (mixer_ && movie_) mixer_->stop(audio::Mixer::Bus::Movie, 0);
    }

    Engine engine() const override { return Engine::WA2; }
    wa2::Game& game() { return *game_; }
    std::string title() const override { return "WHITE ALBUM2"; }  // the window title (0x4a3c4c)
    int screenWidth() const override { return kScreenW; }
    int screenHeight() const override { return kScreenH; }
    void setLog(std::function<void(const std::string&)> log) override { game_->logSink = std::move(log); }

    void frame(int width, int height, GLuint target) override {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        if (movie_) {
            // The game does not tick while a movie plays (0x4335ae); the movie runs by the clock.
            movieTime_ += std::min(dt, 0.25);
            movieFrame();
            renderMovie(width, height, target);
            return;
        }
        accumulator_ += dt;
        int steps = static_cast<int>(accumulator_ * wa2::Game::kTicksPerSecond);
        accumulator_ -= steps / static_cast<double>(wa2::Game::kTicksPerSecond);
        if (steps > 4) {
            steps = 4;  // after a stall, do not try to catch up
            accumulator_ = 0;
        }
        for (int i = 0; i < steps; ++i) step();
        renderFrame(width, height, target);
    }

    void step() override {
        if (movie_) {
            movieTime_ += 1.0 / wa2::Game::kTicksPerSecond;
            ++movieFrames_;
            movieFrame();
            return;
        }
        game_->tick();
        // A click arrives as press and release on the button's release (so that a touch that
        // turns into a gesture never clicks): the press was polled this frame, release it now.
        for (int b = 0; b < 3; ++b) {
            if (releaseNext_[static_cast<size_t>(b)]) {
                game_->input.setButton(b, false);
                releaseNext_[static_cast<size_t>(b)] = false;
            }
        }
    }

    void renderFrame(int width, int height, GLuint target) override {
        if (!renderer_) renderer_ = std::make_unique<wa2r::Renderer>();
        if (movie_) return renderMovie(width, height, target);
        // 0x441f70: the objects in priority order; then the back buffer onto the screen.
        auto commands = game_->gfx().buildFrame(game_->clearScreen);
        game_->clearScreen = false;
        renderer_->execute(commands, game_->gfx());
        renderer_->present(width, height, target);
        viewW_ = width;
        viewH_ = height;
    }

    uint64_t frameNumber() const override { return game_->frame() + movieFrames_; }

    void pointerMove(float x, float y) override {
        float vx, vy;
        toVirtual(x, y, vx, vy);
        game_->input.setPointer(vx, vy);
    }

    void mouseButton(int button, bool down, float x, float y) override {
        pointerMove(x, y);
        if (movie_) {
            // 0x44b630: a click in the window ends a movie that may be skipped.
            if (!down && (button == 1 || button == 3)) movieSkip_ = true;
            return;
        }
        int b = button == 1 ? wa2::Input::LeftButton : button == 3 ? wa2::Input::RightButton : wa2::Input::MiddleButton;
        auto i = static_cast<size_t>(b);
        if (down) {
            pending_[i] = true;
            return;
        }
        if (!pending_[i]) return;
        pending_[i] = false;
        game_->input.setButton(b, true);
        releaseNext_[i] = true;
    }

    void wheel(int steps) override { game_->input.addWheel(steps); }
    void key(int vk, bool down) override {
        // The decision and skip keys end a movie that may be skipped.
        if (movie_ && down && (vk == 0x0d || vk == 0x20 || vk == 0x1b || vk == 0x11)) movieSkip_ = true;
        game_->input.setKey(vk, down);
    }
    void cancelPress() override { pending_ = {}; }

    std::optional<Dialog> dialog() override { return std::nullopt; }
    void answerDialog(bool, const std::string&) override {}

    void setSkip(bool on) override { game_->modes.skip = on; }
    void setAuto(bool on) override { game_->modes.autoMode = on; }
    void setForceSkip(bool on) override { game_->input.setKey(0x11, on); }

    bool inStory() const override { return game_->mode() == wa2::Game::Mode::Story && game_->storyRunning(); }
    bool returnToTitle() override {
        // The window menu's Return To Title (WM_COMMAND 0x9c84 -> 0x413ac0).
        if (!game_->canGoTitle()) return false;
        game_->goTitle();
        return true;
    }
    bool openSettings() override {
        // The window menu has no options item; the window's OPTION button, as a click on it would.
        if (!game_->canGoTitle() || !game_->sceneAtRest()) return false;
        game_->openOptions();
        return true;
    }
    void suspend() override {
        game_->writeSystem();
        game_->saveSettings();
    }
    void releaseHeld() override {
        game_->input.releaseAll();
        pending_ = {};
    }
    bool quitRequested() const override { return game_->mode() == wa2::Game::Mode::Quit; }

    std::vector<std::string> languages() const override { return {}; }
    int language() const override { return 0; }
    void setLanguage(int) override {}

private:
    bool startMovie(const wa2::Game::MovieRequest& r) {
        std::shared_ptr<RandomAccessFile> file = game_->assets().folder().open(r.file);
        if (!file) return false;
        auto video = media::FfmpegVideo::open(file);
        if (!video) return false;
        movie_ = std::move(video);
        movieTime_ = 0;
        movieSkip_ = false;
        movieChanged_ = false;
        movieAudio_ = mixer_ && movie_->hasAudio();
        if (movieAudio_) {
            mixer_->playStream(audio::Mixer::Bus::Movie, 0, movie_->sampleRate(), movie_->audioSource(false),
                               static_cast<float>(r.volume) / 256.0f);
        }
        game_->input.releaseAll();
        pending_ = {};
        return true;
    }

    void movieFrame() {
        movieChanged_ = movie_->seek(movieTime_) || movieChanged_;
        bool skip = movieSkip_ && game_->movieSkippable();
        movieSkip_ = false;
        // EC_COMPLETE: every stream has finished.
        bool done = movie_->ended() && (!movieAudio_ || !mixer_->isPlaying(audio::Mixer::Bus::Movie, 0));
        if (!skip && !done) return;
        if (movieAudio_) mixer_->stop(audio::Mixer::Bus::Movie, 0);
        movie_.reset();
        game_->endMovie();
        accumulator_ = 0;
        last_ = std::chrono::steady_clock::now();
    }

    void renderMovie(int width, int height, GLuint target) {
        if (!renderer_) renderer_ = std::make_unique<wa2r::Renderer>();
        if (!movie_) return renderFrame(width, height, target);
        const auto& px = movie_->rgba();
        renderer_->presentMovie(px.empty() ? nullptr : px.data(), movie_->width(), movie_->height(), movieChanged_, width,
                                height, target);
        movieChanged_ = false;
        viewW_ = width;
        viewH_ = height;
    }

    // Framebuffer pixels to the 1280 x 720 screen, letterboxed.
    void toVirtual(float x, float y, float& vx, float& vy) const {
        if (viewW_ <= 0 || viewH_ <= 0) {
            vx = x, vy = y;
            return;
        }
        float scale = std::min(static_cast<float>(viewW_) / kScreenW, static_cast<float>(viewH_) / kScreenH);
        float offX = (static_cast<float>(viewW_) - kScreenW * scale) / 2;
        float offY = (static_cast<float>(viewH_) - kScreenH * scale) / 2;
        vx = (x - offX) / scale;
        vy = (y - offY) / scale;
    }

    std::unique_ptr<wa2::Game> game_;
    std::unique_ptr<wa2r::Renderer> renderer_;
    std::unique_ptr<media::FfmpegVideo> movie_;
    double movieTime_ = 0;
    uint64_t movieFrames_ = 0;
    bool movieSkip_ = false, movieChanged_ = false, movieAudio_ = false;
    audio::Mixer* mixer_;
    Options options_;
    std::chrono::steady_clock::time_point last_;
    double accumulator_ = 0;
    std::array<bool, 3> pending_{};
    std::array<bool, 3> releaseNext_{};
    int viewW_ = 0, viewH_ = 0;
};

}  // namespace

wa2::Game& wa2Game(Session& session) {
    return static_cast<Wa2Session&>(session).game();
}

std::unique_ptr<Session> createWa2Session(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                          Session::Options options) {
    return std::make_unique<Wa2Session>(std::move(folder), mixer, std::move(options));
}

}  // namespace nyan::render
