#include "nyan/wa2/game.hpp"

#include <cstdlib>
#include <cstring>

#include "stb_image.h"
#include <filesystem>
#include <fstream>

namespace nyan::wa2 {

namespace {

// _mbstok over Global.vrb's separators: empty tokens are skipped (0x452880).
std::vector<std::string> tokens(std::string_view s, std::string_view delims) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<uint8_t>(s[i]);
        bool lead = (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
        if (lead && i + 1 < s.size()) {
            cur += s[i];
            cur += s[++i];
            continue;
        }
        if (delims.find(s[i]) != std::string_view::npos) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur += s[i];
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

}  // namespace

Game::Game(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer)
    : assets_(std::move(folder)),
      soundOut_(std::make_unique<SoundOut>(assets_, mixer)),
      sound_(std::make_unique<Sound>(*this, *soundOut_)) {
    for (const char* name : {"LFScriptFunc.fnc", "LFScriptFuncEx.fnc"}) {
        auto d = assets_.read("script", name);
        if (!d) fail(std::string(name) + ": not found");
        functions_.load(*d);
    }
    registerNatives();
    gfx_.drawAni = [this](const SpriteObject& obj, const Gfx::DrawObject& draw) { ani_.draw(obj, draw); };
    scene_.onTransitionEnd = [this](bool keepChars) { effects_.transitionEnded(keepChars); };
    if (auto d = assets_.read("", "todokanai/subtitles")) {
        subtitles.load(*d);
        // The overlay's font (todokanai/font.png, 458 x 159) in slot 0x1c8, which the game leaves free.
        if (auto png = assets_.read("", "todokanai/font.png")) {
            int w = 0, h = 0, n = 0;
            if (stbi_uc* px = stbi_load_from_memory(png->data(), static_cast<int>(png->size()), &w, &h, &n, 4)) {
                RgbaImage img{w, h, Bytes(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4)};
                stbi_image_free(px);
                gfx_.loadPixels(0x1c8, std::move(img));
                gfx_.overlay = [this](const Gfx::DrawObject& draw) {
                    if (!subtitles.text().empty()) Subtitles::draw(subtitles.text(), 0x1c8, draw);
                };
            }
        }
    }
    // The patch's hooks on the SE and voice play functions.
    sound_->onTrigger = [this](int key, int type) {
        subtitles.trigger(key, type, sound_->sceneOfVi(), currentLine, milliseconds());
    };
}

Game::~Game() = default;

void Game::log(const std::string& message) {
    if (logSink) logSink(message);
}

std::optional<Bytes> Game::readScriptFile(const std::string& group, const std::string& name) {
    return assets_.read(group, name);
}

void Game::loadGlobals() {
    // FUN_00452880: type, name, value per record; types 1 byte, 2 short, 3 int, 4 float, 5 string
    // (a value starting with '0' is the empty string).
    globals_.clear();
    auto d = assets_.read("script", "Global.vrb");
    if (!d) return log("Global.vrb: not found");
    auto t = tokens(std::string_view(reinterpret_cast<const char*>(d->data()), d->size()), ",\n\r");
    for (size_t i = 0; i + 2 < t.size(); i += 3) {  // only complete records count
        GlobalVar g;
        g.type = std::atoi(t[i].c_str());
        g.name = t[i + 1];
        const std::string& v = t[i + 2];
        switch (g.type) {
        case 1: g.i = static_cast<uint8_t>(std::atol(v.c_str())); break;
        case 2: g.i = static_cast<int16_t>(std::atol(v.c_str())); break;
        case 3: g.i = static_cast<int32_t>(std::atol(v.c_str())); break;
        case 4: g.f = static_cast<float>(std::atof(v.c_str())); break;
        case 5: g.str = v[0] == '0' ? std::string() : v; break;
        }
        globals_.push_back(std::move(g));
    }
    // Then GlobalDebug.dat: a short record count, a short switch (also kept for the debug DLL), and
    // records that set globals' values by name (0x18 bytes: type, int, float, ..., name length; the
    // name; a string's value). The records apply only when the switch is set: WA2's file has none,
    // Special Contents' one (one ending's flag set to 123) is switched off. Applying them is not
    // ported, so a file that would is reported.
    if (auto dbg = assets_.read("script", "GlobalDebug.dat"); dbg && dbg->size() >= 4) {
        auto count = static_cast<int16_t>((*dbg)[0] | ((*dbg)[1] << 8));
        auto on = static_cast<int16_t>((*dbg)[2] | ((*dbg)[3] << 8));
        if (count > 0 && on != 0) log("GlobalDebug.dat: " + std::to_string(count) + " global values not applied");
    }
}

void Game::loadFonts() {
    // 本体80.tga, 袋影80.tga, 14pt本体.tga, 14pt袋.tga from fnt (0x418560's first frame, the title, the
    // story start); Nyanroid keeps them loaded once read.
    if (gfx_.slots[0x1c4].loaded) return;
    static const char* kFonts[4] = {"\x96\x7b\x91\xcc""80.tga", "\x91\xdc\x89\x65""80.tga",
                                    "14pt\x96\x7b\x91\xcc.tga", "14pt\x91\xdc.tga"};
    for (int i = 0; i < 4; ++i) {
        if (!gfx_.load(0x1c4 + i, kFonts[i], 0x805, false, "", "", 0x100, "fnt")) noteMissing(std::string("font ") + kFonts[i]);
    }
}

void Game::startStory(const std::string& script, int label) {
    // 0x4178b0 and the scene's first frame (0x413c10: render targets and window pictures).
    loadFonts();
    // 0x449020(0): every picture, object and animation released.
    effects_.reset();
    gfx_.reset();
    ani_.reset();
    message_.reset();
    scene_.reset();
    choiceOptions.clear();
    visibleChoices.clear();
    choiceShown = false;
    choiceResult = choicePick = -1;
    scene_.begin();
    message_.begin();
    vm_ = std::make_unique<ScriptVm>(*this, "script");
    loadGlobals();
    titleRequested_ = false;
    mode_ = Mode::Story;
    storyPhase_ = 4;
    if (!vm_->load(script)) return;
    if (!vm_->start(label)) log("no label " + std::to_string(label) + " in " + script);
    enterScript(script);
    lastScript_ = script;
}

void Game::goTitle() {
    if (assets_.build() == Build::Special) {
        // Special Contents (0x409840): only the request; the story's frame fades the screen out over
        // 16 frames first, then cleans up (0x413630, 0x4134d0).
        storyPhase_ = 2;
        titleFade_ = 0;
        return;
    }
    // 0x413ac0
    storyPhase_ = 2;
    goTitleCleanup();
}

void Game::goTitleCleanup() {
    titleRequested_ = true;
    if (assets_.build() == Build::Special) {
        // 0x4134d0 closes the window and drops its text first (0x403780), and ends the frame.
        message_.closeWindow(0, 0);
        message_.releaseText();
        runEx = 1;
    }
    message_.novelMode(0);      // 0x403400(0)
    effects_.reset();           // the shake, the free pictures, the weather
    sound_->stopAll();          // the effects, the BGM, the voices
    gfx_.reset();               // 0x449020(0)
    ani_.reset();
    scene_.reset();             // colour levels, the tone, the tint, object 1 black over the screen
    message_.closeWindow(0, 0); // 0x403240
    // 0x404d10: the choice's objects and state.
    choiceOptions.clear();
    visibleChoices.clear();
    choiceShown = false;
    choiceResult = choicePick;
    modes.autoMode = false;
    modes.demo = 0;
    modes.skip = false;
}

bool Game::canGoTitle() const {
    // The window menu's item is enabled by the window's frame (0x40a180) while 0x409e00 finds the scene
    // at rest; Nyanroid asks only that the story runs (Fitted).
    return mode_ == Mode::Story && storyPhase_ == 4 && !titleRequested_ && !movieActive_;
}

void Game::boot() {
    // 0x418560's first frame: the glyph atlases, Sys.sav (setSaveDirectory), the pattern-fade shaders
    // (the renderer's), bitmap.tga, the colour levels; then the title (a script named on the command
    // line would start instead: the desktop app's scene argument calls startStory).
    loadFonts();
    gfx_.load(0x1ba, "bitmap.tga", 4, false, "", "", 0x100, "");
    lastScript_.clear();
    scene_.bgLevel = scene_.charLevel = 0x100;
    mode_ = Mode::Title;
}

std::string Game::currentScript() const {
    return vm_ ? vm_->innermost().name() : std::string();
}

void Game::tick() {
    ++frame_;
    // The text records' flags come from the frame draw; without a renderer, a draw-less pass.
    if (!gfx_.takeDrawn()) gfx_.layoutTexts();
    // 0x418560: the input poll (0x44a600 / 0x44a350), the .ani tick, then the state's work.
    input.poll();
    updateKeys();
    modes.forceSkip = keys.forceSkip;
    ani_.tick(kTicksPerSecond);
    switch (mode_) {
    case Mode::Boot:
        boot();
        break;
    case Mode::Title: {
        int next = title_.step();
        if (next != -1) mode_ = static_cast<Mode>(next);
        break;
    }
    case Mode::Story:
        storyFrame();
        break;
    case Mode::Quit:
        break;
    }
    // 0x40fb80: the sound channels' frame. Then the patch's overlay, which steps once per EndScene.
    sound_->update();
    subtitles.step(milliseconds());
    soundOut_->advanceFrame();
}

void Game::storyFrame() {
    // 0x418560 state 3: the VM's run, unless 0x413c10 returned 1 (a menu) last frame; when the innermost
    // script's name changes, 0x40e960.
    if (!vmHeld_ && vm_ && !titleRequested_) {
        if (vm_->run() == ScriptVm::Running) {
            std::string now = currentScript();
            if (toLowerAscii(now) != toLowerAscii(lastScript_)) enterScript(now);
            lastScript_ = now;
        }
    }
    // 0x413c10: its state's work, then the scene's animators in the engine's order.
    int result = 0;
    switch (storyPhase_) {
    case 1:
        // A save loaded from the title's load screen: the story's pictures, then 0x424310.
        effects_.reset();
        gfx_.reset();
        ani_.reset();
        message_.reset();
        scene_.reset();
        scene_.begin();
        message_.begin();
        storyPhase_ = 4;
        loadSave(saveMenu_.slot());
        break;
    case 2:
        if (assets_.build() == Build::Special) {
            // 0x413630: the screen darkens over 16 frames while the window keeps its frame; then the
            // clean-up and the title.
            ++titleFade_;
            int t = (0x10 - titleFade_) * 8;
            gfx_.setTint(t, t, t, 0);
            if (titleFade_ > 15) {
                runEx = 1;
                if (titleFade_ > 16) {
                    titleFade_ = 0;
                    goTitleCleanup();
                    storyPhase_ = 3;
                }
            }
            message_.update();
            break;
        }
        storyPhase_ = 3;
        break;
    case 3:
        storyPhase_ = 0;
        result = 2;
        break;
    case 5:
        result = 1;
        optionsFrame();
        break;
    case 6:
        result = 1;
        saveMenu_.step();
        break;
    case 7:
        result = 1;
        saveMenu_.stepQuick();
        break;
    default:
        message_.update();
        break;
    }
    effects_.calendarStep();
    effects_.shakeStep();
    scene_.transitionStep();
    scene_.toneStep();
    effects_.weatherStep();
    scene_.charStep();
    effects_.picturesStep();
    vmHeld_ = result == 1;
    if (result == 2) mode_ = Mode::Title;
}

bool Game::sceneAtRest() const {
    // 0x409e00
    bool still = !scene_.transitioning() && !scene_.toneActive() && !effects_.shaking();
    bool choice = !message_.choicesShown() || message_.choicePhase() == 0;
    return still && scene_.charsIdle() && choice && message_.windowExists() && message_.state() == 3;
}

void Game::openOptions() {
    options_.open(0);  // 0x419140(0)
}

void Game::optionsFrame() {
    options_.step();  // 0x41f800(0)
}

void Game::loadSettings() {
    if (saveDir_.empty()) return;
    std::ifstream in(std::filesystem::path(saveDir_) / "settings.ini");
    if (!in) return;
    std::string line;
    auto& c = message_.config;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        int v = std::atoi(line.c_str() + eq + 1);
        if (key == "wheel") settings.wheel = v;
        else if (key == "wait") settings.speed = static_cast<float>(v) * 0.5f;
        else if (key == "msg_wait") c.speed = static_cast<float>(v) * 0.5f;
        else if (key == "msg_cut_optin") modes.skipUnread = (v & 1) != 0;
        else if (key == "all_sound") settings.allSound = v;
        else if (key == "bgm") settings.bgm = v;
        else if (key == "se") settings.se = v;
        else if (key == "voice") settings.voice = v;
        else if (key == "all_vol") settings.allVol = v;
        else if (key == "bgm_vol") settings.bgmVol = v;
        else if (key == "se_vol") settings.seVol = v;
        else if (key == "voice_vol") settings.voiceVol = v;
        else if (key == "mov_lv") settings.movieLevel = v;
        else if (key == "ero_voice") settings.eroVoice = v;
        else if (key == "page_voice") settings.pageVoice = v;
        else if (key.rfind("char_voice", 0) == 0 && key.size() == 11 && key[10] >= '0' && key[10] <= '9')
            settings.charVoice[static_cast<size_t>(key[10] - '0')] = v;
        else if (key == "auto_max") c.autoMax = v;
        else if (key == "win_color") c.winColor = v;
        else if (key == "win_alpha") c.winAlpha = v;
        else if (key == "win_alpha_vis") c.winAlphaVis = v;
        else if (key == "win_alpha_novel") c.winAlphaNovel = v;
        else if (key == "win_alpha_command") c.winAlphaCommand = v;
        else if (key == "win_disp") c.winDisp = v;
        else if (key == "yes_no") settings.confirm = v;
        else if (key == "R_click") c.rightClick = v;
    }
}

void Game::saveSettings() {
    if (saveDir_.empty()) return;
    const auto& c = message_.config;
    std::string out = "[SYSTEM]\n";
    auto put = [&](const char* key, int v) { out += std::string(key) + "=" + std::to_string(v) + "\n"; };
    put("wheel", settings.wheel);
    put("wait", static_cast<int>(settings.speed + settings.speed));
    put("msg_wait", static_cast<int>(c.speed + c.speed));
    put("msg_cut_optin", modes.skipUnread ? 1 : 0);
    put("all_sound", settings.allSound);
    put("bgm", settings.bgm);
    put("se", settings.se);
    put("voice", settings.voice);
    put("all_vol", settings.allVol);
    put("bgm_vol", settings.bgmVol);
    put("se_vol", settings.seVol);
    put("voice_vol", settings.voiceVol);
    put("mov_lv", settings.movieLevel);
    put("ero_voice", settings.eroVoice);
    put("page_voice", settings.pageVoice);
    for (int i = 0; i < 10; ++i) put(("char_voice" + std::to_string(i)).c_str(), settings.charVoice[static_cast<size_t>(i)]);
    put("auto_max", c.autoMax);
    put("win_color", c.winColor);
    put("win_alpha", c.winAlpha);
    put("win_alpha_vis", c.winAlphaVis);
    put("win_alpha_novel", c.winAlphaNovel);
    put("win_alpha_command", c.winAlphaCommand);
    put("win_disp", c.winDisp);
    put("yes_no", settings.confirm);
    put("R_click", c.rightClick);
    std::error_code ec;
    std::filesystem::create_directories(saveDir_, ec);
    auto path = std::filesystem::path(saveDir_) / "settings.ini";
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << out;
        if (!f) return log("settings.ini: cannot write");
    }
    std::filesystem::rename(tmp, path, ec);
}

int Game::globalInt(const std::string& name) const {
    for (const GlobalVar& g : globals_) {
        if (g.name == name) return g.type == 4 ? static_cast<int>(g.f) : g.i;
    }
    return 0;
}

int Game::musicIndex(int track) {
    // 0x428210 over 0x4c1dd0
    static constexpr int kOrder[64] = {4,  1,  2,  3,  5,  6,  18, 7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 27, 33, 32, 28,
                                       19, 20, 21, 26, 29, 23, 24, 22, 34, 60, 31, 40, 41, 42, 43, 44, 45, 46, 47, 69, 74, 52,
                                       53, 61, 50, 51, 62, 63, 67, 68, 70, 71, 54, 55, 64, 58, 59, 66, 56, 57, 65, 0};
    for (int i = 0; i < 64; ++i) {
        if (kOrder[i] == track) return i;
    }
    return 64;
}

void Game::updateKeys() {
    using I = Input;
    Keys k;
    bool enterOrLeft = input.held(I::Enter) || input.buttonHeld(I::LeftButton);
    // Alt+Enter is the window's full-screen switch, not an advance.
    bool enterPressed = input.pressed(I::Enter) && !input.held(I::Alt);
    k.advanceRepeat = input.repeat(I::Enter) || input.buttonRepeat(I::LeftButton);
    k.advance = enterPressed || input.buttonPressed(I::LeftButton);
    k.cancel = input.pressed(I::Escape) || input.pressed(I::Back) || input.buttonPressed(I::RightButton);
    k.space = input.pressed(I::Space);
    bool skipHeld = input.held(I::Ctrl) || input.buttonHeld(I::MiddleButton);
    k.shift = input.pressed(I::Shift);
    bool end = input.pressed(I::End) && !enterOrLeft;
    bool home = input.pressed(I::Home) && !enterOrLeft;
    int wheel = input.wheel();
    if (wheel > 0) {
        k.scrollUp = !enterOrLeft;
        k.scrollDown = false;
    } else if (wheel < 0) {
        k.scrollUp = false;
        k.scrollDown = !enterOrLeft;
    } else {
        k.scrollUp = input.repeat(I::PageUp) && !enterOrLeft;
        k.scrollDown = input.repeat(I::PageDown) && !enterOrLeft;
    }
    k.forceSkip = skipHeld;
    // Advance or cancel with the skip key held: cancel wins over advance, and the frame does not skip.
    if (k.advance || k.cancel) {
        if (k.advance && k.cancel) k.advance = false;
        if (skipHeld) k.shift = false, k.forceSkip = false;
    }
    k.home = home;
    k.end = end;
    if (end && home) k.home = true, k.end = false;
    if (k.scrollUp && k.scrollDown) k.scrollDown = false;
    k.up = input.repeat(I::Up);
    k.down = input.repeat(I::Down);
    k.left = input.repeat(I::Left);
    k.right = input.repeat(I::Right);
    for (int d = 0; d < 10; ++d) k.digits[static_cast<size_t>(d)] = input.pressed(I::Digit0 + d) || input.pressed(I::Num0 + d);
    keys = k;
}

int Game::callNative(ScriptVm& vm, int id, CallContext& ctx) {
    if (id >= 0 && id < static_cast<int>(natives_.size()) && natives_[static_cast<size_t>(id)]) {
        return (this->*natives_[static_cast<size_t>(id)])(vm, ctx);
    }
    const ScriptFunction* f = functions_.find(id);
    return unhandledNative(f ? f->name : "function " + std::to_string(id));
}

int Game::unhandledNative(const std::string& name) {
    ++unhandled_[name];
    return 0;
}

bool Game::skipActive() {
    // GetSkip (0x40ad50).
    if (modes.demo != 0) return false;
    if (modes.frameOverride != 0) {
        modes.skip = false;
        return false;
    }
    if (!modes.skipUnread) {
        if (!modes.forceSkip && !modes.skip) return false;
        return currentTextRead() != 0;
    }
    return modes.forceSkip || modes.skip;
}

bool Game::skipActive2() {
    // GetSkip2 (0x40adb0).
    if (modes.demo == 0) {
        if (modes.frameOverride != 0) {
            modes.skip = false;
            return false;
        }
        if (modes.skipUnread) return modes.forceSkip || modes.skip;
    }
    return (modes.forceSkip || modes.skip) && currentTextRead() != 0;
}

int Game::fr(int n) {
    bool skip = skipActive();
    if (modes.demo != 0) return frP(n);
    if (skip) return 0;
    float speed = settings.speed;
    float frames = n == -1 ? 15.0f : n == -2 ? 30.0f : static_cast<float>(n);
    return static_cast<int>(frames * speed * kTicksPerSecond / 60.0f);
}

int Game::frT(int n) {
    if (skipActive()) return 0;
    return frP(n);
}

int Game::frP(int n) {
    if (n == -1) n = 15;
    if (n == -2) n = 30;
    return kTicksPerSecond * n / 60;
}

int Game::currentTextRead() {
    // 0x405250: the bit of the line after the current one, or a replay.
    if (currentLine == -1) return 0;
    if (system.readBit(scriptIndexNow, currentLine + 1) || modes.replay != 0) return 1;
    return ifSkip != 0 ? 2 : 0;
}

void Game::enterScript(const std::string& name) {
    scriptName = name;
    scriptNumber = std::atoi(name.c_str());
    chapterDigit = name.empty() ? 0 : name[0] - '0';
    int previous = scriptIndexNow;
    int index = scriptIndex(name);
    if (index == previous) return;
    // catch, catch2, catch3 (indices 0x1d..0x1f) mark even at line 0.
    if (currentLine != 0 || previous == 0x1d || previous == 0x1e || previous == 0x1f) markLine(currentLine + 1);
    scriptIndexNow = index;
    currentLine = 0;
}

void Game::markLine(int line) {
    if (line > 0xfff) {
        log("message line " + std::to_string(line) + " past the read-text table");
    } else {
        system.setReadBit(scriptIndexNow, line);
        if (line % 100 == 0) writeSystem();
        currentLine = line;
    }
    // The patch's hook on this function: the story reached a subtitle's end message.
    subtitles.messageMarked(sound_->sceneOfVi(), currentLine);
}

void Game::setSaveDirectory(const std::string& dir) {
    saveDir_ = dir;
    if (dir.empty()) return;
    loadSettings();
    std::ifstream in(std::filesystem::path(dir) / "system.nsv", std::ios::binary);
    if (!in) return;
    Bytes file((std::istreambuf_iterator<char>(in)), {});
    if (auto d = SystemData::decode(file)) system = std::move(*d);
    else log("system.nsv: unreadable, starting afresh");
}

void Game::writeSystem() {
    if (saveDir_.empty() || !system.dirty) return;
    std::error_code ec;
    std::filesystem::create_directories(saveDir_, ec);
    auto path = std::filesystem::path(saveDir_) / "system.nsv";
    auto tmp = path;
    tmp += ".tmp";
    Bytes data = system.encode();
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return log("system.nsv: cannot write");
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return log("system.nsv: " + ec.message());
    system.dirty = false;
}

}  // namespace nyan::wa2
