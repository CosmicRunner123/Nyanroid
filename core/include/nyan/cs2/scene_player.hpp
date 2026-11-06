#pragma once

#include <deque>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/cst_scene.hpp"
#include "nyan/cs2/dictionary.hpp"
#include "nyan/cs2/emote.hpp"
#include "nyan/cs2/emote_player.hpp"
#include "nyan/cs2/emote_profile.hpp"
#include "nyan/cs2/expr.hpp"
#include "nyan/cs2/hg_image.hpp"
#include "nyan/cs2/stage.hpp"
#include "nyan/cs2/vars.hpp"

namespace nyan::cs2 {

class FesInstance;

// Sound/movie requests produced by scripts, drained by the platform layer each frame.
struct MediaCommand {
    enum class Kind : uint8_t {
        PlayBgm, StopBgm, BgmVolume,
        PlaySe, StopSe, SeVolume,
        PlayVoice, StopVoice,
        PlayMovie,
        VoiceVolume,
        BgmPan, SePan, VoicePan,  // `volume` holds the pan, -100 .. 100
        BgmPause, SePause, VoicePause,  // `loop`: held (true) or going on again (false)
    };
    Kind kind;
    int32_t channel = 0;   // PlayMovie: the skip mode (2 left click, 1 right click)
    std::string name;      // asset name without extension
    bool loop = false;
    uint32_t fadeFrames = 0;
    int32_t volume = 100;  // percent; PlayBgm / PlaySe / PlayVoice: the level the sound starts at
    int32_t fromVolume = -1;  // percent to jump to before fading (-1: current)
    uint32_t crossfadeFrames = 0;  // PlayBgm / PlaySe: fade from what the channel plays into this
    // PlayVoice: the other voice channels (bits) left playing when it starts, the ones asked for in
    // the same frame (ScenePlayer::soundPlay); every other voice stops.
    uint32_t spareVoices = 0;
};

// Geometry of an image asset needed for layout (first frame of the HG file).
struct ImageGeometry {
    int32_t width = 0, height = 0;
    int32_t offsetX = 0, offsetY = 0;
    int32_t baseX = 0, baseY = 0;
    std::vector<HgAttribute> attributes;
};

// Executes CatScene (.cst) scripts frame by frame (60 frames per second),
// maintaining the Stage the renderer draws. This is a high-level
// reimplementation of the commands CatSystem2's sscript.kcs provides.
class ScenePlayer {
public:
    // WaitKey: `wait key`, until a click (auto: its delay; skip: at once). WaitSe / WaitPcm /
    // WaitMpl: `wait se` / `wait pcm` / `wait mpl [n]`, until the channel's sound or the bank's
    // E-mote animation is over. Saved as their numbers.
    enum class State : uint8_t { Running, Input, WaitFrames, WaitAnimation, Choice, Movie, Fes, Ended, WaitKey,
                                 WaitSe, WaitPcm, WaitMpl, View };

    // Engine layout defaults from config/vram.xml.
    struct Layout {
        int32_t screenWidth = 1280, screenHeight = 720;  // the game's resolution (startup.xml)
        // The first E-mote face bank (`fmpl 0`): banks from it on draw at 60100, over the window
        // (sscript's depth: Nekopara 0x0c0d61, objects 75 on, bank 8; Amakano 0x0c559b, 77 on, bank 10).
        int32_t mplFaceFrom = 10;
        int32_t planeBaseX = 512, planeBaseY = 288;  // plane base point (pl00/base), surface coordinates
        int32_t planePosX = 512, planePosY = 288;    // where it lands on screen (pl00/pos)
        struct PlaneDefault {
            int32_t baseX, baseY, posX, posY;
            int32_t width = 0, height = 0;  // the plane's surface (plNN/width, height); 0: unbounded
            int32_t vramX = 0, vramY = 0;   // its window in the VRAM canvas (plNN/x, y)
            bool disp = true;               // plNN/disp: shown until a script says otherwise
        };
        std::map<int32_t, PlaneDefault> planes;  // plane/plNN, where given
        // Default plane for each layer slot (`image/cg01/pl`); missing entries mean plane 0.
        std::map<std::pair<Layer, int32_t>, int32_t> imagePlane;
        // `image/cg01/shift_x|y`: where the slot's image coordinate 0,0 sits on its plane's surface
        // (planes larger than the screen leave a margin for shakes and zooms).
        std::map<std::pair<Layer, int32_t>, std::pair<int32_t, int32_t>> imageShift;
        // `image/cg01/pos`: where an image loaded without coordinates is placed.
        std::map<std::pair<Layer, int32_t>, std::pair<int32_t, int32_t>> imagePos;
        // config.int/emodef.xml (Happiness 2): the emotion marks `emoinfo_getinfo` /
        // `emoinfo_getoffset` look up (cs2.exe 0x55d7a0 / 0x55d960).
        struct EmoMark {
            std::string name;
            int32_t x = 0, y = 0, scale = 100;  // pos, scale
            bool half = false;                  // tex/half: a half-size texture for small marks
            std::string large, small;           // tex/l, tex/s
        };
        struct EmoDef {
            int32_t texScale = 100;
            std::map<int32_t, int32_t> sizes;  // size/sNN: id -> scale
            std::vector<EmoMark> marks;        // emocon/emNN, in order
            // ch/cNN (id) / pNN (id) / table: emNN id -> offset x, y.
            std::map<std::pair<int32_t, int32_t>, std::map<int32_t, std::pair<int32_t, int32_t>>> offsets;
        };
        EmoDef emo;
    };
    const Layout& layout() const { return layout_; }

    ScenePlayer(const AssetIndex& assets, VarStore& vars);
    ~ScenePlayer();
    void setLayout(Layout layout) {
        const int32_t face = layout_.mplFaceFrom;
        layout_ = std::move(layout);
        layout_.mplFaceFrom = face;
    }
    void setMplFaceFrom(int32_t n) { layout_.mplFaceFrom = n; }
    // `wait tag <name>` with no frames given and no mark found: how long it waits (sscript's own
    // default: 120 in the 2.6.1.90 games' scripts, 0 in the older ones').
    void setWaitTagFrames(int32_t n) { waitTagFrames_ = n; }
    // The sound command of the sscripts before 2.6.1.72 (Fruit's): `end` keeps the channel's `vol`
    // level, and two or three numbers after a sound's name are its position, not a pan and a time.
    void setOlderSoundCommand(bool on) { olderSound_ = on; }
    // `mode` on a loaded image applies on the next frame (every sscript after Fruit's; Fruit's applies
    // it only at a load): read from the game's sscript (kcsNativeCallSites).
    void setModeEachFrame(bool on) { modeEachFrame_ = on; }
    // Development: particle plugins updated every n frames, n frames at a time (the DLLs' Update
    // takes the time since the last one), as the original updates them on a machine running at
    // 60 / n frames a second (the PC harness under software OpenGL runs at about 13).
    void setParticleEvery(int32_t n) { particleEvery_ = n; }
    // The message escape reader has `\t` (bold / italic): the 2.6.1.90 engines (parseMessage).
    void setTextStyles(bool on) { textStyles_ = on; }
    int32_t mplFaceFrom() const { return layout_.mplFaceFrom; }

    void start(const std::string& scene);
    // `cancellabel <scene>` (sscript keyword 123, 0x08266f): where the extras' viewer goes on a right
    // click (request 1 in boot mode -5; 0x0dbd0b). True if there was one to go to.
    bool cancelToLabel();
    // Stops the scene engine and clears the stage (`endkcs`).
    void stop();
    // Advances one frame.
    void tick();
    // While held (touch mode: sscript leaves its story mode for mode 27), the scene's commands,
    // waits and text reveal stand still; the stage keeps moving.
    void setHeld(bool on) { held_ = on; }

    // Touch mode's work on the E-mote banks (Nekopara; sscript 0x0da518 and kcTaskM2Plane messages
    // 0x224-0x22a). The character banks (0 .. mplFaceFrom - 1, sscript's objects 67-74) that can be
    // touched now: a model with a spec, shown, its `touch` switch on.
    std::vector<int32_t> touchBanks() const;
    const StageImage* bank(int32_t i) const;
    // The bank's hit layer on or off: its #hittest variables to 0 or 1, at once (0x575650).
    void setHitLayer(int32_t bank, bool on);
    // The rows of the bank's `touch` table for a hit-layer colour (0x587a70); empty if none.
    std::vector<EmoteProfiles::Resolved::TouchRow> touchRows(int32_t bank, uint32_t argb) const;
    // Starts action.def's action on the bank (0x591620); false if there is no such action.
    bool playAction(int32_t bank, const std::string& name);
    // Every bank's action jumps to its `restore` step (0x591820).
    void stopActions();
    // Some bank's action has a step under way (0x5758f0).
    bool actionsPlaying() const;
    // Forgets the values a bank's `restore` returns to (0x575860), or every bank's (-1).
    void resetActions(int32_t bank);
    // The bank's lip sync stops (touch play's native 928 with 0: its #lip_flow off).
    void stopLip(int32_t bank);
    // User click/tap: completes the text reveal, or continues past a message/wait.
    void advance();
    // Skipping (skip mode, the held key, Skip to Next Option): sscript's per-frame update
    // (Nekopara 0x0888aa) stops the group-0 motions of every slot each frame while the engine's
    // skip flag m[0x8000009c] is up (0x08a765: natives 310 / 313 with 0), unless a screen
    // transition is running (0x0b984d), so classic motions land on their end values at once and
    // mc* / m2* ones run on.
    void setSkipping(bool on) { skipping_ = on; }
    void choose(size_t index);
    // Called by the platform when a requested movie finishes (or is skipped).
    void movieFinished();

    State state() const { return state_; }
    const Stage& stage() const { return shown_; }
    uint64_t frame() const { return frame_; }
    const std::string& sceneName() const { return sceneName_; }
    // `sysbtn` switches by window button slot (see WindowScript::Availability::buttons).
    const std::array<bool, 17>& systemButtons() const { return sysButtons_; }
    size_t lineIndex() const { return pc_; }
    const std::string& sceneTitle() const { return title_; }
    // FE script started by the scene (`fesjs`, e.g. an eyecatch), drawn over the stage; may be null.
    FesInstance* overlay() const { return overlay_.get(); }
    // `fesds` display slots: FE scripts running beside the story (Amakano 1's date card, Happiness
    // 2's place card), drawn over the stage; empty where nothing runs.
    const std::array<std::unique_ptr<FesInstance>, 4>& displayScripts() const { return displayScripts_; }
    // The running screen came from `fex` (`fex sel mapselect`): sscript puts itself in that mode
    // (Fruit 0x05d947, Amakano 0x07d8c4: modes 20-22), which ends skipping and Skip to Next Option.
    bool fexScreen() const { return overlay_ && overlayFex_; }
    // A screen the scene ran opening or closing, for the story modes around it (GameSession):
    // `fes` / `fesjs` (sscript's state 14) turn skip and auto off as the screen opens; `fex`
    // (states 20 / 21) keeps them for its return, `fex sel` (20) giving auto back only with
    // `contauto` on.
    enum class ScreenKind : uint8_t { None, Fes, FexSel, Fex };
    ScreenKind takeScreenOpened() { return std::exchange(screenOpened_, ScreenKind::None); }
    ScreenKind takeScreenClosed() { return std::exchange(screenClosed_, ScreenKind::None); }
    // Skip to Next Option is running: `fesjs` does nothing then (sscript 0x089279: the main state
    // 2, or 25 for a return to a backlog line).
    void setJumping(bool on) { jumping_ = on; }
    // The engine services and config for the screens the scene runs (`fes`, `fesjs`, `fex`): they
    // are FE tasks like any other (sound queries, the held Force Skip key, getconf, text metrics).
    // Game gives them its host and config through this loader; without one they load bare.
    using FesLoader = std::function<std::unique_ptr<FesInstance>(const std::string& name, uint64_t frame)>;
    void setFesLoader(FesLoader loader) { fesLoader_ = std::move(loader); }

    std::vector<MediaCommand> takeMedia() { return std::exchange(media_, {}); }

    // Language of the story text (a prefix of the scenes' translation tables, e.g. "en"); empty
    // for the scenes' own text. Takes effect for the scene on screen as well.
    // The story's language and the sub language shown under it (empty: none).
    void setTextLanguage(const std::string& prefix, const std::string& sub = std::string());
    // The `effspd` setting (0-4): the engine's 2D-effect wipes (Task_Eff2D_Wipe*) run 1, 1.5, 2, 3 or 4
    // times as fast. Render transitions (`rdraw`, `rwipe`) don't use it.
    void setEffectSpeed(int32_t level) { effectSpeed_ = level; }
    // The `effcut` setting (Cyanotype's "Transition Effects": its English ON cuts them): every
    // screen transition command does nothing, so the screen changes at once.
    void setEffectCut(bool on) { effectCut_ = on; }
    // Drama mode (Cyanotype's `motcut`, its config's "Screen Effects", +24 of m[0x8000172c]):
    // while auto runs with it on, a line whose voice may play is shown without the message window
    // (sscript 0x0aa885 sets m[0x2ba54] at the voice, 0x08939f sends MES_SHOW 0 instead of the
    // window's usual MES_SHOW, `frameon` 0x0709bb returns at once); the advance clears it
    // (0x0cfe50), and auto stopping shows the window again (0x0110c7).
    void setDrama(bool on) { drama_ = on; if (!on) dramaVoice_ = false; }
    bool dramaVoice() const { return dramaVoice_; }
    std::function<bool(const std::string&)> voiceAllowed;  // the voice check (Game: voiceoff, config play)
    // `voiceoff <n>`: startup.xml's <VOICE> entry n plays no voice (see the command).
    bool voiceMuted(size_t entry) const { return entry < 50 && (voiceMuted_ >> entry) & 1u; }
    // adv.xml sysflag/kaisou: the first of the "part seen" variables `end_of_kaisou <n>` sets (900 in
    // most games, 700 in Cyanotype); the extras' replay lists read them.
    void setKaisouBase(int32_t var) { kaisouBase_ = var; }
    // sscript's replay flag (Yukikoi m[0x2f990], Nekopara m[0x2c610], Fruit m[0x22900]), set when it
    // boots in mode -2 or -4: `end_of_kaisou` then ends the story (back to the flow script).
    void setReplaying(bool on) { replaying_ = on; }
    // CG gallery (config.int/cglist.dat): groups of entries (image names as scripts write them,
    // `<name>,1,1`). A name is recorded by its key (cgKey): `cgreg <name>`, `cgflag <group>`
    // (every entry), and a shown image whose name starts with one of adv.xml's etc/cgreg letters
    // (setCgRegLetters). The records are kept with global data; see scene_player.cpp.
    struct CgGroup {
        std::string name;
        std::vector<std::string> images;
    };
    void setCgList(std::vector<CgGroup> groups);
    const std::vector<CgGroup>& cgList() const { return cgList_; }
    // A gallery list file (cglist.dat and the ones FE scripts name): int next chunk (0), int 0, int
    // groups; per group int its size in bytes, char name[64], int count, count x char image[64]
    // (Shift-JIS). The engine walks it at 0x642430 (amakanoPE.exe).
    static std::vector<CgGroup> parseCgList(const Bytes& data);
    std::set<std::string>& cgSeen() { return cgSeen_; }
    void setCgRegLetters(std::string letters) { cgRegLetters_ = std::move(letters); }
    // The record a name makes (cs2.exe 0x790750): its comma-separated tokens, the first and second
    // joined by "_", then the first letter of each further one ("e_yuu01MN,1,1" -> "e_yuu01MN_1,1").
    static std::string cgKey(const std::string& name);
    // The pictures an image name stands for: `Base,a,b,c` is Base_a, Base_0b, Base_00c, ...
    // (argument k zero-padded to k digits; "0" and empty arguments add none), a name without a
    // comma the picture itself; only the ones the archives hold. The scene's stand images and the
    // FE character tables (CHTABLE3) build such names.
    static std::vector<std::string> spriteParts(const AssetIndex& assets, const std::string& spec);
    // Whether a gallery entry has been recorded (0x7915b0: the same name and letters, exactly).
    bool cgEntrySeen(const std::string& entry) const { return cgSeen_.count(cgKey(entry)) != 0; }
    // The gallery group of that name (case-insensitive, cs2.exe 0x791b60), -1 if none.
    int32_t cgGroupIndex(const std::string& name) const;
    // The glossary the scenes unlock (adv.xml <dictionary>; null: the game has none) and the flag
    // `dic_check_update` sets (<apend_flag>).
    void setDictionary(Dictionary* d, int32_t apendFlag) { dictionary_ = d, dicApendFlag_ = apendFlag; }
    // Native 422 (`cgreg`, a shown image): the name's record, and with an "l" before the first "_"
    // (a large sprite, "Tkap01l_2") also the name without it. `cgflag` records a group's entries
    // without that.
    void registerCg(const std::string& name, bool alsoSmall = true);
    const std::string& textLanguage() const { return language_; }

    // The message's reveal (cs2.exe 0x694430 / 0x696cf0, grisaia.exe 0x5495a0 / 0x54c2b0): the
    // window string's `layout frame` (`messpd`, or the scene's `auto` speed), startup.xml's
    // strframe_frame, and whether the text shows at once (skipping).
    void setTextTiming(int32_t frame, int32_t strframeTenths, bool instant) {
        textFrame_ = frame, strframeTenths_ = strframeTenths, textInstant_ = instant;
    }
    // The message text's `mag` (its window script's `str mag`, 10 normally): glyphs show when the
    // elapsed time x mag / 10 reaches them (cs2.exe 0x696cf0, the text's +0x34).
    void setTextMag(int32_t mag) { textMag_ = mag; }
    // The text speed the scene's `auto on <base> <char> <speed>` set for the window (-1: none, the
    // config's messpd; sscript 0x0750c7 -> MES_SCROLLSPD, `auto off` puts messpd back).
    int32_t sceneTextSpeed() const { return autoSpeed_; }
    // The window reveals by a wipe (`layout scrolltype 1`): the renderer times it and calls
    // finishWipe once it has passed the end of the text.
    // The player's E-mote settings (m2_setconf; cs2.exe keeps them per character, the config screen
    // sets all alike): motion off stops all of the effects (0x4d4140, 0x4d46e0, 0x4d45d0); eff_dif
    // the crossfades (0x4dae30 via +0x60), eff_blink the blinking (0x4df3b0), eff_lip the lip sync.
    struct EmoteSettings {
        bool motion = true, dif = true, blink = true, lip = true;
        bool iner = true;                             // m2 eff_iner
        std::map<std::string, int32_t> inerLevel;     // m2 iner_lv <group>
        std::map<std::string, int32_t> inerEnable;    // m2 iner_enable <group>
    };
    void setEmoteSettings(const EmoteSettings& s) { emoteSettings_ = s; }
    // The config's contvoice: whether voices play on when a message is left (see advance()).
    void setContVoice(bool on) { contVoice_ = on; }
    // The config's enable_sceneanm (native 1050): whether images loaded from now on run their ANM
    // scripts.
    void setSceneAnimations(bool on) { sceneAnimations_ = on; }
    void setWipe(bool on) { wipe_ = on; }
    bool wipe() const { return wipe_; }
    void finishWipe() { shown_.window.revealed = shown_.window.text.length(); }
    // The whole message at once (its window script's `str skip`), glyphs or wipe.
    void revealAll() { shown_.window.revealed = shown_.window.revealTotal(); }
    // The scene's own auto mode (`auto on [base] [char]`, sscript m[0x20b70]) and the pause it asks
    // for in place of the config's autospd_b / autospd_c.
    bool sceneAuto() const { return autoMode_; }
    int32_t sceneAutoBase() const { return autoBase_; }
    int32_t sceneAutoChar() const { return autoChar_; }
    // The current input wait: the frame it began and the characters shown since the last click
    // (sscript m[0x20a18], counted from each `[message]` line's text with native 47).
    uint64_t inputFrame() const { return inputFrame_; }
    size_t inputChars() const { return inputChars_; }

    // Diagnostics: command keywords with no implementation, and assets that were missing.
    const std::map<std::string, size_t>& unhandledCommands() const { return unhandled_; }
    const std::map<std::string, size_t>& missingAssets() const { return missing_; }
    std::function<void(const std::string&)> log;

    const ImageGeometry* geometry(const std::string& asset);

    // Backlog: every message shown, oldest first (the message log screen reads it).
    struct LogEntry {
        std::string name;   // speaker (UTF-8), empty for narration
        std::string text;   // message (UTF-8)
        std::string voice;  // voice file played with it, if any
        std::string speaker;  // the name line as the scene writes it (WindowState::speaker)
        std::string sub;      // the message in the sub language (UTF-8), empty if none
        // The log screen's way back here (`pathinfo`, then `exit -2`): cs2.exe keeps a path record
        // per entry and replays the scene to it; this keeps the story at the message, encoded as
        // a save (a few KB). `id` is what `pathinfo` hands the script; 0 before it has one.
        uint32_t id = 0;
        std::shared_ptr<const Bytes> point;
    };
    const std::vector<LogEntry>& backlog() const { return log_; }
    // True while the last backlog entry is the message on screen (the log screen leaves it out).
    bool lastEntryOnScreen() const { return logCurrent_ && !log_.empty(); }
    // Voice of the message on screen ("" if none): the window's voice-replay button. sscript's flag
    // (Nekopara m[0x2870c]) goes up as the `pcm` command starts a voice (0x055954) and down at the
    // click (0xc605c), so a message still in its waits (`\@`) has its voice before it is logged.
    std::string currentVoice() const { return lastEntryOnScreen() ? log_.back().voice : pendingVoice_; }
    void clearLog() { log_.clear(); }
    // Back to the backlog entry `id` (cs2.exe native 994: the path history is cut there and the
    // scene replayed to it): its message is on screen again and later entries leave the log.
    // Flags in [keepFrom, keepTo) keep their values, as on a load. False if there is no such point.
    bool returnToLog(uint32_t id, int32_t keepFrom, int32_t keepTo);
    void setLogLimit(size_t n) { logLimit_ = n; }
    // The message waiting for input: its scene and line (read-text tracking). Empty when not waiting.
    std::string messageKey() const;
    // Waiting at a novel page break (a `Page` line) rather than an input line: sscript tells the
    // window MES_WAITPAGE then, MES_WAITINPUT otherwise (Nekopara 0x0867fe: state -9 / -6).
    bool waitingAtPage() const;
    // Whether the message waited on now gives way to the next one (not a `\@` continuation nor a
    // novel page's next message): the window then clears as that one starts.
    uint64_t waitKeyFrame() const { return waitKeyFrame_; }  // when the current `wait key` began
    // The input lines (input and page breaks) before the one waited at: sscript's input index
    // (its scene struct m[0x28208] +0x24, Nekopara). -1 when not waiting.
    int32_t inputIndex() const;
    // Waiting at an input with the message fully shown: where sscript sets its page flag
    // (m[0x28850], Nekopara 0x086339).
    bool inputReady() const { return state_ == State::Input && shown_.window.revealed >= shown_.window.revealTotal(); }
    // `autosave` scene command since the last call.
    bool takeAutosaveRequest() { return std::exchange(autosaveRequested_, false); }
    // `end_of_kaisou` since the last call: the global data is written at once.
    bool takeGlobalSaveRequest() { return std::exchange(globalSaveRequested_, false); }
    // `jumpstop` (a new scene's start): Skip to Next Option stops here.
    bool takeJumpStop() { return std::exchange(jumpStop_, false); }
    // `skipstop`: skip mode stops here (GameSession).
    bool takeSkipStop() { return std::exchange(skipStop_, false); }
    // `callmestag <label>`: labels of the message window's script to run (see Game::tick).
    std::vector<std::string> takeWindowCalls() { return std::exchange(windowCalls_, {}); }
    // `rs <n> <params...>`: commands for resident script n (see Game::runResidentCommands), the
    // parameters as the line wrote them (UTF-8 when the line came from a translation).
    struct ResidentCall {
        int32_t slot = 0;
        std::vector<std::string> params;
        bool utf8 = false;
    };
    std::vector<ResidentCall> takeResidentCalls() { return std::exchange(residentCalls_, {}); }
    // How many times a save was loaded / the story went back to a backlog entry (the resident
    // scripts' RS_LOAD / RS_JUMP follow them).
    uint32_t loadCount() const { return loadCount_; }
    uint32_t jumpCount() const { return jumpCount_; }
    // Every restore (a load, a return to a backlog line, Previous Option): the window is redrawn.
    uint32_t restoreCount() const { return restoreCount_; }
    // The text of scene step `step` (the lines between two waits for input, counted from 0 at the
    // scene's start): its message lines joined, as request 0x803 answers (Happiness 2 sscript
    // 0x0126fe -> 0x015e17). `utf8`: the lines came from a translation.
    std::string stepText(int32_t step, bool* utf8 = nullptr) const;
    // What the message window hears from the scene, in the order sscript sends it (Nekopara
    // 0xc605c; Amakano 2, Cyanotype and Yukikoi the same): the click that ends a wait for input
    // hides the click mark (MES_WAITINPUT 0) and, when its message gives way (state -9: an input
    // line outside novel mode, a page break), clears the window (0x001e89: $str1000-1002 emptied,
    // MES_CLEAR); then the lines after it run, each name line sending MES_SETNAME as it runs
    // (0x00135c, whoever speaks); the message's display step (MES_SHOW / MES_DRAW) comes after.
    struct WindowEvent {
        enum class Kind { Click, Name, Draw } kind = Kind::Click;
        bool clears = false;  // Click: the message gives way (MES_CLEAR); Draw: `mesdraw` on
        std::string speaker;  // Name: the name line as the scene writes it (the name table's key)
        std::string name;     // Name: the name as shown
    };
    std::vector<WindowEvent> takeWindowEvents() { return std::exchange(windowEvents_, {}); }

    // A point the story can be resumed from: a message waiting for input or a choice on screen.
    // Used for quick save/load and "return to last option".
    struct Resume {
        std::string scene;
        size_t pc = 0;
        State state = State::Input;
        Stage pending, shown;
        std::string title;
        std::map<int32_t, std::string> bgm;  // playing BGM per channel
        std::map<int32_t, std::string> se;   // looping sound effects per channel (ambience)
        std::map<int32_t, int32_t> bgmVolume, seVolume;  // their volumes (percent) where set
        // Each channel's `vol` level (kind 0 bgm, 1 se, 2 pcm), which its later sounds start at.
        std::map<std::pair<int, int32_t>, int32_t> channelVolume;
        VarStore vars;
        // `drawdef` and `wipedef`: frames and plane each (sscript keeps them in its save record,
        // restored at 0x0b7d93-0x0b7dea).
        std::array<int32_t, 4> transitionDefaults{20, 0, 20, 0};
        // `autoface` (sscript keeps it in its save record, 0x0b1698 / 0x0b71c4).
        bool autoFace = false;
        // `voiceoff`: the voice entries muted (bit n: startup.xml's <VOICE> entry n; REBELLIONS's
        // sscript saves its table, 0x098220, and loads it, 0x09cf1b).
        uint64_t voiceMuted = 0;
    };
    // Captures the current point; empty unless waiting for input or a choice.
    std::optional<Resume> capture() const;
    // The choice the player last made, captured while it was on screen.
    const std::optional<Resume>& lastChoice() const { return lastChoice_; }
    // Restores a captured point. Flags in [keepFrom, keepTo) keep their current values (globals).
    void restore(const Resume& r, int32_t keepFrom, int32_t keepTo);
    // Loads a saved point as a fresh start (no previous choice to return to).
    void load(const Resume& r, int32_t keepFrom, int32_t keepTo);

private:
    void run();
    void execute(const CstLine& line);
    void command(const std::string& text);
    void commit();
    void jump(const std::string& scene);
    std::string cancelLabel_;
    void block(State s);
    CstScene loadScene(const AssetRef& ref);

    // command groups
    void imageCommand(Layer layer, const std::vector<std::string>& args);
    void emoteCommand(const std::vector<std::string>& args);
    std::shared_ptr<const EmoteModel> emoteModel(const std::string& name);
    void startEmote(StageImage& img, int32_t bank);
    // A bank's model loaded again (a restored save): its spec's and body digits' variables are
    // re-sent every frame (StageImage::emotePinned).
    void pinLoaded(StageImage& img);
    // `m2body` digits on a bank (Nekopara's sscript 0x0536bd keeps them, 0x0c27df applies them through
    // native 917; cs2.exe 0x57dbc0 -> 0x5735a0): each digit of the body profile whose value changes
    // sets its #pattern's variables, moving there along the digit's #chain when `animate` (a shown
    // bank); `all` sets every digit (a new model). The values go into emoteVars as well.
    static float emoteVarSpeed(const StageImage& img, const std::string& name);
    static float emoteVarScale(const StageImage& img, const std::string& name);
    void applyBody(StageImage& img, const std::string& digits, bool animate, bool all);
    static std::vector<EmotePlayer::HostBlink> hostBlinks(const EmoteProfiles::Resolved& r);
    void stepEmotes();
    // Every image part's ANM script, once a frame (made when the image first needs them), with the
    // lip-synced images' mouth variable set from their voice first.
    void stepAnimations();
    // sndanm.xml without MPL (sscript 0x099953 when a voice starts): the voice's channel entry
    // links the first two images whose name starts with the entry's image (bg, cg, eg, fg, fw in
    // slot order, sscript's objects 0-26) to it.
    void linkLips(int32_t channel, const std::string& voice);
    std::shared_ptr<const AnmScript> anmScript(const std::string& part);
    void particleCommand(const std::vector<std::string>& args);
    void startParticles(ParticleLayer& p, int32_t slot);
    void stepParticles();
    std::array<float, 2> particlePlanePos(int32_t plane) const;
    void stepSoundPositions();
    static std::string lipKey(const std::string& voice, const std::array<float, 6>& weights, int32_t smooth);
    const std::vector<float>& lipTable(const std::string& voice, const std::array<float, 6>& weights, int32_t smooth);
    // A voice with no lip data: its levels per 50 ms (see voiceLevels) drive the expression's
    // #lip_flow (cs2.exe 0x574640 starts and stops it, 0x583f40 steps it). The value to set, or
    // nothing when the flow does not set the variable this frame.
    std::optional<float> lipFlowValue(const StageImage& img, const EmotePlayer* player);
    const std::vector<float>& voiceLevels(const std::string& voice);
    void setImage(Layer layer, int32_t index, const std::string& spec, const std::vector<std::string>& args, size_t argStart);
    static bool isChoiceOption(const CstLine& line);  // "<n> <scene> <prompt>" after `fselect`
    void soundCommand(const std::string& kind, const std::vector<std::string>& args);
    void soundPlay(int kind, int32_t ch, bool given, std::string name, const std::vector<std::string>& args, size_t next);
    void soundSub(int kind, int32_t ch, const std::string& op, const std::vector<std::string>& args, size_t at);
    void placeSound(int kind, int32_t ch, const std::vector<std::string>& args, size_t from, bool position);
    void planeCommand(const std::vector<std::string>& args);
    void cameraCommand(const std::vector<std::string>& args);

    std::vector<std::string> resolveSprite(const std::string& spec);
    int32_t num(const std::string& expr, int32_t current = 0);
    int32_t argNum(const std::vector<std::string>& args, size_t i, int32_t fallback, int32_t current = 0);
    // `<obj> mcfade <period> <from> <to> [hold]` (args from index 3): the mcFade motion.
    Motion mcFade(const std::vector<std::string>& args, uint64_t at, int32_t current);
    Motion mcDisp(const std::vector<std::string>& args, uint64_t at);
    // `pend`/`m2pend <frames> <half swings> <x> <y> <z deg>` and `mcpend <frames> <x> <y> <z deg>`.
    Motion pendulum(const std::vector<std::string>& args, bool loop, uint64_t at);
    // `tone <r%> <g%> <b%>` / `tone <name>` (config.int/bgtone.tbl) / bare `tone` (100%).
    std::array<int16_t, 3> toneArgs(const std::vector<std::string>& args);
    // config.int/bgtone.tbl (cs2.exe 0x78d740): `#name(r,g,b)` tones, each followed by the image
    // names that take it (lower case).
    void loadToneTable();
    std::map<std::string, std::array<int16_t, 3>> toneTable_;
    std::map<std::string, std::array<int16_t, 3>> toneByImage_;
    bool toneTableLoaded_ = false;
    // `tonetbl` (sscript m[0x5da74], on from the boot): loading an image the table names sets its
    // tone on the character slots and the E-mote banks (the load, Nekopara 0x01b3f9).
    bool toneTableOn_ = true;
    std::array<int16_t, 3> slotTone(Layer layer, int32_t index) const;
    void setSlotTone(Layer layer, int32_t index, const std::array<int16_t, 3>& tone);
    Motion pathKeys(const std::vector<std::string>& args, size_t from, bool spline, bool loop, float curX, float curY);
    void unhandled(const std::string& what);
    void missing(const std::string& asset);
    bool animationsDone() const;
    bool windowDrawing() const;     // MES_ISDRAW: the window's fade or text still coming
    void setSkipFlag(bool on, const char* why);  // NYAN_SKIP_TRACE logs it
    void finishWindowDrawing();     // MES_DRAW / MES_SKIP
    static void hurry(Stage& stage);

    Plane& plane(int32_t index);
    // `pl N`'s reset, keeping what the plane's group-0 engine motions still drive (planeCommand).
    void resetPlane(int32_t index);
    void markImageGroups(Layer layer, int32_t index, const std::string& word, bool m2);
    void finishClassicMotions(Stage& st);
    void noteValueMotion(Plane& p, const std::string& what, bool m2, uint64_t at, const std::vector<std::string>& args);
    StageImage* slot(Layer layer, int32_t index, bool create);

    // sscript queues `pos` and `scale` as motions (pPos, pScale through native 535) that the object
    // update runs next frame, while the relative motions (move, amoveN, mscale, ascaleN) measure from
    // the values the last update left (native 302, the motion list's work values). What such an
    // assignment replaced in this frame, per object and field (0 x, 1 y, 2 scale x, 3 scale y), is
    // what a relative motion after it in the same frame starts from. Objects: images
    // `layer * 1000 + slot`, planes `-1 - plane`.
    std::map<std::pair<int32_t, int32_t>, float> priors_;
    uint64_t priorsFrame_ = UINT64_MAX;
    void notePrior(int32_t object, int32_t field, float replaced);
    void loadPosition(Layer layer, int32_t index, StageImage& img, const std::vector<std::string>& args, size_t at);
    float prior(int32_t object, int32_t field, float current) const;
    // `@` in a load's or `pos`'s coordinates: the object's settled position (StageImage::settledX),
    // rounded; `drawn`, where it is now, when that is not known.
    static int32_t lastUpdatePos(float settled, float drawn);
    void traceAt(const char* what, int32_t object, int axis, int32_t settled, float drawn);
    // At a frame's first line: each image and plane with nothing moving takes its position, motions
    // included, as its settled one (sscript's update writes the record back only then).
    void settlePositions();
    uint64_t settledFrame_ = UINT64_MAX;
    // A relative position motion on an object (sscript 0x0250be move, 0x02beb0 amove1): from the
    // values the last update left, the target's change is added along the curve; started while
    // another relative motion of the object runs, the two add up (AddPos).
    void relativeMove(int32_t object, Tween& x, Tween& y, std::vector<Motion>& motions,
                      const std::vector<std::string>& args, uint64_t at, uint8_t ease);
    // A scale argument as sscript reads it: native 46 turns `n%` into n x `unit` / 100, truncated
    // (cs2.exe 0x655060), native 213 evaluates that with `@` as `at` (0x643520, which drops a
    // trailing `%` left over), and the handler divides by `size`. 1 is 100%.
    float scaleArg(const std::string& tok, int32_t unit, float size, int32_t at);

    const AssetIndex& assets_;
    VarStore& vars_;
    Layout layout_;
    ExprEvaluator expr_;

    std::string sceneName_;
    std::optional<CstScene> scene_;
    // `call`: the scenes to return to (scene, name, line after the call).
    struct CallFrame {
        CstScene scene;
        std::string name;
        size_t pc = 0;
    };
    std::vector<CallFrame> callStack_;
    std::array<bool, 17> sysButtons_{true, true, true, true, true, true, true, true, true,
                                     true, true, true, true, true, true, true, true};
    std::string lastVoice_;  // the voice most recently started, and when
    uint64_t lastVoiceFrame_ = 0;
    // `wait tag`: the voice each channel last started, and when; each voice's length in frames.
    std::map<int32_t, std::pair<std::string, uint64_t>> voiceChannels_;
    std::map<std::string, uint64_t> voiceLengths_;
    uint64_t voiceLength(const std::string& voice);
    int32_t waitTagFrames_ = 120;
    std::string language_, subLanguage_;
    EmoteProfiles emoteProfiles_;
    std::map<std::string, std::weak_ptr<const EmoteModel>> emoteModels_;
    std::deque<std::shared_ptr<const EmoteModel>> recentModels_;
    std::map<std::string, std::vector<float>> lipTables_;  // voice + settings -> mouth value per frame
    std::set<std::string> lipLevels_;  // lipTables_ keys read from `.lv` levels
    std::map<std::string, std::vector<float>> voiceLevels_;  // voice -> level per 50 ms (no lip data)
    // Images whose ANM mouth follows a voice (cs2.exe's kcTaskImageList records, messages 0x13d /
    // 0x13f): by layer and slot, the voice channel and the level that opens the mouth.
    struct LipLink {
        int32_t channel = 0;
        int32_t limit = 1000;
    };
    std::map<std::pair<Layer, int32_t>, LipLink> lipLinks_;
    std::map<std::string, std::shared_ptr<const AnmScript>> anmScripts_;  // part -> script (null: none)
    bool sceneAnimations_ = true;
    std::map<std::string, std::shared_ptr<const EmoteProfiles::Resolved>> lipSpecs_;  // spec -> its lip rows
    struct LipFlow {  // a bank's flow (cs2.exe's lip entry, node +0x28 on)
        std::string voice;
        uint64_t voiceStart = 0;
        bool active = false, stop = false, ended = false;
        size_t step = 0;
        float cur = 0, next = 0, lastLevel = -1;
        int32_t frames = 1, check = 0, nextFrames = 0, nextCheck = 0;
        uint64_t stepStart = 0;
        bool stepStarted = false;
    };
    std::map<const EmotePlayer*, LipFlow> lipFlows_;
    // A bank's action player (cs2.exe's placement +0x180, 0x5914f0): stepped before the model each
    // frame (0x574070 -> 0x591940).
    struct ActionState {
        std::weak_ptr<EmotePlayer> player;
        const EmoteProfiles::Action* action = nullptr;
        size_t next = 0;        // the step to take next
        int32_t current = -1;   // the step under way (-1: none)
        bool waiting = false, capture = false;
        uint64_t until = 0;
        struct Saved {
            std::string name;
            float value = 0;
            bool captured = false;
        };
        std::vector<Saved> restore;  // what `restore` sends each variable back to
        void update(uint64_t now, EmotePlayer& p);
    };
    std::map<int32_t, ActionState> actions_;
    bool held_ = false;
    std::string expandStrings(const std::string& text) const;
    std::string speakerName(const std::string& raw);
    std::string stringTests(const std::string& cond) const;
    bool lineUtf8_ = false;  // the command being run comes from a translated line
    size_t pc_ = 0;
    State state_ = State::Ended;
    uint64_t frame_ = 0;
    uint64_t waitUntil_ = 0;
    uint64_t waitKeyFrame_ = 0;  // `wait key` start (its auto deadline counts from here)
    bool inputClears_ = true;
    int32_t novelBreaks_ = 0;
    bool novelLineHasText_ = false;  // the novel message's current line has text (for `\r`)  // a novel message's leading `\r`s not yet placed (see execute)
    int32_t waitChannel_ = -1;   // `wait se / pcm / mpl <n>`: the channel or bank (-1 any)
    uint64_t waitSince_ = 0;     // and when the wait began
    // Sound effect (1) and voice (2) channels: the frame each one's sound plays until (a loop for
    // ever), for `wait se` / `wait pcm` (sscript asks the sound task, native 160).
    std::map<std::pair<int, int32_t>, uint64_t> soundUntil_;
    bool soundPlaying(int kind, int32_t channel) const;
    bool mplAnimating(int32_t bank) const;
    void skipEmotes();

    Stage pending_;  // edited by the script
    Stage shown_;    // what is on screen (copied from pending_ on commit)
    bool dirty_ = false;

    // Transition for the next commit (`rdraw`, `wipe`).
    Transition nextTransition_;
    int32_t effectSpeed_ = 0;
    bool effectCut_ = false;
    bool drama_ = false, dramaVoice_ = false;
    // `view 0 <style>`: sscript's window change (main state 17), see the command.
    std::string viewStyle_;
    uint64_t viewSince_ = 0;
    int viewStep_ = 0;  // 1 the fade out, 2 the switch made, 3 the fade in
    void stepView();
    void applyViewStyle(const std::string& style);
    bool renderTransitionUp() const;
    uint64_t voiceMuted_ = 0;
    std::array<int32_t, 512> calcPrivate_{};  // the calculator's own `\n` variables (engineCalc)
    int32_t textMag_ = 10;
    int32_t kaisouBase_ = 900;
    bool replaying_ = false;
    // sscript m[0x2c628] (Fruit m[0x22914]): the channel a `bgm` / `se` / `pcm` without one plays on.
    int32_t autoChannel_ = 0;
    bool olderSound_ = false;
    bool modeEachFrame_ = true;
    int32_t particleEvery_ = 1;
    bool textStyles_ = false;
    // The sound records' `vol` levels (+164 while +208 is set), keyed (kind, channel): a new sound on
    // the channel starts at it; `end` (newer sscripts) and starting play go back to 100, a load
    // brings back the saved ones.
    std::map<std::pair<int, int32_t>, int32_t> channelVolume_;
    // `keyskip` (Nekopara's sscript 0x06e894; every game's the same, Fruit 0x0511e4): 1 on (the
    // default), 0 `off`, 2 `ctrl`; each `keyskip` also lowers the skip flag.
    int32_t keySkip_ = 1;
    // The click's skip flag (Nekopara m[0x8000009c]; REBELLIONS' and Fruit's m[0x80000094],
    // Yukikoi's m[0x800000a0]): a click raises it while key skip is on and the window is up or
    // put off by `frameoff` (Nekopara 0x0dbefc; Fruit 0x0a0347, Yukikoi 0x0ed5c0 the same). While
    // it is up the classic motions end at once every frame, transitions end, the waits go
    // through and the `rdraw` family does nothing; a message with text lowers it (0x012af6), so
    // one click skips the animation up to the next line. See advance() and tick().
    bool skipFlag_ = false;
    bool frameoffHidden_ = false;  // m[0x2866c]: the window put off by `frameoff` (frameon, a message: 0)
    bool skipping_ = false;
    std::vector<CgGroup> cgList_;
    std::string cgRegLetters_;  // adv.xml etc/cgreg/r00...: first letters of names a shown image registers
    Dictionary* dictionary_ = nullptr;
    int32_t dicApendFlag_ = -1;
    std::set<std::string> cgSeen_;            // lower-case image names seen  // `effspd` level (setEffectSpeed)
    int32_t drawDefFrames_ = 20, drawDefPlane_ = 0;  // `drawdef` (sscript's defaults: 20 frames, plane 0)
    int32_t wipeDefFrames_ = 20, wipeDefPlane_ = 0;  // `wipedef` (the same, 0x0088e7)
    std::string pendingName_;
    std::string pendingSpeaker_;  // the name line as the scene writes it (WindowState::speaker)
    bool haveName_ = false;
    bool collectingChoices_ = false;
    bool autoMode_ = false;
    bool forceMesDel_ = false;  // `forcemesdel`
    bool autoFace_ = false;     // `autoface`
    int32_t autoBase_ = 15, autoChar_ = 5;  // sscript 0x0574ef: the `auto` command's defaults
    size_t pageChars_ = 0, inputChars_ = 0;
    uint64_t inputFrame_ = 0;
    int32_t textFrame_ = 4, strframeTenths_ = 5;
    bool textInstant_ = false;
    int32_t autoSpeed_ = -1;
    void updateReveal();
    EmoteSettings emoteSettings_;
    bool contVoice_ = true;
    bool wipe_ = false;
    std::string title_;

    std::vector<MediaCommand> media_;
    std::map<int32_t, std::string> bgm_;
    std::map<int32_t, std::string> se_;  // looping SE per channel
    std::map<int32_t, int32_t> bgmVolume_, seVolume_;
    std::vector<LogEntry> log_;
    size_t logLimit_ = 1000;
    uint32_t nextLogId_ = 1;
    bool logCurrent_ = false;
    bool faceSet_ = false;  // an `fw` command since the last message
    bool messageOpen_ = false;
    size_t overlayLine_ = SIZE_MAX;  // the `fex` / `fesjs` line whose screen is running
    bool overlayFex_ = false;        // that line is a `fex`
    ScreenKind overlayKind_ = ScreenKind::None, screenOpened_ = ScreenKind::None, screenClosed_ = ScreenKind::None;
    bool jumping_ = false;
    int32_t fexVar_ = 0;  // `fex sel`'s variable for the screen's exit code
    FesLoader fesLoader_;
    bool overlayChoice_ = false;     // a `fex sel` screen: a choice for Previous Option  // a `[message]` line since the last input: the next one adds to it
    // `se <ch> pos` / `move` (sscript 0x039b6c / 0x03b44f): where a channel's sound sits, which
    // the engine turns into a stereo pan each frame (0x07bdfd -> 0x4a2e20). Keyed by (bgm?0:1, ch).
    struct SoundPosition {
        float from[3] = {0, 0, 0}, to[3] = {0, 0, 0};
        uint64_t start = 0;
        uint32_t frames = 0;
        int32_t pan = 0;
    };
    std::map<std::pair<int, int32_t>, SoundPosition> soundPos_;  // kind 0 bgm, 1 se, 2 pcm
    std::string pendingVoice_;  // voice played since the last logged message
    size_t inputPc_ = 0;        // the input line the player is waiting at
    bool autosaveRequested_ = false;
    bool globalSaveRequested_ = false;
    bool jumpStop_ = false;
    bool skipStop_ = false;
    std::vector<std::string> windowCalls_;
    std::vector<ResidentCall> residentCalls_;
    uint32_t loadCount_ = 0, jumpCount_ = 0, restoreCount_ = 0;
    std::vector<WindowEvent> windowEvents_;
    void logMessage();
    std::optional<Resume> choiceShown_, lastChoice_;
    bool startPointPending_ = false;  // an `undo`: the next message becomes the Previous Option point
    std::unique_ptr<FesInstance> overlay_;
    std::array<std::unique_ptr<FesInstance>, 4> displayScripts_;  // sscript's records at 0x5bda8 (Amakano 1)
    void stepDisplayScripts();
    std::map<std::string, size_t> unhandled_;
    std::map<std::string, size_t> missing_;
    std::unordered_map<std::string, std::vector<std::string>> spriteCache_;
    std::unordered_map<std::string, std::optional<ImageGeometry>> geometryCache_;
};

const char* toString(ScenePlayer::State s);

}  // namespace nyan::cs2
