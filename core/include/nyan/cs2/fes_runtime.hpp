#pragma once

#include <functional>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/anm.hpp"
#include "nyan/cs2/expr.hpp"
#include "nyan/cs2/fes_script.hpp"
#include "nyan/cs2/scene_player.hpp"
#include "nyan/cs2/stage.hpp"
#include "nyan/cs2/vars.hpp"

namespace nyan::cs2 {

class SaveStore;

// One textured quad produced by an FE script for the renderer, in virtual-screen coordinates.
struct FesQuad {
    std::string texture;  // "<image>#<frame id>", "@save:<slot>:<version>" (thumbnail); empty = solid/text
    uint32_t color = 0xFFFFFFFF;  // ARGB for solid quads
    float x[4], y[4];     // corners: top-left, top-right, bottom-right, bottom-left
    float alpha = 1;
    // Text box (when `text` is set): UTF-8 wrapped in the quad's rectangle.
    std::string text;
    float textSize = 0;           // virtual pixels
    int maxLines = 1;
    int lineChars = 0;            // wrap after this many full-width cells (0: box width)
    uint32_t outlineColor = 0;    // 0: no outline
    int align = 0;                // 0 left, 1 right, 2 centre (`\pr`, `\pc` print prefixes)
    float lineHeight = 0;         // line pitch (0: the box height / maxLines)
    std::string font;             // a font the script chose (file or face name); empty: the game's
    bool proportional = false;    // `layout fontfix 0`: glyph advances instead of the cell grid
    // The string's own edge switches (`layout shadow <right-down> <around>`) and weight (`layout
    // weight <n>`); -1 / 0: the config's, as the text follows it until the script sets them.
    int8_t edgeAround = -1, edgeRightDown = -1;
    int32_t weight = 0;
    int32_t layer = 0;            // priority of the plane it belongs to
    // A second text under it (the message log's sub language): drawn at the same size in `subFont`
    // from (subX, the main text's height + subY) within the box, wrapping in the box's width.
    std::string subText;
    std::string subFont;
    float subX = 0, subY = 0;
    // A STRING being revealed after `draw` (its `layout frame` speed above 0): frames since the
    // reveal started (-1: all shown), that speed, `scrolltype` / `wipewidth`, and for the glyph-by-
    // glyph kind the frames each glyph takes.
    int64_t revealElapsed = -1;
    int32_t revealSpeed = 0, scrollType = 0, wipeWidth = 0;
    float revealFramesPerGlyph = 0;
    // The part of the texture drawn (0..1; IMGWND's tiles cut at the window's far edges).
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    // Drawn only inside this screen rectangle (virtual pixels): its container's VIEWPORT.
    bool clip = false;
    float clipX = 0, clipY = 0, clipW = 0, clipH = 0;
};

// A running FE script: its objects, variables and control flow. Driven at 60 frames per second.
class FesInstance {
public:
    // Engine services a script can request: running the scene engine (`execkcs sscript`).
    struct Host {
        virtual ~Host() = default;
        // `execkcs <program>` starts a kcs program in the script's own slot (cs2.exe request 4,
        // 0x5b4cc0: one per script); `endkcs` deletes that one (request 5), and the script's
        // `_EXITCODE_` is that program's (read from its slot each update). So each call names the
        // program the script started.
        virtual void execKcs(const std::string& name) = 0;
        virtual void endKcs(const std::string& name) = 0;
        // The program's exit code: -1 while it runs.
        virtual int32_t kcsExitCode(const std::string& name) = 0;
        // `rettitle`: leave the story and go back to the title screen.
        virtual void retTitle() {}
        // `xchgcursor [name]`: the mouse cursor becomes cursor.xml's `name` (`default` without one).
        virtual void changeCursor(const std::string&) {}
        // `baseimg [object]`: the picture the screen the scripts draw on keeps under them, past the
        // script that set it (empty: none). See FesInstance's `baseimg`.
        virtual void baseImage(std::vector<FesQuad>) {}
        // Save slots (`newsave`, `saveexist`, save panels, ...); null if the game has none.
        virtual SaveStore* saveStore() { return nullptr; }
        // `_SAVEOBJ_`: non-zero when there is a captured game state to save.
        virtual int32_t saveObject() { return 0; }
        // `datasave <slot> <object>`: writes the captured state.
        virtual bool dataSave(int32_t, int32_t) { return false; }
        // `send <code> <a> <b>` the engine answers at once (the script reads the result on the
        // next line, e.g. the message log's name requests). False: queue it for the engine.
        virtual bool request(int64_t, int32_t, int32_t) { return false; }
        // The same, told which script sent it (an answer may go into its locals, as sscript's
        // native 645 does).
        virtual bool request(FesInstance&, int64_t code, int32_t a, int32_t b) { return request(code, a, b); }
        // `getexvar <NAME> <var>`: engine state (SKIP, AUTO, SKIP_FORCE, STILL_READ, NAME).
        virtual std::optional<int32_t> exVar(const std::string&) { return std::nullopt; }
        // The message backlog (LOGSTRING objects).
        virtual const std::vector<ScenePlayer::LogEntry>* backlog() { return nullptr; }
        // `setconf` of a setting the engine derives others from (`autospd` -> autospd_b / _c).
        virtual void configSet(const std::string&) {}
        // The engine's file version (2.6.1.80 = 0x0002000600010050); 0 if unknown.
        virtual uint64_t engineFileVersion() const { return 0; }
        // Whether a CG gallery entry has been recorded (the engine's records, which sscript writes
        // into the global data, native 424).
        virtual bool cgEntrySeen(const std::string&) { return false; }
        // The engine's glossary (adv.xml <dictionary>), for the FE `dic_*` commands; null if none.
        virtual class Dictionary* dictionary() { return nullptr; }
        // Whether music still plays on a BGM channel (a music-room track ending).
        virtual bool bgmPlaying(int32_t) { return true; }
        // Whether a key bound to Force Skip (key action 12) is held, as the FE task's update asks it
        // each frame (message 0x8e; sscript's native 285 asks the same).
        virtual bool forceSkipHeld() { return false; }
        // Whether a sound a script started (kind: the MediaCommand::Kind it was played with) is still
        // playing; false without sound.
        virtual bool soundPlaying(int, int32_t) { return false; }
        // Sound settings of soundconf.xml (getsndconfex / setsndconfex): `field` is volume, play,
        // sysplay, mute, voicebgm or voicemute; the part is `name` (master, bgm, voice, ...) when
        // `ids` is empty, else the path of part ids (`volume_id 2 1`). Nothing if the game has none.
        virtual std::optional<int32_t> soundConf(const std::string&, const std::string&, const std::vector<int32_t>&) {
            return std::nullopt;
        }
        virtual bool setSoundConf(const std::string&, const std::string&, const std::vector<int32_t>&, int32_t, bool) {
            return false;
        }
        // `sysvoiceex`: a random system-voice character among the given part paths (all of the
        // sysvoice parts when empty) whose sysplay is on; -1 for none, nothing if no soundconf.xml.
        virtual std::optional<int32_t> pickSystemVoice(const std::vector<std::vector<int32_t>>&) { return std::nullopt; }
        // `setsndconfex <v> reset <part>` / `reset_id <ids>`: the part's settings back to soundconf.xml's.
        virtual bool resetSoundConf(const std::string&, const std::vector<int32_t>&, bool) { return false; }
        // A STRING's text as the renderer lays it out: the length a wipe reveal runs along (the
        // lines' lengths added up, virtual pixels); none when nothing draws (headless).
        virtual std::optional<float> textWipeLength(const FesQuad&) { return std::nullopt; }
        // The number of lines a STRING lays out into (wrapped lines too); none when nothing draws.
        virtual std::optional<int32_t> textLineCount(const FesQuad&) { return std::nullopt; }
        // The text's laid-out extent within the box (widest line, last line's bottom), in pixels.
        virtual std::optional<std::pair<float, float>> textDrawArea(const FesQuad&) { return std::nullopt; }
        // `fontcom`: the system font list for a charset, a font file added, the startup face; and
        // the config's `font` (getconf / setconf), the face strings start with.
        virtual std::vector<std::string> fontList(int32_t) { return {}; }
        virtual void addFont(const std::string&) {}
        virtual std::string defaultFont() { return {}; }
        virtual std::string configFont() { return {}; }
        virtual void setConfigFont(const std::string&) {}
        // Frames a glyph takes at text speed `speed` (messpd's scale; startup.xml strframe_frame).
        virtual float framesPerGlyph(int32_t speed) { return static_cast<float>(speed) * 0.5f; }
    };

    struct Object {
        std::string name;  // "pl[0]", "bg"
        std::string type;  // PLANE, IMAGE, BUTTON, STRING, SOUND, ...
        int32_t uid = 0;   // value of the object's name in expressions (`_CLICK_L_==bg`)

        std::string file;              // image file (FILE), resolved from $strN
        std::map<int, int32_t> ids;    // ID.n / settbl n -> frame id (the table; unset entries are n)
        bool idTable = false;          // an ID.n column or settbl has written the table
        int32_t idIndex = 0;           // `setid`
        bool disp = true;
        bool dispCommanded = false;  // a `disp` command has set it (not just the #OBJECT row)
        bool dispColumn = false;     // its #OBJECT row gave DISP
        bool enable = true;
        int32_t pri = 0;
        std::string plane;   // PL: container this image is drawn in
        std::string viewport;  // VIEWPORT: the VIEWPORT object whose rectangle clips it
        bool originPlane = false;  // no PL: drawn in the plane over video memory's origin
        std::string parent;  // PARENT: container this container is attached to
        Tween x, y;          // POS
        int32_t baseX = 0, baseY = 0;
        bool baseSet = false;  // BASE or `base` has set it
        // `delay <n>` (cs2.exe 0x748620, +0x28): motions the object starts from now on begin n
        // frames later (each start passes it on, 0x748510); `stop` clears it (0x748540).
        int32_t delay = 0;
        // BTYPE (column 0x3e, cs2.exe 0x759e40 -> object +0x294, used when an image loads, 0x65dbf0):
        // 1 turns the image about its HG base point and puts that point at the position (pivot =
        // the image's base, no extra offset); 0, the default, keeps the canvas at the position.
        int32_t btype = 0;
        int32_t width = 0, height = 0;
        int32_t vramX = -1, vramY = -1;  // VRAM: where a plane's surface lies in video memory
        Tween alpha = Tween::constant(255);
        Tween scaleX = Tween::constant(100), scaleY = Tween::constant(100);
        float rotation = 0, rotX = 0, rotY = 0;  // degrees (z; x/y tilts), as planes
        std::vector<Motion> motions;
        std::string text;  // STRING contents (UTF-8)
        bool mask = false;      // MASK objects shape their plane; they are not drawn
        bool animate = false;   // IDN -1: cycle through all frames of the image
        // `setanm <script>` / `anm <x>` (cs2.exe 0x770e00 / 0x770f20): an ANM script picks the frame;
        // `anm` plays it from the start unless its argument is STOP. The running script (made by
        // `anm`, stepped when the image is drawn).
        std::string anmName;
        bool anmPlaying = false;
        uint64_t anmStart = 0;
        std::shared_ptr<AnmPlayer> anmPlayer;
        bool colorFill = false; // CPLANE/CIMAGE with COLOR: solid rectangle of SIZE
        uint32_t color = 0;
        std::string group;      // SOUND: bgm / se / voice
        int32_t stype = -1;     // SOUND: `STYPE` column, the volume class (0 = BGM); -1 when not given
        // SNDTABLE (music room): its tracks (`sndtbl <n> <bgm> <enabled>`), the play settings, and
        // whether a track was started (its end fires `<object>.STOP`).
        std::vector<std::pair<std::string, bool>> tracks;
        bool repeat = false, shuffle = false, playing = false;
        // `crossfade <on> <frames>` (cs2.exe 0x779ab0 -> +0x1bc / +0x1c0): a new track fades in
        // over the one playing instead of stopping it first (the sound sync 0x662630).
        bool crossfade = false;
        int32_t crossfadeFrames = 0;
        std::string playingFile;  // the track last started
        uint64_t playFrom = 0;
        // SOUND: the engine's playing flag (+0x148: set by `play`, cleared by `stop` and when the
        // sound task reports the sound over), and where it plays, for `wait <object>`.
        bool soundOn = false;
        int soundKind = 0;  // MediaCommand::Kind of the play
        int32_t soundChannel = 7;
        int32_t soundVolume = 100;  // SOUND: `blend` (0x74c4b0), the level a play starts at
        // SLIDER: knob image FILE moving along the track rect (POS2 frame), value in [lo, hi].
        int32_t sliderLo = 0, sliderHi = 10, sliderValue = 0;
        int32_t sliderPage = 0;  // SCROLL: values one view shows
        int32_t scrollGrab = 0, scrollThumb = 0;  // SCROLL2 while its thumb is dragged: where it was held, where it is
        // SAVEPANEL: parts drawn from the panel image FILE and the bound save slot.
        struct PanelPart {
            std::string kind;    // NORM, SIMAGE, SSTR, NUM_O, NUM_O2
            int32_t id = 0;      // frame of the panel image: the part's image or its rectangle
            uint32_t mask = 0;   // SSTR: string key; SIMAGE: data key
            // NUM_O / NUM_O2 <id> <font> <source>[:<divisor>] (cs2.exe 0x67bdd0 / 0x668f40): the
            // digit font object; the source FLAG.<n> (a flag of the save), DATE_Y / M / D / H / N
            // / S (the save's time) or else a number (`setnum` changes it); the divisor (0: none,
            // -1 for a date without one).
            std::string font;
            std::string field;   // FLAG, DATE_*, or empty for a number
            int32_t flag = 0, div = 0, value = 0;
            int32_t size = 12, lineChars = 0, lines = 1;
            uint32_t color = 0xFFFFFFFF, outline = 0;
            bool visible = true;
        };
        std::vector<PanelPart> parts;
        // STRING / LOGSTRING text layout (`layout size/rsize/margin/color...`).
        int32_t textSizes[5] = {16, 20, 20, 25, 28};
        int32_t rubySize = 0;
        int32_t margin[4] = {0, 0, 0, 0};  // left top right bottom
        int32_t moveX = 0, moveY = 0;      // `layout move`: extra spacing between characters / lines
        int32_t indent = 0;                // `layout indent`: hang lines under an opening bracket
        bool sizesSet = false, rubySet = false, moveSet = false, marginSet = false;  // which the script's `layout` has given
        bool indentSet = false;
        // `layout scrolltype` / `wipewidth` / `frame`: the reveal (0 glyph by glyph, 1 a wipe
        // `wipewidth` pixels soft) and its speed (-1: the config's messpd).
        int32_t scrollType = 0, wipeWidth = 0, frameSpeed = -1;
        // STRING / NVSTRING `mag [n]` (cs2.exe 0x776af0 -> +0x530 / +0x638, bare 10): the reveal's
        // speed in tenths, sent to the text with the object (message 0x1cd, 0x6634e4).
        int32_t mag = 10;
        bool scrollSet = false, wipeSet = false, frameSet = false;
        // The text object's line-break sets (UTF-8; kKinsokuDefaults until `layout c_prof / prof /
        // d_prof` changes them).
        std::string kinsoku[3];
        bool kinsokuSet = false;
        // STRING `draw` at a speed above 0: the frame the reveal started (-1: none, all shown) and
        // the speed; the frame of the last `str` (isdraw answers -1 until the next frame).
        int64_t revealStart = -1;
        int32_t revealSpeed = 0;
        uint64_t strFrame = UINT64_MAX;
        int textAlign = 0;                 // STRING: from the `str` print prefix
        // `layout font <face>` / `layout fontind <n> <file>`: the text's font (empty: the game's);
        // `layout fontfix 0`: proportional spacing instead of the fixed cell grid. Until a script
        // sets one, a string follows the config's font (fontFollows; see fontOf).
        std::string fontName;
        bool fontFollows = false;
        std::string subFontName;    // `layout fontind_sub` (the message log's sub language)
        int32_t shiftSubX = 0, shiftSubY = 0;  // `layout shift_sub <x> <y>`
        bool fontFixed = true;
        // `layout shadow <right-down> <around>` / `layout weight <n>` (Nekopara cs2.exe 0x74e8f0 /
        // 0x74eeb0): the string's edge switches and weight, which stop it following the config's
        // (-1: follows).
        int32_t shadow[2] = {-1, -1};
        int32_t weight = -1;
        std::map<std::string, uint32_t> colors;  // color, vcolor, acolor, vacolor, pcolor, vpcolor (ARGB)
        // LOGSTRING: first entry shown, entries per page (COUNT), highlight per row (blockcolor).
        int32_t logPos = 0, logCount = 4;
        std::string pos2File;  // POS2 frame reference: LOGSTRING block i sits at frame pos2Id + i
        int32_t pos2Id = -1;
        bool logBound = false;
        bool logSub = true;  // `remake`: entries show their sub-language line (0x7614a0)
        std::map<int32_t, int32_t> blockColor;
        int32_t saveSlot = -1;
        int32_t index = 0;      // element index within its array (`@` in handlers)
        std::string base;       // array name without index ("btn" for "btn[3]")

        bool container() const;
        int32_t frame() const;  // the frame to draw; -1: the whole image (index -1)
        void setIndex(int32_t n);  // setid / IDN / load's id (cs2.exe 0x749f00)
    };

    // Engine configuration (`getconf`/`setconf`), shared by all scripts of a game.
    using Config = std::map<std::string, int32_t>;

    // Loads `<name>.fes`. Returns nullptr if the script does not exist.
    static std::unique_ptr<FesInstance> load(const AssetIndex& assets, VarStore& globals, const std::string& name,
                                             uint64_t frame, Config* config = nullptr, Host* host = nullptr);
    // An instance of a script already parsed, started at its START as load() starts it.
    static std::unique_ptr<FesInstance> create(const AssetIndex& assets, VarStore& globals, const std::string& name,
                                               FesScript script, uint64_t frame, Config* config = nullptr,
                                               Host* host = nullptr);

    // The innermost running child screen (this instance if it has none). Input goes there.
    FesInstance& top() { return child_ ? child_->top() : *this; }

    void tick(uint64_t frame);
    // Pointer input in virtual-screen coordinates. Hover changes fire FOCUS/UNFOCUS handlers.
    // The pointer is over one of this script's (or its child's) buttons.
    bool hovering() const { return hovered_ || (child_ && !child_->exited() && child_->hovering()); }
    bool dragging() const { return dragging_ || (child_ && !child_->exited() && child_->dragging()); }
    void clearHover();  // the pointer left this script's buttons (UNFOCUS)
    void pointerMove(float x, float y);
    // Mouse buttons: PUSH on press, RELEASE on release, slider dragging. _CLICK_L_ / _CLICK_R_ are
    // set on the release, where the engine sets them on the press (0x60e480 event 2, cleared by the
    // release, event 4): kept by the user's decision (2026-11-02) for touch, where a finger going
    // down may still become a swipe, a hold or a two-finger gesture. Returns true if an object took
    // the press/release.
    bool mouseDown(float x, float y, bool right = false);
    bool mouseUp(float x, float y, bool right = false);
    // Forgets a press without releasing it (the touch turned into a gesture).
    void cancelPress();
    // Press + release at one point.
    bool click(float x, float y, bool right = false);
    // A Confirm key press: as a left press for the click flag (see keyskipOn_), on the topmost script.
    void pressSkip();
    // Runs a block synchronously as an engine callback, with arguments in \0, \1, ...
    void invoke(const std::string& label, const std::vector<int32_t>& args = {});
    bool hasLabel(const std::string& label) const { return script_.labels.count(label) != 0; }
    // Sets a local variable (\N) before the script's first step (engine-passed arguments).
    void setLocal(int32_t index, int32_t value) { locals_.set(index, value); }
    int32_t local(int32_t index) const { return locals_.get(index); }
    // Scroll inputs for this frame (`_SCRL_U_`, `_SCRL_D_`, `_PAGE_U_`, `_PAGE_D_`), from the
    // game's key bindings: 0 up, 1 down, 2 page up, 3 page down.
    void scroll(int kind);
    // Draw STRING objects' own text (false for the message window, whose text the renderer lays out).
    void setDrawStrings(bool on) { drawStrings_ = on; }
    // Strings someone else draws (the message window's text, name and sub text, laid out by the
    // renderer); the script's other strings it draws itself (Nekopara's quick-save card).
    void setHostStrings(std::set<std::string> names) { hostStrings_ = std::move(names); }
    // A host string's `skip` since the last call: its whole text is to show now.
    bool takeHostSkip(const std::string& name) { return hostSkipped_.erase(name) != 0; }
    // An object's priority (INT32_MAX if the script has no such object).
    int32_t objectPri(const std::string& name) const {
        auto it = byName_.find(name);
        return it == byName_.end() ? std::numeric_limits<int32_t>::max() : objects_[it->second].pri;
    }
    // Where the renderer's message text ends, relative to the text object (`endpos`).
    void setTextEnd(int32_t x, int32_t y) { textEndX_ = x, textEndY_ = y; }
    // Text colour from a STRING object's `layout color` (0 if none).
    uint32_t stringColor(const std::string& object, const std::string& key = "color") const;
    // A string object's text layout as the script last set it (`layout size` / `rsize` / `move`);
    // fields the script hasn't set are -1.
    struct StringLayout {
        int32_t sizes[5] = {-1, -1, -1, -1, -1};
        int32_t ruby = -1;
        int32_t moveY = -1;
        int32_t margin[4] = {-1, -1, -1, -1};  // left top right bottom
        int32_t indent = -1;
        int32_t scrollType = -1, wipeWidth = -1;
        int32_t frame = INT32_MIN;  // `layout frame` (-1: messpd); INT32_MIN: not set
        int32_t mag = 10;           // `mag`: glyphs show when elapsed x mag / 10 reaches them
        bool proportional = false;
        // The line-break sets (see kKinsokuDefaults), when the script changed them.
        bool kinsokuSet = false;
        std::string kinsoku[3];
    };
    // A text object's line-break sets as cs2.exe builds them (0x6921e0, the same in grisaia.exe
    // 0x547710): 0, no line start (hangs at a line's end); 1, no line end (an opener moves to the
    // next line); 2, kept together. FE `layout c_prof <n>` empties set n, `prof <n> <chars>` adds
    // to it, `d_prof <n> <chars>` takes out (applied in that order, 0x662da0: messages 0x1d4,
    // 0x1ce, 0x1d1).
    static constexpr const char* kKinsokuDefaults[3] = {
        "\u3000 \u3001\u3002\uff0c\uff0e\u30fb\uff1f\uff01\u309b\u309c\u30fd\u30fe\u309d\u309e\u3005\u30fc\uff0d\uff1d\uff5e\uff1a\uff1b\uff09\uff3d\uff5d\u300d\u300f!),.:;?]}\uff61\uff63\uff64\uff65\uff70\uff9e\uff9f\u266a"
        "\u30a1\u30a3\u30a5\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u30ee\u30f5\u30f6\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308e"
        "\u2460\u2461\u2462\u2463\u2464\u2465\u2466\u2467\u2468\u2469\u246a\u246b\u246c\u246d\u246e\u246f\u2470\u2471\u2472\u2473",
        "\uff08\uff3b\uff5b\u300c\u300e([{\uff62",
        "\u2026\u2025\u2015"};
    StringLayout stringLayout(const std::string& object) const;
    // The font a STRING object's layout chose (empty: the game's) and whether it keeps the cell grid.
    std::string stringFont(const std::string& object, bool* fixed) const;
    // Places a top-level object (e.g. the message window plane) on screen.
    void setPosition(const std::string& object, int32_t x, int32_t y);
    // An object's position as the script left it (POS, `pos`, motions at `frame`), its PARENT and
    // its type; false if the script has no such object.
    bool objectPosition(const std::string& object, uint64_t frame, int32_t& x, int32_t& y, std::string* parent = nullptr,
                        std::string* type = nullptr) const;
    // The script's objects by name, in definition order.
    std::vector<std::string> objectNames() const;
    // A STRING object's text in UTF-8 (empty if none), and clearing it as `<object> clear` does.
    std::string objectText(const std::string& object) const;
    // A STRING's alignment from its text's print prefix: 0 left, 1 right (`\pr`), 2 centred (`\pc`).
    int objectAlign(const std::string& object) const;
    void clearText(const std::string& object);

    // `send <code> <a> <b>` requests to the engine since the last call.
    struct Send {
        int64_t code;
        int32_t a, b;
    };
    std::vector<Send> takeSends() { return std::exchange(sends_, {}); }

    // A dialog the engine shows in a window of its own on PC (`input_text`: a line of text, e.g. the
    // player's name or a save comment; `messagebox`: a question). The script waits until the
    // platform answers. Texts are UTF-8.
    struct Dialog {
        uint32_t serial = 0;    // changes with each new dialog
        bool input = false;     // input_text (else messagebox)
        std::string title;      // input_text: the prompt
        std::string message;    // messagebox body
        std::string value;      // input_text: the starting text
        // input_text: the edit box's limit in characters (EM_LIMITTEXT; 128 unless given, 0 none),
        // and whether it can be cancelled (its fifth number, 1 unless given).
        int32_t maxChars = 128;
        bool cancel = true;
        int32_t type = 0;       // messagebox: 0 OK only, 2 yes / no
    };
    // The waiting dialog of this screen or its open child, if any.
    const Dialog* dialog() const { return child_ ? child_->dialog() : dialog_ ? &*dialog_ : nullptr; }
    // The player's answer: OK / yes, and for input_text the text entered.
    void answerDialog(bool ok, const std::string& valueUtf8 = {});

    bool exited() const { return exited_; }
    // True once the script has finished its #START block (initialisation).
    bool startedUp() const { return startedUp_ || exited_; }
    int32_t exitCode() const { return exitCode_; }
    const std::string& name() const { return name_; }

    // Everything to draw this frame, back to front.
    std::vector<FesQuad> draw(uint64_t frame) const;
    // One plane (or other container) as it shows now, nothing else (`baseimg <plane>`).
    std::vector<FesQuad> drawContainer(const std::string& name, uint64_t frame) const;
    // Sound requests from this script and its children.
    std::vector<MediaCommand> takeMedia();

    std::function<void(const std::string&)> log;
    const std::map<std::string, size_t>& unhandled() const { return unhandled_; }

private:
    FesInstance(const AssetIndex& assets, VarStore& globals, std::string name, FesScript script);

    struct FrameInfo {
        int32_t x = 0, y = 0, w = 0, h = 0, baseX = 0, baseY = 0;
    };
    const FrameInfo* frameInfo(const std::string& file, int32_t id) const;

    void createObjects();
    void applyRow(Object& o, const std::map<std::string, std::string>& values);
    std::vector<Object*> resolve(const std::string& ref);
    Object* find(const std::string& name);

    void run();
    bool execute(const FesScript::Statement& st);  // returns false when execution must stop
    void jump(const std::string& label, bool call);
    void skipBlock(bool toElse);
    void method(Object& o, const std::string& name, const std::vector<std::string>& args);
    void primitive(Object& o, const std::string& prim, const std::vector<std::string>& args);
    // Whether anything moves (cs2.exe 0x601be0: a motion of the object, or of an object inside it,
    // whose running flag +0x9c is still up; mot.int's loops lower it only when stopped), as the
    // update before the frame `at` left it.
    bool animating(uint64_t at) const;
    bool moving(const Object& o, uint64_t at, int depth = 0) const;
    void unhandled(const std::string& what);
    void scriptError(const std::string& what);

    int32_t num(const std::string& expr, int32_t current = 0);
    // `_FRAME_`: the frames since the count's base (the first update, or a `reset_frame`).
    int64_t frameCount() const { return frameBase_ ? static_cast<int64_t>(frame_ - *frameBase_) : 1; }
    // A property's new value: `@` is the property's current value even in an event handler.
    int32_t numAt(const std::string& expr, int32_t current);
    float percent(const std::string& expr, float current);
    std::string str(const std::string& token) const;

    struct Affine {
        float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;  // x' = a*x + c*y + tx, y' = b*x + d*y + ty
        Affine operator*(const Affine& o) const;
        void apply(float x, float y, float& ox, float& oy) const;
    };
    Affine transform(const Object& o, uint64_t frame) const;
    Affine worldTransform(const Object& o, uint64_t frame, int depth = 0) const;
    float worldAlpha(const Object& o, uint64_t frame, int depth = 0) const;
    void drawSavePanel(const Object& o, const Affine& world, float alpha, uint64_t frame,
                       std::vector<FesQuad>& out) const;

    const AssetIndex& assets_;
    // ANM scripts by name (`setanm`), parsed when first drawn.
    mutable std::map<std::string, std::shared_ptr<const AnmScript>> anmScripts_;
    std::vector<std::string> fontList_;  // `fontcom init`
    bool fontListMade_ = false;
    std::string startFace() const;
    std::string fontOf(const Object& o) const;
    std::shared_ptr<const AnmScript> anmScript(const std::string& name) const;
    VarStore& globals_;
    VarStore locals_;
    ExprEvaluator expr_;
    std::string name_;
    FesScript script_;
    std::vector<Object> objects_;
    std::unordered_map<std::string, size_t> byName_;
    void revealPlane(const Object& child);
    // A STRING's text box as draw() hands it over (corners in the object's own space), with its
    // reveal; the frames the reveal runs; what `isdraw` answers.
    FesQuad stringBox(const Object& o, uint64_t frame) const;
    static void styleOf(const Object& o, FesQuad& q);  // the string's own edge switches and weight
    int64_t revealFrames(const Object& o) const;
    int32_t stringDrawing(const Object& o) const;
    // Objects (base names) the script itself shows somewhere: `<name> disp <not 0>` or `<name> fade`.
    std::set<std::string> shownByScript_;

    enum class State : uint8_t { Running, WaitFrames, WaitFrameCount, WaitAnimation, WaitSound, WaitObject, Idle, Dialog };
    std::string waitSound_;  // WaitSound / WaitObject: the object `wait` named
    std::optional<Dialog> dialog_;
    int32_t dialogResultVar_ = -1, dialogValueVar_ = -1;
    State state_ = State::Idle;
    size_t pc_ = 0, end_ = 0;
    std::vector<std::pair<size_t, size_t>> callStack_;
    uint64_t frame_ = 0, waitUntil_ = 0;
    uint64_t motionAt_ = 0;  // where a motion the current method starts begins (frame_ + delay)
    int64_t waitFrameCount_ = 0;  // `wait frame <n>`: until `_FRAME_` reaches n
    std::optional<uint64_t> frameBase_;  // `_FRAME_` counts from here
    // `rwipe` / `rdraw` (cs2.exe 0x780480 / 0x780280): a wipe or fade from the screen as last drawn
    // to the current one, over everything (the engine's `__rwipeobj__` / `__rdrawobj__` on top).
    // A plane's `draw` (its DRAW object, `__drawobj__`) fades the same way inside the plane's box.
    struct ScreenWipe {
        uint64_t start = 0;
        uint32_t frames = 0;
        std::string mask;  // empty: a fade
        bool box = false;  // only inside x0, y0 - x1, y1 (screen coordinates)
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    } wipe_;
    bool wiping(uint64_t frame) const { return wipe_.frames > 0 && frame < wipe_.start + wipe_.frames; }
    mutable const std::string* drawOnly_ = nullptr;  // drawContainer's plane
    bool exited_ = false;
    bool startedUp_ = false;
    int32_t exitCode_ = -1;
    Object* current_ = nullptr;  // `@`: object of the running event handler
    const Object* hovered_ = nullptr;
    const Object* scrollHover_ = nullptr;  // the SCROLL2 whose thumb the pointer is over
    const Object* pressed_ = nullptr;
    Object* dragging_ = nullptr;
    // Click skipping (Amakano 1's cs2.exe; the update 0x60de30). `keyskip` (0x62c140) sets +0x5b0
    // (on: may a click hurry a wait; on at the start, 0x60d0c0), +0x5b4 (`ctrl`) and +0x70c
    // (`cont`). A left press sets the click flag +0x5ac (the input handler 0x60e480, event 2), which
    // the update clears at its end unless `cont` or a held skip (+0x5b8, events 0x13 / 0x14 or
    // `keyskip force`) keeps it: under `cont` one click hurries every wait that follows, one an
    // update (Amakano 1's logo and title).
    bool keyskipOn_ = true, keyskipCtrl_ = false, keyskipCont_ = false;
    bool skipClick_ = false, skipHeld_ = false;
    // The Force Skip key (key action 12) as the FE task's update last asked it (+0x334, 0x52eb08:
    // message 0x8e, what sscript's native 285 asks); a change is event 0x13 / 0x14.
    bool skipKeyDown_ = false;
    // `_CLICK_L_` holds the -1 a held skip put there: the update clears it only after an input
    // event (+0x5c0, events 2, 3 and 6-14), so it stays while nothing else comes.
    bool clickFromHold_ = false;
    bool skipClickFromPress_ = false;  // set by the press still down (a gesture taking it over undoes it)
    // The engine's skip (0x601c10 on the root: every motion of every object gets its stop flag,
    // 0x63b700), as the scenes' `stop`.
    void skipMotions();
    // The task's full-screen "low button" (cs2.exe: created by the FE task's update 0x629840, kept
    // at +0x1a4): clicks that reach no button land on it. `deletebgbutton` removes it,
    // `enablebgbutton` turns it off or on, `basebtnpri <n>` sets its priority (-1, the default:
    // the task's depth, under every button). `sendbtn <n>` (+0x820) is what the task answers for
    // an input event its script leaves unhandled (message 0x5e); with 0 the low button keeps
    // the click, so a dialog raised over its parent is modal.
    bool rehover_ = false;  // `resetactive`: focus what is under the pointer at the next update
    bool lowButton_ = true, lowButtonEnabled_ = true;
    std::optional<int32_t> lowButtonPri_;
    int32_t sendBtn_ = 1;
    std::optional<int32_t> blockingLayer() const;  // the low button's layer when it keeps clicks
    bool drawStrings_ = true;
    std::set<std::string> hostStrings_;
    std::set<std::string> hostSkipped_;
    int32_t textEndX_ = 0, textEndY_ = 0;
    bool proportionalReset_ = false;
    bool vramPlanes_ = false;  // newer engine: planes show areas of video memory (plane-less objects, shared VRAM)  // the script is for the string engine whose `reset` is proportional
    int32_t scrollFlags_[4] = {0, 0, 0, 0};
    uint64_t scrollAt_ = 0;
    void logMethod(Object& o, const std::string& m, const std::vector<std::string>& args);
    void layoutMethod(Object& o, const std::vector<std::string>& args);
    const Object* sliderAt(float x, float y) const;
    static bool sliderUpright(const Object& o);
    static int32_t sliderTop(const Object& o);  // the highest value the bar can take
    // SCROLL2 (kcTaskCtrlScroll2): a thumb of three pieces on the POS2 frame's track.
    struct Scroll2Box {
        bool vertical = true;
        int32_t len = 0;     // the track's length
        int32_t thumb = 0;   // the thumb's length
        int32_t travel = 0;  // how far it moves
        int32_t span = 0;    // its values run 0..span
        int32_t cross = 0;   // its width across the track
        int32_t caps = 0;    // its end pieces' lengths
    };
    bool scroll2Box(const Object& o, Scroll2Box& b) const;
    int32_t scroll2Thumb(const Object& o, const Scroll2Box& b) const;
    static int32_t scroll2Value(const Scroll2Box& b, int32_t thumb);
    // The SCROLL2 under a point, and the point along and across its track.
    Object* scroll2At(float x, float y, float& along, float& across);
    bool scroll2Local(const Object& o, float x, float y, float& along, float& across) const;
    bool fileExists(const std::string& path) const;
    // `archive.int/entry` as fileExists finds it (an update archive's copy first), else an asset.
    std::optional<Bytes> readFile(const std::string& path) const;
    // `cgreg_init`'s table (the FE task's +0x6cc, amakanoPE.exe 0x62c8b0): the groups of the list
    // it named (empty if that list is missing). None before cgreg_init: the gallery commands
    // answer 0. The records are the engine's (Host::cgEntrySeen).
    std::optional<std::vector<ScenePlayer::CgGroup>> cgTable_;
    // A gallery group token: a number (starting with $, \ or a digit) or a group name of the table
    // (case-insensitive, 0x642430); -1 if none.
    int32_t cgTableGroup(const std::string& tok);
    int32_t cgTableIndex(const std::string& name) const;
    // The group's entries recorded / in all; false if the table has no such group.
    bool cgTableCount(int32_t group, int32_t& seen, int32_t& total) const;
    void dragTo(Object& slider, float x);
    bool sliderTrack(const Object& o, float& x0, float& x1, float& y0, float& y1) const;
    std::vector<Send> sends_;
    Config* config_ = nullptr;
    Host* host_ = nullptr;
    std::unique_ptr<FesInstance> child_;  // `execfes`
    enum class Launched : uint8_t { None, Fes, Kcs } launched_ = Launched::None;
    bool kcsRunning_ = false;  // this script's `execkcs` program is running (the one `endkcs` ends)
    std::string kcsProgram_;   // the program this script last started
    size_t blockStart_ = 0;  // start of the main block, re-run every frame when it ends
    size_t blockEnd_ = 0;    // and its end (an empty block, `#WAIT` right above `#EXIT`, stays empty)
    int32_t exitCodeOfLaunched();
    int32_t layerOf(const Object& o) const;  // the priority of the plane it is drawn on
    std::optional<int32_t> hitLayerAt(float x, float y) const;
    bool childTakes(float x, float y) const;
    bool childBlocks(float x, float y) const;
    const Object* hitTest(float x, float y) const;
    void fireEvent(const Object& o, const std::string& event);

    // Click identifiers: 0 = no click this frame, -1 = clicked empty space, else the object's uid.
    int32_t clickL_ = 0, clickR_ = 0;
    uint64_t clickAt_ = 0;    // frame of the last click
    bool clickRead_ = false;  // the script read a click identifier this frame
    float mouseX_ = 0, mouseY_ = 0;
    bool pointerSeen_ = false;  // a pointer position has been given

    std::vector<MediaCommand> media_;
    std::map<std::string, size_t> unhandled_;
    std::set<std::string> scriptErrors_;
    mutable std::unordered_map<std::string, std::map<int32_t, FrameInfo>> frames_;
};

}  // namespace nyan::cs2
