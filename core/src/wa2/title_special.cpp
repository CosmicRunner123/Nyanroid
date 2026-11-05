// Special Contents' title (WA2_Special.exe 0x424cb0): WA2.exe's title with eight rows per menu and the
// product's own menus. The main menu: Mini-After Story, Digital Novels, Voice Dramas, Extra Episode,
// Options, Exit; each sub-menu lists its stories, then Continue (not for the dramas), Options and
// Return. WA2's Specials, chapters and galleries (states 0xc, 0xd, 0xf, 0x12, 0x13) are still in the
// code but nothing reaches them.

#include <algorithm>
#include <cmath>

#include "nyan/wa2/game.hpp"
#include "nyan/wa2/title.hpp"

namespace nyan::wa2 {

namespace {

// Te100's rows each menu shows (-1: none) and the rows' hit widths (each followed by a height of 32
// the code does not read).
constexpr int kMainRows[8] = {0, 1, 2, 3, -1, -1, 22, 24};       // 0x4bd020
constexpr int kAfterRows[8] = {4, 5, -1, -1, -1, 21, 22, 23};    // 0x4bd040: Mini-After Story
constexpr int kNovelRows[8] = {6, 7, 8, 9, 10, 21, 22, 23};      // 0x4bd060: Digital Novels
constexpr int kDramaRows[8] = {14, 15, 16, 17, 18, 19, 22, 23};  // 0x4bd080: Voice Dramas
constexpr int kExtraRows[8] = {20, -1, -1, -1, -1, 21, 22, 23};  // 0x4bd0a0: Extra Episode
constexpr int kMainW[8] = {390, 390, 390, 390, 0, 0, 390, 65};      // 0x4bd0c0
constexpr int kAfterW[8] = {390, 390, 0, 0, 0, 390, 390, 65};       // 0x4bd100
constexpr int kNovelW[8] = {390, 390, 390, 390, 390, 390, 390, 390};  // 0x4bd140
constexpr int kDramaW[8] = {390, 390, 390, 390, 390, 390, 390, 65};  // 0x4bd180
constexpr int kExtraW[8] = {390, 0, 0, 0, 0, 390, 390, 390};        // 0x4bd1c0

constexpr int kRowY = 312;  // the first row's top (0x138), rows 40 apart
constexpr float kScrollEnd = 136;
constexpr int kLabel = 0xb, kPlate = 0x15, kHighlight = 0x1f, kHeader = 0x29, kNote = 0x2a;

float zoomIn(int n, int x) {
    float d = static_cast<float>(n - x);
    float q = d * d / static_cast<float>(std::pow(static_cast<double>(n), 2.0));
    return static_cast<float>(static_cast<double>(q) / 5.0 + 1.0);
}

int countDown(int c) {
    return std::max(c - 1, 0);
}

// One row: its label (Te100, a 400-px column; the lit one beside it), plate and highlight (T0101).
void makeRow(Gfx& gfx, int label, int plate, int highlight, int row, float labelX, float plateX, float y) {
    gfx.makeImage(label, 1, 0x1050, false);
    gfx.setSource(label, 0, static_cast<float>(row * 40), 400, 40);
    gfx.setDest(label, labelX, y, 400, 40);
    gfx.setPivot(label, labelX + 200, y + 20);
    gfx.makeImage(plate, 5, 0x104e, false);
    gfx.setSource(plate, 0, 0, 320, 40);
    gfx.setDest(plate, plateX, y, 320, 40);
    gfx.setPivot(plate, labelX + 200, y + 20);
    gfx.makeImage(highlight, 5, 0x104f, false);
    gfx.setSource(highlight, 0, 40, 320, 40);
    gfx.setDest(highlight, plateX, y, 320, 40);
    gfx.setPivot(highlight, labelX + 200, y + 20);
    gfx.setBlend(highlight, 3);
}

}  // namespace

void Title::setUpSpecial() {
    // State 4.
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    bool ic = game_.assets().introductoryChapterBundled();
    gfx.load(0, "Te000.tga", 2, false, "", "", 0x100, "");
    gfx.load(1, "Te100.tga", 2, false, "", "", 0x100, "");
    gfx.load(2, ic ? "Te011.tga" : "T0010.tga", 2, false, "", "", 0x100, "");
    gfx.load(3, "T0020.tga", 2, false, "", "", 0x100, "");
    gfx.load(4, "T0050.tga", 2, false, "", "", 0x100, "");
    gfx.load(5, "T0101.tga", 1, false, "", "", 0x100, "");
    gfx.load(6, "Te200.tga", 1, false, "", "", 0x100, "");
    gfx.load(7, "Te201.tga", 1, false, "", "", 0x100, "");
    game_.message().begin();
    in.clearAllAreas();
    in.setGroup(0);
    gfx.setTint(0, 0, 0, 0);
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
    gfx.makeImage(kHeader, 7, 0x104f, false);
    gfx.setSource(kHeader, 0, 0, 288, 40);
    gfx.setDest(kHeader, 496, 648, 288, 40);
    gfx.makeImage(kNote, 6, 0x104f, false);
    gfx.setSource(kNote, 0, 0, 288, 20);
    gfx.setDest(kNote, 496, 648, 288, 20);
    hoverCount_.fill(0);
    specialMenuObjects();
    result_ = 3;
    backState_ = -1;
    lastHover_ = -1;
    count_ = 0;
    gfx.setTint(0, 0, 0, 0);
    // (Back from a scene replay the exe would open WA2's replay menu; nothing in this product starts
    // one.)
    game_.runEx = 1;
    state_ = 6;
}

void Title::specialMenuObjects() {
    Gfx& gfx = game_.gfx();
    for (int i = 0; i < 8; ++i) {
        int y = kRowY + i * 40;
        game_.input.setArea(0, i, 640 - kMainW[i] / 2, y, kMainW[i], 32, true);
        makeRow(gfx, kLabel + i, kPlate + i, kHighlight + i, kMainRows[i], 440, 480, static_cast<float>(y));
    }
}

void Title::specialAreas(const int* widths) {
    for (int i = 0; i < 8; ++i) game_.input.setArea(2, i, 640 - widths[i] / 2, kRowY + i * 40, widths[i], 32, true);
}

void Title::specialNovelAreas() {
    // Two columns of five (the novels, then the patch's notes), then Continue, Options and Return.
    Input& in = game_.input;
    for (int i = 0; i < 9; ++i) in.setArea(2, i, i > 4 ? 660 : 220, (i % 5) * 40 + kRowY, kNovelW[i % 5], 32, true);
    for (int i = 5; i < 8; ++i) in.setArea(2, i + 4, 640 - kNovelW[i] / 2, 512 + (i - 5) * 40, kNovelW[i], 32, true);
}

void Title::specialNovelObjects() {
    // The left column: Te100 rows 6..10 (objects 0xb..0xf); the right: row 25 (0x2e), rows 11 and 12
    // (0x10, 0x11) and row 13 (0x2b; made with row 14, set to 13 every frame); the last three rows at
    // 512.
    Gfx& gfx = game_.gfx();
    for (int k = 0; k < 5; ++k) {
        makeRow(gfx, kLabel + k, kPlate + k, kHighlight + k, 6 + k, 220, 260, static_cast<float>(kRowY + k * 40));
    }
    makeRow(gfx, 0x2e, 0x2f, 0x30, 25, 660, 700, kRowY);
    for (int k = 0; k < 2; ++k) {
        makeRow(gfx, 0x10 + k, 0x1a + k, 0x24 + k, 11 + k, 660, 700, static_cast<float>(kRowY + 40 + k * 40));
    }
    makeRow(gfx, 0x2b, 0x2c, 0x2d, 14, 660, 700, kRowY + 120);
    for (int k = 0; k < 3; ++k) {
        makeRow(gfx, 0x12 + k, 0x1c + k, 0x26 + k, kNovelRows[5 + k], 440, 480, static_cast<float>(512 + k * 40));
    }
}

void Title::specialRows(const int* rows, int headerV) {
    Gfx& gfx = game_.gfx();
    for (int i = 0; i < 8; ++i) {
        if (rows[i] == -1 || !game_.input.areaActive(2, i)) {
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

void Title::specialHover(int labelObj, int row, int highlightObj) {
    Gfx& gfx = game_.gfx();
    gfx.moveSource(labelObj, 400, static_cast<float>(row * 40));
    gfx.setVisible(highlightObj, true);
    gfx.setVisible(kNote, true);
    gfx.setSource(kNote, 0, static_cast<float>(row * 20), 288, 20);
}

void Title::specialBack(int fromState) {
    Input& in = game_.input;
    backState_ = fromState;
    in.setGroup(1);
    in.clearAreas(2);
    count_ = game_.settings.speed != 1.0f ? 0x40 : 0;
    state_ = 0x11;
    game_.sound().systemSe(Sound::Cancel);
    in.setGroup(0);
}

bool Title::stepSpecial(int& result) {
    Gfx& gfx = game_.gfx();
    Input& in = game_.input;
    Sound& sound = game_.sound();
    const SystemData& sys = game_.system;
    int hovered = in.hoveredArea();
    bool click = game_.keys.advance;
    bool cancel = game_.keys.cancel;
    bool ic = game_.assets().introductoryChapterBundled();
    int fadeStart = game_.settings.speed != 1.0f ? 0x40 : 0;
    auto submenu = [&](int state, const int* widths) {
        sound.systemSe(Sound::Decide);
        specialAreas(widths);
        in.setGroup(2);
        count_ = fadeStart;
        state_ = state;
    };
    auto openLoad = [&](int from) {
        backState_ = from;
        count_ = 0x10;
        game_.saveMenu().open(2, false);
        state_ = 10;
        sound.systemSe(Sound::Decide);
    };
    auto openOptions = [&](int from) {
        backState_ = from;
        game_.options().open(1);
        count_ = 0x10;
        state_ = 11;
        sound.systemSe(Sound::Decide);
    };
    // Back from the load screen or the options: the sub-menu's hit areas again.
    auto backToMenu = [&] {
        state_ = backState_;
        backState_ = 10;
        if (state_ == 0x15) specialNovelAreas();
        else if (state_ == 0x14) specialAreas(kAfterW);
        else if (state_ == 0x10) specialAreas(kDramaW);
        else if (state_ == 0x16) specialAreas(kExtraW);
        in.setGroup(2);
        count_ = 0x40;
    };

    switch (state_) {
    case 2:
        // The opening movie: the introductory chapter's until flag 210, else closing chapter's or,
        // with flag 220, coda's (none of them ships with this product).
        if (count_ > 2) {
            if (ic && sys.gameFlag(0xd2) == 0) {
                game_.system.setGameFlag(0x68, 1);
                game_.writeSystem();
                game_.chapterDigit = 1;
                game_.startMovie("mv02", 0);
            } else {
                game_.chapterDigit = 0;
                game_.startMovie(sys.gameFlag(0xdc) != 0 ? "mv20" : "mv10", 0);
            }
            state_ = 3;
        }
        ++count_;
        return true;
    case 4:
        setUpSpecial();
        return true;
    case 7: {
        scroll_ = static_cast<float>(scroll_ + static_cast<double>(1.2f));
        gfx.setRect(1, 0, 0, 0, scroll_, 1280, 720);
        if (count_ == 0) sound.playBgm(31, 0, 1, 0xff, 0, 0);
        ++count_;
        for (int obj = 2; obj <= 5; ++obj) gfx.setVisible(obj, true);
        int s = std::min(std::max(count_, 0), 60);
        gfx.setScale(2, zoomIn(60, s));
        for (int obj = 2; obj <= 5; ++obj) gfx.setAlpha(obj, (s << 8) / 60);
        for (int i = 0; i < 8; ++i) {
            if (kMainRows[i] == -1) continue;
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
            for (int i = 0; i < 8; ++i) {
                gfx.setAlpha(kLabel + i, 0xff);
                gfx.setScale(kLabel + i, 1);
                gfx.setAlpha(kPlate + i, 0xff);
                gfx.setScale(kPlate + i, 1);
            }
            if (click) scroll_ = kScrollEnd;
            state_ = 0x11;
        }
        return true;
    }
    case 9: {
        // Exit: the menu fades, then the screen.
        if (count_ == 0) sound.stopBgm(0x1e);
        ++count_;
        int a = (0x10 - count_) * 0x10;
        for (int obj = 2; obj <= 5; ++obj) gfx.setAlpha(obj, a);
        for (int i = 0; i < 8; ++i) {
            gfx.setAlpha(kLabel + i, a);
            gfx.setAlpha(kPlate + i, a);
            gfx.setAlpha(kHighlight + i, a);
            gfx.setAlpha(kHeader + i, a);
        }
        int t = std::clamp(((100 - count_) << 7) / 20, 0, 0x80);
        gfx.setTint(t, t, t, 0);
        if (count_ > 119) state_ = 5;
        return true;
    }
    case 10: {
        count_ = countDown(count_);
        int r = game_.saveMenu().step();
        if (r == 0) return true;
        if (r != 1) {
            loaded_ = true;
            result_ = 3;
            count_ = 0;
            state_ = 8;
            return true;
        }
        backToMenu();
        return true;
    }
    case 11: {
        count_ = countDown(count_);
        int r = game_.options().step();
        if (r == 0) return true;
        if (r == 2) {
            count_ = 0x10;
            state_ = 10;
            return true;
        }
        if (backState_ == 0x11) {
            count_ = 0x40;
            in.setGroup(0);
            state_ = 0x11;
            return true;
        }
        backToMenu();
        return true;
    }
    case 0xe: {
        // A voice drama's player.
        count_ = countDown(count_);
        if (game_.voiceDrama().step(0x4a4, 10, 0x1050, 3) == 1) {
            state_ = 0x10;
            backState_ = 0xe;
            in.setArea(2, 0, 640 - kDramaW[0] / 2, kRowY, kDramaW[0], 32, true);
            in.setGroup(2);
            count_ = 0x40;
        }
        return true;
    }
    case 0x11: {
        // The main menu.
        scroll_ = static_cast<float>(scroll_ + static_cast<double>(1.2f));
        if (click || scroll_ >= kScrollEnd) scroll_ = kScrollEnd;
        gfx.setRect(1, 0, 0, 0, scroll_, 1280, 720);
        if (scroll_ == kScrollEnd && !game_.effects().weatherOn()) startWeather(0xe03, 0x603);
        loaded_ = false;
        for (int i = 0; i < 8; ++i) {
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
            if (hovered < 8) {
                specialHover(kLabel + hovered, kMainRows[hovered], kHighlight + hovered);
                auto& hc = hoverCount_[static_cast<size_t>(hovered)];
                hc = std::clamp(hc + 2, 0, 0x10);
            }
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                if (hovered < 8) gfx.setVisible(kHighlight + hovered, true);
                backState_ = 0x11;
                switch (hovered) {
                case 0: submenu(0x14, kAfterW); break;
                case 1:
                    sound.systemSe(Sound::Decide);
                    specialNovelAreas();
                    in.setGroup(2);
                    count_ = fadeStart;
                    specialNovelObjects();
                    state_ = 0x15;
                    break;
                case 2: submenu(0x10, kDramaW); break;
                case 3: submenu(0x16, kExtraW); break;
                case 6: openOptions(0x11); break;
                case 7:
                    result_ = 1;
                    count_ = 0;
                    state_ = 9;
                    sound.systemSe(Sound::Decide);
                    break;
                default: break;
                }
            }
        }
        lastHover_ = hovered;
        return true;
    }
    case 0x10:
    case 0x14:
    case 0x16: {
        // Voice Dramas, Mini-After Story, Extra Episode.
        const int* rows = state_ == 0x10 ? kDramaRows : state_ == 0x14 ? kAfterRows : kExtraRows;
        int from = state_;
        specialRows(rows, state_ == 0x10 ? 120 : state_ == 0x14 ? 40 : 160);
        if (hovered != -1) {
            specialHover(kLabel + hovered, rows[hovered], kHighlight + hovered);
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                gfx.setVisible(kHighlight + hovered, true);
                backState_ = from;
                if (hovered == 6) {
                    openOptions(from);
                } else if (hovered == 7) {
                    specialBack(from);
                    if (from == 0x10) sound.playBgm(31, 0, 1, 0xff, 0, 0);
                } else if (from == 0x10) {
                    // A drama: its player over the title, on the dramas' background.
                    sound.dramaPart = 0;
                    count_ = 0x10;
                    state_ = 0xe;
                    game_.voiceDrama().select(hovered);
                    gfx.load(0, "BG_VoiceDrama.tga", 2, false, "", "", 0x100, "");
                    gfx.setRect(1, 0, 0, 0, 0, 1280, 720);
                    sound.systemSe(Sound::Decide);
                } else if (from == 0x14) {
                    if (hovered == 0) start("6001");
                    else if (hovered == 1) start("6101");
                    else if (hovered == 5) openLoad(from);
                } else {
                    if (hovered == 0) start("4000");
                    else if (hovered <= 5) openLoad(from);
                }
            }
        }
        if (cancel) {
            specialBack(from);
            if (from == 0x10) sound.playBgm(31, 0, 1, 0xff, 0, 0);
        }
        lastHover_ = hovered;
        return true;
    }
    case 0x15: {
        // Digital Novels: the left column's five novels, the right column's four, then the last three.
        for (int k = 0; k < 5; ++k) {
            int o = kLabel + k;
            if (!in.areaActive(2, k)) {
                gfx.setVisible(o, false);
                gfx.setVisible(o + 10, false);
            } else {
                gfx.setVisible(o, true);
                gfx.setVisible(o + 10, true);
                int a = (count_ - k * 2) * 0x10;
                gfx.setAlpha(o, a);
                gfx.setAlpha(o + 10, a);
                gfx.setAlpha(o + 20, a);
                gfx.moveSource(o, 0, static_cast<float>(240 + k * 40));
            }
            gfx.setVisible(o + 20, false);
        }
        auto right = [&](int area, int label, int plate, int highlight, int fade, float v) {
            if (!in.areaActive(2, area)) {
                gfx.setVisible(label, false);
                gfx.setVisible(plate, false);
            } else {
                gfx.setVisible(label, true);
                gfx.setVisible(plate, true);
                int a = (count_ - fade) * 0x10;
                gfx.setAlpha(label, a);
                gfx.setAlpha(plate, a);
                gfx.setAlpha(highlight, a);
                gfx.moveSource(label, 0, v);
            }
            gfx.setVisible(highlight, false);
        };
        right(5, 0x2e, 0x2f, 0x30, 10, 1000);
        right(6, 0x10, 0x1a, 0x24, 10, 440);
        right(7, 0x11, 0x1b, 0x25, 12, 480);
        right(8, 0x2b, 0x2c, 0x2d, 14, 520);
        for (int k = 0; k < 3; ++k) {
            int o = 0x12 + k;
            if (kNovelRows[5 + k] == -1 || !in.areaActive(2, 9 + k)) {
                gfx.setVisible(o, false);
                gfx.setVisible(o + 10, false);
            } else {
                gfx.setVisible(o, true);
                gfx.setVisible(o + 10, true);
                int a = (count_ - 10 - k * 2) * 0x10;
                gfx.setAlpha(o, a);
                gfx.setAlpha(o + 10, a);
                gfx.setAlpha(o + 20, a);
                gfx.moveSource(o, 0, static_cast<float>(kNovelRows[5 + k] * 40));
            }
            gfx.setVisible(o + 20, false);
        }
        gfx.setVisible(kHeader, true);
        gfx.setSource(kHeader, 0, 80, 288, 40);
        gfx.setAlpha(kHeader, count_ << 4);
        gfx.setVisible(kNote, false);
        ++count_;
        // The main menu's rows again (they were remade as the novels' columns).
        auto backToMain = [&] {
            specialMenuObjects();
            for (int o : {0x13, 0x14, 0x1d, 0x1e, 0x27, 0x28, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30}) gfx.setVisible(o, false);
            specialBack(0x15);
        };
        if (hovered != -1) {
            if (hovered < 5) {
                gfx.moveSource(kLabel + hovered, 400, static_cast<float>(240 + hovered * 40));
                gfx.setSource(kNote, 0, static_cast<float>(120 + hovered * 20), 288, 20);
                gfx.setVisible(kHighlight + hovered, true);
            } else if (hovered == 5) {
                gfx.moveSource(0x2e, 400, 1000);
                gfx.setSource(kNote, 0, 500, 288, 20);
                gfx.setVisible(0x30, true);
            } else if (hovered < 8) {
                gfx.moveSource(hovered + 10, 400, static_cast<float>(hovered * 40 + 200));
                gfx.setSource(kNote, 0, static_cast<float>(hovered * 20 + 100), 288, 20);
                gfx.setVisible(hovered + 0x1e, true);
            } else if (hovered == 8) {
                gfx.moveSource(0x2b, 400, 520);
                gfx.setSource(kNote, 0, 260, 288, 20);
                gfx.setVisible(0x2d, true);
            } else {
                int row = kNovelRows[hovered - 4];
                gfx.moveSource(hovered + 9, 400, static_cast<float>(row * 40));
                gfx.setSource(kNote, 0, static_cast<float>(row * 20), 288, 20);
                gfx.setVisible(hovered + 0x1d, true);
            }
            gfx.setVisible(kNote, true);
            if (lastHover_ != hovered) sound.systemSe(Sound::Hover);
            if (click) {
                backState_ = 0x15;
                static constexpr const char* kScripts[9] = {"5400", "5000", "5300", "5200", "5100", "7300", "7000", "7100", "7200"};
                if (hovered < 9) start(kScripts[hovered]);
                else if (hovered == 9) openLoad(0x15);
                else if (hovered == 10) openOptions(0x15);
                else backToMain();
            }
        }
        if (cancel) backToMain();
        lastHover_ = hovered;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace nyan::wa2
