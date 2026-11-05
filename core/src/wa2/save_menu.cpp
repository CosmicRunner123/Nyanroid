#include "nyan/wa2/save_menu.hpp"

#include <algorithm>
#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

constexpr int kTop = 0x4a4, kBottom = 0x4a5, kArt = 0x4a7, kShade = 0x4a8, kCursor = 0x4a9, kNew = 0x4aa;
constexpr int kPrev = 0x4ab, kNext = 0x4ac, kBack = 0x4ad;
constexpr int kPlate = 0x4ae, kThumb = 0x4b8, kChapter = 0x4c2, kNumber = 0x4cc, kText = 0x4d6, kMonth = 0x508,
              kDay = 0x512;
constexpr int kTab = 0x4e0, kPromptText = 0x4ea, kPromptBox = 0x4ed, kYes = 0x4ee, kNo = 0x4ef, kPromptShade = 0x4f1;
constexpr int kQuickPlate = 0x4f4, kQuickLabel = 0x4f5, kQuickHover = 0x4f6, kQuickDate = 0x4f7, kQuickText = 0x4f8,
              kQuickThumb = 0x4f9, kFolder = 0x4fa, kFolderTip = 0x4fb, kHeader = 0x4fc, kTabs = 0x4fe, kTabsBack = 0x4ff;
constexpr int kNoticeBox = 0x500, kNoticeText = 0x501, kMeter = 0x43f, kMeterTitle = 0x440;
constexpr int kThumbTarget = 0x1c9, kThumbObj = 0x56e;
constexpr int kTextRecord = 0x24, kPromptRecord = 0x2e, kNoticeRecord = 0x31, kQuickRecord = 0x33;
constexpr uint32_t kWhite = 0xffffffff, kGrey = 0xffa0a0a0;  // 0x4be48c, 0x4be498

int slotX(int i) { return (i / 5) * 0x278; }
int slotY(int i) { return (i % 5) * 0x60; }

// (1 - t) * n truncated, the x87 way.
int fade(float t, double n) {
    return static_cast<int>((1.0 - static_cast<double>(t)) * n);
}

// 0x423230's time value of a save, for the newest one.
int timeValue(const std::array<uint16_t, 8>& t) {
    return t[6] + (t[5] + (t[4] + ((t[1] + (t[0] % 100) * 12) * 31 + t[3]) * 24) * 60) * 60;
}

}  // namespace

SaveMenu::SaveMenu(Game& game) : game_(game) {}

void SaveMenu::open(int mode, bool thumbnail) {
    // 0x41a5e0
    mode_ = mode;
    if (mode != 2 && mode != 10) game_.setStoryPhase(6);
    thumbPending_ = thumbnail;
    if (thumbnail) {
        Gfx& gfx = game_.gfx();
        gfx.makeTarget(kThumbTarget, 0x80, 0x80);
        gfx.makeImage(kThumbObj, game_.scene().currentBuffer() + 0x105, 0x104d, true);
        gfx.setTarget(kThumbObj, kThumbTarget);
        gfx.setSource(kThumbObj, 0, 0, 1280, 720);
        gfx.setDest(kThumbObj, 0, 0, 128, 128);
        gfx.setMask(kThumbObj, 2);
        gfx.setFilter(kThumbObj, 2);
        game_.runEx = 1;
    }
}

void SaveMenu::openQuick(int mode) {
    // 0x41edf0
    mode_ = mode;
    game_.setStoryPhase(7);
}

void SaveMenu::takeThumbnail() {
    // 0x4391f0(0x1c9, 0xa61a90), then 0x4395d0(0x56e), 0x43e180(0x1c9).
    thumbPending_ = false;
    Gfx& gfx = game_.gfx();
    RgbaImage img;
    if (!gfx.readTarget || !gfx.readTarget(kThumbTarget, img) || img.width != 128 || img.height != 128) {
        img = RgbaImage{128, 128, Bytes(128u * 128 * 4, 0)};
    }
    game_.thumbnail = std::move(img);
    gfx.remove(kThumbObj);
    gfx.release(kThumbTarget);
}

std::string SaveMenu::slotLabel(int n) const {
    // 0x423470: "%02d%12s%04d %02d/%02d%1s%02d:%02d" of the header last read.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02d%12s%04d %02d/%02d%1s%02d:%02d", n, " ", time_[0], time_[1], time_[3], "", time_[4],
                  time_[5]);
    return buf;
}

// ---- The screen (0x41a6d0) --------------------------------------------------------------------------

int SaveMenu::step() {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    int result = 0;
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    switch (state_) {
    case 1:
        result = 0;
        if (!saved_ && !loaded_) {
            if (mode_ == 2) result = 1;
            else game_.setStoryPhase(4);
        } else {
            game_.setStoryPhase(4);
            if (mode_ == 2) {
                game_.setStoryPhase(1);
                result = 2;
            }
        }
        close();
        break;
    case 2:
        ++counter_;
        if (counter_ < dur) {
            fadeChrome(static_cast<float>(dur - counter_) / static_cast<float>(dur), false);
        } else {
            state_ = 4;
            counter_ = 0;
            fadeChrome(0, true);
        }
        break;
    case 3: {
        ++counter_;
        float t;
        bool finished = false;
        if (counter_ < dur) {
            t = static_cast<float>(counter_) / static_cast<float>(dur);
        } else {
            t = 1;
            state_ = 1;
            counter_ = 0;
            finished = true;
        }
        gfx.setVisible(kMeter, false);
        gfx.setVisible(kMeterTitle, false);
        fadeChrome(t, finished);
        break;
    }
    case 4: {
        gfx.setVisible(kMeter, true);
        gfx.setVisible(kMeterTitle, true);
        gfx.setVisible(kCursor, false);
        gfx.setSource(kPrev, 0, 0, 48, 64);
        gfx.setSource(kNext, 0, 64, 48, 64);
        gfx.moveSource(kBack, 0, 0);
        gfx.setVisible(kQuickHover, false);
        gfx.moveSource(kFolder, 0, static_cast<float>(folder_ * -0x78 + 0x78));
        gfx.moveSource(kFolderTip, 0, static_cast<float>(folder_ * -0x10 + 0x128));
        gfx.setVisible(kFolderTip, false);
        if (game_.keys.scrollDown) hover = 0x16, click = true;
        if (game_.keys.scrollUp) hover = 0x15, click = true;
        bool refresh = false;
        int& page = page_[static_cast<size_t>(folder_)];
        for (int t = 0; t < 10; ++t) {
            auto x = static_cast<float>(0x270 + 0x40 * t);
            if (page == t) {
                gfx.setRect(kTab + t, x, 72, static_cast<float>(0x40 * t), 0, 64, 32);
                gfx.setPriority(kTab + t, 0x1064);
            } else {
                gfx.setRect(kTab + t, x, 72, static_cast<float>(0x40 * t), hover - 10 == t ? 32.0f : 64.0f, 64, 32);
                gfx.setPriority(kTab + t, 0x1062);
            }
        }
        if (hover != -1) {
            if (hover < 10) {
                gfx.setVisible(kCursor, true);
                gfx.moveTo(kCursor, static_cast<float>((hover / 5) * 0x278 + 8), static_cast<float>((hover % 5) * 0x60 + 0x70));
                if (click) {
                    selected_ = hover + page * 10;
                    if (mode_ == 0 && !game_.readSaveInfo(selected_, false)) {
                        sound.se(0x22c9, 0xff);  // 0x40ea10
                        game_.writeSave(selected_);
                        state_ = 7;
                        saved_ = true;
                    } else {
                        sound.systemSe(Sound::Decide);
                        in.setGroup(2);
                        state_ = 8;
                        gfx.setTextString(kPromptRecord, mode_ == 0 ? game_.systemMessage("Overwrite file in this slot?")
                                                                     : game_.systemMessage("Load file from this slot?"));
                    }
                }
            } else if (hover < 0x14 || hover == 0x15 || hover == 0x16) {
                int dir = 0;
                slide_ = 0;
                bool go = true;
                if (hover == 0x15) {
                    gfx.setSource(kPrev, 48, 0, 48, 64);
                    if (!click) go = false;
                    else {
                        gfx.setSource(kPrev, 96, 0, 48, 64);
                        page = (page + 9) % 10;
                        dir = slide_ = 1;
                        hover = page + 10;
                    }
                }
                if (go && hover == 0x16) {
                    gfx.setSource(kNext, 48, 64, 48, 64);
                    if (!click) go = false;
                    else {
                        gfx.setSource(kNext, 96, 64, 48, 64);
                        page = (page + 1) % 10;
                        dir = slide_ = -1;
                        hover = page + 10;
                    }
                }
                if (go && click && (page != hover - 10 || refresh || dir != 0)) {
                    sound.systemSe(Sound::Page);
                    page = hover - 10;
                    state_ = 5;
                }
            } else if (hover == 0x17) {
                gfx.setVisible(kQuickHover, true);
                if (click) {
                    selected_ = -1;
                    sound.systemSe(Sound::Decide);
                    gfx.setTextString(kPromptRecord, game_.systemMessage("Load file from this slot?"));
                    in.setGroup(2);
                    state_ = 8;
                }
            } else if (hover == 0x18) {
                gfx.setVisible(kFolderTip, true);
                gfx.moveSource(kFolder, 0, static_cast<float>(folder_ * -0x78 + 0xa0));
                if (click) {
                    gfx.moveSource(kFolder, 0, static_cast<float>(folder_ * -0x78 + 200));
                    sound.systemSe(Sound::Decide);
                    slide_ = 0;
                    folder_ = folder_ == 0 ? 1 : 0;
                    state_ = 5;
                }
            } else if (hover == 0x14) {
                gfx.moveSource(kBack, 0, 24);
                if (click) {
                    gfx.moveSource(kBack, 0, 48);
                    state_ = 3;
                    sound.systemSe(Sound::Cancel);
                }
            }
        }
        if (game_.keys.cancel) {
            state_ = 3;
            sound.systemSe(Sound::Cancel);
        }
        if (lastHover_ != hover && hover != -1) sound.systemSe(Sound::Hover);
        // The prompt fades away after a "no".
        --counter_;
        float t;
        if (counter_ < 1) {
            counter_ = 0;
            t = 0;
            for (int k = 0; k < 8; ++k) gfx.setVisible(kPromptText + k, false);
        } else {
            t = static_cast<float>(dur - counter_) / static_cast<float>(dur);
        }
        promptAlpha(t);
        lastHover_ = hover;
        return finishFrame(result, hover);
    }
    case 5: {
        ++counter_;
        float t;
        if (counter_ < slideDur) {
            t = static_cast<float>(counter_) / static_cast<float>(slideDur);
        } else {
            state_ = 6;
            t = slideDur != 0 ? 1.0f : 0.0f;
            counter_ = 0;
            for (int i = 0; i < 10; ++i) fillSlot(i, false);
            int latest = latest_[static_cast<size_t>(folder_)];
            if (latest == -1 || latest / 10 != page_[static_cast<size_t>(folder_)]) {
                gfx.remove(kNew);
            } else {
                gfx.makeImage(kNew, 0x15b, 0x1064, true);
                gfx.setRect(kNew, static_cast<float>(((latest % 10) / 5) * 0x278 + 0xc0),
                            static_cast<float>(((latest % 10) % 5) * 0x60 + 0xb2), 0, 0, 160, 20);
            }
        }
        slidePage(t, true);
        break;
    }
    case 6: {
        ++counter_;
        float t;
        if (counter_ < slideDur) {
            t = static_cast<float>(slideDur - counter_) / static_cast<float>(slideDur);
        } else {
            t = 0;
            state_ = 4;
            counter_ = 0;
            // The filters back to point sampling (from the second slot on, as the engine's loop starts).
            for (int i = 1; i < 10; ++i) {
                for (int k = 0; k < 5; ++k) gfx.setFilter(kPlate + i + 10 * k, 0);
            }
            gfx.setFilter(kCursor, 0);
            gfx.setFilter(kNew, 0);
        }
        slidePage(t, false);
        break;
    }
    case 7: {
        ++counter_;
        float t;
        if (counter_ < slideDur) {
            t = static_cast<float>(counter_) / static_cast<float>(slideDur);
        } else {
            if (counter_ > 0x3b) {
                gfx.setVisible(kNoticeBox, false);
                gfx.setVisible(kNoticeText, false);
                gfx.setVisible(kPromptShade, false);
                state_ = 3;
                counter_ = 0;
            }
            t = 1;
        }
        if (counter_ == 1) {
            gfx.setVisible(kPromptShade, true);
            gfx.makeImage(kNoticeBox, 0x10a, 0x1067, true);
            gfx.setRect(kNoticeBox, 400, 304, 0, 0, 480, 112);
            gfx.makeTextSlot(kNoticeRecord, 0x1c4, mode_ == 0 ? game_.systemMessage("File Saved") : game_.systemMessage("File Loaded"), 0x1c, 0x14, 1, 0, 0,
                             0x22, 0x28, false, 0xff, 0xff, 0xff, 0, 0, 0, true);
            gfx.makeText(kNoticeText, kNoticeRecord, 0x1068, true);
            gfx.moveTo(kNoticeText, 504, 345);
        }
        gfx.setAlpha(kPromptShade, 0x4c);
        int a = static_cast<int>(static_cast<double>(t) * 256.0);
        gfx.setAlpha(kNoticeBox, a);
        gfx.setAlpha(kNoticeText, a);
        break;
    }
    case 8: {
        int next = counter_ + 1;
        float t = 0;
        counter_ = dur;
        if (next < dur) {
            t = static_cast<float>(dur - next) / static_cast<float>(dur);
            counter_ = next;
        }
        promptAlpha(t);
        for (int k = 0; k < 4; ++k) gfx.setVisible(kPromptText + k, true);
        gfx.setVisible(kPromptShade, true);
        gfx.setVisible(kYes, true);
        gfx.setVisible(kNo, true);
        gfx.moveSource(kYes, 0, 0);
        gfx.moveSource(kNo, 160, 0);
        int choice = hover;
        bool go = click;
        if (game_.settings.confirm == 0) {
            choice = 0;
            go = true;
        }
        if (choice == 0 || choice == 1) {
            gfx.moveSource(choice == 0 ? kYes : kNo, choice == 0 ? 0.0f : 160.0f, 40);
            if (go) {
                if (choice == 0) {
                    gfx.moveSource(kYes, 0, 80);
                    if (mode_ == 0) {
                        sound.se(0x22c9, 0xff);
                        game_.writeSave(selected_);
                        state_ = 9;
                        saved_ = true;
                    } else {
                        sound.stopBgm(0x1e);
                        if (selected_ == -1) {
                            if (mode_ != 2) game_.loadSave(-1);
                            selected_ = -1;
                            loadFolder_ = 0;
                        } else {
                            if (mode_ != 2) game_.loadSave(selected_);
                            loadFolder_ = folder_;
                        }
                        state_ = 9;
                        loaded_ = true;
                        sound.se(0x22c9, 0xff);
                    }
                } else {
                    gfx.moveSource(kNo, 160, 80);
                    sound.systemSe(Sound::Cancel);
                    state_ = 4;
                    in.setGroup(1);
                }
            }
        }
        if (lastHover_ != choice && choice != -1) sound.systemSe(Sound::Hover);
        if (game_.keys.cancel) {
            sound.systemSe(Sound::Cancel);
            state_ = 4;
            in.setGroup(1);
        }
        lastHover_ = choice;
        return finishFrame(result, choice);
    }
    case 9: {
        --counter_;
        float t;
        if (counter_ < 1) {
            counter_ = 0;
            t = 0;
            state_ = 7;
            for (int k = 0; k < 7; ++k) gfx.setVisible(kPromptText + k, false);
        } else {
            t = static_cast<float>(dur - counter_) / static_cast<float>(dur);
        }
        int a = fade(t, 256.0);
        for (int k = 0; k < 7; ++k) gfx.setAlpha(kPromptText + k, a);
        break;
    }
    default:
        setUp();
        state_ = 2;
        // The opening fade's first frame.
        ++counter_;
        if (counter_ < dur) {
            fadeChrome(static_cast<float>(dur - counter_) / static_cast<float>(dur), false);
        } else {
            state_ = 4;
            counter_ = 0;
            fadeChrome(0, true);
        }
        break;
    }
    return finishFrame(result, hover);
}

int SaveMenu::finishFrame(int result, int hover) {
    // The BGM's level bar (0x43f): its width follows the playing BGM's peak level, which Nyanroid does
    // not measure; it stays empty.
    lastHover_ = hover;
    return result;
}

void SaveMenu::setUp() {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    folder_ = 0;
    if (thumbPending_) takeThumbnail();
    auto load = [&](int slot, const char* name, uint32_t flags) {
        if (!gfx.load(slot, name, flags, false, "", "", 0x100, "grp")) game_.noteMissing(std::string("picture ") + name);
    };
    char name[32];
    if (mode_ == 0) {
        load(0x155, "sys_01000.tga", 2);
        load(0x159, "sys_01011.tga", 1);
    } else {
        load(0x155, "sys_02000.tga", 2);
        load(0x159, "sys_02011.tga", 1);
        load(0x175, "sys_02030.tga", 2);
        load(0x176, "sys_02031.tga", 2);
        load(0x177, "sys_02050.tga", 2);
    }
    load(0x156, "sys_09000.tga", 2);
    load(0x15a, "sys_01010.tga", 2);
    load(0x15b, "sys_01014.tga", 2);
    load(0x15c, "sys_01013.tga", 2);
    load(0x15d, "sys_01020.tga", 2);
    load(0x15e, "sys_01021.tga", 2);
    std::snprintf(name, sizeof name, "sys_%01d0100.tga", game_.message().config.winColor);
    load(0x169, name, 2);
    load(0x16a, "sys_00101.tga", 2);
    std::snprintf(name, sizeof name, "sys_%01d0110.tga", game_.message().config.winColor);
    load(0x16b, name, 2);
    load(0x173, "sys_01022.tga", 2);
    load(0x174, "sys_09100.tga", 2);
    load(0x178, "sys_01015.tga", 2);
    // Special Contents (0x419c40): the translators' sys_e1016, and its own logo whatever the chapter.
    bool special = game_.assets().build() == Build::Special;
    load(0x179, special ? "sys_e1016.tga" : "sys_01016.tga", 2);
    int flg = game_.globalInt("FLG_00");
    const char* art = "sys_04010.tga";
    if (mode_ != 2) art = flg == 0 ? "sys_05010.tga" : flg == 1 ? "sys_05011.tga" : flg == 2 ? "sys_05012.tga" : "sys_04010.tga";
    if (special) art = "sys_e4010.tga";
    load(0x17b, art, 2);
    gfx.makeImage(kArt, 0x17b, 0x1062, true);
    bool chapterArt = special || (mode_ != 2 && static_cast<unsigned>(flg) <= 2);
    gfx.setRect(kArt, 40, 0, 0, chapterArt ? 16.0f : 0.0f, 304, 168);
    gfx.makeImage(kTop, 0x155, 0x1062, true);
    gfx.makeImage(kBottom, 0x156, 0x1062, true);
    gfx.setRect(kTop, 0, 0, 0, 0, 1280, 40);
    gfx.setRect(kBottom, -64, 664, 0, 0, 1280, 56);
    gfx.makePolygon(kShade, 4, 0x1061, true);
    gfx.setDest(kShade, 0, 0, 1280, 720);
    gfx.setColor(kShade, 0x80, 0, 0, 0);
    gfx.makeImage(kCursor, 0x159, 0x1064, false);
    gfx.setRect(kCursor, 8, 112, 0, 0, 632, 96);
    // The newest save of each folder (the second, the separately installed chapter's, never exists).
    for (int f = 0; f < 2; ++f) {
        int best = -1;
        latest_[static_cast<size_t>(f)] = -1;
        if (f == 0) {
            for (int i = 0; i < 100; ++i) {
                auto info = game_.readSaveInfo(i, false);
                if (!info) continue;
                int v = timeValue(info->time);
                if (best < v) {
                    latest_[0] = i;
                    best = v;
                }
            }
        }
        if (best == -1) {
            page_[static_cast<size_t>(f)] = 0;
            latest_[static_cast<size_t>(f)] = -1;
        } else {
            page_[static_cast<size_t>(f)] = std::clamp(latest_[static_cast<size_t>(f)] / 10, 0, 9);
        }
    }
    if (latest_[0] != -1) {
        gfx.makeImage(kNew, 0x15b, 0x1063, true);
        gfx.setRect(kNew, static_cast<float>(((latest_[0] % 10) / 5) * 0x278 + 0xc0),
                    static_cast<float>(((latest_[0] % 10) % 5) * 0x60 + 0xb2), 0, 0, 160, 20);
    }
    gfx.makeImage(kHeader, 0x173, 0x1063, true);
    gfx.setRect(kHeader, 496, 608, 144, mode_ == 0 ? 0.0f : 40.0f, 288, 40);
    gfx.makeImage(kPrev, 0x173, 0x1064, true);
    gfx.setRect(kPrev, 440, 600, 0, 0, 48, 64);
    gfx.makeImage(kNext, 0x173, 0x1064, true);
    gfx.setRect(kNext, 792, 600, 0, 64, 48, 64);
    gfx.makeImage(kBack, 0x174, 0x1064, true);
    gfx.setRect(kBack, 1164, 688, 0, 0, 56, 24);
    for (int i = 0; i < 10; ++i) fillSlot(i, true);
    gfx.makeImage(kTabs, 0x15d, 0x1063, true);
    gfx.setRect(kTabs, 440, 64, 0, 0, 840, 48);
    gfx.makeImage(kTabsBack, 0x15d, 0x1062, true);
    gfx.setRect(kTabsBack, 440, 64, 0, 48, 840, 48);
    int page = page_[static_cast<size_t>(folder_)];
    for (int t = 0; t < 10; ++t) {
        int x = 0x270 + 0x40 * t;
        gfx.makeImage(kTab + t, 0x15e, 0x1063, true);
        gfx.setRect(kTab + t, static_cast<float>(x), 72, static_cast<float>(0x40 * t), page == t ? 0.0f : 64.0f, 64, 32);
        gfx.setPriority(kTab + t, page == t ? 0x1064 : 0x1062);
        in.setArea(1, t + 10, x, 0x48, 0x40, 0x20, true);
    }
    gfx.makePolygon(kPromptShade, 4, 0x1066, false);
    gfx.setDest(kPromptShade, 0, 0, 1280, 720);
    gfx.setColor(kPromptShade, 0x4c, 0, 0, 0);
    gfx.makeTextSlot(kPromptRecord, 0x1c4, game_.systemMessage("Load file from this slot?"), 0x1c, 0xf, 2, 0, 0, 0x22, 0x28, false, 0xff, 0xff,
                     0xff, 0, 0, 0, true);
    gfx.makeText(kPromptText, kPromptRecord, 0x1068, false);
    gfx.moveTo(kPromptText, 516, 304);
    gfx.makeImage(kPromptBox, 0x169, 0x1066, false);
    gfx.setRect(kPromptBox, 368, 256, 0, 0, 656, 264);
    gfx.makeImage(kYes, 0x16a, 0x1067, false);
    gfx.setRect(kYes, 472, 376, 0, 0, 160, 40);
    gfx.makeImage(kNo, 0x16a, 0x1067, false);
    gfx.setRect(kNo, 648, 376, 160, 0, 160, 40);
    in.setArea(2, 0, 0x1d8, 0x178, 0xa0, 0x28, true);
    in.setArea(2, 1, 0x288, 0x178, 0xa0, 0x28, true);
    if (mode_ != 0) {
        // The quick slot.
        gfx.makeImage(kQuickPlate, 0x10c, 0x1063, true);
        gfx.setRect(kQuickPlate, 16, 608, 0, 176, 320, 56);
        gfx.makeImage(kQuickLabel, 0x176, 0x1063, true);
        gfx.moveTo(kQuickLabel, 16, 592);
        gfx.makeImage(kQuickHover, 0x175, 0x1064, false);
        gfx.setRect(kQuickHover, 16, 608, 0, 0, 320, 56);
        if (auto info = game_.readSaveInfo(-1)) {
            bool locked = info->sortKey == 0 && !game_.assets().introductoryChapterBundled();
            if (locked) in.enableArea(1, 0x17, false);
            gfx.setRect(kQuickPlate, 16, 608, 0, 232, 320, 56);
            char date[64];
            std::snprintf(date, sizeof date, "%4d %02d/%02d %02d:%02d", info->time[0], info->time[1], info->time[3], info->time[4],
                          info->time[5]);
            gfx.makeString(kQuickDate, 0x15c, 0x1064, true, date);
            gfx.setDest(kQuickDate, 112, 616, 12, 16);
            gfx.setSource(kQuickDate, 0, 0, 12, 16);
            gfx.makeTextSlot(kQuickRecord, 0x1c4, "<F10" + info->text + ">", 0x1c, 0x16, 1, 0, 0, 0x22, 0x28, false, 0xff, 0xff,
                             0xff, 0, 0, 0, false);
            gfx.makeText(kQuickText, kQuickRecord, 0x1064, true);
            gfx.moveTo(kQuickText, 106, 628);
            gfx.setFilter(kQuickText, 2);
            gfx.loadPixels(0x17a, info->thumbnail);
            gfx.makeImage(kQuickThumb, 0x17a, 0x1064, true);
            gfx.setDest(kQuickThumb, 40, 614, 64, 36);
            gfx.setFilter(kQuickThumb, 2);
            in.setArea(1, 0x17, 16, 0x260, 0x140, 0x38, true);
            if (locked) {
                in.enableArea(1, 0x17, false);
                for (int obj : {kQuickPlate, kQuickDate, kQuickText, kQuickThumb}) gfx.setBrightness(obj, 0x40);
                gfx.setTextColor(kQuickRecord, kGrey >> 16 & 0xff, kGrey >> 8 & 0xff, kGrey & 0xff);
            }
        }
        gfx.makeImage(kFolderTip, 0x177, 0x1063, false);
        gfx.setRect(kFolderTip, 832, 640, 0, 280, 432, 16);
    }
    in.setArea(1, 0x14, 0x48c, 0x2b0, 0x38, 0x18, true);
    in.setArea(1, 0x15, 0x1b8, 600, 0x30, 0x40, true);
    in.setArea(1, 0x16, 0x318, 600, 0x30, 0x40, true);
    gfx.makeImage(kMeter, 0x116, 0x1063, false);
    gfx.setRect(kMeter, 24, 688, 0, 0, 0, 8);
    int track = game_.sound().bgmTrack();
    if (track != -1) {
        gfx.makeImage(kMeterTitle, 0x117, 0x1063, false);
        gfx.setRect(kMeterTitle, 24, 696, 0, static_cast<float>(game_.musicIndex(track) << 4), 264, 16);
    }
    in.setGroup(1);
    counter_ = 0;
    game_.runEx = 1;
}

void SaveMenu::fillSlot(int i, bool setup) {
    // A slot's plate, thumbnail, chapter and date, number and time, and the message's start.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    constexpr bool icInstalled = false;
    bool ic = game_.assets().introductoryChapterBundled();
    int page = page_[static_cast<size_t>(folder_)];
    int x0 = slotX(i), y0 = slotY(i);
    int plate = kPlate + i;
    if (setup) {
        gfx.makeImage(plate, 0x15a, 0x1063, true);
        gfx.setRect(plate, static_cast<float>(x0 + 8), static_cast<float>(y0 + 0x70), 0, static_cast<float>(folder_ * 0x60), 632, 96);
        in.setArea(1, i, x0 + 8, y0 + 0x70, 0x278, 0x60, true);
    } else {
        gfx.setVisible(plate, true);
    }
    gfx.setBrightness(plate, 0x80);
    std::optional<SaveInfo> info = folder_ == 0 ? game_.readSaveInfo(i + page * 10) : std::nullopt;
    int n = i + page * 10 + 1;
    auto empty = [&]() {
        gfx.remove(kThumb + i);
        gfx.remove(kChapter + i);
        gfx.remove(kNumber + i);
        gfx.remove(kText + i);
        gfx.remove(kMonth + i);
        gfx.remove(kDay + i);
        gfx.release(0x15f + i);
        if (!setup) gfx.moveSource(plate, 0, static_cast<float>(folder_ * 0x60));
        char label[16];
        std::snprintf(label, sizeof label, n == 100 ? "%03d" : " %02d", n);
        gfx.makeString(kNumber + i, 0x15c, 0x1065, true, label);
        gfx.setDest(kNumber + i, static_cast<float>(x0 + 0x14), static_cast<float>(y0 + 0x84), 12, 16);
        gfx.setSource(kNumber + i, 0, 0, 12, 16);
        in.enableArea(1, i, mode_ == 0);
    };
    if (!info) {
        keys_[static_cast<size_t>(i)] = 0;
        empty();
        if (!setup && folder_ != 0 && i + page * 10 > 0x5f) {
            gfx.setVisible(plate, false);
            in.enableArea(1, i, false);
        }
        return;
    }
    time_ = info->time;
    int key = info->sortKey;
    keys_[static_cast<size_t>(i)] = key;
    gfx.moveSource(plate, 632, static_cast<float>(folder_ * 0x60));
    gfx.loadPixels(0x15f + i, info->thumbnail);
    gfx.makeImage(kThumb + i, 0x15f + i, 0x1064, true);
    gfx.setDest(kThumb + i, static_cast<float>(x0 + 0x40), static_cast<float>(y0 + 0x7a), 128, 72);
    gfx.setFilter(kThumb + i, 2);
    gfx.makeImage(kChapter + i, 0x179, 0x1064, true);
    int c = (key / 10) % 10, d = key % 10;
    auto fx = static_cast<float>(x0 + 0x184), fy = static_cast<float>(y0 + 0x80);
    bool locked = c == 0 && mode_ != 0 && !icInstalled && !ic;
    // Special Contents (0x419c40): the line is row key / 10 of its sys_e1016, no date.
    if (game_.assets().build() == Build::Special) c = -1;
    switch (c) {
    case -1:
        gfx.setRect(kChapter + i, fx, fy, 0, static_cast<float>((key / 10) * 0x18), 248, 24);
        gfx.remove(kMonth + i);
        gfx.remove(kDay + i);
        break;
    case 0:
        gfx.setRect(kChapter + i, fx, fy, 0, 0, 248, 24);
        gfx.remove(kMonth + i);
        gfx.remove(kDay + i);
        break;
    case 1:
    case 2:
        gfx.setRect(kChapter + i, fx, fy, 0, static_cast<float>(c * 0x18), 248, 24);
        if (d == 0) {
            gfx.makeImage(kMonth + i, 0x178, 0x1064, true);
            int m = (key / 10000) % 100 - 1;
            gfx.setRect(kMonth + i, static_cast<float>(x0 + 0x222), fy, static_cast<float>((m / 4) * 0x28),
                        static_cast<float>((m % 4) * 0x18), 40, 24);
            char day[8];
            std::snprintf(day, sizeof day, "%02d", (key / 100) % 100);
            gfx.makeString(kDay + i, 0x15c, 0x1065, true, day);
            gfx.setRect(kDay + i, static_cast<float>(x0 + 0x252), fy, 0, 0, 12, 16);
        } else {
            gfx.makeImage(kMonth + i, 0x179, 0x1064, true);
            gfx.setRect(kMonth + i, static_cast<float>(x0 + 0x17a), fy, 0, static_cast<float>((d + 2) * 0x18), 248, 24);
            gfx.remove(kDay + i);
        }
        break;
    case 5:
        gfx.setRect(kChapter + i, fx, fy, 0, static_cast<float>((d * 3 + 0x12) * 8), 248, 24);
        gfx.remove(kMonth + i);
        gfx.remove(kDay + i);
        break;
    case 6:
        gfx.setRect(kChapter + i, fx, fy, 0, static_cast<float>((d * 3 + 0x21) * 8), 248, 24);
        gfx.remove(kMonth + i);
        gfx.remove(kDay + i);
        break;
    default:
        break;
    }
    std::string label = (n == 100 ? "" : " ") + slotLabel(n);
    gfx.makeString(kNumber + i, 0x15c, 0x1065, true, label);
    gfx.setDest(kNumber + i, static_cast<float>(x0 + 0x14), static_cast<float>(y0 + 0x84), 12, 16);
    gfx.setSource(kNumber + i, 0, 0, 12, 16);
    gfx.makeTextSlot(kTextRecord + i, 0x1c4, "<F20" + info->text + ">", 0x1c, 0x16, 1, 0, 0, 0x22, 0x28, false, 0xff, 0xff,
                     0xff, 0, 0, 0, false);
    gfx.makeText(kText + i, kTextRecord + i, 0x1064, true);
    gfx.moveTo(kText + i, static_cast<float>(x0 + 0xc4), static_cast<float>(y0 + 0x92));
    if (locked) {
        in.enableArea(1, i, false);
        for (int obj : {plate, kThumb + i, kChapter + i, kNumber + i}) gfx.setBrightness(obj, 0x40);
        gfx.setTextColor(kTextRecord + i, kGrey >> 16 & 0xff, kGrey >> 8 & 0xff, kGrey & 0xff);
    } else {
        in.enableArea(1, i, true);
    }
}

void SaveMenu::fadeChrome(float t, bool finished) {
    // States 2 and 3: the bars slide 32 px, the chapter art and the slots zoom by a tenth, everything
    // fades; t is 0 at rest.
    Gfx& gfx = game_.gfx();
    double k = static_cast<double>(t) * 32.0;
    gfx.moveTo(kTop, 0, static_cast<float>(0.0 - k));
    gfx.moveTo(kBottom, 0, static_cast<float>(k + 664.0));
    gfx.moveTo(kBack, 1164, static_cast<float>(k + 688.0));
    auto zoom = static_cast<float>(static_cast<double>(t) * 0.1 + 1.0);
    gfx.setScale(kArt, zoom);
    gfx.setPivot(kArt, 640, 360);
    int a = fade(t, 256.0);
    gfx.setAlpha(kArt, a);
    gfx.setAlpha(kTop, a);
    gfx.setAlpha(kBottom, a);
    gfx.setColor(kShade, fade(t, 128.0), 0, 0, 0);
    for (int obj = kPrev; obj <= kBack; ++obj) gfx.setAlpha(obj, a);
    gfx.setAlpha(kTop + 0x4e, a);
    gfx.setAlpha(kTop + 0x4f, a);
    for (int obj = kTab; obj < kTab + 10; ++obj) gfx.setAlpha(obj, a);
    int filter = finished ? 0 : 2;
    for (int obj : {kCursor, kNew}) {
        gfx.setAlpha(obj, a);
        gfx.setPivot(obj, 640, 360);
        gfx.setScale(obj, zoom);
        gfx.setFilter(obj, filter);
    }
    for (int obj = kPlate; obj < kPlate + 0x6e; ++obj) {
        gfx.setAlpha(obj, a);
        gfx.setPivot(obj, 640, 360);
        gfx.setScale(obj, zoom);
        gfx.setFilter(obj, filter);
    }
}

void SaveMenu::slidePage(float t, bool out) {
    // States 5 (out: towards the slide's direction, t 0 -> 1) and 6 (in: from the other side, t 1 -> 0).
    Gfx& gfx = game_.gfx();
    int a = fade(t, 256.0);
    float shift = out ? static_cast<float>(slide_ << 5) * t : static_cast<float>(slide_ * -0x20) * t;
    for (int i = 0; i < 10; ++i) {
        auto x0 = static_cast<float>(slotX(i));
        int y0 = slotY(i);
        float base = shift + 8.0f + x0;
        gfx.moveTo(kPlate + i, base, static_cast<float>(y0 + 0x70));
        gfx.moveTo(kThumb + i, base + 56.0f, static_cast<float>(y0 + 0x7a));
        auto fy = static_cast<float>(y0 + 0x80);
        gfx.moveTo(kChapter + i, base + 360.0f + 20.0f, fy);
        int key = keys_[static_cast<size_t>(i)];
        if (static_cast<unsigned>((key / 10) % 10 - 1) < 2) {
            if (key % 10 == 0) {
                gfx.moveTo(kMonth + i, base + 528.0f + 10.0f, fy);
                gfx.moveTo(kDay + i, base + 576.0f + 10.0f, fy);
            } else {
                gfx.moveTo(kMonth + i, base + 360.0f + 10.0f, fy);
            }
        }
        gfx.moveTo(kNumber + i, base + 12.0f, static_cast<float>(y0 + 0x84));
        gfx.moveTo(kText + i, base + 188.0f, static_cast<float>(y0 + 0x92));
        float zoom = out ? 1.0f - t * 0.07f : t * 0.1f + 1.0f;
        auto apply = [&](int obj) {
            gfx.setAlpha(obj, a);
            if (slide_ == 0) {
                gfx.setScale(obj, zoom);
                gfx.setPivot(obj, 640, 360);
                gfx.setFilter(obj, 2);
            }
        };
        for (int k = 0; k < 5; ++k) apply(kPlate + i + 10 * k);
        apply(kMonth + i);
        apply(kDay + i);
    }
    int latest = latest_[static_cast<size_t>(folder_)];
    if (out) {
        gfx.setAlpha(kCursor, a);
    } else {
        gfx.moveTo(kCursor, static_cast<float>(slide_ * -0x20) * t + 40.0f, 104);
        gfx.setAlpha(kCursor, a);
    }
    gfx.moveTo(kNew, shift + 8.0f + static_cast<float>(((latest % 10) / 5) * 0x278) + 184.0f,
               static_cast<float>(((latest % 10) % 5) * 0x60 + 0xb2));
    gfx.setAlpha(kNew, a);
    if (slide_ == 0) {
        float zoom = out ? 1.0f - t * 0.07f : t * 0.1f + 1.0f;
        for (int obj : {kCursor, kNew}) {
            gfx.setScale(obj, zoom);
            if (out) {
                gfx.setPivot(obj, 640, 360);
                gfx.setFilter(obj, 2);
            }
        }
    }
}

void SaveMenu::promptAlpha(float t) {
    Gfx& gfx = game_.gfx();
    int a = fade(t, 256.0);
    for (int k = 0; k < 7; ++k) gfx.setAlpha(kPromptText + k, a);
    gfx.setAlpha(kPromptShade, fade(t, 76.0));
}

void SaveMenu::close() {
    // State 1: every object and picture of the screen released; the state cleared but the slot.
    Gfx& gfx = game_.gfx();
    for (int obj = kTop; obj < kTop + 0x78; ++obj) gfx.remove(obj);
    for (int slot = 0x155; slot < 0x155 + 0x3c; ++slot) gfx.release(slot);
    gfx.remove(kMeter);
    gfx.remove(kMeterTitle);
    int keep = selected_;
    page_ = {};
    latest_ = {-1, -1};
    thumbPending_ = loaded_ = saved_ = false;
    selected_ = keep;
    game_.input.clearAreas(2);
    game_.input.clearAreas(1);
    game_.input.setGroup(0);
    state_ = 0;
}

// ---- The quick notice (0x41ee10) --------------------------------------------------------------------

int SaveMenu::stepQuick() {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    switch (state_) {
    case 1:
        game_.setStoryPhase(4);
        for (int obj = kTop + 3; obj < kTop + 0x5a; ++obj) gfx.remove(obj);
        for (int slot = 0x155 + 3; slot < 0x155 + 0x3c; ++slot) gfx.release(slot);
        {
            int keep = selected_;
            page_ = {};
            latest_ = {-1, -1};
            thumbPending_ = loaded_ = saved_ = false;
            selected_ = keep;
        }
        in.clearAreas(2);
        in.clearAreas(1);
        in.setGroup(0);
        state_ = 0;
        break;
    case 2:
    case 3: {
        // 2: the prompt's opening (then 8); 3: the closing (then 1).
        ++quickCounter_;
        float t;
        bool done = quickCounter_ >= dur;
        if (state_ == 2) t = done ? 0.0f : static_cast<float>(dur - quickCounter_) / static_cast<float>(dur);
        else t = done ? 1.0f : static_cast<float>(quickCounter_) / static_cast<float>(dur);
        if (done) {
            state_ = state_ == 2 ? 8 : 1;
            quickCounter_ = 0;
        }
        int a = fade(t, 256.0);
        auto zoom = static_cast<float>(static_cast<double>(t) * 0.1 + 1.0);
        for (int obj = kPlate; obj < kPlate + 0x6e; ++obj) {
            gfx.setAlpha(obj, a);
            gfx.setPivot(obj, 640, 360);
            gfx.setScale(obj, zoom);
            gfx.setFilter(obj, done ? 0 : 2);
        }
        break;
    }
    case 7: {
        ++quickCounter_;
        float t;
        if (quickCounter_ < slideDur) {
            t = static_cast<float>(quickCounter_) / static_cast<float>(slideDur);
        } else {
            if (quickCounter_ > 0x3b) {
                gfx.setVisible(kQuickPlate, false);
                gfx.setVisible(kQuickLabel, false);
                gfx.setVisible(kPromptShade, false);
                state_ = 3;
                quickCounter_ = 0;
            }
            t = 1;
        }
        if (quickCounter_ == 1) {
            gfx.setVisible(kPromptShade, true);
            gfx.makeImage(kQuickPlate, 0x10a, 0x1067, true);
            gfx.setRect(kQuickPlate, 400, 304, 0, 0, 480, 112);
            gfx.makeTextSlot(kNoticeRecord, 0x1c4, mode_ == 0 ? game_.systemMessage("File Saved") : game_.systemMessage("File Loaded"), 0x1c, 0x14, 1, 0, 0,
                             0x22, 0x28, false, 0xff, 0xff, 0xff, 0, 0, 0, true);
            gfx.makeText(kQuickLabel, kNoticeRecord, 0x1068, true);
            gfx.moveTo(kQuickLabel, 504, 345);
        }
        gfx.setAlpha(kPromptShade, 0x4c);
        int a = static_cast<int>(static_cast<double>(t) * 256.0);
        gfx.setAlpha(kQuickPlate, a);
        gfx.setAlpha(kQuickLabel, a);
        break;
    }
    case 8: {
        int next = quickCounter_ + 1;
        float t = 0;
        quickCounter_ = dur;
        if (next < dur) {
            t = static_cast<float>(dur - next) / static_cast<float>(dur);
            quickCounter_ = next;
        }
        int a = fade(t, 256.0);
        for (int k = 0; k < 7; ++k) gfx.setAlpha(kPromptText + k, a);
        gfx.setAlpha(kPromptShade, fade(t, 76.0));
        for (int k = 0; k < 4; ++k) gfx.setVisible(kPromptText + k, true);
        gfx.setVisible(kPromptShade, true);
        gfx.setVisible(kYes, true);
        gfx.setVisible(kNo, true);
        gfx.moveSource(kYes, 0, 0);
        gfx.moveSource(kNo, 160, 0);
        int choice = hover;
        bool go = click;
        if (game_.settings.confirm == 0) {
            choice = 0;
            go = true;
        }
        if (choice == 0 || choice == 1) {
            gfx.moveSource(choice == 0 ? kYes : kNo, choice == 0 ? 0.0f : 160.0f, 40);
            if (go) {
                if (choice == 0) {
                    gfx.moveSource(kYes, 0, 80);
                    sound.stopBgm(0x1e);
                    game_.loadSave(-1);
                    loadFolder_ = 0;
                    state_ = 9;
                    loaded_ = true;
                    sound.se(0x22c9, 0xff);
                } else {
                    gfx.moveSource(kNo, 160, 80);
                    sound.systemSe(Sound::Cancel);
                    state_ = 3;
                    in.setGroup(1);
                }
            }
        }
        if (quickLastHover_ != choice && choice != -1) sound.systemSe(Sound::Hover);
        if (game_.keys.cancel) {
            sound.systemSe(Sound::Cancel);
            state_ = 3;
            in.setGroup(1);
        }
        quickLastHover_ = choice;
        return 0;
    }
    case 9: {
        --quickCounter_;
        float t;
        if (quickCounter_ < 1) {
            state_ = 7;
            t = 0;
            quickCounter_ = 0;
            for (int k = 0; k < 7; ++k) gfx.setVisible(kPromptText + k, false);
        } else {
            t = static_cast<float>(dur - quickCounter_) / static_cast<float>(dur);
        }
        int a = fade(t, 256.0);
        for (int k = 0; k < 7; ++k) gfx.setAlpha(kPromptText + k, a);
        break;
    }
    default: {
        char name[32];
        std::snprintf(name, sizeof name, "sys_%01d0100.tga", game_.message().config.winColor);
        gfx.load(0x169, name, 2, false, "", "", 0x100, "grp");
        gfx.load(0x16a, "sys_00101.tga", 2, false, "", "", 0x100, "grp");
        std::snprintf(name, sizeof name, "sys_%01d0110.tga", game_.message().config.winColor);
        gfx.load(0x16b, name, 2, false, "", "", 0x100, "grp");
        gfx.makePolygon(kPromptShade, 4, 0x1066, false);
        gfx.setDest(kPromptShade, 0, 0, 1280, 720);
        gfx.setColor(kPromptShade, 0x4c, 0, 0, 0);
        gfx.makeTextSlot(kPromptRecord, 0x1c4, game_.systemMessage("Load Quicksave?"), 0x1c, 0xf, 2, 0, 0, 0x22, 0x28, false, 0xff, 0xff, 0xff,
                         0, 0, 0, true);
        gfx.makeText(kPromptText, kPromptRecord, 0x1068, false);
        gfx.moveTo(kPromptText, 516, 304);
        gfx.makeImage(kPromptBox, 0x169, 0x1066, false);
        gfx.setRect(kPromptBox, 368, 256, 0, 0, 656, 264);
        gfx.makeImage(kYes, 0x16a, 0x1067, false);
        gfx.setRect(kYes, 472, 376, 0, 0, 160, 40);
        gfx.makeImage(kNo, 0x16a, 0x1067, false);
        gfx.setRect(kNo, 648, 376, 160, 0, 160, 40);
        in.setArea(2, 0, 0x1d8, 0x178, 0xa0, 0x28, true);
        in.setArea(2, 1, 0x288, 0x178, 0xa0, 0x28, true);
        in.setGroup(2);
        quickCounter_ = 0;
        game_.runEx = 1;
        if (mode_ != 0) {
            if (game_.settings.confirm != 0) {
                state_ = 2;
                quickCounter_ = 0;
                return stepQuick();
            }
            game_.loadSave(-1);
        }
        state_ = 7;
        break;
    }
    }
    quickLastHover_ = hover;
    return 0;
}

}  // namespace nyan::wa2
