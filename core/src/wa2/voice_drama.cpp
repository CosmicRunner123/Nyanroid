#include "nyan/wa2/voice_drama.hpp"

#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// Each drama's parts (0x4bd208, seven a drama): rows of sys_e4101, -1 after the last.
constexpr int kParts[6][7] = {
    {0, 1, 2, 3, 4, 5, 6},       {7, 8, 9, 10, 11, -1, 0}, {12, 13, 14, 15, -1, 0, 0},
    {16, -1, 0, 0, 0, 0, 0},     {17, 18, -1, 0, 0, 0, 0}, {19, 20, 21, 22, 23, -1, 0},
};
// The dramas' BGM numbers (0x4beed0).
constexpr int kTracks[6] = {80, 81, 82, 83, 84, 85};
// The music room's order (0x4beef8), whose heard flags the plates still read (track + 100; 21 by
// flags 121 or 135). With the introductory chapter's flag kept at 1 the list starts at its top.
constexpr int kOrder[7] = {4, 1, 2, 3, 5, 6, 18};

int itemX(int i) { return (i / 13) * 400 + 0x28; }
int itemY(int i) { return (i % 13) * 0x20 + 0x58; }

}  // namespace

VoiceDrama::VoiceDrama(Game& game) : game_(game) {}

int VoiceDrama::part(int i) const {
    if (drama_ < 0 || drama_ > 5 || i < 0 || i > 6) return -1;
    return kParts[drama_][i];
}

int VoiceDrama::step(int obj, int slot, int prio, int group) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    const SystemData& sys = game_.system;
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int result = 0;
    auto heard = [&](int i) {
        int t = kOrder[i];
        if (t == 0x15) return sys.gameFlag(0x79) != 0 || sys.gameFlag(0x87) != 0;
        return sys.gameFlag(t + 100) != 0;
    };
    // The opening and closing fades: the bars slide 32 px, the list and the panels zoom by a tenth
    // and fade; the title's menu fades the other way.
    auto fade = [&](float t, bool opening) {
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
        zoomFade(obj + 7);
        zoomFade(obj + 8);
        if (opening) zoomFade(obj + 10);
        for (int i = 0; i < 7 && part(i) != -1; ++i) {
            zoomFade(obj + 0x14 + i);
            zoomFade(obj + 0x3c + i);
        }
        int b = static_cast<int>(static_cast<double>(t) * 256.0);
        for (int i = 0; i < 8; ++i) {
            gfx.setVisible(0xb + i, true);
            gfx.setVisible(0x15 + i, true);
            gfx.setAlpha(0xb + i, b);
            gfx.setAlpha(0x15 + i, b);
            gfx.setAlpha(0x1f + i, b);
            gfx.setVisible(0x1f + i, false);
        }
        gfx.setAlpha(2, b);
        gfx.setAlpha(0x29, b);
        gfx.setAlpha(0x2a, b);
    };
    switch (state_) {
    case 0: {
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
        char picture[16];
        std::snprintf(picture, sizeof picture, "VD%02d.tga", drama_ + 1);
        load(slot, "sys_e4000.tga", 2, "grp");
        load(slot + 1, "sys_09000.tga", 2, "grp");
        load(slot + 2, picture, 2, "grp");
        load(slot + 3, "VD_Title.tga", 2, "grp");
        load(slot + 4, "sys_04100.tga", 0x802, "");
        load(slot + 5, "sys_e4101.tga", 0x802, "");
        load(slot + 6, "sys_e4010.tga", 0x802, "");
        load(slot + 7, "sys_e4021.tga", 0x802, "");
        load(slot + 10, "sys_e4030.tga", 0x802, "");
        load(slot + 0x1f, "sys_09100.tga", 2, "grp");
        load(slot + 0x28, "sys_00301.tga", 2, "grp");
        // Now Playing's bar (0x406dc0's play counters size it; not ported, it keeps 4 px).
        gfx.makeImage(obj + 0x11, slot + 0x28, prio + 2, false);
        gfx.setRect(obj + 0x11, 24, 688, 0, 0, 4, 8);
        gfx.makeImage(obj, slot, prio + 1, false);
        gfx.makeImage(obj + 1, slot + 1, prio + 1, false);
        gfx.setRect(obj, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(obj + 1, 0, 664, 0, 0, 1280, 56);
        gfx.makeImage(obj + 2, slot + 2, prio + 2, false);
        gfx.setRect(obj + 2, 664, 80, 0, 0, 384, 376);
        gfx.makeImage(obj + 3, slot + 3, prio + 2, false);
        gfx.setRect(obj + 3, 152, 488, 0, static_cast<float>(drama_ << 5), 384, 32);
        gfx.makeImage(obj + 8, slot + 6, prio + 1, false);
        gfx.setRect(obj + 8, 45, -15, 0, 0, 304, 184);
        gfx.makeImage(obj + 7, slot + 7, prio + 3, false);
        gfx.setRect(obj + 7, 496, 608, 0, 0, 288, 40);
        gfx.makeImage(obj + 9, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 9, 1164, 688, 0, 0, 56, 24);
        gfx.makeImage(obj + 10, slot + 10, prio + 3, false);
        gfx.setRect(obj + 10, 120, 528, 0, 0, 1040, 56);
        gfx.makeImage(obj + 0xc, slot + 5, prio + 4, false);
        gfx.setRect(obj + 0xc, 160, 552, 0, 0, 344, 24);
        gfx.makeImage(obj + 0x13, slot + 4, prio + 3, false);
        gfx.setRect(obj + 0x13, 40, 88, 0, 96, 400, 32);
        in.setArea(group, 0x32, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group, 0x33, 0x1b8, 600, 0x30, 0x40, true);
        in.setArea(group, 0x34, 0x318, 600, 0x30, 0x40, true);
        for (int i = 0; i < 7 && part(i) != -1; ++i) {
            gfx.makeImage(obj + 0x14 + i, slot + 4, prio + 2, false);
            gfx.setRect(obj + 0x14 + i, static_cast<float>(itemX(i)), static_cast<float>(itemY(i)), 0, 32, 400, 32);
            gfx.makeImage(obj + 0x3c + i, slot + 5, prio + 3, false);
            gfx.setRect(obj + 0x3c + i, static_cast<float>(itemX(i) + 40), static_cast<float>(itemY(i) + 4), 0,
                        static_cast<float>(part(i) * 0x18), 344, 24);
            in.setArea(group, i, itemX(i), itemY(i), 400, 0x20, true);
        }
        game_.runEx = 1;
        counter_ = 0;
        lastHover_ = -1;
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
        fade(t, true);
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
        fade(t, false);
        gfx.setVisible(obj + 0x11, false);
        break;
    }
    case 4: {
        gfx.setVisible(obj + 0x11, sound.bgmTrack() != -1);
        for (int i = 0; i < 7 && part(i) != -1; ++i) {
            if (heard(i)) gfx.setRect(obj + 0x14 + i, static_cast<float>(itemX(i)), static_cast<float>(itemY(i)), 0, 32, 400, 32);
        }
        if (playing_ == -1) {
            gfx.setVisible(obj + 0xb, false);
            gfx.setVisible(obj + 0xc, false);
        } else {
            if (playingPage_ == 0) gfx.moveSource(obj + 0x14 + playing_, 0, 64);
            gfx.setVisible(obj + 0xc, true);
            gfx.setRect(obj + 0xc, 160, 552, 0, static_cast<float>((playingPage_ * 0x1f + part(playing_)) * 0x18), 344, 24);
        }
        gfx.moveSource(obj + 9, 0, 0);
        gfx.setVisible(obj + 0x13, false);
        if (hover == -1 || hover > 0x1f) {
            if (hover == 0x32) gfx.moveSource(obj + 9, 0, click ? 48.0f : 24.0f);
        } else {
            // The hovered part's title in the strip, the marker on its plate; a click plays or stops it.
            gfx.setVisible(obj + 0xc, true);
            gfx.setRect(obj + 0xc, 160, 552, 0, static_cast<float>(part(hover) * 0x18), 344, 24);
            gfx.setVisible(obj + 0x13, true);
            gfx.moveTo(obj + 0x13, static_cast<float>(itemX(hover)), static_cast<float>(itemY(hover)));
            if (click) {
                game_.runEx = 1;
                int fadeIn = 0;
                bool play = true;
                if (playing_ != -1) {
                    if (playing_ != hover) gfx.moveSource(obj + 0x14 + playing_, 0, 32);
                    if (playing_ == hover && playingPage_ == 0) {
                        gfx.moveSource(obj + 0x14 + playing_, 0, 32);
                        playing_ = playingPage_ = -1;
                        sound.stopBgm(0x3c);
                        play = false;
                    } else {
                        fadeIn = 0x3c;
                    }
                }
                if (play) {
                    playing_ = hover;
                    playingPage_ = 0;
                    sound.dramaPart = hover;
                    sound.playBgm(kTracks[drama_ < 0 || drama_ > 5 ? 0 : drama_], fadeIn, 1, 0xff, 1, 0);
                }
            }
        }
        if (game_.keys.cancel || (hover == 0x32 && click)) {
            gfx.load(0, "Te000.tga", 2, false, "", "", 0x100, "");
            gfx.setRect(1, 0, 0, 0, 136, 1280, 720);
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
    default:
        break;
    }
    lastHover_ = hover;
    return result;
}

}  // namespace nyan::wa2
