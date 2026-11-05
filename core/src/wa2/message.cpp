#include "nyan/wa2/message.hpp"

#include <algorithm>
#include <cstdio>

#include "nyan/common/file_source.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// Objects and slots of the window (0x402cd0, 0x40e420, 0x402890).
constexpr int kWindowBack = 0x40d, kWindowRing = 0x40e, kButton0 = 0x40f, kBar = 0x418, kTip = 0x419;
constexpr int kNovelBack = 0x41d, kIconRead = 0x41e, kIconAuto = 0x41f, kIconSkip = 0x420, kOnAuto = 0x43d, kOnSkip = 0x43e;
constexpr int kText = 0x443, kName0 = 0x444, kCursorEnd = 0x452, kCursorMore = 0x453;
constexpr int kBacklogShade = 0x41c, kBacklogText = 0x446, kBacklogName = 0x44a, kBacklogIcon = 0x435;
constexpr int kChoiceBar = 0x454, kChoiceText = 0x474;
constexpr int kAtlas = 0x1c4;
constexpr int kTextX = 274, kTextY = 536, kNovelX = 75, kNovelY = 26;
// The ani slots of the page cursors (page_02 = the end, page_01 = more after a `\k`).
constexpr int kAniEnd = 0xf1, kAniMore = 0xf2;
// Colours (0x4be468.., 0x4be48c..): all text white; backlog hover 0xfff799; outline 0x212e38;
// choices normal / hover / disabled.
constexpr uint32_t kChoiceNormal = 0xffffff, kChoiceHover = 0xfff799, kOutline = 0x212e38, kChoiceGrey = 0xa0a0a0;
// The button rectangles (0x4be3c8): Q.SAVE Q.LOAD OPTION SAVE LOAD BACKLOG AUTO SKIP WINDOW OFF.
constexpr int kButtonX[9] = {808, 848, 928, 968, 1008, 1048, 1088, 1128, 1168};
constexpr int kButtonY = 680, kButtonSize = 40;
constexpr int kBacklogButton = 5, kAutoButton = 6, kSkipButton = 7, kHideButton = 8;
// WN(n)'s names (0x4be348), Japanese.
constexpr const char* kNames[32] = {"春希",     "かずさ",   "雪菜",     "",         "",       "",         "",     "",
                                    "",         "",         "武也",     "親志",     "依緒",   "小木曽父", "小木曽母", "孝宏",
                                    "曜子",     "担任教師", "諏訪",     "",         "ドルウク", "ガイウス", "クレオン", "クレオン",
                                    "レクトール", "兵士",   "",         "",         "",       "",         "ミルディン", "プィル"};
// Speaker names that stand for a group (0x4a1e64..0x4a1f1c): with these, or any ASCII space, the line
// keeps the last played voice in the backlog.
constexpr const char* kGroupNames[] = {"みんな", "全員", "７人", "ジーニアス", "列席者", "クラス", "女子達",
                                       "男子達", "女性陣", "４人", "四人", "五人", "六人", "お妃候補",
                                       "男子多数", "客", "野球部員一同", "一同", "スタッフ一同", "男女"};

int triangle(int n, int p) {
    // 0x44eb70
    n %= p * 2;
    return n >= p ? p * 2 - n : n;
}

}  // namespace

Message::Message(Game& game) : game_(game), gfx_(game.gfx()) {}

void Message::begin() {
    // 0x40a3a0: the page cursors and the window's pictures.
    auto& ani = game_.ani();
    ani.load(kAniEnd, "page_02.ani", 2, "", "", 0x100, "");
    ani.load(kAniMore, "page_01.ani", 2, "", "", 0x100, "");
    auto sys = [&](int slot, const char* fmt, uint32_t flags) {
        char name[32];
        std::snprintf(name, sizeof name, fmt, config.winColor);
        if (!gfx_.load(slot, name, flags, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
    };
    sys(0x107, "sys_%01d0000.tga", 1);
    sys(0x108, "sys_%01d0001.tga", 1);
    sys(0x10a, "sys_%01d0110.tga", 1);
    sys(0x10b, "sys_%01d0010.tga", 1);
    sys(0x10c, "sys_%01d0011.tga", 1);
    sys(0x10e, "sys_%01d0030.tga", 1);
    sys(0x10f, "sys_%01d0031.tga", 1);
    sys(0x110, "sys_%01d0032.tga", 1);
    sys(0x111, "sys_%01d0033.tga", 1);
    sys(0x112, "sys_%01d9000.tga", 1);
    sys(0x113, "sys_%01d9100.tga", 1);
    gfx_.load(0x114, "sys_01013.tga", 2, false, "", "", 0x100, "grp");
    gfx_.load(0x116, "sys_00301.tga", 2, false, "", "", 0x100, "grp");
    gfx_.load(0x117, "sys_00300.tga", 2, false, "", "", 0x100, "grp");
    sys(0x12a, "sys_%01d0200.tga", 2);
}

void Message::reset() {
    // 0x4032f0: the backlog; and no window.
    log_.clear();
    logScroll_ = 0;
    exists_ = false;
    state_ = 0;
    name_.clear();
    logName_.clear();
    nameShown_ = nameChanged_ = false;
    nameCount_ = 0;
    speaker_ = -1;
    novel_ = 0;
    novelFading_ = false;
    game_.sound().voiceSpeaker = -1;
}

int Message::colorIndex(int speaker) const {
    // 0x4027a0
    switch (speaker) {
    case 0: case 1: case 2: case 3: case 4: case 5: return speaker;
    case 10: case 12: case 13: case 14: case 15: case 16: case 18: case 19: case 22: case 25: case 28: case 29:
    case 30: case 32: case 33: case 34: case 38: case 46: case 50: case 51: case 52: case 53: case 54: case 55:
    case 56: case 57: case 98:
        return 6;
    case 100: return 8;
    case -1: return 9;
    default: return 7;
    }
}

// ---- Script side -------------------------------------------------------------------------------------

bool Message::advanceInput(int p) {
    // 0x40ae10
    const auto& m = game_.modes;
    if (m.demo != 0) {
        if (m.demoArg <= demoCount_) {
            demoCount_ = 0;
            return true;
        }
        ++demoCount_;
        if (!game_.keys.advance && !m.forceSkip && !m.skip) return false;
        if (game_.currentTextRead() == 0) return false;
        demoCount_ = 0;
        return true;
    }
    if (p == 0 && m.autoMode) {
        if (config.autoMax <= autoCount_) {
            autoCount_ = 0;
            return true;
        }
        ++autoCount_;
        if (game_.keys.advance || game_.skipActive()) autoCount_ = 0;
    }
    return game_.keys.advance || game_.skipActive() || game_.keys.scrollDown;
}

int Message::revealStep() {
    // 0x40b210: units per frame.
    const auto& m = game_.modes;
    if (m.demo == 0) {
        if (advanceInput(0) || config.speed == 0.0f) return 9999;
        int n = static_cast<int>(2.0f / config.speed * 60.0f / static_cast<float>(Game::kTicksPerSecond));
        return n == 0 ? 1 : n;
    }
    if ((game_.keys.advance || m.forceSkip || m.skip) && m.demo == 1 && game_.currentTextRead() != 0) return 9999;
    int n = 120 / Game::kTicksPerSecond;
    return n == 0 ? 1 : n;
}

void Message::openWindow(int nameIndex, int style, int fade) {
    // 0x402cd0
    if (nameIndex != -1) {
        std::string n = nameIndex >= 0 && nameIndex < 32 ? utf8ToSjis(kNames[nameIndex]) : std::string();
        setName(&n, 0, nameIndex);
        speaker_ = nameIndex;
    }
    if (exists_) return;
    if (config.winDisp != 0) {
        gfx_.makeImage(kWindowBack, 0x108, 0x104d, true);
        gfx_.setRect(kWindowBack, 16, 496, 0, 0, 1248, 208);
        int alpha = fade == 0 ? (game_.scene().lastKind() == 0 ? config.winAlpha : config.winAlphaVis) : 0;
        gfx_.setAlpha(kWindowBack, alpha);
        gfx_.makeImage(kWindowRing, 0x107, 0x104e, true);
        gfx_.setRect(kWindowRing, 16, 496, 0, 0, 1248, 208);
    }
    if (novel_ != 0) {
        gfx_.setVisible(kWindowBack, false);
        gfx_.setVisible(kWindowRing, false);
    }
    auto& ani = game_.ani();
    ani.makeObject(kCursorEnd, kAniEnd, 0, true, 0x104f, false, 1, 0, 0);
    gfx_.moveTo(kCursorEnd, 912, 700);
    gfx_.setMask(kCursorEnd, 2);
    ani.makeObject(kCursorMore, kAniMore, 0, true, 0x104f, false, 1, 0, 0);
    gfx_.moveTo(kCursorMore, 912, 700);
    gfx_.setMask(kCursorMore, 2);
    for (int i = 0; i < 9; ++i) {
        gfx_.makeImage(kButton0 + i, 0x10b, 0x104f, true);
        gfx_.setRect(kButton0 + i, static_cast<float>(kButtonX[i]), kButtonY, static_cast<float>(40 * i), 0, kButtonSize, kButtonSize);
        game_.input.setArea(0, i, kButtonX[i], kButtonY, kButtonSize, kButtonSize, true);
    }
    gfx_.makeImage(kBar, 0x10c, 0x104e, true);
    gfx_.setDest(kBar, 0, 672, 1280, 48);
    gfx_.setSource(kBar, 208, 104, 120, 48);
    gfx_.makeImage(kTip, 0x10c, 0x104e, false);
    gfx_.setDest(kTip, 1000, 664, 208, 16);
    gfx_.setSource(kTip, 0, 0, 208, 16);
    for (int obj : {kIconRead, kIconAuto, kIconSkip, kOnAuto, kOnSkip}) gfx_.makeImage(obj, 0x10c, 0x104f, false);
    gfx_.setRect(kIconRead, 72, 680, 208, 64, 40, 40);
    gfx_.setRect(kIconAuto, 112, 680, 248, 64, 40, 40);
    gfx_.setRect(kIconSkip, 152, 680, 288, 64, 40, 40);
    gfx_.setRect(kOnAuto, 112, 680, 328, 64, 40, 40);
    gfx_.setRect(kOnSkip, 152, 680, 368, 64, 40, 40);
    for (int obj = kWindowBack; obj < kWindowBack + 9; ++obj) {
        gfx_.setMask(obj, 2);
        gfx_.setPivot(obj, 640, 639);
    }
    openCount_ = 0;
    exists_ = true;
    state_ = 1;
    style_ = style;
    openFrames_ = fade;
    game_.scene().windowShown = true;
}

void Message::closeWindow(int duration, int style) {
    // 0x403240
    if (!exists_) return;
    setName(nullptr, 0, -1);
    style_ = style;
    state_ = 2;
    openCount_ = 0;
    openFrames_ = duration;
}

void Message::speak(const std::string& name, int speaker) {
    speaker_ = speaker;
    setName(&name, 0, speaker);
    openWindow(-1, 0, -1);
}

void Message::setName(const std::string* name, int mode, int speaker) {
    // 0x402890: a speaker's name, in text record 1 or 2 (double-buffered for the change slide).
    gfx_.setVisible(kText, false);
    if (name == nullptr || name->empty()) {
        if (mode == 0) logName_.clear();
        else nameCount_ = 0;
        if (name == nullptr) game_.sound().voiceSpeaker = -1;
        gfx_.setVisible(kName0 + (nameBuf_ == 0 ? 1 : 0), false);
        nameShown_ = false;
        nameChanged_ = false;
        return;
    }
    if (!name_.empty()) {
        if (name_ == *name && mode != 0) {
            nameChanged_ = false;
        } else {
            nameChanged_ = true;
            nameBuf_ = nameBuf_ == 0 ? 1 : 0;
        }
    }
    int buf = nameBuf_;
    int color = speaker != -1 ? colorIndex(speaker) : 9;
    bool group = name->find(' ') != std::string::npos;
    for (const char* g : kGroupNames) {
        if (group) break;
        group = name->find(utf8ToSjis(g)) != std::string::npos;
    }
    if (group) {
        color = 8;
        game_.sound().voiceSpeaker = 100;
    }
    (void)color;  // every entry of the colour table is white
    name_ = *name;
    int outline = 0;
    if (mode == 0) {
        logName_ = *name;
        nameCount_ = 0;
    } else {
        nameCount_ = 10;
        if (mode != 2) outline = static_cast<int>(kOutline);
    }
    gfx_.makeTextSlot(buf + 1, kAtlas, name_, 28, 36, 1, 2, 0, 32, 40, true, 0xff, 0xff, 0xff, outline >> 16 & 0xff,
                      outline >> 8 & 0xff, outline & 0xff, true);
    gfx_.makeText(kName0 + buf, buf + 1, 0x105b, false);
    gfx_.setMask(kName0 + buf, 2);
    nameShown_ = true;
}

void Message::novelMode(int on) {
    // 0x403400
    novel_ = on;
    if (on != 0) {
        gfx_.setVisible(kWindowBack, false);
        gfx_.setVisible(kWindowRing, false);
        gfx_.makePolygon(kNovelBack, 4, 0x104c, true);
        gfx_.setDest(kNovelBack, 0, 0, 1280, 720);
        gfx_.setColor(kNovelBack, config.winAlphaNovel, 0, 0, 0);
        novelHidden_ = 0;
        return;
    }
    gfx_.setVisible(kWindowBack, true);
    gfx_.setVisible(kWindowRing, true);
    gfx_.setVisible(kNovelBack, false);
}

void Message::novelFade(int frames, int hidden) {
    // 0x403290
    if (novel_ != 0 && novelHidden_ != hidden) {
        novelHidden_ = hidden;
        novelFading_ = true;
        novelCount_ = 0;
        novelFrames_ = frames;
    }
}

bool Message::novelFading() const {
    return novel_ != 0 && novelFading_;
}

int Message::pushLog(const std::string& text, bool noName) {
    // 0x4034c0: a new entry at the front; the voice played for it, unless LogOut or no voice since the
    // last line.
    LogEntry e;
    e.text = text;
    e.name = noName ? std::string() : logName_;
    e.novel = novel_ != 0;
    buffer_ = text;
    auto& snd = game_.sound();
    int color = 9;
    if (snd.voiceSpeaker == -1 || logOut != 0) {
        if (!logName_.empty()) color = colorIndex(speaker_);
    } else {
        e.voices[0] = {snd.lastVoice.scene, snd.lastVoice.num, snd.lastVoice.chara, snd.lastVoice.vol};
        e.speaker = snd.voiceSpeaker;
        color = colorIndex(snd.voiceSpeaker);
    }
    log_.push_front(std::move(e));
    if (log_.size() > 255) log_.pop_back();
    return color;
}

void Message::newMessage(const std::string& text, int kind) {
    // 0x40e420
    pushLog(text, false);
    game_.ifSkip = std::max(game_.ifSkip - 1, 0);
    int cols = novel_ == 0 ? 28 : 40, rows = novel_ == 0 ? 4 : 15;
    gfx_.makeTextSlot(0, kAtlas, text, 28, cols, rows, 0, 14, 32, 40, true, 0xff, 0xff, 0xff, kOutline >> 16 & 0xff,
                      kOutline >> 8 & 0xff, kOutline & 0xff, true);
    gfx_.makeText(kText, 0, 0x1057, true);
    if (nameShown_) gfx_.setTextColor(nameBuf_ + 1, 0xff, 0xff, 0xff);
    gfx_.setMask(kText, 2);
    if (novel_ == 0) gfx_.moveTo(kText, kTextX, kTextY);
    else gfx_.moveTo(kText, kNovelX, kNovelY);
    gfx_.setPivot(kText, 640, 624);
    gfx_.setFilter(kText, 2);
    state_ = state_ == 1 || state_ == 2 ? 1 : 0;
    kind_ = kind;
    page_ = 0;
    reveal_ = 0;
    voiceSlot_ = 0;
    gfx_.setTextPage(kText, 0);
    gfx_.setTextReveal(kText, 0);
}

void Message::appendMessage(const std::string& text, int kind) {
    // 0x40e5d0: the text after a `\k`; another voice of the same entry goes to its next slot.
    game_.ifSkip = std::max(game_.ifSkip - 1, 0);
    buffer_ += "\\k";
    buffer_ += text;
    if (!log_.empty()) {
        LogEntry& e = log_.front();
        e.text += text;
        const auto& v = game_.sound().lastVoice;
        const Voice& cur = e.voices[static_cast<size_t>(voiceSlot_)];
        if ((cur.scene != v.scene || cur.num != v.num || cur.chara != v.chara || cur.vol != v.vol) && voiceSlot_ + 1 < 10) {
            e.voices[static_cast<size_t>(++voiceSlot_)] = {v.scene, v.num, v.chara, v.vol};
        }
    }
    state_ = state_ == 1 || state_ == 2 ? 1 : 0;
    kind_ = kind;
    gfx_.setTextString(0, buffer_);
    reveal_ = 0;
    gfx_.setTextReveal(kText, 0);
    gfx_.setTextPage(kText, page_);
}

void Message::say(const std::string& text, int line, int mode, int kind) {
    // The natives' first run (0x454970 / 0x4549e0 / 0x454a60).
    novelFade(30, 0);
    openWindow(-1, 0, -1);
    if (mode == 0) appendMessage(text, kind);
    else if (mode == 1) newMessage(text, kind);
    game_.markLine(line);
}

// ---- Choices -----------------------------------------------------------------------------------------

void Message::showChoices(int id, int scriptNumber) {
    // 0x4046a0: a bar, the text and (when chosen before) a check per shown option, centred on the
    // number shown; options shown only because of their `always` flag are grey and cannot be taken.
    auto& g = game_;
    g.choiceResult = -1;
    g.choicePick = -1;
    game_.clearScreen = true;
    choiceShown_ = true;
    choicePhase_ = 1;
    choiceCount_ = 0;
    choiceActive_ = true;
    g.choiceScript = scriptNumber;
    g.choiceId = id;
    int flag = g.choiceFlagIndex();
    int shown = static_cast<int>(g.visibleChoices.size());
    int k = 0;
    for (size_t i = 0; i < g.choiceOptions.size() && i < 8; ++i) {
        const auto& o = g.choiceOptions[i];
        bool taken = g.system.gameFlag(o.flag) == o.value;
        choiceDisabled_[i] = false;
        if (!taken && o.always == 0) continue;
        int slot = 0x10 + static_cast<int>(i);
        uint32_t color = kChoiceNormal;
        bool chosenBefore = (g.system.gameFlag(flag) >> i & 1) != 0;
        if (!taken || (!chosenBefore && g.modes.replay != 0)) {
            choiceDisabled_[i] = true;
            color = kChoiceGrey;
        }
        int y = (k - shown) * 0x2a;
        gfx_.makeTextSlot(slot, kAtlas, o.text, 28, 24, 2, 0, 0, 34, 40, true, color >> 16 & 0xff, color >> 8 & 0xff,
                          color & 0xff, 0, 0, 0, true);
        gfx_.makeText(kChoiceText + static_cast<int>(i), slot, 0x1059, false);
        gfx_.moveTo(kChoiceText + static_cast<int>(i), 358, static_cast<float>(y + 0x14e));
        g.input.setArea(0, slot, 0x166, y + 0x14e, 0x230, 0x28, true);
        int bar = kChoiceBar + 4 * static_cast<int>(i);
        for (int b = 0; b < 3; ++b) {
            gfx_.makeImage(bar + b, 0x12a, 0x1057, false);
            gfx_.setRect(bar + b, 248, static_cast<float>(y + 0x140), 0, static_cast<float>(144 - 72 * b), 784, 72);
        }
        gfx_.setBlend(bar, 3);
        if (chosenBefore) {
            gfx_.makeImage(bar + 3, 0x10c, 0x1058, false);
            gfx_.setRect(bar + 3, 328, static_cast<float>((k + 8 - shown) * 0x2a), 208, 64, 40, 40);
        }
        k += 2;
    }
}

std::vector<int> Message::selectableChoices() const {
    std::vector<int> out;
    for (size_t i = 0; i < game_.choiceOptions.size() && i < 8; ++i) {
        const auto& o = game_.choiceOptions[i];
        if (game_.system.gameFlag(o.flag) == o.value && !choiceDisabled_[i]) out.push_back(static_cast<int>(i));
    }
    return out;
}

void Message::pick(int option) {
    auto& g = game_;
    if (option < 0 || option >= static_cast<int>(g.choiceOptions.size())) return;
    pushLog(g.choiceOptions[static_cast<size_t>(option)].text, true);
    g.choicePick = option;
    int flag = g.choiceFlagIndex();
    clearChoices();
    g.system.setGameFlag(flag, g.system.gameFlag(flag) | 1 << (option & 31));
    g.writeSystem();
    g.sound().se(8905, 255);  // 0x40ea10
}

void Message::clearChoices() {
    // 0x404d10
    auto& g = game_;
    choicePhase_ = 0;
    g.choiceResult = g.choicePick;
    choiceShown_ = false;
    choiceCount_ = 0;
    gfx_.release(0x12a);
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) gfx_.remove(kChoiceBar + 4 * i + b);
        gfx_.releaseTextSlot(0x10 + i);
        gfx_.remove(kChoiceText + i);
        gfx_.remove(kChoiceText + 8 + i);
        g.input.setArea(0, 0x10 + i, 0, 0, 0, 0, false);
    }
    g.choiceOptions.clear();
    g.visibleChoices.clear();
    g.choiceShown = false;
}

void Message::choiceFrame() {
    // 0x413400
    auto& g = game_;
    int dur = g.fr(-2);
    int hover = g.input.hoveredArea();
    bool click = g.keys.advance;
    int sel = hover - 0x10;
    int prev = choiceHover_;
    if (choiceShown_ && choiceActive_) {
        int n = static_cast<int>(g.choiceOptions.size());
        auto shown = [&](int i) {
            const auto& o = g.choiceOptions[static_cast<size_t>(i)];
            return g.system.gameFlag(o.flag) == o.value || o.always != 0;
        };
        if (choicePhase_ == 0) {
            if (state_ == 6) {
                choiceBackFromHidden_ = true;
                return;
            }
            if (state_ != 5) {
                if (g.keys.up) g.input.pointAt(0, sel >= 0 ? hover - 1 : 0x10);
                if (g.keys.down) g.input.pointAt(0, sel >= 0 ? hover + 1 : 0x10);
                for (int i = 0; i < n; ++i) {
                    if (!choiceDisabled_[static_cast<size_t>(i)]) {
                        const auto& o = g.choiceOptions[static_cast<size_t>(i)];
                        if (g.system.gameFlag(o.flag) != o.value) continue;
                        gfx_.setTextColor(0x10 + i, kChoiceNormal >> 16 & 0xff, kChoiceNormal >> 8 & 0xff, kChoiceNormal & 0xff);
                        gfx_.setVisible(kChoiceBar + 4 * i + 1, false);
                    }
                    if (i + 1 < 10 && g.keys.digits[static_cast<size_t>(i + 1)]) {
                        click = true;
                        sel = i;
                    }
                }
                if (choiceBackFromHidden_) click = false;
                choiceBackFromHidden_ = false;
                if (sel >= 0 && sel < n) {
                    if (choiceDisabled_[static_cast<size_t>(sel)]) {
                        if (click) g.sound().se(9214, 255);  // 0x405cd0: the refusal
                        return;
                    }
                    gfx_.setTextColor(0x10 + sel, kChoiceHover >> 16 & 0xff, kChoiceHover >> 8 & 0xff, kChoiceHover & 0xff);
                    gfx_.setVisible(kChoiceBar + 4 * sel + 1, true);
                    gfx_.setBlend(kChoiceBar + 4 * sel + 1, 3);
                    if (prev != sel) g.sound().systemSe(Sound::Hover);  // 0x405af0
                }
                prev = sel;
                if (click && sel >= 0 && sel < n) pick(sel);
            }
        } else if (choicePhase_ == 1) {
            ++choiceCount_;
            choiceBackFromHidden_ = false;
            for (int i = 0; i < n; ++i) {
                if (!shown(i)) continue;
                for (int obj : {kChoiceBar + 4 * i, kChoiceBar + 4 * i + 2, kChoiceBar + 4 * i + 3, kChoiceText + i}) gfx_.setVisible(obj, true);
            }
            int a = choiceCount_ < dur ? choiceCount_ * 0xff / dur : 0xff;
            if (choiceCount_ >= dur) choicePhase_ = 0;
            for (int i = 0; i < n; ++i) {
                if (!shown(i)) continue;
                for (int obj : {kChoiceBar + 4 * i, kChoiceBar + 4 * i + 2, kChoiceBar + 4 * i + 3, kChoiceText + i}) gfx_.setAlpha(obj, a);
            }
            return;
        }
    }
    choiceHover_ = prev;
}

// ---- The frame ---------------------------------------------------------------------------------------

void Message::hideCursors() {
    gfx_.setVisible(kCursorEnd, false);
    gfx_.setVisible(kCursorMore, false);
    cursorCount_ = 0;
}

void Message::showWindow(bool on) {
    // 0x40e220
    gfx_.setVisible(kText, on);
    for (int i = 0; i < 0xb; ++i) gfx_.setVisible(kWindowBack + i, on);
    if (config.winDisp == 0) {
        gfx_.setVisible(kWindowBack, false);
        gfx_.setVisible(kWindowRing, false);
    }
    gfx_.setVisible(kBar, on);
    if (!on) gfx_.setVisible(kTip, false);
    if (novel_ != 0) {
        gfx_.setVisible(kWindowBack, false);
        gfx_.setVisible(kWindowRing, false);
        gfx_.setVisible(kNovelBack, on);
    }
    if (nameShown_) gfx_.setVisible(kName0 + nameBuf_, on);
    if (game_.modes.skip) {
        gfx_.setVisible(kIconSkip, on);
        gfx_.setVisible(kOnSkip, on);
    }
    if (!game_.modes.autoMode) {
        bool end = gfx_.setTextPage(kText, page_);
        gfx_.setVisible(end ? kCursorEnd : kCursorMore, on);
    } else {
        gfx_.setVisible(kIconAuto, on);
        gfx_.setVisible(kOnAuto, on);
    }
    int read = game_.currentTextRead();
    if (read != 0 && read != 2) gfx_.setVisible(kIconRead, on);
    if (choiceShown_) {
        for (size_t i = 0; i < game_.choiceOptions.size(); ++i) {
            for (int b = 0; b < 4; ++b) gfx_.setVisible(kChoiceBar + 4 * static_cast<int>(i) + b, on && b != 1);
            gfx_.setVisible(kChoiceText + static_cast<int>(i), on);
        }
    }
}

void Message::nameFrame() {
    // Top of 0x410300: the name slides in (from the right when it changed) and fades.
    int dur = game_.fr(10);
    int c;
    if (dur == 0) {
        dur = 10;
        c = nameShown_ ? 10 : 0;
        if (!nameShown_) name_.clear();
    } else {
        if (state_ == 1 || !nameShown_) nameCount_ = std::clamp(nameCount_ - 1, 0, dur);
        else nameCount_ = std::clamp(nameCount_ + 1, 0, dur);
        c = nameCount_;
        if (!nameShown_ && c == 0) name_.clear();
    }
    if (state_ == 6) {
        gfx_.setVisible(kName0, false);
        gfx_.setVisible(kName0 + 1, false);
        return;
    }
    float t = static_cast<float>(dur - c);
    float k = t * t / static_cast<float>(dur * dur) * 256.0f;
    if (nameChanged_) k = -k;
    float off = k * 32.0f * 0.00390625f;
    float sx = game_.effects().winX, sy = game_.effects().winY;
    int cur = kName0 + nameBuf_, other = kName0 + (nameBuf_ == 0 ? 1 : 0);
    gfx_.setVisible(cur, true);
    gfx_.moveTo(cur, 274.0f - off + sx, sy + 508.0f);
    int a = c * 0xff / dur;
    gfx_.setAlpha(cur, a);
    gfx_.moveTo(other, off + 274.0f + sx, sy + 508.0f);
    gfx_.setAlpha(other, 0xff - a);
    if (novel_ != 0) {
        gfx_.setVisible(kName0, false);
        gfx_.setVisible(kName0 + 1, false);
    }
}

void Message::stateOpenClose() {
    // 0x411b7e
    ++openCount_;
    int dur = game_.fr(openFrames_);
    int win = game_.scene().lastKind() == 0 ? config.winAlpha : config.winAlphaVis;
    if (openCount_ < dur) {
        int t = state_ == 1 ? openCount_ : dur - openCount_;
        for (int obj = kWindowBack; obj <= kBar; ++obj) gfx_.setAlpha(obj, config.winAlphaCommand * t / dur);
        gfx_.setAlpha(kWindowBack, win * t / dur);
        gfx_.setAlpha(kWindowRing, 0xff * t / dur);
        gfx_.setAlpha(kText, 0xff * t / dur);
        for (int obj : {kIconRead, kIconAuto, kIconSkip, kOnAuto, kOnSkip}) gfx_.setVisible(obj, false);
        hideCursors();
        return;
    }
    if (state_ == 1) {
        for (int obj = kButton0; obj <= kBar; ++obj) {
            gfx_.setAlpha(obj, config.winAlphaCommand);
            gfx_.setScale(obj, 1.0f);
        }
        gfx_.setAlpha(kWindowBack, win);
        gfx_.setAlpha(kWindowRing, 0xff);
        gfx_.setAlpha(kText, 0xff);
        state_ = 0;
        return;
    }
    for (int obj = kWindowBack; obj <= kBar; ++obj) gfx_.remove(obj);
    gfx_.remove(kName0);
    gfx_.remove(kName0 + 1);
    gfx_.remove(kCursorEnd);
    gfx_.remove(kCursorMore);
    gfx_.remove(kText);
    for (int i = 0; i < 9; ++i) game_.input.setArea(0, i, 0, 0, 0, 0, false);
    exists_ = false;
    game_.scene().windowShown = false;
}

void Message::stateReveal() {
    // 0x411b34
    hideCursors();
    reveal_ += revealStep();
    if (gfx_.setTextReveal(kText, reveal_)) state_ = 3;
}

void Message::buttons(bool takeInput) {
    // The bar, the icons and the tooltip (state 3).
    auto& g = game_;
    int hover = g.input.hoveredArea();
    bool onBar = hover >= 0 && hover < 9;
    barCount_ = std::clamp(barCount_ + (g.input.pointerY() >= 664 ? 1 : -1), 0, 16);
    int barAlpha = ((16 - barCount_) * config.winAlphaCommand + barCount_ * 256) / 16;
    for (int i = 0; i < 9; ++i) {
        gfx_.setAlpha(kButton0 + i, barAlpha);
        gfx_.setSource(kButton0 + i, static_cast<float>(40 * i), onBar && hover == i ? 40.0f : 0.0f, 40, 40);
    }
    gfx_.setAlpha(kBar, barAlpha);
    int read = g.currentTextRead();
    gfx_.setVisible(kIconRead, read == 1);
    gfx_.setVisible(kIconAuto, g.modes.autoMode);
    gfx_.setVisible(kOnAuto, g.modes.autoMode);
    gfx_.setVisible(kIconSkip, g.modes.skip);
    gfx_.setVisible(kOnSkip, g.modes.skip);
    int blink = triangle(static_cast<int>(g.frame()), 30) * 256 / 30;
    gfx_.setAlpha(kOnAuto, blink);
    gfx_.setAlpha(kOnSkip, blink);
    // The tooltip of the hovered button slides in.
    if (onBar && hover == tipButton_) tipCount_ = std::min(tipCount_ + 1, 16);
    else tipCount_ = 0;
    tipButton_ = onBar ? hover : -1;
    if (onBar) {
        int v = 16 * hover;
        if (hover == kAutoButton) v = g.modes.autoMode ? 96 : 112;
        else if (hover == kSkipButton) v = g.modes.skip ? 128 : 144;
        else if (hover == kHideButton) v = 160;
        float x = static_cast<float>(4 * (250 - (16 - tipCount_) * (16 - tipCount_) / 16));
        gfx_.setVisible(kTip, true);
        gfx_.setRect(kTip, x, 664, 0, static_cast<float>(v), 208, 16);
        gfx_.setAlpha(kTip, 16 * tipCount_);
    }
    if (!takeInput) return;
    bool click = g.keys.advance;
    if ((g.keys.scrollUp || (click && hover == kBacklogButton)) && log_.size() >= 2 && g.modes.demo == 0) {
        openBacklog();
        return;
    }
    if (click && hover == kAutoButton) {
        g.modes.autoMode = !g.modes.autoMode;
        autoCount_ = 0;
    } else if (click && hover == kSkipButton) {
        g.modes.skip = !g.modes.skip;
    } else if (((click && hover == kHideButton) || g.keys.space) && g.modes.demo == 0 && !g.skipActive()) {
        showWindow(false);
        state_ = 6;
    }
}

void Message::stateWait() {
    // 0x410bc4
    auto& g = game_;
    bool in = advanceInput(0);
    if (reveal_ != 0) {
        ++page_;
        reveal_ = 0;
    }
    gfx_.setTextReveal(kText, 0);
    bool end = gfx_.setTextPage(kText, page_);
    bool demo = g.modes.demo != 0;
    if (!g.modes.autoMode && !demo) {
        int show = end ? kCursorEnd : kCursorMore;
        gfx_.setVisible(show, true);
        gfx_.setVisible(end ? kCursorMore : kCursorEnd, false);
        gfx_.setAlpha(show, cursorCount_ * 255 / 30);
        int ax = 0;
        float ay = 0;
        gfx_.textAnchor(kText, ax, ay);
        float ox = novel_ == 0 ? 296.0f : 97.0f, oy = novel_ == 0 ? 554.0f : 44.0f;
        gfx_.moveTo(show, static_cast<float>(ax) + ox, ay + oy);
        cursorCount_ = std::min(cursorCount_ + 1, 30);
    } else {
        hideCursors();
    }
    int hover = g.input.hoveredArea();
    bool onButton = hover >= 0 && hover < 9;
    buttons(true);
    if (state_ != 3) return;
    if (!g.modes.autoMode && !demo && onButton && !g.modes.skip) return;
    bool voiceOn = g.settings.voice == 1;
    bool playing = voiceOn && !g.sound().voiceDone(0, 0, 0);
    bool hold = false;
    if (g.modes.autoMode && playing && !g.keys.advance && !g.skipActive()) hold = true;
    if (demo && playing && !((g.keys.advance || g.modes.forceSkip || g.modes.skip) && g.currentTextRead() != 0)) hold = true;
    bool go = (in && (!onButton || demo)) || g.skipActive() || g.keys.scrollDown;
    if (end) {
        if (hold && kind_ != 1) return;
        if (!(go || kind_ == 1)) return;
        if (g.settings.pageVoice != 0 && kind_ == 2 && !demo) g.sound().stopVoice(0, 0);
        else if (!g.skipActive()) g.sound().restoreDucked();
        if (choiceShown_ && choicePhase_ == 0) return;
        demoCount_ = 0;
        state_ = 4;
        return;
    }
    if (hold || !go) return;
    state_ = 0;
}

void Message::stateDone() {
    // 0x410aca
    hideCursors();
    if (speaker_ == -1) game_.sound().voiceSpeaker = -1;
    // The window's alpha follows the picture kind (background / CG) over 32 frames.
    int target = game_.scene().lastKind() == 0 ? config.winAlpha : config.winAlphaVis;
    int source = game_.scene().lastKind() == 0 ? config.winAlphaVis : config.winAlpha;
    int f = std::min(game_.scene().windowFade(), 32);
    if (novel_ == 0) gfx_.setAlpha(kWindowBack, (source * (32 - f) + target * f) / 32);
    if (choiceShown_ && choicePhase_ == 0) state_ = 3;
}

void Message::stateHidden() {
    // 0x410a83
    auto& k = game_.keys;
    if (k.advance || k.cancel || k.space) {
        state_ = 3;
        showWindow(true);
    }
}

void Message::backlogTexts(int hover) {
    // The visible entries' texts (records 3.., objects 0x446..): newest at the bottom, four of them, or
    // in novel mode one page; all in the colour of the top entry's speaker (white in this build); the
    // current message's entry at the page before the shown one. A hovered replay icon's entry is drawn
    // in the hover colour.
    int count = static_cast<int>(log_.size());
    if (count == 0) return;
    const LogEntry& top = log_[static_cast<size_t>(logScroll_)];
    int color = top.name.empty() ? 9 : colorIndex(top.speaker);
    (void)color;  // 0x4be468's colours are all white
    auto make = [&](int record, const std::string& text, int cols, int rows, uint32_t rgb) {
        gfx_.makeTextSlot(record, kAtlas, text, 28, cols, rows, 0, 14, 32, 40, true, rgb >> 16 & 0xff, rgb >> 8 & 0xff,
                          rgb & 0xff, kOutline >> 16 & 0xff, kOutline >> 8 & 0xff, kOutline & 0xff, true);
    };
    bool novel = log_.front().novel;
    if (!novel) {
        int shown = std::min(count, 4);
        for (int j = 0; j < shown; ++j) {
            int k = shown - j - 1;
            make(3 + j, log_[static_cast<size_t>(logScroll_ + k)].text, 28, 4, 0xffffff);
            if (logScroll_ + k == 0) gfx_.setTextPage(kBacklogText + j, page_ - 1);
        }
        if (hover >= 0xf && hover < 0xf + 4) {
            int index = shown - hover + 0xe + logScroll_;
            if (index >= 0 && index < count && log_[static_cast<size_t>(index)].speaker != -1 &&
                log_[static_cast<size_t>(index)].voices[0].chara != -1) {
                make(hover - 0xc, log_[static_cast<size_t>(index)].text, 28, 4, kChoiceHover);
            }
        }
    } else {
        make(3, top.text, 40, 15, 0xffffff);
        if (logScroll_ == 0) gfx_.setTextPage(kBacklogText, page_ - 1);
    }
}

void Message::openBacklog() {
    // 0x410300 state 3: the wheel up or BACKLOG with two entries or more.
    game_.sound().systemSe(Sound::Decide);
    logScroll_ = 0;
    const LogEntry& e = log_.front();
    setName(&e.name, 1, e.speaker);
    int shown = std::min(static_cast<int>(log_.size()), 4);
    if (e.novel) {
        backlogTexts(-1);
        gfx_.makeText(kBacklogText, 3, 0x1065, true);
        gfx_.setMask(kBacklogText, 2);
        gfx_.moveTo(kBacklogText, 75, 26);
        gfx_.setPivot(kBacklogText, 640, 624);
        gfx_.setFilter(kBacklogText, 2);
        if (logScroll_ == 0) gfx_.setTextPage(kBacklogText, page_ - 1);
    } else {
        backlogTexts(-1);
        for (int j = 0; j < shown; ++j) {
            gfx_.makeText(kBacklogText + j, 3 + j, 0x1065, true);
            gfx_.setMask(kBacklogText + j, 2);
            gfx_.moveTo(kBacklogText + j, 274, static_cast<float>(0x38 + 0xa0 * j));
            gfx_.setPivot(kBacklogText + j, 640, 624);
            gfx_.setFilter(kBacklogText + j, 2);
            if (logScroll_ + shown - j - 1 == 0) gfx_.setTextPage(kBacklogText + j, page_ - 1);
        }
    }
    gfx_.setVisible(kText, false);
    state_ = 5;
    backlogFrame();
}

void Message::backlogFrame() {
    // 0x4038a0: the window and its bar hidden; the shade, the frame, the scroll bar (0x4be49c's places),
    // the replay icons, Back; the hit areas in group 1.
    gfx_.setVisible(kWindowBack, false);
    gfx_.setVisible(kWindowRing, false);
    for (int i = 0; i < 9; ++i) gfx_.setVisible(kButton0 + i, false);
    gfx_.setVisible(kBar, false);
    gfx_.setVisible(kTip, false);
    gfx_.makePolygon(kBacklogShade, 4, 0x1061, true);
    gfx_.setDest(kBacklogShade, 0, 0, 1280, 720);
    gfx_.setColor(kBacklogShade, 0x80, 0, 0, 0);
    gfx_.makeImage(0x43f, 0x116, 0x1066, true);
    gfx_.setRect(0x43f, 24, 688, 0, 0, 0, 8);
    int bgm = game_.sound().bgmTrack();
    if (bgm != -1) {
        gfx_.makeImage(0x440, 0x117, 0x1066, true);
        gfx_.setRect(0x440, 24, 696, 0, static_cast<float>(Game::musicIndex(bgm) << 4), 264, 16);
    }
    int off = novel_ != 0 ? 0xc4 : 0;
    gfx_.makeImage(0x42b, 0x10e, 0x1065, true);
    gfx_.setRect(0x42b, static_cast<float>(1032 + off), 32, 0, 0, 32, 48);
    gfx_.makeImage(0x42c, 0x10e, 0x1065, true);
    gfx_.setRect(0x42c, static_cast<float>(1032 + off), 624, 0, 592, 32, 48);
    gfx_.makeImage(0x42d, 0x10e, 0x1065, true);
    gfx_.setRect(0x42d, static_cast<float>(1032 + off), 80, 0, 48, 32, 544);
    gfx_.makeImage(0x42e, 0x10e, 0x1066, true);
    gfx_.setRect(0x42e, static_cast<float>(1040 + off), 80 + 0x1c2, 64, 48, 16, 88);
    if (novel_ == 0) {
        for (int i = 0; i < 4; ++i) {
            gfx_.makeImage(kBacklogIcon + i, 0x10f, 0x1065, false);
            gfx_.setRect(kBacklogIcon + i, 236, static_cast<float>(0x20 + 0xa0 * i), 0, 0, 30, 30);
        }
        gfx_.makeImage(0x431, 0x111, 0x1064, true);
        gfx_.setDest(0x431, 216, 0, 848, 672);
    } else {
        gfx_.makeImage(0x431, 0x111, 0x1064, true);
        gfx_.setDest(0x431, 20, 0, 1240, 672);
    }
    gfx_.makeImage(0x432, 0x110, 0x1065, true);
    gfx_.moveTo(0x432, 0, 0);
    gfx_.makeImage(0x433, 0x112, 0x1065, true);
    gfx_.moveTo(0x433, 0, 664);
    gfx_.makeImage(0x434, 0x113, 0x1066, true);
    gfx_.setRect(0x434, 1164, 688, 0, 0, 56, 24);
    auto& in = game_.input;
    in.setGroup(1);
    for (int i = 0; i < 4; ++i) in.setArea(1, 0xf + i, 0xec, 0x20 + 0xa0 * i, 0x1c, 0x1c, true);
    in.setArea(1, 0xb, 1032 + off, 32, 32, 48, true);
    in.setArea(1, 0xc, 1032 + off, 624, 32, 48, true);
    in.setArea(1, 0xd, 1040 + off, 86, 16, 532, true);
    in.setArea(1, 0xe, 0x48c, 0x2b0, 0x38, 0x18, true);
}

void Message::closeBacklog() {
    // 0x403e50
    if (novel_ == 0) {
        gfx_.setVisible(kWindowBack, true);
        gfx_.setVisible(kWindowRing, true);
    }
    state_ = 3;
    logScroll_ = 0;
    if (!log_.empty()) setName(&log_.front().name, 2, log_.front().speaker);
    for (int i = 0; i < 4; ++i) {
        gfx_.setVisible(kBacklogText + i, false);
        gfx_.setVisible(kBacklogName + i, false);
    }
    gfx_.setVisible(kText, true);
    for (int i = 0; i < 9; ++i) gfx_.setVisible(kButton0 + i, true);
    gfx_.setVisible(kBar, true);
    for (int obj = 0x42b; obj < 0x42b + 10; ++obj) gfx_.remove(obj);
    for (int i = 0; i < 4; ++i) gfx_.remove(kBacklogIcon + i);
    gfx_.remove(kBacklogShade);
    gfx_.remove(0x43f);
    gfx_.remove(0x440);
    game_.input.clearAreas(1);
    game_.input.setGroup(0);
}

void Message::stateBacklog() {
    // 0x410300 state 5 (0x4120b9)
    auto& g = game_;
    auto& k = g.keys;
    auto& sound = g.sound();
    int count = static_cast<int>(log_.size());
    int hover = g.input.hoveredArea();
    bool click = k.advance;
    bool novel = log_.front().novel;
    int shown = std::min(count, 4);
    gfx_.setVisible(kName0, false);
    gfx_.setVisible(kName0 + 1, false);
    for (int obj = 0x421; obj <= 0x423; ++obj) gfx_.setVisible(obj, false);
    for (int i = 0; i < 4; ++i) {
        gfx_.setVisible(kBacklogIcon + i, false);
        g.input.enableArea(1, 0xf + i, false);
    }
    // The names and the replay icons.
    for (int j = 0; j < shown; ++j) {
        int kk = shown - j - 1;
        const LogEntry& e = log_[static_cast<size_t>(logScroll_ + kk)];
        gfx_.makeTextSlot(7 + j, kAtlas, e.name, 28, 0x24, 1, 2, 0, 32, 40, true, 0xff, 0xff, 0xff, kOutline >> 16 & 0xff,
                          kOutline >> 8 & 0xff, kOutline & 0xff, true);
        gfx_.makeText(kBacklogName + j, 7 + j, 0x1065, true);
        gfx_.moveTo(kBacklogName + j, 274, static_cast<float>(0x1c + 0xa0 * j));
        if (e.speaker == -1 || e.voices[0].chara == -1) {
            gfx_.setVisible(kBacklogIcon + j, false);
            g.input.enableArea(1, 0xf + j, false);
        } else {
            gfx_.setVisible(kBacklogIcon + j, true);
            bool on = !sound.charVoiceOff(e.voices[0].chara);
            gfx_.moveSource(kBacklogIcon + j, 0, on ? 30.0f : 0.0f);
            g.input.enableArea(1, 0xf + j, on);
        }
    }
    g.modes.skip = false;
    for (int obj : {kIconRead, kIconAuto, kIconSkip, kOnAuto, kOnSkip, kCursorEnd, kCursorMore}) gfx_.setVisible(obj, false);
    gfx_.moveSource(0x42b, 0, 0);
    gfx_.moveSource(0x42c, 0, 592);
    gfx_.moveSource(0x42d, 0, 48);
    gfx_.moveSource(0x434, 0, 0);
    bool atBottom = logScroll_ < 1;
    int max = std::max(novel ? count - 1 : count - 4, 0);
    bool canUp = std::min(logScroll_ + 1, max) != logScroll_;
    int off = novel ? 0xcc : 8;
    if ((novel ? count - 1 : count - 4) > 0) {
        int y = logScroll_ * 0x1bc / max;
        gfx_.moveTo(0x42e, static_cast<float>(1032 + off), static_cast<float>(80 - y + 0x1c2));
    }
    // Dragging the thumb.
    bool dragged = false;
    if (hover == 0xd && g.input.buttonHeld(Input::LeftButton) && (!atBottom || canUp)) {
        gfx_.moveSource(0x42d, 32, 48);
        if ((novel ? count - 1 : count - 4) > 0) {
            int py = std::clamp(g.input.pointerY() - 0x32 - 80, 0, 0x1bc);
            logScroll_ = static_cast<int>(((static_cast<double>(max) + 0.5) * static_cast<double>(0x1bc - py)) / 444.0);
            dragged = true;
        }
    }
    bool up = k.scrollUp, down = k.scrollDown;
    if (!click) {
        switch (hover) {
        case 0xb:
            if (canUp) gfx_.moveSource(0x42b, 32, 0);
            else hover = -1;
            break;
        case 0xc:
            if (atBottom) hover = -1;
            else gfx_.moveSource(0x42c, 32, 592);
            break;
        case 0xd:
            if (!atBottom || canUp) gfx_.moveSource(0x42d, 32, 48);
            break;
        case 0xe:
            gfx_.moveSource(0x434, 0, 24);
            break;
        case 0xf:
        case 0x10:
        case 0x11:
        case 0x12:
            if (!novel) {
                int index = shown - hover + 0xe + logScroll_;
                if (index >= 0 && index < count && log_[static_cast<size_t>(index)].speaker != -1 &&
                    log_[static_cast<size_t>(index)].voices[0].chara != -1) {
                    gfx_.moveSource(hover + 0x426, 0, 60);
                    gfx_.setTextColor(hover - 8, kChoiceHover >> 16 & 0xff, kChoiceHover >> 8 & 0xff, kChoiceHover & 0xff,
                                      kOutline >> 16 & 0xff, kOutline >> 8 & 0xff, kOutline & 0xff);
                }
            }
            break;
        default:
            break;
        }
    } else {
        switch (hover) {
        case 0xb:
            if (!canUp) {
                hover = -1;
                break;
            }
            up = true;
            gfx_.moveSource(0x42b, 64, 0);
            break;
        case 0xc:
            if (atBottom) {
                hover = -1;
                break;
            }
            if (logScroll_ > 0) down = true;
            gfx_.moveSource(0x42c, 64, 592);
            break;
        case 0xe:
            sound.systemSe(Sound::Cancel);
            gfx_.moveSource(0x434, 0, 48);
            closeBacklog();
            return;
        case 0xf:
        case 0x10:
        case 0x11:
        case 0x12: {
            sound.systemSe(Sound::Decide);
            if (novel) break;
            int kk = shown - hover + 0xe;
            int index = logScroll_ + kk;
            if (kk >= 0 && index < count && log_[static_cast<size_t>(index)].speaker != -1) {
                const Voice& v = log_[static_cast<size_t>(index)].voices[0];
                if (v.chara != -1) {
                    sound.playVoice(0, v.chara, v.scene, v.num, v.vol, 0, 1);
                    replayStep_ = 1;
                    replayRow_ = kk;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    auto redraw = [&]() {
        const LogEntry& e = log_[static_cast<size_t>(logScroll_)];
        setName(&e.name, 1, e.speaker);
        backlogTexts(-1);
        backlogHover_ = -1;
    };
    if (down) {
        sound.systemSe(Sound::Page);
        --logScroll_;
        replayStep_ = 0;
        if (logScroll_ < 0) {
            logScroll_ = 0;
            closeBacklog();
            return;
        }
        redraw();
    } else if (up) {
        replayStep_ = 0;
        int next = std::min(logScroll_ + 1, max);
        bool changed = next != logScroll_;
        logScroll_ = next;
        if (changed) {
            sound.systemSe(Sound::Page);
            redraw();
        }
        backlogHover_ = -1;
    } else if (k.cancel) {
        sound.systemSe(Sound::Cancel);
        gfx_.moveSource(0x434, 0, 48);
        replayStep_ = 0;
        closeBacklog();
        return;
    }
    // A replay plays the entry's voices one after another.
    if (sound.voiceDone(0, 0, 0) && replayStep_ != 0) {
        int index = logScroll_ + replayRow_;
        if (index < 0 || index >= count || replayStep_ >= 10 ||
            log_[static_cast<size_t>(index)].voices[static_cast<size_t>(replayStep_)].chara == -1) {
            replayStep_ = 0;
        } else {
            const Voice& v = log_[static_cast<size_t>(index)].voices[static_cast<size_t>(replayStep_)];
            sound.playVoice(0, v.chara, v.scene, v.num, v.vol, 0, 1);
            ++replayStep_;
        }
    }
    if (backlogHover_ != hover) {
        sound.systemSe(Sound::Hover);
        backlogTexts(hover);
    } else if (dragged) {
        backlogTexts(hover);
    }
    backlogHover_ = hover;
}

void Message::storyInput() {
    // 0x40a180: the bar's Q.SAVE, Q.LOAD, OPTION, SAVE and LOAD, and the right click (the setting: hide
    // the window, options, or save); only while the scene is at rest (0x409e00), which also enables the
    // window menu's Return To Title. A quick save's thumbnail was drawn last frame: it is saved now.
    auto& g = game_;
    if (!g.sceneAtRest()) return;
    auto& menu = g.saveMenu();
    auto& sound = g.sound();
    if (menu.thumbnailPending()) {
        menu.takeThumbnail();
        g.writeSave(-1);
        quickPreview();
        menu.openQuick(0);
    }
    if (g.modes.demo != 0) return;
    int hover = g.input.hoveredArea();
    bool click = g.keys.advance;
    bool replay = g.modes.replay != 0;
    constexpr bool icInstalled = false;
    bool save = false;
    if (!g.keys.cancel) {
        if (!click) return;
        switch (hover) {
        case 0:  // Q.SAVE
            if (replay) return sound.systemSe(Sound::Buzz);
            sound.systemSe(Sound::Decide);
            menu.open(10, true);
            g.runEx = 1;
            return;
        case 1: {  // Q.LOAD
            if (replay) return sound.systemSe(Sound::Buzz);
            auto info = g.readSaveInfo(-1, false);
            if (!info) return;
            if (info->sortKey == 0 && !icInstalled && !g.assets().introductoryChapterBundled()) return sound.systemSe(Sound::Buzz);
            sound.systemSe(Sound::Decide);
            menu.openQuick(1);
            return;
        }
        case 2:  // OPTION
            sound.systemSe(Sound::Decide);
            g.openOptions();
            return;
        case 3:  // SAVE
            save = true;
            break;
        case 4:  // LOAD
            if (replay) return sound.systemSe(Sound::Buzz);
            sound.systemSe(Sound::Decide);
            menu.open(1, false);
            return;
        default:
            return;
        }
    } else if (config.rightClick == 0) {
        sound.systemSe(Sound::Decide);
        g.keys.space = true;
        return;
    } else if (config.rightClick == 1) {
        sound.systemSe(Sound::Decide);
        g.openOptions();
        return;
    } else if (config.rightClick == 2) {
        save = true;
    }
    if (!save) return;
    if (replay) return sound.systemSe(Sound::Buzz);
    sound.systemSe(Sound::Decide);
    menu.open(0, true);
}

void Message::applyWindowArt() {
    // 0x41f800 row 9 / 0x41a160
    if (config.winDisp == 0) {
        gfx_.remove(kWindowBack);
        gfx_.remove(kWindowRing);
    } else if (novel_ == 0) {
        gfx_.makeImage(kWindowBack, 0x108, 0x104d, true);
        gfx_.setRect(kWindowBack, 16, 496, 0, 0, 1248, 208);
        gfx_.setAlpha(kWindowBack, game_.scene().lastKind() == 0 ? config.winAlpha : config.winAlphaVis);
        gfx_.makeImage(kWindowRing, 0x107, 0x104e, true);
        gfx_.setRect(kWindowRing, 16, 496, 0, 0, 1248, 208);
    }
}

void Message::quickPreview() {
    // 0x409e80: the quick save's card for the Q.LOAD button (objects 0x425..0x429, hidden).
    gfx_.makeImage(0x425, 0x10c, 0x1063, false);
    gfx_.setRect(0x425, 480, 664, 0, 176, 320, 56);
    if (auto info = game_.readSaveInfo(-1)) {
        gfx_.setRect(0x425, 480, 664, 0, 232, 320, 56);
        char date[64];
        std::snprintf(date, sizeof date, "%4d %02d/%02d %02d:%02d", info->time[0], info->time[1], info->time[3], info->time[4],
                      info->time[5]);
        gfx_.makeString(0x426, 0x114, 0x1064, false, date);
        gfx_.setDest(0x426, 576, 672, 12, 16);
        gfx_.setSource(0x426, 0, 0, 12, 16);
        gfx_.makeTextSlot(0x33, kAtlas, "<F10" + info->text + ">", 0x1c, 0x16, 1, 0, 0, 0x22, 0x28, false, 0xff, 0xff, 0xff, 0,
                          0, 0, false);
        gfx_.makeText(0x427, 0x33, 0x1064, false);
        gfx_.moveTo(0x427, 570, 684);
        gfx_.setFilter(0x427, 2);
        gfx_.loadPixels(0x115, info->thumbnail);
        gfx_.makeImage(0x428, 0x115, 0x1064, false);
        gfx_.setDest(0x428, 504, 670, 64, 36);
        gfx_.setFilter(0x428, 2);
    }
    gfx_.makeImage(0x429, 0x10c, 0x1065, false);
    gfx_.setRect(0x429, 488, 648, 208, 0, 88, 32);
}

void Message::placeWindow() {
    if (!exists_) return;
    const auto& e = game_.effects();
    auto put = [&](int obj, float x, float y) {
        gfx_.moveTo(obj, x + e.winX, y + e.winY);
        gfx_.setPivot(obj, 640, 600);
        gfx_.setScale(obj, e.winZoom);
        gfx_.setRotation(obj, e.winAngle);
    };
    put(kWindowBack, 16, 496);
    put(kWindowRing, 16, 496);
    for (int i = 0; i < 9; ++i) put(kButton0 + i, static_cast<float>(kButtonX[i]), kButtonY);
    put(kBar, 0, 672);
    put(kIconRead, 72, 680);
    put(kIconAuto, 112, 680);
    put(kIconSkip, 152, 680);
    put(kOnAuto, 112, 680);
    put(kOnSkip, 152, 680);
    if (novel_ == 0) put(kText, kTextX, kTextY);
    else put(kText, kNovelX, kNovelY);
}

void Message::update() {
    // 0x413c10 state 4: 0x40a180, 0x410300, 0x413400.
    auto& g = game_;
    storyInput();
    g.fr(openFrames_);
    int hover = g.input.hoveredArea();
    if (g.keys.shift) {
        g.modes.skip = !g.modes.skip;
    } else if ((g.keys.advance && (!choiceShown_ || hover < 0x10)) || g.keys.cancel || g.keys.space) {
        g.modes.skip = false;
    }
    // The novel backdrop's fade.
    if (novelFading_) {
        int dur = g.fr(novelFrames_);
        int a;
        if (novelHidden_ == 0) {
            gfx_.setVisible(kText, true);
            if (novelCount_ < dur) a = config.winAlphaNovel * ++novelCount_ / dur;
            else novelFading_ = false, novelCount_ = 0, a = config.winAlphaNovel;
        } else {
            gfx_.setVisible(kText, false);
            if (novelCount_ < dur) a = config.winAlphaNovel - config.winAlphaNovel * ++novelCount_ / dur;
            else novelCount_ = 0, novelFading_ = false, a = 0;
        }
        gfx_.setAlpha(kNovelBack, a);
    }
    gfx_.setVisible(kTip, false);
    if (exists_) {
        if (g.effects().windowShakeOn()) placeWindow();
        nameFrame();
        switch (state_) {
        case 0: stateReveal(); break;
        case 1:
        case 2: stateOpenClose(); break;
        case 3: stateWait(); break;
        case 4: stateDone(); break;
        case 5: stateBacklog(); break;
        case 6: stateHidden(); break;
        default: break;
        }
    }
    choiceFrame();
}

// ---- Saves ------------------------------------------------------------------------------------------

void Message::clearBacklog() {
    // 0x4032f0
    log_.clear();
    logScroll_ = 0;
    replayStep_ = 0;
    game_.sound().voiceSpeaker = -1;
}

void Message::capture(SaveRecord::Message& m) const {
    // 0x4037c0: the kind and page as bytes, the reveal as a short; the name and text as fixed strings.
    m.window = exists_;
    m.kind = static_cast<int8_t>(kind_);
    m.page = static_cast<int8_t>(page_);
    m.reveal = static_cast<int16_t>(reveal_);
    m.novel = novel_;
    m.nameShown = nameShown_;
    m.name = name_.substr(0, 0x40);
    m.text = buffer_.substr(0, 0x3ff);
}

void Message::releaseText() {
    // 0x403780
    gfx_.remove(kText);
    gfx_.release(0x127);
    exists_ = false;
}

void Message::restore(const SaveRecord::Message& m) {
    // 0x40e830
    if (m.window) {
        releaseText();
        novelMode(m.novel);
        gfx_.setVisible(kName0, false);
        gfx_.setVisible(kName0 + 1, false);
        if (m.novel == 0 && m.nameShown) {
            // The speaker argument is the record's byte at +0x212, which nothing writes: 0.
            setName(&m.name, 0, 0);
        } else {
            setName(nullptr, 0, -1);
            if (m.novel == 0) nameCount_ = 0;
        }
        openWindow(-1, 0, 0);
        newMessage(m.text, m.kind);
        reveal_ = m.reveal;
        page_ = m.page;
        state_ = 3;
    }
    logOut = 0;  // 0x4033f0(0)
    clearBacklog();
}

}  // namespace nyan::wa2
