#include <cstdio>
#include "nyan/render/game_session.hpp"

#include <algorithm>

#include "nyan/cs2/fes_runtime.hpp"
#include "nyan/cs2/font_select.hpp"

namespace nyan::render {

using KeyMap = cs2::KeyMap;

GameSession::GameSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer, Options options)
    : game_(std::make_unique<cs2::Game>(std::move(folder), mixer)) {
    const auto& cfg = game_->config();
    int fontFace = 0;
    game_->setFontDirectories(options.fontDirectories);
    Bytes font = game_->loadFont(&fontFace);
    // A face the player has not added: Windows' font mapper would give another Shift-JIS face. The
    // engine's own default is ＭＳ ゴシック (Happiness 2 cs2.exe 0x5b1aa0, the face until one is given),
    // so that one when it is among the font folders.
    if (font.empty()) {
        font = game_->loadFont(&fontFace, "ＭＳ ゴシック");
    }
    if (font.empty()) {
        // Else the platform's font (Android's own): in a CJK collection, its Japanese face (CatSystem2
        // games expect Japanese system fonts such as MS Gothic).
        font = std::move(options.fallbackFont);
        fontFace = 0;
        for (int f = 0; f < cs2::fontFaceCount(font); ++f) {
            if (!cs2::fontRenderable(font, f)) continue;
            bool jp = false;
            for (const auto& n : cs2::fontFaceNames(font, f)) jp |= n.find(" JP") != std::string::npos;
            if (jp) {
                fontFace = f;
                break;
            }
        }
    }
    StageRenderer::Options ro;
    ro.virtualWidth = cfg.width;
    ro.virtualHeight = cfg.height;
    ro.windowColor = cfg.windowColor;
    ro.textAround = cfg.textAround;
    ro.textRightDown = cfg.textRightDown;
    ro.textureBudgetBytes = options.textureBudgetBytes;
    ro.theme = &game_->windowTheme();
    ro.fontFace = fontFace;
    ro.userFontImage = game_->userFontImage();
    ro.fontLoader = [this](const std::string& name, int32_t weight, int& face, bool& fakeBold) {
        return game_->loadFont(&face, name, weight, &fakeBold);
    };
    ro.userFontGlyphs = game_->userFontGlyphs();
    const auto& lay = game_->player().layout();
    // A plane vram.xml leaves out starts at base and position 0 (ScenePlayer::plane).
    ro.planeOffset = lay.planes.empty() ? std::pair{lay.planePosX - lay.planeBaseX, lay.planePosY - lay.planeBaseY} : std::pair{0, 0};
    for (const auto& [i, d] : lay.planes) {
        ro.planeOffsets[i] = {d.posX - d.baseX, d.posY - d.baseY};
        ro.planeVram[i] = {d.vramX, d.vramY, d.width, d.height};
    }
    ro.imageShift = lay.imageShift;
    // Save thumbnails: "@save:<slot>:<version>".
    ro.externalTexture = [this](const std::string& name, int& w, int& h, Bytes& rgba) {
        if (!name.starts_with("@save:")) return false;
        const cs2::SaveFile* f = game_->saves().get(std::atoi(name.c_str() + 6));
        if (!f || f->thumbnail.empty()) return false;
        w = f->thumbWidth;
        h = f->thumbHeight;
        rgba = f->thumbnail;
        return true;
    };
    renderer_ = std::make_unique<StageRenderer>(game_->assets(), std::move(font), ro);
    game_->setBankCapture([this](const cs2::StageImage& img) { return renderer_->captureImage(img, game_->player().frame()); });
    game_->setTextMeasure([this](const cs2::FesQuad& q) { return renderer_->textWipeLength(q); });
    game_->setTextLines([this](const cs2::FesQuad& q) { return renderer_->textLineCount(q); });
    game_->setTextArea([this](const cs2::FesQuad& q) { return renderer_->textDrawArea(q); });
    game_->setSaveDirectory(options.saveDirectory);
    game_->boot();
    last_ = std::chrono::steady_clock::now();
}

void GameSession::systemRequest(int32_t n) {
    auto log = [&](const std::string& m) {
        if (game_->player().log) game_->player().log(m);
    };
    // `send 0xffff0003 0 <n>` from the message window: the engine's story commands.
    switch (n) {
    case 3: windowHidden_ = true; break;
    case 15: storyAction(KeyMap::MessageLog, true); break;
    case 18:
    case 29: game_->replayVoice(); break;  // replay voice (29: newer engines' "CV" button)
    case 13: skipping_ = !skipping_; break;
    case 14: autoRequest(); break;
    case 16: storyAction(KeyMap::NextOption, true); break;
    case 17: storyAction(KeyMap::PrevOption, true); break;
    case 21: storyAction(KeyMap::QuickSave, true); break;
    case 22: storyAction(KeyMap::QuickLoad, true); break;
    case 23:
        // The window's settings button: sscript's config request (its own state, apart from save
        // and load) opens the game's config script (Love's Sweet Garnish's Game Settings; Fruit's
        // Menu). Right-click stays the save/load script.
        skipping_ = jumping_ = false;
        captureThumbnail();
        game_->openMenu("config");  // through sscript's fade (state 19)
        break;
    case 19:
    case 20:
        // Save / Load buttons.
        skipping_ = jumping_ = false;
        captureThumbnail();
        game_->openSaveMenu(n == 20);  // through sscript's fade (state 7)
        break;
    case 100:
    case 101:
    case 102:
    case 103:
        // The window's extra buttons (meswnd.xml <btn><exN>): Amakano 2's Amakano Graph.
        skipping_ = jumping_ = false;
        if (!game_->openExtraButton(n - 100)) log("extra button " + std::to_string(n - 100) + ": no script");
        break;
    // The window script's `_SYS_*_FORCE` labels: the same actions, asked for regardless of the
    // buttons' state.
    case 1000: systemRequest(19); break;
    case 1001: systemRequest(20); break;
    case 1002: systemRequest(21); break;
    case 1003: systemRequest(22); break;
    case 1004: systemRequest(23); break;
    case 1005: systemRequest(15); break;
    default: log("system request " + std::to_string(n) + " not supported yet"); break;
    }
}

void GameSession::autoRequest() {
    // sscript's Auto request (14; Yukikoi 0x0ed7c5): starts auto; while it runs, with adv.xml
    // etc/automag on it speeds auto up (Game::stepAutoLevel), else it stops auto.
    if (!auto_) {
        auto_ = true;
    } else if (game_->autoMagEnabled()) {
        game_->stepAutoLevel(true);
    } else {
        auto_ = false;
        game_->autoStoppedByRequest();
    }
}

void GameSession::screenModes() {
    using K = cs2::ScenePlayer::ScreenKind;
    auto& p = game_->player();
    if (const K k = p.takeScreenOpened(); k != K::None) {
        // The keywords put sscript in another main state (Yukikoi: `fes` / `fesjs` 0x078bfb /
        // 0x078d67 -> state 14, `fex` 0x07d533 -> 20 for `sel`, else 21), which ends Skip to Next
        // Option. State 14 turns skip mode and auto off as it opens the screen (0x0d5a8b; Fruit
        // 0x096d84, Nekopara 0x0cc9bb, the same in every sscript) and gives neither back; `fex`
        // keeps them first (skip m[0x84454] at 0x07d77c, auto m[0x2ba38] at 0x0dfb44).
        if (k != K::Fes) {
            fexSkip_ = skipping_;
            fexAuto_ = auto_;
            fexAutoLevel_ = game_->autoLevel();
        }
        skipping_ = jumping_ = false;
        if (auto_) {
            game_->autoStoppedByRequest();  // no MES_SETMAG: the window hears MES_PUSH 0 0
            auto_ = false;
        }
    }
    if (const K k = p.takeScreenClosed(); k == K::FexSel || k == K::Fex) {
        // The returns (`fex sel` 0x0dfdb8, other `fex` 0x0e0423): auto as it was, after `fex sel`
        // only with contauto on (else off, level 1); skip as it was only with contskip on and the
        // text read (0x0ae251) or fullskip (config fields 24, 52, 32; Nekopara's setconf 0x77d770
        // names them).
        if (fexAuto_ && (k == K::Fex || game_->conf("contauto", 1))) {
            game_->resumeAuto(fexAutoLevel_);
            auto_ = true;
        }
        skipping_ = fexSkip_ && game_->conf("contskip", 0) && (game_->conf("fullskip", 0) || game_->messageRead());
        fexSkip_ = fexAuto_ = false;
    }
}

void GameSession::step() {
    using State = cs2::ScenePlayer::State;
    auto& p = game_->player();
    for (const auto& s : game_->windowScript().takeSends()) {
        if (s.code == 0xffff0003) systemRequest(s.b);
    }
    // A screen the scene runs (Amakano 2's map) asks for the same things: its
    // Save / Load / System buttons are `_SYS_SAVE_FORCE` etc. (`send 0xffff0003 0 1000`).
    if (auto* ov = p.overlay()) {
        for (const auto& s : ov->takeSends()) {
            if (s.code == 0xffff0003) {
                systemRequest(s.b);
            } else if (s.code == 0x503) {
                // `_SYS_FEXSEL_CAPTURE` (Amakano sscript 0x0f3c4b): the whole screen as it is now, the
                // screen itself included (native 113), for the save that follows.
                thumbnailHeld_ = false;
                captureThumbnail();
                thumbnailHeld_ = true;
            }
        }
    }
    bool menuOpen = game_->systemScreen() != nullptr || game_->menuFading();
    p.setDrama(auto_ && game_->conf("motcut", 0) != 0);  // ScenePlayer::setDrama
    if (game_->loading()) skipping_ = jumping_ = auto_ = false;  // sscript: a load ends skip and auto
    if (game_->touchMode()) skipping_ = jumping_ = false;      // touch mode leaves the story mode
    stepJump();
    if (p.takeJumpStop()) jumping_ = false;  // a new scene: Skip to Next Option stops
    if (p.takeSkipStop()) skipping_ = false;  // `skipstop`
    p.setJumping(jumping_);
    game_->setModes(skipping_ || jumping_, auto_, forceSkip_);
    // Skip mode passes only text read before, unless "skip unread" (fullskip) is on; Skip to Next
    // Option always stops at unread text (sscript 0x091e99 checks the read flag alone).
    if ((skipping_ || jumping_) && p.state() == State::Input && !game_->messageRead() &&
        (jumping_ || !game_->conf("fullskip", 0))) {
        skipping_ = jumping_ = false;
    }
    bool fast = (skipping_ || forceSkip_ || jumping_) && !menuOpen;
    p.setSkipping(fast);  // the engine's skip flag: classic motions end at once
    // Force skip (held key) is skip mode while held: the window hears MES_MODE on press and release.
    game_->windowScript().setMode(skipping_ || jumping_ || (forceSkip_ && !menuOpen) ? 2 : auto_ ? 1 : 0);
    {
        // Skip and Skip-to-next-choice only pass text read before, unless "skip unread" is on.
        bool readOk = game_->messageRead() || game_->conf("fullskip", 0) != 0;
        cs2::WindowScript::Availability a;
        // sscript (Amakano 0x0ad387 / 0x0ad5fa, Fruit 0x0802d7): skip needs read text or "skip
        // unread"; Next Option needs read text.
        a.skip = readOk || skipping_;
        a.next = game_->messageRead();
        a.jumping = jumping_ || jumpPhase_ != JumpPhase::None;
        a.choice = p.state() == State::Choice;
        a.replay = game_->replaying();
        a.prev = game_->hasLastChoice();
        a.voice = game_->currentHasVoice();
        a.quickLoad = game_->hasQuickSave();
        a.buttons = game_->player().systemButtons();
        game_->windowScript().setAvailability(a);
    }
    {
        // `layout scrolltype 1`: the window reveals by a wipe, timed by the renderer.
        const auto tl = game_->windowScript().textLayout();
        p.setWipe(tl.scrollType == 1 && tl.wipeWidth > 0);
        cs2::ScenePlayer::EmoteSettings es{game_->conf("m2 motion", 1) != 0, game_->conf("m2 eff_dif", 1) != 0,
                                           game_->conf("m2 eff_blink", 1) != 0, game_->conf("m2 eff_lip", 1) != 0};
        es.iner = game_->conf("m2 eff_iner", 1) != 0;
        es.inerLevel = game_->confGroup("m2 iner_lv ");
        es.inerEnable = game_->confGroup("m2 iner_enable ");
        p.setEmoteSettings(es);
        p.setContVoice(game_->conf("contvoice", 1) != 0);
        // Only a wipe the renderer drew for this message ends it (a glyph-by-glyph window never
        // does: the scene player times it).
        if (p.wipe() && renderer_->wipeDone(p.stage().window.wipeStart)) p.finishWipe();
    }
    {
        // The window string's `layout frame` as its script last set it (MES_SCROLLSPD keeps it at
        // the window's speed; -1 means messpd, grisaia.exe 0x5cb7b0), else the window's speed.
        const auto layout = game_->windowScript().textLayout();
        const int32_t lf = layout.frame;
        const int32_t speed = lf >= 0 ? lf : lf == -1 ? game_->conf("messpd", 4) : game_->messageFrame();
        p.setTextTiming(speed, game_->strFrameTenths(), fast);
        p.setTextMag(layout.mag);  // `str mag`: 20 in Grisaia's windows while auto runs sped up
    }
    if (fast) p.setWipe(false);  // skipping shows text at once
    // A click since the last step (or a pick), and auto when it is due, move the story on here,
    // before the tick: in sscript the click's request (code 2, Nekopara 0x0dbd9c) and the auto
    // deadline (0x0865e7) only set the advance flag m[0x8000009c], which the input wait reads in the
    // tick that then runs the next lines, their name line's MES_SETNAME included, before the frame
    // is drawn. Running a click at once and auto after the tick (as before) drew a frame or two of
    // the new message under the last speaker.
    if (choicePending_ >= 0) {
        const auto index = static_cast<size_t>(std::exchange(choicePending_, -1));
        if (p.state() == State::Choice) p.choose(index);
    } else if (std::exchange(advancePending_, false)) {
        if (p.state() != State::Choice) p.advance();
    } else if (auto_ && !menuOpen && !game_->touchMode() && (p.state() == State::Input || p.state() == State::WaitKey) &&
               game_->autoDue()) {
        p.advance();
    }
    game_->tick();
    screenModes();
    // A pick on the game's own choice screen goes the same way as a click on our choice buttons.
    if (int32_t c = game_->takeChoice(); c >= 0 && p.state() == State::Choice) choose(static_cast<size_t>(c));
    renderer_->setChoicesHidden(game_->choiceScreenOpen());
    State st = p.state();
    if (fast) {
        // Skip to Next Option runs many lines per frame; skip modes one per skip_speed interval.
        int bursts = jumping_ ? 40 : 1;
        if (!jumping_ && skipWait_ > 0) {
            --skipWait_;
            bursts = 0;
        } else if (!jumping_) {
            skipWait_ = game_->skipInterval();
        }
        for (int i = 0; i < bursts; ++i) {
            st = p.state();
            // A screen the scene runs plays at its own pace: opening it ended skip mode and the jump
            // (screenModes), and a held Force Skip it hears itself (FesInstance::tick).
            if (st == State::Choice || st == State::Ended || st == State::Fes) break;
            if (st == State::Input && !game_->messageRead() && (jumping_ || (!game_->conf("fullskip", 0) && !forceSkip_))) {
                skipping_ = jumping_ = false;  // unread text: reading resumes here
                break;
            }
            p.advance();
            if (i + 1 < bursts) {
                game_->tick();
                screenModes();
                if (!skipping_ && !jumping_ && !forceSkip_) break;
            }
        }
        st = p.state();
    }
    if (jumping_ && (st == State::Choice || st == State::Ended || !game_->sceneRunning() ||
                     (st == State::Fes && p.fexScreen()))) {
        jumping_ = false;
        if (p.log) p.log(std::string("next option: stopped (") + cs2::toString(st) + ") in " + p.sceneName() + " at frame " + std::to_string(p.frame()));
    }
    if (game_->takeAutosaveRequest()) {
        captureThumbnail();
        game_->autosave();
    }
    // A movie that couldn't start has already let the story go on; one playing holds it in Movie.
    if (!game_->flow() && p.state() == State::Ended) game_->startNewGame();  // no flow script: loop the story
    // Skip's bursts ran lines after the tick: the window hears them now, and the renderer takes the
    // window's state (speaker, colours) as the frame will show it.
    game_->syncWindowEvents();
    syncRenderer();
}

void GameSession::syncRenderer() {
    auto& p = game_->player();
    // The message window steps aside while the backlog is up.
    renderer_->setWindowHidden((windowHidden_ && game_->sceneRunning()) || game_->logOpen() || game_->touchMode() ||
                               game_->menuHidesWindow() || p.dramaVoice());
    renderer_->setTextLayout(game_->windowScript().textLayout());
    renderer_->setWindowName(game_->windowScript().nameText(), game_->windowScript().nameAlign());
    {
        bool subFixed = true;
        std::string sf = game_->windowScript().subFont(&subFixed);
        renderer_->setSubText(game_->windowScript().subLayout(), std::move(sf), !subFixed);
    }
    renderer_->setMessageSpeed(game_->messageFrame());
    renderer_->setEffectSpeed(game_->conf("effspd", 0));
    renderer_->setMplFaceFrom(p.mplFaceFrom());
    renderer_->setFacesShown(p.stage().window.visible && !windowHidden_ && !game_->touchMode() && !game_->menuHidesWindow() &&
                             !p.dramaVoice());
    renderer_->setTextColors(game_->windowScript().textColor(), game_->windowScript().nameColor(),
                            game_->windowScript().textEdgeColor(), game_->windowScript().nameEdgeColor());
    {
        bool textFixed = true, nameFixed = true;
        std::string tf = game_->windowScript().textFont(&textFixed), nf = game_->windowScript().nameFont(&nameFixed);
        renderer_->setTextFonts(std::move(tf), !textFixed, std::move(nf), !nameFixed);
        renderer_->setTextWeight(game_->textWeight());
        renderer_->setTextShadow(game_->textAround(), game_->textRightDown());
    }
}

void GameSession::frame(int width, int height, GLuint target) {
    auto now = std::chrono::steady_clock::now();
    accumulator_ += std::chrono::duration<double>(now - last_).count();
    last_ = now;
    int steps = static_cast<int>(accumulator_ * 60.0);
    accumulator_ -= steps / 60.0;
    // After a stall (app in background, slow load) don't try to catch up.
    if (steps > 4) {
        steps = 4;
        accumulator_ = 0;
    }
    for (int i = 0; i < steps; ++i) step();
    renderFrame(width, height, target);
}

void GameSession::renderFrame(int width, int height, GLuint target) {
    if (auto* mv = game_->movie()) {
        renderer_->renderMovie(mv->rgba().data(), mv->width(), mv->height(), mv->changed, width, height, target);
        mv->changed = false;
        return;
    }
    auto& p = game_->player();
    std::vector<cs2::FesQuad> overlay;
    // `baseimg`: the picture the scripts' screen keeps under them (title to config and back).
    // Fitted: drawn only while no story runs, and dropped as the story starts and ends
    // (Game::execKcs / endKcs). Cyanotype's prologue starts with the splash's white still set
    // and shows on PC; where the screen draws it beside the story is not traced.
    if (!game_->sceneRunning()) overlay = game_->baseImage();
    if (game_->sceneRunning()) {
        for (const auto& r : game_->residents()) {  // the resident scripts (adv.xml regident)
            if (!r) continue;
            auto q = r->draw(p.frame());
            overlay.insert(overlay.end(), q.begin(), q.end());
        }
        for (const auto& d : p.displayScripts()) {  // `fesds` slots (date and place cards)
            if (!d) continue;
            auto q = d->draw(p.frame());
            overlay.insert(overlay.end(), q.begin(), q.end());
        }
    }
    if (p.overlay()) {
        auto q = p.overlay()->draw(p.frame());
        overlay.insert(overlay.end(), q.begin(), q.end());
    }
    if (auto* flow = game_->flow()) {
        auto screens = flow->draw(p.frame());  // title/menu screens over the story
        overlay.insert(overlay.end(), screens.begin(), screens.end());
    }
    std::vector<cs2::FesQuad> top;
    if (auto* sys = game_->systemScreen()) top = sys->draw(p.frame());
    renderer_->setScreenFade(std::max({game_->screenFade(), jumpFade(), game_->menuFade()}));
    renderer_->setStoryHidden(game_->menuHidesStory());
    auto& ws = game_->windowScript();
    if (ws.active() && game_->sceneRunning()) {
        float a = std::clamp(p.stage().window.alpha.value(p.frame()), 0, 255) / 255.0f;
        auto window = windowHidden_ || game_->logOpen() ? std::vector<cs2::FesQuad>{} : ws.draw(p.frame(), a);
        renderer_->setSideQuads(windowHidden_ || game_->logOpen() ? std::vector<cs2::FesQuad>{} : ws.drawSide(p.frame()));
        renderer_->setWindowTextLayer(ws.textLayer());
        renderer_->render(p.stage(), p.frame(), width, height, target, overlay, &window, &top);
        if (auto end = renderer_->novelTextEnd()) ws.setTextEnd(end->first, end->second);
    } else {
        renderer_->setSideQuads({});
        renderer_->render(p.stage(), p.frame(), width, height, target, overlay, nullptr, &top);
    }
}

std::optional<Session::Dialog> GameSession::dialog() {
    auto* screen = activeScreen();
    const cs2::FesInstance::Dialog* d = screen ? screen->dialog() : nullptr;
    if (!d) return std::nullopt;
    return Dialog{d->serial, d->input, d->title, d->message, d->value, d->maxChars, d->type, d->cancel};
}

void GameSession::answerDialog(bool ok, const std::string& valueUtf8) {
    if (auto* screen = activeScreen()) screen->answerDialog(ok, valueUtf8);
}

cs2::FesInstance* GameSession::activeScreen() {
    if (auto* sys = game_->systemScreen()) return sys;
    if (!game_->sceneRunning() && game_->flow()) return game_->flow();
    return game_->player().overlay();
}

const Session::Cursor* GameSession::cursor() {
    const cs2::CursorImage* c = game_->cursor();
    if (!c) return nullptr;
    if (c != cursorFrom_) {
        cursorFrom_ = c;
        cursor_ = Cursor{cursor_.serial + 1, c->width, c->height, c->argb, c->hotX, c->hotY};
    }
    return &cursor_;
}

void GameSession::pointerMove(float x, float y) {
    renderer_->toVirtual(x, y, mouseVX_, mouseVY_);
    if (auto* screen = activeScreen()) {
        screen->pointerMove(mouseVX_, mouseVY_);
        return;
    }
    if (!windowHidden_) game_->windowScript().pointerMove(mouseVX_, mouseVY_);
    renderer_->setHoveredChoice(renderer_->choiceAt(game_->player().stage(), mouseVX_, mouseVY_));
}

void GameSession::mouseButton(int button, bool down, float x, float y) {
    pointerMove(x, y);
    bool right = button == KeyMap::RightButton;
    if (auto* mv = game_->movie()) {
        // A movie takes the clicks: its mode says which end it (2 left, 1 right).
        if (!down && (mv->skipMode & (right ? 1 : 2))) game_->endMovie();
        return;
    }
    if (auto* screen = activeScreen()) {
        // FE screens get the buttons as such; a left press also raises the script's click flag,
        // which hurries its waits (keyskip; FesInstance::mouseDown). A press that is cancelled (it
        // became a touch gesture) never releases and takes its flag back.
        if (down) screen->mouseDown(mouseVX_, mouseVY_, right);
        else screen->mouseUp(mouseVX_, mouseVY_, right);
        return;
    }
    if (!game_->sceneRunning() || game_->menuFading()) return;  // a menu's fade takes no clicks
    // The message window's own buttons (Skip, Auto, ...) take the click before the story does.
    auto& ws = game_->windowScript();
    if (!windowHidden_ && (down ? ws.mouseDown(mouseVX_, mouseVY_, right) : ws.mouseUp(mouseVX_, mouseVY_, right))) {
        return;
    }
    if (game_->touchMode()) return;  // touch.fes has the clicks (its full-screen button)
    if (!right && !down && game_->player().state() == cs2::ScenePlayer::State::Choice && !windowHidden_ &&
        !game_->choiceScreenOpen()) {
        int idx = renderer_->choiceAt(game_->player().stage(), mouseVX_, mouseVY_);
        if (idx >= 0) choose(static_cast<size_t>(idx));
        return;
    }
    // Story actions bound to mouse buttons happen on the click (release); a cancelled press does nothing.
    if (down) {
        mousePending_ = button;
        return;
    }
    if (mousePending_ != button) return;
    mousePending_ = 0;
    storyInput(button, true, true);
    storyInput(button, true, false);
}

namespace {
// Scroll actions of the key table, as FE scripts see them (_SCRL_U_, _SCRL_D_, _PAGE_U_, _PAGE_D_).
void scrollScreen(cs2::FesInstance& screen, const KeyMap& km, int code, bool mouse) {
    static const KeyMap::Action actions[4] = {KeyMap::ScrollUp, KeyMap::ScrollDown, KeyMap::PageUp, KeyMap::PageDown};
    for (int k = 0; k < 4; ++k) {
        if (mouse ? km.mouseSlot(actions[k], code) : km.keySlot(actions[k], code)) screen.scroll(k);
    }
}
}  // namespace

void GameSession::wheel(int steps) {
    int code = steps > 0 ? KeyMap::WheelUp : KeyMap::WheelDown;
    if (auto* screen = activeScreen()) {
        for (int i = 0; i < std::abs(steps); ++i) scrollScreen(*screen, game_->keyMap(), code, true);
        return;
    }
    if (!game_->sceneRunning() || game_->touchMode()) return;
    for (int i = 0; i < std::abs(steps); ++i) {
        storyInput(code, true, true);
        storyInput(code, true, false);
    }
}

void GameSession::key(int vk, bool down) {
    const auto& km = game_->keyMap();
    // Releasing the Force Skip key ends it wherever input is going now: it may have been pressed in
    // the story before a menu or screen came up (or the story ended) while it was held.
    if (!down && km.keySlot(KeyMap::ForceSkip, vk)) forceSkip_ = false;
    if (auto* screen = activeScreen()) {
        // The FE scripts ask whether the Force Skip key is held each update (FesInstance::tick),
        // whatever screen is up.
        if (down && km.keySlot(KeyMap::ForceSkip, vk)) forceSkip_ = true;
        if (!down) return;
        auto& s = screen->top();
        // Confirm is a press for the click flag (keyskip); Cancel is the right button (_CLICK_R_);
        // scroll keys scroll.
        if (km.keySlot(KeyMap::Confirm, vk)) s.pressSkip();
        else if (km.keySlot(KeyMap::Cancel, vk)) s.click(mouseVX_, mouseVY_, true);
        scrollScreen(*screen, km, vk, false);
        return;
    }
    if (game_->sceneRunning() && !game_->touchMode()) storyInput(vk, false, down);
}

void GameSession::storyInput(int code, bool mouse, bool down) {
    // The story's actions, in the engine's table order; the binding with the best slot wins
    // (e.g. Esc/right button: Options In Game, slot 1, over Hide Message Window, slot 2).
    static const std::vector<KeyMap::Action> story = {
        KeyMap::AdvanceText, KeyMap::HideWindow, KeyMap::ForceSkip, KeyMap::SkipMode,
        KeyMap::AutoMode, KeyMap::MessageLog, KeyMap::NextOption, KeyMap::PrevOption,
        KeyMap::ReplayVoice, KeyMap::QuickSave, KeyMap::QuickLoad, KeyMap::OptionsInGame,
        KeyMap::AutoSpeedUp, KeyMap::AutoSpeedDown,
    };
    // sscript gets the clicks before the key table (requests 2 left, 1 right; 0x0dbbdd): in the
    // extras' viewer (boot -5) a right click goes to its `cancellabel` scene (0x0dbcd0).
    if (mouse && down && code == KeyMap::RightButton && game_->cancelViewer()) return;
    int action = game_->keyMap().pick(story, code, mouse);
    if (action < 0) return;
    // A hidden window comes back with the next press of a reading key; nothing else happens.
    if (windowHidden_ && down &&
        (action == KeyMap::AdvanceText || action == KeyMap::HideWindow || action == KeyMap::OptionsInGame)) {
        windowHidden_ = false;
        return;
    }
    storyAction(action, down);
}

void GameSession::storyAction(int action, bool down) {
    auto& p = game_->player();
    if (action == KeyMap::ForceSkip) {
        forceSkip_ = down;
        return;
    }
    // Nothing reads input while a load or Skip to Next Option fades.
    if (!down || game_->loading() || game_->menuFading() || jumpPhase_ != JumpPhase::None) return;
    auto log = [&](const std::string& m) {
        if (p.log) p.log(m);
    };
    switch (action) {
    case KeyMap::AdvanceText:
        // sscript's handler for the advance action (request 2, Nekopara 0x0dbd9c; the action is
        // KEYCUSTOMIZE key02, a click or Enter): while skip runs it only stops skip; while auto runs
        // it stops auto, unless the autoleft setting is on (m[0x800059b0], from the config block at
        // 0x0a64a1), when the text moves on and auto keeps going. A click that stops a mode does
        // not advance. In Nekopara the window's buttons are all off in auto (its SETBUTTON's
        // `btn enable 0`), so a click on the auto button comes here too.
        if (skipping_ || jumping_) {
            skipping_ = jumping_ = false;
            return;
        }
        if (auto_ && !game_->conf("autoleft", 0)) {
            auto_ = false;
            return;
        }
        if (p.state() != cs2::ScenePlayer::State::Choice) advance();
        break;
    case KeyMap::HideWindow:
        if (p.stage().window.visible) windowHidden_ = true;
        break;
    case KeyMap::SkipMode: skipping_ = !skipping_; break;
    case KeyMap::AutoMode: autoRequest(); break;
    case KeyMap::AutoSpeedUp:
    case KeyMap::AutoSpeedDown:
        // Requests 24 / 25 (Yukikoi's sscript 0x0ee3a1 / 0x0ee433): with adv.xml etc/automag, while
        // auto runs, its level doubles or halves. Their default keys F4 / Z are Auto's too, which
        // wins them (the earlier action); F5 / X slow auto down.
        if (auto_ && game_->autoMagEnabled()) game_->stepAutoLevel(action == KeyMap::AutoSpeedUp);
        break;
    case KeyMap::NextOption:
        if (p.state() != cs2::ScenePlayer::State::Choice && jumpPhase_ == JumpPhase::None) {
            skipping_ = auto_ = false;
            jumpPhase_ = JumpPhase::Hold;
            jumpFrame_ = 0;
        }
        break;
    case KeyMap::PrevOption:
        skipping_ = jumping_ = false;
        game_->returnToLastChoice();
        break;
    case KeyMap::QuickSave:
        captureThumbnail();
        if (game_->quickSave()) game_->windowScript().quickSaved();
        break;
    case KeyMap::QuickLoad:
        skipping_ = jumping_ = false;
        game_->quickLoad();
        break;
    case KeyMap::OptionsInGame:
        // sscript opens the save/load script in game; it captures the save object and shows the
        // game's menu (directly in some games, via config.fes in others).
        skipping_ = jumping_ = false;
        captureThumbnail();
        game_->openSystemScreen("saveload");
        break;
    case KeyMap::ReplayVoice: game_->replayVoice(); break;
    case KeyMap::MessageLog:
        skipping_ = jumping_ = false;
        game_->openLog();
        break;
    default: break;
    }
}

void GameSession::pointerUp(float x, float y) {
    mouseButton(KeyMap::LeftButton, true, x, y);
    mouseButton(KeyMap::LeftButton, false, x, y);
}

void GameSession::stepJump() {
    switch (jumpPhase_) {
    case JumpPhase::None:
        return;
    case JumpPhase::Hold:
    case JumpPhase::Out:
    case JumpPhase::In:
        if (++jumpFrame_ < kJumpFadeFrames) return;
        jumpFrame_ = 0;
        if (jumpPhase_ == JumpPhase::Hold) {
            jumpPhase_ = JumpPhase::Out;
        } else if (jumpPhase_ == JumpPhase::Out) {
            jumpPhase_ = JumpPhase::Run;
            jumping_ = true;
        } else {
            jumpPhase_ = JumpPhase::None;
        }
        return;
    case JumpPhase::Run:
        // The run ends where Skip to Next Option stops (a choice, unread text, a `jumpstop`).
        if (!jumping_) jumpPhase_ = JumpPhase::In;
        return;
    }
}

float GameSession::jumpFade() const {
    float t = static_cast<float>(jumpFrame_) / static_cast<float>(kJumpFadeFrames);
    switch (jumpPhase_) {
    case JumpPhase::Out: return t;
    case JumpPhase::Run: return 1.0f;
    case JumpPhase::In: return 1.0f - t;
    default: return 0.0f;
    }
}

void GameSession::captureThumbnail() {
    // A screen took the picture already (`_SYS_FEXSEL_CAPTURE`, before it fades itself out): the
    // save uses that one (sscript keeps it in m[0x84530], native 113).
    if (std::exchange(thumbnailHeld_, false)) return;
    int w = game_->thumbnailWidth(), h = game_->thumbnailHeight();
    game_->setThumbnail(w, h, renderer_->captureThumbnail(w, h));
}

void GameSession::cancelPress() {
    mousePending_ = 0;
    if (auto* screen = activeScreen()) screen->cancelPress();
    game_->windowScript().cancelPress();
}

void GameSession::advance() {
    advancePending_ = true;  // run by the next step, before its tick (see step())
}

void GameSession::releaseHeld() {
    forceSkip_ = false;
}

void GameSession::suspend() {
    // The app is closing or going to the background: the story's place is kept as an autosave (the
    // title's Continue picks it up, as after quitting the original), and global data is written.
    if (game_->sceneRunning() && !game_->systemScreen()) {
        captureThumbnail();
        game_->autosave();
    }
    game_->saveGlobal();
}

bool GameSession::returnToTitle() {
    if (!game_->sceneRunning()) return false;
    skipping_ = jumping_ = auto_ = false;
    jumpPhase_ = JumpPhase::None;
    captureThumbnail();
    game_->autosave();
    game_->retTitle();
    return true;
}

bool GameSession::openSettings() {
    // The original menu bar's settings open the engine's own Windows dialogs (cs2conf); the game's
    // in-game system menu, where its options live, is what stands in (the options key's action).
    if (!game_->sceneRunning() || game_->systemScreen()) return false;
    storyAction(KeyMap::OptionsInGame, true);
    return game_->systemScreen() != nullptr;
}

std::vector<std::string> GameSession::languages() const {
    std::vector<std::string> names;
    for (const auto& l : game_->languages()) names.push_back(l.name);
    return names;
}

void GameSession::setLanguage(int index) {
    game_->setLanguage(index);
    game_->saveGlobal();
}

void GameSession::choose(size_t index) {
    renderer_->setHoveredChoice(-1);
    // contskip / contauto: whether skip and auto carry on past a choice.
    if (!game_->conf("contskip", 0)) skipping_ = false;
    if (!game_->conf("contauto", 1)) auto_ = false;
    choicePending_ = static_cast<int64_t>(index);  // as a click: the next step makes it
}

}  // namespace nyan::render
