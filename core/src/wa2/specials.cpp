#include "nyan/wa2/specials.hpp"

#include <algorithm>
#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

// ---- Voice Message (0x42a520) ----------------------------------------------------------------------

namespace {

// The five panels (x, y, w, h; 0x4c21a8) and their heroines (the function's local table).
constexpr int kPanels[5][4] = {{0, 29, 256, 646}, {256, 29, 256, 646}, {512, 29, 256, 646}, {768, 29, 256, 646}, {1024, 29, 256, 646}};
constexpr int kHeroine[5] = {4, 2, 1, 3, 5};

}  // namespace

VoiceMessage::VoiceMessage(Game& game) : game_(game) {}

int VoiceMessage::step(int obj, int slot, int prio, int group) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int result = 0;
    auto chrome = [&](float t, bool opening) {
        float k = t * 32.0f;
        gfx.moveTo(obj, 0, 0.0f - k);
        gfx.moveTo(obj + 1, 0, k + 664.0f);
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int o = obj; o < obj + 4; ++o) {
            gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
        }
        if (opening) gfx.setVisible(obj + 4, true);
        gfx.setColor(obj + 4, static_cast<int>((1.0 - static_cast<double>(t)) * 128.0), 0, 0, 0);
        gfx.setVisible(obj + 9, true);
        gfx.moveTo(obj + 9, 1164, k + 688.0f);
        gfx.setAlpha(obj + 9, a);
        float zoom = t * 0.1f + 1.0f;
        auto zoomFade = [&](int o) {
            gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
            gfx.setScale(o, zoom);
            gfx.setPivot(o, 640, 360);
        };
        if (!opening) {
            gfx.setAlpha(obj + 10, a);
            gfx.setScale(obj + 10, zoom);
            gfx.setPivot(obj + 10, 640, 360);
        }
        for (int o = obj + 5; o < obj + 9; ++o) zoomFade(o);
        for (int i = 0; i < 5; ++i) zoomFade(obj + 0x14 + i);
        if (!opening) zoomFade(obj + 0x32);
    };
    switch (state_) {
    case 0: {
        sound.stopBgm(0x78);
        playing_ = -1;
        in.setGroup(group);
        auto load = [&](int s, const char* name, uint32_t flags, const char* grp) {
            if (!gfx.load(s, name, flags, false, "", "", 0x100, grp)) game_.noteMissing(std::string("picture ") + name);
        };
        load(slot, "sys_07000.tga", 2, "grp");
        load(slot + 1, "sys_09000.tga", 2, "grp");
        load(slot + 4, "sys_07100.tga", 0x802, "");
        load(slot + 6, "sys_07020.tga", 0x802, "");
        load(slot + 7, "sys_07021.tga", 0x802, "");
        load(slot + 0x1e, "sys_09100.tga", 2, "grp");
        load(slot + 0x28, "sys_00301.tga", 2, "grp");
        gfx.makeImage(obj + 0x1c, slot + 0x28, prio + 6, false);
        gfx.setRect(obj + 0x1c, 24, 688, 0, 0, 4, 8);
        gfx.makeImage(obj, slot, prio + 5, false);
        gfx.makeImage(obj + 1, slot + 1, prio + 5, false);
        gfx.setRect(obj, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(obj + 1, 0, 664, 0, 0, 1280, 56);
        gfx.makeImage(obj + 7, slot + 7, prio + 3, false);
        gfx.setRect(obj + 7, 496, 608, 0, 0, 288, 40);
        gfx.makeImage(obj + 9, slot + 0x1e, prio + 6, false);
        gfx.setRect(obj + 9, 1164, 688, 0, 0, 56, 24);
        gfx.makeImage(obj + 10, slot + 6, prio + 6, false);
        gfx.setRect(obj + 10, 1056, 624, 0, 0, 160, 40);
        in.setArea(group, 0x20, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group + 1, 0x20, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group + 1, 0x21, 0x420, 0x270, 0xa0, 0x28, true);
        for (int i = 0; i < 5; ++i) {
            const int* p = kPanels[i];
            gfx.makeImage(obj + 0x14 + i, slot + 4, prio + 3, false);
            gfx.setRect(obj + 0x14 + i, static_cast<float>(p[0]), 0, static_cast<float>(p[0]), 0, static_cast<float>(p[2]), 720);
            gfx.setBrightness(obj + 0x14 + i, 0x40);
            in.setArea(group, i, p[0], p[1], p[2], p[3], true);
        }
        game_.runEx = 1;
        counter_ = 0;
        state_ = 2;
        break;
    }
    case 1:
        // The bars and Back (objects 0..9) stay, faded out, as the engine leaves them.
        for (int o = obj + 10; o < obj + 200; ++o) gfx.remove(o);
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
        break;
    }
    case 4:
        for (int i = 0; i < 5; ++i) gfx.setBrightness(obj + 0x14 + i, 0x40);
        gfx.moveSource(obj + 9, 0, 0);
        if (hover >= 0 && hover <= 4) {
            gfx.setBrightness(obj + 0x14 + hover, 0x80);
            if (click) {
                int c = kHeroine[hover];
                playing_ = hover;
                char name[32];
                std::snprintf(name, sizeof name, "sys_072%02d.tga", c - 1);
                if (!gfx.load(slot + 8, name, 0x802, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
                gfx.makeImage(obj + 0x32, slot + 8, prio + 4, false);
                gfx.setVisible(obj + 10, true);
                counter_ = 0;
                state_ = 5;
                sound.playVoiceMessage(c, 0xff);
                sound.systemSe(Sound::Decide);
                sound.stopBgm(0x78);
                in.setGroup(group + 1);
            }
        } else if (hover == 0x20) {
            gfx.moveSource(obj + 9, 0, 24);
        }
        if (game_.keys.cancel || (hover == 0x20 && click)) {
            sound.systemSe(Sound::Cancel);
            gfx.moveSource(obj + 9, 0, 48);
            counter_ = 0;
            state_ = 3;
            closed_ = 1;
        }
        break;
    case 5:
        ++counter_;
        gfx.setVisible(obj + 0x32, true);
        gfx.setRect(obj + 0x32, 0, 0, 0, 0, 1280, 720);
        gfx.setAlpha(obj + 0x32, counter_ << 4);
        if (playing_ != -1) gfx.setVisible(obj + 0x1c, true);  // the voice's level bar: not measured
        gfx.moveSource(obj + 9, 0, 0);
        gfx.moveSource(obj + 10, 0, 0);
        if (hover == 0x20) {
            gfx.moveSource(obj + 9, 0, 24);
            if (click) {
                gfx.moveSource(obj + 9, 0, 48);
                gfx.setVisible(obj + 0x1c, false);
                playing_ = -1;
                sound.stopVoice(0, 0);
                counter_ = 0;
                state_ = 3;
                closed_ = 1;
            }
        } else {
            if (hover == 0x21) gfx.moveSource(obj + 10, 0, click ? 80.0f : 40.0f);
            if (game_.keys.cancel || sound.voiceDone(0, 0, 1) || (hover == 0x21 && click)) {
                if (!sound.voiceDone(0, 0, 0)) sound.systemSe(Sound::Cancel);
                sound.stopVoice(0, 0);
                playing_ = -1;
                gfx.setVisible(obj + 0x1c, false);
                gfx.setVisible(obj + 10, false);
                gfx.remove(obj + 0x32);
                counter_ = 0;
                state_ = 4;
                in.setGroup(group);
            }
        }
        break;
    default:
        break;
    }
    return result;
}

// ---- CG Mode (0x42b300) -----------------------------------------------------------------------------

namespace {

// 0x4c01b0: 14 pages of 12 entries, each up to ten CG numbers (gallery flag kind 0); 0 none.
constexpr int kCgTable[168][10] = {
    {100100, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {100200, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {100300, 100301, 0, 0, 0, 0, 0, 0, 0, 0}, {100400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {100500, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {100600, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {102300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {102500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {102700, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {100700, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {102600, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {100800, 100801, 100802, 0, 0, 0, 0, 0, 0, 0},
    {101000, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {101100, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {103300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {101200, 101201, 101202, 101203, 101204, 101205, 101206, 101207, 0, 0},
    {101300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {102400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {101400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {101500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {101600, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {101700, 101701, 0, 0, 0, 0, 0, 0, 0, 0},
    {101800, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {101900, 101901, 0, 0, 0, 0, 0, 0, 0, 0},
    {102000, 102001, 0, 0, 0, 0, 0, 0, 0, 0}, {102200, 102201, 102202, 102203, 102204, 0, 0, 0, 0, 0},
    {103200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {200000, 200001, 200002, 0, 0, 0, 0, 0, 0, 0}, {200100, 200101, 200102, 200103, 200104, 0, 0, 0, 0, 0},
    {200200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {200300, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {211200, 211201, 0, 0, 0, 0, 0, 0, 0, 0}, {211400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {200400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {201800, 201801, 0, 0, 0, 0, 0, 0, 0, 0},
    {200500, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {200501, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {200600, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {205600, 205601, 0, 0, 0, 0, 0, 0, 0, 0},
    {200700, 200701, 0, 0, 0, 0, 0, 0, 0, 0}, {200201, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {200302, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {200800, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {200801, 200802, 200803, 200804, 0, 0, 0, 0, 0, 0}, {200900, 200901, 0, 0, 0, 0, 0, 0, 0, 0},
    {201000, 201001, 0, 0, 0, 0, 0, 0, 0, 0}, {201101, 201100, 201103, 201104, 201102, 0, 0, 0, 0, 0},
    {201200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {201300, 201301, 0, 0, 0, 0, 0, 0, 0, 0},
    {201400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {201500, 201501, 0, 0, 0, 0, 0, 0, 0, 0},
    {201600, 201601, 0, 0, 0, 0, 0, 0, 0, 0}, {201700, 201701, 0, 0, 0, 0, 0, 0, 0, 0},
    {201900, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {202000, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {202100, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {220000, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {202200, 202201, 202202, 0, 0, 0, 0, 0, 0, 0}, {202300, 202301, 0, 0, 0, 0, 0, 0, 0, 0},
    {202400, 202401, 202402, 0, 0, 0, 0, 0, 0, 0}, {202500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {202600, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {202700, 202701, 202702, 202703, 202704, 0, 0, 0, 0, 0},
    {202800, 202801, 202802, 0, 0, 0, 0, 0, 0, 0}, {202900, 202901, 0, 0, 0, 0, 0, 0, 0, 0},
    {203000, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {203100, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {203200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {203300, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {203400, 203401, 203402, 203403, 203404, 203405, 203406, 0, 0, 0}, {203500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {203600, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {203700, 203701, 0, 0, 0, 0, 0, 0, 0, 0},
    {203800, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {203900, 203901, 0, 0, 0, 0, 0, 0, 0, 0},
    {204000, 204001, 0, 0, 0, 0, 0, 0, 0, 0}, {204100, 204101, 0, 0, 0, 0, 0, 0, 0, 0},
    {204200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {204300, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {204400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {204500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {204600, 204601, 0, 0, 0, 0, 0, 0, 0, 0}, {204700, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {220400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {204800, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {211300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {204900, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {205000, 205001, 205002, 205003, 0, 0, 0, 0, 0, 0}, {205100, 205101, 205102, 0, 0, 0, 0, 0, 0, 0},
    {205200, 205201, 0, 0, 0, 0, 0, 0, 0, 0}, {205300, 205301, 205302, 0, 0, 0, 0, 0, 0, 0},
    {205400, 205401, 205402, 0, 0, 0, 0, 0, 0, 0}, {205500, 205501, 0, 0, 0, 0, 0, 0, 0, 0},
    {205700, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {205800, 205801, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {205900, 205901, 205902, 205903, 205904, 0, 0, 0, 0, 0}, {206000, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {206100, 206101, 206102, 0, 0, 0, 0, 0, 0, 0}, {206200, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {206300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {206400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {206500, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {206600, 206601, 0, 0, 0, 0, 0, 0, 0, 0},
    {206700, 206800, 206801, 206802, 0, 0, 0, 0, 0, 0}, {206900, 206901, 0, 0, 0, 0, 0, 0, 0, 0},
    {207000, 207001, 207002, 0, 0, 0, 0, 0, 0, 0}, {207100, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {207200, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {207300, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {207400, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {220500, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {220100, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {207600, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {220600, 220601, 0, 0, 0, 0, 0, 0, 0, 0}, {208000, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {208100, 208101, 208102, 0, 0, 0, 0, 0, 0, 0}, {208200, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {208300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {208400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {208500, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {208600, 208601, 208603, 208602, 0, 0, 0, 0, 0, 0},
    {208700, 208702, 208701, 0, 0, 0, 0, 0, 0, 0}, {208800, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {208900, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {209000, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {209100, 209101, 0, 0, 0, 0, 0, 0, 0, 0}, {209200, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {209300, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {209400, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {209500, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {207003, 207004, 0, 0, 0, 0, 0, 0, 0, 0},
    {209700, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {209701, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {209702, 209703, 0, 0, 0, 0, 0, 0, 0, 0}, {209800, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {209900, 209901, 209902, 0, 0, 0, 0, 0, 0, 0}, {210000, 210001, 210002, 0, 0, 0, 0, 0, 0, 0},
    {210100, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {210200, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {210300, 210301, 0, 0, 0, 0, 0, 0, 0, 0}, {210400, 210401, 210402, 210403, 210404, 210405, 210406, 210407, 0, 0},
    {210500, 210501, 210502, 210503, 0, 0, 0, 0, 0, 0}, {210600, 210601, 210602, 210603, 210604, 210605, 0, 0, 0, 0},
    {210700, 210701, 0, 0, 0, 0, 0, 0, 0, 0}, {210800, 210801, 0, 0, 0, 0, 0, 0, 0, 0},
    {210900, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {211000, 211001, 0, 0, 0, 0, 0, 0, 0, 0},
    {220200, 220201, 0, 0, 0, 0, 0, 0, 0, 0}, {211100, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

int thumbX(int i) { return (i & 3) * 0x110; }
int thumbY(int i) { return (i >> 2) * 0xa0; }

}  // namespace

CgMode::CgMode(Game& game) : game_(game) {}

bool CgMode::seen(int entry) const {
    // Any of the entry's ten numbers seen (a 0 reads the flag of CG 0, as the engine does).
    for (int n : kCgTable[entry]) {
        if (game_.system.galleryFlag(0, n) != 0) return true;
    }
    return false;
}

int CgMode::firstSeen(int entry) const {
    for (int n : kCgTable[entry]) {
        if (game_.system.galleryFlag(0, n) != 0) return n;
    }
    return 0;
}

int CgMode::seenVariant(int entry, int k) const {
    int found = 0;
    for (int i = 0; i < 10; ++i) {
        if (game_.system.galleryFlag(0, kCgTable[entry][i]) != 0) {
            if (found == k) return i;
            ++found;
        }
    }
    return -1;
}

void CgMode::thumbnails(int obj, int slot, int prio, int group, bool visible) {
    // The page's twelve: thumbnails of seen CGs in their frames, placeholders for the others.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    for (int i = 0; i < 12; ++i) {
        int entry = i + page_ * 12;
        int n = kCgTable[entry][0];
        int x = thumbX(i), y = thumbY(i);
        if (!visible) in.setArea(group, i, x + 0x68, y + 0x74, 0x100, 0x90, true);
        if (n == 0) {
            gfx.remove(obj + 0x1e + i);
            gfx.remove(obj + 0x32 + i);
            in.enableArea(group, i, false);
            continue;
        }
        char name[32];
        if (!seen(entry)) {
            std::snprintf(name, sizeof name, "%s", page_ < 3 ? "sys_05900.tga" : page_ < 9 ? "sys_05901.tga" : "sys_05902.tga");
            in.enableArea(group, i, false);
            if (!gfx.load(slot + 10 + i, name, 0x802, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
            gfx.makeImage(obj + 0x1e + i, slot + 10 + i, prio + 3, visible);
            gfx.setRect(obj + 0x1e + i, static_cast<float>(x + 0x60), static_cast<float>(y + 0x70), 0, 0, 272, 160);
            gfx.remove(obj + 0x32 + i);
        } else {
            if (n == 0x3357c || n == 0x335e0) n = firstSeen(entry);
            std::snprintf(name, sizeof name, "tv%06d.tga", n);
            if (visible) in.enableArea(group, i, true);
            if (!gfx.load(slot + 10 + i, name, 0x802, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
            gfx.makeImage(obj + 0x1e + i, slot + 10 + i, prio + 3, visible);
            gfx.setRect(obj + 0x1e + i, static_cast<float>(x + 0x68), static_cast<float>(y + 0x74), 0, 0, 256, 144);
            gfx.makeImage(obj + 0x32 + i, slot + 6, prio + 4, visible);
            gfx.setRect(obj + 0x32 + i, static_cast<float>(x + 0x60), static_cast<float>(y + 0x70), 0, 0, 272, 160);
        }
    }
}

int CgMode::step(int obj, int slot, int prio, int group) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    const SystemData& sys = game_.system;
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    int result = 0;
    bool icPages = game_.assets().introductoryChapterBundled() && sys.gameFlag(0) != 0;
    bool codaPages = sys.gameFlag(5) != 0;
    int tabShift = icPages ? 0 : 3;  // the tabs' art moves left without the first three
    if (!icPages && page_ < 3) page_ = 3;
    if (!codaPages && page_ > 8) page_ = 8;
    auto chrome = [&](float t, bool opening) {
        float k = t * 32.0f;
        gfx.moveTo(obj, 0, 0.0f - k);
        gfx.moveTo(obj + 1, 0, k + 664.0f);
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int o = obj; o < obj + 4; ++o) {
            gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
        }
        if (opening) gfx.setVisible(obj + 4, true);
        gfx.setColor(obj + 4, static_cast<int>((1.0 - static_cast<double>(t)) * 128.0), 0, 0, 0);
        if (opening) gfx.setVisible(obj + 9, true);
        gfx.moveTo(obj + 9, 1164, k + 688.0f);
        gfx.setAlpha(obj + 9, a);
        float zoom = t * 0.1f + 1.0f;
        auto zoomFade = [&](int o) {
            gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
            gfx.setScale(o, zoom);
            gfx.setPivot(o, 640, 360);
        };
        for (int o = obj + 5; o < obj + 9; ++o) zoomFade(o);
        for (int o = obj + 10; o < obj + 25; ++o) zoomFade(o);
        for (int i = 0; i < 12; ++i) {
            zoomFade(obj + 0x1e + i);
            zoomFade(obj + 0x32 + i);
        }
    };
    auto artSlot = [&](int page) { return slot + (page < 3 ? 0x21 : page < 9 ? 0x22 : 0x23); };
    switch (state_) {
    case 0: {
        sound.playBgm(0x29, 0x1e, 1, 0xff, 0, 0);
        buffer_ = 0;
        in.setGroup(group);
        auto load = [&](int s, const char* name) {
            if (!gfx.load(s, name, 2, false, "", "", 0x100, "grp")) game_.noteMissing(std::string("picture ") + name);
        };
        load(slot, "sys_05000.tga");
        load(slot + 1, "sys_09000.tga");
        load(slot + 4, "sys_05100.tga");
        load(slot + 6, "sys_05110.tga");
        load(slot + 0x1e, "sys_09100.tga");
        load(slot + 0x1f, "sys_05020.tga");
        load(slot + 0x20, "sys_05021.tga");
        load(slot + 0x21, "sys_05010.tga");
        load(slot + 0x22, "sys_05011.tga");
        load(slot + 0x23, "sys_05012.tga");
        load(slot + 0x24, "sys_05200.tga");
        load(slot + 0x25, "sys_05201.tga");
        load(slot + 0x28, "sys_00301.tga");
        load(slot + 0x29, "sys_00300.tga");
        if (sound.bgmTrack() != -1) {
            gfx.makeImage(obj + 0x1c, slot + 0x28, prio + 2, false);
            gfx.setRect(obj + 0x1c, 24, 688, 0, 0, 4, 8);
            gfx.makeImage(obj + 0x1d, slot + 0x29, prio + 2, false);
            gfx.setRect(obj + 0x1d, 24, 696, 0, static_cast<float>(Game::musicIndex(sound.bgmTrack()) << 4), 264, 16);
        }
        gfx.makeImage(obj, slot, prio + 1, false);
        gfx.makeImage(obj + 1, slot + 1, prio + 1, false);
        gfx.setRect(obj, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(obj + 1, -64, 664, 0, 0, 1280, 56);
        gfx.makePolygon(obj + 4, 4, prio + 1, false);
        gfx.setDest(obj + 4, 0, 0, 1280, 720);
        gfx.setColor(obj + 4, 0x80, 0, 0, 0);
        gfx.makeImage(obj + 5, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 5, 440, 600, 0, 0, 48, 64);
        gfx.makeImage(obj + 6, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 6, 792, 600, 0, 64, 48, 64);
        gfx.makeImage(obj + 7, slot + 0x20, prio + 3, false);
        gfx.setRect(obj + 7, 496, 608, 0, 0, 288, 40);
        gfx.makeImage(obj + 8, artSlot(page_), prio + 2, false);
        gfx.setRect(obj + 8, 40, 0, 0, 16, 304, 168);
        gfx.makeImage(obj + 9, slot + 0x1e, prio + 3, false);
        gfx.setRect(obj + 9, 1164, 688, 0, 0, 56, 24);
        gfx.makeImage(obj + 0x18, slot + 0x24, prio + 3, false);
        gfx.setRect(obj + 0x18, 280, 86, 0, 0, 1000, 24);
        int x = (codaPages ? 0 : 300) + 0x1ac;
        for (int t = 0; t < 14; ++t) {
            int o = obj + 10 + t;
            gfx.makeImage(o, slot + 0x25, prio + 3, false);
            gfx.setPriority(o, page_ == t ? prio + 4 : prio + 2);
            gfx.setRect(o, static_cast<float>(x), 72, static_cast<float>((t - tabShift) * 0x3c), page_ == t ? 0.0f : 64.0f, 60, 32);
            in.setArea(group, 20 + t, x, 0x48, 0x3c, 0x20, true);
            if ((t < 3 && !icPages) || (t > 8 && !codaPages)) {
                gfx.remove(o);
                in.enableArea(group, 20 + t, false);
            }
            x += 0x3c;
        }
        thumbnails(obj, slot, prio, group, false);
        in.setArea(group, 0x28, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group, 0x29, 0x1b8, 600, 0x30, 0x40, true);
        in.setArea(group, 0x2a, 0x318, 600, 0x30, 0x40, true);
        game_.runEx = 1;
        viewCounter_ = 0;
        lastHover_ = -1;
        state_ = 2;
        break;
    }
    case 1:
        for (int o = obj; o < obj + 200; ++o) gfx.remove(o);
        for (int s = slot; s < slot + 0x32; ++s) gfx.release(s);
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
        gfx.setVisible(obj + 0x1c, false);
        gfx.setVisible(obj + 0x1d, false);
        break;
    }
    case 4: {
        gfx.setVisible(obj + 0x1c, true);
        gfx.setVisible(obj + 0x1d, true);
        prevPage_ = page_;
        if (game_.keys.scrollUp || (hover == 0x29 && click)) {
            slide_ = 1;
            state_ = 5;
            page_ = (0xe - (0xf - page_) % 0xe) % 0xe;
            if ((!icPages && page_ < 3) || page_ < 0) page_ = codaPages ? 13 : 8;
            counter_ = 0;
            sound.systemSe(Sound::Page);
        }
        if (game_.keys.scrollDown || (hover == 0x2a && click)) {
            slide_ = -1;
            state_ = 5;
            page_ = (0xe - (0xd - page_) % 0xe) % 0xe;
            if ((!codaPages && page_ > 8) || page_ > 13) page_ = icPages ? 0 : 3;
            counter_ = 0;
            sound.systemSe(Sound::Page);
        }
        for (int t = 0; t < 14; ++t) {
            gfx.setPriority(obj + 10 + t, prio + 2);
            gfx.moveSource(obj + 10 + t, static_cast<float>((t - tabShift) * 0x3c), 64);
        }
        gfx.setPriority(obj + 10 + page_, prio + 4);
        gfx.moveSource(obj + 10 + page_, static_cast<float>((page_ - tabShift) * 0x3c), 0);
        for (int i = 0; i < 12; ++i) gfx.setSlot(obj + 0x32 + i, slot + 6);
        gfx.moveSource(obj + 5, 0, 0);
        gfx.moveSource(obj + 6, 0, 64);
        gfx.moveSource(obj + 9, 0, 0);
        if (hover == -1 || hover > 11) {
            variant_ = 0;
            if (hover >= 20 && hover < 34) {
                if (hover - 20 != page_) {
                    gfx.moveSource(obj + hover - 10, static_cast<float>((hover - tabShift - 20) * 0x3c), 32);
                    if (click) {
                        sound.systemSe(Sound::Page);
                        slide_ = 0;
                        counter_ = 0;
                        page_ = hover - 20;
                        state_ = 5;
                    }
                }
            } else if (hover == 0x29) {
                gfx.moveSource(obj + 5, click ? 96.0f : 48.0f, 0);
            } else if (hover == 0x2a) {
                gfx.moveSource(obj + 6, click ? 96.0f : 48.0f, 64);
            } else if (hover == 0x28) {
                gfx.moveSource(obj + 9, 0, click ? 48.0f : 24.0f);
            }
        } else {
            gfx.setSlot(obj + 0x32 + hover, slot + 4);
            if (click) {
                selected_ = hover + page_ * 12;
                variant_ = 0;
                viewCounter_ = 0;
                state_ = 7;
                sound.systemSe(Sound::Decide);
            }
        }
        if (game_.keys.cancel || (hover == 0x28 && click)) {
            viewCounter_ = 0;
            counter_ = 0;
            state_ = 3;
            closed_ = 1;
            sound.systemSe(Sound::Cancel);
        }
        if (lastHover_ != hover && hover != -1) sound.systemSe(Sound::Hover);
        lastHover_ = hover;
        return 0;
    }
    case 5:
    case 6: {
        bool out = state_ == 5;
        float t;
        if (out) {
            for (int i = 0; i < 12; ++i) gfx.setSlot(obj + 0x32 + i, slot + 6);
            int next = counter_ + 1;
            if (next < slideDur) {
                t = std::min(static_cast<float>(counter_ + 2) / static_cast<float>(slideDur), 1.0f);
                counter_ = next;
            } else {
                state_ = 6;
                t = slideDur == 0 ? 0.0f : 1.0f;
                counter_ = 0;
                if (artSlot(page_) != artSlot(prevPage_)) gfx.setSlot(obj + 8, artSlot(page_));
                thumbnails(obj, slot, prio, group, true);
            }
        } else {
            ++counter_;
            t = 0;
            if (counter_ < slideDur) t = static_cast<float>(slideDur - counter_) / static_cast<float>(slideDur);
            else {
                state_ = 4;
                counter_ = 0;
            }
        }
        int a = static_cast<int>(256.0 - static_cast<double>(t) * 256.0);
        float shift = out ? static_cast<float>(slide_ << 5) * t : static_cast<float>(slide_ * -0x20) * t;
        for (int i = 0; i < 12; ++i) {
            auto x0 = static_cast<float>(thumbX(i));
            int y0 = thumbY(i);
            gfx.moveTo(obj + 0x1e + i, shift + 104.0f + x0, static_cast<float>(y0 + 0x74));
            gfx.moveTo(obj + 0x32 + i, shift + 104.0f + x0 - 8.0f, static_cast<float>(y0 + 0x70));
            if (slide_ == 0) {
                float zoom = out ? t * 0.1f + 1.0f : 1.0f - t * 0.05f;
                gfx.setScale(obj + 0x1e + i, zoom);
                gfx.setScale(obj + 0x32 + i, zoom);
                gfx.setPivot(obj + 0x1e + i, 640, 360);
                gfx.setPivot(obj + 0x32 + i, 640, 360);
            }
            int entry = i + page_ * 12;
            if (!out && kCgTable[entry][0] != 0 && !seen(entry)) {
                gfx.moveTo(obj + 0x1e + i, shift + 96.0f + x0, static_cast<float>(y0 + 0x70));
            }
            gfx.setAlpha(obj + 0x1e + i, a);
            gfx.setAlpha(obj + 0x32 + i, a);
        }
        break;
    }
    case 7: {
        // The CG zooms out of its thumbnail over 16 frames.
        int view = obj + 0x46 + buffer_;
        if (viewCounter_ == 0) {
            int v = seenVariant(selected_, variant_);
            ++variant_;
            scrollX_ = scrollY_ = 0;
            if (v == -1) {
                gfx.remove(obj + 0x13);
                viewCounter_ = 0;
                state_ = 4;
                break;
            }
            buffer_ = buffer_ == 0 ? 1 : 0;
            view = obj + 0x46 + buffer_;
            int n = kCgTable[selected_][v];
            char name[32];
            std::snprintf(name, sizeof name, "v%05d%d.tga", n / 10, n % 10);
            if (!gfx.load(slot + 8 + buffer_, name, 2, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
            gfx.makeImage(view, slot + 8 + buffer_, prio + 6, true);
            gfx.setSource(view, 0, 0, 1280, 720);
            game_.runEx = 1;
        }
        ++viewCounter_;
        float d = 16.0f - static_cast<float>(viewCounter_);
        float q = d * d / 256.0f;
        gfx.setAlpha(view, static_cast<int>(255.0 - static_cast<double>(q) * 255.0));
        int item = selected_ % 12;
        int h = static_cast<int>((1.0 - static_cast<double>(q)) * 720.0 + static_cast<double>(q) * 144.0);
        int w = static_cast<int>(static_cast<double>(q) * 256.0 + (1.0 - static_cast<double>(q)) * 1280.0);
        int y = static_cast<int>(static_cast<double>(thumbY(item) + 0x74) * static_cast<double>(q));
        int x = static_cast<int>(static_cast<double>(q) * static_cast<double>(thumbX(item) + 0x68));
        gfx.setDest(view, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        if (viewCounter_ > 0xf) {
            scrollX_ = scrollY_ = 0;
            gfx.setAlpha(view, 0xff);
            gfx.setRect(view, 0, 0, 0, 0, 1280, 720);
            state_ = 8;
        }
        break;
    }
    case 8: {
        int view = obj + 0x46 + buffer_;
        auto leave = [&]() {
            gfx.remove(obj + 0x46);
            gfx.remove(obj + 0x47);
            gfx.release(slot + 8);
            gfx.release(slot + 9);
            viewCounter_ = 0;
            state_ = 4;
        };
        if (viewCounter_ == 0) {
            // A click: the next seen variant crossfades in, or back to the page after the last.
            int v = seenVariant(selected_, variant_);
            ++variant_;
            scrollX_ = scrollY_ = 0;
            if (v == -1) {
                leave();
                break;
            }
            int n = kCgTable[selected_][v];
            sound.systemSe(Sound::Decide);
            gfx.setPriority(view, prio + 5);
            buffer_ = buffer_ == 0 ? 1 : 0;
            view = obj + 0x46 + buffer_;
            char name[32];
            std::snprintf(name, sizeof name, "v%05d%d.tga", n / 10, n % 10);
            if (!gfx.load(slot + 8 + buffer_, name, 2, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
            gfx.makeImage(view, slot + 8 + buffer_, prio + 6, false);
            gfx.setSource(view, 0, 0, 1280, 720);
            viewCounter_ = 0;
            state_ = 9;
            game_.runEx = 1;
        }
        // Pictures larger than the screen scroll when the pointer nears an edge (4 px a frame, twice as
        // fast nearer).
        int w = 0, h = 0;
        gfx.pictureSize(slot + 8 + buffer_, w, h);
        int px = in.pointerX(), py = in.pointerY();
        if (px < 100) scrollX_ = std::max(scrollX_ - 4, 0);
        if (px > 0x49c) scrollX_ = std::min(scrollX_ + 4, w - 0x500);
        if (px < 200) scrollX_ = std::max(scrollX_ - 4, 0);
        if (px > 0x438) scrollX_ = std::min(scrollX_ + 4, w - 0x500);
        if (py < 100) scrollY_ = std::max(scrollY_ - 4, 0);
        if (py > 0x26c) scrollY_ = std::min(scrollY_ + 4, h - 0x2d0);
        if (py < 200) scrollY_ = std::max(scrollY_ - 4, 0);
        if (py > 0x208) scrollY_ = std::min(scrollY_ + 4, h - 0x2d0);
        gfx.setSource(view, static_cast<float>(scrollX_), static_cast<float>(scrollY_), 1280, 720);
        gfx.setDest(view, 0, 0, 1280, 720);
        ++viewCounter_;
        if (click) viewCounter_ = 0;
        if (game_.keys.cancel) {
            sound.systemSe(Sound::Cancel);
            leave();
        }
        break;
    }
    case 9: {
        int view = obj + 0x46 + buffer_;
        ++viewCounter_;
        gfx.setVisible(view, true);
        gfx.setAlpha(view, viewCounter_ << 4);
        if (viewCounter_ > 0xf) {
            scrollX_ = scrollY_ = 0;
            gfx.setVisible(obj + 0x46 + (buffer_ == 0 ? 1 : 0), false);
            gfx.setAlpha(view, 0xff);
            state_ = 8;
        }
        break;
    }
    default:
        break;
    }
    lastHover_ = hover;
    return result;
}

// ---- Scene Replay (0x42d910) -----------------------------------------------------------------------

namespace {

// The scenes (0x4c1bf4, 20 bytes each): the game flag that unlocks it, its CG and its part (0 the
// introductory chapter, 1 closing chapter, 2 coda); two string pointers follow, unused here.
struct ReplayScene {
    int flag, cg, part;
};
constexpr ReplayScene kReplayScenes[24] = {
    {80, 101700, 0},  {300, 200800, 1}, {301, 201200, 1}, {302, 201700, 1}, {303, 202200, 1}, {304, 202400, 1},
    {305, 202700, 1}, {306, 203400, 1}, {307, 203500, 1}, {308, 203800, 1}, {309, 204900, 1}, {310, 205100, 1},
    {311, 205300, 1}, {312, 205500, 1}, {313, 205900, 2}, {314, 206500, 2}, {315, 208400, 2}, {316, 207003, 2},
    {317, 209800, 2}, {318, 209900, 2}, {319, 210200, 2}, {320, 210500, 2}, {321, 210600, 2}, {322, 210800, 2},
};

}  // namespace

SceneReplay::SceneReplay(Game& game) : game_(game) {}

bool SceneReplay::unlocked(int entry) const {
    // 0xa730ac (the introductory chapter installed on its own) is never set on Android.
    const ReplayScene& r = kReplayScenes[entry];
    if (game_.system.gameFlag(r.flag) == 0) return false;
    return r.part != 0 || game_.assets().introductoryChapterBundled();
}

void SceneReplay::items(int obj, int slot, int prio, int group, bool visible) {
    // The page's twelve: the CG's thumbnail in a frame for an unlocked scene, a placeholder by part
    // (coda's the darker 05903 until flag 5) for the others, which take no clicks.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    for (int i = 0; i < 12; ++i) {
        const ReplayScene& r = kReplayScenes[i + page_ * 12];
        int x = thumbX(i), y = thumbY(i);
        in.setArea(group, i, x + 0x68, y + 0x74, 0x100, 0x90, true);
        if (r.cg == 0) {
            gfx.remove(obj + 0x14 + i);
            gfx.remove(obj + 0x28 + i);
            in.enableArea(group, i, false);
            continue;
        }
        gfx.makeImage(obj + 0x28 + i, slot + 6, prio + 3, visible);
        gfx.setRect(obj + 0x28 + i, static_cast<float>(x + 0x60), static_cast<float>(y + 0x70), 0, 0, 272, 160);
        char name[32];
        bool thumb = unlocked(i + page_ * 12);
        if (thumb) {
            std::snprintf(name, sizeof name, "tv%06d.tga", r.cg);
        } else {
            const char* placeholder = r.part == 0 ? "sys_05900.tga"
                                      : r.part == 1 ? "sys_05901.tga"
                                      : game_.system.gameFlag(5) != 0 ? "sys_05902.tga" : "sys_05903.tga";
            std::snprintf(name, sizeof name, "%s", placeholder);
            in.enableArea(group, i, false);
            gfx.remove(obj + 0x28 + i);
        }
        if (!gfx.load(slot + 10 + i, name, 0x802, false, "", "", 0x100, "")) game_.noteMissing(std::string("picture ") + name);
        gfx.makeImage(obj + 0x14 + i, slot + 10 + i, prio + 2, visible);
        if (thumb) gfx.setRect(obj + 0x14 + i, static_cast<float>(x + 0x68), static_cast<float>(y + 0x74), 0, 0, 256, 144);
        else gfx.setRect(obj + 0x14 + i, static_cast<float>(x + 0x60), static_cast<float>(y + 0x70), 0, 0, 272, 160);
    }
}

int SceneReplay::step(int obj, int slot, int prio, int group) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    int result = 0;
    auto chrome = [&](float t, bool opening) {
        float k = t * 32.0f;
        gfx.moveTo(obj, 0, 0.0f - k);
        gfx.moveTo(obj + 1, 0, k + 664.0f);
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int o = obj; o < obj + 4; ++o) {
            gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
        }
        if (opening) gfx.setVisible(obj + 4, true);
        gfx.setColor(obj + 4, static_cast<int>((1.0 - static_cast<double>(t)) * 128.0), 0, 0, 0);
        if (opening) gfx.setVisible(obj + 9, true);
        gfx.moveTo(obj + 9, 1164, k + 688.0f);
        gfx.setAlpha(obj + 9, a);
        float zoom = t * 0.1f + 1.0f;
        auto zoomFade = [&](int o, bool show) {
            if (show) gfx.setVisible(o, true);
            gfx.setAlpha(o, a);
            gfx.setScale(o, zoom);
            gfx.setPivot(o, 640, 360);
        };
        // The heading (and, closing, the hovered scene's title) zoom with the page.
        zoomFade(obj + 0x10, opening);
        if (!opening) zoomFade(obj + 0x11, false);
        for (int o = obj + 5; o < obj + 10; ++o) zoomFade(o, true);
        for (int o = obj + 10; o < obj + 13; ++o) zoomFade(o, true);
        for (int i = 0; i < 12; ++i) {
            zoomFade(obj + 0x14 + i, true);
            zoomFade(obj + 0x28 + i, true);
        }
    };
    switch (state_) {
    case 0: {
        sound.playBgm(0xf, 0, 1, 0xff, 0, 0);
        game_.modes.replay = 0;
        in.setGroup(group);
        auto load = [&](int s, const char* name) {
            if (!gfx.load(s, name, 2, false, "", "", 0x100, "grp")) game_.noteMissing(std::string("picture ") + name);
        };
        load(slot, "sys_06000.tga");
        load(slot + 1, "sys_09000.tga");
        load(slot + 4, "sys_05100.tga");
        load(slot + 6, "sys_05110.tga");
        load(slot + 0x1e, "sys_09100.tga");
        load(slot + 0x1f, "sys_05020.tga");
        load(slot + 0x20, "sys_06021.tga");
        load(slot + 0x21, "sys_05010.tga");
        load(slot + 0x22, "sys_04010.tga");
        load(slot + 0x23, "sys_05012.tga");
        load(slot + 0x24, "sys_05200.tga");
        load(slot + 0x25, "sys_05201.tga");
        load(slot + 0x26, "sys_05300.tga");
        load(slot + 0x27, "sys_05301.tga");
        load(slot + 0x28, "sys_00301.tga");
        load(slot + 0x29, "sys_00300.tga");
        if (sound.bgmTrack() != -1) {
            // Now Playing: the bar (0x406dc0's play counters size it; not ported, it keeps 4 px) and
            // the track's name.
            gfx.makeImage(obj + 0x12, slot + 0x28, prio + 2, false);
            gfx.setRect(obj + 0x12, 24, 688, 0, 0, 4, 8);
            gfx.makeImage(obj + 0x13, slot + 0x29, prio + 2, false);
            gfx.setRect(obj + 0x13, 24, 696, 0, static_cast<float>(Game::musicIndex(sound.bgmTrack()) << 4), 264, 16);
        }
        gfx.makeImage(obj, slot, prio + 1, false);
        gfx.makeImage(obj + 1, slot + 1, prio + 1, false);
        gfx.setRect(obj, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(obj + 1, 0, 664, 0, 0, 1280, 56);
        gfx.makePolygon(obj + 4, 4, prio, false);
        gfx.setDest(obj + 4, 0, 0, 1280, 720);
        gfx.setColor(obj + 4, 0x80, 0, 0, 0);
        gfx.makeImage(obj + 5, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 5, 440, 600, 0, 0, 48, 64);
        gfx.makeImage(obj + 6, slot + 0x1f, prio + 3, false);
        gfx.setRect(obj + 6, 792, 600, 0, 64, 48, 64);
        gfx.makeImage(obj + 7, slot + 0x20, prio + 3, false);
        gfx.setRect(obj + 7, 496, 608, 0, 0, 288, 40);
        gfx.makeImage(obj + 8, slot + 0x22, prio + 1, false);
        gfx.setRect(obj + 8, 40, 0, 0, 0, 304, 168);
        gfx.makeImage(obj + 9, slot + 0x1e, prio + 3, false);
        gfx.setRect(obj + 9, 1164, 688, 0, 0, 56, 24);
        gfx.makeImage(obj + 0x10, slot + 0x26, prio + 3, false);
        gfx.setRect(obj + 0x10, 104, 624, 0, 0, 288, 24);
        gfx.makeImage(obj + 0x11, slot + 0x27, prio + 3, false);
        gfx.setRect(obj + 0x11, 104, 608, 0, 0, 288, 16);
        gfx.makeImage(obj + 0xc, slot + 0x24, prio + 3, false);
        gfx.setRect(obj + 0xc, 280, 86, 0, 0, 1000, 24);
        for (int t = 0; t < 2; ++t) {
            int o = obj + 10 + t, x = 0x47c + t * 0x3c;
            gfx.makeImage(o, slot + 0x25, prio + 3, false);
            gfx.setPriority(o, page_ == t ? prio + 4 : prio + 2);
            gfx.setRect(o, static_cast<float>(x), 72, static_cast<float>(t * 0x3c), page_ == t ? 0.0f : 64.0f, 60, 32);
            in.setArea(group, 20 + t, x, 0x48, 0x3c, 0x20, true);
        }
        items(obj, slot, prio, group, false);
        in.setArea(group, 0x1e, 0x48c, 0x2b0, 0x38, 0x18, true);
        in.setArea(group, 0x1f, 0x1b8, 600, 0x30, 0x40, true);
        in.setArea(group, 0x20, 0x318, 600, 0x30, 0x40, true);
        game_.runEx = 1;
        closed_ = 0;
        lastHover_ = -1;
        counter_ = 0;
        state_ = 2;
        break;
    }
    case 1:
        for (int o = obj; o < obj + 200; ++o) gfx.remove(o);
        for (int s = slot; s < slot + 0x32; ++s) gfx.release(s);
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
        gfx.setVisible(obj + 0x12, false);
        gfx.setVisible(obj + 0x13, false);
        break;
    }
    case 4: {
        gfx.setVisible(obj + 0x12, true);
        gfx.setVisible(obj + 0x13, true);
        // The arrows and the wheel swap the two pages.
        if (game_.keys.scrollUp || (hover == 0x1f && click)) {
            slide_ = 1;
            state_ = 5;
            page_ ^= 1;
            counter_ = 0;
            sound.systemSe(Sound::Page);
        }
        if (game_.keys.scrollDown || (hover == 0x20 && click)) {
            slide_ = -1;
            state_ = 5;
            page_ ^= 1;
            counter_ = 0;
            sound.systemSe(Sound::Page);
        }
        for (int t = 0; t < 2; ++t) {
            gfx.setPriority(obj + 10 + t, prio + 2);
            gfx.moveSource(obj + 10 + t, static_cast<float>(t * 0x3c), 64);
        }
        gfx.setPriority(obj + 10 + page_, prio + 4);
        gfx.moveSource(obj + 10 + page_, static_cast<float>(page_ * 0x3c), 0);
        for (int i = 0; i < 12; ++i) gfx.setSlot(obj + 0x28 + i, slot + 6);
        gfx.moveSource(obj + 5, 0, 0);
        gfx.moveSource(obj + 6, 0, 64);
        gfx.moveSource(obj + 9, 0, 0);
        gfx.setVisible(obj + 0x11, false);
        if (hover == -1 || hover > 11) {
            if (hover == 20 || hover == 21) {
                if (hover - 20 != page_) {
                    gfx.moveSource(obj + hover - 10, static_cast<float>((hover - 20) * 0x3c), 32);
                    if (click) {
                        sound.systemSe(Sound::Page);
                        slide_ = 0;
                        counter_ = 0;
                        state_ = 5;
                        page_ = hover - 20;
                    }
                }
            } else if (hover == 0x1f) {
                gfx.moveSource(obj + 5, click ? 96.0f : 48.0f, 0);
            } else if (hover == 0x20) {
                gfx.moveSource(obj + 6, click ? 96.0f : 48.0f, 64);
            } else if (hover == 0x1e) {
                gfx.moveSource(obj + 9, 0, click ? 48.0f : 24.0f);
            }
        } else {
            // The hovered scene's title under the arrows, its frame lit; a click starts it.
            gfx.setVisible(obj + 0x11, true);
            gfx.setRect(obj + 0x11, 104, 608, 0, static_cast<float>((hover + page_ * 12) * 16), 288, 16);
            gfx.setSlot(obj + 0x28 + hover, slot + 4);
            if (click) {
                gfx.setSlot(obj + 0x28 + hover, slot + 5);  // an empty slot: the frame goes
                sound.systemSe(Sound::Decide);
                sound.stopBgm(0x1e);
                game_.modes.replay = hover + 1 + page_ * 12;
                counter_ = 0;
                state_ = 1;
                closed_ = 5;
                // The exe also renames the window menu's Return to Title to "Return to Replays"
                // (ModifyMenuA); the platform's own menu keeps its label.
            }
        }
        if (game_.keys.cancel || (hover == 0x1e && click)) {
            game_.modes.replay = 0;
            counter_ = 0;
            state_ = 3;
            closed_ = 1;
            sound.systemSe(Sound::Cancel);
        }
        if (lastHover_ != hover && hover != -1) sound.systemSe(Sound::Hover);
        break;
    }
    case 5:
    case 6: {
        bool out = state_ == 5;
        float t;
        if (out) {
            for (int i = 0; i < 12; ++i) gfx.setSlot(obj + 0x28 + i, slot + 6);
            int next = counter_ + 1;
            if (next < slideDur) {
                t = std::min(static_cast<float>(counter_ + 2) / static_cast<float>(slideDur), 1.0f);
                counter_ = next;
            } else {
                t = 1.0f;
                state_ = 6;
                counter_ = 0;
                game_.runEx = 1;
                items(obj, slot, prio, group, true);
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
        for (int i = 0; i < 12; ++i) {
            auto x0 = static_cast<float>(thumbX(i));
            int y0 = thumbY(i);
            gfx.moveTo(obj + 0x14 + i, shift + 104.0f + x0, static_cast<float>(y0 + 0x74));
            gfx.moveTo(obj + 0x28 + i, shift + 104.0f + x0 - 8.0f, static_cast<float>(y0 + 0x70));
            int entry = i + page_ * 12;
            if (!out && kReplayScenes[entry].cg != 0 && !unlocked(entry)) {
                gfx.moveTo(obj + 0x14 + i, shift + 96.0f + x0, static_cast<float>(y0 + 0x70));
            }
            if (slide_ == 0) {
                float zoom = out ? t * 0.1f + 1.0f : 1.0f - t * 0.05f;
                gfx.setScale(obj + 0x14 + i, zoom);
                gfx.setScale(obj + 0x28 + i, zoom);
                gfx.setPivot(obj + 0x14 + i, 640, 360);
                gfx.setPivot(obj + 0x28 + i, 640, 360);
            }
            gfx.setAlpha(obj + 0x14 + i, a);
            gfx.setAlpha(obj + 0x28 + i, a);
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
