#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "nyan/common/bytes.hpp"
#include "nyan/wa2/image.hpp"
#include "nyan/wa2/serial.hpp"

namespace nyan::wa2 {

// What the save and load screens show of a save: the header the engine reads first (0x423330, the
// first 0x10058 bytes of save_NN.sav).
struct SaveInfo {
    // 0xa4e5bc: FLG_章管理サブ + (FLG_00 + (month * 100 + day) * 10) * 10 from the script globals and
    // the date card: the screen draws the chapter, the route and the date from it.
    int sortKey = 0;
    // 0xa4e5c0: the local time (SYSTEMTIME: year, month, weekday, day, hour, minute, second, ms).
    std::array<uint16_t, 8> time{};
    // 0xa4e5d0: the start of the current message (Shift-JIS, tags kept), as 0x423060 cuts it.
    std::string text;
    // 0xa4e610: the scene, 128 x 128 (squeezed from 16:9; drawn back at 128 x 72).
    RgbaImage thumbnail;
    uint32_t version = 0;  // Nyanroid's file version (2 adds SaveRecord::timer)
};

// The engine state a save holds besides the scripts (0x4234c0, restored by 0x423d10): what the engine
// needs to draw the scene again, not the state of every animation.
struct SaveRecord {
    // [0..5]: an endless shake only.
    struct Shake {
        bool on = false;
        int dir = 0, amp = 0, type = 0, speed = 0;
    } shake;
    // [6..0x27], 0x401e70, [0x440..0x442].
    struct Char {
        bool used = false;
        int id = 0, pic = 0;
        bool posFloat = false;
        int posIndex = 0;
        float pos = 0;
        int z = 0, r = 0, alpha = 0;  // r: the colour the restore gives all three channels
    };
    struct Scene {
        bool shown = false;
        int number = 0, kind = 0;
        int panX = 0, panY = 0;
        float zoomX = 1, zoomY = 1;
        int rotation = 0;  // truncated
        int timeMode = 0, bgLevel = 0x100, charLevel = 0x100;
        std::string bgAmp, bgEffect, charEffect;
        std::array<int, 3> tone{0x80, 0x80, 0x80};
        std::array<Char, 8> chars;
    } scene;
    // 0x4037c0.
    struct Message {
        bool window = false;
        int kind = 1, page = 0, reveal = 0, line = -1, novel = 0;
        bool nameShown = false;
        std::string name, text;
    } message;
    // 0x404dc0: flag, value and always are kept as signed bytes.
    struct Choice {
        std::string text;
        int flag = 0, value = 0, always = 0;
    };
    struct Choices {
        bool shown = false;
        int id = 0, script = 0;
        std::array<Choice, 8> options;
        int count = 0;
    } choices;
    // [0x3b5..0x3bb].
    struct Weather {
        bool on = false;
        int type = 0, wind = 0, fall = 0, count = 0, mode = 0;
    } weather;
    // [0x3bf..0x3ce]: channel 0 the current track, 1 none, 2 and 3 as they are.
    struct Bgm {
        int track = -1, state = 0, loop = 0, vol = 0;
    };
    std::array<Bgm, 4> bgm;
    // [0x3cf..0x40e].
    struct Sep {
        bool inUse = false;
        int num = 0, loop = 0, vol = 0;
    };
    std::array<Sep, 16> sep;
    // [0x41f..0x436]: looping voices only.
    struct Voice {
        bool on = false;
        int chara = 0, scene = 0, num = 0, vol = 0, base = 0;
    };
    std::array<Voice, 4> voices;
    bool eroMode = false;
    int viScene = -1, vaOffset = 0, voiceSpeaker = -1;
    // [0x447..]: the free pictures' records (0xa71b90, 0x98 bytes each).
    struct Picture {
        bool used = false, disp = false;
        int kind = 0;
        std::string file, amp, group;
        int colourMode = 0, level = 0x100;
        int blend = 1, mirror = 0, alpha = 0xff;
        int r = 0x80, g = 0x80, b = 0x80;
        int layer = 0, fixed = 0;
        int x = 0, y = 0, w = 0, h = 0, sx = 0, sy = 0, fullW = 0, fullH = 0;
        float zoom = 1;
        int cx = 0, cy = 0;
    };
    std::array<Picture, 32> pictures;
    // [0x909..0x90c]: the date card's date.
    int year = 0, month = 0, day = 0, weekday = 0;
    // Special Contents ([0x3bd], 0x422960 / 0x4231f0): the script timer's elapsed milliseconds
    // (GetTimer), started again from it on load. WA2.exe keeps no timer.
    int timer = 0;

    void write(Writer& w) const;
    bool read(Reader& r, uint32_t version);
};

// 0x423060: the save screen's text from the message buffer: a leading full-width space dropped, at
// most 24 bytes (a cut ends in "…"), at the first line break; tags left open are closed.
std::string saveExcerpt(const std::string& message);

}  // namespace nyan::wa2
