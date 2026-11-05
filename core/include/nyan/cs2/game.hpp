#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "nyan/audio/mixer.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/cursor_image.hpp"
#include "nyan/cs2/key_map.hpp"
#include "nyan/cs2/save_data.hpp"
#include "nyan/cs2/scene_player.hpp"
#include "nyan/cs2/vars.hpp"
#include "nyan/cs2/window_script.hpp"
#include "nyan/cs2/window_theme.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/media/mpeg_video.hpp"

namespace nyan::cs2 {

// Settings read from config/startup.xml.
struct GameConfig {
    std::string title;
    int width = 1024;   // virtual screen size
    int height = 576;
    uint32_t windowColor = 0xCC000C10;  // message window ARGB (GAME/wndcolor)
    // CONFIG_B/wnd_alpha (default 1): GAME/wndcolor's alpha is 0-255 and becomes the config's
    // percentage (x 100 / 255); with 0 it is taken as the percentage itself.
    bool windowAlphaByte = true;
    // FONT/shadow/around and rightdown: the text's edge all round and one pixel right-down
    // (cs2.exe 0x71d060 starts both at 1, 0x71ec40 reads them; drawn by kcFontImage).
    bool textAround = true, textRightDown = true;
};

// One loaded CatSystem2 game: assets, variables, scene player, and routing of
// script sound requests to a mixer. Platform front ends own one of these.
class Game : public FesInstance::Host {
public:
    // `mixer` may be null (headless).
    Game(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer);

    const GameConfig& config() const { return config_; }
    const WindowTheme& windowTheme() const { return theme_; }
    WindowScript& windowScript() { return *window_; }
    FesInstance::Config& config_store() { return engineConfig_; }
    const KeyMap& keyMap() const { return keyMap_; }
    const AssetIndex& assets() const { return assets_; }
    const GameFolder& folder() const { return *folder_; }
    ScenePlayer& player() { return player_; }
    VarStore& vars() { return vars_; }

    // Where this game's saves and global data live (empty: memory only). Call before boot().
    void setSaveDirectory(const std::string& dir);
    // Writes global data (flags, settings) if it changed. Also done automatically at checkpoints.
    void saveGlobal();
    SaveStore& saves() { return saves_; }
    // Thumbnail for the next save capture (RGBA, top row first), from the renderer.
    void setThumbnail(int32_t w, int32_t h, Bytes rgba);
    int32_t thumbnailWidth() const { return thumbW_; }
    int32_t thumbnailHeight() const { return thumbH_; }

    // Boots the game: its flow script (logo, title, ...) if it has one, else straight into the story.
    void boot();
    // Starts the story from its first scene (no title screen).
    void startNewGame();
    // Development: plays one scene directly, without the flow script.
    void playScene(const std::string& scene);
    // The game's top-level FE script (flow.fes), or null.
    FesInstance* flow() { return flow_.get(); }
    // The resident scripts (adv.xml regident) while the story runs; null slots are unused.
    const std::vector<std::unique_ptr<FesInstance>>& residents() const { return residents_; }
    // True while the scene engine runs (the story, as opposed to title/menu screens).
    bool sceneRunning() const { return sceneRunning_; }
    // True once the game asked to quit (its flow script ended, or an in-game menu's Game End).
    bool quitRequested() const { return quitRequested_ || (flow_ && flow_->exited()); }
    // The mouse cursor the engine shows now (startup.xml <CURSOR>, cursor.xml, `xchgcursor`), or
    // null for Windows' own arrow. A returned picture stays valid for the game's lifetime.
    const CursorImage* cursor();

    // FesInstance::Host
    void execKcs(const std::string& name) override;
    void endKcs(const std::string& name) override;
    int32_t kcsExitCode(const std::string& name) override;
    void retTitle() override;
    void changeCursor(const std::string& name) override;
    void baseImage(std::vector<FesQuad> quads) override { baseImage_ = std::move(quads); }
    // What the FE scripts' screen keeps under them (`baseimg`), shown while no story runs.
    const std::vector<FesQuad>& baseImage() const { return baseImage_; }
    SaveStore* saveStore() override { return &saves_; }
    int32_t saveObject() override { return pendingSave_ ? 1 : 0; }
    bool dataSave(int32_t slot, int32_t object) override;
    ~Game() override;
    bool request(int64_t code, int32_t a, int32_t b) override;
    bool request(FesInstance& from, int64_t code, int32_t a, int32_t b) override;
    std::optional<int32_t> exVar(const std::string& name) override;
    void configSet(const std::string& key) override;
    uint64_t engineFileVersion() const override { return engineVersion_; }
    bool cgEntrySeen(const std::string& entry) override;
    Dictionary* dictionary() override { return dictionary_.loaded() ? &dictionary_ : nullptr; }
    bool bgmPlaying(int32_t channel) override;
    bool soundPlaying(int kind, int32_t channel) override;
    bool forceSkipHeld() override { return forceSkip_; }
    std::optional<int32_t> soundConf(const std::string& field, const std::string& name,
                                     const std::vector<int32_t>& ids) override;
    bool setSoundConf(const std::string& field, const std::string& name, const std::vector<int32_t>& ids,
                      int32_t value, bool notify) override;
    std::optional<int32_t> pickSystemVoice(const std::vector<std::vector<int32_t>>& paths) override;
    bool resetSoundConf(const std::string& name, const std::vector<int32_t>& ids, bool notify) override;
    std::optional<float> textWipeLength(const FesQuad& q) override {
        return textMeasure_ ? textMeasure_(q) : std::nullopt;
    }
    std::vector<std::string> fontList(int32_t charset) override { return static_cast<const Game*>(this)->fontList(charset); }
    void addFont(const std::string& file) override { addFontFile(file); }
    std::string defaultFont() override { return defaultFontFace(); }
    std::string configFont() override { return static_cast<const Game*>(this)->configFont(); }
    void setConfigFont(const std::string& face) override;
    std::optional<int32_t> textLineCount(const FesQuad& q) override {
        return textLines_ ? textLines_(q) : std::nullopt;
    }
    std::optional<std::pair<float, float>> textDrawArea(const FesQuad& q) override {
        return textArea_ ? textArea_(q) : std::nullopt;
    }
    // A text speed step adds strframe_frame / 10 frames per glyph (as the message window's speed).
    float framesPerGlyph(int32_t speed) override { return static_cast<float>(speed) * static_cast<float>(strFrameTenths_) / 10.0f; }
    // The backlog as the log screen shows it: past messages only (not the one on screen).
    const std::vector<ScenePlayer::LogEntry>* backlog() override;
    // Speaker id from the game's name table (config nametable.csv) for a name line as the scene
    // writes it (WindowState::speaker): -1 for no name or no table, 999 for a name it doesn't list.
    int32_t nameId(const std::string& speaker) const;
    // Sends the window's speaker to its script (MES_SETNAME).
    void sendSpeaker();
    void sendSpeaker(const std::string& speaker, const std::string& name);
    // Replays the voice of the message on screen (the window's voice button).
    void replayVoice();
    // A recollection replay (sscript boot mode -2 / -4): the window offers no saving.
    bool replaying() const { return replaying_ && sceneRunning_; }
    // The extras' viewer (boot -5) cancelled by a right click: to its `cancellabel` scene, if set.
    bool cancelViewer() { return viewing_ && sceneRunning_ && player_.cancelToLabel(); }

    // Touch mode (Nekopara's おさわり; sscript mode 27, 0x0da518): the paw button beside the window
    // (touch.fes) enters it; a click on a character plays one of its reactions; a right click or
    // the paw leaves. The story waits meanwhile.
    bool touchMode() const { return touch_.state != TouchState::Off; }
    // Draws one bank alone, as the platform's renderer does, into an ARGB picture of the virtual
    // screen (rows top-down): the hit surfaces (sscript 0x0daa9d, natives 112 / 116). Without it
    // nothing can be touched.
    using BankCapture = std::function<std::vector<uint32_t>(const StageImage&)>;
    void setBankCapture(BankCapture f) { bankCapture_ = std::move(f); }
    // The renderer's measure of a STRING's text (the FE screens' `isdraw` under a wipe).
    using TextMeasure = std::function<std::optional<float>(const FesQuad&)>;
    using TextLines = std::function<std::optional<int32_t>(const FesQuad&)>;
    void setTextLines(TextLines f) { textLines_ = std::move(f); }
    using TextArea = std::function<std::optional<std::pair<float, float>>(const FesQuad&)>;
    void setTextArea(TextArea f) { textArea_ = std::move(f); }
    void setTextMeasure(TextMeasure f) { textMeasure_ = std::move(f); }
    bool currentHasVoice() const { return !player_.currentVoice().empty(); }

    // Reading modes as the front end runs them (getexvar SKIP / AUTO / SKIP_FORCE).
    void setModes(bool skip, bool autoMode, bool forceSkip);
    // adv.xml etc/automag (read by main.kcs: Yukikoi 0x0055d0 -> m[0x80005990]; 0 when missing).
    bool autoMagEnabled() const { return autoMag_; }
    // The Auto request (window request 14, or the key) while auto runs, with automag on (Yukikoi's
    // sscript 0x0ed7c5), and the speed keys (requests 24 / 25, 0x0ee3a1 / 0x0ee433): the auto level
    // (m[0x84444]) doubles up to 32 or halves down to 1, and the wait is timed again.
    void stepAutoLevel(bool up);
    // The same request without automag stops auto with MES_MODE 0 alone (0x0ed82f).
    void autoStoppedByRequest() { autoQuietStop_ = true; }
    // Auto given back by a `fex` screen's return (Yukikoi's sscript 0x0dff18 / 0x0e04b2): at the
    // level it had, with MES_SETMAG 10 (level 1) or 20 when automag is on. The next setModes that
    // turns auto on takes it.
    void resumeAuto(int32_t level) { autoResumeLevel_ = level; }
    int32_t autoLevel() const { return autoLevel_; }
    // The message waiting for input was read in an earlier play (read-text tracking).
    bool messageRead() const {
        std::string key = player_.messageKey();
        return !key.empty() && isRead(key);
    }
    // Autosave due (a choice came up, or the scene's `autosave` command); the front end adds a
    // thumbnail and calls autosave().
    bool takeAutosaveRequest() { return std::exchange(autosaveRequested_, false); }
    void autosave();
    // The message log screen of the current window style (its theme's <log> script).
    bool openLog();
    // Extra window button N (request 100 + N): the script meswnd.xml names for it.
    bool openExtraButton(int32_t n);
    // The game's own choice screen (window theme <fselect>) is up for the current choice.
    bool choiceScreenOpen() const { return choiceScreen_; }
    // The choice the player made on it, once (-1: none yet).
    int32_t takeChoice() {
        int32_t c = chosen_;
        chosen_ = -1;
        return c;
    }
    bool logOpen() const { return systemScreen_ && logScreen_; }

    // Engine screens opened over the story (the options screen): input and drawing go here first.
    FesInstance* systemScreen() { return systemScreen_.get(); }
    void openSystemScreen(const std::string& name);
    // The save or load screen (adv.xml sysflag/sltype: 0 save, 1 load).
    void openSaveScreen(bool load);
    // Story services requested by the message window and key bindings.
    bool quickSave();
    bool quickLoad();
    bool hasQuickSave();
    bool returnToLastChoice();
    // Loads (a slot, a quick save, the Previous Option point) go through sscript's black fade:
    // 16 frames out, the load, 16 frames back in (0x093ec8 states 7-9 and 3-4). While it runs
    // the story takes no input; screenFade() is the black overlay's opacity, 0..1.
    bool loading() const { return fadeStep_ != FadeStep::None; }
    float screenFade() const;
    // The in-game menus through sscript's black fade (every sscript alike: config state 19,
    // save / load state 7; Cyanotype 0x0df292 / 0x0d4288, Amakano 1 0x0ceaf3 / 0x0c4444). With
    // adv.xml etc/erase/config (or /save) on, which main.kcs reads with 1 as the default, the
    // window hides, the screen fades to black over 16 frames, the screen opens and the black goes;
    // after it closes, black at once, back to the story over 30 frames. The story is not drawn
    // while the screen is open (menuHidesStory). The window comes back as the fade back starts
    // when etc/meswnd_restore/config (or /save) is on (1 by default), else after it. With erase
    // off the screen opens at once. Config is the window's request 23, save / load 19 / 20.
    void openMenu(const std::string& name);
    void openSaveMenu(bool load);
    bool menuFading() const { return menuStep_ == MenuStep::Out || menuStep_ == MenuStep::In; }
    // The window is hidden from the menu's request until it comes back (MES_SHOW 0 ... 1).
    bool menuHidesWindow() const { return menuStep_ != MenuStep::None && !menuWindowBack_; }
    // While an in-game menu's screen is open the story underneath is not drawn: sscript sends
    // its own task message 0x1d with 1 (native 694, Amakano 1 0x0cf1b7 / 0x0c4bed) just before it
    // opens the screen and with 0 when the screen exits (0x0cee17 / 0x0c479d). The screen's task is
    // a child too, made after. On PC the black stays from the fade until the screen's own fade-in.
    bool menuHidesStory() const { return menuStep_ == MenuStep::Open && systemScreen_ != nullptr; }
    float menuFade() const;
    bool hasLastChoice() const { return player_.lastChoice().has_value(); }

    // Reading settings from the engine config (the options screens write them), interpreted
    // with startup.xml's timing tables (GAME/strframe_frame, GAME/autopriset).
    int32_t conf(const std::string& key, int32_t fallback) const;
    // The engine config entries named `<prefix><group>` (`m2 iner_lv bust`), by group.
    std::map<std::string, int32_t> confGroup(const std::string& prefix) const;
    // The message window's text speed (its string's `layout frame`): the scene's `auto` speed, else
    // messpd (0 shows text at once; startup.xml strframe_count steps).
    int32_t messageFrame() const;
    int32_t strFrameTenths() const { return strFrameTenths_; }
    uint32_t autoWaitFrames(size_t chars) const;   // auto mode pause for a message (`autospd_b/_c`)
    bool autoDue() const;                          // auto mode may go on to the next message
    uint32_t skipInterval() const;                 // frames between skipped messages (`skip_speed`)
    bool voicePlaying() const;
    // Advances one frame (1/60 s) and forwards sound requests to the mixer.
    void tick();
    // The window hears what the story's lines told it since the last time (see game.cpp).
    void syncWindowEvents();

    // A full-screen `movie` playing over everything (the story waits for it), or null.
    struct Movie {
        std::unique_ptr<media::MpegVideo> mpeg;      // MPEG-1 (pl_mpeg): Grisaia, LSG, ...
        std::unique_ptr<media::FfmpegVideo> ffmpeg;  // the rest (Nekopara's WMV)
        uint64_t frames = 0;   // simulation frames since it started
        int32_t skipMode = 2;  // 2: a left click ends it, 1: a right click
        bool changed = true;   // the picture changed since it was last drawn
        bool open() const { return mpeg || ffmpeg; }
        int width() const { return mpeg ? mpeg->width() : ffmpeg ? ffmpeg->width() : 0; }
        int height() const { return mpeg ? mpeg->height() : ffmpeg ? ffmpeg->height() : 0; }
        const std::vector<uint8_t>& rgba() const { return mpeg ? mpeg->rgba() : ffmpeg->rgba(); }
        bool seek(double s) { return mpeg ? mpeg->seek(s) : ffmpeg->seek(s); }
        bool ended() const { return mpeg ? mpeg->ended() : ffmpeg->ended(); }
    };
    Movie* movie() { return movie_.open() ? &movie_ : nullptr; }
    // Ends the movie now (a click it allows, or the story being left).
    void endMovie();
    bool startMovie(const std::string& name, int32_t skipMode, int32_t volume);

    // Languages the game ships (startup.xml <language>, in its order; empty if it has none). The
    // engine config holds the chosen ones as indices: `lang_ui` (system images, %LANG%) and
    // `lang_text_main` (story text and %LANG_M%), which the game's options screen sets.
    struct Language {
        std::string prefix;  // asset suffix and translation-table column ("en")
        std::string name;    // as the game lists it ("English")
    };
    const std::vector<Language>& languages() const { return languages_; }
    // Chooses both languages (front ends; before boot for the first screens to follow).
    void setLanguage(int32_t index);

    // Font data for message text: the first .ttf/.otf shipped in the game folder, if any.
    // The message font: the face startup.xml's <FONT><face> names, found by its internal name among
    // the fonts the game ships (game folder, font/ folder, font archive). If none matches (it is a
    // Windows system font), what the font archive/folder provides (default.ttf, else the first).
    // `face` receives the face index for font collections. `weight` is the LOGFONT weight asked
    // for (400, or 700 with the config's `fontweight`): of a family's faces the nearest is taken, as
    // Windows' font mapper does (meiryob.ttc's Meiryo Bold at 700). `fakeBold` receives whether
    // bold was asked for and the face is not a bold one (Windows then emboldens it itself).
    Bytes loadFont(int* face = nullptr, const std::string& wanted = "", int32_t weight = 400, bool* fakeBold = nullptr) const;
    // The weight text is drawn at: 700 with the config's `fontweight` on (its default startup.xml
    // FONT/bold, 0 when absent), else 400; see game.cpp.
    int32_t textWeight() const { return conf("fontweight", 0) != 0 ? 700 : 400; }
    // The text edge: `fshadow 0` all around the glyph, `fshadow 1` the copy one pixel right and
    // down (startup.xml FONT/shadow/around and rightdown, 1 when absent); see game.cpp.
    bool textAround() const { return conf("fshadow 0", config_.textAround) != 0; }
    bool textRightDown() const { return conf("fshadow 1", config_.textRightDown) != 0; }
    // The engine's font list (`fontcom`): the font families Nyanroid can draw with that charset's
    // code page (0x80 Shift-JIS); see game.cpp. `fontcom add` adds a file of the game's.
    std::vector<std::string> fontList(int32_t charset) const;
    void addFontFile(const std::string& file);
    // The startup face (`fontcom default`) and the config's `font`, which strings start with.
    std::string defaultFontFace() const;
    const std::string& configFont() const;
    // Characters drawn from an image instead of the font ("user font"): the image (hist.xml
    // IMAGE/ufont) and code point -> frame id. CP932 0x8740.. (①..⑨) are frames 1..9.
    const std::string& userFontImage() const { return userFontImage_; }
    const std::map<uint32_t, int32_t>& userFontGlyphs() const { return userFontGlyphs_; }
    // The engine executable's FileVersion (e.g. 0x0002000600010050 = 2.6.1.80), 0 if unknown.
    uint64_t engineVersion() const { return engineVersion_; }
    // Extra folders searched for the configured face (e.g. Windows fonts the user copied over).
    void setFontDirectories(std::vector<std::string> dirs) { fontDirs_ = std::move(dirs); }
    // The faces startup.xml names (FONT/face) when Nyanroid can find none of them among the game's
    // fonts and those folders and the game ships no default.ttf, so the message font would be a
    // stand-in; empty otherwise. Reads only the archives' indexes, startup.xml and the font files,
    // so a front end can ask before it starts the game.
    static std::vector<std::string> missingFonts(const GameFolder& folder, const std::vector<std::string>& fontDirs);

private:
    void dispatchMedia();
    void loadConfig();
    void loadLayout();
    // soundconf.xml (newer engines): MASTER and the PART tree. Each part's settings default to the
    // file's values, a child's missing ones to its parent's (cs2.exe 0x6aff..0x6b00c2); the values
    // the player changed are engineConfig_ entries "snd.<field> <path>".
    struct SoundPart {
        std::string tag, type;
        std::string head;  // the file-name prefix that picks this part, lower-cased (see filePath)
        int32_t id = -1;
        // volume, play, sysplay, voicebgm (voicevol), mastervol, skipvol, mute, voicemute; a value
        // the XML leaves out is the parent's as loaded (each part keeps its own setting after).
        std::map<std::string, int32_t> def;
        std::vector<SoundPart> parts;
    };
    // The part a sound file belongs to and its parents, top first (each with its settings key):
    // cs2.exe 0x71ae60 / 0x71af30. Empty: no part.
    using PartPath = std::vector<std::pair<const SoundPart*, std::string>>;
    const PartPath& filePath(const std::string& name) const;
    mutable std::map<std::string, PartPath> filePaths_;
    // sscript's level for a sound (natives 964 / 995, cs2.exe 0x71b4e0 / 0x71b790): over the part
    // and each parent, volume (or voicevol) x mastervol, 0 if muted.
    float partLevel(const PartPath& path, bool voicevol) const;
    // The scene's sounds with soundconf.xml (sscript 0x0a35e9 each frame): their part, whether they
    // duck while a voice plays and their skipvol (-1: none), and the gain last sent.
    struct PlayingSound {
        PartPath path;
        bool duck = false;
        int32_t skipvol = -1;
        float gain = -1;
    };
    std::map<std::pair<int, int>, PlayingSound> playingSounds_;  // (bus, channel)
    void startSound(int bus, int channel, const std::string& name);
    void updateSoundGains();
    void loadSoundConf();
    const SoundPart* soundPart(const std::string& name, const std::vector<int32_t>& ids, std::string& key) const;
    int32_t soundValue(const SoundPart& part, const std::string& key, const std::string& field) const;
    bool soundConf_ = false;
    bool extraScreen_ = false;  // the open system screen is a window extra button's
    bool configScreen_ = false; // ... or the engine's `config` (system menu) screen
    bool saveScreen_ = false;   // ... or its save/load screen (exits with the slot to load)
    SoundPart soundMaster_;
    std::vector<SoundPart> soundParts_;

    std::unique_ptr<GameFolder> folder_;
    AssetIndex assets_;
    VarStore vars_;
    ScenePlayer player_;
    audio::Mixer* mixer_;
    Movie movie_;
    GameConfig config_;
    WindowTheme theme_;
    FesInstance::Config engineConfig_;
    std::unique_ptr<WindowScript> window_;
    std::unique_ptr<FesInstance> flow_;
    // adv.xml regident/reg00-03 (main.kcs 0x005b6c, Amakano 2: system memory 0x80005ff4, 44 bytes
    // each): the script, the flag that gets the count of an `rs` command's parameters and the first
    // string variable for them.
    struct ResidentDef {
        std::string script;
        int32_t prmcnt = 0, prmstr = 0;
    };
    std::vector<ResidentDef> residentDefs_;
    std::vector<std::unique_ptr<FesInstance>> residents_;
    void loadEmoDef(ScenePlayer::Layout::EmoDef& emo);
    void startResidents();
    void runResidentCommands();
    void residentLabel(const std::string& label, const std::vector<int32_t>& args = {});
    // What the resident scripts last heard: the skip state (RS_SKIP), the player's load and jump
    // counts (RS_LOAD / RS_JUMP), whether a system screen was open (RS_REDRAW when it closes).
    int32_t residentSkip_ = 0;
    uint32_t residentLoads_ = 0, residentJumps_ = 0;
    uint32_t windowRestores_ = 0;  // player_.restoreCount() the window was last redrawn for
    bool residentScreen_ = false;
    bool sceneRunning_ = false;
    int32_t kcsExit_ = -1;
    int32_t bootFlag_ = 512;     // adv.xml sysflag/boot: how sscript starts (-1 new game, >=0 load slot)
    int32_t bootString_ = 200;   // adv.xml sysstr/boot: first scene name
    int32_t movieVolFlag_ = 514;     // adv.xml sysflag/movie_vol: a screen script's movie volume
    int32_t movieCancelFlag_ = 515;  // adv.xml sysflag/movie_cancel: a click may end it
    int32_t movieStr_ = 500;         // adv.xml sysstr/movie: its file
    bool kcsMovie_ = false;          // movie.kcs is playing (execkcs movie)
    // The open system screen is a kcs program's (call_config.kcs's config.fes): it stays open with
    // no story running, and its exit code is the program's.
    bool kcsScreen_ = false;
    bool replaying_ = false;         // started as a recollection (boot -2 / -4)
    bool viewing_ = false;           // started as the extras' viewer (boot -5: m[0x2c614])
    int32_t chvolFlag_ = 670;    // adv.xml sysflag/chvol: first per-character voice volume flag
    int32_t slModeFlag_ = 801;
    int32_t slTypeFlag_ = 800;
    // adv.xml sysflag/logret_pathindex: where the log screen's `exit -2` goes back to (-1: none).
    int32_t logRetFlag_ = -1;
    // adv.xml sysflag/savenew_quick: the last quick save's place in the quick range (-1: none).
    int32_t saveNewQuickFlag_ = -1;
    int32_t retTitleFlag_ = 513; // adv.xml sysflag/rettitle: set when the game returns to its title
    // startup.xml <CURSOR>: type (0 a picture the engine draws, 1 cursor.xml's cursor files), mode
    // (the config's cursor_mode: 1 the game's cursor, else Windows' arrow), default / hx / hy (type
    // 0's picture and hotspot); cursor.xml's names and files; the name `xchgcursor` chose.
    int32_t cursorType_ = 0, cursorMode_ = 1, cursorHx_ = 0, cursorHy_ = 0;
    std::string cursorPicture_, cursorName_ = "default";
    std::map<std::string, std::string> cursorFiles_;
    std::map<std::string, std::optional<CursorImage>> cursorCache_;
    int32_t savePageNowFlag_ = 802, savePageMaxFlag_ = 803;  // adv.xml sysflag/savepage_now, _max
    bool restartFlow_ = false;   // `rettitle`: flow.fes starts over from #START (it reads the flags)
    bool quitRequested_ = false;
    void startFlow();   // adv.xml sysflag/sltype: 0 save screen, 1 load screen   // adv.xml sysflag/slmode: 0 from the title, 1 in game
    int32_t globalFrom_ = 512, globalTo_ = 1024;  // startup.xml GAME/flag/global0: kept across loads
    std::unique_ptr<FesInstance> systemScreen_;
    // Touch mode's states (m[0x28bc4] in mode 27): 0 setup, 1 idle, 4 play, 5 playing, 2 stop,
    // 3 stopping.
    enum class TouchState { Off, Setup, Idle, Play, Playing, Stop, Stopping };
    struct Touch {
        TouchState state = TouchState::Off;
        int32_t x = 0, y = 0;                         // the click (m[0x61b84], m[0x61b88])
        std::map<int32_t, std::vector<uint32_t>> hit;  // bank -> its hit surface (m[400220 + 4 k])
        bool voice = false;                           // a reaction's voice was played (m[0x61b8c])
    } touch_;
    bool touchAttached_ = false;  // touch.fes said it is there (0x783)
    bool touchEnabled_ = false;   // the paw is up (MES_TOUCH_ENABLE 1)
    BankCapture bankCapture_;
    TextMeasure textMeasure_;
    TextLines textLines_;
    TextArea textArea_;
    void stepTouch();
    uint32_t touchPixel(int32_t bank, int32_t x, int32_t y) const;
    KeyMap keyMap_;
    int32_t autoStart_ = -1, autoCount_ = 0;  // adv.xml save/autosave
    bool autosaveRequested_ = false, choiceSeen_ = false;
    bool logScreen_ = false;  // the open system screen is the message log
    bool choiceScreen_ = false;     // the open system screen is the choice screen
    bool choiceScreenFailed_ = false;  // no usable choice screen for this choice: the generic one shows
    int32_t chosen_ = -1;
    int32_t fsstrBase_ = 700;  // adv.xml fsstr: first string variable of the choice texts
    bool openChoiceScreen();
    std::vector<ScenePlayer::LogEntry> backlogView_;
    // FileVersion of the game's engine executable (major.minor.build.revision, 16 bits each), 0 if unknown.
    uint64_t engineVersion_ = 0;
    std::string userFontImage_;
    std::map<uint32_t, int32_t> userFontGlyphs_;
    // The first engine generation whose sscript logs a message as it is shown (2.6.1.72); Fruit's
    // (2.6.1.70) logs it when the player moves on. See Game::backlog.
    static constexpr uint64_t kEngineLogsCurrentLine = 0x0002000600010048;  // 2.6.1.72
    // The sscripts of 2.6.1.90 and later send MES_CLEAR when `mesdraw` turns the window off (Amakano 2
    // 0x07d278, Cyanotype 0x07dba5, Yukikoi 0x07d16c, Happiness 2 0x07dba5); Nekopara's (2.6.1.76),
    // LSG's and Hatsumira's (2.6.1.80) and the Grisaia games' only set the flag.
    static constexpr uint64_t kEngineMesdrawClears = 0x000200060001005A;  // 2.6.1.90
    void loadEngineVersion();
    void loadUserFont();
    std::vector<Language> languages_;
    int32_t defaultLanguage_ = 0;  // <language><default>
    std::string rawTitle_;         // APP/title, with its `$xxxx-xxxx$` string id
    std::string appliedUi_ = "\x01", appliedMain_ = "\x01";
    // Points asset placeholders, story text and the title at the configured languages.
    void applyLanguage();
    // Each window style placed where its script puts its window plane (see game.cpp).
    void placeWindowLayouts();
    void applyScriptDefaults();
    std::map<std::string, int32_t> nameTable_;  // speaker name (UTF-8) -> id
    bool nameTableLoaded_ = false;
    std::vector<std::string> fontFaces_;        // startup.xml FONT/face f00, f01, ...
    struct FontCandidate {
        std::string name;
        bool bundled;  // in the font archive or font/ folder (the engine's own fonts)
        std::function<Bytes()> read;
    };
    std::vector<FontCandidate> fontCandidates() const;
    // The font files a game can use: its archives', its font/ folder's and its own folder's, the
    // extra folders', and those `fontcom add` gave. The readers refer to `assets` and `folder`.
    static std::vector<FontCandidate> fontCandidates(const AssetIndex& assets, const GameFolder& folder,
                                                     const std::vector<std::string>& dirs,
                                                     const std::vector<std::string>& added);
    mutable std::string resolvedFace_;          // the fontFaces_ entry the message font came from
    mutable bool faceTried_ = false;
    std::vector<std::string> addedFonts_;       // `fontcom add`
    mutable std::map<std::string, std::vector<std::string>> fontLists_;  // charset / adds -> list
    bool fontProportional_ = true;              // startup.xml FONT/proportional (default 1)
    bool fontBold_ = false;                     // startup.xml FONT/bold (default 0): fontweight's default
    mutable std::string configFont_;
    mutable bool configFontSet_ = false;
    std::vector<std::string> fontDirs_;
    std::string logNameFirst_, logNameLast_;  // GAME/log/nameformat: around backlog names (e.g. 【 】)
    void playVoiceFile(const std::string& voice);
    void loadNameTable();
    uint32_t readColor_ = 0xFFB0E2FF;         // startup.xml GAME/read/color, as ARGB
    bool readEnabled_ = false;                // GAME/read/enable: enable_readcol's starting value
    std::map<std::string, std::vector<uint8_t>> read_;  // scene -> read lines (bitset)
    std::string currentKey_;
    std::string sentSpeaker_, sentName_;  // the speaker last sent to the window (sendSpeaker)
    bool currentRead_ = false;
    bool skipMode_ = false, autoMode_ = false, forceSkip_ = false;
    bool autoMag_ = false;
    int32_t autoLevel_ = 1;          // sscript m[0x84444] (Fruit m[0x47498]): 1, 2, 4 ... 32
    bool autoQuietStop_ = false;
    int32_t autoResumeLevel_ = 0;
    // A speed-up re-times the wait (0x0164ca): from the wait that began at autoTimedWait_ it counts
    // from autoFrom_; autoTimerStart_ is where the engine's timer now starts (the last speed-up).
    uint64_t autoTimedWait_ = UINT64_MAX, autoFrom_ = 0, autoTimerStart_ = 0;
    uint64_t autoWaitStart() const;
    uint64_t autoFrom(uint64_t waitStart) const { return autoTimedWait_ == waitStart ? autoFrom_ : waitStart; }
    enum class FadeStep { None, Out, In };
    static constexpr uint32_t kLoadFadeFrames = 16;
    FadeStep fadeStep_ = FadeStep::None;
    uint32_t fadeFrame_ = 0;
    std::function<void()> fadeLoad_;
    enum class MenuStep { None, Out, Open, In };
    MenuStep menuStep_ = MenuStep::None;
    uint32_t menuFrame_ = 0;
    bool menuBlack_ = false;  // the frame the screen opens on (state 19 step 2): still black
    std::string menuScreen_;  // opened when the fade out ends ("saveload": menuLoad_ says which)
    bool menuLoad_ = false;
    bool eraseConfig_ = true, eraseSave_ = true;  // adv.xml etc/erase/config, /save
    bool restoreConfig_ = true, restoreSave_ = true;  // adv.xml etc/meswnd_restore/config, /save
    bool menuWindowBack_ = false;  // the window shown again before the fade back (meswnd_restore)
    std::vector<FesQuad> baseImage_;
    void stepMenu();
    void menuClosed(bool save, int32_t code);
    void loadThroughFade(std::function<void()> load);
    void stepFade();
    bool isRead(const std::string& key) const;
    void markRead(const std::string& key);
    int32_t rotatingSlot(int32_t start, int32_t count);
    int32_t strFrameTenths_ = 5;  // GAME/strframe_frame: frames per character per messpd step, in tenths
    int32_t strFrameCount_ = 11;  // GAME/strframe_count: messpd's steps
    std::vector<std::pair<int32_t, int32_t>> autoPresets_;  // GAME/autopriset: (base, char) per level
    FesInstance::Config notifiedConfig_;  // config as last announced to the window (MES_CHANGECONFIG)
    SaveStore saves_;
    std::optional<SaveFile> pendingSave_;  // `_SAVEOBJ_`: the game as it was when the menu opened
    std::optional<SaveFile> captureSave();
    int32_t quickStart_ = -1, quickCount_ = 0;  // adv.xml save/quicksave
    int32_t thumbW_ = 160, thumbH_ = 90;        // adv.xml save/thumbnail
    int32_t saveStrLen_ = 0;                   // adv.xml save/strlen: bytes of a save's message string
    std::string optTable_;                     // startup.xml language/OPTTABLE
    std::map<std::string, std::map<std::string, std::string>> optTables_;  // loaded, by file
    std::string optText(const std::string& text);
    Bytes thumbnail_;
    int32_t thumbnailW_ = 0, thumbnailH_ = 0;
    GlobalData persisted_;                      // global data as last written
    std::optional<GlobalData> loadedGlobal_;    // from disk, applied at boot
    GlobalData collectGlobal() const;
    // adv.xml <dictionary> (Cyanotype's tips): the table for the current language, the variable
    // the FE scripts find it by, and the flag the scenes set when something was unlocked.
    Dictionary dictionary_;
    bool dictionaryOn_ = false;
    std::string dictionaryFile_;
    int32_t dictionaryVar_ = -1, dictionaryFlag_ = -1;
    std::string dictionaryLoaded_;  // the file the table came from
    void loadDictionary();
    void resetVars();
    uint64_t lastGlobalCheck_ = 0;

    // startup.xml <VOICE>: sound channels with their file prefix, group and archive.
    struct VoiceEntry {
        std::string head, archive;
        int32_t group = 0, mastervol = 100;
        int32_t character = -1;  // index among the voice group's entries
    };
    std::vector<VoiceEntry> voices_;
    const VoiceEntry* voiceFor(const std::string& asset) const;
    float volumeFor(int group, const VoiceEntry* v) const;
    void applyBusVolumes();
};

}  // namespace nyan::cs2
