#pragma once

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/cs2/fes_runtime.hpp"
#include "nyan/cs2/stage.hpp"
#include "nyan/cs2/window_theme.hpp"

namespace nyan::cs2 {

// Runs the message window's own FE script (meswnd.fes etc.) for the current window style
// and drives it through the engine callbacks it exposes (MES_SHOW, MES_WAITINPUT, ...).
// The script draws the window art, buttons and click-wait icon; message text is drawn by
// the renderer using the layout the same script defines.
class WindowScript {
public:
    WindowScript(const AssetIndex& assets, VarStore& vars, const WindowTheme& theme, FesInstance::Config& config,
                 FesInstance::Host* host = nullptr);

    // Call once per frame after the scene player has ticked.
    // `atPage`: the wait is a novel page break (MES_WAITPAGE where the script has it).
    void update(const MessageWindow& window, bool waitingForInput, uint64_t frame, bool atPage = false);

    bool active() const { return instance_ != nullptr; }
    // While a window extra-button screen is open the window is hidden (sscript's ex-btn entry
    // 0x0e156b: MES_SHOW with \0 = 0, the window script asleep; its exit shows it again).
    void setSuppressed(bool on) { suppressed_ = on; }
    // Touch mode hides the window itself (sscript 0x0da6fb: MES_SHOW with \0 = 0 to the window
    // script alone); the scripts beside it stay up.
    void setTouchHidden(bool on) { touchHidden_ = on; }
    // The in-game menus hide it while they fade and show (sscript's config / save states: MES_SHOW
    // with \0 = 0 first, MES_SHOW 1 after the fade back when it was up; Game::openMenu).
    void setMenuHidden(bool on) { menuHidden_ = on; }
    // Drama mode hides it on a voiced line (ScenePlayer::setDrama: MES_SHOW 0, then MES_SHOW 1).
    void setDramaHidden(bool on) { dramaHidden_ = on; }
    // A message for the scripts beside the window (sscript sends MES_ENTER_EX_OSAWARI through
    // 0x005f59), with \0.. = args.
    void sideInvoke(const std::string& label, const std::vector<int32_t>& args = {});
    // A label of the window's own script, as `callmestag` runs it (when the script has it).
    void callLabel(const std::string& label);
    // MES_TOUCH_ENABLE to the scripts beside the window when it changes (sscript 0x0c53e3).
    void setTouchEnable(bool on);
    void cancelPress();
    FesInstance* instance() { return instance_.get(); }
    std::vector<FesQuad> draw(uint64_t frame, float windowAlpha) const;
    // What the window shows (its images and texts with any opacity, the name the renderer would
    // draw) as of `frame`, for tracing: `nyan-cli wintrace` compares it message by message.
    std::string traceSignature(uint64_t frame) const;
    // The scripts beside the window, which show and hide themselves.
    std::vector<FesQuad> drawSide(uint64_t frame) const;

    // Pointer input in virtual coordinates; click returns true if the window consumed it.
    void pointerMove(float x, float y);
    bool click(float x, float y);
    std::vector<FesInstance::Send> takeSends();
    // Tells the window the read mode (0 normal, 1 auto, 2 skip) via its MES_MODE callback.
    void setMode(int mode);
    // sscript's MES_SETMAG <mag> (10, or 20 while auto runs sped up) and then MES_AUTOMAG <n> (-1
    // auto off, 0-5 for 1x-32x), to the window and then each ex window (Yukikoi 0x0058c8 ->
    // 0x005a6d); sent with the frame's update, after MES_MODE.
    void setMag(int32_t mag, int32_t autoIndex);
    // While auto runs sped up, each message's send tells the window again (0x003cc1).
    void setMagEachMessage(bool on) { magEach_ = on; }
    // The script asked for the message's whole text since the last call (`str skip` on its text
    // object: MES_SKIP, MES_CHANGECONFIG, MES_SCROLLSPD, Yukikoi's MES_AUTOMAG).
    bool takeTextSkip() { return instance_ && instance_->takeHostSkip(textObject_); }
    // Which story commands are available (Prev needs a previous choice, Q.Load a quick save).
    // Which commands are available now (MES_SETSTATE): skip and next need text read before (or
    // "skip unread"), prev a previous choice, Q.Load a quick save, the voice button a voiced line.
    struct Availability {
        bool skip = true, next = true, prev = false, voice = false, quickLoad = false;
        bool jumping = false;  // Skip to Next Option runs (sscript state 2)
        bool choice = false;   // a choice is up (sscript states 3 select / 23 fselect)
        bool replay = false;   // a recollection replay (sscript boot -2 / -4: no saving)
        // The scene's `sysbtn <n> <0/1>` switches, by MES_SETSTATE slot (sscript 0x0807f6: a
        // table of 17 flags, all on at start, ANDed into each slot's availability at 0x0aab50).
        std::array<bool, 17> buttons{true, true, true, true, true, true, true, true, true,
                                     true, true, true, true, true, true, true, true};
        bool operator==(const Availability&) const = default;
    };
    void setAvailability(const Availability& a);
    // adv.xml etc/mesbtn/btnmode (main.kcs puts it in m[0x80005b5c]; only Nekopara sets it, to 1):
    // the buttons keep their usual states while skip or auto runs.
    void setButtonMode(bool on) { buttonMode_ = on; }
    // MES_SETNAME: the speaker of the message now shown (name-table id, -1 = none), name in $str1001.
    void setName(int32_t id, const std::string& nameSjis);
    // The click that ended a wait for input, before the lines after it run (ScenePlayer::
    // WindowEvent): MES_WAITINPUT 0, then MES_CLEAR when the message gives way.
    void clicked(bool clears);
    // `mesdraw <on>`: off sends MES_CLEAR (when `clear`); while off the window hears no
    // MES_SETNAME or MES_DRAW (sscript gates its MES_SETSTRING / SETNAME / SETMARK / REDRAW /
    // ISDRAW / DRAW sends on the flag, Happiness 2 0x0010c3 ... 0x003a9d).
    void setDraw(bool on, bool clear = true);
    // Mouse buttons on the window's own buttons; true if the window took it.
    bool mouseDown(float x, float y, bool right);
    bool mouseUp(float x, float y, bool right);
    // Tells the window a quick save completed (it plays its confirmation).
    void quickSaved();
    // Engine settings changed: MES_CHANGECONFIG.
    void configChanged();
    // The text speed for the window string's `layout frame`: sscript sends it with MES_SCROLLSPD
    // when the settings change, after a load, and for the scene's `auto <speed>` (Nekopara's
    // 0x0a61da / 0x0d5da1 / 0x0750c7 -> 0x0025f1). Sent when it differs from the last one.
    void setScrollSpeed(int32_t speed);
    // MES_SETNAMECOLOR: the colour for the next message (read text is shown in the read colour).
    void setTextColor(uint32_t argb);
    // Where the renderer laid the message text's end (relative to the text box); an NVL window
    // waiting for a click places its mark there again when it moves.
    void setTextEnd(int32_t x, int32_t y);
    // The window script's current text / name colours (`str` / `name` STRING layout), 0 if unknown.
    uint32_t textColor() const;
    // The fonts the window script chose for the message and the name (`layout font`/`fontind`), and
    // whether each keeps the fixed cell grid (`fontfix`).
    // The message string's priority: the script's planes above it draw over the text.
    int32_t textLayer() const {
        if (!instance_) return std::numeric_limits<int32_t>::max();
        int32_t p = instance_->objectPri("str");
        return p != std::numeric_limits<int32_t>::max() ? p : instance_->objectPri("nvstr");
    }
    std::string textFont(bool* fixed) const { return instance_ ? instance_->stringFont(textObject_, fixed) : (*fixed = true, std::string()); }
    std::string nameFont(bool* fixed) const { return instance_ ? instance_->stringFont("name", fixed) : (*fixed = true, std::string()); }
    uint32_t nameColor() const;
    // The speaker's name as the window script writes it into its `name` STRING (MES_SETNAME:
    // `name apend 【$str1001】` in Labyrinth's, Fruit's and Yukikoi's windows, nothing for some
    // speaker ids); nullopt when the style has no such object (the renderer uses the
    // scene's name).
    std::optional<std::string> nameText() const {
        if (!instance_ || !nameString_ || !haveName_) return std::nullopt;
        return nameText_;
    }
    // Its alignment in the name box (FesInstance::objectAlign): Amakano 1's and REBELLIONS'
    // `name apend \pc$str1001` centre it.
    int nameAlign() const { return instance_ && nameString_ ? instance_->objectAlign("name") : 0; }
    // Edge (outline) colours of the text and the name; 0 = none set.
    uint32_t textEdgeColor() const;
    // The message text's layout as the window script set it (STRLAYOUT runs the branch for the
    // current language, which a static reading of the script can't pick).
    FesInstance::StringLayout textLayout() const {
        return instance_ ? instance_->stringLayout(textObject_) : FesInstance::StringLayout{};
    }
    uint32_t nameEdgeColor() const;
    // The sub language's text object (meswnd.fes `strsub`): its layout and font.
    FesInstance::StringLayout subLayout() const {
        return instance_ ? instance_->stringLayout("strsub") : FesInstance::StringLayout{};
    }
    std::string subFont(bool* fixed) const { return instance_ ? instance_->stringFont("strsub", fixed) : (*fixed = true, std::string()); }

    std::function<void(const std::string&)> log;

private:
    const AssetIndex& assets_;
    VarStore& vars_;
    const WindowTheme& theme_;
    FesInstance::Config& config_;
    FesInstance::Host* host_;
    std::string style_;
    std::string textObject_ = "str";  // the style's text box (MES_SETSTRING's object)
    std::unique_ptr<FesInstance> instance_;
    std::vector<std::unique_ptr<FesInstance>> side_;  // WindowLayout::sideScripts
    bool shown_ = false, sideShown_ = false;
    bool suppressed_ = false, touchHidden_ = false, menuHidden_ = false, dramaHidden_ = false;
    int touchEnable_ = -1;  // last MES_TOUCH_ENABLE sent (-1: none since the scripts loaded)
    int32_t scrollSpeed_ = INT32_MIN;  // last speed the window has (its START read messpd)
    bool waiting_ = false, waitPage_ = false;
    int32_t pushed_[4] = {0, 0, 0, 0};  // MES_PUSH last sent per button
    int32_t textEndX_ = INT32_MIN, textEndY_ = INT32_MIN;
    int mode_ = 0;
    bool buttonMode_ = false;
    Availability avail_;
    bool stateDirty_ = false;
    bool modeDirty_ = false, availDirty_ = false;  // setMode / setAvailability, sent by update()
    int32_t mag_ = 10, autoIndex_ = -1;            // setMag, sent by update()
    bool magDirty_ = false, magEach_ = false;
    void sendMag();
    uint32_t color_ = 0;  // current message's text colour (MES_SETNAMECOLOR)
    int32_t nameId_ = -1;  // current message's speaker (MES_SETNAME)
    std::string nameSjis_;
    bool haveName_ = false;
    bool displayPending_ = false;
    bool drawOn_ = true;      // `mesdraw`
    bool clearSent_ = false;  // MES_CLEAR sent since the last message went up  // a message set (setName): its MES_SHOW / MES_DRAW at the update
    bool nameFresh_ = false;  // setName since the last wait began (the speaker of a message now up)
    bool redrawPending_ = false;  // a new style's script waits for `view`'s redraw (its name)
    bool nameString_ = false;  // the style's `name` is a STRING the script fills (MES_SETNAME)
    std::string nameText_;     // what MES_SETNAME put in it (UTF-8)
    void applyName();
    void applyColor();
    void sendClear();  // MES_CLEAR, and the message's name gone with it
public:
    // The window after a load or a return to an earlier point (sscript's redraw 0x0017ae: from the
    // load 0x0aa019, the backlog's return 0x0d227e, Himawari): MES_CLEAR, then the restored message's
    // text and name (Game sends the name), its wait told again.
    void redraw();
private:
    void sendState();
};

}  // namespace nyan::cs2
