#include "nyan/cs2/scene_player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <cctype>

#include "nyan/common/sjis.hpp"
#include "nyan/cs2/scene_text.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "nyan/cs2/emote_inertia.hpp"
#include "nyan/cs2/emote_player.hpp"
#include "nyan/cs2/hg_image.hpp"
#include "nyan/cs2/particle.hpp"
#include "nyan/cs2/fes_runtime.hpp"
#include "nyan/cs2/save_data.hpp"

namespace nyan::cs2 {
namespace {

constexpr uint32_t kMaxLinesPerTick = 200000;

std::vector<std::string> splitArgs(std::string_view text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (i > start) out.emplace_back(text.substr(start, i - start));
    }
    return out;
}

// Characters in a message line's raw text, as sscript's native 47 (0x510380) counts them: a
// Shift-JIS double-byte character is one (a UTF-8 script: one per code point).
size_t countChars(std::string_view raw, bool utf8) {
    size_t n = 0;
    for (size_t i = 0; i < raw.size(); ++n) {
        auto b = static_cast<uint8_t>(raw[i]);
        if (utf8) {
            ++i;
            while (i < raw.size() && (static_cast<uint8_t>(raw[i]) & 0xC0) == 0x80) ++i;
        } else {
            i += ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) ? 2 : 1;
        }
    }
    return n;
}

bool isNumberLike(const std::string& s) {
    return !s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-' || s[0] == '#' ||
                          s[0] == '(' || s[0] == '@');
}

std::optional<Layer> layerFromWord(const std::string& w) {
    if (w == "bg") return Layer::Bg;
    if (w == "cg") return Layer::Cg;
    if (w == "eg") return Layer::Eg;
    if (w == "fg") return Layer::Fg;
    if (w == "fw") return Layer::Fw;
    return std::nullopt;
}

// Image sub-commands that take the place of an image name (`cg 0 blend 255`): sscript looks the word
// up in its image keyword list (Happiness 2's has 145 words, m[0x4cda4]; every other game's list is
// part of it) and loads an image only for a word it does not know. A word missing here loaded an
// image of that name over the slot's: Happiness 2's `attpos` lines emptied the speaker's portrait
// at every emotion mark.
bool isImageSubcommand(const std::string& w) {
    static const char* kWords[] = {
        "blend", "disp", "fade", "fade2", "move", "amove1", "amove2", "amove3", "attr", "shake", "quake",
        "scale", "mscale", "vscale", "mvscale", "rot", "mrot", "rotc", "mrotc", "rot_ypr", "mrot_ypr", "mode",
        "pri", "base", "vram", "mosaic", "sepia", "mono", "tone", "blur", "color", "spline", "spline_r",
        "spline_s", "arc", "vmove", "pend", "delay", "anm", "anmset", "anmframe", "attach", "clone", "camera",
        "texture", "billboard_y", "persmode", "raster", "spot", "fog", "stop", "loop", "frameon", "frameoff",
        "pos", "ascale1", "ascale2", "ascale3",
        // The rest of the engine's list.
        "actcnv", "action", "after", "arot1", "arot2", "arot3", "attpos", "attr_def", "autosave", "bmode",
        "cfade", "command", "def", "end", "lipbank", "loc", "mrotrep", "mrotrepc", "packpl", "pause", "pl",
        "plmode", "replay", "shift", "skip", "topmask", "touch", "vcom", "veff", "video", "vol", "wait",
    };
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    // Continuous (mc*) motion variants, and m2*: the plain motion started a frame later (sscript
    // passes 1 instead of 0 as native 535's last argument, the motion's start time, cs2.exe
    // 0x725f20 / 0x725da5; not modelled).
    return w.size() > 2 && ((w.starts_with("mc") && w != "mc") || w.starts_with("m2"));
}

// Subcommands the engine runs as motions, which an object's `delay` holds back (sscript.kcs passes
// the delay to every motion it starts; these are the subcommands that do not take it).
bool startsMotion(const std::string& w) {
    static const char* const kImmediate[] = {"after",  "anm",   "attach", "attpos",  "attr",   "attr_def", "blur",
                                             "bmode",  "clone", "def",    "disp",    "mode",   "packpl",   "persmode",
                                             "plmode", "raster", "shift", "tone",    "vcom",   "delay",    "stop"};
    if (w.empty() || !std::isalpha(static_cast<unsigned char>(w[0]))) return false;
    for (const char* k : kImmediate) {
        if (w == k) return false;
    }
    return true;
}

// Where a one-shot `arc` leaves its object: motArc ends by rounding the moved position, which stays.
std::pair<int32_t, int32_t> arcEnd(const Motion& m) {
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    double a0 = m.a[0] * kRad, a1 = m.a[1] * kRad;
    return {static_cast<int32_t>(std::lround(m.a[2] * (std::cos(a1) - std::cos(a0)))),
            static_cast<int32_t>(std::lround(-m.a[3] * (std::sin(a1) - std::sin(a0))))};
}

// A one-shot `arc` on an image or a plane moves the object's position to where the arc ends, at its
// end (arcEnd: motArc leaves the moved position), so once over it adds nothing: the engine's
// finished motion is gone (sscript's per-frame step clears an object's motions once none runs,
// Happiness 2 0x08c643: native 311, then 296). Kept, it held its end offset on top of the moved
// position (the evaluator holds a finished one-shot's end, which FE objects need: they do not move
// the position), and a load's own coordinates did not clear it: in Yukikoi a character hopped with
// `arc` stayed 100 pixels high through her next images, where the PC shows her in place.
void dropFinishedArcs(Stage& st, uint64_t now) {
    auto finished = [now](const Motion& m) { return m.kind == Motion::Kind::Arc && !m.loop && !m.active(now); };
    for (auto& layer : st.layers) {
        for (auto& [i, img] : layer) std::erase_if(img.motions, finished);
    }
    for (auto& [i, p] : st.planes) std::erase_if(p.motions, finished);
}

bool isMoveWord(const std::string& w) {
    return w == "move" || w == "amove1" || w == "amove2" || w == "amove3" || w == "mcmove" || w == "m2move" ||
           w == "m2amove1" || w == "m2amove2" || w == "m2amove3" || w == "vmove" || w == "m2vmove";
}

// The easing of a move subcommand (Tween::ease): amoveN and m2amoveN run motAMoveN.
uint8_t moveEase(const std::string& w) {
    if (w.size() >= 6 && (w.starts_with("amove") || w.starts_with("m2amove")) && w.size() == (w[0] == 'a' ? 6u : 8u)) {
        char n = w.back();
        if (n >= '1' && n <= '3') return static_cast<uint8_t>(n - '0');
    }
    return 0;
}

}  // namespace

const char* toString(Layer layer) {
    static const char* kNames[] = {"bg", "cg", "eg", "fg", "fw", "mpl"};
    return kNames[static_cast<size_t>(layer)];
}

const char* toString(ScenePlayer::State s) {
    switch (s) {
    case ScenePlayer::State::Running: return "running";
    case ScenePlayer::State::Input: return "input";
    case ScenePlayer::State::WaitFrames: return "wait-frames";
    case ScenePlayer::State::WaitAnimation: return "wait-animation";
    case ScenePlayer::State::Choice: return "choice";
    case ScenePlayer::State::Movie: return "movie";
    case ScenePlayer::State::Fes: return "fes";
    case ScenePlayer::State::Ended: return "ended";
    case ScenePlayer::State::WaitKey: return "wait-key";
    case ScenePlayer::State::WaitSe: return "wait-se";
    case ScenePlayer::State::WaitPcm: return "wait-pcm";
    case ScenePlayer::State::WaitMpl: return "wait-mpl";
    case ScenePlayer::State::View: return "view";
    }
    return "?";
}

int32_t Tween::value(uint64_t now) const {
    // A change made with a `delay` (start in the future, 0 frames) holds the old value until then.
    if (now >= start + frames) return ease == 4 ? from : to;
    if (now <= start) return from;
    double t = static_cast<double>(now - start) / frames;
    if (ease == 4) t = 1 - std::fabs(2 * t - 1);
    constexpr double kPi = 3.14159265358979323846;
    if (ease == 1) t = std::sin(kPi / 2 * t);
    else if (ease == 2) t = 1 - std::cos(kPi / 2 * t);
    else if (ease == 3) t = (1 - std::cos(kPi * t)) / 2;
    return from + static_cast<int32_t>((to - from) * t);
}

ScenePlayer::ScenePlayer(const AssetIndex& assets, VarStore& vars)
    : assets_(assets), vars_(vars), expr_(vars, '#'), emoteProfiles_(EmoteProfiles::load(assets)) {}
ScenePlayer::~ScenePlayer() = default;

bool ScenePlayer::cancelToLabel() {
    // sscript loads the scene as `next` does (0x0dbd32: call 82318), the stage left as it is.
    if (cancelLabel_.empty()) return false;
    callStack_.clear();
    jump(cancelLabel_);
    return true;
}

void ScenePlayer::start(const std::string& scene) {
    keySkip_ = 1;
    skipFlag_ = frameoffHidden_ = false;
    cancelLabel_.clear();
    callStack_.clear();
    log_.clear();
    se_.clear();
    pendingVoice_.clear();
    windowEvents_.clear();
    residentCalls_.clear();
    lastChoice_.reset();
    choiceShown_.reset();
    // No return point until a choice or `undo` (sscript resets it to -1 at the start: Amakano
    // 0x0148c6; Fruit's m[0x20ea0] is only created at choices and `undo`).
    startPointPending_ = false;
    bgm_.clear();
    channelVolume_.clear();
    pending_ = Stage{};
    shown_ = Stage{};
    nextTransition_ = Transition{};
    collectingChoices_ = false;
    autoMode_ = false;
    forceMesDel_ = false;
    autoFace_ = false;
    voiceMuted_ = 0;  // a new game clears the table (REBELLIONS 0x0c9486)
    pageChars_ = 0;
    soundPos_.clear();
    messageOpen_ = false;
    jump(scene);
}

void ScenePlayer::stop() {
    pending_ = Stage{};
    shown_ = Stage{};
    nextTransition_ = Transition{};
    overlay_.reset();
    for (auto& d : displayScripts_) d.reset();
    overlayLine_ = SIZE_MAX;
    overlayChoice_ = false;
    overlayKind_ = screenOpened_ = screenClosed_ = ScreenKind::None;
    scene_.reset();
    callStack_.clear();
    bgm_.clear();
    se_.clear();
    soundUntil_.clear();
    bgmVolume_.clear();
    seVolume_.clear();
    channelVolume_.clear();
    lastChoice_.reset();
    choiceShown_.reset();
    startPointPending_ = false;
    state_ = State::Ended;
}

void ScenePlayer::jump(const std::string& scene) {
    std::string name = scene;
    if (!toLowerAscii(name).ends_with(".cst")) name += ".cst";
    auto ref = assets_.find(name);
    if (!ref) {
        if (log) log("scene not found: " + scene);
        missing(name);
        block(State::Ended);
        return;
    }
    scene_ = loadScene(*ref);
    sceneName_ = scene;
    pc_ = 0;
    pending_.choices.clear();
    collectingChoices_ = false;
    state_ = State::Running;
    if (log) log("scene: " + scene);
}

CstScene ScenePlayer::loadScene(const AssetRef& ref) {
    CstScene scene = CstScene::parse(ref.archive->read(*ref.entry));
    if (language_.empty() && subLanguage_.empty()) return scene;
    // The translation table sits next to the scene: name.cst -> name.cstl.
    std::string name = ref.entry->name + "l";
    if (auto t = assets_.find(name)) {
        try {
            SceneTranslations table = SceneTranslations::parse(t->archive->read(*t->entry));
            localizeScene(scene, table, table.languageIndex(language_),
                          subLanguage_.empty() ? -1 : table.languageIndex(subLanguage_));
        } catch (const Error& e) {
            if (log) log("bad translation table " + name + ": " + e.what());
        }
    }
    return scene;
}

void ScenePlayer::setTextLanguage(const std::string& prefix, const std::string& sub) {
    if (prefix == language_ && sub == subLanguage_) return;
    language_ = prefix;
    subLanguage_ = sub;
    // Re-read the scene on screen: translation keeps every line where it was.
    if (!scene_ || sceneName_.empty()) return;
    std::string name = sceneName_;
    if (!toLowerAscii(name).ends_with(".cst")) name += ".cst";
    if (auto ref = assets_.find(name)) scene_ = loadScene(*ref);
}

void ScenePlayer::block(State s) {
    state_ = s;
    if (log && std::getenv("NYAN_STATE_TRACE"))
        log(std::string("block ") + toString(s) + " at " + sceneName_ + ":" + std::to_string(pc_) + " f" + std::to_string(frame_));
    // The engine updates at least once before the script goes on, so what a `pos` replaced in this
    // block is no longer "the value the last update left" for the next one (a skip runs several
    // blocks in one of our frames).
    priors_.clear();
    if (s == State::Choice) choiceShown_ = capture();
    // The message's way back from the log, at its first wait (a continued `\@` message keeps it).
    if (s == State::Input && logCurrent_ && !log_.empty() && !log_.back().point) {
        if (auto r = capture()) {
            SaveFile f;
            f.resume = std::move(*r);
            log_.back().point = std::make_shared<const Bytes>(encodeSave(f, frame_));
        }
    }
    if (startPointPending_ && (s == State::Input || s == State::Choice)) {
        startPointPending_ = false;
        lastChoice_ = capture();
    }
}

void ScenePlayer::logMessage() {
    logCurrent_ = true;
    const auto& w = shown_.window;
    LogEntry e{w.name, w.text.plain(), std::exchange(pendingVoice_, std::string()), w.speaker, w.subText.plain()};
    // A continued message (`\@`, novel pages) grows the previous entry instead of adding one.
    if (!log_.empty() && !log_.back().text.empty() && e.text.starts_with(log_.back().text) && log_.back().name == e.name) {
        if (e.voice.empty()) e.voice = log_.back().voice;
        e.id = log_.back().id;
        e.point = std::move(log_.back().point);
        log_.back() = std::move(e);
        return;
    }
    e.id = nextLogId_++;
    log_.push_back(std::move(e));
    if (log_.size() > logLimit_) log_.erase(log_.begin(), log_.begin() + static_cast<long>(log_.size() - logLimit_));
}

bool ScenePlayer::waitingAtPage() const {
    return state_ == State::Input && scene_ && inputPc_ < scene_->lines().size() &&
           scene_->lines()[inputPc_].type == CstLineType::Page;
}

int32_t ScenePlayer::inputIndex() const {
    if (state_ != State::Input || !scene_) return -1;
    const auto& lines = scene_->lines();
    int32_t n = 0;
    for (size_t i = 0; i < inputPc_ && i < lines.size(); ++i) {
        if (lines[i].type == CstLineType::Input || lines[i].type == CstLineType::Page) ++n;
    }
    return n;
}

std::string ScenePlayer::stepText(int32_t step, bool* utf8) const {
    // 0x015e17: the step's lines one by one (native 326), each message line's text in the text
    // language (natives 1038 / 1033) added to the end.
    std::string out;
    if (utf8) *utf8 = false;
    if (!scene_ || step < 0) return out;
    int32_t at = 0;
    for (const auto& line : scene_->lines()) {
        if (at > step) break;
        if (at == step && line.type == CstLineType::Message) {
            out += line.text;
            if (line.utf8 && utf8) *utf8 = true;
        }
        if (line.type == CstLineType::Input || line.type == CstLineType::Page) ++at;
    }
    return out;
}

std::string ScenePlayer::messageKey() const {
    if (state_ != State::Input || !scene_) return {};
    return toLowerAscii(sceneName_) + ":" + std::to_string(inputPc_);
}

std::optional<ScenePlayer::Resume> ScenePlayer::capture() const {
    // A screen the scene runs (`fex sel mapselect`: Amakano 2's map has its own Save button) saves
    // the scene at that line: loading replays the scene to it (sscript's load, Fruit 0x094524 on)
    // and so runs the screen again.
    const bool atScreen = state_ == State::Fes && overlay_ && overlayLine_ != SIZE_MAX;
    if ((state_ != State::Input && state_ != State::Choice && !atScreen) || !scene_ || (overlay_ && !atScreen))
        return std::nullopt;
    Resume r;
    r.scene = sceneName_;
    r.pc = atScreen ? overlayLine_ : pc_;
    r.state = atScreen ? State::Running : state_;
    r.pending = pending_;
    r.shown = shown_;
    r.shown.window.revealed = r.shown.window.revealTotal();
    r.title = title_;
    r.bgm = bgm_;
    r.se = se_;
    r.bgmVolume = bgmVolume_;
    r.seVolume = seVolume_;
    r.channelVolume = channelVolume_;
    r.vars = vars_;
    r.transitionDefaults = {drawDefFrames_, drawDefPlane_, wipeDefFrames_, wipeDefPlane_};
    r.autoFace = autoFace_;
    r.voiceMuted = voiceMuted_;
    return r;
}

void ScenePlayer::load(const Resume& r, int32_t keepFrom, int32_t keepTo) {
    ++loadCount_;
    keySkip_ = 1;
    skipFlag_ = frameoffHidden_ = false;
    log_.clear();
    pendingVoice_.clear();
    windowEvents_.clear();
    residentCalls_.clear();
    lastChoice_.reset();
    choiceShown_.reset();
    startPointPending_ = false;
    autoMode_ = false;
    forceMesDel_ = false;
    pageChars_ = 0;
    soundPos_.clear();
    messageOpen_ = false;
    // Whatever plays now (title music, another story) is not this save's: always switch over.
    media_.push_back({MediaCommand::Kind::StopBgm, -1, "", false, 30});
    bgm_.clear();
    restore(r, keepFrom, keepTo);
}

bool ScenePlayer::returnToLog(uint32_t id, int32_t keepFrom, int32_t keepTo) {
    ++jumpCount_;
    auto it = std::find_if(log_.begin(), log_.end(), [&](const LogEntry& e) { return e.id == id; });
    if (id == 0 || it == log_.end() || !it->point) return false;
    SaveFile f;
    try {
        f = decodeSave(*it->point, frame_);
    } catch (const Error&) {
        return false;
    }
    log_.erase(it + 1, log_.end());
    // As a load: the scene runs on from the message (sscript replays to it in mode 2).
    keySkip_ = 1;
    skipFlag_ = frameoffHidden_ = false;
    pendingVoice_.clear();
    windowEvents_.clear();
    residentCalls_.clear();
    choiceShown_.reset();
    startPointPending_ = false;
    autoMode_ = false;
    forceMesDel_ = false;
    pageChars_ = 0;
    soundPos_.clear();
    messageOpen_ = false;
    restore(f.resume, keepFrom, keepTo);
    logCurrent_ = state_ == State::Input;  // the entry is the message on screen again
    return true;
}

void ScenePlayer::restore(const Resume& r, int32_t keepFrom, int32_t keepTo) {
    using K = MediaCommand::Kind;
    ++restoreCount_;
    overlay_.reset();
    for (auto& d : displayScripts_) d.reset();  // the slots are not saved; a load starts without them
    overlayLine_ = SIZE_MAX;
    overlayChoice_ = false;
    overlayKind_ = screenOpened_ = screenClosed_ = ScreenKind::None;
    callStack_.clear();
    jump(r.scene);
    if (state_ == State::Ended) return;
    pc_ = r.pc;
    std::map<int32_t, int32_t> kept;
    for (const auto& [i, v] : vars_.ints()) {
        if (i >= keepFrom && i < keepTo) kept[i] = v;
    }
    vars_ = r.vars;
    for (int32_t i = keepFrom; i < keepTo; ++i) {
        auto it = kept.find(i);
        vars_.set(i, it == kept.end() ? 0 : it->second);
    }
    drawDefFrames_ = r.transitionDefaults[0], drawDefPlane_ = r.transitionDefaults[1];
    wipeDefFrames_ = r.transitionDefaults[2], wipeDefPlane_ = r.transitionDefaults[3];
    autoFace_ = r.autoFace;
    voiceMuted_ = r.voiceMuted;
    // Stage tweens are timed against old frames; they are finished by now.
    pending_ = r.pending;
    shown_ = r.shown;
    shown_.transition = Transition{};
    nextTransition_ = Transition{};
    title_ = r.title;
    collectingChoices_ = false;
    state_ = r.state;
    if (state_ == State::Choice) choiceShown_ = r;
    // The restored message is the one waited at (read-text key, the auto deadline from now).
    if (state_ == State::Input && r.pc > 0) inputPc_ = r.pc - 1, inputFrame_ = frame_;
    if (state_ == State::WaitKey) waitKeyFrame_ = frame_;
    waitChannel_ = -1;
    waitSince_ = frame_;
    media_.push_back({K::StopSe, -1});
    media_.push_back({K::StopVoice, -1});
    voiceChannels_.clear();
    lipLinks_.clear();
    soundUntil_.clear();
    // Looping sound effects (rain, traffic, a running faucet) are part of the scene's state.
    for (const auto& [ch, name] : r.se) {
        media_.push_back({K::PlaySe, ch, name, true, 30});
        soundUntil_[{1, ch}] = UINT64_MAX;
        if (auto v = r.seVolume.find(ch); v != r.seVolume.end()) media_.push_back({K::SeVolume, ch, "", false, 0, v->second});
    }
    se_ = r.se;
    seVolume_ = r.seVolume;
    channelVolume_ = r.channelVolume;
    if (log) {
        std::string what;
        for (const auto& [ch, name] : r.bgm) what += " bgm" + std::to_string(ch) + "=" + name;
        for (const auto& [ch, name] : r.se) what += " se" + std::to_string(ch) + "=" + name;
        if (!what.empty()) log("restore sound:" + what);
    }
    if (r.bgm != bgm_) {
        media_.push_back({K::StopBgm, -1, "", false, 30});
        for (const auto& [ch, name] : r.bgm) {
            media_.push_back({K::PlayBgm, ch, name, true, 30});
            if (auto v = r.bgmVolume.find(ch); v != r.bgmVolume.end()) {
                media_.push_back({K::BgmVolume, ch, "", false, 0, v->second});
            }
        }
        bgm_ = r.bgm;
        soundPos_.clear();  // not kept in saves: sounds come back centred
        bgmVolume_ = r.bgmVolume;
    }
}

void ScenePlayer::tick() {
    ++frame_;
    dropFinishedArcs(pending_, frame_);
    dropFinishedArcs(shown_, frame_);
    // The images' ANM scripts run after the frame's script lines (an image loaded now starts now).
    struct AtEnd {
        ScenePlayer* p;
        ~AtEnd() { p->stepAnimations(); }
    } atEnd{this};
    stepEmotes();
    stepParticles();
    stepSoundPositions();

    // Typewriter reveal on the shown message.
    auto& w = shown_.window;
    size_t total = w.revealTotal();
    if (held_) {
        // Touch mode: the story mode doesn't run (sscript is in mode 27).
    } else if (w.revealed < total && wipe_) {
        // The wipe runs in the renderer; the text counts as shown once it is through.
    } else if (w.revealed < total) {
        updateReveal();
    }

    if (overlay_) {
        overlay_->tick(frame_);
        for (auto& m : overlay_->takeMedia()) media_.push_back(std::move(m));
        if (overlay_->exited()) {
            if (log) log("fes " + overlay_->name() + " exited with " + std::to_string(overlay_->exitCode()));
            // A `fex sel` screen is a choice: once picked, it is the Previous Option point.
            if (overlayChoice_ && choiceShown_) lastChoice_ = std::move(choiceShown_);
            overlayChoice_ = false;
            // `fex sel <script> [var]`: the screen's exit code goes into the variable (Amakano 1's
            // sscript 0x0cffc9, the same in Amakano 2 0x0dfc96, Happiness 2 0x0e0955 and Yukikoi
            // 0x0dfa4b: native 452 gives the code, and unless it is -1 native 211 stores it in the
            // variable the line's fourth word names through the calculator, 0 without one: Amakano 1
            // 0x078151 / 0x07818c). One of Amakano 1's choice screens answers only that way: yes 1,
            // no 0. The other `fex` modes (state 21) store nothing.
            if (overlayKind_ == ScreenKind::FexSel && overlay_->exitCode() != -1) vars_.set(fexVar_, overlay_->exitCode());
            screenClosed_ = std::exchange(overlayKind_, ScreenKind::None);
            // Both `fex` returns lower the click's skip flag (Yukikoi's sscript 0x0e0056 / 0x0e059d:
            // m[0x800000a0]).
            if (screenClosed_ == ScreenKind::FexSel || screenClosed_ == ScreenKind::Fex) setSkipFlag(false, "fex return");
            overlay_.reset();
            overlayLine_ = SIZE_MAX;
            overlayChoice_ = false;
            if (state_ == State::Fes) state_ = State::Running;
        }
    }
    stepDisplayScripts();

    if (held_) return;
    // The click's skip flag: a render transition up ends (on PC a click during one of Nekopara's
    // `rwipe` transitions showed the next screen at once).
    if (skipFlag_) {
        // While the flag is up, sscript's per-frame skip (0x08a765 -> 0x0b79e1) closes each running
        // render transition, effect kinds 2-8 (0x0b7101: natives 528 and 67, the record cleared):
        // `rdraw`, `rwipe` / `rwipe2`, `rwmos`, `rfmos`, `wbreak`, `rpflip`, `rcflip`. The 2D ones
        // (`draw`, `wipe`, `wipe2`) are not in that table and run on.
        for (Stage* st : {&shown_, &pending_}) {
            if (st->transition.active(frame_) && st->transition.render) st->transition.frames = 0;
        }
    }
    // Skipping and the skip flag: the classic motions end at once (setSkipping; sscript's per-frame
    // update 0x0888aa calls 0x08a765 while the flag is up), unless a render transition holds them
    // (0x0b984d reads only the render effects' records, kinds 2-8, by their phase; fitted: any
    // part of one while it runs). A 2D `draw` / `wipe` does not.
    if (((skipping_ && keySkip_ != 0) || skipFlag_) && !(shown_.transition.active(frame_) && shown_.transition.render)) {
        finishClassicMotions(pending_);
        finishClassicMotions(shown_);
    }
    // The waits with the flag up (Nekopara's per-frame checks 0x085dd6): its first use is the
    // window's drawing (MES_ISDRAW: its fade, the text still coming), which ends and takes the
    // flag with it (0x0020aa / 0x002422: MES_DRAW, MES_SKIP).
    const bool flag = skipFlag_ && keySkip_ != 0;
    switch (state_) {
    case State::WaitFrames:
        // `wait N` (0x086da6): with the flag through once nothing is drawing or moving, else at its frame.
        if (flag) {
            if (windowDrawing()) {
                finishWindowDrawing();
                setSkipFlag(false, "window drawing (wait N)");
                state_ = State::Running;
            } else if (animationsDone()) {
                state_ = State::Running;
            }
        } else if (frame_ >= waitUntil_) {
            state_ = State::Running;
        }
        break;
    case State::WaitAnimation:
        // A bare `wait` (0x085efc): until no transition or classic motion runs.
        if (flag && windowDrawing()) {
            finishWindowDrawing();
            setSkipFlag(false, "window drawing (wait)");
        }
        if (animationsDone()) state_ = State::Running;
        break;
    case State::WaitSe:
    case State::WaitPcm:
        // Checked from the frame after (sscript 0x08613f / 0x086197): the sound over, or the flag.
        if (skipFlag_ || !soundPlaying(state_ == State::WaitSe ? 1 : 2, waitChannel_)) state_ = State::Running;
        break;
    case State::View:
        stepView();
        break;
    case State::WaitMpl:
        // The first check only moves code -17 on to -18 (0x086aed), so at least two frames. The
        // flag takes every bank's animation to its end (0x0868a9 -> 0x0bc5fc: native 908).
        if (skipFlag_) {
            skipEmotes();
            state_ = State::Running;
        } else if (frame_ >= waitSince_ + 2 && !mplAnimating(waitChannel_)) {
            state_ = State::Running;
        }
        break;
    case State::Input:
        // The page reached with the flag still up (a message without text does not lower it): the
        // text shows whole and the flag goes, else the page goes on as if clicked (0x086643).
        if (skipFlag_) {
            setSkipFlag(false, "page");
            advance();
        }
        break;
    default:
        break;
    }
    if (state_ == State::Running) run();
}

bool ScenePlayer::renderTransitionUp() const {
    // sscript 0x0bce46 (Cyanotype): any of the effect records 2-8 (the render transitions) holds
    // a task, from its keyword on until the task ends; one asked for this frame counts too.
    return (shown_.transition.render && shown_.transition.active(frame_)) ||
           (nextTransition_.render && nextTransition_.kind != Transition::Kind::Cut);
}

void ScenePlayer::applyViewStyle(const std::string& style) {
    for (Stage* st : {&pending_, &shown_}) {
        st->window.style = style;
        st->window.novel = style == "novel";
    }
}

void ScenePlayer::stepView() {
    // Main state 17 (Cyanotype 0x0deba7), one step a frame from the frame after `view`: 0 notes the
    // time t0; 1 fades the window out, 255 - (now - t0) x 255 / 30, until now > t0 + 30 (then 0);
    // 2 changes the window; 3 one more call; 4 notes t1 and shows the window (MES_SHOW 1) at
    // alpha 0; 5 fades it in, (now - t1) x 255 / 30, until now > t1 + 30 (then 255); 6 hands back
    // to the story, whose next line runs the frame after. The click's skip flag ends 1 and 5 at
    // once. A window `frameoff` put away stays hidden throughout (0x000c78).
    auto fade = [&](int32_t from, int32_t to, uint64_t at, uint32_t frames) {
        for (Stage* st : {&pending_, &shown_}) {
            if (st->window.visible) st->window.alpha = {from, to, at, frames};
        }
    };
    const uint64_t now = frame_;
    switch (viewStep_) {
    case 0:  // t0
        viewSince_ = now;
        fade(shown_.window.alpha.value(now), 0, now, 30);
        viewStep_ = 1;
        break;
    case 1:
        if (skipFlag_ || now > viewSince_ + 30) {
            fade(0, 0, now, 0);
            viewStep_ = 2;
        }
        break;
    case 2:
        applyViewStyle(viewStyle_);
        viewStep_ = 3;
        break;
    case 3:
        viewStep_ = 4;
        break;
    case 4:  // t1
        viewSince_ = now;
        fade(0, 255, now, 30);
        viewStep_ = 5;
        break;
    case 5:
        if (skipFlag_ || now > viewSince_ + 30) {
            fade(255, 255, now, 0);
            viewStep_ = 6;
        }
        break;
    case 6:  // hands back: the story's next line runs the next frame
        viewStep_ = 7;
        break;
    default:
        viewStep_ = 0;
        state_ = State::Running;
        break;
    }
}

void ScenePlayer::setSkipFlag(bool on, const char* why) {
    if (skipFlag_ == on) return;
    skipFlag_ = on;
    if (log && std::getenv("NYAN_SKIP_TRACE"))
        log(std::string("skip flag ") + (on ? "up" : "down") + " (" + why + ") at line " + std::to_string(pc_) + " frame " + std::to_string(frame_));
}

bool ScenePlayer::windowDrawing() const {
    const auto& w = shown_.window;
    return !w.alpha.done(frame_) || w.revealed < w.revealTotal();
}

void ScenePlayer::finishWindowDrawing() {
    for (Stage* st : {&shown_, &pending_}) st->window.alpha.frames = 0;
    shown_.window.revealed = shown_.window.revealTotal();
}

void ScenePlayer::updateReveal() {
    // Glyph k of the text set (k from 0) is due strframe_frame x frame x k / 10 frames after it
    // was set, so the first shows at once; frame 0 shows it all (cs2.exe 0x694430 times each
    // glyph, 0x696cf0 shows those due; grisaia.exe 0x5495a0 / 0x54c2b0 the same). Text added to
    // what is shown (a continuation) counts on from the glyphs kept.
    auto& w = shown_.window;
    const size_t total = w.revealTotal();
    const int64_t per = static_cast<int64_t>(strframeTenths_) * textFrame_;
    if (textInstant_ || per <= 0) {
        w.revealed = total;
        return;
    }
    // The time counted is (elapsed x mag) / 10 (0x696cf0: unsigned, mag 10 but for `str mag`).
    const int64_t elapsed = static_cast<int64_t>(frame_ - std::min(frame_, w.wipeStart));
    const int64_t counted = (elapsed + 1) * std::max<int64_t>(0, textMag_) / 10;
    const size_t due = w.wipeFrom + static_cast<size_t>((10 * counted + per - 1) / per);
    w.revealed = std::min(total, std::max(w.revealed, due));
}

void ScenePlayer::advance() {
    auto& w = shown_.window;
    switch (state_) {
    case State::Input:
        if (w.revealed < w.revealTotal()) {
            w.revealed = w.revealTotal();
        } else {
            logCurrent_ = false;  // the message becomes history
            // Leaving a message stops every bank's lip sync, whether its voice plays on or not:
            // sscript's advance (0x0893bd..0x089434) calls 0xc8613, PlayLip(bank, 0) for the
            // E-mote banks 67-78; the next line's voice starts them again.
            for (Stage* st : {&shown_, &pending_}) {
                for (auto& [i, img] : st->layer(Layer::Mpl)) img.emoteVoice.clear();
            }
            // And with contvoice off every voice stops (Nekopara 0x086711: m[0x800000f8], which
            // main.kcs's native 359 ties to the config's contvoice, 0x119a0; 0x0a4d0e stops channels
            // 12-21); with it on they play until another voice starts (0x0a4d79 marks them).
            if (!contVoice_) media_.push_back({MediaCommand::Kind::StopVoice, -1});
            dramaVoice_ = false;  // the advance's resets (Cyanotype 0x0cfe50: m[0x2ba54])
            // The advance (a click, auto or skip) ends the scene step there (Amakano 1 0x0827a3,
            // states -6 / -9: local 24), and the step's end stops every object's group 0 motions
            // (0x082eb4 -> 0x086747: native 310, then native 313 with 0) unless a render transition
            // holds them (0x0b4c5d: effect records 2-8 by their phase). The main loop then runs the
            // update (0x0c0354 -> 0x082f11), where they take their end values, before any later
            // line. Every sscript has it (Fruit 0x064741, Nekopara 0x086776, Amakano 2 0x089422).
            // So `pl <n> stop` and a bare `pl <n>` after the click find nothing running and put the
            // planes back whole: in Amakano 1 a 6-second pan clicked through ends at once at the
            // click and the reset brings the backdrop home, as on PC; kept running, it held its end
            // through the reset, the backdrop high.
            if (!(shown_.transition.active(frame_) && shown_.transition.render)) {
                finishClassicMotions(pending_);
                finishClassicMotions(shown_);
            }
            // The window hears the click before the next lines run (see WindowEvent).
            windowEvents_.push_back({WindowEvent::Kind::Click, inputClears_, {}, {}});
            state_ = State::Running;
            run();
        }
        break;
    case State::WaitKey:
        state_ = State::Running;
        run();
        break;
    case State::WaitSe:
    case State::WaitPcm:
    case State::WaitMpl:
    case State::WaitAnimation:
    case State::WaitFrames:
        if (!skipping_) {
            // A click raises the skip flag (sscript's click handler, Nekopara 0x0dbefc: with key
            // skip on and the window up or put off by `frameoff`; Fruit 0x0a0347, REBELLIONS
            // 0x0beddb, Yukikoi 0x0ed5c0 the same). This step's tick answers it (the waits, the
            // motions, a transition; see tick()), and it stays up until a line with text: one
            // click skips the animation up to the next message, as on PC (a Nekopara character's
            // entrance: the click in its last `wait` ended the rest and the empty page).
            if (keySkip_ == 1 && (pending_.window.visible || frameoffHidden_)) setSkipFlag(true, "click");
            break;
        }
        // Skip mode's bursts (GameSession::step) go straight through.
        if (!keySkip_) break;
        if (state_ == State::WaitMpl) skipEmotes();
        if (state_ == State::WaitAnimation || state_ == State::WaitFrames) {
            // Apply it to the pending stage too, or the next commit would bring the tweens back.
            hurry(shown_);
            hurry(pending_);
            shown_.window.alpha.frames = 0;
            waitUntil_ = frame_;
        }
        state_ = State::Running;
        run();
        break;
    default:
        break;
    }
}

void ScenePlayer::choose(size_t index) {
    if (state_ != State::Choice || index >= shown_.choices.size()) return;
    if (choiceShown_) lastChoice_ = std::move(choiceShown_);
    choiceShown_.reset();
    std::string target = shown_.choices[index].target;
    pending_.choices.clear();
    shown_.choices.clear();
    jump(target);
    run();
}

void ScenePlayer::movieFinished() {
    if (state_ == State::Movie) {
        state_ = State::Running;
        run();
    }
}

void ScenePlayer::hurry(Stage& st) {
    st.transition.frames = 0;
    for (auto& layer : st.layers) {
        for (auto& [i, img] : layer) {
            img.x.frames = img.y.frames = img.alpha.frames = 0;
            img.scaleX.frames = img.scaleY.frames = 0;
            std::erase_if(img.motions, [](const Motion& m) { return !m.loop && m.kind != Motion::Kind::Move; });
        }
    }
    for (auto& [i, p] : st.planes) {
        p.x.frames = p.y.frames = p.alpha.frames = 0;
        std::erase_if(p.motions, [](const Motion& m) { return !m.loop; });
    }
    st.window.alpha.frames = 0;
}

bool ScenePlayer::soundPlaying(int kind, int32_t channel) const {
    // Channels 0-9 of the kind (sound slots 2-11 / 12-21 in sscript's table).
    for (int32_t c = channel < 0 ? 0 : channel; c <= (channel < 0 ? 9 : channel); ++c) {
        auto it = soundUntil_.find({kind, c});
        if (it != soundUntil_.end() && frame_ < it->second) return true;
    }
    return false;
}

bool ScenePlayer::mplAnimating(int32_t bank) const {
    // Banks 0-9 (sscript's objects 67-76, the window's face banks among them).
    for (const auto& [i, img] : shown_.layer(Layer::Mpl)) {
        if (bank >= 0 ? i != bank : (i < 0 || i > 9)) continue;
        if (img.emotePlayer && img.emotePlayer->animating()) return true;
    }
    return false;
}

void ScenePlayer::skipEmotes() {
    std::set<EmotePlayer*> done;
    for (Stage* st : {&shown_, &pending_}) {
        for (auto& [i, img] : st->layer(Layer::Mpl)) {
            if (i < 0 || i > 9 || !img.emotePlayer || !done.insert(img.emotePlayer.get()).second) continue;
            img.emotePlayer->skip();
        }
    }
}

bool ScenePlayer::animationsDone() const {
    // A bare `wait` (sscript's code -1, checked each frame at 0x08963e): a transition still up
    // (0x0bce46), or a slot with a group-0 motion running (0x08de04: native 310 with group 0). The
    // mc* (looping) and m2* motions are group 1 and do not hold it: Cyanotype pans its background
    // with `m2move 1800` and goes on at once.
    if (shown_.transition.active(frame_)) return false;
    if (!shown_.window.alpha.done(frame_)) return false;
    auto oneShotRunning = [&](const std::vector<Motion>& ms) {
        for (const auto& m : ms) {
            if (!m.loop && m.group == 0 && m.active(frame_)) return true;
        }
        return false;
    };
    for (const auto& layer : shown_.layers) {
        for (const auto& [i, img] : layer) {
            if (img.posGroup == 0 && (!img.x.done(frame_) || !img.y.done(frame_))) return false;
            if (img.alphaGroup == 0 && !img.alpha.done(frame_)) return false;
            if (img.scaleGroup == 0 && !img.scaleX.done(frame_)) return false;
            if (oneShotRunning(img.motions)) return false;
        }
    }
    for (const auto& [i, p] : shown_.planes) {
        auto group = [&](size_t v) { return p.valueMotions[v].set ? p.valueMotions[v].group : 0; };
        if (group(0) == 0 && (!p.x.done(frame_) || !p.y.done(frame_))) return false;
        if (group(1) == 0 && !p.alpha.done(frame_)) return false;
        if (oneShotRunning(p.motions)) return false;
    }
    return true;
}

void ScenePlayer::commit() {
    // Tweens are expressed against the frame they were scheduled on; keep them running.
    Transition t = nextTransition_;
    t.start = frame_;
    t.id = shown_.transition.id + 1;
    if (t.plane >= 0 && t.kind != Transition::Kind::Cut) {
        // A plane fade starts from the plane as it is now.
        auto from = std::make_shared<Stage>(shown_);
        from->transition = Transition{};
        t.from = std::move(from);
    }
    // `mode`: sscript's per-frame step converts a marked image's pixels or loads it again (see
    // stepImageMode; only sscripts after Fruit's mark them).
    for (Layer layer : {Layer::Bg, Layer::Cg, Layer::Eg, Layer::Fg, Layer::Fw}) {
        for (auto& [i, img] : pending_.layer(layer)) stepImageMode(img);
    }
    size_t revealed = shown_.window.revealed;
    // Text that only grew (a novel page's next message, a `\@` continuation) keeps what was
    // already shown; anything else types out from the start.
    const std::string before = shown_.window.text.plain(), after = pending_.window.text.plain();
    bool keeps = !before.empty() && after.starts_with(before);
    // A commit that leaves the text as it is (a picture changing while the text reveals) is not a
    // new text: the reveal goes on from where it is. In Amakano 1 a character changes pose
    // and hops while her line wipes in; each commit restarted the wipe, so the line stayed hidden
    // until the pictures stopped (200 frames).
    const bool sameText = keeps && after == before;
    const auto wipeStart = shown_.window.wipeStart;
    const auto wipeFrom = shown_.window.wipeFrom;
    shown_ = pending_;
    shown_.transition = t;
    shown_.window.revealed = keeps ? std::min(revealed, shown_.window.revealTotal()) : 0;
    if (sameText) {
        shown_.window.wipeStart = wipeStart;
        shown_.window.wipeFrom = wipeFrom;
    } else if (!keeps || shown_.window.revealed < shown_.window.revealTotal()) {
        // A wipe starts with the new text (grisaia.exe's string plane runs its clock from when the
        // text is set, 0x502540); what was already shown stays.
        shown_.window.wipeStart = frame_;
        shown_.window.wipeFrom = shown_.window.revealed;
        // The engine draws the text's first glyph on the frame it is set.
        if (!wipe_ && !held_) updateReveal();
    }
    nextTransition_ = Transition{};
    dirty_ = false;
}

void ScenePlayer::run() {
    uint32_t guard = 0;
    while (state_ == State::Running) {
        if (scene_ && pc_ >= scene_->lines().size() && !callStack_.empty()) {
            // The end of a called scene: back to the caller.
            scene_ = std::move(callStack_.back().scene);
            sceneName_ = callStack_.back().name;
            pc_ = callStack_.back().pc;
            callStack_.pop_back();
            continue;
        }
        if (!scene_ || pc_ >= scene_->lines().size()) {
            if (!pending_.choices.empty()) {
                commit();
                block(State::Choice);
            } else {
                commit();
                block(State::Ended);
                if (log) log("end of scene " + sceneName_);
            }
            return;
        }
        if (++guard > kMaxLinesPerTick) {
            if (log) log("too many lines without a wait; pausing a frame");
            return;
        }
        // sscript's fselect (0x07d361) reads the option lines after it itself (0x01450c) and stops the
        // scene as soon as they end (state 23, return -11): the line after them runs only if the
        // choice does not jump away (Himawari's `next` after its first choice).
        if (collectingChoices_ && !isChoiceOption(scene_->lines()[pc_])) {
            collectingChoices_ = false;
            if (!pending_.choices.empty()) {
                commit();
                block(State::Choice);
                return;
            }
        }
        const CstLine& line = scene_->lines()[pc_++];
        execute(line);
    }
}

bool ScenePlayer::isChoiceOption(const CstLine& line) {
    // fselect's option loop (Nekopara 0x07d4a4): debug lines are passed over; every command line
    // is an option, `<id> <target> <text>` after an optional `if (<condition>)` (false: the line
    // is skipped and the loop reads on; Cyanotype's chapter menus). The id is read as a number
    // (a non-numeric id, as one of Yukikoi's, is option 0). A line without its target or text
    // (Himawari ends a list with a `next` line), or any other line, ends the options and runs after the choice.
    if (line.type == CstLineType::ScriptName || line.type == CstLineType::LineNumber) return true;
    if (line.type != CstLineType::Command) return false;
    std::string_view t = line.text;
    size_t at = t.find_first_not_of(" \t");
    if (at == std::string_view::npos) return false;
    if (t.substr(at, 2) == "if") {
        at = t.find_first_not_of(" \t", at + 2);
        if (at == std::string_view::npos || t[at] != '(') return false;
        int depth = 0;
        for (; at < t.size(); ++at) {
            if (t[at] == '(') ++depth;
            else if (t[at] == ')' && --depth == 0) break;
        }
        if (at >= t.size()) return false;
        t = t.substr(at + 1);
    }
    return splitArgs(t).size() >= 3;
}

void ScenePlayer::execute(const CstLine& line) {
    auto& w = pending_.window;
    switch (line.type) {
    case CstLineType::Name:
        pendingName_ = speakerName(cleanLabel(line.text, line.utf8));
        pendingSpeaker_ = !line.own.empty() ? line.own : line.utf8 ? line.text : decodeGameText(line.text);
        haveName_ = true;
        windowEvents_.push_back({WindowEvent::Kind::Name, false, pendingSpeaker_, pendingName_});
        break;
    case CstLineType::Message: {
        // A line with text lowers the click's skip flag and the `frameoff` mark (Nekopara's sscript
        // 0x012a4c: an empty one skips all of it, 0x012af6 / 0x012ab0).
        if (!line.text.empty()) {
            setSkipFlag(false, "message");
            if (!forceMesDel_) frameoffHidden_ = false;
        }
        // `$strN` in the text (Amakano 2 puts the player's name in its text this way) is the
        // variable's value, as in commands and speaker names. Variables hold Shift-JIS.
        std::string raw = line.text;
        if (raw.find("$str") != std::string::npos)
            raw = line.utf8 ? sjisToUtf8(expandStrings(utf8ToSjis(raw))) : expandStrings(raw);
        pageChars_ += countChars(raw, line.utf8);
        if (w.novel) {
            if (!messageOpen_) novelLineHasText_ = false;  // a message starts its own line
            // A novel message's leading `\r`s: cs2.exe's NVSTRING counts them (0x69dd30) and puts
            // that many line breaks into its text and name columns (0x69dc70). Yukikoi's opening
            // pages start with six and then a line break: on PC the text
            // stands on the page's 7th line, six breaks down, so the break after the run is one
            // of them (fitted to the PC screenshot; how the two fold together is not traced).
            if (!messageOpen_ || novelBreaks_ > 0) {
                size_t at = 0;
                while (at + 1 < raw.size() && raw[at] == '\\' && (raw[at + 1] == 'r' || raw[at + 1] == 'R')) at += 2;
                novelBreaks_ += static_cast<int32_t>(at / 2);
                raw.erase(0, at);
                if (novelBreaks_ > 0 && !raw.empty()) {
                    if (raw.starts_with("\\n") || raw.starts_with("\\N")) raw.erase(0, 2);
                    std::string breaks;
                    for (int32_t k = 0; k < novelBreaks_; ++k) breaks += "\\n";
                    raw = breaks + raw;
                    novelBreaks_ = 0;
                }
            }
            // Any other `\r` in a novel message ends a line that has text (PC, Yukikoi's novel
            // pages: the two parts on two lines, each centred); at a line's start it
            // does nothing (the `\n\r` blank lines of Hatsumira's, Eden's, Labyrinth's and Himawari's
            // novel pages stay single). ADV text keeps it as a code without width.
            bool& hasText = novelLineHasText_;
            for (size_t k = 0; k < raw.size(); ++k) {
                const auto c = static_cast<uint8_t>(raw[k]);
                if (!line.utf8 && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC))) {
                    // A Shift-JIS character (its second byte may be 0x5C); a full-width space
                    // opening a line takes no room, so it is not text.
                    if (!(c == 0x81 && k + 1 < raw.size() && static_cast<uint8_t>(raw[k + 1]) == 0x40)) hasText = true;
                    ++k;
                    continue;
                }
                if (raw[k] != '\\' || k + 1 >= raw.size()) {
                    if (line.utf8 && raw.compare(k, 3, "\xE3\x80\x80") == 0) k += 2;
                    else if (raw[k] != ' ') hasText = true;
                    continue;
                }
                const char e = static_cast<char>(raw[k + 1] | 0x20);
                if (e == 'n') {
                    hasText = false;
                } else if (e == 'r') {
                    if (hasText) raw[k + 1] = 'n';
                    hasText = false;
                }
                ++k;  // the escaped character is not an escape start
                // Nor are an escape's own letters and number (`\fn`, `\fss`, `\f+4`, `\cd0xff..;`,
                // `\pc`, `\w12`, as parseMessage reads them): Fruit's "\n\fn\r\pc..." keeps its
                // `\r` at the line's start.
                auto peek = [&](size_t at) { return at < raw.size() ? static_cast<char>(raw[at] | 0x20) : '\0'; };
                const char* sub = e == 'f' ? "nslr" : e == 'c' ? "drs" : e == 'p' ? "lcr" : e == 'w' ? "fr" : "";
                if (*sub && std::strchr(sub, peek(k + 1)) && peek(k + 1)) {
                    ++k;
                    if (e == 'f' && (peek(k) == 's' || peek(k) == 'l') && peek(k + 1) == peek(k)) ++k;
                }
                if (e == 'f' || e == 'c' || e == 'w' || e == 'x') {
                    if (k + 1 < raw.size() && (raw[k + 1] == '+' || raw[k + 1] == '-')) ++k;
                    if (raw.compare(k + 1, 2, "0x") == 0 || raw.compare(k + 1, 2, "0X") == 0) {
                        k += 2;
                        while (k + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[k + 1]))) ++k;
                    } else {
                        while (k + 1 < raw.size() && std::isdigit(static_cast<unsigned char>(raw[k + 1]))) ++k;
                    }
                    if (k + 1 < raw.size() && raw[k + 1] == ';') ++k;
                }
            }
        }
        // Every `[message]` line up to the next input adds to the page: sscript appends each to its
        // page text (0x00fe9a, native 15 onto string 139472) and clears it only after the click
        // (0x094408). Fruit puts a "\n" on a line of its own after a voiced line.
        // The joined lines are one string to the text reader, so a line goes on in the style the
        // last one ended in (Hatsumira: blank lines, then a `\pc` line).
        const bool samePage = std::exchange(messageOpen_, true) && !w.text.empty();
        MessageText msg = parseMessage(raw, line.utf8, samePage ? &w.text : nullptr, textStyles_);
        bool append = w.text.continues || w.novel;
        // The sub language's text goes with the message's first line, the same way.
        MessageText subMsg = line.sub.empty() ? MessageText{} : parseMessage(line.sub, true, samePage ? &w.subText : nullptr, textStyles_);
        if (samePage || (append && !w.text.empty())) w.subText.append(subMsg);
        else w.subText = std::move(subMsg);
        if (samePage) {
            w.text.append(msg);
        } else if (append && !w.text.empty()) {
            if (w.novel && !w.text.continues) {
                // A new message on the novel page: its own paragraph, with its speaker.
                w.text.spans.push_back({"\n"});
                w.novelLines.push_back({w.text.spans.size(), haveName_ ? pendingName_ : std::string()});
                w.name = haveName_ ? pendingName_ : std::string();
                w.speaker = haveName_ ? pendingSpeaker_ : std::string();
            }
            w.text.append(msg);
        } else {
            w.text = std::move(msg);
            w.name = haveName_ ? pendingName_ : std::string();
            w.speaker = haveName_ ? pendingSpeaker_ : std::string();
            w.novelLines.clear();
            if (w.novel) w.novelLines.push_back({0, w.name});
            // With `autoface` on, a page whose lines hold no `fw` (sscript's look-ahead 0x013951 sets
            // bSetFace m[0x80de4] for an `fw` that is not a sub-command, autoface clears it; reset
            // with each page, 0x0cf0a2) loses its faces: a message without a name (0x012b83 ->
            // 0x003306) or with one nametable.csv's lookup, 0x003ed0, finds no sprite for (every
            // game's: Yukikoi's and Hatsumira's tables are another game's, Grisaia's prefixes `mic`
            // match no `Tmic...` sprite; the face it would derive from one, native 640, is not
            // modelled; nor that Skip to Next Option keeps faces there). With it off, faces stay
            // until a script line changes them (Amakano 2, Cyanotype, Happiness 2).
            if (autoFace_ && !faceSet_ && !pending_.layer(Layer::Fw).empty()) pending_.layer(Layer::Fw).clear();
        }
        faceSet_ = false;
        haveName_ = false;
        // A message brings back a window `frameoff` hid (Yukikoi's sscript 0x012e6b clears the
        // frame-off flag m[0x2b9b0]), unless `forcemesdel 1` is on (0x012e5f).
        if (!w.visible && !forceMesDel_) {
            w.visible = true;
            w.alpha = Tween::constant(255);
        }
        break;
    }
    case CstLineType::Input:
    case CstLineType::Page: {
        messageOpen_ = false;
        novelBreaks_ = 0;
        commit();
        // sscript stops at every input line, text or not (0x012c62: state -6 / -9, both waiting for
        // the click, auto or skip at 0x0863b4): Nekopara's extras viewers step their pictures on
        // empty messages.
        inputPc_ = pc_ - 1;
        logMessage();
        // sscript sets the auto deadline as the line goes up and counts characters until the click
        // (0x00fe5a; reset at 0x091af2 after the advance).
        inputFrame_ = frame_;
        inputChars_ = std::exchange(pageChars_, 0);
        block(State::Input);
        // After the click, the next message replaces this one unless it continues.
        inputClears_ = (line.type == CstLineType::Page || !w.text.continues) && (line.type == CstLineType::Page || !w.novel);
        if (line.type == CstLineType::Page || !w.text.continues) {
            if (line.type == CstLineType::Page || !w.novel) {
                w.text = MessageText{};
                w.subText = MessageText{};
                w.name.clear();
                w.speaker.clear();
                w.novelLines.clear();
            }
        }
        if (!pending_.choices.empty()) block(State::Choice);
        break;
    }
    case CstLineType::Command:
        lineUtf8_ = line.utf8;
        if (log && std::getenv("NYAN_LINE_TRACE")) log("line " + std::to_string(pc_ - 1) + " frame " + std::to_string(frame_) + ": " + line.text);
        // Language placeholders in asset names (`eva70_%LANG_M%`).
        if (line.text.find('%') != std::string::npos) command(assets_.expand(line.text));
        else command(line.text);
        lineUtf8_ = false;
        break;
    case CstLineType::ScriptName:
    case CstLineType::LineNumber:
        break;
    }
}

std::string ScenePlayer::expandStrings(const std::string& text) const {
    // `$strN` -> string variable N (Amakano 2 keeps the player's name in one and face parts in another).
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 4, "$str") == 0 && i + 4 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 4]))) {
            size_t j = i + 4;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            out += vars_.str(std::atoi(text.c_str() + i + 4));
            i = j;
        } else {
            out += text[i++];
        }
    }
    return out;
}

std::string ScenePlayer::stringTests(const std::string& cond) const {
    // `$strN == "text"` / `!=` (Amakano 2 tests a string variable against words this way): each test
    // becomes 1 or 0 for the number evaluator.
    if (cond.find("$str") == std::string::npos) return cond;
    std::string out;
    for (size_t i = 0; i < cond.size();) {
        size_t d = i + 4;
        if (cond.compare(i, 4, "$str") == 0 && d < cond.size() && std::isdigit(static_cast<unsigned char>(cond[d]))) {
            size_t j = d;
            while (j < cond.size() && std::isdigit(static_cast<unsigned char>(cond[j]))) ++j;
            size_t op = cond.find_first_not_of(" \t", j);
            size_t q1 = op == std::string::npos ? op : cond.find_first_not_of(" \t", op + 2);
            if (op != std::string::npos && q1 != std::string::npos && cond[q1] == '"' &&
                (cond.compare(op, 2, "==") == 0 || cond.compare(op, 2, "!=") == 0)) {
                size_t q2 = cond.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    bool same = vars_.str(std::atoi(cond.c_str() + d)) == cond.substr(q1 + 1, q2 - q1 - 1);
                    out += (same == (cond[op] == '=')) ? "1" : "0";
                    i = q2 + 1;
                    continue;
                }
            }
        }
        out += cond[i++];
    }
    return out;
}

std::string ScenePlayer::speakerName(const std::string& raw) {
    std::string name = raw.find("$str") == std::string::npos ? raw : sjisToUtf8(expandStrings(utf8ToSjis(raw)));
    // `<character>＠<shown name>` (Cyanotype and Amakano 2 name speakers this way): the
    // window shows the part after ＠ (sscript 0x0013d4).
    static const std::string kAt = "\xEF\xBC\xA0";  // U+FF20
    size_t at = name.find(kAt);
    return at == std::string::npos ? name : name.substr(at + kAt.size());
}

int32_t ScenePlayer::num(const std::string& e, int32_t current) {
    return expr_.eval(e, current);
}

Motion ScenePlayer::pathKeys(const std::vector<std::string>& args, size_t from, bool spline, bool loop, float curX,
                             float curY) {
    // `quake`/`spline` keyframes: "<frames>,<x>,<y>[,<z>]"; `@` in x/y is the current position.
    Motion m;
    m.kind = Motion::Kind::Path;
    m.loop = loop;
    m.start = frame_;
    m.a[0] = spline ? 1.0f : 0.0f;
    m.a[1] = curX, m.a[2] = curY;  // a spline starts where the object is
    uint32_t total = 0;
    for (size_t i = from; i < args.size(); ++i) {
        std::vector<std::string> f;
        for (size_t s = 0, c; s <= args[i].size(); s = c + 1) {
            c = args[i].find(',', s);
            if (c == std::string::npos) c = args[i].size();
            f.push_back(args[i].substr(s, c - s));
        }
        int32_t frames = std::max(0, argNum(f, 0, 0));
        int32_t cx = spline ? static_cast<int32_t>(curX) : 0, cy = spline ? static_cast<int32_t>(curY) : 0;
        float x = static_cast<float>(f.size() > 1 && !f[1].empty() ? argNum(f, 1, cx, cx) : cx);
        float y = static_cast<float>(f.size() > 2 && !f[2].empty() ? argNum(f, 2, cy, cy) : cy);
        m.keys.push_back({static_cast<float>(frames), x, y});
        total += static_cast<uint32_t>(frames);
    }
    m.period = std::max<uint32_t>(1, total);
    return m;
}

Motion ScenePlayer::pendulum(const std::vector<std::string>& args, bool loop, uint64_t at) {
    // motPend: the angle is sin(triangle(t, 2 * frames / halfSwings) * pi/2) * z for `frames`
    // frames; mcPend repeats with a period of 2 * frames. Only the z (screen) angle is drawn.
    Motion m;
    m.kind = Motion::Kind::Pendulum;
    m.loop = loop;
    m.start = at;
    float frames = std::max(1.0f, static_cast<float>(argNum(args, 3, 30)));
    if (loop) {
        m.a[0] = 2 * frames;
        m.a[1] = static_cast<float>(argNum(args, 6, 90));
        m.period = static_cast<uint32_t>(m.a[0]);
    } else {
        float half = static_cast<float>(argNum(args, 4, 1));
        m.a[0] = half != 0 ? 2 * frames / half : 2 * frames;
        m.a[1] = static_cast<float>(argNum(args, 7, 90));
        m.period = static_cast<uint32_t>(frames);
    }
    return m;
}

void ScenePlayer::loadToneTable() {
    if (toneTableLoaded_) return;
    toneTableLoaded_ = true;
    if (!assets_.find("bgtone.tbl")) return;
    Bytes b = assets_.read("bgtone.tbl");
    std::string text(b.begin(), b.end());
    // Each line without its tabs and spaces (0x78d740); a `#name(r,g,b)` line starts a tone, the
    // others name the images that take it (looked up in lower case, 0x78d220). Lines before the
    // first tone belong to "__UNKNOWN__" (100 100 100).
    std::array<int16_t, 3> cur{100, 100, 100};
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line;
        for (size_t i = pos; i < eol; ++i) {
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') line += text[i];
        }
        pos = eol + 1;
        if (size_t c = line.find("//"); c != std::string::npos) line.erase(c);
        if (line.empty()) continue;
        if (line[0] == '#') {
            size_t open = line.find('('), close = line.find(')');
            std::array<int16_t, 3> v{100, 100, 100};
            if (open != std::string::npos && close != std::string::npos && close > open) {
                std::string body = line.substr(open + 1, close - open - 1);
                size_t k = 0;
                for (size_t c = 0, n; k < 3 && c <= body.size(); c = n + 1, ++k) {
                    n = body.find(',', c);
                    if (n == std::string::npos) n = body.size();
                    v[k] = static_cast<int16_t>(std::atoi(body.substr(c, n - c).c_str()));
                }
                toneTable_[line.substr(1, open - 1)] = v;
            }
            cur = v;
        } else {
            toneByImage_[toLowerAscii(line)] = cur;
        }
    }
}

std::array<int16_t, 3> ScenePlayer::slotTone(Layer layer, int32_t index) const {
    auto it = pending_.slotTones.find({static_cast<int32_t>(layer), index});
    return it == pending_.slotTones.end() ? std::array<int16_t, 3>{100, 100, 100} : it->second;
}

void ScenePlayer::setSlotTone(Layer layer, int32_t index, const std::array<int16_t, 3>& tone) {
    const std::pair<int32_t, int32_t> key{static_cast<int32_t>(layer), index};
    if (tone == std::array<int16_t, 3>{100, 100, 100}) pending_.slotTones.erase(key);
    else pending_.slotTones[key] = tone;
}

std::array<int16_t, 3> ScenePlayer::toneArgs(const std::vector<std::string>& args) {
    std::array<int16_t, 3> t{100, 100, 100};
    if (args.size() <= 3) return t;
    if (!args[3].empty() && !std::isdigit(static_cast<unsigned char>(args[3][0])) && args[3][0] != '-' &&
        args[3][0] != '#') {
        // A named tone from config.int/bgtone.tbl.
        loadToneTable();
        auto it = toneTable_.find(args[3]);
        if (it != toneTable_.end()) return it->second;
        unhandled("tone " + args[3]);
        return t;
    }
    for (size_t k = 0; k < 3 && args.size() > 3 + k; ++k) {
        // `0 0 0` and `0% 0% 0%` alike (sscript.kcs reads the number before any `%`).
        t[k] = static_cast<int16_t>(std::clamp(std::atoi(args[3 + k].c_str()), 0, 1000));
    }
    return t;
}

int32_t ScenePlayer::argNum(const std::vector<std::string>& args, size_t i, int32_t fallback, int32_t current) {
    if (i >= args.size() || !isNumberLike(args[i])) return fallback;
    try {
        return num(args[i], current);
    } catch (const Error&) {
        return fallback;
    }
}

void ScenePlayer::unhandled(const std::string& what) {
    if (unhandled_[what]++ == 0 && log) log("unhandled: " + what);
}

void ScenePlayer::missing(const std::string& asset) {
    if (missing_[asset]++ == 0 && log) log("missing asset: " + asset + " (" + sceneName_ + ":" + std::to_string(pc_ - 1) + ")");
}

void ScenePlayer::command(const std::string& text) {
    settlePositions();
    if (text.empty()) return;

    // `if (cond) command` / `if(cond) command`
    if (text.starts_with("if")) {
        size_t open = text.find('(');
        if (open != std::string::npos && text.find_first_not_of(" \t", 2) == open) {
            int depth = 0;
            size_t close = open;
            for (; close < text.size(); ++close) {
                if (text[close] == '(') ++depth;
                else if (text[close] == ')' && --depth == 0) break;
            }
            if (close >= text.size()) {
                // An unclosed `(` (REBELLIONS has 594: an `if` short of its last `)`, then an assignment):
                // the condition word runs to the line's end (native 519's split, amakanoPE.exe
                // 0x591780), so no command follows it. The calculator's pass (REBELLIONS.exe
                // 0x501bf0) makes the open `(` the root of the tree, and the tree's evaluation
                // (0x502fa0) has no case for a `(`: 0, and nothing under it runs, the assignment
                // included. Whatever splits it, the line does nothing.
                return;
            }
            const size_t from = text.find_first_not_of(" \t", close + 1);
            const std::string rest = text.substr(from == std::string::npos ? text.size() : from);
            bool holds = false;
            try {
                holds = num(stringTests(text.substr(open + 1, close - open - 1))) != 0;
            } catch (const Error&) {
                // A condition our parser takes for a mistake is still the calculator's (native 213):
                // a name is 0 there, an operator without an operand gives 0 (engineCalc). In Fruit
                // an `if (#(960+s)==0)` holds when #960 is 0; Amakano 1's `if( == 0 )` never.
                holds = engineCalc(text.substr(open, close - open + 1), vars_, calcPrivate_) != 0;
            }
            if (holds) command(rest);
            return;
        }
    }

    if (text[0] == '(') {
        // `(#<n> = 1)` (Amakano 2, after an `if`): a parenthesised assignment.
        size_t close = text.rfind(')');
        std::string inner = text.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        size_t first = inner.find_first_not_of(" \t");
        if (first != std::string::npos && inner[first] == '#') {
            command(inner.substr(first));
            return;
        }
    }
    if (text[0] == '#') {
        // A `#` line is an unknown word: sscript hands the whole line to native 213, cs2.exe's
        // calculator (Happiness 2 0x635910), where `@` is the value native 212 last stored
        // (calc +0x14), not the variable's own. That value lives only until the frame ends: in the
        // original under Wine (Happiness 2, 2026-10-31) it read 0 at every page, a command's 640 or
        // 360 lasting one frame, and with a variable set to 520 beforehand, a later `#<n> = @`
        // made it 0. Every Happiness 2 and Amakano 2 `@` line comes after a frame break; the same-
        // frame value (Yukikoi / Eden / Hatsumira's `#(<n>+#<m>)=@` after a load) is not modelled,
        // and those variables are written again before anything reads them.
        try {
            if (!expr_.assign(text, 0)) unhandled("# (not an assignment)");
        } catch (const Error&) {
            // What our parser rejects the calculator still runs: Hatsumira's
            // `#(<n>+R)=<v>` sets #<n> (an unknown name such as R is 0), as does Labyrinth's
            // `#(<n>+b)=<v>`; a Himawari line `#(<n>+0) = <v> se 0 <name>` stores nothing, its last
            // tree being `0 = <name>` (engineCalc).
            engineCalc(text, vars_, calcPrivate_, 0);
        }
        return;
    }

    // `$strN` anywhere in a command is the string variable's text (Himawari's `fg <n> $str<m>`,
    // Amakano 2's face parts `fw 0 <face>,b,a,$str<n>,...`).
    auto args = splitArgs(text.find("$str") == std::string::npos ? text : expandStrings(text));
    if (args.empty()) return;
    const std::string& cmd = args[0];
    // `%command`: a line the authors switched off (no window script or engine command answers it).
    if (cmd[0] == '%') return;

    // Choice entries following `fselect`: "<n> <scene> <prompt>" (see isChoiceOption)
    if (collectingChoices_ && args.size() >= 3) {
        std::string prompt;
        for (size_t i = 2; i < args.size(); ++i) prompt += (i > 2 ? " " : "") + args[i];
        // `(<condition>:<text>)`: a choice offered only while the condition holds (empty: always).
        if (prompt.size() >= 3 && prompt.front() == '(' && prompt.back() == ')') {
            if (size_t colon = prompt.find(':'); colon != std::string::npos) {
                std::string cond = prompt.substr(1, colon - 1);
                prompt = prompt.substr(colon + 1, prompt.size() - colon - 2);
                if (!cond.empty()) {
                    bool keep = true;
                    try {
                        keep = num(cond) != 0;
                    } catch (const Error&) {
                    }
                    if (!keep) return;
                }
            }
        }
        std::string utf8 = lineUtf8_ ? prompt : sjisToUtf8(prompt);
        // Full-width spaces separate words in translated prompts.
        for (size_t p; (p = utf8.find("\xE3\x80\x80")) != std::string::npos;) utf8.replace(p, 3, " ");
        std::replace(utf8.begin(), utf8.end(), '_', ' ');
        std::replace(utf8.begin(), utf8.end(), '|', ' ');
        pending_.choices.push_back({args[1], cleanLabel(utf8, true)});
        return;
    }

    if (auto layer = layerFromWord(cmd)) {
        imageCommand(*layer, args);
        return;
    }

    if (cmd == "wait" && args.size() >= 3 && args[1] == "tag") {
        // `wait tag <name> [frames]` (sscript: Nekopara 0x06ed92 -> 0x0a5c02, Amakano 2 0x070714 ->
        // 0x0a9b6a): each playing sound in channel order is asked for its mark <name> (native 903:
        // `<voice>.tag`, 24-byte records, a 20-byte name and a 32-bit position in milliseconds); the
        // first that has it sets the end to where it started plus the mark. Otherwise <frames> from
        // now; without <frames>, the script's own default (120 in Amakano 2, Cyanotype and
        // Yukikoi; 0 in Nekopara and the older games: no wait).
        commit();
        int32_t frames = args.size() > 3 ? argNum(args, 3, waitTagFrames_) : waitTagFrames_;
        uint64_t until = frame_ + static_cast<uint64_t>(std::max(0, frames));
        for (const auto& [ch, v] : voiceChannels_) {
            const auto& [voice, start] = v;
            if (frame_ >= start + voiceLength(voice)) continue;  // over: the channel is free again
            auto ref = assets_.find(voice + ".tag");
            if (!ref) continue;
            Bytes b = ref->archive->read(*ref->entry);
            bool found = false;
            for (size_t o = 0; o + 24 <= b.size(); o += 24) {
                std::string name(reinterpret_cast<const char*>(b.data() + o), strnlen(reinterpret_cast<const char*>(b.data() + o), 20));
                if (name != args[2]) continue;
                uint64_t ms = readLE32(b.data() + o + 20);
                until = start + (ms * 60 + 999) / 1000;
                found = true;
                break;
            }
            if (found) {
                if (log && std::getenv("NYAN_TAG_TRACE"))
                    log("wait tag " + args[2] + ": " + voice + " from f" + std::to_string(start) + " -> f" + std::to_string(until));
                break;
            }
        }
        if (log && std::getenv("NYAN_TAG_TRACE")) log("wait tag " + args[2] + " at f" + std::to_string(frame_) + " until f" + std::to_string(until));
        waitUntil_ = std::max(until, frame_);
        block(State::WaitFrames);
        return;
    }
    if (cmd == "wait") {
        commit();
        if (args.size() >= 2 && (args[1] == "se" || args[1] == "pcm" || args[1] == "mpl")) {
            // `wait se [ch]` / `wait pcm [ch]` (codes -3 / -4): until that channel's sound (any
            // channel without one) has stopped (0x0a4b57 / 0x0a49a0: native 160 on each). `wait mpl
            // [n]` (-17, then -18): until E-mote bank n (any of the ten without one) is no longer
            // animating (0x0c1e75 / 0x0c1f65: native 909, message 0x210, the DLL's IsAnimating).
            // Nekopara 0x06ea4a / 0x06eae2 / 0x06ecb5; the same in Cyanotype (0x070d09).
            waitChannel_ = args.size() >= 3 ? argNum(args, 2, -1) : -1;
            waitSince_ = frame_;
            block(args[1] == "se" ? State::WaitSe : args[1] == "pcm" ? State::WaitPcm : State::WaitMpl);
            return;
        }
        if (args.size() >= 2 && args[1] == "key") {
            // `wait key` (sscript 0x070e39: code -5, m[0x2f9b4]): the script stops until a click
            // (0x0ee55b), or in auto mode until the auto delay passes (0x0899e3, the delay of
            // 0x0163e8); skipping goes through it. Cyanotype holds its camera shots this way.
            waitKeyFrame_ = frame_;
            block(State::WaitKey);
            return;
        }
        if (args.size() >= 2) {
            // The end is the start plus the frames, divided by 1.5 at the `effspd` setting 1 and
            // by 2 at 2 (Cyanotype's sscript 0x08dd29: the setting's switch has only cases 0-2,
            // so 3 and 4 wait the full count), cut to a whole frame.
            float n = static_cast<float>(std::max(0, argNum(args, 1, 0)));
            if (effectSpeed_ == 1) n /= 1.5f;
            else if (effectSpeed_ == 2) n /= 2.0f;
            waitUntil_ = frame_ + static_cast<uint64_t>(n);
            block(State::WaitFrames);
        } else {
            block(State::WaitAnimation);
        }
        return;
    }
    if (cmd == "get" && args.size() > 3) {
        // `get <bg|cg|eg|fg|fw|pl> <n> <property> [x var] [y var] [z var]` (Cyanotype's sscript):
        // the object's current values into those variables (-1 or none: not wanted). Scripts
        // keep a plane's place this way and put it back with `pl <n> pos #<x> #<y> #<z>`.
        const std::string& kind = args[1];
        const std::string& prop = args[3];
        int32_t n = argNum(args, 2, 0);
        std::array<int32_t, 3> v{0, 0, 0};
        bool known = true;
        if (kind == "pl") {
            const Plane& p = plane(n);
            if (prop == "pos") v = {p.x.value(frame_), p.y.value(frame_), 0};
            else if (prop == "base") v = {p.baseX, p.baseY, 0};
            else if (prop == "vram") v = {p.vramX, p.vramY, 0};
            else known = false;
        } else if (auto layer = layerFromWord(kind)) {
            const StageImage* img = slot(*layer, n, false);
            if (prop == "pos") v = img ? std::array<int32_t, 3>{img->x.value(frame_), img->y.value(frame_), 0} : v;
            else known = false;
        } else {
            known = false;
        }
        if (!known) {
            unhandled("get " + kind + " " + prop);
            return;
        }
        for (size_t i = 0; i < 3; ++i) {
            int32_t var = args.size() > 4 + i ? argNum(args, 4 + i, -1) : -1;
            if (var != -1) vars_.set(var, v[i]);
        }
        return;
    }
    if (cmd == "drawdef" || cmd == "wipedef") {
        // `drawdef [frames] [plane]` / `wipedef [frames] [plane]` (sscript 0x071d35 / 0x0756fb): what
        // a `draw` / a wipe without them uses; bare, they change nothing.
        auto& frames = cmd == "drawdef" ? drawDefFrames_ : wipeDefFrames_;
        auto& plane = cmd == "drawdef" ? drawDefPlane_ : wipeDefPlane_;
        if (args.size() > 1) frames = argNum(args, 1, frames);
        if (args.size() > 2) plane = argNum(args, 2, plane);
        return;
    }
    if (effectCut_ && (cmd == "draw" || cmd == "rdraw" || cmd == "rpflip" || cmd == "rcflip" || cmd == "wbreak" || cmd == "rwmos" ||
                       cmd == "rfmos" || cmd == "wipe" || cmd == "rwipe" || cmd == "wipe2" || cmd == "wipe3")) {
        // The config's `effcut` (offset 0x13494 in Cyanotype's cs2.exe, getconf 0x68ca5e; main.kcs's
        // native 359 lays the block from 0x13490 at m[0x8000172c], +4): the first test of `draw`,
        // `wipe`, `wipe2`, `wipe3`, `rdraw`, `rwipe`, `rwipe2`, `rwmos`, `rfmos`, `wbreak`, `rpflip`
        // and `rcflip` (Cyanotype 0x071570 ... 0x084066; rdraw / rwipe test the same field in
        // every game's sscript), which return at once when it is set.
        return;
    }
    if (cmd == "draw") {
        // `draw [frames] [plane]` (sscript -> Task_Eff2D_Fade on that plane): that plane's images
        // cross-fade over frames - 2 (none at all from 3 frames down), counted at the `effspd` speed
        // like the 2D wipes; everything else changes at once.
        int32_t frames = (args.size() > 1 ? argNum(args, 1, drawDefFrames_) : drawDefFrames_) - 2;
        nextTransition_.plane = std::clamp(args.size() > 2 ? argNum(args, 2, drawDefPlane_) : drawDefPlane_, 0, 31);
        static constexpr float kSpeed[5] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
        nextTransition_.kind = frames > 1 ? Transition::Kind::Fade : Transition::Kind::Cut;
        nextTransition_.frames = frames > 1 ? static_cast<uint32_t>(std::ceil(static_cast<float>(frames) / kSpeed[std::clamp(effectSpeed_, 0, 4)])) : 0;
        return;
    }
    if (cmd == "rdraw") {
        if (skipFlag_) return;  // the skip flag up: no effect (Nekopara's `rdraw` 0x078ad1)
        // Another render transition up (asked for this frame or running): this one does nothing
        // (every render keyword tests 0x0bce46 first, Cyanotype `rdraw` 0x07ad94; see renderTransitionUp).
        if (renderTransitionUp()) return;
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 1, 0)));
        nextTransition_.kind = frames ? Transition::Kind::Fade : Transition::Kind::Cut;
        nextTransition_.frames = frames;
        nextTransition_.render = true;
        return;
    }
    if (cmd == "rpflip") {
        if (skipFlag_) return;  // as `rdraw`
        if (renderTransitionUp()) return;  // as `rdraw`
        // `rpflip <frames> [angle] [radius] [fold]` (Amakano's sscript 0x0830f0, transition 7):
        // ignored unless frames > 2; it runs over frames - 2 when the next screen is drawn. The
        // angles are degrees (sscript's builtin 62: x pi / 180), defaults 0, 0 and 90; two more
        // numbers (default 0 and 45000) only set where the effect sits among the layers.
        int32_t frames = argNum(args, 1, 0);
        if (frames <= 2) return;
        const float kRad = 3.14159274f / 180.0f;
        nextTransition_.kind = Transition::Kind::PageFlip;
        nextTransition_.render = true;
        nextTransition_.frames = static_cast<uint32_t>(frames - 2);
        nextTransition_.flipAngle = static_cast<float>(argNum(args, 2, 0)) * kRad;
        nextTransition_.flipRadius = static_cast<float>(argNum(args, 3, 0));
        nextTransition_.flipFold = static_cast<float>(argNum(args, 4, 90)) * kRad;
        return;
    }
    if (cmd == "rcflip") {
        if (skipFlag_) return;  // as `rdraw`
        if (renderTransitionUp()) return;  // as `rdraw`
        // `rcflip [frames] [w] [h] [angle] [band] [axis] [colour] [from] [to]` (Amakano+'s sscript
        // 0x07fc96, transition 8): frames default to drawdef's, nothing at 2 or fewer, the effect
        // runs frames - 2; cells w x h (256 x 256); angle 270 and axis 340 (degrees, builtin 62: x pi
        // / 180), band 200, colour $ff000000; from / to (0, 45000) only set where the effect sits
        // among the layers. Native 971 hands them to kcTaskRenderCFlip (Amakano 2 0x592ea0, message
        // 400) when the next screen is drawn. See StageRenderer::drawCellFlip.
        const int32_t given = args.size() > 1 ? argNum(args, 1, drawDefFrames_) : drawDefFrames_;
        if (given <= 2) return;
        const float kRad = 3.14159274f / 180.0f;
        nextTransition_.kind = Transition::Kind::CellFlip;
        nextTransition_.render = true;
        nextTransition_.frames = static_cast<uint32_t>(given - 2);
        nextTransition_.flipCellW = std::max(1, argNum(args, 2, 256));
        nextTransition_.flipCellH = std::max(1, argNum(args, 3, 256));
        nextTransition_.flipAngle = static_cast<float>(argNum(args, 4, 270)) * kRad;
        nextTransition_.flipBand = static_cast<float>(argNum(args, 5, 200));
        nextTransition_.flipAxis = static_cast<float>(argNum(args, 6, 340)) * kRad;
        nextTransition_.flipColor = static_cast<uint32_t>(argNum(args, 7, static_cast<int32_t>(0xFF000000u)));
        return;
    }
    if (cmd == "wbreak") {
        if (skipFlag_) return;  // as `rdraw`
        if (renderTransitionUp()) return;  // as `rdraw`
        // `wbreak [frames] [w] [h] [speed] [spin] [from] [to]` (Yukikoi's sscript 0x0817c5,
        // transition 6; a draw like rdraw, the same ending 0x0bb2fb / 0x0bc401): frames default to
        // drawdef's (m[0x2b5cc]), nothing at 2 or fewer, the effect runs frames - 2. The cell is w x h
        // pixels (default 32 x 32); speed and spin are tenths (defaults 30.0 and 6.0); from / to
        // (0, 45000) only set where the effect sits among the layers (from is never read). The
        // engine side is cs2.exe kcTaskRenderWBreak (message 0x209 0x5fe510, cells 0x5fda00, each
        // frame 0x5fe640); see StageRenderer::drawBreak.
        const int32_t given = args.size() > 1 ? argNum(args, 1, drawDefFrames_) : drawDefFrames_;
        if (given <= 2) return;
        nextTransition_.kind = Transition::Kind::Break;
        nextTransition_.render = true;
        nextTransition_.frames = static_cast<uint32_t>(given - 2);
        nextTransition_.breakCellW = std::max(1, argNum(args, 2, 32));
        nextTransition_.breakCellH = std::max(1, argNum(args, 3, 32));
        nextTransition_.breakSpeed = args.size() > 4 ? static_cast<float>(argNum(args, 4, 0)) * 0.1f : 30.0f;
        nextTransition_.breakSpin = args.size() > 5 ? static_cast<float>(argNum(args, 5, 0)) * 0.1f : 6.0f;
        return;
    }
    if (cmd == "rwmos") {
        if (skipFlag_) return;  // as `rdraw`
        if (renderTransitionUp()) return;  // as `rdraw`
        // `rwmos <frames> <size> <mode>` (the engine's kcTaskRenderWMosaic): over the first half the
        // old screen breaks into blocks growing from 1 to <size> pixels, over the second half the new
        // one resolves from <size> back to 1. (<mode> 0 is the variant every game here uses.)
        nextTransition_.kind = Transition::Kind::Mosaic;
        nextTransition_.render = true;
        nextTransition_.frames = static_cast<uint32_t>(std::max(0, argNum(args, 1, 30)));
        nextTransition_.mosaic = std::max(1, argNum(args, 2, 10));
        return;
    }
    if (cmd == "rfmos") {
        if (skipFlag_) return;  // as `rdraw`
        if (renderTransitionUp()) return;  // as `rdraw`
        // `rfmos [frames] [size] [mode] [n] [from] [to]` (Happiness 2's sscript 0x081d42, transition
        // 5; the engine's kcTaskRenderFMosaic, cs2.exe 0x5050d0 / 0x504a30 / 0x505200): frames
        // default to drawdef's, nothing at 2 or fewer, the effect runs frames - 2; the block size
        // defaults to 32; from / to (0, 45000) set where it sits among the layers. The old screen
        // breaks into ever larger blocks while it fades out over the new one (StageRenderer).
        const int32_t given = args.size() > 1 ? argNum(args, 1, drawDefFrames_) : drawDefFrames_;
        if (given <= 2) return;
        nextTransition_.kind = Transition::Kind::FadeMosaic;
        nextTransition_.render = true;
        nextTransition_.frames = static_cast<uint32_t>(given - 2);
        nextTransition_.mosaic = std::max(1, args.size() > 2 ? argNum(args, 2, 32) : 32);
        return;
    }
    if (cmd == "wipe" || cmd == "rwipe" || cmd == "wipe2" || cmd == "wipe3") {
        if (cmd == "rwipe" && skipFlag_) return;  // as `rdraw` (0x078e35); the 2D wipes start (`draw` too)
        if (cmd == "rwipe" && renderTransitionUp()) return;  // as `rdraw` (Cyanotype 0x07b0e2)
        // `<wipe> <mask> [frames] ...` (sscript: rwipe 0x07b04d, wipe 0x071def, wipe2 0x0734e2; the
        // same in every game's script): without frames, `wipedef`'s; the wipe runs frames - 2
        // (rwipe: none at 2 or fewer; the 2D wipes: none at 3 or fewer, as `draw`).
        if (args.size() >= 2) {
            const int32_t given = args.size() >= 3 ? argNum(args, 2, wipeDefFrames_) : wipeDefFrames_;
            const int32_t frames = given - 2;
            if (cmd == "rwipe" ? given <= 2 : frames <= 1) return;
            nextTransition_.kind = Transition::Kind::Wipe;
            nextTransition_.mask = args[1];
            nextTransition_.frames = static_cast<uint32_t>(frames);
            nextTransition_.hard = cmd == "wipe2";
            nextTransition_.render = cmd == "rwipe";
            if (cmd != "rwipe") {
                // A 2D wipe is one plane's (`<frames> <plane>`, `wipedef`'s plane without it): that
                // plane's images wipe, the rest of the screen changes at once (as `draw`).
                const size_t at = cmd == "wipe3" ? 4 : 3;  // wipe3 has two lengths (0x073f4a)
                nextTransition_.plane = std::clamp(args.size() > at ? argNum(args, at, wipeDefPlane_) : wipeDefPlane_, 0, 31);
                // The 2D-effect wipes count elapsed time times the `effspd` factor (x1, 1.5, 2, 3, 4)
                // and end once that reaches their length.
                static constexpr float kSpeed[5] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
                float speed = kSpeed[std::clamp(effectSpeed_, 0, 4)];
                nextTransition_.frames = static_cast<uint32_t>(std::ceil(static_cast<float>(nextTransition_.frames) / speed));
            }
            if (!assets_.find(args[1] + ".hg3")) missing(args[1] + ".hg3");
        }
        return;
    }
    if (cmd == "forcemesdel") {
        // `forcemesdel [n]` (Yukikoi's sscript 0x080c71; bare 0): m[0x4cd9c], which keeps a window
        // `frameoff` hid off when a message goes up (0x012e5f). Yukikoi, the one user, turns it off
        // again before each message.
        forceMesDel_ = argNum(args, 1, 0) != 0;
        return;
    }
    if (cmd == "frameon" && dramaVoice_) return;  // drama mode (Cyanotype 0x0709bb; see setDrama)
    if (cmd == "frameon" || cmd == "frameoff") {
        bool on = cmd == "frameon";
        frameoffHidden_ = !on;  // m[0x2866c] (Nekopara 0x06e690 / 0x06e848)
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 2, 0)));
        auto& w = pending_.window;
        int32_t from = shown_.window.alpha.value(frame_);
        // `visible` is the logical state (a message re-shows a hidden window); `alpha` animates it.
        w.alpha = {from, on ? 255 : 0, frame_, frames};
        w.visible = on;
        return;
    }
    if (cmd == "view") {
        // `view [n] [style]` (Amakano 1's sscript 0x07781d, Amakano 2 0x07cc13, Cyanotype
        // 0x07d540): the style is the third word, "default" when there is none (0x077841: the
        // string copied before the arguments are read), passed to the window's change (0x0045ca,
        // config.int/<window xml>/<style>/mes/...). So a bare `view 0` puts the default window back
        // (Amakano 1 and 2 go back to the default window this way).
        const std::string style = args.size() >= 3 ? args[2] : "default";
        // The second word (through the calculator, 1 without it) says how. 1, or a jump (Skip
        // to Next Option, 0x089279), changes the window at once (Cyanotype 0x07d65b: "clear 2").
        // Anything else hands over to main state 17 (0x07d6cb, return -11; every sscript the
        // same): the script waits while the window fades out over 30 frames (MES_ALPHA, 0x0dec2a),
        // changes, shows again (MES_SHOW 1, which a `frameoff` keeps at 0: 0x000c78) and fades in
        // over 30 frames (0x0dedd7); the click's skip flag ends either fade at once. In Cyanotype a
        // `view 0 default` after a letterbox `rwipe` and its `wait` so holds the script until that
        // wipe is over, and the next `rwipe` then runs.
        if (argNum(args, 1, 1) == 1 || jumping_) {
            applyViewStyle(style);
            return;
        }
        commit();
        viewStyle_ = style;
        viewSince_ = frame_;
        viewStep_ = 0;
        block(State::View);
        return;
    }
    if (cmd == "novel") {
        pending_.window.novel = args.size() >= 2 && args[1] == "on";
        return;
    }
    if (cmd == "mesdraw") {
        // sscript keyword 79 (Happiness 2 0x07dba5): the flag m[0x80de8], 1 unless given; turning
        // it off sends MES_CLEAR to the window scripts (the window event below).
        pending_.window.textVisible = argNum(args, 1, 1) != 0;
        windowEvents_.push_back({WindowEvent::Kind::Draw, pending_.window.textVisible, {}, {}});
        return;
    }
    if (cmd == "dic_reg" || cmd == "dic_reset_apend" || cmd == "dic_check_update") {
        // Cyanotype's sscript 0x0828d5 / 0x0829fd / 0x08363f: nothing without a glossary.
        // `dic_reg <n> <id>` unlocks paragraph n of entry <id> (native 871 -> cs2.exe 0x82a630),
        // `dic_reset_apend` clears the "something new" mark (878 -> 0x82a7f0), `dic_check_update`
        // copies it into the apend flag (877 -> +0x4c; native 211 sets the variable).
        if (!dictionary_) return;
        if (cmd == "dic_reg" && args.size() >= 3) dictionary_->reg(argNum(args, 1, 1) - 1, args[2]);
        else if (cmd == "dic_reset_apend") dictionary_->resetApend();
        else if (cmd == "dic_check_update" && dicApendFlag_ != -1) vars_.set(dicApendFlag_, dictionary_->apended() ? 1 : 0);
        return;
    }
    if (cmd == "autosave") {
        autosaveRequested_ = true;
        return;
    }
    if (cmd == "auto") {
        // sscript 0x0574e3: `auto on [base] [char]` sets the pause the scene's auto mode waits
        // (defaults 15 and 5, in autospd_b / autospd_c units); `auto off` ends it.
        // A 5th number (Yukikoi 0x076b16 -> m[0x2bba4], 1 by default and on every `auto`) only acts
        // at 0 (0x01614a, 0x0887c8, 0x088acf, ... test `scene auto && !it`); the one user, Yukikoi,
        // passes 1 (`auto on 0 0 0 1`), so it is not kept.
        autoMode_ = args.size() >= 2 && args[1] == "on";
        autoBase_ = autoMode_ && args.size() >= 3 ? argNum(args, 2, 15) : 15;
        autoChar_ = autoMode_ && args.size() >= 4 ? argNum(args, 3, 5) : 5;
        // The 4th argument (0x075245, Yukikoi 0x076aa8 the same) is the window's text speed:
        // `auto on 0 0 0 1` shows the text at once until `auto off` puts messpd back. Without it
        // the speed stays as it was.
        if (!autoMode_) autoSpeed_ = -1;
        else if (args.size() >= 5 && argNum(args, 4, -1) != -1) autoSpeed_ = argNum(args, 4, -1);
        return;
    }
    if (cmd == "next") {
        callStack_.clear();
        if (args.size() >= 2) jump(args[1]);
        return;
    }
    if (cmd == "call" && args.size() >= 2) {
        // `call <scene>` (Amakano 2: 1171 two-to-twelve-line scenes that put up an event CG and
        // register it): run that scene, then carry on after the call when it ends.
        callStack_.push_back({std::move(*scene_), sceneName_, pc_});
        jump(args[1]);
        if (state_ == State::Ended) {  // no such scene: carry on here
            scene_ = std::move(callStack_.back().scene);
            sceneName_ = callStack_.back().name;
            pc_ = callStack_.back().pc;
            callStack_.pop_back();
            state_ = State::Running;
        }
        return;
    }
    if (cmd == "rand" && args.size() >= 4) {
        // `rand <var> <min> <max>`: a random whole number in [min, max] into scene variable #<var>.
        vars_.set(num(args[1]), randomBetween(num(args[2]), num(args[3])));
        return;
    }
    if (cmd == "fselect") {
        // `fselect [screen]`: the choices that follow, shown by that FE script if one is named.
        collectingChoices_ = true;
        pending_.choices.clear();
        pending_.choiceScript = args.size() >= 2 ? args[1] : std::string();
        return;
    }
    if (cmd == "scenetitle") {
        // `scenetitle [text]` (Nekopara's sscript 0x082517): the scene title that saves carry (record
        // 0x201), `#id#text` resolved by the language's option table when saved; none clears it.
        std::string t;
        for (size_t i = 1; i < args.size(); ++i) t += (i > 1 ? " " : "") + args[i];
        title_ = lineUtf8_ ? t : sjisToUtf8(t);
        return;
    }
    if (cmd == "scene") {
        std::string t;
        for (size_t i = 1; i < args.size(); ++i) t += (i > 1 ? " " : "") + args[i];
        title_ = cleanLabel(t);
        return;
    }
    if (cmd == "str") {
        if (args.size() >= 2) {
            std::string v;
            for (size_t i = 2; i < args.size(); ++i) v += (i > 2 ? " " : "") + args[i];
            vars_.setStr(argNum(args, 1, 0), v);
        }
        return;
    }
    if (cmd == "bgm" || cmd == "se" || cmd == "pcm") {
        soundCommand(cmd, args);
        return;
    }
    if (cmd == "pl") {
        planeCommand(args);
        return;
    }
    if (cmd == "cam") {
        cameraCommand(args);
        return;
    }
    if (cmd == "mpl") {
        emoteCommand(args);
        return;
    }
    if (cmd == "fmpl") {
        // `fmpl <n> ...`: the window face's E-mote bank, sscript's object 75 + n in Nekopara (0x06f143;
        // Amakano's 77 + n), which is the same object as `mpl <first face bank + n>`; bare `fmpl`
        // clears the face banks. vram.xml's mpl08 / mpl09 put them at the window's face (120, 540).
        const int32_t base = layout_.mplFaceFrom;
        if (args.size() < 2) {
            std::vector<int32_t> banks;
            for (const auto& [i, img] : pending_.layer(Layer::Mpl)) {
                if (i >= base) banks.push_back(i);
            }
            for (int32_t i : banks) emoteCommand({"mpl", std::to_string(i)});
            return;
        }
        std::vector<std::string> one = args;
        one[0] = "mpl";
        const std::string& a1 = args[1];
        if (a1.size() > 2 && a1.front() == '[' && a1.back() == ']') {
            std::string list = "[";
            std::string inner = a1.substr(1, a1.size() - 2);
            for (size_t start = 0; start <= inner.size();) {
                size_t comma = inner.find(',', start);
                std::string item = inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = comma == std::string::npos ? inner.size() + 1 : comma + 1;
                if (item.empty()) continue;
                list += (list.size() > 1 ? "," : "") + std::to_string(num(item) + base);
            }
            one[1] = list + "]";
        } else if (isNumberLike(a1)) {
            one[1] = std::to_string(argNum(args, 1, 0) + base);
        } else {
            one.insert(one.begin() + 1, std::to_string(base));  // `fmpl m2body ...`: bank 0
        }
        emoteCommand(one);
        return;
    }
    if (cmd == "particle3" || cmd == "particle" || cmd == "particle2") {
        // One set of slots for the three (Happiness 2's sscript: m[4 + 396 n] in 0x0757b5, 0x077c5e
        // and 0x07fda2 alike). `particle` and `particle2` load a script, ptcl/<name>.kcs (record +368
        // 0: the per-frame step 0x09f474 sends native 271, message 0x140 with no plugin); Amakano 2
        // ships none ("particle script not found", 0x587376), so its `particle2` shows nothing.
        particleCommand(args);
        return;
    }
    if (cmd == "movie") {
        if (args.size() >= 2) {
            commit();
            // `movie <name> [mode] [volume]` (sscript): mode bit 2 lets a left click end it, bit 1
            // a right click (default 2); volume in percent (default 100).
            MediaCommand m{MediaCommand::Kind::PlayMovie, std::clamp(args.size() > 2 ? argNum(args, 2, 2) : 2, 0, 3), args[1]};
            m.volume = std::clamp(args.size() > 3 ? argNum(args, 3, 100) : 100, 0, 100);
            media_.push_back(m);
            block(State::Movie);
        }
        return;
    }
    if (cmd == "fesds" && args.size() >= 5) {
        // `fesds <slot> <a> <b> <script> <n>...` (Amakano 1's sscript 0x07cd4a; Amakano 2's and
        // Happiness 2's the same): slot 0-3 (a record of 140 bytes at 0x5bda8: the name, the task,
        // a, b, 16 numbers). The script running there ends (native 67). With a = 0 nothing starts
        // during Skip to Next Option or a return to a backlog line (main states 2 / 25). Then the
        // script runs in an FE task of its own (native 66 type 34; 449 puts it on one of two layers
        // by b, 447 loads it, 448 starts it), the numbers after its name in its \0.. (native 645),
        // beside the story until it exits: Amakano 1 slides a date card in this way for 90 frames,
        // Happiness 2 a place card. Its start runs now, before the
        // scene goes on (Amakano 2's costume checks set the face parts the next line draws). It
        // used to run that once and stop, so neither card showed.
        const int32_t slot = argNum(args, 1, -1);
        if (slot < 0 || slot >= static_cast<int32_t>(displayScripts_.size())) return;
        displayScripts_[static_cast<size_t>(slot)].reset();
        if (jumping_ && argNum(args, 2, 0) == 0) return;
        if (auto ds = FesInstance::load(assets_, vars_, args[4], frame_)) {
            ds->log = log;
            for (size_t k = 5; k < args.size() && k < 5 + 16; ++k) ds->setLocal(static_cast<int32_t>(k - 5), argNum(args, k, 0));
            ds->tick(frame_);
            for (auto& m : ds->takeMedia()) media_.push_back(std::move(m));
            if (!ds->exited()) displayScripts_[static_cast<size_t>(slot)] = std::move(ds);
        } else {
            missing(args[4] + ".fes");
        }
        return;
    }
    if (cmd == "setfbit" || cmd == "clearfbit") {
        // `setfbit <base> <n>...` (Yukikoi's sscript 0x07e17a; `clearfbit` 0x07e34e): for each n,
        // bit n % 32 of variable base + n / 32 set (cleared), read and written through natives 210
        // / 211, the game's variables. Yukikoi's scenes mark its H scenes in #711-#715, which the
        // replay screen tests (omake_hscene.fes `$(711+\1)&(1<<\0)`).
        if (args.size() < 2) return;
        const int32_t base = argNum(args, 1, 0);
        for (size_t i = 2; i < args.size(); ++i) {
            const int32_t n = argNum(args, i, 0);
            const int32_t var = base + n / 32;
            const uint32_t bit = 1u << (static_cast<uint32_t>(n % 32) & 31);
            const uint32_t v = static_cast<uint32_t>(vars_.get(var));
            vars_.set(var, static_cast<int32_t>(cmd == "setfbit" ? v | bit : v & ~bit));
        }
        return;
    }
    if (cmd == "getfbit") {
        // `getfbit <var> <base> <n>` (0x07df31): #var = that bit, 1 or 0; nothing unless all three are given.
        if (args.size() < 4) return;
        const int32_t n = argNum(args, 3, 0);
        const uint32_t v = static_cast<uint32_t>(vars_.get(argNum(args, 2, 0) + n / 32));
        vars_.set(argNum(args, 1, 0), (v >> (static_cast<uint32_t>(n % 32) & 31)) & 1u ? 1 : 0);
        return;
    }
    if (cmd == "emoinfo_getinfo") {
        // `emoinfo_getinfo <mark> <size> <dx> <dy> <xflag> <yflag> <scaleflag> <strvar>` (Happiness 2
        // sscript 0x084aa9 -> native 1056 -> cs2.exe 0x55d7a0): emodef.xml's size entry and the mark
        // of that name; x = (pos.x + dx) * size / 100, y likewise, scale = texscale * size / 100 *
        // the mark's scale / 100, doubled with the half-size texture (tex/s) when under 50 and the
        // mark has one, else tex/l. An unknown mark gives "dummy" (and 0 0 0).
        if (args.size() < 9) return;
        const auto& emo = layout_.emo;
        int32_t x = 0, y = 0, scale = 0;
        std::string tex = "dummy";
        auto size = emo.sizes.find(argNum(args, 2, 0));
        auto mark = std::find_if(emo.marks.begin(), emo.marks.end(), [&](const auto& m) { return m.name == args[1]; });
        if (size != emo.sizes.end() && mark != emo.marks.end()) {
            x = (mark->x + argNum(args, 3, 0)) * size->second / 100;
            y = (mark->y + argNum(args, 4, 0)) * size->second / 100;
            scale = emo.texScale * size->second / 100 * mark->scale / 100;
            if (scale < 50 && mark->half) {
                scale *= 2;
                tex = mark->small;
            } else {
                tex = mark->large;
            }
        }
        // Natives 211 / 276 store them (only into flags above 0).
        auto put = [&](size_t i, int32_t v) {
            if (const int32_t f = argNum(args, i, 0); f > 0) vars_.set(f, v);
        };
        put(5, x);
        put(6, y);
        put(7, scale);
        if (const int32_t sv = argNum(args, 8, -1); sv >= 0) vars_.setStr(sv, tex);
        return;
    }
    if (cmd == "emoinfo_getoffset") {
        // `emoinfo_getoffset <character> <pose> <mark id> <xflag> <yflag>` (0x084f04 -> native 1061 ->
        // cs2.exe 0x55d960): the offset the character's pose table (emodef.xml ch / p / table) gives
        // that mark; 0 0 without one.
        if (args.size() < 6) return;
        int32_t x = 0, y = 0;
        const auto& offsets = layout_.emo.offsets;
        if (auto t = offsets.find({argNum(args, 1, 0), argNum(args, 2, 0)}); t != offsets.end()) {
            if (auto e = t->second.find(argNum(args, 3, 0)); e != t->second.end()) x = e->second.first, y = e->second.second;
        }
        if (const int32_t f = argNum(args, 4, 0); f > 0) vars_.set(f, x);
        if (const int32_t f = argNum(args, 5, 0); f > 0) vars_.set(f, y);
        return;
    }
    if (cmd == "getscenestep") {
        // `getscenestep <flag>` (Happiness 2 sscript 0x085440, keyword 129): the flag gets the
        // scene's step, the count of waits for input passed (the line pointer's +36, which each
        // advance raises; 0x014b48), i.e. the input and page lines before this one.
        if (args.size() < 2 || !scene_) return;
        int32_t step = 0;
        const auto& lines = scene_->lines();
        for (size_t i = 0; i + 1 < pc_ && i < lines.size(); ++i) {
            if (lines[i].type == CstLineType::Input || lines[i].type == CstLineType::Page) ++step;
        }
        vars_.set(argNum(args, 1, 0), step);
        return;
    }
    if (cmd == "rs") {
        // `rs <n> <params...>` (Amakano 2 sscript 0x084851, keyword 128): resident script n (0-3) gets
        // the parameters; Game passes them on (RS_COMMAND).
        if (args.size() < 2) return;
        ResidentCall c;
        c.slot = argNum(args, 1, -1);
        c.params.assign(args.begin() + 2, args.end());
        c.utf8 = lineUtf8_;
        residentCalls_.push_back(std::move(c));
        return;
    }
    if (cmd == "callmestag") {
        // `callmestag <label>`: the message window's script runs that label (WindowScript::callLabel).
        // Hatsumira's full-screen window script switches its text to the font file in a string
        // variable there, after a `strcmp` sets a flag.
        if (args.size() >= 2) windowCalls_.push_back(args[1]);
        return;
    }
    if ((cmd == "max" || cmd == "min") && args.size() >= 2) {
        // `max <var> <a> <b> ...` / `min ...` (Amakano 1's sscript 0x077201 / 0x0773af, keywords 68 /
        // 69): the place, from 0, of the largest (smallest) of the values goes into #var. The best starts at INT_MIN (INT_MAX) and a value replaces it only when strictly
        // larger (smaller), so a tie keeps the first; each word through the calculator (native 213),
        // the result by native 211. Amakano 1 picks each heroine's part of the common route this way
        // (`max` over the heroines' points, then an `if` on the result before each `next`).
        // Unhandled, the result stayed 0: every route was the first heroine's.
        const bool largest = cmd == "max";
        int32_t best = largest ? INT32_MIN : INT32_MAX, place = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            const int32_t v = argNum(args, i, 0);
            if (largest ? best < v : best > v) best = v, place = static_cast<int32_t>(i - 2);
        }
        vars_.set(argNum(args, 1, 0), place);
        return;
    }
    if (cmd == "voiceoff") {
        // `voiceoff [n]` (REBELLIONS's sscript 0x06e5a3, keyword 98): with a number (through the
        // calculator), entry n of a 50-entry table is set; bare, the whole table is cleared. A
        // voice about to play (0x08f5db) is looked up (native 844: its startup.xml <VOICE> entry,
        // counted from v00 with BGM and SE; REBELLIONS's `voiceoff 6` is v06, a character whose
        // voice some of its replays turn off) and plays only if its entry is clear and the
        // config lets that character speak (native 606). A third word (`voiceoff 6 0`) is not read.
        if (args.size() < 2) {
            voiceMuted_ = 0;
        } else if (const int32_t n = argNum(args, 1, -1); n >= 0 && n < 50) {
            voiceMuted_ |= uint64_t{1} << n;
        }
        return;
    }
    if (cmd == "strn" && args.size() >= 2) {
        // `strn <n> [value]` (REBELLIONS's sscript 0x0643fa, keyword 85): $str<n> takes the value
        // as decimal text (sprintf "%d", native 14; native 276), 0 without one; each word through the
        // calculator. REBELLIONS writes its dates this way.
        vars_.setStr(argNum(args, 1, 0), std::to_string(args.size() > 2 ? argNum(args, 2, 0) : 0));
        return;
    }
    if (cmd == "bs_clear") {
        // `bs_clear` (Amakano 1's sscript 0x07da1c, keyword 116): closes the point a choice (or `fex
        // sel`) made for Previous Option (m[0x27bc4], made by 0x0a7446) and resets its record; Amakano 1
        // runs it as a heroine's route is settled, so the way back before it is gone.
        lastChoice_.reset();
        choiceShown_.reset();
        return;
    }
    if (cmd == "skipstop") {
        // `skipstop` (Amakano 1's sscript 0x07b621, keyword 95): MES_PUSH 0 0, and skip mode off
        // (m[0x80000098] = 0, after MES_MODE 0 when it was on). Force Skip, a held key, goes on.
        skipStop_ = true;
        return;
    }
    if (cmd == "strcmp" && args.size() >= 3) {
        // `strcmp <var> <a> [b]`: 0 when the two texts match (as C's strcmp), into #<var>.
        int c = args[2].compare(args.size() > 3 ? args[3] : std::string());
        vars_.set(argNum(args, 1, 0), c < 0 ? -1 : c > 0 ? 1 : 0);
        return;
    }
    if (cmd == "fex" && args.size() >= 2 && args[1] == "end") return;  // the screen has exited by now
    if (((cmd == "fesjs" || cmd == "fes") && args.size() >= 2) || (cmd == "fex" && args.size() >= 3)) {
        // Run an FE script (eyecatch, mini-screen) over the stage until it exits. `fex <mode>
        // <script> [n]` (Amakano 2's name entry and map screens)
        // does the same; its `fex end` follows once the script is done. `fes <script> <n>...`
        // (Happiness 2's sscript 0x079634, keyword 53) is `fesjs` (0x0797a0) without its test
        // (0x089279: none during Skip to Next Option, request state 2 or 25): both keep the name
        // and up to 16 numbers and raise request 14, then wait (-11). Happiness 2's route endings
        // (the staff roll and information screens), Amakano 1's name entry.
        // Skip to Next Option passes `fesjs` by (0x089279, ahead of the keyword's own work).
        if (cmd == "fesjs" && jumping_) return;
        const std::string& name = cmd == "fex" ? args[2] : args[1];
        commit();
        overlay_ = fesLoader_ ? fesLoader_(name, frame_) : FesInstance::load(assets_, vars_, name, frame_);
        if (!overlay_) {
            missing(name + ".fes");
            return;
        }
        overlay_->log = log;
        // `fesjs <script> <n>...`: up to 16 numbers for the script's \0, \1, ... (sscript 0x0774d9
        // keeps them for the screen it starts; Nekopara's 18 eyecatches pick their picture with
        // `cg setid \0`).
        if (cmd == "fesjs" || cmd == "fes") {
            for (size_t i = 2; i < args.size() && i < 18; ++i) overlay_->setLocal(static_cast<int32_t>(i - 2), argNum(args, i, 0));
        }
        overlayLine_ = pc_ - 1;
        overlayFex_ = cmd == "fex";
        overlayKind_ = cmd != "fex" ? ScreenKind::Fes : args[1] == "sel" ? ScreenKind::FexSel : ScreenKind::Fex;
        fexVar_ = overlayKind_ == ScreenKind::FexSel ? argNum(args, 3, 0) : 0;
        screenOpened_ = overlayKind_;
        block(State::Fes);
        // `fex sel` (Amakano 2's maps) makes the Previous Option point as `select` does: both run
        // sscript's 0x0aff49 for their line (Amakano 0x07d6db / 0x076098).
        overlayChoice_ = cmd == "fex" && args[1] == "sel";
        if (overlayChoice_) choiceShown_ = capture();
        return;
    }
    // Commands that are intentionally no-ops for now (bookkeeping, debug, skip control).
    if (cmd == "end_of_kaisou") {
        // `end_of_kaisou [n]`, the end of a replayable part (Yukikoi's sscript 0x077046; Fruit
        // 0x057a93, Nekopara 0x0757ca, Amakano 2, Cyanotype, Labyrinth, Hatsumira, LSG the same):
        // with n (through the calculator, native 213) variable kaisou + n is set to 1 (native 211),
        // the part seen, which the extras' replay lists read (the variables from kaisou on); then the global data is written (0x0afb4c:
        // savegen.dat, dic.dat). If sscript booted as a replay (mode -2 / -4) the story ends here
        // (return -13) and the flow script comes back; otherwise play goes on.
        if (args.size() >= 2) vars_.set(kaisouBase_ + argNum(args, 1, 0), 1);
        globalSaveRequested_ = true;
        if (replaying_) {
            commit();
            block(State::Ended);
        }
        return;
    }
    if (cmd == "cgreg" && args.size() >= 2) {
        // `cgreg <name>` (Nekopara's sscript 0x0758cc): native 422 with the name (registerCg).
        registerCg(args[1]);
        return;
    }
    if (cmd == "cgflag" && args.size() >= 2) {
        // `cgflag <group>` (0x077c4b): native 459 (cs2.exe 0x652b60 -> 0x790690), the group of that
        // name (case-insensitive) in cglist.dat's table 0, each entry recorded; nothing for an
        // unknown name.
        const int32_t g = cgGroupIndex(args[1]);
        if (g >= 0) {
            for (const auto& entry : cgList_[static_cast<size_t>(g)].images) registerCg(entry, false);
        }
        return;
    }
    if (cmd == "sysbtn") {
        // `sysbtn [n [0/1]]` (sscript 0x0807f6): bare, every button on; `n` alone, on; else on/off.
        if (args.size() < 2) {
            sysButtons_.fill(true);
        } else {
            int32_t n = argNum(args, 1, -1);
            if (n >= 0 && n < static_cast<int32_t>(sysButtons_.size())) {
                sysButtons_[static_cast<size_t>(n)] = args.size() < 3 || argNum(args, 2, 1) != 0;
            }
        }
        return;
    }
    if (cmd == "initialize_header") return;  // sscript 0x07d0a5: a debug log only (mode 2)
    // `cache <image>` (Happiness 2's sscript 0x07f601, keyword 89): the name to native 681, which
    // reads the picture ahead; nothing shows (Nyanroid loads pictures as they are used).
    if (cmd == "cache") return;
    // `pg <n>` (Fruit's sscript 0x05c6ed, keyword 61): the object family 0 of the handler
    // 0x03d337 that `cm` / `lt` / `mt` use too; bare `pg 3` clears its slot 3. No scene of any game
    // makes one (one of Fruit's scenes has the only `pg`, a bare one), so it changes nothing.
    if (cmd == "pg" && args.size() <= 2) return;
    // Nekopara's scene headers (sscript keyword table m[0x46650]); each puts back what it starts as:
    // `tonetbl on` (73, 0x07b4a6) and `thumbnail 1` (95, 0x07f165) set flags the boot sets to 1
    // (0x0e6ddb / 0x0e6ef9): tone tables used for `tone` and a thumbnail kept with saves; bare
    // `fesds` / `fesds <n>` (111, 0x08092c) close the display-script slots, which these scenes never
    // open (the `fesds <n> 1 0 <script> 0` form is below).
    if (cmd == "tonetbl") {
        // sscript 0x07b4a6: bare or `on` 1, `off` 0, else the number.
        const std::string v = args.size() > 1 ? toLowerAscii(args[1]) : "on";
        toneTableOn_ = v == "on" ? true : v == "off" ? false : argNum(args, 1, 1) != 0;
        return;
    }
    if (cmd == "fesds" && args.size() <= 2) {
        // Bare `fesds` / `fesds <n>` (0x08092c): the slots' scripts end (native 67), all or slot n.
        for (size_t i = 0; i < displayScripts_.size(); ++i) {
            if (args.size() < 2 || static_cast<int32_t>(i) == argNum(args, 1, -1)) displayScripts_[i].reset();
        }
        return;
    }
    if (cmd == "thumbnail") return;
    // `steam achievement <id>` (122): Steam only.
    if (cmd == "steam") return;
    if (cmd == "cancellabel") {
        // `cancellabel [scene]` (123, 0x08266f): copied into sscript's buffer (400300); bare, cleared.
        cancelLabel_ = args.size() >= 2 ? args[1] : std::string();
        return;
    }
    // `baseimg <n>` (53, 0x077683): copies the screen under plane 27 + n (its place and size, less
    // its shift) into the base image that FE scripts start from (native 455). Nekopara calls it on
    // the white screen before each eyecatch, whose script wipes away from it (`rwipe`); our FE wipes
    // start from the frame last shown, that same screen.
    if (cmd == "baseimg") return;
    // `sceneskip <scene> [label]` / `sceneskip end` (Amakano's sscript 0x084b5f): notes a scene the
    // player may skip to and, when the `use_sceneskip` setting is on (native 1078), stops for the
    // scene-skip offer (request state 29). Only debug scenes use it.
    if (cmd == "sceneskip") return;
    if (cmd == "jumpstop") {
        jumpStop_ = true;
        return;
    }
    if (cmd == "keyskip") {
        // `keyskip [off|ctrl]` (Nekopara's sscript 0x06e894, Fruit 0x0511e4): 1 unless `off` (0)
        // or `ctrl` (2, skipping only with the skip key held); the skip flag goes down (0x06e943).
        keySkip_ = 1;
        if (args.size() >= 2 && args[1] == "off") keySkip_ = 0;
        else if (args.size() >= 2 && args[1] == "ctrl") keySkip_ = 2;
        setSkipFlag(false, "keyskip");
        return;
    }
    if (cmd == "undo") {
        // `undo`: Previous Option returns here (Fruit's sscript keyword 89, 0x062d10 -> 0x060145:
        // the old return point is released and the current position taken). Taken at the next
        // message, where the story can be resumed.
        startPointPending_ = true;
        return;
    }
    // Bare `place` (0x0775e5 -> 0x0b90dd) hides the place-name display, which never shows.
    if (cmd == "autoface") {
        // `autoface on|off|<n>` (Yukikoi's sscript 0x07d2ee, Cyanotype 0x07dd27): 0 first, then `on`
        // 1, `off` 0, else the number (bare: 0). See the message line for what it does.
        const std::string v = args.size() > 1 ? toLowerAscii(args[1]) : std::string();
        autoFace_ = v.empty() ? false : v == "on" ? true : v == "off" ? false : argNum(args, 1, 0) != 0;
        return;
    }
    static const char* kIgnored[] = {"cgflag", "cgreg",
                                     "place", "set_bgm_flag", "title"};
    for (const char* k : kIgnored) {
        if (cmd == k) {
            if (cmd == "title") {
                commit();
                block(State::Ended);
            }
            return;
        }
    }
    unhandled(cmd);
}

namespace {
// sscript's object sub-command table (Fruit m[0x355cc], Amakano m[0x4cd90]): a word from it where a
// channel or file name could stand is a sub-command.
bool kOpsAll(const std::string& op) {
    static const std::set<std::string> kSub = {
        "vram", "base", "pos", "rot", "rotc", "rot_ypr", "scale", "color", "vscale", "blur", "clone", "arc",
        "fade", "cfade", "move", "quake", "spline", "spline_s", "spline_r", "mrot", "mrotc", "mrot_ypr",
        "mscale", "shake", "vmove", "mvscale", "amove1", "amove2", "amove3", "pend", "fade2", "vol", "pause",
        "replay", "def", "blend", "disp", "pri", "stop", "mode", "tone", "loop", "end", "after", "bmode",
        "raster", "persmode", "shift", "video", "vcom", "delay", "attr", "attr_def", "anm", "anmset",
        "anmframe", "attach", "loc", "skip", "plmode", "command", "action", "wait", "camera", "topmask",
        // The rest of Fruit's table (registered at 0x011858 onward).
        "mcrot", "mcrotc", "mcrot_ypr", "mcscalecos", "mcscalesin", "mcquake", "mcshake", "mcspline",
        "mcspline_s", "mcspline_r", "mcmove", "mcfade", "mccfade", "mcarc", "mcpend", "mcdisp", "m2arc",
        "m2fade", "m2cfade", "m2move", "m2quake", "m2spline", "m2spline_s", "m2spline_r", "m2rot", "m2rotc",
        "m2rot_ypr", "m2scale", "m2shake", "m2vmove", "m2vscale", "m2pend", "actcnv", "autosave"};
    return kSub.count(op) != 0;
}
}  // namespace

// sscript's one sound command (Nekopara 0x0546bd, the same code in Labyrinth, Eden, Himawari,
// Hatsumira and LSG; Fruit 0x037e80): `bgm`, `se` and `pcm` are kinds 0, 2 and 12, each channel a
// sound record (kind + channel: 2 BGM, 10 SE and 10 voice records; Amakano 2 has 11 voices). Tokens
// count from the command word:
//   <kind>                    every channel of the kind stops at once
//   <kind> <sub> ...          a sub-command for every channel (only `vol` takes it; `fade` / `end`
//                             without a channel are a "param error")
//   <kind> <name> ...         a name (not starting with a digit) plays on the channel of a counter
//                             shared by all three kinds
//   <kind> <n>                channel n stops at once
//   <kind> <n> <sub> ...      a sub-command for channel n
//   <kind> <n> <name> ...     the name plays on channel n
void ScenePlayer::soundCommand(const std::string& kind, const std::vector<std::string>& args) {
    using K = MediaCommand::Kind;
    const int k = kind == "bgm" ? 0 : kind == "se" ? 1 : 2;
    auto digit = [](const std::string& s) { return !s.empty() && std::isdigit(static_cast<unsigned char>(s[0])); };
    auto stopKind = [&](int32_t ch) {
        media_.push_back({k == 0 ? K::StopBgm : k == 1 ? K::StopSe : K::StopVoice, ch});
        auto& playing = k == 0 ? bgm_ : se_;
        if (k != 0) {
            if (ch >= 0) soundUntil_.erase({k, ch});
            else std::erase_if(soundUntil_, [&](const auto& e) { return e.first.first == k; });
        }
        if (k == 2) {
            if (ch < 0) voiceChannels_.clear();
            else voiceChannels_.erase(ch);
        } else if (ch < 0) {
            playing.clear();
        } else {
            playing.erase(ch);
        }
    };
    if (args.size() < 2) {
        stopKind(-1);
        return;
    }
    if (kOpsAll(args[1])) {
        soundSub(k, -1, args[1], args, 1);
    } else if (!digit(args[1])) {
        soundPlay(k, autoChannel_, false, args[1], args, 2);
    } else {
        const int32_t ch = static_cast<int32_t>(std::atoi(args[1].c_str()));
        if (args.size() < 3) stopKind(ch);
        else if (kOpsAll(args[2])) soundSub(k, ch, args[2], args, 2);
        else soundPlay(k, ch, true, args[2], args, 3);
    }
}

// sscript 0x054c20 (Fruit 0x0383e3): a sound by name on a channel. With the channel given, the
// counter moves on to the next one (not for voices) and the channel stops first (0x0a0b08: its
// sound and motions end, its `vol` level stays); without one, the counter's channel is taken and the
// counter moves on (after 9, back to 0). A name starting with a digit is `bgm_%02d` (no game has
// such files: the channel only stops). A BGM already playing on its channel is left to play on.
// BGM loops (+184 = 0), the others play once. A number after the name is its pan (0x09fdf2: the
// position (p / 100, 0, sqrt(1 - (p / 100)^2)) in front, panned 100 sin(atan2(x, z)) as `pos`); the
// newer sscripts read a second number into +240 (frames, handed on in milliseconds to the sound task;
// not applied), Fruit's read two or three as the position. A voice whose file is missing does not
// play and leaves the others alone; one that starts ends every other voice (0x0a4e3d).
void ScenePlayer::soundPlay(int k, int32_t ch, bool given, std::string name, const std::vector<std::string>& args,
                            size_t next) {
    using K = MediaCommand::Kind;
    if (given) {
        if (k != 2) autoChannel_ = autoChannel_ >= 9 ? 0 : ch + 1;
    } else {
        ch = autoChannel_;
        autoChannel_ = autoChannel_ >= 9 ? 0 : autoChannel_ + 1;
    }
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "bgm_%02d", std::atoi(name.c_str()));
        name = buf;
    }
    if (k == 0 && given) {
        auto it = bgm_.find(ch);
        if (it != bgm_.end() && it->second == name) return;
    }
    const bool exists = assets_.find(name + ".ogg").has_value();
    if (!exists) missing(name + ".ogg");
    if (given) {
        // The channel stops before its new sound (a voice's channel too, even if the file is missing).
        media_.push_back({k == 0 ? K::StopBgm : k == 1 ? K::StopSe : K::StopVoice, ch});
        if (k == 0) bgm_.erase(ch);
        if (k == 1) se_.erase(ch);
        soundUntil_.erase({k, ch});
    }
    soundPos_.erase({k, ch});  // a new sound starts centred
    auto lv = channelVolume_.find({k, ch});
    const int32_t level = lv != channelVolume_.end() ? lv->second : 100;
    if (k == 2) {
        if (!exists) {
            voiceChannels_.erase(ch);
            return;
        }
        // The other voices end (0x0559e8 -> 0x0a4e3d: each voice channel's sound stops, 0x0a12ed),
        // but not one only asked for in this frame: a request marks its channel (record +0 bit 0,
        // 0x055169), the frame's sound pass starts it (0x087624 on), and the stop leaves the mark.
        // So the voices of one line start together (Yukikoi voices two characters' line together this way; Yukikoi 0x0572ae and Fruit 0x038efc
        // the same).
        uint32_t spare = 0;
        for (auto it = voiceChannels_.begin(); it != voiceChannels_.end();) {
            if (it->first != ch && it->second.second == frame_ && it->first >= 0 && it->first < 32) {
                spare |= 1u << it->first;
                ++it;
            } else {
                it = voiceChannels_.erase(it);
            }
        }
        if (drama_ && (!voiceAllowed || voiceAllowed(name))) dramaVoice_ = true;  // see setDrama
        MediaCommand c{K::PlayVoice, ch, name};
        c.volume = level;
        c.spareVoices = spare;
        media_.push_back(c);
        pendingVoice_ = name;  // only a voice that exists makes the line voiced
        lastVoice_ = name;
        lastVoiceFrame_ = frame_;
        voiceChannels_[ch] = {name, frame_};
        linkLips(ch, name);
        std::erase_if(soundUntil_, [&](const auto& e) {
            return e.first.first == 2 && !(e.first.second >= 0 && e.first.second < 32 && (spare >> e.first.second & 1));
        });
        soundUntil_[{2, ch}] = frame_ + voiceLength(name);
        // The banks whose image matches the voice's sndanm.xml channel follow it with their mouths.
        if (const auto* lc = emoteProfiles_.lipChannel(name)) {
            int32_t givenBanks = 0;
            for (auto& [i, img] : pending_.layer(Layer::Mpl)) {
                if (givenBanks >= lc->limit) break;
                std::string spec = img.spec;
                for (char& c2 : spec) c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(c2)));
                if (img.emote.empty() || !spec.starts_with(lc->image)) continue;
                ++givenBanks;
                if (!img.emoteLip) continue;
                img.emoteVoice = name;
                img.emoteVoiceStart = frame_;
            }
        }
    } else {
        MediaCommand c{k == 0 ? K::PlayBgm : K::PlaySe, ch, name, k == 0};
        c.volume = level;
        media_.push_back(c);
        if (k == 0) bgm_[ch] = name;
        if (k == 1 && exists) soundUntil_[{1, ch}] = frame_ + voiceLength(name);
        auto& volumes = k == 0 ? bgmVolume_ : seVolume_;
        if (level != 100) volumes[ch] = level;
        else volumes.erase(ch);
    }
    placeSound(k, ch, args, next, olderSound_ && args.size() > next + 1);
}

// Where a sound sits from the numbers at `from`: one is a pan (0x09fdf2), `position` reads up to
// three as x, y, z (Fruit's play, every sscript's `loop` with a third number).
void ScenePlayer::placeSound(int k, int32_t ch, const std::vector<std::string>& args, size_t from, bool position) {
    if (args.size() <= from) return;
    auto& p = soundPos_[{k, ch}];
    p.start = frame_;
    p.frames = 0;
    if (position) {
        for (int i = 0; i < 3; ++i) p.from[i] = p.to[i] = static_cast<float>(argNum(args, from + static_cast<size_t>(i), 0));
        return;
    }
    const float s = std::clamp(static_cast<float>(argNum(args, from, 0)) * 0.01f, -1.0f, 1.0f);
    const float a = std::asin(s);
    p.from[0] = p.to[0] = std::sin(a);
    p.from[1] = p.to[1] = 0;
    p.from[2] = p.to[2] = std::cos(a);
}

// The sound sub-commands (Nekopara 0x055d07: base, pos, rot, vol, arc, fade, move, quake, mrot,
// shake, loop, end, pause, replay, lipbank; any other word does nothing). `ch` < 0: none was given.
void ScenePlayer::soundSub(int k, int32_t ch, const std::string& op, const std::vector<std::string>& args, size_t at) {
    using K = MediaCommand::Kind;
    const int32_t count = k == 0 ? 2 : 10;
    const K volumeKind = k == 0 ? K::BgmVolume : k == 1 ? K::SeVolume : K::VoiceVolume;
    const K stopKind = k == 0 ? K::StopBgm : k == 1 ? K::StopSe : K::StopVoice;
    auto* volumes = k == 0 ? &bgmVolume_ : k == 1 ? &seVolume_ : nullptr;
    if (op == "vol") {
        // 0x0572be: the level (100 without a number) at once, for the channel or every channel of the
        // kind; it stays the channel's level for its later sounds (+208).
        const int32_t v = argNum(args, at + 1, 100);
        MediaCommand c{volumeKind, ch};
        c.volume = v;
        media_.push_back(c);
        for (int32_t c2 = ch < 0 ? 0 : ch; c2 <= (ch < 0 ? count - 1 : ch); ++c2) {
            channelVolume_[{k, c2}] = v;
            if (volumes) (*volumes)[c2] = v;
        }
    } else if (op == "fade") {
        // 0x057da7: motFade over <frames> (100) from <a> (0) to <b> (0); the sound keeps playing at
        // the end, silent at 0 (Cyanotype fades its music to 0 and later back up). A channel is needed.
        if (ch < 0) return;
        MediaCommand c{volumeKind, ch};
        c.fadeFrames = static_cast<uint32_t>(std::max(0, argNum(args, at + 1, 100)));
        c.fromVolume = std::clamp(argNum(args, at + 2, 0), 0, 100);
        c.volume = std::clamp(argNum(args, at + 3, 0), 0, 100);
        media_.push_back(c);
        if (volumes) (*volumes)[ch] = c.volume;
    } else if (op == "end") {
        // 0x059eb7: the channel fades out over <frames> (30) and stops; the newer sscripts also drop
        // its `vol` level (+208 = 0). A channel is needed.
        if (ch < 0) return;
        media_.push_back({stopKind, ch, "", false, static_cast<uint32_t>(std::max(0, argNum(args, at + 1, 30)))});
        if (auto u = soundUntil_.find({k, ch}); u != soundUntil_.end())
            u->second = std::min(u->second, frame_ + static_cast<uint64_t>(std::max(0, argNum(args, at + 1, 30))));
        if (k == 0) bgm_.erase(ch);
        if (k == 1) se_.erase(ch);
        if (k == 2) voiceChannels_.erase(ch);
        if (volumes) volumes->erase(ch);
        if (!olderSound_) channelVolume_.erase({k, ch});
    } else if (op == "loop") {
        // 0x059755 (the same in every sscript): the channel stops; <name> plays on it looping, at the
        // channel's level; one number after it is a pan, three a position. Without a channel the
        // engine reads the wrong record (kind - 1): nothing here.
        if (ch < 0) return;
        media_.push_back({stopKind, ch});
        if (k == 0) bgm_.erase(ch);
        if (k == 1) se_.erase(ch);
        soundUntil_.erase({k, ch});
        soundPos_.erase({k, ch});
        if (args.size() <= at + 1 || args[at + 1].empty() || std::isdigit(static_cast<unsigned char>(args[at + 1][0]))) return;
        const std::string& name = args[at + 1];
        if (!assets_.find(name + ".ogg")) missing(name + ".ogg");
        auto lv = channelVolume_.find({k, ch});
        MediaCommand c{k == 0 ? K::PlayBgm : k == 1 ? K::PlaySe : K::PlayVoice, ch, name, true};
        c.volume = lv != channelVolume_.end() ? lv->second : 100;
        media_.push_back(c);
        if (k == 0) bgm_[ch] = name;
        if (k == 1) se_[ch] = name;
        if (k != 0 && assets_.find(name + ".ogg")) soundUntil_[{k, ch}] = UINT64_MAX;
        placeSound(k, ch, args, at + 2, args.size() > at + 3);
    } else if (op == "pause" || op == "replay") {
        // Happiness 2's sscript (sub-commands 108 / 109 of the sound switch, 0x05842a / 0x058471 ->
        // 0x05c3c5 / 0x05c51b): for the channel, or every channel of the kind, 0x0a61f3 holds a
        // playing sound (native 158, the sound task's message 0xb8) and marks it (+212); 0x0a62ec
        // lets it go on (native 157, message 0xb7) and clears the mark. 0_05_06 2414 / 2426.
        const K kind = k == 0 ? K::BgmPause : k == 1 ? K::SePause : K::VoicePause;
        MediaCommand c{kind, ch};
        c.loop = op == "pause";
        media_.push_back(c);
        // `pos <x> [<y> <z>]` places the sound at once; `move <frames> <x> [<y> <z>]` slides it
        // there (motMove). Axes left out are 0 (sscript fills a zeroed vector).
        const size_t first = op == "move" ? at + 2 : at + 1;
        for (int32_t c = ch < 0 ? 0 : ch; c <= (ch < 0 ? 9 : ch); ++c) {
            auto& p = soundPos_[{k, c}];
            const uint32_t frames = op == "move" ? static_cast<uint32_t>(std::max(0, argNum(args, at + 1, 0))) : 0;
            float now[3];
            for (int i = 0; i < 3; ++i) {
                float t = p.frames ? std::min(1.0f, static_cast<float>(frame_ - p.start) / static_cast<float>(p.frames)) : 1.0f;
                now[i] = p.from[i] + (p.to[i] - p.from[i]) * t;
            }
            for (int i = 0; i < 3; ++i) {
                p.from[i] = now[i];
                p.to[i] = static_cast<float>(argNum(args, first + static_cast<size_t>(i), 0));
            }
            p.start = frame_;
            p.frames = frames;
        }
    } else {
        // Amakano's sub-command switch (0x0576fd; Cyanotype's the same at 0x05802a) has base, rot,
        // arc, quake, mrot, shake, pause, replay and lipbank too; any other keyword falls to the
        // default and does nothing (Amakano's `se 0 stop 60` and `se wait`, Cyanotype's `se stop`).
        static const std::set<std::string> kSound = {"base", "rot", "arc", "quake", "mrot", "shake", "lipbank"};
        if (kSound.count(op)) unhandled((k == 0 ? "bgm " : k == 1 ? "se " : "pcm ") + op);
    }
}

void ScenePlayer::stepSoundPositions() {
    // The engine's 2D pan from a position (grisaia.exe 0x4a2e20, used while SOUND/ds3d is off, its
    // default): 100 sin(atan2(x, z)), truncated; at z = 0 that is fully left or right.
    for (auto& [key, p] : soundPos_) {
        float t = p.frames ? std::min(1.0f, static_cast<float>(frame_ - p.start) / static_cast<float>(p.frames)) : 1.0f;
        float x = p.from[0] + (p.to[0] - p.from[0]) * t;
        float z = p.from[2] + (p.to[2] - p.from[2]) * t;
        auto pan = static_cast<int32_t>(static_cast<float>(std::sin(std::atan2(static_cast<double>(x), static_cast<double>(z)))) * 100.0f);
        if (pan == p.pan) continue;
        p.pan = pan;
        using K = MediaCommand::Kind;
        MediaCommand c{key.first == 0 ? K::BgmPan : key.first == 1 ? K::SePan : K::VoicePan, key.second};
        c.volume = pan;
        media_.push_back(c);
        if (log) log(std::string(key.first == 0 ? "bgm " : key.first == 1 ? "se " : "pcm ") + std::to_string(key.second) + " pan " + std::to_string(pan));
    }
}

void ScenePlayer::stepDisplayScripts() {
    // The slots' FE tasks update after sscript's (the engine's task order); one that exits ends.
    for (auto& d : displayScripts_) {
        if (!d) continue;
        d->tick(frame_);
        for (auto& m : d->takeMedia()) media_.push_back(std::move(m));
        if (d->exited()) d.reset();
    }
}

void ScenePlayer::setCgList(std::vector<CgGroup> groups) {
    cgList_ = std::move(groups);
}

std::vector<ScenePlayer::CgGroup> ScenePlayer::parseCgList(const Bytes& d) {
    std::vector<CgGroup> out;
    auto i32 = [&](size_t o) { return o + 4 <= d.size() ? static_cast<int32_t>(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24) : -1; };
    auto name = [&](size_t o) {
        std::string s;
        for (size_t k = o; k < o + 64 && k < d.size() && d[k]; ++k) s += static_cast<char>(d[k]);
        return s;
    };
    int32_t n = i32(8);
    size_t o = 12;
    for (int32_t g = 0; g < n && o + 72 <= d.size(); ++g) {
        CgGroup group;
        group.name = name(o + 4);
        int32_t count = i32(o + 68);
        o += 72;
        for (int32_t k = 0; k < count && o + 64 <= d.size(); ++k, o += 64) group.images.push_back(name(o));
        out.push_back(std::move(group));
    }
    return out;
}

std::string ScenePlayer::cgKey(const std::string& name) {
    // cs2.exe 0x790750: the name split at "," (0x8f7634); the record is "<first>_<second>" (the
    // join 0x8f7638) with the first character of every later token, which the record keeps as a
    // path of letters (0x791260) and an entry must match exactly (0x7915b0). Names are compared as
    // written (a std::map of strings).
    std::vector<std::string> tokens;
    size_t from = 0;
    for (size_t at = name.find(','); ; at = name.find(',', from)) {
        std::string t = name.substr(from, at == std::string::npos ? std::string::npos : at - from);
        if (!t.empty()) tokens.push_back(std::move(t));
        if (at == std::string::npos) break;
        from = at + 1;
    }
    if (tokens.empty()) return {};
    std::string key = tokens[0];
    if (tokens.size() > 1) key += "_" + tokens[1];
    if (tokens.size() > 2) {
        key += ',';
        for (size_t i = 2; i < tokens.size(); ++i) key += tokens[i][0];
    }
    return key;
}

int32_t ScenePlayer::cgGroupIndex(const std::string& name) const {
    for (size_t i = 0; i < cgList_.size(); ++i) {
        if (toLowerAscii(cgList_[i].name) == toLowerAscii(name)) return static_cast<int32_t>(i);
    }
    return -1;
}

void ScenePlayer::registerCg(const std::string& name, bool alsoSmall) {
    // Native 422 (cs2.exe 0x652870): the record (0x7900a0 adds it, listed in cglist.dat or not);
    // then, if an "l" or "L" stands before a "_" at index 2 or later, the same record without that
    // letter (`Xl,2,...` also as `X_2`), the first such "_" only.
    std::string key = cgKey(name);
    if (key.empty()) return;
    cgSeen_.insert(key);
    if (!alsoSmall) return;
    const size_t end = key.find(',');
    for (size_t i = 2; i < std::min(end, key.size()); ++i) {
        if (key[i] == '_' && (key[i - 1] == 'l' || key[i - 1] == 'L')) {
            cgSeen_.insert(key.substr(0, i - 1) + key.substr(i));
            break;
        }
    }
}

// `mcfade [frames] [from] [to] [hold]` (Happiness 2 sscript 0x03904f; every game's handler, Fruit's
// 0x02a5b8 / 0x04aa51 without the hold): the numbers left out are 30, 0, 255 and 0; from and to count
// `@` from the object's opacity (0x09b10a: the colour word's top byte). Native 535 starts mot.int's
// mcFade on the object's motion controller (record +108); each frame native 307 runs it and native
// 302 writes its colour word back into the record (+236), which the sync then sends to the object.
Motion ScenePlayer::mcFade(const std::vector<std::string>& args, uint64_t at, int32_t current) {
    Motion m;
    m.kind = Motion::Kind::Fade;
    m.loop = true;
    m.start = at;
    m.period = static_cast<uint32_t>(std::max(1, argNum(args, 3, 30)));
    m.a[0] = static_cast<float>(argNum(args, 4, 0, current));
    m.a[1] = static_cast<float>(argNum(args, 5, 255, current));
    m.a[2] = static_cast<float>(std::max(0, argNum(args, 6, 0)));
    return m;
}

// `mcdisp <frames>...` (sscript 0x044063 builds mcDisp's parameters: the count, the sum, then each
// argument as a whole number of frames, and starts it with native 535): shown for the first,
// hidden for the next, and so on, over and over (mot.int's mcDisp).
Motion ScenePlayer::mcDisp(const std::vector<std::string>& args, uint64_t at) {
    Motion m;
    m.kind = Motion::Kind::Disp;
    m.loop = true;
    m.start = at;
    uint32_t total = 0;
    for (size_t i = 3; i < args.size(); ++i) {
        const int32_t d = argNum(args, i, 0);
        m.keys.push_back({static_cast<float>(d), 0, 0});
        total += static_cast<uint32_t>(std::max(0, d));
    }
    m.period = std::max<uint32_t>(1, total);
    return m;
}

namespace {

// `stop` ends an mcfade where it is: the opacity it last set stays; an mccfade's colour too (it
// writes the object's colour, which stays as it was last written).
void holdMotionAlpha(Tween& alpha, const std::vector<Motion>& motions, uint64_t now, uint32_t* color = nullptr,
                     bool* visible = nullptr) {
    const MotionState s = evaluate(motions, now);
    if (s.setAlpha >= 0) alpha = Tween::constant(s.setAlpha);
    if (color && s.hasColor) *color = s.color | 0xFF000000u;
    if (visible && s.setVisible >= 0) *visible = s.setVisible != 0;  // mcdisp's flag stays as last set
}

// `stop` (sscript 0x038e20 -> native 314 -> cs2.exe 0x789860): every motion of the object gets its
// stop flag (0x78a5b0: the motion's +0xa0, mot.int's m[0x80000098]) and ends at its next update,
// its delay skipped (0x78a4d0). The one-shot motions take their end values (motMove, motFade, ...:
// "time >= frames or stopped" adds the whole change), mcMove one cycle on (it adds its whole move),
// the other loops nothing (mcShake, mcArc, mcRot, ...: back to where they started), and mcFade's
// opacity stays (it sets it; holdMotionAlpha). Classic moves and fades are tweens here.
void finishTween(Tween& t, uint64_t now) {
    if (!t.done(now)) t = Tween::constant(t.value(t.start + t.frames));
}

template <class T>
void stopMotions(T& o, uint64_t now) {
    if constexpr (std::is_same_v<T, Plane>) holdMotionAlpha(o.alpha, o.motions, now, &o.color, &o.visible);
    else holdMotionAlpha(o.alpha, o.motions, now, nullptr, &o.visible);
    for (const auto& m : o.motions) {
        if (m.kind == Motion::Kind::Move && m.loop) {
            o.x = Tween::constant(static_cast<int32_t>(std::lround(m.a[2])));
            o.y = Tween::constant(static_cast<int32_t>(std::lround(m.a[3])));
        }
    }
    for (Tween* t : {&o.x, &o.y, &o.alpha}) finishTween(*t, now);
    o.motions.clear();
    o.delay = 0;
}

// A load stops the object's group 1 motions, as `stop` does, and leaves group 0 running. sscript
// (Yukikoi 0x01baff; every game's the same, Fruit 0x015fdb / 0x017513) calls native 313 with 1 on
// the object's motion controller (record +108) before it reads the load's arguments: for an image
// slot (0x01bc81), and for a plane (0x01d525) on every form that is not a subcommand (a name, the
// size form, `$colour`); `video` and `veff` too (0x047a28 / 0x049d1d). Native 313 (cs2.exe
// 0x551620 -> 0x699aa0) sets the stop flag (0x69a460, `stop`'s) on each motion whose group, native
// 535's 7th argument (kept at +0x5c of its entry, 0x699250), is 1: every mc* and m2* motion
// (Yukikoi's 168 motion starts: mcShake, mcFade, ..., m2move 1; move, fade, shake, pos, ... 0).
// So `cg 0 mcshake ...` sways until the next image goes into cg 0.
bool groupOne(const Motion& m) { return m.group == 1 || m.loop; }

void stopGroupOne(StageImage& img, uint64_t now) {
    std::vector<Motion> stopped;
    for (const auto& m : img.motions) {
        if (groupOne(m)) stopped.push_back(m);
    }
    holdMotionAlpha(img.alpha, stopped, now);
    for (const auto& m : stopped) {
        if (m.kind == Motion::Kind::Move && m.loop) {
            img.x = Tween::constant(static_cast<int32_t>(std::lround(m.a[2])));
            img.y = Tween::constant(static_cast<int32_t>(std::lround(m.a[3])));
        }
    }
    if (img.posGroup == 1) finishTween(img.x, now), finishTween(img.y, now);
    if (img.alphaGroup == 1) finishTween(img.alpha, now);
    if (img.scaleGroup == 1) finishTween(img.scaleX, now), finishTween(img.scaleY, now);
    std::erase_if(img.motions, groupOne);
}

void stopGroupOne(Plane& p, uint64_t now) {
    std::vector<Motion> stopped;
    for (const auto& m : p.motions) {
        if (groupOne(m)) stopped.push_back(m);
    }
    holdMotionAlpha(p.alpha, stopped, now, &p.color);
    for (const auto& m : stopped) {
        if (m.kind == Motion::Kind::Move && m.loop) {
            p.x = Tween::constant(static_cast<int32_t>(std::lround(m.a[2])));
            p.y = Tween::constant(static_cast<int32_t>(std::lround(m.a[3])));
        }
    }
    auto group = [&](size_t v) { return p.valueMotions[v].set ? p.valueMotions[v].group : 0; };
    if (group(0) == 1) finishTween(p.x, now), finishTween(p.y, now);
    if (group(1) == 1) finishTween(p.alpha, now);
    for (auto& v : p.valueMotions) {
        if (v.set && v.group == 1 && v.until > now) v.until = now, v.timed = false;
    }
    std::erase_if(p.motions, groupOne);
}

// "30%" -> 30; "1.5" -> 150 when `fraction` (plain factors are 1.0-based).
float percent(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    try {
        if (s.back() == '%') return std::stof(s.substr(0, s.size() - 1));
        return std::stof(s) * 100.0f;
    } catch (...) {
        return fallback;
    }
}

// `spline_s <frames>,<sx%>,<sy%>[,<sz%>] ...` (motSpline_s): motSpline's curve through scales,
// starting from the current one.
Motion scaleSpline(const std::vector<std::string>& args, size_t from, float curX, float curY, uint64_t at) {
    Motion m;
    m.kind = Motion::Kind::Path;
    m.start = at;
    m.a[0] = 1;
    m.a[1] = curX, m.a[2] = curY;
    m.a[3] = 1;
    uint32_t total = 0;
    for (size_t i = from; i < args.size(); ++i) {
        std::vector<std::string> f;
        for (size_t st = 0, c; st <= args[i].size(); st = c + 1) {
            c = args[i].find(',', st);
            if (c == std::string::npos) c = args[i].size();
            f.push_back(args[i].substr(st, c - st));
        }
        int32_t frames = 0;
        try {
            frames = std::max(0, std::stoi(f[0]));
        } catch (...) {
        }
        float x = percent(f.size() > 1 ? f[1] : "", curX);
        float y = percent(f.size() > 2 ? f[2] : "", curY);
        m.keys.push_back({static_cast<float>(frames), x, y});
        total += static_cast<uint32_t>(frames);
    }
    m.period = std::max<uint32_t>(1, total);
    return m;
}

// `ascale1-3 <frames> <sx> <sy>` run motAScale1-3: motAMove1-3's curves on the scale.
uint8_t scaleEase(const std::string& w) {
    if (w == "ascale1" || w == "m2ascale1") return 1;
    if (w == "ascale2" || w == "m2ascale2") return 2;
    if (w == "ascale3" || w == "m2ascale3") return 3;
    return 0;
}

}  // namespace

Plane& ScenePlayer::plane(int32_t index) {
    auto it = pending_.planes.find(index);
    if (it != pending_.planes.end()) return it->second;
    Plane p;
    if (index == kCameraPlane) {
        // The camera slot's start (0x0cbd64): position 0, 0, 0, scale 1, no turn.
        p.x = Tween::constant(0);
        p.y = Tween::constant(0);
        return pending_.planes.emplace(index, p).first->second;
    }
    p.baseX = layout_.planeBaseX;
    p.baseY = layout_.planeBaseY;
    int32_t px = layout_.planePosX, py = layout_.planePosY;
    if (auto d = layout_.planes.find(index); d != layout_.planes.end()) {
        p.baseX = d->second.baseX;
        p.baseY = d->second.baseY;
        px = d->second.posX;
        py = d->second.posY;
        // A plane is a surface of its own size: shaking or moving it shows black past its edges,
        // not more of an oversized backdrop (Hatsumira's opening `pl 0 shake`).
        p.width = d->second.width;
        p.height = d->second.height;
        // vram.xml's `disp`: the Grisaia games start planes 5-17 hidden. A colour put on one
        // (`pl 5 $ff000000`) shows only once the script also says `pl 5 disp 1`.
        p.visible = d->second.disp;
    } else if (!layout_.planes.empty()) {
        // A plane vram.xml leaves out: sscript's record init (Happiness 2 0x08eb4e) copies the base
        // and position from vram.xml's table (m[0x80003fa0], 48 bytes a plane: +40 / +44, +32 / +36),
        // whose entry for it is empty, so both are 0. REBELLIONS places its date on such a plane and
        // Happiness 2 its letterbox bars, with `pos` from that corner, as on PC. Its `disp` (table +28 into record +4, which the
        // per-frame sync sends as the object's visibility, native 92) is 0 too: hidden until
        // `pl N disp 1` (the scripts that use such planes say so).
        p.baseX = p.baseY = 0;
        px = py = 0;
        p.visible = false;
    }
    p.x = Tween::constant(px);
    p.y = Tween::constant(py);
    p.settledX = static_cast<float>(px);
    p.settledY = static_cast<float>(py);
    return pending_.planes.emplace(index, p).first->second;
}

void ScenePlayer::loadPosition(Layer layer, int32_t index, StageImage& img, const std::vector<std::string>& args, size_t at) {
    // A coordinate given with a load is a `pos` (pPos): it replaces the value and the relative motions
    // on that axis; one left out leaves the axis alone. `@` is the slot's settled position
    // (lastUpdatePos).
    const int32_t obj = static_cast<int32_t>(layer) * 1000 + index;
    for (int axis = 0; axis < 2; ++axis) {
        if (at + axis >= args.size() || !isNumberLike(args[at + axis])) break;
        Tween& t = axis == 0 ? img.x : img.y;
        const int32_t cur = t.value(frame_);
        const float drawn = static_cast<float>(cur) + addedValue(img.motions, Motion::Kind::AddPos, axis, frame_);
        const int32_t last = lastUpdatePos(axis == 0 ? img.settledX : img.settledY, drawn);
        if (args[at + axis].find('@') != std::string::npos) traceAt("load", obj, axis, last, drawn);
        notePrior(obj, axis, drawn);
        t = Tween::constant(argNum(args, at + axis, cur, last));
        for (auto& m : img.motions) {
            if (m.kind == Motion::Kind::AddPos) m.a[axis] = 0;
        }
    }
}

void ScenePlayer::notePrior(int32_t object, int32_t field, float replaced) {
    if (priorsFrame_ != frame_) {
        priors_.clear();
        priorsFrame_ = frame_;
    }
    priors_.try_emplace({object, field}, replaced);  // the first assignment this frame is what the update left
}

float ScenePlayer::prior(int32_t object, int32_t field, float current) const {
    if (priorsFrame_ != frame_) return current;
    auto it = priors_.find({object, field});
    return it == priors_.end() ? current : it->second;
}

int32_t ScenePlayer::lastUpdatePos(float settled, float drawn) {
    // `@` in a load's or `pos`'s coordinates is the object record's position (+124): the load
    // (Amakano 1 sscript 0x01b0ab-0x01b338) and `pos` (0x01f19d, every object kind) hand it to the
    // calculator (natives 319 and 212). Both only start a pPos motion (native 535, a load only with
    // coordinates); the update runs it and, once nothing on the object moves, writes the result
    // back into the record (see settlePositions). So a `pos` earlier in the same frame does not count
    // yet, one in an earlier frame does: in Amakano 1 a bank moved by `pos` and loaded later with
    // `@ @` stays where it was moved, while a `pos` and an `@ @` load in the same frame leave it
    // where it was before (the load's pPos comes after the first). Relative motions (`move`, `amove`) take the live values instead (0x09366d).
    return static_cast<int32_t>(std::lround(std::isnan(settled) ? drawn : settled));
}

void ScenePlayer::traceAt(const char* what, int32_t object, int axis, int32_t settled, float drawn) {
    // NYAN_AT_TRACE: each `@` in a load's or `pos`'s coordinates, with the settled position it
    // counts from, where the object is (the frame's changes included) and its default place.
    if (!log || !std::getenv("NYAN_AT_TRACE")) return;
    int32_t home = 0;
    if (object < 0) {
        auto d = layout_.planes.find(-1 - object);
        if (d != layout_.planes.end()) home = axis == 0 ? d->second.posX : d->second.posY;
    } else if (auto d = layout_.imagePos.find({static_cast<Layer>(object / 1000), object % 1000}); d != layout_.imagePos.end()) {
        home = axis == 0 ? d->second.first : d->second.second;
    }
    log("at " + sceneName_ + ":" + std::to_string(pc_ - 1) + " " + what + " " + std::to_string(object) + " " + "xy"[axis] +
        " settled " + std::to_string(settled) + " now " + std::to_string(std::lround(drawn)) + " home " + std::to_string(home));
}

void ScenePlayer::settlePositions() {
    // sscript's per-frame update (Amakano 1 0x0848e4, objects 0-74: images, planes, E-mote banks)
    // runs each object's motions (native 307), and when none of them is moving any more (native
    // 311) removes them (296) and writes the results into the object's record (native 302 at
    // 0x084f94, cs2.exe 0x543d30: the result block's position +0x18 into +124, and the scale,
    // colour and angles). The sync that sends a moving object to the engine (0x093069) writes them
    // into locals only. The lines of a frame run before its update, so at its first line the record
    // holds the position from the last update in which nothing on the object moved; a loop (mc*,
    // m2*) holds it until stopped. A clear puts it back to the slot's default (0x00e504 images,
    // 0x0bab76 E-mote banks: vram.xml's place), as a new slot here starts.
    if (settledFrame_ == frame_) return;
    settledFrame_ = frame_;
    const uint64_t now = frame_;
    auto active = [now](const std::vector<Motion>& ms) {
        return std::any_of(ms.begin(), ms.end(), [now](const Motion& m) { return m.active(now); });
    };
    for (auto& layer : pending_.layers) {
        for (auto& [i, img] : layer) {
            if (!img.x.done(now) || !img.y.done(now) || !img.alpha.done(now) || !img.scaleX.done(now) ||
                !img.scaleY.done(now) || active(img.motions))
                continue;
            const MotionState ms = evaluate(img.motions, now);
            img.settledX = (ms.hasPos ? ms.x : static_cast<float>(img.x.value(now))) + ms.dx;
            img.settledY = (ms.hasPos ? ms.y : static_cast<float>(img.y.value(now))) + ms.dy;
        }
    }
    for (auto& [i, p] : pending_.planes) {
        bool moving = !p.x.done(now) || !p.y.done(now) || !p.alpha.done(now) || active(p.motions);
        for (const auto& v : p.valueMotions) moving = moving || v.running(now);
        if (moving) continue;
        const MotionState ms = evaluate(p.motions, now);
        p.settledX = (ms.hasPos ? ms.x : static_cast<float>(p.x.value(now))) + ms.dx;
        p.settledY = (ms.hasPos ? ms.y : static_cast<float>(p.y.value(now))) + ms.dy;
    }
}

float ScenePlayer::scaleArg(const std::string& tok, int32_t unit, float size, int32_t at) {
    std::string t = tok;
    if (!t.empty() && t.back() == '%') {
        t.pop_back();
        // Native 46 rewrites a whole number before the `%` only; anything else just loses the `%`.
        bool whole = !t.empty() && t.find_first_not_of("0123456789", t[0] == '-' ? 1 : 0) == std::string::npos &&
                     t != "-";
        if (whole) {
            float n = static_cast<float>(std::atoi(t.c_str()));
            t = std::to_string(static_cast<int32_t>(n * static_cast<float>(unit) / 100.0f));
        }
    }
    int32_t v = 0;
    try {
        v = num(t, at);
    } catch (const Error&) {
        v = at;
    }
    return size != 0 ? static_cast<float>(v) / size : 1.0f;
}

void ScenePlayer::relativeMove(int32_t object, Tween& x, Tween& y, std::vector<Motion>& motions,
                               const std::vector<std::string>& args, uint64_t at, uint8_t ease) {
    uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 3, 0)));
    // The values the last update left: the position with the relative motions still running, or
    // what a `pos` earlier in this frame replaced. `@` is that, rounded (natives 319 and 212).
    float cx = prior(object, 0, static_cast<float>(x.value(at)) + addedValue(motions, Motion::Kind::AddPos, 0, at));
    float cy = prior(object, 1, static_cast<float>(y.value(at)) + addedValue(motions, Motion::Kind::AddPos, 1, at));
    int32_t ix = static_cast<int32_t>(std::lround(cx)), iy = static_cast<int32_t>(std::lround(cy));
    // The target minus that; a coordinate left out is no change.
    auto change = [&](size_t i, float cur, int32_t icur) -> int32_t {
        if (i >= args.size() || !isNumberLike(args[i])) return 0;
        return static_cast<int32_t>(std::lround(static_cast<float>(argNum(args, i, icur, icur)) - cur));
    };
    int32_t dx = change(4, cx, ix), dy = change(5, cy, iy);
    bool running = !x.done(at) || !y.done(at) ||
                   std::any_of(motions.begin(), motions.end(), [&](const Motion& m) { return m.kind == Motion::Kind::AddPos && m.active(at); });
    std::erase_if(motions, [&](const Motion& m) { return m.kind == Motion::Kind::AddPos && !m.active(at); });
    if (running) {
        // The running motion goes on; this one adds its change on top along its own curve.
        x.from += dx, x.to += dx;
        y.from += dy, y.to += dy;
        Motion m;
        m.kind = Motion::Kind::AddPos;
        m.start = at;
        m.period = std::max<uint32_t>(1, frames);
        m.a[0] = static_cast<float>(dx);
        m.a[1] = static_cast<float>(dy);
        m.a[5] = ease;
        motions.push_back(m);
    } else {
        int32_t bx = x.value(at), by = y.value(at);
        x = {bx, bx + dx, at, frames, ease};
        y = {by, by + dy, at, frames, ease};
    }
}

void ScenePlayer::cameraCommand(const std::vector<std::string>& args) {
    // `cam [0] ...` (Happiness 2 sscript keyword 21 -> 0x0714d6): the image command (0x01b28f) on
    // slot 79, the one slot of its group: the plane subcommands on the camera (kCameraPlane). Bare
    // `cam` / `cam 0` put it back as it starts; `cam stop` ends its motions.
    dirty_ = true;
    if (args.size() == 1) {
        resetPlane(kCameraPlane);
        return;
    }
    if (args[1] == "stop") {
        if (auto it = pending_.planes.find(kCameraPlane); it != pending_.planes.end()) {
            holdMotionAlpha(it->second.alpha, it->second.motions, frame_, &it->second.color);
            it->second.motions.clear();
            it->second.delay = 0;
        }
        return;
    }
    std::vector<std::string> one = args;
    one[0] = "pl";
    if (isNumberLike(args[1])) {
        const int32_t n = argNum(args, 1, -1);
        if (n >= 1 && n <= 8) {
            // The image command adds the index to the group's first object without checking it
            // against the group's size (0x01b28f -> callidx 539, then 79 + n): `cam 3` (in one of Happiness 2's
            // scenes) works on object 82, `epl` slot 2 (objects 80-87). No scene of
            // any game uses `epl`, so nothing it holds is drawn; Nyanroid has no `epl` slots.
            return;
        }
        if (n != 0) {
            unhandled("cam " + args[1]);
            return;
        }
        one[1] = std::to_string(kCameraPlane);
    } else {
        one.insert(one.begin() + 1, std::to_string(kCameraPlane));
    }
    planeCommand(one);
}

void ScenePlayer::planeCommand(const std::vector<std::string>& args) {
    dirty_ = true;
    if (args.size() == 1) {  // `pl`: reset every plane (as `pl N`, below, for each)
        std::vector<int32_t> used;
        for (const auto& [i, p] : pending_.planes) {
            if (i != kCameraPlane) used.push_back(i);
        }
        for (int32_t i : used) resetPlane(i);
        return;
    }
    if (args[1] == "stop") {
        for (auto& [i, p] : pending_.planes) {
            if (i == kCameraPlane) continue;
            holdMotionAlpha(p.alpha, p.motions, frame_, &p.color);
            p.motions.clear();
            p.delay = 0;
        }
        return;
    }
    // `pl attr_def ...`: attribute defaults exist only for image slots; the engine ignores it here.
    if (args[1] == "attr_def") return;
    if (isImageSubcommand(args[1]) || args[1] == "after" || args[1] == "raster" || args[1] == "bmode") {
        // A subcommand without a plane number goes to every plane (sscript.kcs lists them all);
        // planes never used keep their defaults, so only the ones in use need it: those set up and
        // those images are drawn through (Himawari's `pl mcshake 10 4 2 2 0` shakes the scene).
        std::set<int32_t> used;
        for (const auto& [i, p] : pending_.planes) {
            if (i != kCameraPlane) used.insert(i);
        }
        for (const auto& layer : pending_.layers) {
            for (const auto& [slot, img] : layer) used.insert(img.plane);
        }
        for (int32_t i : used) {
            std::vector<std::string> one = args;
            one.insert(one.begin() + 1, std::to_string(i));
            planeCommand(one);
        }
        return;
    }
    if (args[1].size() > 2 && args[1].front() == '[' && args[1].back() == ']') {
        // `pl [a-b] ...` / `pl [a,b,c-d] ...`: the same command for each plane listed.
        std::string list = args[1].substr(1, args[1].size() - 2);
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (item.empty()) continue;
            size_t dash = item.find('-', 1);
            int32_t from = num(item.substr(0, dash));
            int32_t to = dash == std::string::npos ? from : num(item.substr(dash + 1));
            for (int32_t i = from; i <= to; ++i) {
                std::vector<std::string> one = args;
                one[1] = std::to_string(i);
                planeCommand(one);
            }
        }
        return;
    }
    int32_t index = argNum(args, 1, -1);
    if (index < 0) {
        unhandled("pl " + args[1]);
        return;
    }
    if (args.size() == 2) {  // `pl N`: reset plane N
        resetPlane(index);
        return;
    }
    Plane& p = plane(index);
    // A load (a name, the size form, `$colour`), `video` or `veff` stops the plane's group 1 motions.
    if (!isImageSubcommand(args[2]) || args[2] == "video" || args[2] == "veff") stopGroupOne(p, frame_);
    // The motion groups of this subcommand's motions and values (see the end).
    const bool m2 = args[2].starts_with("m2");
    for (auto& m : p.motions) m.marked = true;
    // Newer engines' m2* motions take the same arguments as the classic ones.
    std::string what = args[2];
    if (what == "m2fade") what = "fade";
    else if (what == "m2arc") what = "arc";
    else if (what == "m2scale") what = "mscale";
    else if (what == "m2rot") what = "mrot";
    else if (what == "m2pend") what = "pend";
    // The `p` forms (motAMove1p, motAScale1p, motSplinep) only differ in running on through a skip
    // (their end test leaves out the skip flag).
    if (((what.starts_with("m2amove") && what.size() == 9) || (what.starts_with("m2ascale") && what.size() == 10)) &&
        what.back() == 'p') {
        what.pop_back();
    } else if (what == "m2splinep") {
        what = "m2spline";
    }
    // `delay` holds back every motion and property change after it (the engine passes the object's
    // delay to each motion it starts); display, mode and effect commands act at once.
    const uint64_t at = frame_ + (startsMotion(what) ? static_cast<uint64_t>(std::max(0, p.delay)) : 0);
    auto motion = [&](Motion::Kind kind, bool loop, uint32_t period) {
        Motion m;
        m.kind = kind;
        m.loop = loop;
        m.start = at;
        m.period = std::max<uint32_t>(1, period);
        return m;
    };

    if (args.size() >= 4 && isNumberLike(what) && isNumberLike(args[3])) {
        // `pl N <w> <h>`: a plane of its own size, clipped to it. The base point stays where it was
        // (scripts that want it centred say so: `pl <n> <w> <h>`, then `pl <n> base <w/2> <h/2>`).
        p.width = std::max(0, argNum(args, 2, 0));
        p.height = std::max(0, argNum(args, 3, 0));
        p.delay = 0;
        // Sizing a plane makes it a surface of its own, shown (scripts then hide the ones they set
        // up for later with `pl 10 disp 0`).
        p.visible = true;
        // sscript (Happiness 2 0x01e1ba, Fruit 0x017b43) makes it a plane again (bit 1: a colour
        // plane is replaced) and sets its colour word to -1: white, opaque.
        p.fill = false;
        p.color = 0xFFFFFFFFu;
        std::erase_if(p.motions, [](const Motion& m) { return m.kind == Motion::Kind::Fade || m.kind == Motion::Kind::CFade; });
        p.alpha = {p.alpha.value(at), 255, at, 0};
    } else if (what[0] == '$') {
        // `pl N $AARRGGBB [w h]` (Happiness 2's sscript 0x01dce9; Fruit's 0x017711): the plane becomes
        // a colour plane. sscript stores the number as the plane's colour word (+236: ARGB, its top
        // byte the plane's opacity, the byte `blend` / `fade` write, 0x00f1c3) and the size (the
        // numbers given, else vram.xml's width and height for the plane, else the screen's), and
        // sets bit 2. The next frame (0x0924a9) replaces the plane's engine object with a colour plane
        // (native 66 type 2, kcFEScriptObjCPlane; native 79 its size and colour), which the per-frame
        // sync (0x0916b2, native 100) keeps at the plane's colour word and place. So the fill takes
        // the plane's place, depth, opacity and colour, and the plane's images are not shown until
        // `pl N <w> <h>`, a load or a reset makes it a plane again. Seen on PC: a colour plane faded in with
        // `fade 0 0 255` draws opaque bars; one given `blend 100` darkens the screen to 1 - 100/255.
        p.fill = true;
        p.delay = 0;
        try {
            const uint32_t c = static_cast<uint32_t>(std::stoul(what.substr(1), nullptr, 16));
            p.fillColor = c;
            p.color = c | 0xFF000000u;  // the word's colour; its opacity is p.alpha
            std::erase_if(p.motions, [](const Motion& m) { return m.kind == Motion::Kind::Fade || m.kind == Motion::Kind::CFade; });
            p.alpha = {p.alpha.value(at), static_cast<int32_t>(c >> 24), at, 0};
        } catch (...) {
        }
        auto d = layout_.planes.find(index);
        const bool sized = d != layout_.planes.end() && d->second.width > 0 && d->second.height > 0;
        p.fillWidth = argNum(args, 3, sized ? d->second.width : layout_.screenWidth);
        p.fillHeight = argNum(args, 4, sized ? d->second.height : layout_.screenHeight);
    } else if (what == "blend") {
        // pAlpha, one byte: sscript keeps the number in a byte (0x0385f0), so 2128 is 80.
        p.alpha = {p.alpha.value(at), static_cast<int32_t>(static_cast<uint8_t>(argNum(args, 3, 255))), at, 0};
    } else if (what == "mcfade") {
        std::erase_if(p.motions, [](const Motion& m) { return m.kind == Motion::Kind::Fade; });
        p.motions.push_back(mcFade(args, at, p.alpha.value(at)));
    } else if (what == "mccfade") {
        // `mccfade [frames] [$from] [$to]` (sscript 0x039450 -> native 535 "mcCFade"): the frames
        // (30 if left out) and the two colours' bytes; a colour left out is the object's own
        // (its record's +236: here the opacity over the colour's red, green and blue).
        const uint32_t own = static_cast<uint32_t>(std::clamp(p.alpha.value(at), 0, 255)) << 24 | (p.color & 0xFFFFFFu);
        auto colour = [&](size_t k) {
            if (args.size() <= k) return own;
            if (args[k].starts_with("$")) return static_cast<uint32_t>(std::strtoul(args[k].c_str() + 1, nullptr, 16));
            return static_cast<uint32_t>(argNum(args, k, 0));
        };
        const uint32_t from = colour(4), to = colour(5);
        Motion m = motion(Motion::Kind::CFade, true, static_cast<uint32_t>(std::max(1, argNum(args, 3, 30))));
        for (int b = 0; b < 4; ++b)
            m.keys.push_back({static_cast<float>((from >> (8 * b)) & 0xFF), static_cast<float>((to >> (8 * b)) & 0xFF), 0});
        std::erase_if(p.motions, [](const Motion& x) { return x.kind == Motion::Kind::CFade; });
        p.motions.push_back(m);
    } else if (what == "blur") {
        // sscript's `blur` (0x045670) sets the blur of image slots only: for a plane (slot 27 on)
        // it returns 0 at once, and the scene loop drops the result (0x014096). Nothing happens.
    } else if (what == "fade" || what == "fade2") {
        int32_t cur = p.alpha.value(at);
        p.alpha = {std::clamp(argNum(args, 4, cur, cur), 0, 255), std::clamp(argNum(args, 5, 255, cur), 0, 255),
                   at, static_cast<uint32_t>(std::max(0, argNum(args, 3, 0))), static_cast<uint8_t>(what == "fade2" ? 4 : 0)};
    } else if (what == "disp") {
        // `disp <n>`; a bare `disp` puts back vram.xml's (sscript 0x0387cd: planes from their record
        // table at m[0x80003e94] +28, images 1).
        if (args.size() > 3) {
            p.visible = argNum(args, 3, 1) != 0;
        } else {
            auto d = layout_.planes.find(index);
            p.visible = d != layout_.planes.end() ? d->second.disp : Plane{}.visible;
        }
    } else if (what == "move" || what == "amove1" || what == "amove2" || what == "amove3" || what == "m2move" ||
               what == "m2amove1" || what == "m2amove2" || what == "m2amove3") {
        relativeMove(-1 - index, p.x, p.y, p.motions, args, at, moveEase(what));
        std::erase_if(p.motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
    } else if (what == "pos") {
        // `@` / `@+n`: the plane's settled position (`pl 0 pos @ @` after a quake; see
        // lastUpdatePos). pPos takes the place of the motions before it.
        int32_t cx = p.x.value(at), cy = p.y.value(at);
        const float drawnX = static_cast<float>(cx) + addedValue(p.motions, Motion::Kind::AddPos, 0, at);
        const float drawnY = static_cast<float>(cy) + addedValue(p.motions, Motion::Kind::AddPos, 1, at);
        const int32_t lastX = lastUpdatePos(p.settledX, drawnX), lastY = lastUpdatePos(p.settledY, drawnY);
        if (args.size() > 3 && args[3].find('@') != std::string::npos) traceAt("pl pos", -1 - index, 0, lastX, drawnX);
        if (args.size() > 4 && args[4].find('@') != std::string::npos) traceAt("pl pos", -1 - index, 1, lastY, drawnY);
        notePrior(-1 - index, 0, drawnX);
        notePrior(-1 - index, 1, drawnY);
        std::erase_if(p.motions, [](const Motion& m) { return m.kind == Motion::Kind::AddPos; });
        // A number left out is vram.xml's position for the plane, not where it is (pPos with the
        // defaults 0x093d15 gives: the record table m[0x80003e94] +32 / +36).
        auto d = layout_.planes.find(index);
        const bool table = !layout_.planes.empty();  // a plane it leaves out: 0 (see plane())
        const int32_t dx = d != layout_.planes.end() ? d->second.posX : table ? 0 : layout_.planePosX;
        const int32_t dy = d != layout_.planes.end() ? d->second.posY : table ? 0 : layout_.planePosY;
        p.x = {cx, argNum(args, 3, dx, lastX), at, 0};
        p.y = {cy, argNum(args, 4, dy, lastY), at, 0};
    } else if (what == "base") {
        p.depth = std::numeric_limits<float>::quiet_NaN();  // `base` puts z back at the default depth
        p.baseX = argNum(args, 3, p.baseX, p.baseX);
        p.baseY = argNum(args, 4, p.baseY, p.baseY);
    } else if (what == "rot") {
        // `rot rx ry rz` (pRot): the plane's angles outright.
        p.rotX = static_cast<float>(argNum(args, 3, 0));
        p.rotY = static_cast<float>(argNum(args, 4, 0));
        p.rotation = static_cast<float>(argNum(args, 5, 0));
    } else if (what == "mrot") {
        // `mrot <frames> rx ry rz`: the plane takes the new value and a Settle motion eases from the
        // old one over <frames>. motRot turns by the angles given (0 -> them, linearly), on top of
        // where the plane already is.
        Motion m = motion(Motion::Kind::Settle, false, static_cast<uint32_t>(std::max(1, argNum(args, 3, 1))));
        m.a[0] = m.a[1] = 1;
        float dx = static_cast<float>(argNum(args, 4, 0)), dy = static_cast<float>(argNum(args, 5, 0));
        float dz = static_cast<float>(argNum(args, 6, 0));
        m.a[2] = -dz, m.a[3] = -dx, m.a[4] = -dy;
        p.rotation += dz, p.rotX += dx, p.rotY += dy;
        if (argNum(args, 3, 0) > 0) p.motions.push_back(m);
    } else if (what == "mscale" || scaleEase(what) != 0) {
        // `mscale <frames> sx sy` (motScale), `ascaleN` (motAScaleN) and their m2 forms (sscript
        // 0x02a3fe / 0x04898e for planes; Eden's the same): each argument is a size against the
        // plane's own (`n%` of it, or pixels; `@` the current size), divided by it; one left out is
        // 100%. The change from the scale the last update left is added along the curve, on top of
        // any still running (AddScale).
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 3, 0)));
        const int32_t obj = -1 - index;
        float w = static_cast<float>(p.width > 0 ? p.width : layout_.screenWidth);
        float h = static_cast<float>(p.height > 0 ? p.height : layout_.screenHeight);
        float cx = prior(obj, 2, p.scaleX + addedValue(p.motions, Motion::Kind::AddScale, 0, at));
        float cy = prior(obj, 3, p.scaleY + addedValue(p.motions, Motion::Kind::AddScale, 1, at));
        auto target = [&](size_t i, float size, float cur) {
            if (i >= args.size() || !isNumberLike(args[i])) return 1.0f;
            return scaleArg(args[i], static_cast<int32_t>(std::lround(size)), size, static_cast<int32_t>(std::lround(cur * size)));
        };
        float tx = target(4, w, cx), ty = target(5, h, cy);
        std::erase_if(p.motions, [&](const Motion& o) { return o.kind == Motion::Kind::AddScale && !o.active(at); });
        Motion m = motion(Motion::Kind::AddScale, false, std::max<uint32_t>(1, frames));
        m.a[0] = tx - cx;
        m.a[1] = ty - cy;
        m.a[5] = scaleEase(what);
        p.scaleX += m.a[0];
        p.scaleY += m.a[1];
        if (frames > 0) p.motions.push_back(m);
        std::erase_if(p.motions, [](const Motion& o) { return o.kind == Motion::Kind::Path && o.a[3] != 0; });
    } else if (what == "arc" || what == "mcarc") {
        // `arc <frames> <deg0> <deg1> <radiusX> <radiusY>`, as for images.
        Motion m = motion(Motion::Kind::Arc, what == "mcarc", static_cast<uint32_t>(std::max(1, argNum(args, 3, 30))));
        m.a[0] = static_cast<float>(argNum(args, 4, 0));
        m.a[1] = static_cast<float>(argNum(args, 5, 180));
        m.a[2] = static_cast<float>(argNum(args, 6, 0));
        m.a[3] = static_cast<float>(argNum(args, 7, 0));
        p.motions.push_back(m);
        if (!m.loop) {
            auto [ex, ey] = arcEnd(m);
            uint64_t end = m.start + m.period;
            int32_t x = p.x.value(end), y = p.y.value(end);
            if (ex) p.x = {x, x + ex, end, 0};
            if (ey) p.y = {y, y + ey, end, 0};
        }
    } else if (what == "color") {
        // `$AARRGGBB` (`$` marks hex in scene scripts); without a value, white.
        std::string v = args.size() > 3 ? args[3] : "";
        if (!v.empty() && v[0] == '$') p.color = static_cast<uint32_t>(std::strtoul(v.c_str() + 1, nullptr, 16));
        else p.color = v.empty() ? 0xFFFFFFFFu : static_cast<uint32_t>(argNum(args, 3, -1));
    } else if (what == "attr_def") {
    } else if (what == "scale") {
        // pScale (sscript 0x02256a for planes): each argument a size against the plane's own (`n%`
        // of it, or pixels; `@` the sscript's record of the scale, rounded), divided by it; one left
        // out is 100%. It takes the place of the scale motions before it; a relative one later in
        // this frame starts from the scale it replaced.
        const int32_t obj = -1 - index;
        float w = static_cast<float>(p.width > 0 ? p.width : layout_.screenWidth);
        float h = static_cast<float>(p.height > 0 ? p.height : layout_.screenHeight);
        notePrior(obj, 2, p.scaleX + addedValue(p.motions, Motion::Kind::AddScale, 0, at));
        notePrior(obj, 3, p.scaleY + addedValue(p.motions, Motion::Kind::AddScale, 1, at));
        auto value = [&](size_t i, float size, float stored) {
            if (i >= args.size() || !isNumberLike(args[i])) return 1.0f;
            return scaleArg(args[i], static_cast<int32_t>(std::lround(size)), size, static_cast<int32_t>(std::lround(stored)));
        };
        float sx = value(3, w, p.scaleX), sy = value(4, h, p.scaleY);
        std::erase_if(p.motions, [](const Motion& o) { return o.kind == Motion::Kind::AddScale; });
        p.scaleX = sx;
        p.scaleY = sy;
    } else if (what == "mcmove") {
        // Continuous: travel from the current position to the target every `period` frames.
        Motion m = motion(Motion::Kind::Move, true, static_cast<uint32_t>(argNum(args, 3, 60)));
        m.a[0] = static_cast<float>(p.x.value(at));
        m.a[1] = static_cast<float>(p.y.value(at));
        m.a[2] = static_cast<float>(argNum(args, 4, static_cast<int32_t>(m.a[0]), static_cast<int32_t>(m.a[0])));
        m.a[3] = static_cast<float>(argNum(args, 5, static_cast<int32_t>(m.a[1]), static_cast<int32_t>(m.a[1])));
        p.motions.push_back(m);
    } else if (what == "mcrot") {
        // `mcrot px py pz`: a turn every |p| frames about each axis.
        Motion m = motion(Motion::Kind::Rotate, true, 1);
        m.a[0] = static_cast<float>(argNum(args, 5, 0));
        m.a[1] = static_cast<float>(argNum(args, 3, 0));
        m.a[2] = static_cast<float>(argNum(args, 4, 0));
        p.motions.push_back(m);
    } else if (what == "mcdisp") {
        Motion m = mcDisp(args, at);
        std::erase_if(p.motions, [](const Motion& o) { return o.kind == Motion::Kind::Disp; });
        p.motions.push_back(m);
    } else if (what == "mcscalecos" || what == "mcscalesin") {
        // `mcscalecos px py pz fromX fromY fromZ toX toY toZ`
        Motion m = motion(Motion::Kind::ScaleCos, true, static_cast<uint32_t>(argNum(args, 3, 60)));
        m.a[0] = percent(args.size() > 6 ? args[6] : "", 100);
        m.a[1] = percent(args.size() > 7 ? args[7] : "", 100);
        m.a[2] = percent(args.size() > 9 ? args[9] : "", 100);
        m.a[3] = percent(args.size() > 10 ? args[10] : "", 100);
        p.motions.push_back(m);
    } else if ((what == "spline_s" || what == "m2spline_s") && args.size() > 3) {
        std::erase_if(p.motions, [](const Motion& o) { return o.kind == Motion::Kind::Path && o.a[3] != 0; });
        p.motions.push_back(scaleSpline(args, 3, p.scaleX * 100, p.scaleY * 100, at));
    } else if ((what == "quake" || what == "mcquake" || what == "spline" || what == "mcspline" || what == "m2spline" ||
                what == "m2quake") &&
               args.size() > 3 && args[3].find(',') != std::string::npos) {
        bool spline = what.find("spline") != std::string::npos;
        p.motions.push_back(pathKeys(args, 3, spline, what.starts_with("mc"), static_cast<float>(p.x.value(at)),
                                     static_cast<float>(p.y.value(at))));
        p.motions.back().start = at;
    } else if (what == "shake" || what == "m2shake" || what == "quake" || what == "mcshake") {
        // `shake <frames> <count> <ampX> <ampY> ...`; motShake makes a length under 1 one frame
        // (Himawari's `pl 0 shake -1 ...` is not endless).
        Motion m = motion(Motion::Kind::Shake, what == "mcshake", static_cast<uint32_t>(std::max(1, argNum(args, 3, 15))));
        m.a[2] = static_cast<float>(argNum(args, 4, 1));
        m.a[0] = static_cast<float>(argNum(args, 5, 0));
        m.a[1] = static_cast<float>(argNum(args, 6, 0));
        p.motions.push_back(m);
    } else if (what == "pend" || what == "mcpend") {
        p.motions.push_back(pendulum(args, what == "mcpend", at));
    } else if (what == "mcscale") {
        // `mcscale <cycleX> <cycleY> <cycleZ> <fromX> <fromY> <fromZ> <toX> <toY> <toZ>`: from/to
        // default to the current scale (sscript.kcs), which leaves it unchanged.
        Motion m = motion(Motion::Kind::ScaleSaw, true, 1);
        float cx = p.scaleX * 100, cy = p.scaleY * 100;
        m.a[0] = percent(args.size() > 6 ? args[6] : "", cx);
        m.a[1] = percent(args.size() > 7 ? args[7] : "", cy);
        m.a[2] = percent(args.size() > 9 ? args[9] : "", cx);
        m.a[3] = percent(args.size() > 10 ? args[10] : "", cy);
        m.a[4] = static_cast<float>(std::max(1, argNum(args, 3, 1)));
        m.a[5] = static_cast<float>(std::max(1, argNum(args, 4, 1)));
        p.motions.push_back(m);
    } else if (what == "pri") {
        // `pri <depth>` (`@+n` relative): where the plane sits in the draw order; bare `pri` is the
        // default 5000 + 1000 * plane.
        float def = 5000.0f + 1000.0f * static_cast<float>(index);
        float cur = std::isnan(p.depth) ? def : p.depth;
        p.depth = args.size() > 3 ? static_cast<float>(argNum(args, 3, static_cast<int32_t>(cur), static_cast<int32_t>(cur)))
                                  : std::numeric_limits<float>::quiet_NaN();
    } else if (what == "clone") {
        // `pl N clone pl M`: plane N becomes a copy of plane M (sscript.kcs copies the whole object),
        // keeping its own draw depth and effects; its VRAM window is M's, so it shows M's images.
        if (args.size() > 4 && args[3] == "pl") {
            int32_t src = argNum(args, 4, -1);
            if (src >= 0 && src != index) {
                Plane copy = plane(src);
                Plane& dst = plane(index);
                auto d = layout_.planes.find(src);
                if (!copy.hasVram && d != layout_.planes.end()) {
                    copy.hasVram = true;
                    copy.vramX = d->second.vramX;
                    copy.vramY = d->second.vramY;
                }
                copy.depth = dst.depth;
                copy.raster = dst.raster;
                copy.wave = dst.wave;
                copy.after = dst.after;
                copy.blendMode = dst.blendMode;
                dst = std::move(copy);
            }
        } else {
            unhandled("pl clone " + (args.size() > 3 ? args[3] : std::string()));
        }
    } else if (what == "video") {
        // `pl N video <name> [x y z]` (sscript, plane objects only): the plane's picture becomes the
        // clip, placed at x, y (default: where the plane is).
        p.video = args.size() > 3 ? args[3] : std::string();
        p.fill = false;  // the plane's object becomes the clip's (bit 1024), no colour plane
        p.videoPlays = 0;
        p.videoStart = at;
        if (args.size() > 4) {
            int32_t cx = p.x.value(at), cy = p.y.value(at);
            p.x = Tween::constant(argNum(args, 4, cx, cx));
            p.y = Tween::constant(argNum(args, 5, cy, cy));
        }
    } else if (what == "vcom") {
        // `vcom play [n]` / `vcom stop` / `vcom vol <v>` on the plane's clip.
        const std::string sub = args.size() > 3 ? args[3] : std::string();
        if (sub == "play") {
            p.videoPlays = args.size() > 4 ? argNum(args, 4, 1) : 1;
            if (p.videoPlays == 0) p.videoPlays = 1;
            p.videoStart = at;
        } else if (sub == "stop") {
            p.videoPlays = 0;
        } else if (sub != "vol") {
            unhandled("pl vcom " + sub);
        }
    } else if (what == "tone") {
        // Planes take a tone like images, but it only acts when an image is loaded; planes load none.
    } else if (what == "stop") {
        stopMotions(p, at);
    } else if (what == "delay") {
        p.delay = std::max(0, argNum(args, 3, 0));
    } else if (what == "vram") {
        // `vram <x> [<y>]`: move the plane's window in VRAM; without numbers, back to vram.xml's.
        p.hasVram = args.size() > 3;
        p.vramX = argNum(args, 3, 0);
        p.vramY = argNum(args, 4, 0);
    } else if (what == "bmode") {
        int32_t m = argNum(args, 3, 0);
        p.blendMode = static_cast<uint8_t>(m >= 0 && m <= 5 ? m : 0);
    } else if (what == "raster") {
        const std::string a = args.size() > 3 ? args[3] : "";
        if (a.empty() || a == "stop") {
            p.raster = false;
        } else if (a == "fade") {
            // `raster fade <frames> <amp> <cycle> <speed>` (Happiness 2's sscript 0x03c132): all four
            // needed (sscript reports a missing one); only a plane with a raster changes, from where
            // its wave is now. `pl 0 raster fade 90 0 0 0` lets a sway die down.
            if (!p.raster || args.size() < 8) return;
            const Raster now = p.wave.at(frame_);
            p.wave.fromAmplitude = now.amplitude, p.wave.fromCycle = now.cycle, p.wave.fromSpeed = now.speed;
            p.wave.amplitude = static_cast<float>(argNum(args, 5, 0));
            p.wave.cycle = static_cast<float>(argNum(args, 6, 0));
            p.wave.speed = static_cast<float>(argNum(args, 7, 0));
            p.wave.fadeStart = frame_;
            p.wave.fadeFrames = static_cast<uint32_t>(std::max(0, argNum(args, 4, 0)));
        } else if (!p.raster) {  // the engine refuses a second raster on the same plane
            p.raster = true;
            p.wave.dir = argNum(args, 3, 1);
            p.wave.amplitude = static_cast<float>(argNum(args, 4, 0));
            p.wave.cycle = static_cast<float>(std::max(1, argNum(args, 5, 1)));
            p.wave.speed = static_cast<float>(argNum(args, 6, 0));
            p.wave.start = frame_;
            p.wave.fadeFrames = 0;
            if (p.wave.dir < 0) unhandled("pl raster <negative dir>");
        }
    } else if (what == "after") {
        // Arguments as sscript.kcs reads them; no count (or 0) ends the afterimages.
        auto scale = [&](size_t i) {
            if (args.size() <= i) return 1.0f;
            if (args[i].ends_with("%")) return percent(args[i], 100) / 100.0f;
            unhandled("pl after <pixel size>");
            return 1.0f;
        };
        AfterImage& a = p.after;
        a.count = std::max(0, argNum(args, 3, 0));
        a.interval = std::max(1, argNum(args, 4, 1));
        a.dx = static_cast<float>(argNum(args, 5, 0));
        a.dy = static_cast<float>(argNum(args, 6, 0));
        a.alpha = std::clamp(argNum(args, 8, 255), 0, 255);
        a.scaleX = scale(9);
        a.scaleY = scale(10);
        a.mode = argNum(args, 12, 0);
        a.priority = argNum(args, 13, -10);
        a.start = frame_;
        if (a.mode > 2) unhandled("pl after <mode " + std::to_string(a.mode) + ">");
    } else if (std::isdigit(static_cast<unsigned char>(what[0]))) {
        unhandled("pl <size>");
    } else if (assets_.find(what + ".kx2")) {
        // A KX2 mesh names the plane's shape (split screens, circular cut-ins, and the raster_*
        // grids `raster` sways).
        p.shape = what;
        p.fill = false;  // a load makes it a plane again (bit 1, Happiness 2 0x01e765)
    } else {
        unhandled("pl " + what);
    }
    noteValueMotion(p, what, m2, at, args);
}

void ScenePlayer::noteValueMotion(Plane& p, const std::string& what, bool m2, uint64_t at,
                                  const std::vector<std::string>& args) {
    // The motions this subcommand started: mc* (looping) and m2* ones in group 1, the rest in 0
    // (sscript's native 535 calls: pScale, pPos, motMove, ... 0; mcScale, motMove from m2move ... 1).
    for (auto& m : p.motions) {
        if (m.marked) continue;
        m.group = m2 || m.loop ? 1 : 0;
        m.marked = true;
    }
    // Which value it drives and until when: a property set until the engine's next update (this
    // frame), a timed motion for its frames.
    const uint64_t timed = at + static_cast<uint64_t>(std::max(0, argNum(args, 3, 0)));
    auto note = [&](size_t value, uint64_t until) {
        p.valueMotions[value] = {true, static_cast<uint8_t>(m2 ? 1 : 0), until, until > at};
    };
    if (what == "pos") note(0, at);
    else if (what == "move" || what == "m2move" || what.starts_with("amove") || what.starts_with("m2amove") || what == "arc") note(0, timed);
    else if (what == "blend") note(1, at);
    else if (what == "fade" || what == "fade2") note(1, timed);
    else if (what == "scale" || what == "vscale") note(2, at);
    else if (what == "mscale" || scaleEase(what) != 0) note(2, timed);
    else if (what == "rot" || what == "rotc" || what == "rot_ypr") note(3, at);
    else if (what == "mrot") note(3, timed);
    else if (what == "color") note(4, at);
    else if (what == "base") note(5, at);
    else if (what == "pri") note(6, at);
}

void ScenePlayer::markImageGroups(Layer layer, int32_t index, const std::string& word, bool m2) {
    StageImage* img = slot(layer, index, false);
    if (!img) return;
    // mc* (looping) and m2* motions are group 1, the rest 0 (sscript's native 535 calls).
    for (auto& m : img->motions) {
        if (m.marked) continue;
        m.group = m2 || m.loop ? 1 : 0;
        m.marked = true;
    }
    const uint8_t g = m2 ? 1 : 0;
    std::string w = m2 ? word.substr(2) : word;
    if (w.ends_with('p') && (w.starts_with("amove") || w.starts_with("ascale"))) w.pop_back();
    if (w == "move" || w == "vmove" || w == "pos" || w == "arc" || w.starts_with("amove")) img->posGroup = g;
    else if (w == "fade" || w == "fade2" || w == "blend") img->alphaGroup = g;
    else if (w == "scale" || w == "mscale" || w == "vscale" || w.starts_with("ascale")) img->scaleGroup = g;
}

void ScenePlayer::finishClassicMotions(Stage& st) {
    // 0x08a765 -> native 313 with 0: the stop flag on group 0, so the classic one-shots take their
    // end values at their next update (see `stop`); the group 1 ones are left running. sscript's
    // main loop (Amakano 1 0x0c0022) runs the scene step (0x081e26), whose end raises the flag
    // (0x082ed7 -> 0x086747), and then the update (0x082f11), whose motions (native 307) end there.
    // So a motion the lines of one frame start is over before the next frame's lines: the next
    // bare `wait` goes on and a bare `pl N` finds nothing running (native 311) and resets the plane
    // whole. Nyanroid finishes them at the top of the next frame, so they count as over now.
    for (auto& layer : st.layers) {
        for (auto& [i, img] : layer) {
            if (img.posGroup == 0) finishTween(img.x, frame_), finishTween(img.y, frame_);
            if (img.alphaGroup == 0) finishTween(img.alpha, frame_);
            if (img.scaleGroup == 0) finishTween(img.scaleX, frame_), finishTween(img.scaleY, frame_);
            std::erase_if(img.motions, [](const Motion& m) { return m.group == 0 && !m.loop; });
        }
    }
    for (auto& [i, p] : st.planes) {
        auto group = [&](size_t v) { return p.valueMotions[v].set ? p.valueMotions[v].group : 0; };
        if (group(0) == 0) finishTween(p.x, frame_), finishTween(p.y, frame_);
        if (group(1) == 0) finishTween(p.alpha, frame_);
        for (auto& v : p.valueMotions) {
            if (v.set && v.group == 0 && v.until > frame_) v.until = frame_, v.timed = true;  // its last update was the last frame's
        }
        std::erase_if(p.motions, [](const Motion& m) { return m.group == 0 && !m.loop; });
    }
}

void ScenePlayer::resetPlane(int32_t index) {
    // `pl N` (sscript's image command 0x019f4a with an index alone: native 519 clears the next
    // token, so the reset path) runs 0x08c20d on the plane: its values back to vram.xml's
    // (record at m[0x80001794]: +28 disp, the coordinates), sent to the engine at once (native 301),
    // and its engine motions removed: all of them (native 296) while sscript replays (modes 2 and
    // 25: a jump, a return from the backlog), when none is running (native 311) or for a plane
    // vram.xml doesn't define (record +0); otherwise only group 1 (native 298 with 1). The group 0
    // ones then go on driving their values, so `pl 0 scale 133% 133%` just before a `pl 0` leaves
    // the plane at 133% (Nekopara does this; the PC shows it so). Nyanroid never replays.
    auto it = pending_.planes.find(index);
    if (it == pending_.planes.end()) return;
    Plane old = std::move(it->second);
    pending_.planes.erase(it);
    dirty_ = true;
    bool running = false;
    // A timed motion is over on the frame a `wait` for it ends (Happiness 2:
    // `pl <n> fade 20 255 0`, `wait`, `pl <n>` brings the plane back at 255, as on PC); a set given
    // this frame is still to be applied (Nekopara).
    for (const auto& v : old.valueMotions) running = running || v.running(frame_);
    for (const auto& m : old.motions) running = running || m.active(frame_);
    if (!running || !layout_.planes.count(index)) return;
    auto kept = [&](size_t value) {
        const auto& v = old.valueMotions[value];
        return v.group == 0 && v.running(frame_);
    };
    Plane& p = plane(index);
    if (kept(0)) p.x = old.x, p.y = old.y;
    if (kept(1)) p.alpha = old.alpha;
    if (kept(2)) p.scaleX = old.scaleX, p.scaleY = old.scaleY;
    if (kept(3)) p.rotation = old.rotation, p.rotX = old.rotX, p.rotY = old.rotY;
    if (kept(4)) p.color = old.color;
    if (kept(5)) p.baseX = old.baseX, p.baseY = old.baseY;
    if (kept(6)) p.depth = old.depth;
    for (size_t v = 0; v < old.valueMotions.size(); ++v) {
        if (kept(v)) p.valueMotions[v] = old.valueMotions[v];
    }
    for (const auto& m : old.motions) {
        if (m.group == 0 && !m.loop && m.active(frame_)) p.motions.push_back(m);
    }
}

std::vector<std::string> ScenePlayer::resolveSprite(const std::string& spec) {
    auto it = spriteCache_.find(spec);
    if (it != spriteCache_.end()) return it->second;

    std::vector<std::string> parts = spriteParts(assets_, spec);
    if (parts.empty()) missing(spec + ".hg3");
    spriteCache_[spec] = parts;
    return parts;
}

std::vector<std::string> ScenePlayer::spriteParts(const AssetIndex& assets, const std::string& spec) {
    std::vector<std::string> parts;
    size_t comma = spec.find(',');
    if (comma == std::string::npos) {
        if (assets.find(spec + ".hg3")) parts.push_back(spec);
        return parts;
    }
    // `Base,a,b,c` -> Base_a, Base_0b, Base_00c, ... (argument k zero-padded to k digits)
    std::string base = spec.substr(0, comma);
    size_t k = 1;
    size_t pos = comma + 1;
    while (pos <= spec.size()) {
        size_t next = spec.find(',', pos);
        if (next == std::string::npos) next = spec.size();
        std::string arg = spec.substr(pos, next - pos);
        if (!arg.empty() && arg != "0") {
            std::string padded = arg.size() < k ? std::string(k - arg.size(), '0') + arg : arg;
            std::string name = base + "_" + padded;
            if (assets.find(name + ".hg3")) parts.push_back(name);
        }
        ++k;
        pos = next + 1;
    }
    return parts;
}

const ImageGeometry* ScenePlayer::geometry(const std::string& asset) {
    auto it = geometryCache_.find(asset);
    if (it == geometryCache_.end()) {
        std::optional<ImageGeometry> g;
        if (auto ref = assets_.find(asset + ".hg3")) {
            try {
                HgImage img = HgImage::parse(ref->archive->read(*ref->entry));
                const HgFrame& f = img.frames().front();
                g = ImageGeometry{static_cast<int32_t>(f.width), static_cast<int32_t>(f.height), f.offsetX, f.offsetY,
                                  f.baseX, f.baseY, f.attributes};
            } catch (const Error& e) {
                if (log) log("bad image " + asset + ": " + e.what());
            }
        }
        it = geometryCache_.emplace(asset, std::move(g)).first;
    }
    return it->second ? &*it->second : nullptr;
}

StageImage* ScenePlayer::slot(Layer layer, int32_t index, bool create) {
    auto& slots = pending_.layer(layer);
    auto it = slots.find(index);
    if (it != slots.end()) return &it->second;
    if (!create) return nullptr;
    StageImage img;
    auto def = layout_.imagePlane.find({layer, index});
    img.plane = def == layout_.imagePlane.end() ? 0 : def->second;
    // A cleared object is reset to its slot's default position (sscript, Fruit 0x00d2ac).
    auto pos = layout_.imagePos.find({layer, index});
    if (pos != layout_.imagePos.end()) {
        img.x = Tween::constant(pos->second.first);
        img.y = Tween::constant(pos->second.second);
    }
    img.settledX = static_cast<float>(img.x.value(frame_));  // the record's default (see settlePositions)
    img.settledY = static_cast<float>(img.y.value(frame_));
    return &slots.emplace(index, std::move(img)).first->second;
}

std::shared_ptr<const EmoteModel> ScenePlayer::emoteModel(const std::string& name) {
    // Recently used models stay loaded (a scene swaps a character's costumes and sizes back and forth).
    auto keep = [this](const std::shared_ptr<const EmoteModel>& m) {
        std::erase(recentModels_, m);
        recentModels_.push_front(m);
        if (recentModels_.size() > 8) recentModels_.pop_back();
    };
    if (auto it = emoteModels_.find(name); it != emoteModels_.end()) {
        if (auto m = it->second.lock()) {
            keep(m);
            return m;
        }
    }
    std::shared_ptr<const EmoteModel> m;
    try {
        Bytes file = assets_.read(name);
        if (auto model = EmoteModel::load(file, assets_.emoteKey(file))) m = std::make_shared<const EmoteModel>(std::move(*model));
    } catch (const Error&) {
    }
    if (!m) {
        missing(name);
        return nullptr;
    }
    emoteModels_[name] = m;
    keep(m);
    return m;
}

void ScenePlayer::startEmote(StageImage& img, int32_t bank) {
    auto model = emoteModel(img.emote);
    if (!model) return;
    if (img.emoteLipVar.empty()) {
        // Restored from a save (lip settings are not kept): read them from the spec again.
        if (auto r = emoteProfiles_.resolve(img.spec)) {
            img.emoteLipVar = r->lipVariable;
            img.emoteLipRest = r->lipRest;
            img.emoteLipWeights = r->lipWeights;
            img.emoteLipLatency = r->lipLatency;
            img.emoteLipSmooth = r->lipSmooth;
        }
    }
    img.emotePlayer = std::make_shared<EmotePlayer>(model, static_cast<uint32_t>(frame_ * 2654435761u + bank + 1));
    for (const auto& [name, v] : img.emoteVars) img.emotePlayer->setVariable(name, v * emoteVarScale(img, name));
    // The idle as a difference timeline (Nekopara 0x571f88, Amakano 0x4d2126: flags 2).
    if (!img.emoteIdle.empty()) img.emotePlayer->playTimeline(img.emoteIdle, EmotePlayer::kTimelineDifference);
    if (auto r = emoteProfiles_.resolve(img.spec)) img.emotePlayer->setHostBlinks(hostBlinks(*r));
    img.emotePlayer->setBlink(img.emoteBlink);
}

void ScenePlayer::pinLoaded(StageImage& img) {
    if (auto r = emoteProfiles_.resolve(img.spec)) {
        for (const auto& [name, arg] : r->varArg) {
            auto chain = r->chains.find(arg);
            if (chain == r->chains.end() || chain->second.first != 6) img.emotePinned.insert(name);
        }
    }
    if (img.emoteBodyDif.empty()) return;
    const std::vector<std::string> defaults = emoteProfiles_.bodyDefaults(img.emoteBodyDif);
    std::vector<std::string> cur;
    size_t start = 0;
    for (size_t c; (c = img.emoteBody.find(',', start)) != std::string::npos; start = c + 1) cur.push_back(img.emoteBody.substr(start, c - start));
    if (!img.emoteBody.empty()) cur.push_back(img.emoteBody.substr(start));
    for (size_t i = 0; i < defaults.size(); ++i) {
        const int32_t digit = static_cast<int32_t>(i) + 1;
        if (emoteProfiles_.bodyChain(img.emoteBodyDif, digit).first == 6) continue;
        const std::string value = i < cur.size() && !cur[i].empty() && cur[i] != "@" ? cur[i] : defaults[i];
        for (const auto& [name, v] : emoteProfiles_.bodyPattern(img.emoteBodyDif, digit, value)) {
            img.emoteVars[name] = v;
            img.emotePinned.insert(name);
        }
    }
}

float ScenePlayer::emoteVarSpeed(const StageImage& img, const std::string& name) {
    auto it = img.emoteSpeedVars.find(name);
    return it != img.emoteSpeedVars.end() ? it->second : img.emoteSpeedAll;
}

float ScenePlayer::emoteVarScale(const StageImage& img, const std::string& name) {
    auto it = img.emoteScaleVars.find(name);
    return it != img.emoteScaleVars.end() ? it->second : img.emoteScaleAll;
}

void ScenePlayer::applyBody(StageImage& img, const std::string& digits, bool animate, bool all) {
    if (img.emoteBodyDif.empty()) {
        img.emoteBody = digits;
        return;
    }
    auto split = [](const std::string& text) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t c; (c = text.find(',', start)) != std::string::npos; start = c + 1) out.push_back(text.substr(start, c - start));
        if (!text.empty()) out.push_back(text.substr(start));
        return out;
    };
    const std::vector<std::string> defaults = emoteProfiles_.bodyDefaults(img.emoteBodyDif);
    const std::vector<std::string> cur = split(img.emoteBody), next = split(digits);
    std::string merged;
    for (size_t i = 0; i < defaults.size(); ++i) {
        // Native 904: `@` (or nothing) keeps the digit; an empty record takes the model's (0x587430).
        const std::string from = i < cur.size() && !cur[i].empty() && cur[i] != "@" ? cur[i] : defaults[i];
        std::string to = i < next.size() && !next[i].empty() ? next[i] : from;
        if (to == "@") to = from;
        if (i > 0) merged += ',';
        merged += to;
        const auto vars = emoteProfiles_.bodyPattern(img.emoteBodyDif, static_cast<int32_t>(i) + 1, to);
        for (const auto& [name, v] : vars) img.emoteVars[name] = v;
        if (!img.emotePlayer || (!all && to == from)) continue;
        // 0x5735a0: the chain's move (types 1-4) on a shown bank, at once otherwise; `nomove` (6)
        // leaves the variables.
        auto [type, frames] = emoteProfiles_.bodyChain(img.emoteBodyDif, static_cast<int32_t>(i) + 1);
        if (type == 6) continue;
        const bool move = animate && type >= 1 && type <= 4 && frames > 0;
        const uint8_t curve = static_cast<uint8_t>(type <= 1 ? 0 : type - 1);
        // The record's mode is the chain type, 0 when not moving (0x5735a0: a crossfade, no move
        // asked, a hidden bank); mode 0 is re-sent every frame (see StageImage::emotePinned).
        const bool pin = !animate || type == 0;
        for (const auto& [name, v] : vars) {
            if (pin) img.emotePinned.insert(name);
            else img.emotePinned.erase(name);
            // 0x56d280: the frames over the variable's speed (truncated), the target times its scale.
            const float speed = emoteVarSpeed(img, name);
            const int32_t f = move && speed > 0 ? static_cast<int32_t>(static_cast<float>(frames) / speed) : 0;
            img.emotePlayer->hostTween(name, v * emoteVarScale(img, name), f, curve);
        }
    }
    img.emoteBody = merged;
}

std::vector<EmotePlayer::HostBlink> ScenePlayer::hostBlinks(const EmoteProfiles::Resolved& r) {
    std::vector<EmotePlayer::HostBlink> out;
    for (const auto& b : r.blinks) out.push_back({b.variable, b.rest, b.start, b.end, b.frames, b.min, b.range});
    return out;
}

std::shared_ptr<const AnmScript> ScenePlayer::anmScript(const std::string& part) {
    auto it = anmScripts_.find(part);
    if (it != anmScripts_.end()) return it->second;
    std::shared_ptr<const AnmScript> s;
    if (auto ref = assets_.find(part + ".anm")) {
        try {
            if (auto p = AnmScript::parse(ref->archive->read(*ref->entry))) s = std::make_shared<const AnmScript>(std::move(*p));
        } catch (const Error&) {
        }
    }
    return anmScripts_.emplace(part, std::move(s)).first->second;
}

void ScenePlayer::linkLips(int32_t channel, const std::string& voice) {
    // A new sound on the channel ends its old links (message 0x13d with no sound: the record goes
    // and the mouth variable is set back to 0).
    for (auto it = lipLinks_.begin(); it != lipLinks_.end();) {
        if (it->second.channel != channel) {
            ++it;
            continue;
        }
        for (auto* stage : {&pending_, &shown_}) {
            auto& slots = stage->layer(it->first.first);
            if (auto img = slots.find(it->first.second); img != slots.end() && img->second.anm) {
                for (auto& p : img->second.anm->players) {
                    if (p) p->setVar(0, 0);
                }
            }
        }
        it = lipLinks_.erase(it);
    }
    const auto& entries = emoteProfiles_.imageLipChannels();
    if (entries.empty()) return;
    std::string v = voice;
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& e : entries) {
        if (!v.starts_with(e.sound)) continue;
        // 0x099c4e: the scene's objects in order, at most two per entry; each gets the voice (native
        // 879 -> 0x13d) and the entry's limit (881 -> 0x13f). Names compare without case (stricmp).
        int linked = 0;
        for (Layer layer : {Layer::Bg, Layer::Cg, Layer::Eg, Layer::Fg, Layer::Fw}) {
            for (const auto& [i, img] : pending_.layer(layer)) {
                if (linked >= 2) break;
                std::string spec = img.spec;
                for (char& c : spec) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (spec.empty() || !spec.starts_with(e.image)) continue;
                lipLinks_[{layer, i}] = LipLink{channel, e.limit};
                ++linked;
            }
        }
    }
}

void ScenePlayer::stepAnimations() {
    // The lip-synced images' mouths first (kcTaskImageList's update, cs2.exe 0x4fc320): variable 0
    // is 1 while the voice's level is at least the limit. The level is the sound task's for the
    // 50 ms of the voice playing now (message 0xd5 -> 0x4d7d30; see voiceLevels), 0 once the
    // channel's voice has stopped.
    std::map<std::pair<Layer, int32_t>, int32_t> mouths;
    for (const auto& [slot, link] : lipLinks_) {
        int32_t level = 0;
        if (auto v = voiceChannels_.find(link.channel); v != voiceChannels_.end() && frame_ >= v->second.second) {
            const auto& levels = voiceLevels(v->second.first);
            const size_t chunk = static_cast<size_t>((frame_ - v->second.second) * 1000 / 60 / 50);
            if (chunk < levels.size()) level = static_cast<int32_t>(levels[chunk]);
        }
        mouths[slot] = level >= link.limit ? 1 : 0;
        if (log && std::getenv("NYAN_ANM_TRACE")) {
            log("anm lip " + std::string(toString(slot.first)) + " " + std::to_string(slot.second) + " f" + std::to_string(frame_) +
                " level " + std::to_string(level) + " -> " + std::to_string(mouths[slot]));
        }
    }
    std::set<const StageAnm*> stepped;
    for (auto* stage : {&pending_, &shown_}) {
        for (size_t l = 0; l < kLayerCount; ++l) {
            const Layer layer = static_cast<Layer>(l);
            for (auto& [i, img] : stage->layer(layer)) {
                if (img.solid || !img.emote.empty() || img.parts.empty()) continue;
                if ((!img.anm || img.anm->parts != img.parts) && stage == &shown_) {
                    // Loaded and shown in the same frame: the copy shares the loaded image's scripts
                    // (the engine has one image; the loaded parts start new scripts, 0x650560).
                    const auto& slots = pending_.layer(layer);
                    if (auto o = slots.find(i); o != slots.end() && o->second.anm && o->second.anm->parts == img.parts) img.anm = o->second.anm;
                }
                if (!img.anm || img.anm->parts != img.parts) {
                    auto a = std::make_shared<StageAnm>();
                    a->parts = img.parts;
                    a->playing = sceneAnimations_;
                    for (size_t k = 0; k < img.parts.size(); ++k) {
                        auto s = anmScript(img.parts[k]);
                        uint32_t seed = static_cast<uint32_t>(frame_ * 2654435761u) ^ static_cast<uint32_t>(l * 977 + static_cast<size_t>(i) * 31 + k);
                        for (char c : img.parts[k]) seed = seed * 16777619u ^ static_cast<uint8_t>(c);
                        a->players.push_back(s ? std::make_unique<AnmPlayer>(std::move(s), seed) : nullptr);
                    }
                    img.anm = std::move(a);
                }
                StageAnm& a = *img.anm;
                if (!a.playing || !stepped.insert(&a).second) continue;
                const auto m = mouths.find({layer, i});
                for (size_t k = 0; k < a.players.size(); ++k) {
                    auto& p = a.players[k];
                    if (!p) continue;
                    if (m != mouths.end()) p->setVar(0, m->second);
                    const int32_t before = p->frame();
                    p->advance(frame_);
                    if (log && p->frame() != before && std::getenv("NYAN_ANM_TRACE")) {
                        log("anm " + std::string(toString(layer)) + " " + std::to_string(i) + " " + a.parts[k] + " f" + std::to_string(frame_) +
                            " frame " + std::to_string(p->frame()));
                    }
                }
            }
        }
    }
}

void ScenePlayer::stepEmotes() {
    // Each bank's player once per frame (the stage's copies share it), with lip sync from the
    // voice (see lipTable).
    std::set<EmotePlayer*> stepped;
    for (auto* stage : {&pending_, &shown_}) {
        for (auto& [i, img] : stage->layer(Layer::Mpl)) {
            if (img.emote.empty()) continue;
            if (!img.emotePlayer) {
                // A bank restored from a save: its model is loaded again, as on PC, where a load
                // reloads each bank's model (0x571a30 -> 0x5735a0 with no move: every body digit as
                // mode 0, like the spec's arguments at a model load).
                startEmote(img, i);
                pinLoaded(img);
            }
            EmotePlayer* p = img.emotePlayer.get();
            if (!img.emoteFading(frame_)) img.emoteFadeFrom.reset();
            if (!p || !stepped.insert(p).second) continue;
            // During a crossfade the bank's update is skipped (0x4dae30 returns before it).
            if (img.emoteFading(frame_)) continue;
            // The sender's mode-0 records, again (0x573d60 -> 0x56d280, before the player's update):
            // immediate, so a timeline's move on them lasts at most the frame it starts. Then the
            // records are dropped unless the player is animating (0x56d280's end: IsAnimating); a
            // looping idle alone doesn't count, so they rarely outlive the frame they were sent.
            for (const auto& name : img.emotePinned) {
                if (auto v = img.emoteVars.find(name); v != img.emoteVars.end())
                    p->setVariable(name, v->second * emoteVarScale(img, name));
            }
            if (!img.emotePinned.empty() && !p->animating()) img.emotePinned.clear();
            p->setBlink(img.emoteBlink && emoteSettings_.motion && emoteSettings_.blink);
            if (!img.emoteLipVar.empty()) {
                // cs2.exe (0x4e2410): while the voice plays, the analyser's value for its position;
                // otherwise the expression's rest value (StopLip, 0x4e2cc0).
                float v = img.emoteLipRest;
                if (img.emoteLip && emoteSettings_.motion && emoteSettings_.lip && !img.emoteVoice.empty() &&
                    frame_ >= img.emoteVoiceStart) {
                    const auto& table = lipTable(img.emoteVoice, img.emoteLipWeights, img.emoteLipSmooth);
                    uint64_t t = frame_ - img.emoteVoiceStart;
                    const bool levels = lipLevels_.count(lipKey(img.emoteVoice, img.emoteLipWeights, img.emoteLipSmooth)) != 0;
                    if (table.empty()) v = lipFlowValue(img, p).value_or(img.emoteLipRest);  // no lip data: #lip_flow
                    else if (t < static_cast<uint64_t>(std::max(0, img.emoteLipLatency))) v = levels ? img.emoteLipRest : 0;
                    else if (t - static_cast<uint64_t>(std::max(0, img.emoteLipLatency)) < table.size()) v = 5.0f * table[t - static_cast<uint64_t>(std::max(0, img.emoteLipLatency))];
                }
                p->setVariable(img.emoteLipVar, v);
                if (log && std::getenv("NYAN_LIP_TRACE") && !img.emoteVoice.empty()) {
                    log("lip " + std::to_string(i) + " f" + std::to_string(frame_) + " " + img.emoteVoice + " +" + std::to_string(frame_ - img.emoteVoiceStart) + " " +
                        img.emoteLipVar + "=" + std::to_string(v));
                }
            }
            // Where the character stands, for the physics: cs2.exe sends the bank's position to the
            // DLL every frame (kcTaskM2Plane message 0x33 -> 0x4d4080, SetCoord at 0x4d3c6c), which
            // divides it by the scale (0x1000be30); here the anchor plus motions over the bank's scale.
            MotionState ms = evaluate(img.motions, frame_);
            float ax = (ms.hasPos ? ms.x : static_cast<float>(img.x.value(frame_))) + ms.dx;
            float ay = (ms.hasPos ? ms.y : static_cast<float>(img.y.value(frame_))) + ms.dy;
            float zoom = (ms.setScaleX >= 0 ? ms.setScaleX : img.scaleX.value(frame_) / 100.0f + ms.addScaleX) * ms.scaleX;
            // The coordinate and scale cs2.exe hands the DLL (0x574070): the bank's position plus the
            // .dif's #position and #scaleoffset x (1 - zoom); #scale x zoom (the x zoom for both).
            float cx = ax + static_cast<float>(img.emoteX - layout_.screenWidth / 2) + img.emoteScaleOffX * (1.0f - zoom);
            float cy = ay + static_cast<float>(img.emoteY - layout_.screenHeight / 2) + img.emoteScaleOffY * (1.0f - zoom);
            float s = img.emoteScale * zoom;
            // The bank's touch action first (0x574070 -> 0x591940), then the model's update.
            for (auto& [b, a] : actions_) {
                if (a.player.lock().get() != p) continue;
                const size_t before = a.next;
                a.update(frame_, *p);
                if (a.next != before && log && std::getenv("NYAN_TOUCH_TRACE"))
                    log("touch: bank " + std::to_string(b) + " f" + std::to_string(frame_) + " step " + std::to_string(before));
            }
            p->step(1, std::fabs(s) > 1e-4f ? cx / s : cx, std::fabs(s) > 1e-4f ? cy / s : cy);
            if (const char* v = std::getenv("NYAN_VAR_TRACE"); v && log) {
                if (std::string(v) == "*") {  // every variable (after the springs: see "iner" below)
                    std::string line = "vars " + std::to_string(i) + " f" + std::to_string(frame_);
                    for (const auto& [name, value] : p->values()) line += " " + name + "=" + std::to_string(value);
                    log(line);
                } else {
                    log("var " + std::to_string(i) + " f" + std::to_string(frame_) + " " + v + "=" + std::to_string(p->values()[v]));
                }
            }
            // cs2.exe's springs (0x573d60 -> 0x5930c0) at the bank's coordinate (message 0x33: its
            // position with its motions, quakes included); the group levels and switches first.
            if (img.emoteInertia) {
                for (const auto& [group, level] : emoteSettings_.inerLevel) img.emoteInertia->setLevel(group, level);
                for (const auto& [group, on] : emoteSettings_.inerEnable) img.emoteInertia->setEnabled(group, on != 0);
                img.emoteInertia->setPaused(img.emoteInerPaused);
                img.emoteInertia->step(ax, ay, *p, emoteSettings_.motion && emoteSettings_.iner);
                if (log && std::getenv("NYAN_PHYS_TRACE")) {
                    log("iner " + std::to_string(i) + " f" + std::to_string(frame_) + " at " + std::to_string(ax) + "," + std::to_string(ay) +
                        " bust_UD=" + std::to_string(p->values()["bust_UD"]) + " bust_LR=" + std::to_string(p->values()["bust_LR"]));
                }
            }
            if (log && std::getenv("NYAN_PHYS_TRACE") && !p->model().busts().empty()) {
                const auto& b = p->model().busts().front();
                log("phys " + std::to_string(i) + " f" + std::to_string(frame_) + " at " + std::to_string(ax) + "," + std::to_string(ay) + " " + b.varLR + "=" +
                    std::to_string(p->variable(b.varLR)) + " " + b.varUD + "=" + std::to_string(p->variable(b.varUD)));
            }
        }
    }
}

std::string ScenePlayer::lipKey(const std::string& voice, const std::array<float, 6>& weights, int32_t smooth) {
    std::string key = voice;
    for (float w : weights) key += "|" + std::to_string(w);
    return key + "|" + std::to_string(smooth);
}

const std::vector<float>& ScenePlayer::lipTable(const std::string& voice, const std::array<float, 6>& weights,
                                               int32_t smooth) {
    // cs2.exe's lip analyser (0x4fdd40 / 0x4fde90 / 0x4fde00) over `<voice>.lpb` ("LPD", 32-byte
    // records from offset 20: frame, vowel class 0-5 (silence a i u e o), ..., the segment's peak dB
    // at +28; the header's silence level at +16, -99 if absent). Frame i takes the record for i (or
    // the last before it): x = min(max(20, -peak), -silence), level = 0.4 * (1 - (x - 20) /
    // (-silence - 20)) + 0.6, times the class's weight; then each value is averaged with `smooth`
    // neighbours on each side. The mouth is 0 + (5 - 0) * value.
    // Without one, `<voice>.lv` (Nekopara; cs2.exe 0x5eabf0 loads it, MotLipLV 0x5a1e20 / 0x5a1f30):
    // a float level per frame, averaged with `smooth` neighbours on each side, used as the mouth's
    // value itself (0x58436f: the variable is set to it, no scale). Stored here divided by 5 so both
    // read the same way; `lipLevels_` marks it, for the rest value outside the file.
    const std::string key = lipKey(voice, weights, smooth);
    auto it = lipTables_.find(key);
    if (it != lipTables_.end()) return it->second;
    if (lipTables_.size() > 64) lipTables_.clear(), lipLevels_.clear();
    std::vector<float> table;
    if (!assets_.find(voice + ".lpb")) {
        if (auto ref = assets_.find(voice + ".lv")) {
            Bytes b = ref->archive->read(*ref->entry);
            const size_t n = b.size() / 4;
            std::vector<float> raw(n);
            for (size_t i = 0; i < n; ++i) std::memcpy(&raw[i], b.data() + i * 4, 4);
            table = raw;
            if (smooth > 0) {
                for (size_t i = static_cast<size_t>(smooth); i + static_cast<size_t>(smooth) < n; ++i) {
                    float sum = 0;
                    for (int32_t d = -smooth; d <= smooth; ++d) sum += raw[static_cast<size_t>(static_cast<int64_t>(i) + d)];
                    table[i] = sum / static_cast<float>(2 * smooth + 1);
                }
            }
            for (float& v : table) v /= 5.0f;
            lipLevels_.insert(key);
        }
    }
    if (auto ref = assets_.find(voice + ".lpb")) {
        Bytes b = ref->archive->read(*ref->entry);
        uint32_t count = b.size() >= 20 ? readLE32(b.data() + 12) : 0;
        if (b.size() >= 20 && std::memcmp(b.data(), "LPD\0", 4) == 0 && readLE32(b.data() + 8) == 32 && count &&
            b.size() >= 20 + static_cast<size_t>(count) * 32) {
            float silence;
            std::memcpy(&silence, b.data() + 16, 4);
            auto rec = [&](uint32_t k) { return b.data() + 20 + static_cast<size_t>(k) * 32; };
            uint32_t n = readLE32(rec(count - 1)) + 1;
            std::vector<float> raw(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                if (count <= 1) break;
                const uint8_t* use = nullptr;
                const uint8_t* prev = rec(0);
                for (uint32_t k = 1; k < count; ++k) {
                    uint32_t f = readLE32(rec(k));
                    if (f == i) {
                        use = rec(k);
                        break;
                    }
                    if (f > i) {
                        use = prev;
                        break;
                    }
                    prev = rec(k);
                }
                if (!use) continue;
                float peak;
                std::memcpy(&peak, use + 28, 4);
                uint32_t cls = readLE32(use + 4);
                float x = std::min(std::max(20.0f, -peak), -silence);
                float range = -silence - 20.0f;
                float level = range != 0 ? (range - (x - 20.0f)) / range * 0.4f + 0.6f : 1.0f;
                raw[i] = cls < 6 ? level * weights[cls] : 0.0f;
            }
            table = raw;
            if (smooth > 0) {
                for (uint32_t i = static_cast<uint32_t>(smooth); i + static_cast<uint32_t>(smooth) < n; ++i) {
                    float sum = 0;
                    for (int32_t d = -smooth; d <= smooth; ++d) sum += raw[static_cast<size_t>(static_cast<int64_t>(i) + d)];
                    table[i] = sum / static_cast<float>(2 * smooth + 1);
                }
            }
        }
    }
    return lipTables_.emplace(key, std::move(table)).first->second;
}

uint64_t ScenePlayer::voiceLength(const std::string& voice) {
    auto it = voiceLengths_.find(voice);
    if (it != voiceLengths_.end()) return it->second;
    if (voiceLengths_.size() > 256) voiceLengths_.clear();
    uint64_t frames = 0;
    if (auto ref = assets_.find(voice + ".ogg")) {
        Bytes b = ref->archive->read(*ref->entry);
        int err = 0;
        if (stb_vorbis* v = stb_vorbis_open_memory(b.data(), static_cast<int>(b.size()), &err, nullptr)) {
            const stb_vorbis_info info = stb_vorbis_get_info(v);
            const uint64_t samples = stb_vorbis_stream_length_in_samples(v);
            if (info.sample_rate > 0) frames = (samples * 60 + info.sample_rate - 1) / info.sample_rate;
            stb_vorbis_close(v);
        }
    }
    return voiceLengths_.emplace(voice, frames).first->second;
}

const std::vector<float>& ScenePlayer::voiceLevels(const std::string& voice) {
    // The sound task's levels (cs2.exe 0x5d0370, a chunk at a time as it decodes into its buffer):
    // the mean absolute difference between neighbouring 16-bit samples, the two channels' averaged,
    // per tenth of the buffer, which holds half a second (0x5d1440: nAvgBytesPerSec / 2), so per
    // 50 ms.
    auto it = voiceLevels_.find(voice);
    if (it != voiceLevels_.end()) return it->second;
    if (voiceLevels_.size() > 32) voiceLevels_.clear();
    std::vector<float> out;
    if (auto ref = assets_.find(voice + ".ogg")) {
        Bytes b = ref->archive->read(*ref->entry);
        int channels = 0, rate = 0;
        short* pcm = nullptr;
        const int frames = stb_vorbis_decode_memory(b.data(), static_cast<int>(b.size()), &channels, &rate, &pcm);
        if (frames > 0 && pcm && channels > 0 && rate > 0) {
            const int chunk = std::max(2, rate / 20);
            for (int start = 0; start < frames; start += chunk) {
                const int n = std::min(chunk, frames - start);
                int64_t sum = 0;
                for (int k = 0; k + 1 < n; ++k) {
                    const short* a = pcm + static_cast<size_t>(start + k) * static_cast<size_t>(channels);
                    const short* c = a + channels;
                    if (channels >= 2) sum += (std::abs(a[0] - c[0]) + std::abs(a[1] - c[1])) >> 1;
                    else sum += std::abs(a[0] - c[0]);
                }
                out.push_back(static_cast<float>(sum / std::max(1, n)));
            }
        }
        std::free(pcm);
    }
    return voiceLevels_.emplace(voice, std::move(out)).first->second;
}

std::optional<float> ScenePlayer::lipFlowValue(const StageImage& img, const EmotePlayer* player) {
    auto& spec = lipSpecs_[img.spec];
    if (!spec) {
        if (lipSpecs_.size() > 64) lipSpecs_.clear();
        auto r = emoteProfiles_.resolve(img.spec);
        spec = std::make_shared<const EmoteProfiles::Resolved>(r ? std::move(*r) : EmoteProfiles::Resolved{});
    }
    const auto& R = *spec;
    LipFlow& f = lipFlows_[player];
    if (R.lipFlow.empty() || R.lipPatterns.empty()) return std::nullopt;
    if (f.voice != img.emoteVoice || f.voiceStart != img.emoteVoiceStart) f = LipFlow{img.emoteVoice, img.emoteVoiceStart};
    auto pattern = [&](int32_t k) { return k >= 0 && static_cast<size_t>(k) < R.lipPatterns.size() ? R.lipPatterns[static_cast<size_t>(k)] : R.lipPatterns[0]; };
    const auto& steps = R.lipFlow;
    auto begin = [&] {  // 0x584fc0: the first step current, the second queued
        f.step = 0, f.ended = false, f.stepStarted = false;
        f.cur = pattern(steps[0].pattern), f.frames = steps[0].frames, f.check = steps[0].check;
        if (steps.size() > 1) f.next = pattern(steps[1].pattern), f.nextFrames = steps[1].frames, f.nextCheck = steps[1].check;
    };
    const auto& levels = voiceLevels(img.emoteVoice);
    const uint64_t t = frame_ - img.emoteVoiceStart;
    const size_t chunk = static_cast<size_t>(t * 1000 / 60 / 50);
    if (chunk + 1 >= levels.size()) {
        // The voice has ended (0x5748a0): the rest value at once, the flow off.
        const bool was = f.active;
        f.active = false;
        if (was) return R.lipRest;
        return std::nullopt;
    }
    // 0x574640, when the level ahead of the play position changes: at or over the table's minimum
    // the flow runs (starting if it was off); under it, a running flow is asked to stop once fewer
    // than a tenth of the levels ahead are loud. cs2 sees the levels its buffer already holds; here
    // the next four seconds (its 80 chunks), of which it checks the first quarter.
    const float level = levels[chunk + 1];
    if (level != f.lastLevel) {
        f.lastLevel = level;
        if (level >= R.lipLevelMin) {
            if (!f.active) begin();
            f.active = true;
        } else if (f.active) {
            const size_t n = 80, look = n / 4;
            size_t loud = 0;
            for (size_t k = 1; k < look; ++k) {
                if (chunk + k < levels.size() && levels[chunk + k] >= R.lipLevelMin) ++loud;
            }
            if (loud < n / 10) f.stop = true;
        }
    }
    if (!f.active) return std::nullopt;
    // 0x583f40: each step for its frames, linear (or held) from its pattern to the next one's.
    if (!f.stepStarted) f.stepStart = frame_, f.stepStarted = true;
    const float k = f.frames > 0 ? static_cast<float>(frame_ - f.stepStart) / static_cast<float>(f.frames) : 1.0f;
    if (k >= 1.0f) {
        if (f.ended) {
            f.active = false, f.stop = false, f.ended = false;
            return R.lipRest;
        }
        if (f.stop && f.check) {
            f.ended = true;  // 0x5851d0: closing on pattern 0
            f.next = R.lipPatterns[0];
        } else {
            // 0x5850b0: the queued step becomes current, the one after it queued (wrapping round).
            f.step = f.step + 1 >= steps.size() ? 0 : f.step + 1;
            f.cur = f.next, f.frames = f.nextFrames, f.check = f.nextCheck, f.ended = false;
            const size_t q = f.step + 1 < steps.size() ? f.step + 1 : 0;
            f.next = pattern(steps[q].pattern), f.nextFrames = steps[q].frames, f.nextCheck = steps[q].check;
        }
        f.stepStart = frame_;
        return f.cur;
    }
    return R.lipInterp == 1 ? f.cur + (f.next - f.cur) * k : f.cur;
}

void ScenePlayer::emoteCommand(const std::vector<std::string>& args) {
    // `mpl` (Amakano 2): E-mote character banks. `mpl <n> <spec>` loads a model through the E-mote
    // profiles; `mpl <n> pos` alone puts the bank back at its vram.xml position; the motions and
    // properties of image slots (pos, scale, fade, arc, quake, stop ...) apply as for `cg`.
    if (args.size() >= 2 && args[1] == "touch") {
        // `mpl touch` with no bank (as Nekopara does): every bank.
        std::vector<int32_t> banks;
        for (const auto& [i, img] : pending_.layer(Layer::Mpl)) banks.push_back(i);
        for (int32_t i : banks) {
            std::vector<std::string> one = args;
            one.insert(one.begin() + 1, std::to_string(i));
            emoteCommand(one);
        }
        return;
    }
    if (args.size() >= 3 && !std::isdigit(static_cast<unsigned char>(args[1][0])) && args[1][0] != '#') {
        imageCommand(Layer::Mpl, args);
        return;
    }
    if (args.size() < 3) {
        imageCommand(Layer::Mpl, args);  // `mpl` clears every bank, `mpl <n>` one
        return;
    }
    int32_t n = argNum(args, 1, 0);
    const std::string& what = args[2];
    if (what.starts_with("m2")) {
        // Model behaviour (interrupt motions, blink and lip-sync switches, physics, attribute
        // points): the pose is drawn still for now.
        static const std::set<std::string> kModel = {"m2intmot", "m2enable_blink", "m2enable_lip",
                                                     "m2splay", "m2draw",
                                                     "m2wmot", "m2eff", "m2rotrep", "m2rotrepc"};
        if (what == "m2att") {
            // `m2att <no> <pos> <x var> <y var> [<ex var>]` (Nekopara's sscript 0x052a0c): the
            // model's attribute point <no> (native 910 -> message 0x211, cs2.exe 0x574540 /
            // 0x587790: the .dif's #attribute row, 0, 0, 0 without one or without a model), the
            // bank's position (+124) added when <pos> is non-zero, into those variables. Scenes
            // place their emotion marks there.
            if (args.size() < 7) return;  // the x and y variables are required
            const int32_t att = argNum(args, 3, 0);
            const bool addPos = argNum(args, 4, 0) != 0;
            std::array<int32_t, 3> v{0, 0, 0};
            const StageImage* img = slot(Layer::Mpl, n, false);
            if (img && !img->emote.empty()) {
                if (auto it = img->emoteAttributes.find(att); it != img->emoteAttributes.end()) v = it->second;
            }
            if (addPos) {
                if (img) {
                    v[0] += img->x.value(frame_);
                    v[1] += img->y.value(frame_);
                } else if (auto def = layout_.imagePos.find({Layer::Mpl, n}); def != layout_.imagePos.end()) {
                    v[0] += def->second.first;
                    v[1] += def->second.second;
                }
            }
            for (size_t i = 0; i < 3 && 5 + i < args.size(); ++i) vars_.set(argNum(args, 5 + i, 0), v[i]);
            if (log && std::getenv("NYAN_FES_TRACE"))
                log("m2att " + std::to_string(n) + " " + std::to_string(att) + " -> " + std::to_string(v[0]) + "," +
                    std::to_string(v[1]) + "," + std::to_string(v[2]));
            return;
        }
        if (what == "m2body") {
            // `m2body [d1,d2,...]` (Nekopara's sscript 0x0536bd): the bank's body digits, kept by the
            // bank (scripts set them right after the model); bare, the bank forgets its own and
            // follows the model's.
            StageImage* img = slot(Layer::Mpl, n, true);
            if (args.size() <= 3) {
                img->emoteBody.clear();
                return;
            }
            // 0x5735a0: no move while a crossfade runs ([0x27]) or the bank is hidden ([0x43]).
            applyBody(*img, args[3], img->emotePlayer && img->visible && emoteSettings_.motion && !img->emoteFading(frame_), false);
            dirty_ = true;
            return;
        }
        if (StageImage* img = slot(Layer::Mpl, n, false)) {
            if (what == "m2intmot" && args.size() > 3) {
                // A one-off timeline (a nod) over the idle motion (Amakano 0x4d5436: flags 1, parallel).
                if (img->emotePlayer) img->emotePlayer->playTimeline(args[3], EmotePlayer::kTimelineParallel);
                return;
            }
            if (what == "m2enable_blink" || what == "m2enable_lip") {
                bool on = argNum(args, 3, 1) != 0;
                (what == "m2enable_blink" ? img->emoteBlink : img->emoteLip) = on;
                if (what == "m2enable_blink" && img->emotePlayer) img->emotePlayer->setBlink(on);
                if (what == "m2enable_lip" && !on) img->emoteVoice.clear();
                return;
            }
        }
        // On an empty bank (one a previous scene would have filled) there is no model to tell.
        if (what == "m2intmot" || what == "m2enable_blink" || what == "m2enable_lip") return;
        if (kModel.count(what)) {
            unhandled("mpl " + what);
            return;
        }
    }
    if (what == "m2fspeed" || what == "m2prmscale") {
        // `m2fspeed [n%] [var...]`, `m2prmscale [n%] [var...]` (sscript 0x05306a / 0x053388): n / 100
        // (natives 46 and 213 strip the %) for the named variables, or the bank's default with none;
        // bare, the default back to 1 and the names forgotten (0x0c2314 / 0x0c26ad).
        StageImage* img = slot(Layer::Mpl, n, true);
        float& all = what == "m2fspeed" ? img->emoteSpeedAll : img->emoteScaleAll;
        auto& vars = what == "m2fspeed" ? img->emoteSpeedVars : img->emoteScaleVars;
        if (args.size() <= 3) {
            all = 1;
            vars.clear();
            return;
        }
        std::string t = args[3];
        if (!t.empty() && t.back() == '%') t.pop_back();
        const float v = static_cast<float>(num(t, 100)) / 100.0f;
        if (args.size() == 4) all = v;
        for (size_t k = 4; k < args.size(); ++k) vars[args[k]] = v;
        dirty_ = true;
        return;
    }
    if (what == "m2iner") {
        // `m2iner pause [var]` (sscript 0x053955, the only form): the variable's spring held at rest
        // (cs2.exe 0x593800, message 0x21a); bare, every pause lifted (0x21b, 0x574630).
        if (args.size() < 4 || args[3] != "pause") return;
        StageImage* img = slot(Layer::Mpl, n, true);
        if (args.size() == 4) img->emoteInerPaused.clear();
        for (size_t k = 4; k < args.size(); ++k) {
            if (std::find(img->emoteInerPaused.begin(), img->emoteInerPaused.end(), args[k]) == img->emoteInerPaused.end())
                img->emoteInerPaused.push_back(args[k]);
        }
        dirty_ = true;
        return;
    }
    if (what == "touch") {
        // `touch [on] [table]` (Nekopara's sscript 0x054482): whether the character can be touched in
        // touch mode (default on) and which .inc #touch action table answers (default 0). It is the
        // bank's, so scripts set it before loading the model.
        StageImage* img = slot(Layer::Mpl, n, true);
        img->touchable = argNum(args, 3, 1) != 0;
        img->touchTable = argNum(args, 4, 0);
        dirty_ = true;
        return;
    }
    if (what == "pos" && args.size() == 3) {
        if (StageImage* img = slot(Layer::Mpl, n, false)) {
            auto def = layout_.imagePos.find({Layer::Mpl, n});
            img->x = Tween::constant(def == layout_.imagePos.end() ? 0 : def->second.first);
            img->y = Tween::constant(def == layout_.imagePos.end() ? 0 : def->second.second);
            dirty_ = true;
        }
        return;
    }
    auto resolved = emoteProfiles_.resolve(what);
    if (!resolved) {
        imageCommand(Layer::Mpl, args);
        return;
    }
    if (!assets_.find(resolved->model)) {
        missing(resolved->model);
        return;
    }
    StageImage* existing = slot(Layer::Mpl, n, false);
    StageImage* img = slot(Layer::Mpl, n, true);
    if (!existing) {
        auto def = layout_.imagePos.find({Layer::Mpl, n});
        img->x = Tween::constant(def == layout_.imagePos.end() ? 0 : def->second.first);
        img->y = Tween::constant(def == layout_.imagePos.end() ? 0 : def->second.second);
        img->plane = layout_.imagePlane.count({Layer::Mpl, n}) ? layout_.imagePlane.at({Layer::Mpl, n}) : n;
    }
    // Crossfade (cs2.exe): a bank already showing a model fades from its old pose when the model
    // changes (0x4d18d0 sets the flag, the length m2view.cnf #fade) or when a changed argument's
    // #chain is fade (0x4d3430). The old player is copied as it stands.
    const bool shown = existing && existing->emotePlayer && !existing->emote.empty() && emoteSettings_.motion &&
                       emoteSettings_.dif;
    int32_t fade = 0;
    if (shown) {
        fade = img->emote != resolved->model ? emoteProfiles_.fadeFrames() : EmoteProfiles::chainFade(*resolved, img->spec, what);
    }
    if (fade > 0) {
        img->emoteFadeFrom = std::make_shared<const EmotePlayer>(*img->emotePlayer);
        img->emoteFadeX = img->emoteX;
        img->emoteFadeY = img->emoteY;
        img->emoteFadeScale = img->emoteScale;
        img->emoteFadeScaleOffX = img->emoteScaleOffX;
        img->emoteFadeScaleOffY = img->emoteScaleOffY;
        img->emoteFadeStart = frame_;
        img->emoteFadeFrames = fade;
        if (log && std::getenv("NYAN_PHYS_TRACE")) log("emote fade " + std::to_string(n) + " f" + std::to_string(frame_) + " " + std::to_string(fade) + " " + img->spec + " -> " + what);
    }
    auto newModelOf = [](const StageImage& i, const EmoteProfiles::Resolved& r) {
        return i.emote != r.model || !i.emotePlayer;
    };
    // The spec's arguments go to the sender as records too (0x572640, Amakano 0x4d25e0): a new model
    // (crossfading or hidden) and a hidden bank give mode 0, re-sent every frame; on a shown bank
    // a changed argument's variables take its #chain type, or its `$` table's move for that change
    // (Nekopara's eyes and brows: linear over 10 frames). The keyframes (0x5766e0) start from the
    // variable's current value when the body profile's #smooth lists it and go straight to the target
    // otherwise; either way the player eases from where the variable is.
    std::map<std::string, std::pair<int32_t, int32_t>> specMoves;  // variable -> (type, frames)
    {
        const bool animateSpec = !newModelOf(*img, *resolved) && fade == 0 && img->visible && emoteSettings_.motion;
        std::vector<std::string> oldArgs;
        if (auto old = emoteProfiles_.resolve(img->spec)) oldArgs = old->args;
        for (const auto& [name, arg] : resolved->varArg) {
            const size_t a = static_cast<size_t>(arg);
            const std::string before = a < oldArgs.size() ? oldArgs[a] : std::string();
            const std::string after = a < resolved->args.size() ? resolved->args[a] : std::string();
            if (animateSpec && before == after) continue;  // unchanged: its record stays as it is
            const auto [type, frames] = resolved->argChain(arg, before, after);
            if (type == 6) continue;
            if (!animateSpec || type == 0) img->emotePinned.insert(name);
            else img->emotePinned.erase(name);
            if (animateSpec) specMoves[name] = {type, frames};
        }
    }
    const std::string oldSpec = img->emote.empty() ? std::string() : img->spec;
    img->spec = what;
    img->parts.clear();
    img->solid = false;
    img->tone = slotTone(Layer::Mpl, n);  // the bank's record tone, as an image slot's
    img->toneLoaded = img->tone;
    img->emoteIdle = resolved->idle;
    img->emoteBodyDif = resolved->bodyDif;
    img->emoteVars = std::move(resolved->vars);
    img->emoteLipVar = resolved->lipVariable;
    img->emoteLipRest = resolved->lipRest;
    img->emoteLipWeights = resolved->lipWeights;
    img->emoteLipLatency = resolved->lipLatency;
    img->emoteLipSmooth = resolved->lipSmooth;
    const bool newModel = newModelOf(*img, *resolved);
    if (newModel) {
        // A new model: a player of its own, its variables set and its idle timeline playing.
        img->emote = resolved->model;
        img->emotePlayer.reset();
        startEmote(*img, n);
        // cs2.exe's springs from the profile (0x591eb0), when it has any (Nekopara; none in Amakano).
        img->emoteInertia.reset();
        if (!resolved->materials.empty()) img->emoteInertia = std::make_shared<EmoteInertia>(*resolved);
        // Another model of the same character (the body profile's #group; 0x5785a0 compares the old and new
        // specs' first fields, path and extension removed): its #group_inertia inputs nudge the
        // springs once.
        if (img->emoteInertia && !oldSpec.empty()) {
            auto first = [](const std::string& spec) { return spec.substr(0, spec.find(',')); };
            const std::string a = first(oldSpec), b = first(what);
            if (a != b && emoteProfiles_.sameGroup(resolved->bodyDif, a, b)) {
                img->emoteInertia->nudge(emoteProfiles_.groupInertia(resolved->bodyDif));
                if (log && std::getenv("NYAN_PHYS_TRACE")) log("emote group nudge " + std::to_string(n) + " f" + std::to_string(frame_) + " " + a + " -> " + b);
            }
        }
    } else {
        // The same model with other parts: the variables change at once, or along a changed
        // argument's eased move (types 1-4; the frames over the variable's speed, as for the body
        // digits, 0x56d280).
        for (const auto& [name, v] : img->emoteVars) {
            auto mv = specMoves.find(name);
            if (mv != specMoves.end() && mv->second.first >= 1 && mv->second.first <= 4 && mv->second.second > 0) {
                const float speed = emoteVarSpeed(*img, name);
                const int32_t f = speed > 0 ? static_cast<int32_t>(static_cast<float>(mv->second.second) / speed) : 0;
                const uint8_t curve = static_cast<uint8_t>(mv->second.first <= 1 ? 0 : mv->second.first - 1);
                img->emotePlayer->hostTween(name, v * emoteVarScale(*img, name), f, curve);
            } else {
                img->emotePlayer->setVariable(name, v * emoteVarScale(*img, name));
            }
        }
    }
    // The bank's body digits on the model (Nekopara; 0x0c51aa: none kept, the model's own).
    applyBody(*img, img->emoteBody, false, newModel);
    // The blinks for the spec's arguments (0x4d3137 enables the entry for each one's value).
    if (img->emotePlayer) img->emotePlayer->setHostBlinks(hostBlinks(*resolved));
    // Placement (cs2.exe 0x574070, Amakano's 0x4d3bb0): emotedriver's coordinates start at the
    // centre of the view, the bank's position and the .dif's correction added.
    img->emoteX = layout_.screenWidth / 2 + resolved->posX;
    img->emoteY = layout_.screenHeight / 2 + resolved->posY;
    img->emoteScale = resolved->scale;
    img->emoteScaleOffX = resolved->scaleOffsetX;
    img->emoteScaleOffY = resolved->scaleOffsetY;
    img->emoteAttributes = resolved->attributes;
    // Coordinates as for images: each one given replaces the bank's position as `pos` does (Amakano
    // gives x alone, a stand position kept in a variable); without them the bank
    // stays and a motion running on it goes on (Nekopara changes a character's model in the frame
    // its `amove1` starts).
    loadPosition(Layer::Mpl, n, *img, args, 3);
    dirty_ = true;
}

void ScenePlayer::imageCommand(Layer layer, const std::vector<std::string>& args) {
    auto& slots = pending_.layer(layer);
    if (layer == Layer::Fw && args.size() > 1) faceSet_ = true;
    if (args.size() == 1) {  // `cg`: clear the whole layer
        if (!slots.empty()) dirty_ = true;
        slots.clear();
        return;
    }
    const std::string& a1 = args[1];
    if (a1 == "stop") {
        for (auto& [i, img] : slots) {
            holdMotionAlpha(img.alpha, img.motions, frame_);
            img.motions.clear();
        }
        return;
    }
    if (a1 == "attr_def") return;
    if (a1 == "packpl") {
        // `<type> packpl <flag> [<plane>]` (Happiness 2's sscript, image keyword 111, 0x048c38):
        // with a plane, each slot of the type keeps its position (native 874: x in the low 16 bits,
        // y in the high) in flag <flag> + 2k and its plane in <flag> + 2k + 1, and moves to the
        // plane where it stays put on screen (0x099465: its position gains the old plane's origin
        // less the new one's, each the plane's position less its base); without one, both come
        // back from the flags (native 875, signed halves). The wc_ effects pack everything onto
        // plane 0 to clone it.
        const int32_t flag = argNum(args, 2, 0);
        const int32_t to = args.size() > 3 ? argNum(args, 3, -1) : -1;
        auto origin = [&](int32_t n) -> std::pair<int32_t, int32_t> {
            if (auto it = pending_.planes.find(n); it != pending_.planes.end()) {
                return {static_cast<int32_t>(it->second.x.value(frame_)) - it->second.baseX,
                        static_cast<int32_t>(it->second.y.value(frame_)) - it->second.baseY};
            }
            if (auto d = layout_.planes.find(n); d != layout_.planes.end()) return {d->second.posX - d->second.baseX, d->second.posY - d->second.baseY};
            if (!layout_.planes.empty()) return {0, 0};  // a plane vram.xml leaves out (see plane())
            return {layout_.planePosX - layout_.planeBaseX, layout_.planePosY - layout_.planeBaseY};
        };
        for (auto& [k, img] : slots) {
            const int32_t at = flag + 2 * k;
            if (to != -1) {
                const auto x = static_cast<int32_t>(img.x.value(frame_)), y = static_cast<int32_t>(img.y.value(frame_));
                vars_.set(at, static_cast<int32_t>((static_cast<uint32_t>(y) << 16) | (static_cast<uint32_t>(x) & 0xFFFF)));
                vars_.set(at + 1, img.plane);
                const auto a = origin(img.plane), b = origin(to);
                img.x = Tween::constant(x + a.first - b.first);
                img.y = Tween::constant(y + a.second - b.second);
                img.plane = to;
            } else {
                const auto v = static_cast<uint32_t>(vars_.get(at));
                img.x = Tween::constant(static_cast<int16_t>(v & 0xFFFF));
                img.y = Tween::constant(static_cast<int16_t>(v >> 16));
                img.plane = vars_.get(at + 1);
            }
        }
        dirty_ = true;
        return;
    }
    // Bare `<type> shift` (every Nekopara scene header; sub-keyword 61, sscript 0x03adf4): puts each
    // slot's shift (record +716 / +720) back to its layer's default, the value a slot starts with
    // (0x008c8c). No scene gives one, so there is nothing to undo.
    if (a1 == "shift" && args.size() == 2) return;
    if (a1 == "tone") {
        // `cg tone ...`: every slot of the layer (vram.xml's), for the images loaded next.
        std::vector<std::string> one = args;
        one.insert(one.begin() + 1, "0");  // as `cg 0 tone ...`
        auto t = toneArgs(one);
        for (const auto& [key, pl] : layout_.imagePlane) {
            if (key.first == layer) setSlotTone(layer, key.second, t);
        }
        for (auto& [i, img] : slots) {
            setSlotTone(layer, i, t);
            img.tone = t;
        }
        return;
    }
    if (a1.size() > 2 && a1.front() == '[' && a1.back() == ']') {
        // `fg [0-9]` clears those slots; `eg [2-3] blend 64` / `eg [0,2-3] ...` applies the command to
        // each of them (as `pl [a,b-c] ...` does for planes).
        std::string list = a1.substr(1, a1.size() - 2);
        for (size_t start = 0; start <= list.size();) {
            size_t comma = list.find(',', start);
            std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (item.empty()) continue;
            size_t dash = item.find('-', 1);
            int32_t lo = num(item.substr(0, dash));
            int32_t hi = dash == std::string::npos ? lo : num(item.substr(dash + 1));
            for (int32_t i = lo; i <= hi; ++i) {
                if (args.size() == 2) {
                    slots.erase(i);
                    dirty_ = true;
                    continue;
                }
                std::vector<std::string> one = args;
                one[1] = std::to_string(i);
                // E-mote banks take their model commands (`mpl [1,2] touch 0`) per bank.
                if (layer == Layer::Mpl) emoteCommand(one);
                else imageCommand(layer, one);
            }
        }
        return;
    }
    if (isImageSubcommand(a1)) {
        // `cg fade 45 255 0`, `bg mode sepia`, `eg shake ...`: no slot number, every slot of the layer
        // (`mpl m2body 5,5,...`: every bank).
        std::vector<int32_t> indices;
        for (const auto& [i, img] : slots) indices.push_back(i);
        for (int32_t i : indices) {
            std::vector<std::string> one = args;
            one.insert(one.begin() + 1, std::to_string(i));
            if (layer == Layer::Mpl) emoteCommand(one);
            else imageCommand(layer, one);
        }
        return;
    }
    int32_t index = argNum(args, 1, -1);
    if (index < 0 && !a1.empty() && a1[0] != '-' && a1[0] != '[') {
        // The slot word goes to native 539 (Nekopara 0x646380: `[a-b]` / `[a,b]` lists, else one
        // value through the calculator, 0x728ac0) wherever it is not a subcommand (every game's
        // image function, Cyanotype 0x01b28f: word 2 to native 539 on both paths). A name is 0
        // there: Hatsumira's `cg R ...`, Labyrinth's `cg b ...`, Fruit's `fg s manpu_kira ...` are
        // slot 0 (engineCalc).
        index = engineCalc(a1, vars_, calcPrivate_);
    }
    if (index < 0) {
        unhandled(std::string(toString(layer)) + " " + a1);
        return;
    }
    if (args.size() == 2 || args[2] == "0") {  // `cg 0` / `eg 0 0 0 0`: clear slot and its properties
        slots.erase(index);
        dirty_ = true;
        return;
    }
    if (!isImageSubcommand(args[2])) {
        setImage(layer, index, args[2], args, 3);
        return;
    }
    // The engine motion groups of what this subcommand starts (as noteValueMotion does for planes).
    const bool m2Command = args[2].starts_with("m2");
    if (StageImage* pre = slot(layer, index, false)) {
        for (auto& m : pre->motions) m.marked = true;
    }
    struct GroupMark {
        ScenePlayer* player;
        Layer layer;
        int32_t index;
        std::string word;
        bool m2;
        ~GroupMark() { player->markImageGroups(layer, index, word, m2); }
    } groupMark{this, layer, index, args[2], m2Command};
    // Newer engines' m2* motions take the same arguments as the classic ones.
    std::string what = args[2];
    if (what == "m2fade") what = "fade";
    else if (what == "m2arc") what = "arc";
    else if (what == "m2scale") what = "mscale";
    else if (what == "m2rot") what = "mrot";
    else if (what == "m2pend") what = "pend";
    // The `p` forms (motAMove1p, motAScale1p, motSplinep) only differ in running on through a skip
    // (their end test leaves out the skip flag).
    if (((what.starts_with("m2amove") && what.size() == 9) || (what.starts_with("m2ascale") && what.size() == 10)) &&
        what.back() == 'p') {
        what.pop_back();
    } else if (what == "m2splinep") {
        what = "m2spline";
    }

    // Properties apply to the slot and persist when an image is (re)loaded into it, so
    // scripts may set them first (`fg 0 blend 0` / `fg 0 scale 30%` / `fg 0 parts_x ...`).
    bool creates = what == "blend" || what == "scale" || what == "disp" || what == "mode" || what == "blur" ||
                   what == "tone" || what == "pri" || what == "pos";
    StageImage* img = slot(layer, index, creates);
    dirty_ = true;
    if (!img && what != "attr") return;  // effects on empty slots are no-ops
    const uint64_t at = frame_ + (img && startsMotion(what) ? static_cast<uint64_t>(std::max(0, img->delay)) : 0);
    auto motion = [&](Motion::Kind kind, bool loop, uint32_t period) {
        Motion m;
        m.kind = kind;
        m.loop = loop;
        m.start = at;
        m.period = std::max<uint32_t>(1, period);
        return m;
    };

    const int32_t obj = static_cast<int32_t>(layer) * 1000 + index;
    if (layer == Layer::Mpl && (what == "rot" || what == "mrot")) {
        // An E-mote bank turns about its model's origin by z alone (see StageImage::rotation); x and
        // y tilts are not passed on. `rot rx ry rz` (pRot) sets it; `mrot <frames> rx ry rz` (motRot)
        // turns by rz from where it is, eased in as planes' are.
        if (what == "rot") {
            img->rotation = static_cast<float>(argNum(args, 5, 0));
        } else {
            const float dz = static_cast<float>(argNum(args, 6, 0));
            img->rotation += dz;
            if (argNum(args, 3, 0) > 0) {
                Motion m = motion(Motion::Kind::Settle, false, static_cast<uint32_t>(argNum(args, 3, 1)));
                m.a[0] = m.a[1] = 1;
                m.a[2] = -dz;
                img->motions.push_back(m);
            }
        }
        dirty_ = true;
        return;
    }
    if (what == "pos") {
        // `pos <x> <y>`: place the image (relative values like `+10` are relative to where it is).
        // pPos takes the place of the motions before it; a relative motion later in this frame
        // starts from the position it replaced.
        int32_t cx = img->x.value(at), cy = img->y.value(at);
        const float drawnX = static_cast<float>(cx) + addedValue(img->motions, Motion::Kind::AddPos, 0, at);
        const float drawnY = static_cast<float>(cy) + addedValue(img->motions, Motion::Kind::AddPos, 1, at);
        const int32_t lastX = lastUpdatePos(img->settledX, drawnX), lastY = lastUpdatePos(img->settledY, drawnY);
        if (args.size() > 3 && args[3].find('@') != std::string::npos) traceAt("pos", obj, 0, lastX, drawnX);
        if (args.size() > 4 && args[4].find('@') != std::string::npos) traceAt("pos", obj, 1, lastY, drawnY);
        notePrior(obj, 0, drawnX);
        notePrior(obj, 1, drawnY);
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::AddPos; });
        // A number left out is the slot's default position (vram.xml's; sscript 0x021501 starts from
        // 0x093d15, which reads the slot's record table), not where the image is: a bare
        // `mpl 0 pos` puts the bank back. `@` is its settled position (lastUpdatePos).
        auto def = layout_.imagePos.find({layer, index});
        const int32_t dx = def != layout_.imagePos.end() ? def->second.first : 0;
        const int32_t dy = def != layout_.imagePos.end() ? def->second.second : 0;
        int32_t nx = argNum(args, 3, dx, lastX), ny = argNum(args, 4, dy, lastY);
        img->x = {cx, nx, at, 0};
        img->y = {cy, ny, at, 0};
        img->loadX = nx;  // a placement, like loading: later `mcmove` targets count from here
        img->loadY = ny;
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
    } else if (what == "base") {
        // `base <x> <y>`: anchor the image at this point of its canvas (pBase); `@` keeps a value.
        int32_t bx = img->baseX == INT32_MIN ? 0 : img->baseX, by = img->baseY == INT32_MIN ? 0 : img->baseY;
        img->baseX = argNum(args, 3, bx, bx);
        img->baseY = argNum(args, 4, by, by);
    } else if (what == "blend") {
        // pAlpha, one byte: sscript keeps the number in a byte (0x0385f0), so Hatsumira's
        // `cg [0-4] blend 2128` is 80.
        img->alpha = {img->alpha.value(at), static_cast<int32_t>(static_cast<uint8_t>(argNum(args, 3, 255))), at, 0};
    } else if (what == "disp") {
        img->visible = argNum(args, 3, 1) != 0;
    } else if (what == "mcfade") {
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Fade; });
        img->motions.push_back(mcFade(args, at, img->alpha.value(at)));
    } else if (what == "fade" || what == "fade2") {
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 3, 0)));
        int32_t cur = img->alpha.value(at);
        // fade2 (motFade2) goes to the target and back over the time.
        img->alpha = {std::clamp(argNum(args, 4, cur, cur), 0, 255), std::clamp(argNum(args, 5, 255, cur), 0, 255),
                      at, frames, static_cast<uint8_t>(what == "fade2" ? 4 : 0)};
    } else if (what == "mcmove") {
        // The target is where the image would go from where it was loaded; from where it is now it
        // travels the same distance. Copies loaded together and then shifted with `move 0 ...` scroll
        // in step and tile (Hatsumira's cloud layers: eg 3-5 at 0/-1280/-2560, all `mcmove ... 2560`).
        Motion m = motion(Motion::Kind::Move, true, static_cast<uint32_t>(argNum(args, 3, 60)));
        int32_t cx = img->x.value(at), cy = img->y.value(at);
        int32_t lx = img->loadX == INT32_MIN ? cx : img->loadX, ly = img->loadY == INT32_MIN ? cy : img->loadY;
        m.a[0] = static_cast<float>(cx);
        m.a[1] = static_cast<float>(cy);
        m.a[2] = static_cast<float>(cx + argNum(args, 4, lx, lx) - lx);
        m.a[3] = static_cast<float>(cy + argNum(args, 5, ly, ly) - ly);
        img->motions.push_back(m);
    } else if (what == "vmove" || what == "m2vmove") {
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 3, 0)));
        int32_t cx = img->x.value(at), cy = img->y.value(at);
        img->x = {cx, argNum(args, 4, cx, cx), at, frames, moveEase(what)};
        img->y = {cy, argNum(args, 5, cy, cy), at, frames, moveEase(what)};
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
    } else if (isMoveWord(what)) {
        relativeMove(obj, img->x, img->y, img->motions, args, at, moveEase(what));
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
    } else if (what == "scale") {
        // pScale (sscript 0x02256a): `n%` and plain numbers are both percent (native 46 with 100,
        // then / 100; E-mote banks the same over their size of 100); one left out is 100%. It takes
        // the place of the scale motions before it; a relative one later in this frame starts from
        // the scale it replaced.
        notePrior(obj, 2, img->scaleX.value(at) / 100.0f + addedValue(img->motions, Motion::Kind::AddScale, 0, at));
        notePrior(obj, 3, img->scaleY.value(at) / 100.0f + addedValue(img->motions, Motion::Kind::AddScale, 1, at));
        auto value = [&](size_t i, const Tween& stored) {
            if (i >= args.size() || !isNumberLike(args[i])) return 100;
            return static_cast<int32_t>(std::lround(100.0f * scaleArg(args[i], 100, 100.0f,
                                                                       static_cast<int32_t>(std::lround(stored.value(at) / 100.0f)))));
        };
        int32_t sx = value(3, img->scaleX), sy = value(4, img->scaleY);
        std::erase_if(img->motions, [](const Motion& o) { return o.kind == Motion::Kind::AddScale; });
        img->scaleX = Tween::constant(sx);
        img->scaleY = Tween::constant(sy);
    } else if (what == "mscale" || scaleEase(what) != 0) {
        // `mscale` / `ascaleN` and their m2 forms (sscript 0x02a3fe / 0x04898e): the target in percent
        // (`@` the current scale) minus the scale the last update left is the change, added along the
        // curve, on top of any still running; one left out is 100%. Images pass native 46 a size of
        // 1, so there `n%` means n / 100 percent (no script writes it); E-mote banks their size, 100.
        uint32_t frames = static_cast<uint32_t>(std::max(0, argNum(args, 3, 0)));
        const int32_t unit = layer == Layer::Mpl ? 100 : 1;
        float cx = prior(obj, 2, img->scaleX.value(at) / 100.0f + addedValue(img->motions, Motion::Kind::AddScale, 0, at));
        float cy = prior(obj, 3, img->scaleY.value(at) / 100.0f + addedValue(img->motions, Motion::Kind::AddScale, 1, at));
        auto target = [&](size_t i, float cur) {
            if (i >= args.size() || !isNumberLike(args[i])) return 1.0f;
            return scaleArg(args[i], unit, 100.0f, static_cast<int32_t>(std::lround(cur * 100.0f)));
        };
        int32_t dx = static_cast<int32_t>(std::lround((target(4, cx) - cx) * 100.0f));
        int32_t dy = static_cast<int32_t>(std::lround((target(5, cy) - cy) * 100.0f));
        bool running = !img->scaleX.done(at) || !img->scaleY.done(at) ||
                       std::any_of(img->motions.begin(), img->motions.end(),
                                   [&](const Motion& m) { return m.kind == Motion::Kind::AddScale && m.active(at); });
        std::erase_if(img->motions, [&](const Motion& m) { return m.kind == Motion::Kind::AddScale && !m.active(at); });
        if (running) {
            img->scaleX.from += dx, img->scaleX.to += dx;
            img->scaleY.from += dy, img->scaleY.to += dy;
            Motion m = motion(Motion::Kind::AddScale, false, std::max<uint32_t>(1, frames));
            m.a[0] = dx / 100.0f;
            m.a[1] = dy / 100.0f;
            m.a[5] = scaleEase(what);
            img->motions.push_back(m);
        } else {
            int32_t bx = img->scaleX.value(at), by = img->scaleY.value(at);
            img->scaleX = {bx, bx + dx, at, frames, scaleEase(what)};
            img->scaleY = {by, by + dy, at, frames, scaleEase(what)};
        }
        std::erase_if(img->motions, [](const Motion& o) { return o.kind == Motion::Kind::Path && o.a[3] != 0; });
    } else if ((what == "spline_s" || what == "m2spline_s") && args.size() > 3) {
        std::erase_if(img->motions, [](const Motion& o) { return o.kind == Motion::Kind::Path && o.a[3] != 0; });
        img->motions.push_back(scaleSpline(args, 3, static_cast<float>(img->scaleX.value(at)),
                                           static_cast<float>(img->scaleY.value(at)), at));
    } else if (what == "arc" || what == "mcarc") {
        // `arc <frames> <deg0> <deg1> <radiusX> <radiusY>`: along a circle (motArc); a one-shot arc
        // leaves the image where it ends.
        Motion m = motion(Motion::Kind::Arc, what == "mcarc", static_cast<uint32_t>(std::max(1, argNum(args, 3, 30))));
        m.a[0] = static_cast<float>(argNum(args, 4, 0));
        m.a[1] = static_cast<float>(argNum(args, 5, 180));
        m.a[2] = static_cast<float>(argNum(args, 6, 0));
        m.a[3] = static_cast<float>(argNum(args, 7, 0));
        img->motions.push_back(m);
        if (!m.loop) {
            auto [ex, ey] = arcEnd(m);
            uint64_t end = m.start + m.period;
            int32_t x = img->x.value(end), y = img->y.value(end);
            if (ex) img->x = {x, x + ex, end, 0};
            if (ey) img->y = {y, y + ey, end, 0};
        }
    } else if (what == "mcrot") {
        // `mcrot px py pz` (mot.int's mcRot): a turn every |p| frames about x, y and z, the sign
        // the direction, added to the object's own turn; an image turns about its base point.
        Motion m = motion(Motion::Kind::Rotate, true, 1);
        m.a[0] = static_cast<float>(argNum(args, 5, 0));
        m.a[1] = static_cast<float>(argNum(args, 3, 0));
        m.a[2] = static_cast<float>(argNum(args, 4, 0));
        img->motions.push_back(m);
    } else if (what == "mcdisp") {
        Motion m = mcDisp(args, at);
        std::erase_if(img->motions, [](const Motion& o) { return o.kind == Motion::Kind::Disp; });
        img->motions.push_back(m);
    } else if (what == "mcscalecos" || what == "mcscalesin") {
        Motion m = motion(Motion::Kind::ScaleCos, true, static_cast<uint32_t>(argNum(args, 3, 60)));
        m.a[0] = percent(args.size() > 6 ? args[6] : "", 100);
        m.a[1] = percent(args.size() > 7 ? args[7] : "", 100);
        m.a[2] = percent(args.size() > 9 ? args[9] : "", 100);
        m.a[3] = percent(args.size() > 10 ? args[10] : "", 100);
        img->motions.push_back(m);
    } else if ((what == "quake" || what == "mcquake" || what == "spline" || what == "mcspline" || what == "m2spline" ||
                what == "m2quake") &&
               args.size() > 3 && args[3].find(',') != std::string::npos) {
        bool spline = what.find("spline") != std::string::npos;
        if (spline) std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
        img->motions.push_back(pathKeys(args, 3, spline, what.starts_with("mc"), static_cast<float>(img->x.value(at)),
                                        static_cast<float>(img->y.value(at))));
        img->motions.back().start = at;
    } else if (what == "shake" || what == "quake" || what == "mcshake" || what == "m2shake") {
        // `shake <frames> <count> <ampX> <ampY> ...`
        Motion m = motion(Motion::Kind::Shake, what == "mcshake", static_cast<uint32_t>(std::max(1, argNum(args, 3, 15))));
        m.a[2] = static_cast<float>(argNum(args, 4, 1));
        m.a[0] = static_cast<float>(argNum(args, 5, 0));
        m.a[1] = static_cast<float>(argNum(args, 6, 0));
        img->motions.push_back(m);
    } else if (what == "attr" || what == "attpos") {
        // `cg 0 attr <n> <xvar> <yvar> [<wvar> <hvar> <colourvar>]`: attribute n of the slot's image
        // (sscript 0x042a2a; Happiness 2 0x044a4c; planes and E-mote banks return at once): its x,
        // y, width, height and colour (native 724 -> cs2.exe 0x55a0c0; Nekopara's lookup 0x68c6e0).
        // An image without that attribute gives zeros: the lookup writes nothing and sscript
        // stores its own. Variables left out are `attr_def`'s (no scene relies on them).
        // `attpos <n> <xvar> <yvar>` (Happiness 2 0x049e98): the point on the slot's plane, the
        // slot's position plus the attribute minus the image's base point.
        int32_t n = argNum(args, 3, 1);
        int32_t ax = 0, ay = 0, aw = 0, ah = 0, ac = 0;
        if (img && !img->parts.empty()) {
            if (const ImageGeometry* g = geometry(img->parts.front())) {
                for (const auto& a : g->attributes) {
                    if (static_cast<int32_t>(a.id) == n) {
                        ax = a.x, ay = a.y;
                        aw = static_cast<int32_t>(a.width), ah = static_cast<int32_t>(a.height);
                        ac = static_cast<int32_t>(a.color);
                    }
                }
            }
        }
        if (what == "attpos") {
            int32_t bx = 0, by = 0, px = 0, py = 0;
            if (img) {
                px = img->x.value(frame_), py = img->y.value(frame_);
                if (img->baseX != INT32_MIN) {
                    bx = img->baseX, by = img->baseY;
                } else if (!img->parts.empty()) {
                    if (const ImageGeometry* g = geometry(img->parts.front())) bx = g->baseX, by = g->baseY;
                }
            }
            ax = px + ax - bx;
            ay = py + ay - by;
        }
        const int32_t vals[5] = {ax, ay, aw, ah, ac};
        for (size_t k = 0; k < (what == "attpos" ? 2u : 5u) && 4 + k < args.size(); ++k) {
            try {
                if (const int32_t f = num(args[4 + k]); f > 0) vars_.set(f, vals[k]);
            } catch (const Error&) {
            }
        }
    } else if (what == "stop") {
        stopMotions(*img, at);
        finishTween(img->scaleX, at);
        finishTween(img->scaleY, at);
    } else if (what == "delay") {
        img->delay = std::max(0, argNum(args, 3, 0));
    } else if (what == "mode") {
        // Colour filter: `mode mono|sepia|np` (sscript: 1, 2, 3; anything else 0), kept for the loads
        // to come; the later sscripts also apply it on the next frame (setImageMode, commit). Only the
        // 27 image slots have that step.
        const std::string m = args.size() > 3 ? toLowerAscii(args[3]) : "";
        const bool image = layer == Layer::Bg || layer == Layer::Cg || layer == Layer::Eg || layer == Layer::Fg ||
                           layer == Layer::Fw;
        setImageMode(*img, m == "mono" ? 1 : m == "sepia" ? 2 : m == "np" ? 3 : 0, modeEachFrame_ && image);
    } else if (what == "blur") {
        // `blur <x> <y>`: soften the image by that many pixels; `blur 0 0` (or none) sharp again.
        img->blurX = static_cast<int16_t>(std::clamp(argNum(args, 3, 0), 0, 255));
        img->blurY = static_cast<int16_t>(std::clamp(argNum(args, 4, img->blurX), 0, 255));
    } else if (what == "pri") {
        // `pri <depth>` (`@+n` relative): the image's place in the draw order; bare: the default.
        float cur = std::isnan(img->depth) ? defaultDepth(layer, index, layout_.mplFaceFrom) : img->depth;
        img->depth = args.size() > 3 ? static_cast<float>(argNum(args, 3, static_cast<int32_t>(cur), static_cast<int32_t>(cur)))
                                     : std::numeric_limits<float>::quiet_NaN();
    } else if (what == "tone") {
        // Kept in the slot's record for the next image loaded into it, through clears (0x0391a7).
        img->tone = toneArgs(args);
        setSlotTone(layer, index, img->tone);
    } else if (what == "pend" || what == "mcpend") {
        img->motions.push_back(pendulum(args, what == "mcpend", at));
    } else if (what == "mcscale") {
        // Image scales are percentages; missing from/to are 0 (sscript.kcs).
        Motion m = motion(Motion::Kind::ScaleSaw, true, 1);
        m.a[0] = percent(args.size() > 6 ? args[6] : "", 0);
        m.a[1] = percent(args.size() > 7 ? args[7] : "", 0);
        m.a[2] = percent(args.size() > 9 ? args[9] : "", 0);
        m.a[3] = percent(args.size() > 10 ? args[10] : "", 0);
        m.a[4] = static_cast<float>(std::max(1, argNum(args, 3, 1)));
        m.a[5] = static_cast<float>(std::max(1, argNum(args, 4, 1)));
        img->motions.push_back(m);
    } else {
        unhandled(std::string(toString(layer)) + " " + what);
    }
}

void ScenePlayer::setImage(Layer layer, int32_t index, const std::string& spec, const std::vector<std::string>& args,
                           size_t argStart) {
    StageImage* img = slot(layer, index, true);
    // Without coordinates the image goes where the object already is: the slot's default (vram.xml
    // image/<slot>/pos) after a clear, or where `pos` / a motion left it (sscript's load, Fruit
    // 0x0163c0 / Amakano 0x01bfb4, copies the object's position before reading the arguments; `@`
    // is relative to it). Only fw 0 is put back at its default.
    int32_t px = img->x.value(frame_), py = img->y.value(frame_);
    if (layer == Layer::Fw && index == 0) {
        auto def = layout_.imagePos.find({layer, index});
        px = def == layout_.imagePos.end() ? 0 : def->second.first;
        py = def == layout_.imagePos.end() ? 0 : def->second.second;
    }
    stopGroupOne(*img, frame_);  // native 313 with 1 (see stopGroupOne)
    img->spec = spec;
    img->parts.clear();
    img->solid = false;
    img->delay = 0;  // a newly loaded object starts without a delay
    // With `tonetbl` on, an image the tone table names sets that tone on the character slots and
    // the E-mote banks (sscript's load, Nekopara 0x01b3f9 -> natives 598 / 597: the name before its
    // first comma, in lower case; slots 6-11 and 67-76), for the images they load next.
    if (toneTableOn_) {
        loadToneTable();
        auto it = toneByImage_.find(toLowerAscii(spec.substr(0, spec.find(','))));
        if (it != toneByImage_.end()) {
            for (int32_t i = 0; i < 6; ++i) setSlotTone(Layer::Cg, i, it->second);
            for (int32_t i = 0; i < 10; ++i) setSlotTone(Layer::Mpl, i, it->second);
        }
    }
    img->tone = slotTone(layer, index);
    img->toneLoaded = img->tone;  // the slot's tone is applied to the image as it loads
    loadImageMode(*img);
    size_t a = argStart;
    if (spec.size() >= 2 && spec[0] == '$') {
        // `$AARRGGBB <w> <h> <x> <y>`: solid colour rectangle.
        img->solid = true;
        try {
            img->solidColor = static_cast<uint32_t>(std::stoul(spec.substr(1), nullptr, 16));
        } catch (...) {
        }
        // sscript's command reader turns `$` into `0x` (native 45: Yukikoi 0x015065, Fruit
        // 0x010b06), which the load takes as a colour (Fruit 0x016089), and reads the numbers after
        // it as for any image (Yukikoi 0x01c69e on, the 4th to 7th words): x y z plane, or x y plane
        // with three. The colour takes the first two as its size, as every script writes them
        // (`$ff000000 1280 720`, Yukikoi's bars `1280 100 0 <plane>`; the engine's side of that is
        // not read), and sits at 0,0 (scripts move it with `pos`).
        size_t numbers = 0;
        while (a + numbers < args.size() && isNumberLike(args[a + numbers])) ++numbers;
        img->solidWidth = argNum(args, a, layout_.planeBaseX * 2);
        img->solidHeight = argNum(args, a + 1, layout_.planeBaseY * 2);
        img->x = Tween::constant(0);
        img->y = Tween::constant(0);
        if (numbers >= 4) img->plane = argNum(args, a + 3, img->plane);
        else if (numbers == 3) img->plane = argNum(args, a + 2, img->plane);
        std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
        dirty_ = true;
        return;
    } else if (spec.size() > 2 && spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X')) {
        // `bg 0 0xAARRGGBB`: a colour in the image's place (Fruit's sscript 0x016089: the value is
        // the object's colour and the file name is cleared; the fill is the slot's vram.xml size, or
        // the screen where the slot gives none, as in every Grisaia game). Placed as an image would be.
        img->solid = true;
        try {
            img->solidColor = static_cast<uint32_t>(std::stoul(spec.substr(2), nullptr, 16));
        } catch (...) {
        }
        img->solidWidth = layout_.screenWidth;
        img->solidHeight = layout_.screenHeight;
    } else {
        img->parts = resolveSprite(spec);
        // sscript's image load (Nekopara 0x01b321 -> 0x095511): a name starting with one of the
        // etc/cgreg letters main.kcs read from adv.xml (case as written) goes to native 422.
        if (!spec.empty() && cgRegLetters_.find(spec[0]) != std::string::npos) registerCg(spec);
    }
    // sscript's load queues pPos only for coordinates it is given; otherwise the object keeps its
    // position and the motions running on it (fw 0 alone goes back to its default).
    if (layer == Layer::Fw && index == 0) img->x = Tween::constant(px), img->y = Tween::constant(py);
    loadPosition(layer, index, *img, args, a);
    img->loadX = img->x.value(frame_);
    img->loadY = img->y.value(frame_);
    // The numbers after the name (sscript's load, Fruit 0x016af1 / 0x016cff): `<x> <y> <plane>` with
    // three (z 0), `<x> <y> <z> <plane>` with four (Fruit's `cg 3 <name> x y 1 0` puts the character
    // on plane 0 although slot cg3 defaults to 3; Amakano's `bg 1 <name> -300 -200 6` on plane 6).
    // Without one the image keeps the object's plane: the slot's default after a clear.
    size_t numbers = 0;
    while (a + numbers < args.size() && isNumberLike(args[a + numbers])) ++numbers;
    if (numbers >= 4) img->plane = argNum(args, a + 3, img->plane);
    else if (numbers == 3) img->plane = argNum(args, a + 2, img->plane);
    // Options after the numbers (sscript's load reads every `\` word: 0x01cd75 on): the mask
    // channel. Each load starts without one.
    img->maskChannel = 0;
    img->turn = 0;
    for (size_t k = a + numbers; k < args.size(); ++k) {
        static const char* kMask[] = {"\\mr", "\\mg", "\\mb", "\\ma"};
        for (uint8_t c = 0; c < 4; ++c) {
            if (args[k] == kMask[c]) img->maskChannel = static_cast<uint8_t>(c + 1);
        }
        // The flips and turns (object +756, 0x01cdd7 on): bits OR together as sscript's do.
        static const std::pair<const char*, uint8_t> kTurn[] = {
            {"\\lr", 1}, {"\\ud", 2}, {"\\r90", 0x10}, {"\\r180", 0x20}, {"\\r270", 0x30}};
        for (const auto& [w, bit] : kTurn) {
            if (args[k] == w) img->turn |= bit;
        }
    }
    std::erase_if(img->motions, [](const Motion& m) { return m.kind == Motion::Kind::Move; });
    dirty_ = true;
}

void ScenePlayer::particleCommand(const std::vector<std::string>& args) {
    // sscript 0x07f418: [slot] then `stop`, `alpha [a]`, `fade [frames] [from] [to]`, `set ...`, or
    // <plugin> <texture> <count> <depth> <plane> [up to 32 floats]. Slots are 0-7. `particle`
    // (Happiness 2 0x0757b5) loads <script> <texture> <count> <depth> [floats] with no plane;
    // `particle2` (0x077c5e) <script> <texture> <count> <depth> <plane> [floats].
    const std::string& cmd = args[0];
    const bool script = cmd != "particle3";
    const bool hasPlane = cmd != "particle";
    size_t i = 1;
    int32_t slot = -1;
    if (i < args.size() && !args[i].empty() && std::isdigit(static_cast<unsigned char>(args[i][0]))) slot = std::atoi(args[i++].c_str());
    if (i >= args.size()) return;
    const std::string& sub = args[i];
    auto both = [&](auto fn) {
        for (Stage* st : {&pending_, &shown_}) fn(*st);
    };
    // A number, or an expression of variables (Nekopara's `#<a>+#<b>+#<c>`), as for other commands.
    auto value = [&](const std::string& a) -> float {
        char* end = nullptr;
        const float v = std::strtof(a.c_str(), &end);
        if (end && end != a.c_str() && *end == '\0') return v;
        return static_cast<float>(this->num(a));
    };
    auto num = [&](size_t k, float def) { return k < args.size() ? value(args[k]) : def; };
    if (sub == "stop") {
        // Without a slot: `particle` stops slots 0-3 (0x075980), `particle2` / `particle3` 0-7
        // (0x077e29 / 0x07ff6d), whatever each slot holds (one object table, m[3172 + 4 n]).
        both([&](Stage& st) {
            if (slot >= 0) st.particles.erase(slot);
            else if (cmd == "particle") std::erase_if(st.particles, [](const auto& e) { return e.first < 4; });
            else st.particles.clear();
        });
        return;
    }
    if (slot < 0 || slot > 7) {
        if (log) log(cmd + ": no slot");
        return;
    }
    if (sub == "alpha") {
        int32_t a = static_cast<int32_t>(num(i + 1, 255));
        both([&](Stage& st) {
            if (auto it = st.particles.find(slot); it != st.particles.end()) it->second.alpha = Tween::constant(a);
        });
        return;
    }
    if (sub == "fade") {
        // Linear from <from> to <to> (native 318), starting with the next update.
        uint32_t frames = static_cast<uint32_t>(std::max(0.0f, num(i + 1, 30)));
        int32_t from = static_cast<int32_t>(num(i + 2, 0)), to = static_cast<int32_t>(num(i + 3, 255));
        both([&](Stage& st) {
            if (auto it = st.particles.find(slot); it != st.particles.end()) it->second.alpha = {from, to, frame_ + 1, frames};
        });
        return;
    }
    if (sub == "set") {
        // Happiness 2's sscript 0x0801dd: the slot's 32 floats cleared, then filled from the
        // arguments as the load's are (0x080dfa), and sent to its layer if one is loaded (native
        // 275 -> message 0x145: the layer's floats, which each update gets).
        std::array<float, 32> f{};
        for (size_t k = i + 1, n = 0; k < args.size() && n < f.size(); ++k, ++n) f[n] = value(args[k]);
        both([&](Stage& st) {
            if (auto it = st.particles.find(slot); it != st.particles.end()) {
                it->second.f = f;
                if (it->second.effect) it->second.effect->setFloats(f);
            }
        });
        return;
    }
    // Any other word is a load (Happiness 2's sscript 0x080930): the slot's layer is freed first
    // (natives 273, 67), then the arguments are read, and one missing ends it with an error and the
    // slot empty. Happiness 2's `particle3 3 blend 0` is such a load (sscript compares only stop,
    // set, alpha and fade); each comes right after a `stop` of its slot.
    both([&](Stage& st) { st.particles.erase(slot); });
    if (i + (hasPlane ? 4 : 3) >= args.size()) {
        if (log) log(cmd + ": missing arguments");
        return;
    }
    ParticleLayer p;
    p.plugin = args[i];
    p.texture = args[i + 1];
    p.script = script;
    p.count = static_cast<int32_t>(value(args[i + 2]));
    p.depth = value(args[i + 3]);
    p.plane = hasPlane ? static_cast<int32_t>(value(args[i + 4])) : -1;
    for (size_t k = i + (hasPlane ? 5 : 4), n = 0; k < args.size() && n < p.f.size(); ++k, ++n) p.f[n] = value(args[k]);
    p.start = frame_;
    if (!(script ? ParticleEffect::knownScript(p.plugin) : ParticleEffect::known(p.plugin)) && log)
        log(cmd + (script ? ": script not reimplemented: " : ": plugin not reimplemented: ") + p.plugin);
    if (log && std::getenv("NYAN_PTCL_TRACE")) {
        std::string fs;
        for (float v : p.f) fs += " " + std::to_string(v);
        log(cmd + " " + std::to_string(slot) + " " + p.plugin + " " + p.texture + " n" + std::to_string(p.count) + " depth " +
            std::to_string(p.depth) + " plane " + std::to_string(p.plane) + " @" + std::to_string(frame_) + fs.substr(0, 120));
    }
    both([&](Stage& st) { st.particles[slot] = p; });
}

void ScenePlayer::startParticles(ParticleLayer& p, int32_t slot) {
    ParticleParams params;
    params.count = p.count;
    params.screenW = static_cast<float>(layout_.screenWidth);
    params.screenH = static_cast<float>(layout_.screenHeight);
    // The texture's size as the engine loads it: the image's canvas.
    if (auto ref = assets_.find(p.texture + ".hg3")) {
        try {
            HgImage img = HgImage::parse(assets_.read(p.texture + ".hg3"));
            if (!img.frames().empty()) {
                params.texW = static_cast<float>(img.frames()[0].totalWidth);
                params.texH = static_cast<float>(img.frames()[0].totalHeight);
            }
        } catch (const Error&) {
        }
    }
    params.f = p.f;
    params.planePos = particlePlanePos(p.plane);
    params.readFile = [this](const std::string& name) {
        if (!assets_.find(name)) return std::string();
        const Bytes b = assets_.read(name);
        return std::string(b.begin(), b.end());
    };
    // The engine seeds its generators from the clock; a fixed seed per slot and start keeps runs
    // repeatable.
    uint32_t seed = static_cast<uint32_t>(p.start * 2654435761u) ^ static_cast<uint32_t>(slot + 1);
    // The plugin's DLL stays loaded while another slot runs it, and its globals with it; this
    // slot's own old plugin was freed before the load (PluginGlobals).
    params.depth = p.depth;
    if (p.script) {
        p.effect = ParticleEffect::createScript(p.plugin, params, seed);
        return;
    }
    std::shared_ptr<PluginGlobals> shared;
    for (const Stage* st : {&pending_, &shown_}) {
        for (const auto& [s, q] : st->particles) {
            if (s != slot && q.effect && q.effect->globals() && toLowerAscii(q.plugin) == toLowerAscii(p.plugin))
                shared = q.effect->globals();
        }
    }
    p.effect = ParticleEffect::create(p.plugin, params, seed, shared);
}

std::array<float, 2> ScenePlayer::particlePlanePos(int32_t n) const {
    // The plane fields the engine fills before a plugin's Init and each update
    // (ParticleEffect::setPlanePos): the plane's position as it is now (sscript sends it each
    // frame, message 0x33), for a plane vram.xml defines; 0 for none (-1) or one it leaves out
    // (Happiness 2's planes 30 and 31).
    auto d = layout_.planes.find(n);
    if (n < 0 || d == layout_.planes.end()) return {0, 0};
    auto it = pending_.planes.find(n);
    if (it == pending_.planes.end()) return {static_cast<float>(d->second.posX), static_cast<float>(d->second.posY)};
    const Plane& pl = it->second;
    MotionState ms = evaluate(pl.motions, frame_);
    return {(ms.hasPos ? ms.x : static_cast<float>(pl.x.value(frame_))) + ms.dx,
            (ms.hasPos ? ms.y : static_cast<float>(pl.y.value(frame_))) + ms.dy};
}

void ScenePlayer::stepParticles() {
    // Each slot's plugin once per frame, one frame of the layer clock per update (the stage's copies
    // share it).
    std::set<ParticleEffect*> stepped;
    for (auto* stage : {&pending_, &shown_}) {
        for (auto& [slot, p] : stage->particles) {
            if (!p.effect) {
                // The other copy may already have one.
                Stage* other = stage == &pending_ ? &shown_ : &pending_;
                auto it = other->particles.find(slot);
                if (it != other->particles.end() && it->second.effect && it->second.start == p.start &&
                    it->second.plugin == p.plugin && it->second.script == p.script) {
                    p.effect = it->second.effect;
                } else {
                    startParticles(p, slot);
                }
            }
            if (p.effect && stepped.insert(p.effect.get()).second) {
                const auto at = particlePlanePos(p.plane);
                p.effect->setPlanePos(at[0], at[1]);
                if (particleEvery_ <= 1) p.effect->step(1);
                else if (frame_ % static_cast<uint64_t>(particleEvery_) == 0) p.effect->step(particleEvery_);
            }
        }
    }
}


std::vector<int32_t> ScenePlayer::touchBanks() const {
    // sscript 0x0c53e3 (the paw's MES_TOUCH_ENABLE) and 0x0da953 (the hit surfaces): objects 67-74
    // with a model (+680), a spec (+8, non-empty), shown (+4) and touchable (+936 == 0).
    std::vector<int32_t> out;
    for (const auto& [i, img] : shown_.layer(Layer::Mpl)) {
        if (i < 0 || i >= layout_.mplFaceFrom) continue;
        if (img.emote.empty() || !img.emotePlayer || img.spec.empty() || !img.visible || !img.touchable) continue;
        out.push_back(i);
    }
    return out;
}

const StageImage* ScenePlayer::bank(int32_t i) const {
    const auto& l = shown_.layer(Layer::Mpl);
    auto it = l.find(i);
    return it == l.end() ? nullptr : &it->second;
}

void ScenePlayer::setHitLayer(int32_t b, bool on) {
    const StageImage* img = bank(b);
    if (!img || !img->emotePlayer) return;
    auto r = emoteProfiles_.resolve(img->spec);
    if (!r) return;
    for (const auto& v : r->hitTest) img->emotePlayer->setVariable(v, on ? 0.0f : 1.0f);
}

std::vector<EmoteProfiles::Resolved::TouchRow> ScenePlayer::touchRows(int32_t b, uint32_t argb) const {
    const StageImage* img = bank(b);
    if (!img) return {};
    auto r = emoteProfiles_.resolve(img->spec);
    if (!r) return {};
    auto it = r->touch.find(argb);
    if (it == r->touch.end() || img->touchTable < 0 || static_cast<size_t>(img->touchTable) >= it->second.size()) return {};
    return it->second[static_cast<size_t>(img->touchTable)];
}

bool ScenePlayer::playAction(int32_t b, const std::string& name) {
    const StageImage* img = bank(b);
    if (!img || !img->emotePlayer) return false;
    auto& a = actions_[b];
    if (a.player.lock() != img->emotePlayer) a = ActionState{img->emotePlayer};
    // 0x591620: from the first step; each variable the action moves joins the restore list (once),
    // its value read at the next update.
    a.action = emoteProfiles_.action(name);
    a.next = 0;
    a.waiting = false;
    a.capture = true;
    if (!a.action) return false;
    for (const auto& v : a.action->vars) {
        bool known = false;
        for (const auto& s : a.restore) known = known || s.name == v;
        if (!known) a.restore.push_back({v});
    }
    if (log && std::getenv("NYAN_TOUCH_TRACE")) log("touch: bank " + std::to_string(b) + " plays " + name);
    return true;
}

void ScenePlayer::stopActions() {
    // 0x591820: past the step under way to the next `restore` step (or the end); the next update
    // takes it.
    for (auto& [b, a] : actions_) {
        if (a.current < 0 || !a.action) continue;
        const auto& steps = a.action->steps;
        while (true) {
            if (steps[static_cast<size_t>(a.current)].restore) break;
            ++a.next;
            if (a.next >= steps.size()) {
                a.current = -1;
                break;
            }
            a.current = static_cast<int32_t>(a.next);
        }
        a.waiting = false;
    }
}

bool ScenePlayer::actionsPlaying() const {
    for (const auto& [b, a] : actions_) {
        if (a.current >= 0 && !a.player.expired()) return true;
    }
    return false;
}

void ScenePlayer::resetActions(int32_t b) {
    if (b < 0) actions_.clear();
    else actions_.erase(b);
}

void ScenePlayer::stopLip(int32_t b) {
    for (Stage* st : {&shown_, &pending_}) {
        auto& l = st->layer(Layer::Mpl);
        if (auto it = l.find(b); it != l.end()) it->second.emoteVoice.clear();
    }
}

void ScenePlayer::ActionState::update(uint64_t now, EmotePlayer& p) {
    // 0x591940: nothing while the model is still moving (IsAnimating), so each step's moves finish
    // before the next starts.
    if (p.animating()) return;
    if (capture) {
        for (auto& s : restore) {
            if (!s.captured) s.value = p.variable(s.name), s.captured = true;
        }
        capture = false;
    }
    if (waiting) {
        if (now < until) return;
        waiting = false;
    }
    if (!action || next >= action->steps.size()) {
        current = -1;
        action = nullptr;
        return;
    }
    current = static_cast<int32_t>(next);
    const auto& step = action->steps[next++];
    if (step.wait != 0) {
        waiting = true;
        until = now + static_cast<uint64_t>(std::max(0, step.wait));
        return;
    }
    if (!step.restore) {
        for (const auto& [name, value] : step.vars) p.setVariable(name, value, step.frames, step.easing);
    } else {
        for (const auto& s : restore) p.setVariable(s.name, s.value, step.frames, step.easing);
    }
}

}  // namespace nyan::cs2
