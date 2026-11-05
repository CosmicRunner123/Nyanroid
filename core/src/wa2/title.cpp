#include "nyan/wa2/title.hpp"

#include <algorithm>
#include <cmath>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// The rows each menu shows, as T0100 / T0200 row numbers (-1: no row): New Game, Continue, Options,
// Specials, Exit (0x4c0088); the chapters and Back (0x4c00a0); Specials (0x4c00b8); the Digital
// Novels (0x4c00d0).
constexpr int kMainRows[6] = {0, 1, 2, 3, -1, 4};
constexpr int kChapterRows[6] = {5, 6, 7, -1, -1, 8};
constexpr int kSpecialRows[6] = {9, 10, 11, 12, 13, 8};
constexpr int kNovelRows[6] = {14, 15, -1, -1, -1, 8};
// The rows' hit widths (0x4c00e8, 0x4c0118, 0x4c0148, 0x4c0178; each followed by a height the code
// does not read: the areas are 32 high).
constexpr int kMainW[6] = {140, 140, 140, 140, 0, 140};
constexpr int kChapterW[6] = {300, 240, 100, 0, 0, 65};
constexpr int kSpecialW[6] = {180, 140, 180, 140, 200, 65};
constexpr int kNovelW[6] = {300, 240, 0, 0, 0, 65};

constexpr int kRowY = 360, kRowStep = 40;  // the first row's top, the step (0xa72f04 = 360)
constexpr float kScrollEnd = 136;          // T0000 is 856 high: v runs to 856 - 720

// (n - x)^2 / pow(n, 2) / 5 + 1: the zoom of a fading-in row or logo (x87: the square and the quotient
// stored as floats, the rest in doubles).
float zoomIn(int n, int x) {
    float d = static_cast<float>(n - x);
    float q = d * d / static_cast<float>(std::pow(static_cast<double>(n), 2.0));
    return static_cast<float>(static_cast<double>(q) / 5.0 + 1.0);
}

// The countdowns of the states that run another menu: down to 0, never below.
int countDown(int c) {
    return std::max(c - 1, 0);
}

}  // namespace

Title::Title(Game& game) : game_(game) {}

bool Title::cpuFast() const {
    // 0xb9191c: the processor's clock as the exe measures it at startup. Every device Nyanroid runs on
    // counts as 1700 MHz or more.
    return true;
}

void Title::startWeather(int fastFlags, int slowFlags) {
    bool fast = cpuFast();
    game_.effects().weather(fast ? fastFlags : slowFlags, 0, fast ? 3 : 5, 0, fast ? 400 : 200, 1, 1);
}

int Title::step() {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    const SystemData& sys = game_.system;
    // 0x44b6f0(-1): the hit area under the pointer in the current group.
    int hovered = in.hoveredArea();
    bool click = game_.keys.advance;  // 0xa39770
    bool cancel = game_.keys.cancel;  // 0xa39778
    int flag0 = sys.gameFlag(0), flag5 = sys.gameFlag(5);
    bool ic = game_.assets().introductoryChapterBundled();  // 0xa730b0: IC\bak.pak exists
    // 0xa730ac: the introductory chapter installed on its own (a registry key with its movies and
    // archives); never on Android.
    constexpr bool icInstalled = false;
    int fadeStart = game_.settings.speed != 1.0f ? 0x40 : 0;  // 0x4be004 != 1.0: no fade-in
    int result = -1;

    if (game_.assets().build() == Build::Special && stepSpecial(result)) {
        game_.effects().weatherStep();
        return result;
    }
    switch (state_) {
    case 0:
        if (game_.modes.replay == 0) {
            sound.stopBgm(0);
            game_.chapterDigit = 0;  // 0xa391f0
            game_.startMovie("mv00", 0);
            state_ = 1;
        } else {
            state_ = 4;
        }
        break;
    case 1:
        if (game_.moviePlaying()) break;
        count_ = 0;
        if (!ic) state_ = sys.gameFlag(0xd2) != 0 ? 2 : 4;
        else state_ = sys.gameFlag(0xd2) != 0 || sys.gameFlag(0xca) != 0 ? 2 : 4;
        break;
    case 2:
        if (count_ > 2) {
            if (ic && sys.gameFlag(0xd2) == 0) {
                game_.system.setGameFlag(0x68, 1);
                game_.writeSystem();
                game_.chapterDigit = 1;
                game_.startMovie("mv02", 0);
            } else {
                game_.chapterDigit = 0;
                game_.startMovie("mv10", 0);
            }
            state_ = 3;
        }
        ++count_;
        break;
    case 3:
        if (game_.moviePlaying()) break;
        count_ = 0;
        state_ = 4;
        break;
    case 4:
        setUp();
        break;
    case 5: {
        // The end: everything released (the glyph atlases reloaded); a new game's script starts.
        game_.effects().resetWeather(true);
        for (int obj = 0; obj < 0x572; ++obj) gfx.remove(obj);
        for (int slot = 0; slot < 0x1ba; ++slot) gfx.release(slot);
        for (int a = 0; a < 0x13b; ++a) game_.ani().release(a);
        in.clearAllAreas();
        in.setGroup(0);
        result = result_;
        if (result == 3 && !loaded_) game_.startStory(script_, 0);  // 0x4178b0, 0x40e960
        count_ = 0;
        state_ = 0;
        gfx.setTint(0x80, 0x80, 0x80, 0);
        break;
    }
    case 6: {
        // The background from black, scrolling.
        ++count_;
        scroll_ = static_cast<float>(scroll_ + static_cast<double>(1.2f));
        gfx.setRect(1, 0, 0, 0, scroll_, 1280, 720);
        int t = (count_ << 7) / 30;
        gfx.setTint(t, t, t, 0);
        gfx.setVisible(1, true);
        gfx.setBrightness(1, 0x80);
        if (count_ > 30) {
            count_ = 0;
            gfx.setBrightness(1, 0x80);
            gfx.setTint(0x80, 0x80, 0x80, 0);
            state_ = 7;
        }
        break;
    }
    case 7: {
        // The logo zooms in, then the rows one after another; the theme (BGM 31) starts.
        scroll_ = static_cast<float>(scroll_ + static_cast<double>(1.2f));
        gfx.setRect(1, 0, 0, 0, scroll_, 1280, 720);
        if (count_ == 0) sound.playBgm(31, 0, 1, 0xff, 0, 0);
        ++count_;
        for (int obj = 2; obj <= 5; ++obj) gfx.setVisible(obj, true);
        int s = std::min(std::max(count_, 0), 60);
        gfx.setScale(2, zoomIn(60, s));
        for (int obj = 2; obj <= 5; ++obj) gfx.setAlpha(obj, (s << 8) / 60);
        for (int i = 0; i < 6; ++i) {
            if (kMainRows[i] == -1 || (flag0 == 0 && ic && i == 3)) continue;
            gfx.setVisible(kLabel + i, true);
            gfx.setVisible(kPlate + i, true);
            int r = count_ - i * 10 - 10;
            r = r < 1 ? 0 : std::min(r, 20);
            gfx.setAlpha(kLabel + i, (r << 8) / 20);
            gfx.setAlpha(kPlate + i, (r << 8) / 20);
            float z = zoomIn(20, r);
            gfx.setScale(kLabel + i, z);
            gfx.setScale(kPlate + i, z);
        }
        if (count_ > 80 || click) {
            count_ = 30;
            for (int obj = 2; obj <= 5; ++obj) gfx.setAlpha(obj, 0xff);
            gfx.setScale(2, 1);
            for (int i = 0; i < 6; ++i) {
                gfx.setAlpha(kLabel + i, 0xff);
                gfx.setScale(kLabel + i, 1);
                gfx.setAlpha(kPlate + i, 0xff);
                gfx.setScale(kPlate + i, 1);
            }
            if (click) scroll_ = kScrollEnd;
            state_ = 0x10;
        }
        break;
    }
    case 8: {
        // To black before the story (or a loaded save); the snow fades, the theme goes in 29 frames.
        if (count_ == 0) {
            game_.effects().resetWeather(false);
            sound.stopBgm(0x1d);
        }
        ++count_;
        int t = 0x80 - (count_ << 7) / 30;
        gfx.setTint(t, t, t, 0);
        if (count_ > 44) {
            sound.stopBgm(0);
            state_ = 5;
        }
        break;
    }
    case 9: {
        // Exit: the menu fades, then the screen.
        if (count_ == 0) sound.stopBgm(0x1e);
        ++count_;
        int a = (0x10 - count_) * 0x10;
        for (int obj = 2; obj <= 5; ++obj) gfx.setAlpha(obj, a);
        for (int i = 0; i < 6; ++i) {
            gfx.setAlpha(kLabel + i, a);
            gfx.setAlpha(kPlate + i, a);
            gfx.setAlpha(kHighlight + i, a);
            gfx.setAlpha(kHeader + i, a);
        }
        int t = std::clamp(((100 - count_) << 7) / 20, 0, 0x80);
        gfx.setTint(t, t, t, 0);
        if (count_ > 119) state_ = 5;
        break;
    }
    case 10: {
        count_ = countDown(count_);
        int r = loadMenu();
        if (r == 0) break;
        if (r != 1) {
            loaded_ = true;
            result_ = 3;
            count_ = 0;
            state_ = 8;
            break;
        }
        count_ = 0x40;
        in.setGroup(0);
        state_ = 0x10;
        break;
    }
    case 11: {
        count_ = countDown(count_);
        int r = optionsMenu();
        if (r == 0) break;
        if (r == 2) {
            count_ = 0x10;
            state_ = 10;
            break;
        }
        count_ = 0x40;
        in.setGroup(0);
        state_ = 0x10;
        break;
    }
    case 12:
    case 14:
    case 15: {
        count_ = countDown(count_);
        int r = state_ == 12 ? cgMenu() : state_ == 14 ? musicMenu() : voiceMenu();
        if (r == 1) {
            count_ = 0x40;
            in.setGroup(1);
            state_ = 0x11;
        }
        break;
    }
    case 13: {
        if (game_.modes.replay == 0) {
            // Opened from Specials (not back from a replay): the title stays as Specials shows it.
            gfx.setTint(0x80, 0x80, 0x80, 0);
            gfx.setVisible(1, true);
            gfx.setBrightness(1, 0x80);
            scroll_ = kScrollEnd;
            gfx.setRect(1, 0, 0, 0, kScrollEnd, 1280, 720);
            for (int obj = 2; obj <= 5; ++obj) gfx.setVisible(obj, true);
            for (int i = 0; i < 6; ++i) {
                bool on = kSpecialRows[i] != -1 && in.areaActive(1, i);
                gfx.setVisible(kLabel + i, on);
                gfx.setVisible(kLabel + i + 10, on);
                if (on) gfx.moveSource(kLabel + i, 0, static_cast<float>(kSpecialRows[i] * 40));
            }
        }
        count_ = countDown(count_);
        int r = replayMenu();
        if (r == 0) break;
        if (r != 1) {
            result_ = r - 2;
            count_ = 0;
            state_ = 8;
            break;
        }
        scroll_ = kScrollEnd;
        count_ = 0x40;
        in.setGroup(1);
        state_ = 0x11;
        break;
    }
    case 0x10: {
        // The menu.
        scroll_ = static_cast<float>(scroll_ + static_cast<double>(1.2f));
        if (click || scroll_ >= kScrollEnd) scroll_ = kScrollEnd;
        gfx.setRect(1, 0, 0, 0, scroll_, 1280, 720);
        if (scroll_ == kScrollEnd && !game_.effects().weatherOn()) startWeather(0xe03, 0x603);
        loaded_ = false;
        for (int i = 0; i < 6; ++i) {
            if (kMainRows[i] == -1) {
                gfx.setVisible(kLabel + i, false);
                gfx.setVisible(kPlate + i, false);
            } else {
                gfx.setVisible(kLabel + i, true);
                gfx.setVisible(kPlate + i, true);
                int a = (count_ - i * 2) * 0x10;
                gfx.setAlpha(kLabel + i, a);
                gfx.setAlpha(kPlate + i, a);
                gfx.setAlpha(kHighlight + i, a);
                gfx.moveSource(kLabel + i, 0, static_cast<float>(kMainRows[i] * 40));
            }
            gfx.setVisible(kHighlight + i, false);
        }
        gfx.setVisible(kHeader, true);
        gfx.setSource(kHeader, 0, 0, 288, 40);
        gfx.setAlpha(kHeader, count_ << 4);
        gfx.setVisible(kNote, false);
        ++count_;
        if (hovered != -1) {
            if (hovered < 6) {
                hover(hovered, kMainRows, kLabel + hovered, kHighlight + hovered);
                hoverCount_[static_cast<size_t>(hovered)] = std::clamp(hoverCount_[static_cast<size_t>(hovered)] + 2, 0, 0x10);
            }
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                gfx.setVisible(kHighlight + hovered, true);
                backState_ = 0x10;
                switch (hovered) {
                case 0:  // New Game
                    if (ic) {
                        if (flag0 == 0) {
                            start("1001");
                            break;
                        }
                    } else if (!icInstalled && flag5 == 0) {
                        start("2001");
                        break;
                    }
                    sound.systemSe(Sound::Decide);
                    chapterAreas();
                    if (flag0 == 0) in.enableArea(1, 1, false);
                    if (!ic) in.enableArea(1, 1, true);
                    if (flag5 == 0) in.enableArea(1, 2, false);
                    in.setGroup(1);
                    count_ = fadeStart;
                    state_ = 0x12;
                    break;
                case 1:  // Continue
                    count_ = 0x10;
                    game_.saveMenu().open(2, false);
                    state_ = 10;
                    sound.systemSe(Sound::Decide);
                    break;
                case 2:  // Options
                    game_.options().open(1);
                    count_ = 0x10;
                    state_ = 11;
                    sound.systemSe(Sound::Decide);
                    break;
                case 3:  // Specials
                    sound.systemSe(Sound::Decide);
                    specialsAreas();
                    in.setGroup(1);
                    count_ = fadeStart;
                    state_ = 0x11;
                    break;
                case 5:  // Exit
                    result_ = 1;
                    count_ = 0;
                    state_ = 9;
                    sound.systemSe(Sound::Decide);
                    break;
                default:
                    break;
                }
            }
        }
        lastHover_ = hovered;
        break;
    }
    case 0x11: {
        // Specials: Digital Novel, CG Mode, Scene Replay, Music Mode, Voice Message, Back.
        sound.playBgm(31, 0, 1, 0xff, 0, 0);
        showRows(kSpecialRows, 1, 80);
        if (hovered != -1) {
            hover(hovered, kSpecialRows, kLabel + hovered, kHighlight + hovered);
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                gfx.setVisible(kHighlight + hovered, true);
                backState_ = 0x11;
                switch (hovered) {
                case 0:
                    sound.systemSe(Sound::Decide);
                    for (int i = 0; i < 6; ++i) in.setArea(2, i, 640 - kNovelW[i] / 2, kRowY + i * kRowStep, kNovelW[i], 32, true);
                    in.setGroup(2);
                    count_ = fadeStart;
                    state_ = 0x13;
                    break;
                case 1:
                case 2:
                case 3:
                case 4:
                    count_ = 0x10;
                    state_ = 0xb + hovered;
                    sound.systemSe(Sound::Decide);
                    break;
                case 5:
                    back(0x10, 0, 0x11);
                    break;
                default:
                    break;
                }
            }
        }
        if (cancel) back(0x10, 0, 0x11);
        lastHover_ = hovered;
        break;
    }
    case 0x12: {
        // The chapters.
        if (!icInstalled && !ic) {
            // Without the introductory chapter: closing chapter and coda in the first two rows, Back in
            // the last; the hit areas are numbered from 1.
            const int* rows = kChapterRows + 1;
            for (int i = 0; i < 5; ++i) {
                if (rows[i] == -1 || !in.areaActive(1, i + 1)) {
                    gfx.setVisible(kLabel + i, false);
                    gfx.setVisible(kPlate + i, false);
                } else {
                    gfx.setVisible(kLabel + i, true);
                    gfx.setVisible(kPlate + i, true);
                    int a = (count_ - i * 2) * 0x10;
                    gfx.setAlpha(kLabel + i, a);
                    gfx.setAlpha(kPlate + i, a);
                    gfx.setAlpha(kHighlight + i, a);
                    if (i == 4) {
                        gfx.setVisible(kLabel + 4, false);
                        gfx.setVisible(kPlate + 4, false);
                        gfx.moveSource(kLabel + 5, 0, static_cast<float>(kChapterRows[5] * 40));
                    } else {
                        gfx.moveSource(kLabel + i, 0, static_cast<float>(rows[i] * 40));
                    }
                }
                gfx.setVisible(kHighlight + i, false);
            }
            gfx.setVisible(kLabel + 5, true);
            gfx.setVisible(kPlate + 5, true);
            gfx.setAlpha(kLabel + 5, (count_ - 10) * 0x10);
            gfx.setAlpha(kPlate + 5, (count_ - 10) * 0x10);
            gfx.setAlpha(kHighlight + 5, (count_ - 10) * 0x10);
            gfx.setVisible(kHighlight + 5, false);
            gfx.setVisible(kHeader, true);
            gfx.setSource(kHeader, 0, 40, 288, 40);
            gfx.setAlpha(kHeader, count_ << 4);
            gfx.setVisible(kNote, false);
            ++count_;
        } else {
            showRows(kChapterRows, 1, 40);
        }
        if (hovered != -1) {
            if (!icInstalled && !ic) {
                if (hovered == 5) hover(5, kChapterRows, kLabel + 5, kHighlight + 5);
                else hover(hovered, kChapterRows, kLabel + hovered - 1, kHighlight + hovered - 1);
            } else {
                hover(hovered, kChapterRows, kLabel + hovered, kHighlight + hovered);
            }
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                backState_ = 0x12;
                switch (hovered) {
                case 0: start("1001"); break;
                case 1: start("2001"); break;
                case 2: start("3001"); break;
                case 5: back(0x10, 0, 0x12); break;
                default: break;
                }
            }
        }
        if (cancel) back(0x10, 0, 0x12);
        lastHover_ = hovered;
        break;
    }
    case 0x13: {
        // The Digital Novels.
        showRows(kNovelRows, 2, 120);
        if (hovered != -1) {
            hover(hovered, kNovelRows, kLabel + hovered, kHighlight + hovered);
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                gfx.setVisible(kHighlight + hovered, true);
                backState_ = 0x13;
                if (hovered == 0) start("5000");
                else if (hovered == 1) start("5100");
                else if (hovered == 5) back(0x11, 1, 0x13);
            }
        }
        if (cancel) back(0x11, 1, 0x13);
        lastHover_ = hovered;
        break;
    }
    default:
        break;
    }
    // 0x407900: the snow's frame, whatever the state.
    game_.effects().weatherStep();
    return result;
}

void Title::setUp() {
    // State 4: the pictures, the objects and the menu's hit areas.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    gfx.load(0, "T0000.tga", 2, false, "", "", 0x100, "");
    gfx.load(1, "T0100.tga", 2, false, "", "", 0x100, "");
    bool ic = game_.assets().introductoryChapterBundled();
    constexpr bool icInstalled = false;
    gfx.load(2, ic ? "T0011.tga" : "T0010.tga", 2, false, "", "", 0x100, "");
    gfx.load(3, "T0020.tga", 2, false, "", "", 0x100, "");
    gfx.load(4, "T0050.tga", 2, false, "", "", 0x100, "");
    gfx.load(5, "T0101.tga", 1, false, "", "", 0x100, "");
    gfx.load(6, "T0200.tga", 1, false, "", "", 0x100, "");
    gfx.load(7, "T0201.tga", 1, false, "", "", 0x100, "");
    game_.message().begin();  // 0x40a3a0: the window's pictures
    in.clearAllAreas();
    in.setGroup(0);
    gfx.setTint(0, 0, 0, 0);
    // The background, black until the fade-in; the logo; the copyright line (T0020); T0050 only for
    // the introductory chapter installed on its own.
    gfx.makeImage(1, 0, 2, true);
    gfx.setRect(1, 0, 0, 0, 0, 1280, 720);
    scroll_ = 0;
    gfx.setColor(1, 0xff, 0, 0, 0);
    gfx.setPivot(1, 100, 360);
    gfx.makeImage(2, 2, 0x104e, false);
    gfx.setRect(2, 264, 0, 0, 0, 752, 448);
    gfx.setBlend(2, 1);
    gfx.setFilter(2, 2);
    gfx.setPivot(2, 640, 224);
    gfx.makeImage(3, 3, 0x104e, false);
    gfx.setRect(3, 992, 684, 0, 0, 264, 24);
    gfx.setBlend(3, 1);
    gfx.setFilter(3, 2);
    if (icInstalled && !ic) {
        gfx.makeImage(4, 4, 0x104e, false);
        gfx.setRect(4, 432, 684, 0, 0, 408, 24);
        gfx.setBlend(4, 1);
        gfx.setFilter(4, 2);
    }
    gfx.makeImage(kHeader, 7, 0x104f, false);
    gfx.setSource(kHeader, 0, 0, 288, 40);
    gfx.setDest(kHeader, 496, 608, 288, 40);
    gfx.makeImage(kNote, 6, 0x104f, false);
    gfx.setSource(kNote, 0, 0, 288, 20);
    gfx.setDest(kNote, 496, 608, 288, 20);
    for (int i = 0; i < 6; ++i) {
        hoverCount_[static_cast<size_t>(i)] = 0;
        int y = kRowY + i * kRowStep;
        auto fy = static_cast<float>(y);
        in.setArea(0, i, 640 - kMainW[i] / 2, y, kMainW[i], 32, true);
        gfx.makeImage(kLabel + i, 1, 0x1050, false);
        gfx.setSource(kLabel + i, 0, static_cast<float>(kMainRows[i] * 40), 320, 40);
        gfx.setDest(kLabel + i, 480, fy, 320, 40);
        gfx.setPivot(kLabel + i, 640, fy + 20);
        gfx.makeImage(kPlate + i, 5, 0x104e, false);
        gfx.setSource(kPlate + i, 0, 0, 320, 40);
        gfx.setDest(kPlate + i, 480, fy, 320, 40);
        gfx.setPivot(kPlate + i, 640, fy + 20);
        gfx.makeImage(kHighlight + i, 5, 0x104f, false);
        gfx.setSource(kHighlight + i, 0, 40, 320, 40);
        gfx.setDest(kHighlight + i, 480, fy, 320, 40);
        gfx.setPivot(kHighlight + i, 640, fy + 20);
        gfx.setBlend(kHighlight + i, 3);
    }
    if (game_.system.gameFlag(0) == 0 && ic) {
        // Specials stays hidden until the introductory chapter is finished.
        in.enableArea(0, 3, false);
        gfx.remove(kLabel + 3);
        gfx.remove(kPlate + 3);
        gfx.remove(kHighlight + 3);
    }
    result_ = 3;
    backState_ = -1;
    lastHover_ = -1;
    count_ = 0;
    gfx.setTint(0, 0, 0, 0);
    if (game_.modes.replay == 0) {
        game_.runEx = 1;  // 0x4c223c
        state_ = 6;
        return;
    }
    // Back from a scene replay: straight into the scene replay menu, the snow already falling.
    count_ = 0;
    state_ = 0xd;
    startWeather(0xf03, 0x703);
    specialsAreas();
}

void Title::specialsAreas() {
    Input& in = game_.input;
    const SystemData& sys = game_.system;
    for (int i = 0; i < 6; ++i) in.setArea(1, i, 640 - kSpecialW[i] / 2, kRowY + i * kRowStep, kSpecialW[i], 32, true);
    if (sys.gameFlag(0) == 0 && game_.assets().introductoryChapterBundled()) {
        in.enableArea(1, 0, false);  // Digital Novel
        in.enableArea(1, 3, false);  // Music Mode
    }
    if (sys.gameFlag(3) == 0) {
        in.enableArea(1, 1, false);  // CG Mode
        in.enableArea(1, 2, false);  // Scene Replay
    }
    // Voice Message: flags 20..25 and 30..33 (their lowest bits) all set.
    int all = 1;
    for (int f : {0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1e, 0x1f, 0x20, 0x21}) all &= sys.gameFlag(f);
    if ((all & 1) == 0) in.enableArea(1, 4, false);
}

void Title::chapterAreas() {
    Input& in = game_.input;
    constexpr bool icInstalled = false;
    if (!icInstalled && !game_.assets().introductoryChapterBundled()) {
        in.enableArea(1, 0, false);
        for (int i = 0; i < 5; ++i) {
            int index = i == 4 ? 5 : i + 1;
            int w = kChapterW[index];
            int y = i == 4 ? 560 : kRowY + i * kRowStep;
            in.setArea(1, index, 640 - w / 2, y, w, 32, true);
        }
    } else {
        for (int i = 0; i < 6; ++i) in.setArea(1, i, 640 - kChapterW[i] / 2, kRowY + i * kRowStep, kChapterW[i], 32, true);
    }
}

void Title::showRows(const int* rows, int group, int headerV) {
    // A sub-menu's rows: shown where the row exists and its hit area is active, fading in two frames
    // apart; the header; no description until a row is hovered.
    Gfx& gfx = game_.gfx();
    for (int i = 0; i < 6; ++i) {
        if (rows[i] == -1 || !game_.input.areaActive(group, i)) {
            gfx.setVisible(kLabel + i, false);
            gfx.setVisible(kPlate + i, false);
        } else {
            gfx.setVisible(kLabel + i, true);
            gfx.setVisible(kPlate + i, true);
            int a = (count_ - i * 2) * 0x10;
            gfx.setAlpha(kLabel + i, a);
            gfx.setAlpha(kPlate + i, a);
            gfx.setAlpha(kHighlight + i, a);
            gfx.moveSource(kLabel + i, 0, static_cast<float>(rows[i] * 40));
        }
        gfx.setVisible(kHighlight + i, false);
    }
    gfx.setVisible(kHeader, true);
    gfx.setSource(kHeader, 0, static_cast<float>(headerV), 288, 40);
    gfx.setAlpha(kHeader, count_ << 4);
    gfx.setVisible(kNote, false);
    ++count_;
}

void Title::hover(int row, const int* rows, int labelObj, int highlightObj) {
    // The hovered row: its label in the lit column, its highlight and its description.
    Gfx& gfx = game_.gfx();
    gfx.moveSource(labelObj, 320, static_cast<float>(rows[row] * 40));
    gfx.setVisible(highlightObj, true);
    gfx.setVisible(kNote, true);
    gfx.setSource(kNote, 0, static_cast<float>(rows[row] * 20), 288, 20);
}

void Title::back(int toState, int group, int fromState) {
    if (fromState == 0x13) {
        game_.input.setGroup(1);
        game_.input.clearAreas(2);
    } else {
        game_.input.setGroup(group);
    }
    backState_ = fromState;
    count_ = game_.settings.speed != 1.0f ? 0x40 : 0;
    state_ = toState;
    game_.sound().systemSe(Sound::Cancel);
}

void Title::start(const char* script) {
    // A chapter or a Digital Novel: the start sound (0x40ea30), then state 8 fades to the story.
    script_ = script;
    count_ = 0;
    state_ = 8;
    game_.sound().se(0x22c9, 0xff);
}

int Title::loadMenu() {
    return game_.saveMenu().step();
}

int Title::optionsMenu() {
    return game_.options().step();
}

int Title::cgMenu() {
    return game_.cgMode().step(0x4a4, 10, 0x1050, 3);  // 0x42b300(1, 0, 0x4a4, 10, 0, 0x1050, 3)
}

int Title::replayMenu() {
    int r = game_.sceneReplay().step(0x4a4, 10, 0x1050, 3);  // 0x42d910(1, 0, 0x4a4, 10, 0, 0x1050, 3)
    if (r == 5) script_ = "9999";  // 0x42d910 writes it with the pick: script 9999 loads the scene
    return r;
}

int Title::musicMenu() {
    return game_.musicRoom().step(0x4a4, 10, 0x1050, 3);  // 0x428250(1, 0, 0x4a4, 10, 0, 0x1050, 3)
}

int Title::voiceMenu() {
    return game_.voiceMessage().step(0x4a4, 10, 0x1050, 3);  // 0x42a520(1, 0, 0x4a4, 10, 0, 0x1050, 3)
}

}  // namespace nyan::wa2
