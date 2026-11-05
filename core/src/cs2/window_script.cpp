#include "nyan/cs2/window_script.hpp"

#include <algorithm>
#include <cstdlib>

namespace nyan::cs2 {

WindowScript::WindowScript(const AssetIndex& assets, VarStore& vars, const WindowTheme& theme,
                           FesInstance::Config& config, FesInstance::Host* host)
    : assets_(assets), vars_(vars), theme_(theme), config_(config), host_(host) {}

void WindowScript::update(const MessageWindow& w, bool waitingForInput, uint64_t frame, bool atPage) {
    const WindowLayout* L = theme_.find(w.style);
    if (L && L->novel != w.novel) L = nullptr;
    std::string style = L ? L->script + "/" + L->images[0] : "";
    if (style != style_) {
        if (log) log("window: " + (style.empty() ? std::string("none") : style) + " (style " + w.style + (w.novel ? ", novel" : "") + ")");
        style_ = style;
        instance_.reset();
        side_.clear();
        drawOn_ = w.textVisible;
        shown_ = waiting_ = waitPage_ = sideShown_ = false;
        redrawPending_ = true;  // `view`'s redraw for the new script (see below)
        pushed_[0] = pushed_[1] = pushed_[2] = pushed_[3] = 0;  // the script's START clears them
        touchEnable_ = -1;
        scrollSpeed_ = INT32_MIN;
        if (L && !L->script.empty()) {
            // The theme's images are passed to the script in $str900.. before it starts.
            for (int i = 0; i < 3; ++i) vars_.setStr(900 + i, L->images[i]);
            instance_ = FesInstance::load(assets_, vars_, L->script, frame, &config_, host_);
            if (instance_) {
                // The renderer lays out the message, the speaker and the sub text (the engine's
                // MES_SETSTRING / MES_SETNAME fill these objects); the script's other strings it
                // draws as any screen does.
                textObject_ = L->textObject;
                nameString_ = L->nameText && instance_->hasLabel("MES_SETNAME");
                nameText_.clear();
                instance_->setHostStrings({"str", "nvstr", "name", "strsub", textObject_});
                instance_->log = log;
                stateDirty_ = true;  // after #START has run (it initialises the button states)
                // The engine places the window through the script before it starts: MES_POS with the
                // window's default position, which some scripts take and others replace with their
                // own (PX, PY) (Himawari's START then shifts its 1280x720-sized window from there onto
                // the 960x544 screen).
                if (instance_->hasLabel("MES_POS")) {
                    instance_->invoke("MES_POS", {L->originX, L->originY});
                } else if (!L->windowPlane.empty()) {
                    instance_->setPosition(L->windowPlane, L->originX, L->originY);
                }
            }
            for (const auto& name : L->sideScripts) {
                if (name.empty()) continue;
                if (auto side = FesInstance::load(assets_, vars_, name, frame, &config_, host_)) {
                    side->log = log;
                    side_.push_back(std::move(side));
                }
            }
        }
    }
    for (auto& side : side_) side->tick(frame);
    bool sideShown = !suppressed_ && (w.visible || w.alpha.value(frame) > 0);
    if (sideShown != sideShown_) {
        sideShown_ = sideShown;
        sideInvoke("MES_SHOW", {sideShown ? 1 : 0});
    }
    if (!instance_) return;
    instance_->tick(frame);
    // Engine callbacks wait until the script's own initialisation has run (it sets the window up
    // hidden; a window shown straight away, e.g. after loading a save, would be hidden again).
    if (!instance_->startedUp()) return;
    if (stateDirty_ || modeDirty_ || availDirty_) {
        // After the script's START (stateDirty_) the mode is told if it is not 0; afterwards each
        // change of mode (MES_MODE 0 too) and of the buttons' availability.
        if ((stateDirty_ && mode_ != 0) || modeDirty_) instance_->invoke("MES_MODE", {mode_});
        stateDirty_ = modeDirty_ = availDirty_ = false;
        sendState();
    }
    if (std::exchange(magDirty_, false)) sendMag();

    // A new style's script (`view`, keyword 71: Himawari's sscript 0x077c24 -> 0x001951) gets the
    // engine's redraw: MES_CLEAR, then the message's text and name as they stand, so no name once a
    // clear has emptied them.
    if (std::exchange(redrawPending_, false)) applyName();
    bool shown = !suppressed_ && !touchHidden_ && !menuHidden_ && !dramaHidden_ && (w.visible || w.alpha.value(frame) > 0);
    if (shown != shown_) {
        shown_ = shown;
        // `frameoff` / `frameon` and the other ways the window hides: the engine sends MES_SHOW alone
        // (sscript's 0x000c78 from Himawari's keywords 0x06af0c / 0x06b08a; Nekopara 0x06e690 /
        // 0x06e848 the same), no MES_SETNAME. Re-sending the last name here put Himawari's plate
        // (`img_namebg`, which any MES_SETNAME but -1 shows and only MES_CLEAR hides) back over the
        // narration after every `frameoff` that followed a speaker. A message coming up sends its own
        // MES_SHOW below, after its name.
        if (!displayPending_) instance_->invoke("MES_SHOW", {shown ? 1 : 0});
    }
    // A page break shows the page's mark (sscript 0x0867fe -> 0x002dff: MES_WAITPAGE with \0 = 1;
    // the novel windows put their page animation at the text's end), an input line the line's.
    // A wait of the other kind straight after one (text shown at once) is told again.
    const bool page = waitingForInput && atPage && instance_->hasLabel("MES_WAITPAGE");
    if (waitingForInput != waiting_ || (waitingForInput && page != waitPage_)) {
        waiting_ = waitingForInput;
        waitPage_ = page;
        if (waiting_) {
            // MES_WAITINPUT / MES_WAITPAGE alone (sscript 0x002c8e / 0x002dff): the message's name
            // went with its name line.
            instance_->invoke(waitPage_ ? "MES_WAITPAGE" : "MES_WAITINPUT", {1});
            nameFresh_ = false;  // the speaker now belongs to the message being waited on
        } else if (!clearSent_) {
            // A wait that ended without a click (a load, a jump, the scene's end; a click is
            // `clicked`): the window clears. A name set since the wait began is the next message's
            // (the scene reached it in the same frame): the engine sends it after its clear, so it
            // goes back after this one.
            const bool fresh = nameFresh_;
            const int32_t id = nameId_;
            const std::string name = nameSjis_;
            sendClear();
            if (fresh) {
                nameId_ = id, nameSjis_ = name;
                applyName();
            }
            applyColor();  // the next message's colour went before this clear, like its name
        }
    }
    // Each message as it goes up: MES_SHOW (1, or 0 with the window hidden) and MES_DRAW, after the
    // lines that set its text and name (sscript's display step, Cyanotype 0x0892ea, Nekopara
    // 0x085c70, Fruit 0x063e61, the same in every game: when the count of message lines read moves
    // on, 0x0cf8a5). The scripts' MES_SHOW refreshes their buttons (SETBUTTON): Cyanotype's name
    // plate bar (button 11) follows each message's speaker that way.
    if (displayPending_) {
        displayPending_ = false;
        clearSent_ = false;
        if (magEach_) sendMag();
        instance_->invoke("MES_SHOW", {shown_ ? 1 : 0});
        if (drawOn_ && instance_->hasLabel("MES_DRAW")) instance_->invoke("MES_DRAW", {0});
    }
}

void WindowScript::setTextEnd(int32_t x, int32_t y) {
    if (!instance_ || (x == textEndX_ && y == textEndY_)) return;
    textEndX_ = x, textEndY_ = y;
    instance_->setTextEnd(x, y);
    const char* wait = waitPage_ ? "MES_WAITPAGE" : "MES_WAITINPUT";
    if (waiting_ && instance_->hasLabel(wait)) instance_->invoke(wait, {1});
}

std::vector<FesQuad> WindowScript::draw(uint64_t frame, float windowAlpha) const {
    if (!instance_) return {};
    auto quads = instance_->draw(frame);
    for (auto& q : quads) q.alpha *= windowAlpha;
    return quads;
}

std::string WindowScript::traceSignature(uint64_t frame) const {
    std::vector<std::string> parts;
    auto add = [&](const std::vector<FesQuad>& quads) {
        for (const auto& q : quads) {
            if (q.alpha <= 0.01f) continue;
            if (!q.texture.empty()) parts.push_back(q.texture);
            else if (!q.text.empty()) parts.push_back("T:" + q.text);
        }
    };
    if (instance_) add(instance_->draw(frame));
    add(drawSide(frame));
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::string out = "name=" + (nameText() ? *nameText() : std::string("-"));
    for (const auto& p : parts) out += " | " + p;
    return out;
}

std::vector<FesQuad> WindowScript::drawSide(uint64_t frame) const {
    std::vector<FesQuad> quads;
    for (const auto& side : side_) {
        auto more = side->draw(frame);
        quads.insert(quads.end(), more.begin(), more.end());
    }
    return quads;
}

void WindowScript::sideInvoke(const std::string& label, const std::vector<int32_t>& args) {
    for (auto& side : side_) {
        if (side->startedUp() && side->hasLabel(label)) side->invoke(label, args);
    }
}

void WindowScript::callLabel(const std::string& label) {
    // sscript's `callmestag` (Hatsumira 0x07da47 -> 0x005a90): native 630 (cs2.exe message 0x151)
    // on the window's FE task, m[0x27910], with the label and an empty argument string, as it sends
    // MES_SHOW and the other MES_* labels; nothing while there is no window task.
    if (log && std::getenv("NYAN_FES_TRACE"))
        log("callmestag " + label + (instance_ ? (instance_->hasLabel(label) ? " on " + instance_->name() : " (not in " + instance_->name() + ")") : " (no window script)"));
    if (instance_ && instance_->hasLabel(label)) instance_->invoke(label);
}

void WindowScript::setTouchEnable(bool on) {
    bool ready = !side_.empty();
    for (const auto& side : side_) ready = ready && side->startedUp();
    if (!ready || touchEnable_ == (on ? 1 : 0)) return;
    touchEnable_ = on ? 1 : 0;
    sideInvoke("MES_TOUCH_ENABLE", {touchEnable_});
}

void WindowScript::cancelPress() {
    if (instance_) instance_->cancelPress();
    for (auto& side : side_) side->cancelPress();
}

void WindowScript::pointerMove(float x, float y) {
    if (sideShown_) {
        for (auto& side : side_) side->pointerMove(x, y);
    }
    if (instance_ && shown_) instance_->pointerMove(x, y);
}

bool WindowScript::click(float x, float y) {
    mouseDown(x, y, false);
    return mouseUp(x, y, false);
}

void WindowScript::setMag(int32_t mag, int32_t autoIndex) {
    mag_ = mag;
    autoIndex_ = autoIndex;
    magDirty_ = true;
}

void WindowScript::sendMag() {
    // Each window hears MES_SETMAG, then MES_AUTOMAG (0x005a6d: -1 with auto off, else the level's
    // index). The scripts' labels: Labyrinth's `str mag`, Yukikoi's and Himawari's novel window's
    // auto icon and `str layout frame` (the auto setting's speed, faster by level); most are empty.
    auto send = [&](FesInstance& s) {
        if (s.hasLabel("MES_SETMAG")) s.invoke("MES_SETMAG", {mag_});
        if (s.hasLabel("MES_AUTOMAG")) s.invoke("MES_AUTOMAG", {autoIndex_});
    };
    send(*instance_);
    for (auto& side : side_) {
        if (side->startedUp()) send(*side);
    }
}

void WindowScript::setMode(int mode) {
    if (mode == mode_) return;
    mode_ = mode;
    // Sent with the frame's window update, after the lines it ran (sscript works out the button
    // states at the end of its tick, 0x0cfda5 -> 0x0aaf2b, once the messages are up): Cyanotype's
    // SETBUTTON then sees the speaker of the message on screen when skip stops.
    modeDirty_ = true;
}

void WindowScript::setAvailability(const Availability& a) {
    if (a == avail_) return;
    // A choice coming up or going: sscript's MES_SELECT <1/0> (Nekopara 0x0044ae, from the
    // select / fselect states), which windows keep in $383 to hide their wait mark and not show
    // skip as running meanwhile (Hatsumira's MES_WAITINPUT, GET_SKIPSTATE).
    const bool selectChanged = a.choice != avail_.choice;
    avail_ = a;
    if (selectChanged && instance_ && instance_->startedUp() && instance_->hasLabel("MES_SELECT"))
        instance_->invoke("MES_SELECT", {a.choice ? 1 : 0});
    availDirty_ = true;  // sent with the window update, after the frame's messages (see setMode)
}

void WindowScript::clicked(bool clears) {
    if (!instance_ || !instance_->startedUp()) return;
    // sscript after the click (Nekopara 0xc605c): the click mark goes (0x002c8e, MES_WAITINPUT with
    // \0 = 0, after a page's wait too), then a message that gives way is cleared (0x001e89).
    if (waiting_) {
        waiting_ = false;
        if (instance_->hasLabel("MES_WAITINPUT")) instance_->invoke("MES_WAITINPUT", {0});
    }
    if (clears && !clearSent_) sendClear();
}

void WindowScript::redraw() {
    if (!instance_ || !instance_->startedUp()) return;
    sendClear();
    waiting_ = false;
    applyColor();  // the restores send the colour too (Cyanotype state 2, 0x0d0f8e -> 0x005c9b)
}

void WindowScript::sendClear() {
    instance_->invoke("MES_CLEAR");
    clearSent_ = true;
    // sscript's clear (Himawari 0x001a4a, Nekopara 0x001e89, Fruit 0x00184d: the same in every
    // script) empties the message's text and name buffers after MES_CLEAR, so whatever sends the
    // name again later (a redraw: `view`, a load, a menu closing) sends none until the next name line.
    nameId_ = -1;
    nameSjis_.clear();
    if (nameString_) nameText_ = instance_->objectText("name");
}

void WindowScript::setDraw(bool on, bool clear) {
    if (on == drawOn_) return;
    drawOn_ = on;
    if (on || !clear || !instance_ || !instance_->startedUp()) return;
    sendClear();
    sideInvoke("MES_CLEAR");
}

void WindowScript::setName(int32_t id, const std::string& nameSjis) {
    nameId_ = id;
    nameSjis_ = nameSjis;
    haveName_ = true;
    nameFresh_ = true;
    displayPending_ = true;
    applyName();
}

void WindowScript::applyName() {
    // Like the colour, the speaker belongs to the message on screen: re-sent after the window clears.
    if (!haveName_ || !drawOn_ || !instance_ || !instance_->startedUp() || !instance_->hasLabel("MES_SETNAME")) return;
    vars_.setStr(1001, nameSjis_);
    // The engine sends MES_SETNAME once a message, after MES_CLEAR (which clears `name` in the
    // scripts that only append to it); it is re-sent here after the window resets, so the name
    // starts empty each time.
    if (nameString_) instance_->clearText("name");
    instance_->invoke("MES_SETNAME", {nameId_});
    if (nameString_) nameText_ = instance_->objectText("name");
    // Then the engine draws the message (MES_DRAW), which is where windows show the speaker's name
    // plate (Cyanotype's name image). The text itself is laid out by the renderer.
    if (shown_ && instance_->hasLabel("MES_DRAW")) instance_->invoke("MES_DRAW", {0});
}

void WindowScript::sendState() {
    if (!instance_) return;
    // Command availability, as sscript computes it (Amakano 0x0aa460, Fruit 0x07da53: the same
    // rules). Slots: 0 skip, 1 auto, 2 always, 3 next choice, 4 previous choice, 5-7 and 10/11
    // the save / load group (11 quick load), 8 always, 9 voice. While skip runs only slot 0 stays
    // live, auto only 1, Skip to Next Option only 3; at a choice skip, auto, next and voice are off.
    // The skip and auto rules hold only while etc/mesbtn/btnmode is 0 (Nekopara's sscript 0x0a762f /
    // 0x0a84fe: `!m[0x80005b5c] && skip`, `!m[0x80005b5c] && (... || auto)`); with it the slots are
    // worked out as when neither runs. Skip to Next Option's rule (state 2, 0x0a8019) has no such test.
    const int mode = buttonMode_ ? 0 : mode_;
    std::vector<int32_t> state(12, mode == 0 ? 1 : 0);
    if (avail_.jumping) {
        std::fill(state.begin(), state.end(), 0);
        state[3] = 1;
    } else if (mode == 1) {
        state[1] = 1;
    } else if (mode == 2) {
        state[0] = 1;
    }
    if (!avail_.skip && !avail_.jumping) state[0] = 0;
    if (!avail_.next && !avail_.jumping) state[3] = 0;
    if (!avail_.prev) state[4] = 0;
    if (!avail_.voice) state[9] = 0;
    if (!avail_.quickLoad) state[11] = 0;
    if (avail_.choice && mode == 0) state[0] = state[1] = state[3] = state[9] = 0;
    if (avail_.replay) state[6] = state[7] = state[10] = state[11] = 0;
    for (size_t i = 0; i < state.size() && i < avail_.buttons.size(); ++i) {
        if (!avail_.buttons[i]) state[i] = 0;
    }
    instance_->invoke("MES_SETSTATE", state);
    // The toggles that are running show pushed: sscript's MES_PUSH <button> <on> (Nekopara 0x002756,
    // 27 calls: 0 skip, 1 auto, 3 Skip to Next Option), which the scripts keep in \(250 + button)
    // for their buttons' pushed frame (Amakano 2's _SETBUTTON: `setid 3`).
    if (!instance_->hasLabel("MES_PUSH")) return;
    const int32_t on[4] = {mode_ == 2 && !avail_.jumping, mode_ == 1, 0, avail_.jumping};
    for (int32_t b : {0, 1, 3}) {
        if (pushed_[b] == on[b]) continue;
        pushed_[b] = on[b];
        if (log && std::getenv("NYAN_FES_TRACE")) log("window: MES_PUSH " + std::to_string(b) + " " + std::to_string(on[b]));
        instance_->invoke("MES_PUSH", {b, on[b]});
    }
}

bool WindowScript::mouseDown(float x, float y, bool right) {
    // The scripts beside the window are over it (and touch mode's full-screen button over all).
    if (sideShown_) {
        for (auto it = side_.rbegin(); it != side_.rend(); ++it) {
            if ((*it)->mouseDown(x, y, right)) return true;
        }
    }
    return instance_ && shown_ && instance_->mouseDown(x, y, right);
}

bool WindowScript::mouseUp(float x, float y, bool right) {
    bool taken = false;
    if (sideShown_) {
        for (auto it = side_.rbegin(); it != side_.rend() && !taken; ++it) taken = (*it)->mouseUp(x, y, right);
    }
    return taken || (instance_ && shown_ && instance_->mouseUp(x, y, right));
}

void WindowScript::setScrollSpeed(int32_t speed) {
    if (!instance_ || !instance_->startedUp()) return;
    if (scrollSpeed_ == INT32_MIN) {
        // The script's START took the config's messpd (`getconf 850 messpd`, `\110 = $850`).
        auto it = config_.find("messpd");
        scrollSpeed_ = it != config_.end() ? it->second : 4;
    }
    if (speed == scrollSpeed_) return;
    scrollSpeed_ = speed;
    if (instance_->hasLabel("MES_SCROLLSPD")) instance_->invoke("MES_SCROLLSPD", {speed});
}

void WindowScript::configChanged() {
    if (instance_ && instance_->hasLabel("MES_CHANGECONFIG")) instance_->invoke("MES_CHANGECONFIG");
    applyColor();
}

void WindowScript::setTextColor(uint32_t argb) {
    color_ = argb;
    applyColor();
}

void WindowScript::applyColor() {
    // MES_SETNAMECOLOR (Cyanotype's sscript 0x005c9b): sent with the message and name lines
    // (0x0128ac, 0x013ad5, 0x0140f3), by the restores and after a config change; not when
    // the window shows or hides (`frameoff` / `frameon` send MES_SHOW alone) or a wait starts.
    if (!color_ || !instance_ || !instance_->startedUp() || !instance_->hasLabel("MES_SETNAMECOLOR")) return;
    instance_->invoke("MES_SETNAMECOLOR", {static_cast<int32_t>((color_ >> 16) & 0xFF),
                                            static_cast<int32_t>((color_ >> 8) & 0xFF), static_cast<int32_t>(color_ & 0xFF)});
}

uint32_t WindowScript::textColor() const {
    return instance_ ? instance_->stringColor(textObject_) : 0;
}

uint32_t WindowScript::nameColor() const {
    return instance_ ? instance_->stringColor("name") : 0;
}

uint32_t WindowScript::textEdgeColor() const {
    return instance_ ? instance_->stringColor(textObject_, "color#edge") : 0;
}

uint32_t WindowScript::nameEdgeColor() const {
    return instance_ ? instance_->stringColor("name", "color#edge") : 0;
}

void WindowScript::quickSaved() {
    if (instance_ && instance_->hasLabel("MES_COMP_QSAVE")) instance_->invoke("MES_COMP_QSAVE");
}

std::vector<FesInstance::Send> WindowScript::takeSends() {
    auto sends = instance_ ? instance_->takeSends() : std::vector<FesInstance::Send>{};
    for (auto& side : side_) {
        auto more = side->takeSends();
        sends.insert(sends.end(), more.begin(), more.end());
    }
    return sends;
}

}  // namespace nyan::cs2
