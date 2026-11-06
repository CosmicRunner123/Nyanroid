#include "nyan/wa2/options.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// The rows (0x4bf900, 0x34 bytes each, ten per page): the kind (0 two choices, 1 four, 2 two small,
// 3 three, 4 a slider, 5 a volume, 6 a volume with a test button, -1 none) and the cells' (x, y) in
// the page's art: choices from +4; a slider's -/+ at +4 / +0xc, its bar at +0x14, on / off at +0x1c /
// +0x24, the test button at +0x2c.
struct Row {
    int kind;
    int c[12];
};
constexpr Row kRows[30] = {
    {0, {456, 12, 712, 12}},
    {1, {456, 76, 584, 76, 712, 76, 840, 76}},
    {0, {456, 116, 712, 116}},
    {4, {480, 156, 888, 156, 544, 158}},
    {0, {456, 220, 712, 220}},
    {4, {480, 260, 888, 260, 544, 262}},
    {4, {480, 300, 888, 300, 544, 302}},
    {4, {480, 340, 888, 340, 544, 342}},
    {4, {480, 380, 888, 380, 544, 382}},
    {2, {384, 270, 384, 300}},
    {0, {456, 12, 712, 12}},
    {3, {456, 52, 628, 52, 800, 52}},
    {5, {480, 116, 888, 116, 544, 118, 390, 118, 390, 136}},
    {5, {480, 156, 888, 156, 544, 158, 390, 158, 390, 176}},
    {6, {480, 196, 888, 196, 544, 198, 390, 198, 390, 216, 944, 196}},
    {6, {480, 236, 888, 236, 544, 238, 390, 238, 390, 256, 944, 236}},
    {0, {456, 300, 712, 300}},
    {0, {456, 340, 712, 340}},
    {-1, {}},
    {-1, {}},
    {0, {456, 12, 712, 12}},
    {0, {456, 52, 712, 52}},
    {0, {456, 92, 712, 92}},
    {0, {456, 132, 712, 132}},
    {0, {456, 172, 712, 172}},
    {0, {456, 212, 712, 212}},
    {0, {456, 252, 712, 252}},
    {0, {456, 292, 712, 292}},
    {0, {456, 332, 712, 332}},
    {0, {456, 372, 712, 372}},
};

// Special Contents' table (0x4bc898) is the same but for four rows it has not: the 18+ scene voices
// (17) and the last three characters' voices (27..29).
const Row* rowTable(Game& game) {
    static const std::array<Row, 30> special = [] {
        std::array<Row, 30> t{};
        for (int i = 0; i < 30; ++i) t[static_cast<size_t>(i)] = kRows[i];
        for (int i : {17, 27, 28, 29}) t[static_cast<size_t>(i)].kind = -1;
        return t;
    }();
    return game.assets().build() == Build::Special ? special.data() : kRows;
}

constexpr int kTop = 0x4a4, kBottom = 0x4a5, kBackdrop = 0x4a6, kArt = 0x4a7, kShade = 0x4a8, kPrev = 0x4a9,
              kNext = 0x4aa, kHeader = 0x4ab, kHelp = 0x4ac, kBack = 0x4ad, kTabBar = 0x4ae, kTabBar2 = 0x4af,
              kTab = 0x4b0, kDefaults = 0x4b4, kPage = 0x4b5, kHighlight = 0x4b6, kCell = 0x4b8, kSlider = 0x4cc;
constexpr int kPrompt = 0x530, kPromptBox = 0x533, kYes = 0x534, kNo = 0x535, kPromptShade = 0x537;
constexpr int kMeter = 0x43f, kMeterTitle = 0x440;
constexpr int kPromptRecord = 0x2e;

// A slider's value from a bar position (21 steps): 0..256, or for the auto speed 60..600 frames.
int barValue(int v, bool autoSpeed) {
    if (autoSpeed) {
        if (v < 8) return 60;
        int u = (((v - 2) / 16) + 1) * 0x21c / 0x15 + 0x3c;
        return std::clamp(u, 60, 600);
    }
    if (v < 8) return 0;
    return std::clamp((((v - 2) / 16) + 1) * 0x100 / 0x15, 0, 0x100);
}

float barWidth(int value, bool autoSpeed) {
    if (autoSpeed) return static_cast<float>(((value - 0x3c) * 0x150) / 0x21c);
    return static_cast<float>(value) * 336.0f * 0.00390625f;
}

}  // namespace

Options::Options(Game& game) : game_(game) {}

void Options::open(int fromTitle) {
    // 0x419140
    fromTitle_ = fromTitle;
    game_.runEx = 1;
    if (fromTitle == 0) game_.setStoryPhase(5);
}

void Options::applyWindow() {
    // The window's opacities, as the slider handlers set them (0x40d, 0x41d, the bar's 10 objects).
    Gfx& gfx = game_.gfx();
    const auto& cfg = game_.message().config;
    gfx.setAlpha(0x40d, game_.scene().lastKind() == 0 ? cfg.winAlpha : cfg.winAlphaVis);
    gfx.setColor(0x41d, cfg.winAlphaNovel, 0, 0, 0);
    for (int i = 0; i < 10; ++i) gfx.setAlpha(0x40f + i, cfg.winAlphaCommand);
}

bool Options::hoverChoice(int row, int col, int w, int h, bool current) {
    // A choice cell under the pointer that is not the current one: the highlight on it.
    if (current) return false;
    const Row& r = rowTable(game_)[page_ * 10 + row];
    int x = r.c[col * 2], y = r.c[col * 2 + 1];
    Gfx& gfx = game_.gfx();
    gfx.setVisible(kHighlight, true);
    gfx.setRect(kHighlight, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70), static_cast<float>(x),
                static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
    return true;
}

void Options::build() {
    // 0x419170: every cell made again (hidden: their art shows the choice), the marks of the current
    // values shown, the hit areas set.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    auto& s = game_.settings;
    auto& cfg = game_.message().config;
    for (int obj = kTop + 0x11; obj < kTop + 0x8c; ++obj) gfx.remove(obj);
    for (int i = 0; i < 100; ++i) in.enableArea(1, i + 10, false);
    in.clearAreas(2);
    gfx.makeImage(kPage, page_ + 0x15b, 0x1062, true);
    gfx.setRect(kPage, 128, 112, 0, 0, 1024, 464);
    auto cells = [&](int row, int obj0, int slot, int n, int w, int h, int areaW, int areaH, int prio) {
        const Row& r = rowTable(game_)[page_ * 10 + row];
        for (int k = 0; k < n; ++k) {
            int x = r.c[k * 2], y = r.c[k * 2 + 1];
            gfx.makeImage(obj0 + 10 * k, slot, prio, false);
            gfx.setRect(obj0 + 10 * k, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70), static_cast<float>(x),
                        static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
            in.setArea(1, 10 + row * 10 + k, x + 0x80, y + 0x70, areaW, areaH, true);
        }
    };
    if (page_ == 0) {
        gfx.makeImage(kHighlight, 0x15e, 0x1063, false);
        for (int row = 0; row < 10; ++row) {
            const Row& r = rowTable(game_)[row];
            switch (r.kind) {
            case 0: cells(row, kCell + row, 0x15f, 2, 256, 40, 0x100, 0x28, 0x1063); break;
            case 1: cells(row, kCell + row, 0x15f, 4, 128, 40, 0x80, 0x28, 0x1063); break;
            case 2: cells(row, kCell + row, 0x15f, 2, 88, 30, 0x58, 0x1e, 0x1063); break;
            case 4: {
                int value = 0;
                switch (row) {
                case 3: value = cfg.autoMax; break;
                case 5: value = cfg.winAlpha; break;
                case 6: value = cfg.winAlphaVis; break;
                case 7: value = cfg.winAlphaNovel; break;
                case 8: value = cfg.winAlphaCommand; break;
                default: break;
                }
                int sx = r.c[4], sy = r.c[5];
                gfx.makeImage(kSlider + row, 0x15f, 0x1065, true);
                gfx.setRect(kSlider + row, static_cast<float>(sx + 0x80), static_cast<float>(sy + 0x70), static_cast<float>(sx),
                            static_cast<float>(sy), barWidth(value, row == 3), 24);
                in.setArea(1, row * 10 + 0xc, sx + 0x80, sy + 0x70, 0x150, 0x18, true);
                cells(row, kCell + row, 0x15f, 2, 56, 40, 0x38, 0x28, 0x1064);
                break;
            }
            default:
                break;
            }
        }
        gfx.setVisible(0x4b8, s.windowMode != 0);
        gfx.setVisible(0x4c2, s.windowMode == 0);
        gfx.setVisible(0x4b9, cfg.speed == 0.0f);
        gfx.setVisible(0x4c3, cfg.speed == 0.5f);
        gfx.setVisible(0x4cd, cfg.speed == 1.0f);
        gfx.setVisible(0x4d7, cfg.speed == 2.0f);
        gfx.setVisible(0x4ba, game_.modes.skipUnread);
        gfx.setVisible(0x4c4, !game_.modes.skipUnread);
        gfx.setVisible(0x4bc, s.speed != 1.0f);
        gfx.setVisible(0x4c6, s.speed == 1.0f);
        gfx.setVisible(0x4c1, cfg.winDisp != 0);
        gfx.setVisible(0x4cb, cfg.winDisp == 0);
    } else if (page_ == 1) {
        gfx.makeImage(kHighlight, 0x160, 0x1063, false);
        for (int row = 0; row < 10; ++row) {
            const Row& r = rowTable(game_)[10 + row];
            switch (r.kind) {
            case 0: cells(row, kCell + row, 0x161, 2, 256, 40, 0x100, 0x28, 0x1063); break;
            case 3: cells(row, kCell + row, 0x161, 3, 168, 40, 0xa8, 0x28, 0x1063); break;
            case 5:
            case 6: {
                int value = row == 2 ? s.allVol : row == 3 ? s.bgmVol : row == 4 ? s.seVol : s.voiceVol;
                int sx = r.c[4], sy = r.c[5];
                gfx.makeImage(kSlider + row, 0x161, 0x1065, true);
                gfx.setRect(kSlider + row, static_cast<float>(sx + 0x80), static_cast<float>(sy + 0x70), static_cast<float>(sx),
                            static_cast<float>(sy), barWidth(value, false), 24);
                in.setArea(1, 10 + row * 10 + 2, sx + 0x80, sy + 0x70, 0x150, 0x18, true);
                cells(row, kCell + row, 0x161, 2, 56, 40, 0x38, 0x28, 0x1064);
                // On / off: the cells at +0x1c / +0x24 (0x419170: from 0x4bf91c).
                for (int k = 3; k < 5; ++k) {
                    int x = r.c[k * 2], y = r.c[k * 2 + 1];
                    int obj = kSlider + row + 10 * (k - 2);
                    gfx.makeImage(obj, 0x161, 0x1064, false);
                    gfx.setRect(obj, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70), static_cast<float>(x),
                                static_cast<float>(y), 80, 16);
                    in.setArea(1, 10 + row * 10 + k, x + 0x80, y + 0x70, 0x50, 0x10, true);
                }
                if (r.kind == 6) {
                    int x = r.c[10], y = r.c[11];
                    gfx.makeImage(kSlider + row + 0x1e, 0x161, 0x1064, false);
                    gfx.setRect(kSlider + row + 0x1e, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70),
                                static_cast<float>(x), static_cast<float>(y), 56, 16);
                    in.setArea(1, 10 + row * 10 + 5, x + 0x80, y + 0x70, 0x38, 0x28, true);
                }
                break;
            }
            default:
                break;
            }
        }
        gfx.setVisible(0x4b8, s.confirm != 0);
        gfx.setVisible(0x4c2, s.confirm == 0);
        gfx.setVisible(0x4b9, cfg.rightClick == 0);
        gfx.setVisible(0x4c3, cfg.rightClick == 1);
        gfx.setVisible(0x4cd, cfg.rightClick == 2);
        int flags[4] = {s.allSound, s.bgm, s.se, s.voice};
        for (int k = 0; k < 4; ++k) {
            gfx.setVisible(0x4d8 + k, flags[k] != 0);
            gfx.setVisible(0x4e2 + k, flags[k] == 0);
        }
        gfx.setVisible(0x4be, s.pageVoice != 0);
        gfx.setVisible(0x4c8, s.pageVoice == 0);
        gfx.setVisible(0x4bf, s.eroVoice == 0);
        gfx.setVisible(0x4c9, s.eroVoice != 0);
    } else {
        gfx.makeImage(kHighlight, 0x162, 0x1063, false);
        for (int row = 0; row < 10; ++row) {
            if (rowTable(game_)[20 + row].kind == 0) cells(row, kCell + row, 0x163, 2, 256, 40, 0x100, 0x28, 0x1063);
        }
        for (int k = 0; k < 10; ++k) {
            gfx.setVisible(0x4b8 + k, s.charVoice[static_cast<size_t>(k)] != 0);
            gfx.setVisible(0x4c2 + k, s.charVoice[static_cast<size_t>(k)] == 0);
        }
    }
}

int Options::step() {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    auto& s = game_.settings;
    auto& cfg = game_.message().config;
    int result = 0;
    int hover = in.hoveredArea();
    bool click = game_.keys.advance;
    int dur = game_.fr(-1);
    int slideDur = game_.fr(7);
    switch (state_) {
    case 1:
        result = 1;
        close();
        break;
    case 2:
    case 3: {
        ++counter_;
        float t;
        bool opening = state_ == 2;
        bool done = counter_ >= dur;
        if (opening) t = done ? 0.0f : static_cast<float>(dur - counter_) / static_cast<float>(dur);
        else t = done ? 1.0f : static_cast<float>(counter_) / static_cast<float>(dur);
        if (done) {
            state_ = opening ? 4 : 1;
            counter_ = 0;
        }
        if (!opening) {
            gfx.setVisible(kMeter, false);
            gfx.setVisible(kMeterTitle, false);
        }
        fade(t, opening);
        if (opening && done) {
            for (int obj = kPrev; obj < kPrev + 12; ++obj) {
                gfx.setScale(obj, 1);
                gfx.setAlpha(obj, 0x100);
            }
            for (int obj = kPage; obj < kPage + 0x85; ++obj) {
                gfx.setScale(obj, 1);
                gfx.setAlpha(obj, 0x100);
            }
        }
        break;
    }
    case 4: {
        gfx.setVisible(kMeter, true);
        gfx.setVisible(kMeterTitle, true);
        build();
        gfx.setVisible(kHighlight, false);
        int px = in.pointerX();
        if (hover >= 10) {
            int row = (hover - 10) / 10, col = (hover - 10) % 10;
            const Row& r = rowTable(game_)[page_ * 10 + row];
            auto slider = [&](int& value, bool autoSpeed, bool window) {
                // col 2: the bar; 0 / 1: - / +.
                int v = px - 0x80 - r.c[4];
                if (col == 2) {
                    int u = barValue(v, autoSpeed);
                    gfx.setVisible(kHighlight, true);
                    gfx.setRect(kHighlight, static_cast<float>(r.c[4] + 0x80), static_cast<float>(r.c[5] + 0x70),
                                static_cast<float>(r.c[4]), static_cast<float>(r.c[5]), barWidth(u, autoSpeed), 24);
                    if (click) {
                        sound.systemSe(Sound::Decide);
                        value = u;
                        if (window) applyWindow();
                    }
                } else if (col < 2) {
                    hoverChoice(row, col, 56, 40, false);
                    auto w = static_cast<int>(barWidth(value, autoSpeed));
                    int nw = col == 0 ? std::max(w - 0x10, 0) : std::min(w + 0x10, 0x150);
                    if (click) {
                        gfx.setVisible(kCell + row + col * 10, true);
                        game_.runEx = 1;
                        sound.systemSe(Sound::Decide);
                        value = barValue(nw, autoSpeed);
                        if (window) applyWindow();
                    }
                }
            };
            if (page_ == 0) {
                switch (row) {
                case 0:
                    // Window or full screen: the platform decides; only the highlight and the sound.
                    if (hoverChoice(row, col, 256, 40, s.windowMode == 0 ? col == 1 : col == 0) && click &&
                        ((col == 0 && s.windowMode == 0) || (col == 1 && s.windowMode != 0))) {
                        sound.systemSe(Sound::Decide);
                    }
                    break;
                case 1: {
                    static constexpr float kSpeeds[4] = {0.0f, 0.5f, 1.0f, 2.0f};
                    if (col < 4 && hoverChoice(row, col, 128, 40, cfg.speed == kSpeeds[col]) && click) {
                        cfg.speed = kSpeeds[col];
                        sound.systemSe(Sound::Decide);
                    }
                    break;
                }
                case 2:
                    if (hoverChoice(row, col, 256, 40, game_.modes.skipUnread ? col == 0 : col == 1) && click) {
                        sound.systemSe(Sound::Decide);
                        game_.modes.skipUnread = col == 0;
                    }
                    break;
                case 3: slider(cfg.autoMax, true, false); break;
                case 5: slider(cfg.winAlpha, false, true); break;
                case 6: slider(cfg.winAlphaVis, false, true); break;
                case 7: slider(cfg.winAlphaNovel, false, true); break;
                case 8: slider(cfg.winAlphaCommand, false, true); break;
                case 4:
                    if (hoverChoice(row, col, 256, 40, s.speed == 1.0f ? col == 1 : col == 0) && click) {
                        s.speed = col == 0 ? 0.0f : 1.0f;
                        sound.systemSe(Sound::Decide);
                    }
                    break;
                case 9:
                    if (hoverChoice(row, col, 88, 30, cfg.winDisp == 0 ? col == 1 : col == 0) && click) {
                        cfg.winDisp = col == 0 ? 1 : 0;
                        sound.systemSe(Sound::Decide);
                        if (fromTitle_ == 0) game_.message().applyWindowArt();
                    }
                    break;
                default:
                    break;
                }
            } else if (page_ == 1) {
                switch (row) {
                case 0:
                    if (hoverChoice(row, col, 256, 40, s.confirm == 0 ? col == 1 : col == 0) && click) {
                        sound.systemSe(Sound::Decide);
                        s.confirm = col == 0 ? 1 : 0;
                    }
                    break;
                case 1:
                    if (col < 3 && hoverChoice(row, col, 168, 40, cfg.rightClick == col) && click) {
                        sound.systemSe(Sound::Decide);
                        cfg.rightClick = col;
                    }
                    break;
                case 2:
                case 3:
                case 4:
                case 5: {
                    int& vol = row == 2 ? s.allVol : row == 3 ? s.bgmVol : row == 4 ? s.seVol : s.voiceVol;
                    if (col <= 2) {
                        slider(vol, false, false);
                    } else if (col == 3 || col == 4) {
                        int flag = row == 2 ? s.allSound : row == 3 ? s.bgm : row == 4 ? s.se : s.voice;
                        bool current = flag == 0 ? col == 4 : col == 3;
                        if (!current) {
                            int x = r.c[col * 2], y = r.c[col * 2 + 1];  // 0x41f800: 0x4bf904 + col * 8
                            gfx.setVisible(kHighlight, true);
                            gfx.setRect(kHighlight, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70),
                                        static_cast<float>(x), static_cast<float>(y), 80, 16);
                            if (click) {
                                sound.systemSe(Sound::Decide);
                                bool on = col == 3;
                                if (row == 2) {
                                    s.allSound = on ? 1 : 0;
                                    sound.switchBgm(on, true);
                                    sound.switchSe(on, true);
                                    sound.switchVoice(on, true);
                                } else if (row == 3) {
                                    sound.switchBgm(on, false);
                                } else if (row == 4) {
                                    sound.switchSe(on, false);
                                } else {
                                    sound.switchVoice(on, false);
                                }
                            }
                        }
                    } else if (col == 5) {
                        int x = r.c[10], y = r.c[11];
                        gfx.setVisible(kHighlight, true);
                        gfx.setRect(kHighlight, static_cast<float>(x + 0x80), static_cast<float>(y + 0x70), static_cast<float>(x),
                                    static_cast<float>(y), 56, 40);
                        if (click) {
                            gfx.setVisible(kSlider + row + 0x1e, true);
                            if (row == 4) {
                                sound.sep(0xf, 0x1fa5, 0, 0, 0xff, 0);
                            } else {
                                // One of five lines (character, scene, number), by the C runtime's
                                // rand(); Special Contents (0x41eb90) has its own five.
                                static constexpr int kTest[2][5][3] = {
                                    {{1, 0x9c7, 99}, {2, 0x7d2, 0xf2}, {3, 0x7eb, 0xdf}, {4, 0x7d1, 0x56}, {5, 0x7d1, 0xc6}},
                                    {{2, 4000, 0x53}, {1, 6003, 0x21}, {2, 5000, 0}, {1, 6001, 0x14}, {0, 6001, 0x26}},
                                };
                                const int* t = kTest[game_.assets().build() == Build::Special ? 1 : 0][game_.rand.next() % 5];
                                sound.playVoice(0, t[0], t[1], t[2], 0xff, 0, 1);
                            }
                        }
                    }
                    break;
                }
                case 6:
                    if (hoverChoice(row, col, 256, 40, s.pageVoice == 1 ? col == 0 : col == 1) && click) {
                        sound.systemSe(Sound::Decide);
                        s.pageVoice = col == 0 ? 1 : 0;
                    }
                    break;
                case 7:
                    if (hoverChoice(row, col, 256, 40, s.eroVoice == 0 ? col == 0 : col == 1) && click) {
                        sound.systemSe(Sound::Decide);
                        s.eroVoice = col != 0 ? 1 : 0;
                    }
                    break;
                default:
                    break;
                }
            } else if (r.kind == 0) {
                int& voice = s.charVoice[static_cast<size_t>(row)];
                if (hoverChoice(row, col, 256, 40, voice == 0 ? col == 1 : col == 0) && click) {
                    sound.systemSe(Sound::Decide);
                    voice = col == 0 ? 1 : 0;
                }
            }
        }
        // The wheel turns pages.
        if (game_.keys.scrollDown) hover = 7, click = true;
        if (game_.keys.scrollUp) hover = 8, click = true;
        for (int t = 0; t < 3; ++t) {
            gfx.setSource(kTab + t, static_cast<float>(0xc0 * t), 64, 192, 32);
            gfx.setPriority(kTab + t, 0x1064);
            gfx.setVisible(kTab + t, true);
        }
        gfx.setSource(kTab + page_, static_cast<float>(page_ * 0xc0), 0, 192, 32);
        gfx.setPriority(kTab + page_, 0x1066);
        gfx.setSource(kDefaults, 0, 0, 208, 24);
        gfx.setSource(kPrev, 0, 0, 48, 64);
        gfx.setSource(kNext, 0, 64, 48, 64);
        gfx.setVisible(kHelp, false);
        if (hover > 9) {
            gfx.setVisible(kHelp, true);
            gfx.setSlot(kHelp, page_ + 0x164);
            float v = 0;
            if (page_ == 0) v = static_cast<float>(((hover - 10) / 10) * 0x18);
            else if (page_ == 1) v = hover < 0x1e ? static_cast<float>(((hover - 10) / 10) * 0x18) : hover < 0x46 ? 48.0f : static_cast<float>(((hover - 0x28) / 10) * 0x18);
            gfx.moveSource(kHelp, 0, v);
        }
        gfx.setSource(kBack, 0, 0, 56, 24);
        if (hover != -1 && hover != page_) {
            switch (hover) {
            case 0:
            case 1:
            case 2: gfx.setSource(kTab + hover, static_cast<float>(hover * 0xc0), 32, 192, 32); break;
            case 3: gfx.setSource(kDefaults, 0, 24, 208, 24); break;
            case 7: gfx.setSource(kPrev, 48, 0, 48, 64); break;
            case 8: gfx.setSource(kNext, 48, 64, 48, 64); break;
            case 9: gfx.setSource(kBack, 0, 24, 56, 24); break;
            default: break;
            }
            if (click) {
                switch (hover) {
                case 0:
                case 1:
                case 2:
                    sound.systemSe(Sound::Page);
                    page_ = hover;
                    state_ = 5;
                    counter_ = 0;
                    break;
                case 3:
                    sound.systemSe(Sound::Decide);
                    gfx.setSource(kDefaults, 0, 48, 208, 24);
                    state_ = 8;
                    counter_ = 0;
                    in.setGroup(3);
                    break;
                case 7:
                case 8:
                    sound.systemSe(Sound::Page);
                    if (hover == 7) {
                        gfx.setSource(kPrev, 96, 0, 48, 64);
                        page_ = (page_ + 2) % 3;
                    } else {
                        gfx.setSource(kNext, 96, 64, 48, 64);
                        page_ = (page_ + 1) % 3;
                    }
                    state_ = 5;
                    counter_ = 0;
                    break;
                case 9:
                    gfx.setSource(kBack, 0, 48, 56, 24);
                    state_ = 3;
                    counter_ = 0;
                    sound.systemSe(Sound::Cancel);
                    break;
                default:
                    break;
                }
            }
        }
        if (game_.keys.cancel) {
            sound.systemSe(Sound::Cancel);
            state_ = 3;
            counter_ = 0;
            sound.systemSe(Sound::Cancel);
        }
        if (lastHover_ != hover && hover != -1) sound.systemSe(Sound::Hover);
        lastHover_ = hover;
        // The defaults prompt fades away.
        --counter_;
        float t;
        if (counter_ < 1) {
            t = 0;
            counter_ = 0;
            for (int k = 0; k < 8; ++k) gfx.setVisible(kPrompt + k, false);
        } else {
            t = static_cast<float>(dur - counter_) / static_cast<float>(dur);
        }
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int k = 0; k < 7; ++k) gfx.setAlpha(kPrompt + k, a);
        gfx.setAlpha(kPromptShade, static_cast<int>((1.0 - static_cast<double>(t)) * 76.0));
        return 0;
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
            build();
        }
        pageZoom(t, static_cast<float>(0.1 * static_cast<double>(t) + 1.0));
        break;
    }
    case 6: {
        ++counter_;
        float t;
        if (counter_ < slideDur) {
            t = static_cast<float>(slideDur - counter_) / static_cast<float>(slideDur);
        } else {
            counter_ = 0;
            t = 0;
            state_ = 4;
            for (int obj = kPage; obj < kPage + 0x85; ++obj) {
                gfx.setScale(obj, 1);
                gfx.setAlpha(obj, 0x100);
            }
        }
        pageZoom(t, static_cast<float>(1.0 - 0.05 * static_cast<double>(t)));
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
        int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
        for (int k = 0; k < 7; ++k) gfx.setAlpha(kPrompt + k, a);
        gfx.setAlpha(kPromptShade, static_cast<int>((1.0 - static_cast<double>(t)) * 76.0));
        for (int k = 0; k < 4; ++k) gfx.setVisible(kPrompt + k, true);
        gfx.setVisible(kPromptShade, true);
        gfx.setVisible(kYes, true);
        gfx.setVisible(kNo, true);
        gfx.moveSource(kYes, 0, 0);
        gfx.moveSource(kNo, 160, 0);
        int choice = hover;
        bool go = click;
        if (s.confirm == 0) {
            choice = 0;
            go = true;
        }
        if (choice == 0 || choice == 1) {
            gfx.moveSource(choice == 0 ? kYes : kNo, choice == 0 ? 0.0f : 160.0f, 40);
            if (go) {
                if (choice == 0) {
                    gfx.moveSource(kYes, 0, 80);
                    sound.se(0x22c9, 0xff);
                    state_ = 4;
                    in.setGroup(1);
                    setDefaults();
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
        return 0;
    }
    default: {
        // The set-up.
        in.clearAreas(2);
        in.clearAreas(1);
        auto load = [&](int slot, const char* name) {
            if (!gfx.load(slot, name, 2, false, "", "", 0x100, "grp")) game_.noteMissing(std::string("picture ") + name);
        };
        load(0x155, "sys_03000.tga");
        load(0x156, "sys_09000.tga");
        load(0x159, "sys_03010.tga");
        load(0x15a, "sys_03011.tga");
        load(0x15b, "sys_03100.tga");
        // Special Contents (0x41eb90) names the translators' pictures sys_e*.
        bool special = game_.assets().build() == Build::Special;
        load(0x15c, special ? "sys_e3101.tga" : "sys_03101.tga");
        load(0x15d, special ? "sys_e3102.tga" : "sys_03102.tga");
        load(0x15e, "sys_03200.tga");
        load(0x15f, "sys_03201.tga");
        load(0x160, "sys_03210.tga");
        load(0x161, "sys_03211.tga");
        load(0x162, "sys_03220.tga");
        load(0x163, "sys_03221.tga");
        load(0x164, "sys_03040.tga");
        load(0x165, "sys_03041.tga");
        load(0x166, "sys_03042.tga");
        load(0x167, "sys_03020.tga");
        load(0x168, "sys_03030.tga");
        char name[32];
        std::snprintf(name, sizeof name, "sys_%01d0100.tga", cfg.winColor);
        load(0x169, name);
        load(0x16a, "sys_00101.tga");
        load(0x174, "sys_09100.tga");
        int flg = game_.globalInt("FLG_00");
        bool chapterArt = fromTitle_ == 0 && static_cast<unsigned>(flg) <= 2;
        const char* logo = special ? "sys_e4010.tga" : "sys_04010.tga";
        if (fromTitle_ == 0) load(0x175, flg == 0 ? "sys_05010.tga" : flg == 1 ? "sys_05011.tga" : flg == 2 ? "sys_05012.tga" : logo);
        else load(0x175, logo);
        gfx.makeImage(kArt, 0x175, 0x1062, true);
        gfx.setRect(kArt, 40, special && fromTitle_ != 0 ? -16.0f : 0.0f, 0, chapterArt ? 16.0f : 0.0f, 304, 168);
        gfx.makeImage(kTop, 0x155, 0x1062, true);
        gfx.makeImage(kBottom, 0x156, 0x1062, true);
        gfx.setRect(kTop, 0, 0, 0, 0, 1280, 40);
        gfx.setRect(kBottom, 0, 664, 0, 0, 1280, 56);
        gfx.makePolygon(kShade, 4, 0x1061, true);
        gfx.setDest(kShade, 0, 0, 1280, 720);
        gfx.setColor(kShade, 0x80, 0, 0, 0);
        gfx.makeImage(kPrev, 0x167, 0x1064, true);
        gfx.setRect(kPrev, 440, 600, 0, 0, 48, 64);
        gfx.makeImage(kNext, 0x167, 0x1064, true);
        gfx.setRect(kNext, 792, 600, 0, 64, 48, 64);
        gfx.makeImage(kHeader, 0x167, 0x1064, true);
        gfx.setRect(kHeader, 496, 608, 144, 0, 288, 40);
        gfx.makeImage(kHelp, 0x164, 0x1064, false);
        gfx.setRect(kHelp, 168, 552, 0, 0, 994, 24);
        gfx.makeImage(kBack, 0x174, 0x1064, true);
        gfx.setRect(kBack, 1164, 688, 0, 0, 56, 24);
        in.setArea(1, 7, 0x1b8, 600, 0x30, 0x40, true);
        in.setArea(1, 8, 0x318, 600, 0x30, 0x40, true);
        in.setArea(1, 9, 0x48c, 0x2b0, 0x38, 0x18, true);
        gfx.makeImage(kTabBar, 0x159, 0x1063, true);
        gfx.makeImage(kTabBar2, 0x159, 0x1065, true);
        for (int t = 0; t < 3; ++t) gfx.makeImage(kTab + t, 0x15a, 0x1064, true);
        gfx.setRect(kTabBar, 524, 64, 0, 0, 756, 48);
        gfx.setRect(kTabBar2, 524, 64, 0, 48, 756, 48);
        for (int t = 0; t < 3; ++t) gfx.setRect(kTab + t, static_cast<float>(688 + 192 * t), 72, static_cast<float>(192 * t), 64, 192, 32);
        gfx.setRect(kTab + page_, static_cast<float>(page_ * 0xc0 + 0x2b0), 72, static_cast<float>(page_ * 0xc0), 0, 192, 32);
        gfx.setPriority(kTab + page_, 0x1066);
        gfx.makeImage(kDefaults, 0x168, 0x1063, true);
        gfx.setRect(kDefaults, 880, 688, 0, 0, 208, 24);
        for (int t = 0; t < 3; ++t) in.setArea(1, t, 0x2b0 + 0xc0 * t, 0x48, 0xc0, 0x20, true);
        in.setArea(1, 3, 0x370, 0x2b0, 0xd0, 0x18, true);
        build();
        gfx.makeTextSlot(kPromptRecord, 0x1c4, game_.systemMessage("Are you sure you want\\nto restore defaults?"), 0x1c, 0xf, 2, 0, 0, 0x22,
                         0x28, false, 0xff, 0xff, 0xff, 0, 0, 0, true);
        gfx.makeText(kPrompt, kPromptRecord, 0x1067, false);
        gfx.moveTo(kPrompt, 516, 304);
        gfx.makeImage(kPromptBox, 0x169, 0x1066, false);
        gfx.setRect(kPromptBox, 368, 256, 0, 0, 656, 264);
        gfx.makeImage(kYes, 0x16a, 0x1067, false);
        gfx.setRect(kYes, 472, 376, 0, 0, 160, 40);
        gfx.makeImage(kNo, 0x16a, 0x1067, false);
        gfx.setRect(kNo, 648, 376, 160, 0, 160, 40);
        gfx.makePolygon(kPromptShade, 4, 0x1066, false);
        gfx.setDest(kPromptShade, 0, 0, 1280, 720);
        gfx.setColor(kPromptShade, 0x4c, 0, 0, 0);
        in.setArea(3, 0, 0x1d8, 0x178, 0xa0, 0x28, true);
        in.setArea(3, 1, 0x288, 0x178, 0xa0, 0x28, true);
        gfx.makeImage(kMeter, 0x116, 0x1065, false);
        gfx.setRect(kMeter, 24, 688, 0, 0, 0, 8);
        if (sound.bgmTrack() != -1) {
            gfx.makeImage(kMeterTitle, 0x117, 0x1065, false);
            gfx.setRect(kMeterTitle, 24, 696, 0, static_cast<float>(Game::musicIndex(sound.bgmTrack()) << 4), 264, 16);
        }
        in.setGroup(1);
        state_ = 2;
        counter_ = 0;
        // The opening's first frame.
        ++counter_;
        bool done = counter_ >= dur;
        float t = done ? 0.0f : static_cast<float>(dur - counter_) / static_cast<float>(dur);
        if (done) {
            state_ = 4;
            counter_ = 0;
        }
        fade(t, true);
        break;
    }
    }
    lastHover_ = hover;
    return result;
}

void Options::fade(float t, bool opening) {
    // States 2 / 3: the page and the cells zoom by a tenth and fade, the bars slide 32 px.
    (void)opening;
    Gfx& gfx = game_.gfx();
    auto zoom = static_cast<float>(0.1 * static_cast<double>(t) + 1.0);
    int a = static_cast<int>((1.0 - static_cast<double>(t)) * 256.0);
    for (int obj = kPage; obj < kPage + 0x85; ++obj) {
        gfx.setScale(obj, zoom);
        gfx.setPivot(obj, 640, 360);
        gfx.setAlpha(obj, a);
    }
    double k = static_cast<double>(t) * 32.0;
    gfx.moveTo(kTop, 0, static_cast<float>(0.0 - k));
    gfx.moveTo(kBottom, 0, static_cast<float>(k + 664.0));
    gfx.setScale(kArt, zoom);
    gfx.setPivot(kArt, 640, 360);
    for (int obj = kTop; obj < kTop + 4; ++obj) gfx.setAlpha(obj, a);
    gfx.setColor(kShade, static_cast<int>((1.0 - static_cast<double>(t)) * 128.0), 0, 0, 0);
    for (int obj = kPrev; obj < kPrev + 12; ++obj) {
        gfx.setScale(obj, zoom);
        gfx.setPivot(obj, 640, 360);
        gfx.setAlpha(obj, a);
    }
}

void Options::pageZoom(float t, float zoom) {
    Gfx& gfx = game_.gfx();
    int a = static_cast<int>(256.0 - static_cast<double>(t) * 256.0);
    for (int obj = kPage; obj < kPage + 0x85; ++obj) {
        gfx.setScale(obj, zoom);
        gfx.setPivot(obj, 640, 360);
        gfx.setAlpha(obj, a);
    }
}

void Options::close() {
    // State 1: from the story, back to it (state 4); every object and picture released.
    // From the title, as the engine does, only 0x4a4..0x4e9 and slots 0x155..0x161 (the later cells
    // stay).
    Gfx& gfx = game_.gfx();
    if (fromTitle_ == 0) game_.setStoryPhase(4);
    int objects = fromTitle_ == 0 ? 0x96 : 0x46, slots = fromTitle_ == 0 ? 0x10 : 0xd;
    for (int obj = kTop; obj < kTop + objects; ++obj) gfx.remove(obj);
    for (int slot = 0x155; slot < 0x155 + slots; ++slot) gfx.release(slot);
    (void)kBackdrop;
    gfx.remove(kMeter);
    gfx.remove(kMeterTitle);
    game_.input.clearAreas(3);
    game_.input.clearAreas(2);
    game_.input.clearAreas(1);
    game_.input.setGroup(0);
    state_ = 0;
    game_.saveSettings();
}

void Options::setDefaults() {
    // 0x41a160
    auto& s = game_.settings;
    auto& cfg = game_.message().config;
    s.speed = 1.0f;
    cfg.winColor = 0;
    cfg.speed = 1.0f;
    game_.modes.skipUnread = false;
    cfg.winAlpha = 0xcf;
    cfg.winAlphaVis = 0x86;
    cfg.winAlphaNovel = 0x86;
    cfg.winAlphaCommand = 0x86;
    cfg.winDisp = 1;
    if (fromTitle_ == 0) {
        applyWindow();
        game_.message().applyWindowArt();
    }
    s.confirm = 1;
    cfg.rightClick = 0;
    s.pageVoice = 0;
    s.eroVoice = 0;
    cfg.autoMax = 0x89;
    s.allSound = 1;
    game_.sound().switchBgm(true, false);
    game_.sound().switchSe(true, false);
    game_.sound().switchVoice(true, false);
    s.charVoice.fill(1);
    s.bgmVol = 0x86;
    s.allVol = 0xc3;
    s.seVol = 0xc3;
    s.voiceVol = 0x100;
}

}  // namespace nyan::wa2
