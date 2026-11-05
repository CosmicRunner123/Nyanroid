#include "nyan/wa2/music_room.hpp"

#include <algorithm>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// The music room's order of BGM tracks (0x4c1dd0; also 0x428210's table) and, without the
// introductory chapter, the first page's items (0x4c1ed0: indices into it).
constexpr int kOrder[64] = {4,  1,  2,  3,  5,  6,  18, 7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 27, 33, 32, 28,
                            19, 20, 21, 26, 29, 23, 24, 22, 34, 60, 31, 40, 41, 42, 43, 44, 45, 46, 47, 69, 74, 52,
                            53, 61, 50, 51, 62, 63, 67, 68, 70, 71, 54, 55, 64, 58, 59, 66, 56, 57, 65, 0};
constexpr int kNoIc[27] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28};

int itemX(int i) { return (i / 13) * 400 + 0x28; }
int itemY(int i) { return (i % 13) * 0x20 + 0x58; }

}  // namespace

MusicRoom::MusicRoom(Game& game) : game_(game) {}

int MusicRoom::track(int i) const {
    bool noIc = !game_.assets().introductoryChapterBundled();
    if (noIc && page_ == 0) return kOrder[kNoIc[std::clamp(i, 0, 26)]];
    return kOrder[std::clamp(page_ * 0x1f + i, 0, 63)];
}

bool MusicRoom::heard(int i) const {
    // Track 21 counts as heard in either version (flags 121, 135).
    const SystemData& sys = game_.system;
    int t = track(i);
    if (t == 0x15) return sys.gameFlag(0x79) != 0 || sys.gameFlag(0x87) != 0;
    return sys.gameFlag(t + 100) != 0;
}

int MusicRoom::titleRow(int i) const {
    bool noIc = !game_.assets().introductoryChapterBundled();
    if (noIc && page_ == 0) return kNoIc[std::clamp(i, 0, 26)];
    return page_ * 0x1f + i;
}

void MusicRoom::pageItems(int obj, int slot, int prio, int group, bool visible) {
    // The page's 32 items: lit plates and titles for heard tracks, plain plates for the others.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    bool noIc = !game_.assets().introductoryChapterBundled();
    for (int i = 0; i < 0x20; ++i) {
        if (noIc && page_ == 0 && i > 0x1a) {
            for (int k = 0x2f; k <= 0x33; ++k) gfx.setVisible(obj + k, false);
            for (int k = 0x57; k <= 0x5b; ++k) gfx.setVisible(obj + k, false);
            if (visible) {
                for (int k = 0x1b; k <= 0x1f; ++k) in.enableArea(group, k, false);
            }
            break;
        }
        if (!noIc && page_ == 0 && i > 0x1e) {
            if (game_.system.gameFlag(0x86) == 0) {
                if (visible) {
                    gfx.setVisible(obj + 0x32, false);
                    gfx.setVisible(obj + 0x5a, false);
                    in.enableArea(group, 0x1e, false);
                } else {
                    gfx.remove(obj + 0x32);
                    gfx.remove(obj + 0x5a);
                }
            }
            gfx.setVisible(obj + 0x33, false);
            gfx.setVisible(obj + 0x5b, false);
            if (visible) in.enableArea(group, 0x1f, false);
            break;
        }
        int x = itemX(i), y = itemY(i);
        gfx.makeImage(obj + 0x14 + i, slot + 4, prio + 2, visible);
        if (heard(i)) {
            gfx.setRect(obj + 0x14 + i, static_cast<float>(x), static_cast<float>(y), 0, 32, 400, 32);
            gfx.makeImage(obj + 0x3c + i, slot + 5, prio + 3, visible);
            gfx.setRect(obj + 0x3c + i, static_cast<float>(x + 40), static_cast<float>(y + 4), 0,
                        static_cast<float>(titleRow(i) * 0x18), 344, 24);
            in.setArea(group, i, x, y, 400, 0x20, true);
        } else {
            gfx.setRect(obj + 0x14 + i, static_cast<float>(x), static_cast<float>(y), 0, 0, 400, 32);
            gfx.setVisible(obj + 0x3c + i, false);
            in.enableArea(group, i, false);
        }
    }
}

int MusicRoom::step(int obj, int slot, int prio, int group) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    int result = 0;
    auto chrome = [&](float t, bool opening) {
        // The bars slide 32 px; everything else zooms by a tenth and fades.
        float k = t * 32.0f;
        gfx.moveTo(obj, 0, 0.0f - k);
        gfx.moveTo(obj + 1, 0, k + 664.0f);
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int o = obj; o < obj + 4; ++o) {
            if (opening) gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
        }
        if (opening) gfx.setVisible(obj + 4, true);
        gfx.setColor(obj + 4, static_cast<int>((1.0 - static_cast<double>(t)) * 128.0), 0, 0, 0);
        if (opening) gfx.setVisible(obj + 9, true);
        gfx.moveTo(obj + 9, 1164, k + 688.0f);
        gfx.setAlpha(obj + 9, a);
        float zoom = t * 0.1f + 1.0f;
        auto zoomFade = [&](int o) {
            if (opening) gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
            gfx.setScale(o, zoom);
            gfx.setPivot(o, 640, 360);
        };
        for (int o = obj + 5; o < obj + 9; ++o) zoomFade(o);
        if (opening) zoomFade(obj + 10);
        for (int i = 0; i < 0x20; ++i) {
            zoomFade(obj + 0x14 + i);
            zoomFade(obj + 0x3c + i);
        }
    };
    switch (state_) {
    case 0: {
        // The set-up.
        const SystemData& sys = game_.system;
        if (sys.gameFlag(0x68) != 0 && sys.gameFlag(0xa3) != 0) {
            game_.system.setGameFlag(0x86, 1);
            game_.writeSystem();
        }
        sound.stopBgm(0x78);
        playing_ = playingPage_ = -1;
        in.setGroup(group);
        auto load = [&](int s, const char* name, uint32_t flags, const char* grp) {
            if (!gfx.load(s, name, flags, false, "", "", 0x100, grp)) game_.noteMissing(std::string("picture ") + name);
        };
        load(slot, "sys_04000.tga", 2, "grp");
        load(slot + 1, "sys_09000.tga", 2, "grp");
        load(slot + 4, "sys_04100.tga", 0x802, "");
        load(slot + 5, "sys_04101.tga", 0x802, "");
        load(slot + 6, "sys_04010.tga", 0x802, "");
        load(slot + 7, "sys_04021.tga", 0x802, "");
        load(slot + 10, "sys_04030.tga", 0x802, "");
        load(slot + 0xb, "sys_04031.tga", 0x802, "");
        load(slot + 0x1e, "sys_05020.tga", 2, "grp");
        load(slot + 0x1f, "sys_09100.tga", 2, "grp");
        load(slot + 0x28, "sys_00301.tga", 2, "grp");
        load(slot + 0x29, "sys_00300.tga", 2, "grp");
        gfx.makeImage(obj + 0x11, slot + 0x28, prio + 2, false);
        gfx.setRect(obj + 0x11, 24, 688, 0, 0, 4, 8);
        gfx.makeImage(obj + 0x12, slot + 0x29, prio + 2, false);
        gfx.setRect(obj + 0x12, 24, 696, 0, 0, 264, 16);
        gfx.makeImage(obj, slot, prio + 1, false);
        gfx.makeImage(obj + 1, slot + 1, prio + 1, false);
        gfx.setRect(obj, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(obj + 1, 0, 664, 0, 0, 1280, 56);
        gfx.makeImage(obj + 8, slot + 6, prio + 2, false);
        gfx.setRect(obj + 8, 40, 0, 0, 0, 304, 168);
        gfx.makePolygon(obj + 4, 4, prio + 1, false);
        gfx.setDest(obj + 4, 0, 0, 1280, 720);
        gfx.setColor(obj + 4, 0x80, 0, 0, 0);
        gfx.makeImage(obj + 5, slot + 0x1e, prio + 3, false);
        gfx.setRect(obj + 5, 440, 600, 0, 0, 48, 64);
        gfx.makeImage(obj + 6, slot + 0x1e, prio + 3, false);
        gfx.setRect(obj + 6, 792, 600, 0, 64, 48, 64);
        gfx.makeImage(obj + 7, slot + 7, prio + 3, false);
        gfx.setRect(obj + 7, 496, 608, 0, 0, 288, 40);
        gfx.makeImage(obj + 9, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 9, 1164, 688, 0, 0, 56, 24);
        gfx.makeImage(obj + 10, slot + 10, prio + 3, false);
        gfx.setRect(obj + 10, 120, 528, 0, 0, 1040, 56);
        gfx.makeImage(obj + 0xb, slot + 0xb, prio + 4, false);
        gfx.makeImage(obj + 0xc, slot + 5, prio + 4, false);
        gfx.setRect(obj + 0xc, 160, 552, 0, 0, 344, 24);
        gfx.makeImage(obj + 0x13, slot + 4, prio + 3, false);
        gfx.setRect(obj + 0x13, 40, 88, 0, 96, 400, 32);
        in.setArea(group, 0x32, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group, 0x33, 0x1b8, 600, 0x30, 0x40, true);
        in.setArea(group, 0x34, 0x318, 600, 0x30, 0x40, true);
        pageItems(obj, slot, prio, group, false);
        game_.runEx = 1;
        counter_ = 0;
        state_ = 2;
        break;
    }
    case 1:
        for (int o = obj; o < obj + 200; ++o) gfx.remove(o);
        for (int s = slot + 1; s < slot + 0x1e; ++s) gfx.release(s);
        for (int g = group; g < group + 4; ++g) in.clearAreas(g);
        state_ = 0;
        result = closed_;
        break;
    case 2: {
        ++counter_;
        float t = 0;
        if (counter_ < dur) {
            t = static_cast<float>(dur - counter_) / static_cast<float>(dur);
        } else {
            counter_ = 0;
            state_ = 4;
        }
        chrome(t, true);
        break;
    }
    case 3: {
        ++counter_;
        float t = 1;
        if (counter_ < dur) {
            t = static_cast<float>(counter_) / static_cast<float>(dur);
        } else {
            counter_ = 0;
            state_ = 1;
        }
        chrome(t, false);
        gfx.setVisible(obj + 0x11, false);
        gfx.setVisible(obj + 0x12, false);
        break;
    }
    case 4: {
        int bgm = sound.bgmTrack();
        gfx.setVisible(obj + 0x11, bgm != -1);
        gfx.setVisible(obj + 0x12, bgm != -1);
        if (bgm != -1) {
            gfx.setRect(obj + 0x12, 24, 696, 0, static_cast<float>(Game::musicIndex(bgm == 0x23 ? 0x15 : bgm) << 4), 264, 16);
        }
        bool noIc = !game_.assets().introductoryChapterBundled();
        bool back = game_.keys.scrollUp || (hover == 0x33 && click);
        bool forward = !back && (game_.keys.scrollDown || (hover == 0x34 && click));
        if (back || forward) {
            // The two pages swap whichever way.
            slide_ = back ? 1 : -1;
            page_ = page_ == 0 ? 1 : 0;
            state_ = 5;
            counter_ = 0;
            sound.systemSe(Sound::Page);
            break;
        }
        for (int i = 0; i < 0x20; ++i) {
            if (noIc && page_ == 0 && i > 0x1a) break;
            if (heard(i)) gfx.setRect(obj + 0x14 + i, static_cast<float>(itemX(i)), static_cast<float>(itemY(i)), 0, 32, 400, 32);
        }
        auto showTitle = [&](int row) {
            gfx.setVisible(obj + 0xb, true);
            gfx.setRect(obj + 0xb, 536, 552, 0, static_cast<float>(row * 0x18), 512, 24);
            gfx.setVisible(obj + 0xc, true);
            gfx.setRect(obj + 0xc, 160, 552, 0, static_cast<float>(row * 0x18), 344, 24);
        };
        if (playing_ == -1) {
            gfx.setVisible(obj + 0xb, false);
            gfx.setVisible(obj + 0xc, false);
        } else {
            if (playingPage_ == page_) gfx.moveSource(obj + 0x14 + playing_, 0, 64);
            int row = noIc && playingPage_ == 0 ? playingPage_ * 0x1f + kNoIc[std::clamp(playing_, 0, 26)]
                                                 : playingPage_ * 0x1f + playing_;
            showTitle(row);
        }
        gfx.moveSource(obj + 5, 0, 0);
        gfx.moveSource(obj + 6, 0, 64);
        gfx.moveSource(obj + 9, 0, 0);
        gfx.setVisible(obj + 0x13, false);
        if (hover == -1 || hover > 0x1f) {
            if (hover == 0x33) gfx.moveSource(obj + 5, click ? 96.0f : 48.0f, 0);
            else if (hover == 0x34) gfx.moveSource(obj + 6, click ? 96.0f : 48.0f, 64);
            else if (hover == 0x32) gfx.moveSource(obj + 9, 0, click ? 48.0f : 24.0f);
        } else {
            showTitle(titleRow(hover));
            gfx.setVisible(obj + 0x13, true);
            gfx.moveTo(obj + 0x13, static_cast<float>(itemX(hover)), static_cast<float>(itemY(hover)));
            if (click) {
                game_.runEx = 1;
                int t = track(hover);
                int play = t == 0x15 && game_.system.gameFlag(0x87) != 0 ? 0x23 : t;
                if (playing_ == -1) {
                    playingPage_ = page_;
                    playing_ = hover;
                    sound.playBgm(play, 0, 1, 0xff, 1, 0);
                } else if (playing_ == hover && playingPage_ == page_) {
                    playing_ = playingPage_ = -1;
                    sound.stopBgm(0x3c);
                } else {
                    playingPage_ = page_;
                    playing_ = hover;
                    sound.playBgm(play, 0x3c, 1, 0xff, 1, 0);
                }
            }
        }
        if (game_.keys.cancel || (hover == 0x32 && click)) {
            gfx.setVisible(obj + 10, false);
            gfx.setVisible(obj + 0xb, false);
            gfx.setVisible(obj + 0xc, false);
            sound.systemSe(Sound::Cancel);
            sound.stopBgm(0x78);
            counter_ = 0;
            state_ = 3;
            closed_ = 1;
        }
        break;
    }
    case 5:
    case 6: {
        float t;
        bool out = state_ == 5;
        if (out) {
            int next = counter_ + 1;
            if (next < slideDur) {
                t = std::min(static_cast<float>(counter_ + 2) / static_cast<float>(slideDur), 1.0f);
                counter_ = next;
            } else {
                counter_ = 0;
                state_ = 6;
                t = 1;
                pageItems(obj, slot, prio, group, true);
            }
        } else {
            ++counter_;
            t = 0;
            if (counter_ < slideDur) {
                t = static_cast<float>(slideDur - counter_) / static_cast<float>(slideDur);
            } else {
                state_ = 4;
                counter_ = 0;
            }
        }
        int a = static_cast<int>(256.0 - static_cast<double>(t) * 256.0);
        float shift = out ? static_cast<float>(slide_ << 5) * t : static_cast<float>(slide_ * -0x20) * t;
        for (int i = 0; i < 0x20; ++i) {
            auto x0 = static_cast<float>((i / 13) * 400);
            int y0 = (i % 13) * 0x20;
            gfx.moveTo(obj + 0x14 + i, shift + 40.0f + x0, static_cast<float>(y0 + 0x58));
            gfx.moveTo(obj + 0x3c + i, x0 + shift + 40.0f + 40.0f, static_cast<float>(y0 + 0x5c));
            if (slide_ == 0) {
                float zoom = out ? t * 0.1f + 1.0f : 1.0f - t * 0.05f;
                gfx.setScale(obj + 0x14 + i, zoom);
                gfx.setScale(obj + 0x3c + i, zoom);
                gfx.setPivot(obj + 0x14 + i, 640, 360);
                gfx.setPivot(obj + 0x3c + i, 640, 360);
            }
            gfx.setAlpha(obj + 0x14 + i, a);
            gfx.setAlpha(obj + 0x3c + i, a);
        }
        break;
    }
    case 7:
        if (game_.keys.cancel) {
            counter_ = 0;
            state_ = 4;
        }
        break;
    default:
        break;
    }
    // The BGM level bar (obj + 0x11) follows a peak meter Nyanroid does not measure.
    return result;
}

}  // namespace nyan::wa2
