// nyan-desktop: plays a game in a window (development front end).
//
//   nyan-desktop <game-dir> [scene] [--auto] [--lang en] [--capture <dir> --every <frames> --frames <max>]
//   nyan-desktop <game-dir> --capture <dir> --pages <n> [--settle <frames>] [--click ...]
//     (CatSystem2) at each input with its message shown, waits --settle frames (30), saves
//     <scene>_<input index>.png and clicks on, for n pages, so that captures of the original game at
//     the same inputs compare page by page.
//
// Controls: the game's own key and mouse bindings (CatSystem2: startup.xml KEYCUSTOMIZE), e.g.
// click/Enter advance, right click/Esc options, Space hide window, Ctrl skip. F11 toggles fullscreen.
// The scene argument, --conf, --var, --lang, --fes and --skip-to are CatSystem2 development options.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "nyan/audio/mixer.hpp"
#include "nyan/cs2/kif_seed_search.hpp"
#include "nyan/render/game_session.hpp"
#include "nyan/render/gl.hpp"
#include "nyan/render/session.hpp"
#include "nyan/render/wa2_session.hpp"
#include "nyan/wa2/game.hpp"
#include "pulse_output.hpp"
#include "stb_image_write.h"

namespace {

// SDL key -> Windows virtual-key code (the engine's key bindings use these).
int virtualKey(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return 'A' + static_cast<int>(k - SDLK_A);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + static_cast<int>(k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + static_cast<int>(k - SDLK_F1);
    switch (k) {
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 0x0D;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return 0x11;
    case SDLK_LALT:
    case SDLK_RALT: return 0x12;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_DELETE: return 0x2E;
    case SDLK_KP_2: return 0x62;
    case SDLK_KP_4: return 0x64;
    case SDLK_KP_6: return 0x66;
    case SDLK_KP_8: return 0x68;
    default: return 0;
    }
}

}  // namespace

using namespace nyan;

namespace {

struct Options {
    std::string gameDir;
    std::string scene;
    bool autoAdvance = false;
    bool mix = false;  // --mix: captures keep the mixer, rendered silently in step with the frames
    std::string captureDir;
    uint64_t captureEvery = 0;
    uint64_t maxFrames = 0;
    size_t skipTo = 0;  // fast-forward to this line of the starting scene
    int32_t particleEvery = 1;
    int32_t skipToInput = -1;   // --skip-to-input <n>: fast-forward until the starting scene waits at input n
                                // (--click frames then count from there)  // --particle-every <n>: particle plugins updated every n frames (PC harness speed)
    uint64_t pages = 0;   // --pages <n>: capture n pages, clicking on after each
    uint64_t settle = 30; // --settle <frames>: wait after the message is shown before a page capture
    std::string saveDir;  // --saves <dir>; default ~/.local/share/nyanroid/<game folder>
    std::string fallbackFont;  // --fallback-font <file>
    bool noBundledFonts = false;  // --no-bundled-fonts: as a player without the fonts/ folder's fonts
    std::string fes;           // --fes <script>: open an FE screen over the game (testing menus)
    std::string language;      // --lang <prefix>: story and system language (startup.xml <language>)
    std::vector<std::pair<std::string, int32_t>> conf;  // --conf <key>=<value>: engine settings (getconf keys)
    std::vector<std::pair<int32_t, int32_t>> vars;      // --var <n>=<value>: a variable set before the start
    struct Click {
        uint64_t frame;
        float x, y;     // virtual-screen coordinates
        int button = 1; // engine mouse code; 0 = key `vk`
        int vk = 0;
    };
    // Scripted input for captures: --click frame:x,y, --rclick frame:x,y, --key frame:vk,
    // --press / --move / --release frame:x,y (a drag with the left button)
    std::vector<Click> clicks;
    // Answers to the game's dialogs (name entry, questions), in order: `--answer ok`, `--answer no`,
    // `--answer "ok:Text"`. Without one a dialog is accepted as it stands.
    std::vector<std::string> answers;
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--auto") o.autoAdvance = true;
        else if (a == "--capture") o.captureDir = next();
        else if (a == "--every") o.captureEvery = std::stoull(next());
        else if (a == "--frames") o.maxFrames = std::stoull(next());
        else if (a == "--skip-to") o.skipTo = std::stoull(next());
        else if (a == "--skip-to-input") o.skipToInput = std::atoi(next().c_str());
        else if (a == "--particle-every") o.particleEvery = std::max(1, std::atoi(next().c_str()));
        else if (a == "--pages") o.pages = std::stoull(next());
        else if (a == "--settle") o.settle = std::stoull(next());
        else if (a == "--saves") o.saveDir = next();
        else if (a == "--mix") o.mix = true;
        else if (a == "--fallback-font") o.fallbackFont = next();
        else if (a == "--no-bundled-fonts") o.noBundledFonts = true;
        else if (a == "--fes") o.fes = next();
        else if (a == "--lang") o.language = next();
        else if (a == "--answer") o.answers.push_back(next());
        else if (a == "--var") {
            std::string v = next();
            if (auto eq = v.find('='); eq != std::string::npos) o.vars.emplace_back(std::atoi(v.c_str()), std::atoi(v.c_str() + eq + 1));
        } else if (a == "--conf") {
            std::string v = next();
            if (auto eq = v.find('='); eq != std::string::npos) o.conf.emplace_back(v.substr(0, eq), std::atoi(v.c_str() + eq + 1));
        }
        else if (a == "--click" || a == "--rclick" || a == "--press" || a == "--move" || a == "--release") {
            // --press / --move / --release frame:x,y: the left button's parts, for drags.
            std::string v = next();
            Options::Click c{};
            c.button = a == "--click" ? 1 : a == "--rclick" ? 3 : a == "--press" ? -4 : a == "--move" ? -5 : -6;
            unsigned long long f = 0;
            if (std::sscanf(v.c_str(), "%llu:%f,%f", &f, &c.x, &c.y) == 3) {
                c.frame = f;
                o.clicks.push_back(c);
            }
        } else if (a == "--wheel") {
            std::string v = next();
            Options::Click c{};
            c.button = -1;
            unsigned long long f = 0;
            if (std::sscanf(v.c_str(), "%llu:%i", &f, &c.vk) == 2) {
                c.frame = f;
                o.clicks.push_back(c);
            }
        } else if (a == "--key" || a == "--keydown" || a == "--keyup") {
            std::string v = next();
            Options::Click c{};
            c.button = a == "--key" ? 0 : a == "--keydown" ? -2 : -3;
            unsigned long long f = 0;
            if (std::sscanf(v.c_str(), "%llu:%i", &f, &c.vk) == 2) {
                c.frame = f;
                o.clicks.push_back(c);
            }
        }
        else if (o.gameDir.empty()) o.gameDir = a;
        else o.scene = a;
    }
    return !o.gameDir.empty();
}

std::string defaultSaveDir(const std::string& gameDir) {
    namespace fs = std::filesystem;
    fs::path base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) base = xdg;
    else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".local" / "share";
    else return {};
    fs::path game = fs::path(gameDir).lexically_normal();
    if (game.filename().empty()) game = game.parent_path();
    return (base / "nyanroid" / game.filename()).string();
}

void savePng(const std::string& path, int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        std::fprintf(stderr, "usage: nyan-desktop <game-dir> [scene] [--auto] [--capture dir --every N --frames N]\n");
        return 2;
    }
    bool capture = !opt.captureDir.empty();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (capture) flags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("Nyanroid", 1024, 576, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(capture ? 0 : 1);
    std::printf("GL: %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    audio::Mixer mixer(48000);
    std::unique_ptr<render::Session> session;
    try {
        render::Session::Options so;
        // Fonts in the project's fonts/ folder (none in the public source): searched by name. A face
        // no folder has falls back to MS Gothic by name, else to the system's Japanese font (the
        // games are made for Japanese Windows), else to a Latin one.
        if (!opt.noBundledFonts) so.fontDirectories.push_back(NYAN_BUNDLED_FONT_DIR);
        if (!opt.fallbackFont.empty()) {
            if (auto f = FdFile::open(opt.fallbackFont)) so.fallbackFont = f->readAll();
        }
        for (const char* path : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            if (!so.fallbackFont.empty()) break;
            if (auto f = FdFile::open(path)) {
                so.fallbackFont = f->readAll();
                break;
            }
        }
        so.saveDirectory = opt.saveDir.empty() ? defaultSaveDir(opt.gameDir) : opt.saveDir;
        // Archive seeds found from the names (a protected exe's): ~/.local/share/nyanroid, or the
        // --saves folder (a test run's).
        if (!so.saveDirectory.empty()) {
            cs2::setKifSeedCacheDirectory(opt.saveDir.empty() ? std::filesystem::path(so.saveDirectory).parent_path().string()
                                                              : opt.saveDir);
        }
        // User-supplied fonts (e.g. Windows system fonts a game names): ~/.local/share/nyanroid/fonts
        if (const char* home = std::getenv("HOME")) {
            so.fontDirectories.push_back(std::string(home) + "/.local/share/nyanroid/fonts");
        }
        session = render::createSession(std::make_unique<DirectoryGameFolder>(opt.gameDir), capture && !opt.mix ? nullptr : &mixer,
                                        std::move(so));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    // The CatSystem2 development options reach into the game itself.
    auto* catSystem2 =
        session->engine() == Engine::CatSystem2 ? static_cast<render::GameSession*>(session.get()) : nullptr;
    // White Album 2: a scene argument starts the story at that script; --skip-to skips (unread text
    // too) until that message line of it or a choice.
    if (session->engine() == Engine::WA2 && !opt.scene.empty()) {
        auto& wa2 = render::wa2Game(*session);
        wa2.startStory(opt.scene);
        opt.scene.clear();
        if (opt.skipTo) {
            wa2.modes.skip = wa2.modes.skipUnread = true;
            while (wa2.storyRunning() && wa2.currentLine < static_cast<int>(opt.skipTo) && !wa2.choiceShown) session->step();
            wa2.modes.skip = wa2.modes.skipUnread = false;
            std::printf("skipped to line %d at frame %llu\n", wa2.currentLine, static_cast<unsigned long long>(wa2.frame()));
            opt.skipTo = 0;
        }
    }
    if (!catSystem2 &&
        (!opt.scene.empty() || !opt.conf.empty() || !opt.vars.empty() || !opt.language.empty() || !opt.fes.empty() ||
         opt.skipTo)) {
        std::fprintf(stderr, "a scene, --conf, --var, --lang, --fes and --skip-to are for CatSystem2 games: ignored\n");
    }
    if (catSystem2) {
        auto& game = catSystem2->game();
        for (const auto& [k, v] : opt.conf) game.config_store()[k] = v;
        for (const auto& [n, v] : opt.vars) game.vars().set(n, v);
        if (!opt.language.empty()) {
            const auto& langs = game.languages();
            auto it = std::find_if(langs.begin(), langs.end(), [&](const auto& l) { return l.prefix == opt.language; });
            if (it == langs.end()) std::fprintf(stderr, "language %s not offered by this game\n", opt.language.c_str());
            else game.setLanguage(static_cast<int32_t>(it - langs.begin()));
        }
    }
    std::printf("%s (%dx%d)\n", session->title().c_str(), session->screenWidth(), session->screenHeight());
    std::string title = "Nyanroid - " + session->title();
    SDL_SetWindowTitle(window, title.c_str());
    int capW = session->screenWidth(), capH = session->screenHeight();
    if (!capture) SDL_SetWindowSize(window, capW, capH);
    // NYAN_CAPTURE_SCALE=<f>: captures rendered that much larger (a phone's or tablet's resolution).
    if (const char* s = std::getenv("NYAN_CAPTURE_SCALE"); s && capture && std::atof(s) > 0) {
        capW = static_cast<int>(capW * std::atof(s)), capH = static_cast<int>(capH * std::atof(s));
    }

    session->setLog([](const std::string& m) {
        if (!m.starts_with("unhandled") && !m.starts_with("missing")) std::printf("%s\n", m.c_str());
    });
    if (catSystem2) {
        if (!opt.scene.empty()) {
            catSystem2->game().playScene(opt.scene);
            // Again after the flow's #START, which playScene runs and which sets variables too.
            for (const auto& [n, v] : opt.vars) catSystem2->game().vars().set(n, v);
        }
        if (!opt.fes.empty()) catSystem2->game().openSystemScreen(opt.fes);
    }
    session->setAuto(opt.autoAdvance);
    if (catSystem2) catSystem2->game().player().setParticleEvery(opt.particleEvery);
    if (catSystem2 && opt.skipToInput >= 0) {
        // Skip quickly until the starting scene waits at that input (the PC harness counts pages
        // the same way: sscript's input index), then wait there: captures and --click go on.
        auto& player = catSystem2->game().player();
        const std::string start = player.sceneName();
        // Force skip can pass an input within one step, so it stops a few inputs short; from there
        // each input seen waiting below the target is clicked, one at a time.
        auto waitingAt = [&]() {
            return player.sceneName() == start && player.state() == cs2::ScenePlayer::State::Input ? player.inputIndex() : -1;
        };
        session->setForceSkip(true);
        const int32_t fastUntil = std::getenv("NYAN_SKIP_FAST_UNTIL") ? std::atoi(std::getenv("NYAN_SKIP_FAST_UNTIL")) : opt.skipToInput - 4;
        while (player.state() != cs2::ScenePlayer::State::Ended && waitingAt() < fastUntil) {
            session->step();
            if (player.state() == cs2::ScenePlayer::State::Choice) player.choose(0);
        }
        session->setForceSkip(false);
        for (uint64_t n = 0; player.state() != cs2::ScenePlayer::State::Ended && waitingAt() < opt.skipToInput && n < 200000; ++n) {
            if (waitingAt() >= 0 && n % 4 == 0) {
                session->pointerMove(640, 400);
                session->mouseButton(1, true, 640, 400);
                session->mouseButton(1, false, 640, 400);
            }
            session->step();
            if (player.state() == cs2::ScenePlayer::State::Choice) player.choose(0);
        }
        for (auto& c : opt.clicks) c.frame += session->frameNumber();  // relative to the skip's end
        std::printf("skipped to %s input %d at frame %llu\n", player.sceneName().c_str(), player.inputIndex(),
                    static_cast<unsigned long long>(player.frame()));
    }
    if (catSystem2 && opt.skipTo) {
        // Skip quickly to the requested line, then continue normally (captures start from there).
        auto& player = catSystem2->game().player();
        session->setForceSkip(true);
        // Lines count in the starting scene: the scenes it calls (`call sub_getattm2`) have their own.
        const std::string start = player.sceneName();
        while ((player.sceneName() != start || player.lineIndex() < opt.skipTo) &&
               player.state() != cs2::ScenePlayer::State::Ended) {
            session->step();
            if (opt.mix) {  // a frame's worth of sound, so sounds end and fades run in time
                static std::vector<float> silent(48000 / 60 * 2);
                mixer.render(silent.data(), 48000 / 60);
            }
            if (player.state() == cs2::ScenePlayer::State::Choice) player.choose(0);
        }
        session->setForceSkip(false);
        std::printf("skipped to %s:%zu at frame %llu\n", player.sceneName().c_str(), player.lineIndex(),
                    static_cast<unsigned long long>(player.frame()));
        if (std::getenv("NYAN_STAGE_TRACE")) {
            for (const auto& [n, p] : player.stage().planes) {
                std::printf("  plane %d: visible %d alpha %d pos %d,%d\n", n, p.visible, p.alpha.value(player.frame()),
                            p.x.value(player.frame()), p.y.value(player.frame()));
            }
            for (size_t l = 0; l < cs2::kLayerCount; ++l) {
                for (const auto& [i, img] : player.stage().layers[l]) {
                    std::printf("  layer %s %d: %s at %d,%d scale %d alpha %d visible %d plane %d\n",
                                cs2::toString(static_cast<cs2::Layer>(l)), i, img.spec.c_str(), img.x.value(player.frame()),
                                img.y.value(player.frame()), img.scaleX.value(player.frame()), img.alpha.value(player.frame()),
                                img.visible, img.plane);
                }
            }
            for (const auto& [n, p] : player.stage().planes) {
                std::printf("  plane %d: pos %d,%d base %d,%d alpha %d visible %d scale %.2f,%.2f rot %.1f/%.1f/%.1f size %dx%d fill %d motions %zu shape '%s' vram %d,%d\n",
                            n, p.x.value(player.frame()), p.y.value(player.frame()), p.baseX, p.baseY,
                            p.alpha.value(player.frame()), p.visible, p.scaleX, p.scaleY, p.rotX, p.rotY, p.rotation,
                            p.width, p.height, p.fill, p.motions.size(), p.shape.c_str(), p.vramX, p.vramY);
            }
        }
    }

    GLuint capFbo = 0, capTex = 0;
    if (capture) {
        std::filesystem::create_directories(opt.captureDir);
        glGenTextures(1, &capTex);
        glBindTexture(GL_TEXTURE_2D, capTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capW, capH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &capFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, capFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capTex, 0);
    }
    std::unique_ptr<PulseOutput> audioOut;
    if (!capture) audioOut = std::make_unique<PulseOutput>(mixer);

    bool running = true;
    uint64_t shots = 0;
    float mouseX = 0, mouseY = 0;
    uint32_t dialogSerial = 0;
    std::string pageKey;    // --pages: the input waited at, and when its message was shown
    uint64_t pageFrame = 0;
    bool pageDone = false;  // captured and clicked; the next page is a new key
    uint64_t pagesSaved = 0;
    uint64_t dialogFrame = 0;
    size_t nextAnswer = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            float density = SDL_GetWindowPixelDensity(window);
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_MOUSE_MOTION:
                mouseX = ev.motion.x * density;
                mouseY = ev.motion.y * density;
                session->pointerMove(mouseX, mouseY);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                int button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 0;
                if (button) {
                    session->mouseButton(button, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, ev.button.x * density,
                                         ev.button.y * density);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.y != 0) session->wheel(ev.wheel.y > 0 ? 1 : -1);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = ev.type == SDL_EVENT_KEY_DOWN;
                SDL_Keycode k = ev.key.key;
                if (down && k == SDLK_F11) {
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                    break;
                }
                if (down && ev.key.repeat && k != SDLK_RETURN) break;
                if (int vk = virtualKey(k)) session->key(vk, down);
                break;
            }
            default: break;
            }
        }

        // Dialogs the game opens (a window of their own on PC): answered from --answer after
        // half a second, so captures show the screen behind them.
        if (auto d = session->dialog()) {
            if (d->serial != dialogSerial) {
                dialogSerial = d->serial;
                dialogFrame = session->frameNumber();
                if (d->input) std::printf("dialog: input \"%s\" [%s] max %d%s\n", d->title.c_str(), d->value.c_str(), d->maxChars, d->cancel ? "" : " no cancel");
                else std::printf("dialog: message %d \"%s\" %s\n", d->type, d->title.c_str(), d->message.c_str());
            } else if (session->frameNumber() >= dialogFrame + 30) {
                std::string a = nextAnswer < opt.answers.size() ? opt.answers[nextAnswer++] : "ok";
                bool ok = !a.starts_with("no");
                std::string text = a.size() > 3 && a[2] == ':' ? a.substr(3) : d->value;
                std::printf("dialog answered: %s %s\n", ok ? "ok" : "no", d->input ? text.c_str() : "");
                session->answerDialog(ok, text);
            }
        }

        if (capture) {
            for (const auto& c : opt.clicks) {
                if (c.frame == session->frameNumber()) {
                    // Capture target is the virtual size, so virtual == framebuffer pixels.
                    if (c.button == -2 || c.button == -3) {
                        session->key(c.vk, c.button == -2);
                    } else if (c.button == -1) {
                        session->wheel(c.vk);
                    } else if (c.button == 0) {
                        session->key(c.vk, true);
                        session->key(c.vk, false);
                    } else if (c.button <= -4) {
                        session->pointerMove(c.x, c.y);
                        if (c.button != -5) session->mouseButton(1, c.button == -4, c.x, c.y);
                    } else {
                        session->pointerMove(c.x, c.y);
                        session->mouseButton(c.button, true, c.x, c.y);
                        session->mouseButton(c.button, false, c.x, c.y);
                    }
                }
            }
            session->step();
            if (opt.mix) {
                static std::vector<float> silent(48000 / 60 * 2);
                mixer.render(silent.data(), 48000 / 60);
            }
            // Unattended runs take the first choice; otherwise choices wait for a (scripted) click.
            if (catSystem2 && (opt.autoAdvance || opt.pages)) {
                auto& player = catSystem2->game().player();
                if (player.state() == cs2::ScenePlayer::State::Choice) player.choose(0);
            }
            uint64_t f = session->frameNumber();
            session->renderFrame(capW, capH, capFbo);
            // NYAN_CAPTURE_FROM=<frame>: frames before it are not saved (a transition frame by frame).
            static const uint64_t captureFrom = std::getenv("NYAN_CAPTURE_FROM") ? std::strtoull(std::getenv("NYAN_CAPTURE_FROM"), nullptr, 10) : 0;
            if (opt.captureEvery && f % opt.captureEvery == 0 && f >= captureFrom) {
                char name[64];
                std::snprintf(name, sizeof(name), "/frame_%07llu.png", static_cast<unsigned long long>(f));
                glBindFramebuffer(GL_FRAMEBUFFER, capFbo);
                savePng(opt.captureDir + name, capW, capH);
                ++shots;
            }
            if (catSystem2 && opt.pages) {
                auto& player = catSystem2->game().player();
                if (!player.inputReady()) {
                    pageKey.clear();
                    pageDone = false;
                } else {
                    char key[96];
                    std::snprintf(key, sizeof(key), "%s_%04d", player.sceneName().c_str(), player.inputIndex());
                    if (key != pageKey) {
                        pageKey = key;
                        pageFrame = f;
                        pageDone = false;
                    } else if (!pageDone && f >= pageFrame + opt.settle) {
                        std::string path = opt.captureDir + "/" + pageKey + ".png";
                        for (int n = 2; std::filesystem::exists(path); ++n) {
                            path = opt.captureDir + "/" + pageKey + "-" + std::to_string(n) + ".png";
                        }
                        glBindFramebuffer(GL_FRAMEBUFFER, capFbo);
                        savePng(path, capW, capH);
                        std::printf("page %s at frame %llu\n", pageKey.c_str(), static_cast<unsigned long long>(f));
                        pageDone = true;
                        // The PC harness clicks at 640,400 of 1280x720: above the message window.
                        float x = capW * 0.5f, y = capH * 400.0f / 720.0f;
                        session->pointerMove(x, y);
                        session->mouseButton(1, true, x, y);
                        session->mouseButton(1, false, x, y);
                        if (++pagesSaved >= opt.pages) running = false;
                    }
                }
            }
            if (opt.maxFrames && f >= opt.maxFrames) running = false;
            if (session->quitRequested()) {
                std::printf("game quit at frame %llu\n", static_cast<unsigned long long>(f));
                running = false;
            }
        } else {
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            session->frame(w, h);
            SDL_GL_SwapWindow(window);
            if (session->quitRequested()) running = false;
        }
    }
    if (capture) std::printf("captured %llu frames\n", static_cast<unsigned long long>(shots));

    session.reset();
    audioOut.reset();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
