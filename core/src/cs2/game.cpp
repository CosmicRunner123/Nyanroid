#include "nyan/cs2/game.hpp"

#include "nyan/cs2/font_select.hpp"
#include "nyan/cs2/kcs_code.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nyan/common/sjis.hpp"
#include "nyan/common/xml.hpp"

namespace nyan::cs2 {
namespace {

// FileVersion of a Windows executable (from VS_FIXEDFILEINFO in its .rsrc section), packed as
// major.minor.build.revision in 16-bit fields; 0 if it has none.
uint64_t peFileVersion(const RandomAccessFile& f) {
    try {
        if (f.size() < 0x40) return 0;
        Bytes dos = f.readAt(0, 0x40);
        if (dos[0] != 'M' || dos[1] != 'Z') return 0;
        uint32_t pe = readLE32(&dos[0x3C]);
        Bytes coff = f.readAt(pe, 24);
        if (std::memcmp(coff.data(), "PE\0\0", 4) != 0) return 0;
        uint16_t sections = static_cast<uint16_t>(coff[6] | (coff[7] << 8));
        uint16_t optSize = static_cast<uint16_t>(coff[20] | (coff[21] << 8));
        Bytes table = f.readAt(pe + 24 + optSize, static_cast<size_t>(sections) * 40);
        for (uint16_t i = 0; i < sections; ++i) {
            const uint8_t* sec = &table[i * 40];
            if (std::memcmp(sec, ".rsrc", 5) != 0) continue;
            uint32_t rawSize = readLE32(sec + 16), rawOff = readLE32(sec + 20);
            if (static_cast<uint64_t>(rawOff) + rawSize > f.size()) return 0;
            Bytes rsrc = f.readAt(rawOff, rawSize);
            for (size_t k = 0; k + 16 <= rsrc.size(); k += 4) {
                if (readLE32(&rsrc[k]) != 0xFEEF04BD) continue;
                uint32_t ms = readLE32(&rsrc[k + 8]), ls = readLE32(&rsrc[k + 12]);
                return (static_cast<uint64_t>(ms) << 32) | ls;
            }
        }
    } catch (const std::exception&) {
    }
    return 0;
}


// How well a face answers a LOGFONT naming `want` at `weight`, upright (the engine's fonts,
// cs2.exe 0x6a8160: weight 400 unless bold is on, never italic), as Windows' font mapper chooses: -1
// if no name of the face is `want`; 0 for a face named by its own full name; else, for a face of
// that family, the distance from the weight, italic faces after upright ones (at 400 meiryo.ttc's
// メイリオ, at 700 meiryob.ttc's bold face of the same family). Lower is better.
int faceMatchScore(const Bytes& data, int face, const std::string& want, int32_t weight) {
    if (!fontRenderable(data, face)) return -1;
    const auto names = fontFaceNames(data, face);
    const auto equals = [&](const std::string& n) { return fontNameEquals(n, want); };
    if (std::none_of(names.begin(), names.end(), equals)) return -1;
    const auto families = fontFamilyNames(data, face);
    if (std::none_of(families.begin(), families.end(), equals)) return 0;
    const FontFamilyInfo info = fontFamilyInfo(data, face);
    return std::abs(info.weight - weight) + (info.italic ? 1000 : 0);
}
}  // namespace

Game::Game(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer)
    : folder_(std::move(folder)), assets_(AssetIndex::load(*folder_)), player_(assets_, vars_), mixer_(mixer) {
    loadConfig();
    loadSoundConf();
    loadNameTable();
    loadEngineVersion();
    // The window face banks start at `mpl 8` in Nekopara's sscript (2.6.1.76; objects 75 on) and at
    // `mpl 10` in Amakano's (2.6.1.90; 77 on): the sscript constant, gated on the engine's version.
    player_.setMplFaceFrom(engineVersion_ != 0 && engineVersion_ < 0x000200060001005Aull ? 8 : 10);
    player_.setWaitTagFrames(engineVersion_ != 0 && engineVersion_ < 0x000200060001005Aull ? 0 : 120);
    // The voice check (Cyanotype 0xa8833, REBELLIONS 0x08f5db): its <VOICE> entry not muted by
    // `voiceoff`, then the config's play for its character (native 606).
    player_.voiceAllowed = [this](const std::string& name) {
        const VoiceEntry* v = voiceFor(name);
        if (!v) return true;
        if (player_.voiceMuted(static_cast<size_t>(v - voices_.data()))) return false;
        if (v->group != 2 || v->character < 0) return true;
        if (soundConf_) {
            std::string key;
            const SoundPart* p = soundPart({}, {2, v->character}, key);
            return !p || key.find(' ') == std::string::npos || (soundValue(*p, key, "play") && !soundValue(*p, key, "mute"));
        }
        return conf("play 3 " + std::to_string(v->character), 1) != 0;
    };
    // Fruit's sscript (before 2.6.1.72) has the older sound command (Fruit 0x0383e3 / 0x03d171).
    // REBELLIONS (2.6.1.63) has its `end` (0x0498e4: the level kept) but the later play (0x04462a:
    // a pan and a time after the name); it never writes numbers after a sound's name, so this
    // gives it the right sound. Its `wait tag` default is 0 (0x05e58c) and it has no E-mote banks.
    player_.setOlderSoundCommand(engineVersion_ != 0 && engineVersion_ < kEngineLogsCurrentLine);
    // `mode` on a loaded image: Fruit's sscript calls native 737 (the colour conversion) only from its
    // image load (0x06f5a0), every later one from its per-frame step too (Happiness 2 0x08abbf), the
    // engine version no guide (REBELLIONS' 2.6.1.63 has the later sscript). One call: Fruit's.
    if (assets_.find("sscript.kcs")) player_.setModeEachFrame(kcsNativeCallSites(assets_.read("sscript.kcs"), 737) != 1);
    // `\t` text styles: read in the 2.6.1.90 exes (Yukikoi 0x5eaac0, Amakano 2, Cyanotype) and
    // Happiness 2's; Nekopara's 2.6.1.76 and Amakano 1's 2.6.1.73 have none. 2.6.1.80 (Hatsumira, LSG)
    // is not read; no game before 2.6.1.90 writes `\t` in its text.
    player_.setTextStyles(engineVersion_ >= 0x000200060001005Aull);
    player_.setFesLoader([this](const std::string& name, uint64_t frame) {
        return FesInstance::load(assets_, vars_, name, frame, &engineConfig_, this);
    });
    loadUserFont();
    theme_ = loadWindowTheme(assets_, config_.width, config_.height);
    // Engine config defaults (getconf/setconf), from the game's startup.xml where it defines them.
    engineConfig_["wndframe"] = 1;
    // `wndcolor` (cs2.exe 0x71ec40, grisaia.exe 0x5ac630): startup.xml's colour, its alpha made a
    // percentage (x 100 / 255, truncated) only when CONFIG_B/wnd_alpha is on; the window scripts read
    // the top byte as a percentage. Nekopara sets wnd_alpha 0 and gives 80 outright; the other games
    // with 0 set the colour themselves on first start (flow.fes `setconf 50<<24 wndcolor`).
    {
        const uint32_t a = config_.windowColor >> 24;
        engineConfig_["wndcolor"] = static_cast<int32_t>((config_.windowAlphaByte ? a * 100 / 255 : a) << 24 |
                                                         (config_.windowColor & 0xFFFFFF));
    }
    // The engine takes startup.xml's GAME/enabledrag (cs2.exe 0x720600; 1 in Fruit, Labyrinth, Eden,
    // Cyanotype, Nekopara), which lets the player drag the message window. Kept off on purpose: on a
    // tablet a draggable window would catch taps meant for the text (a known difference).
    engineConfig_["dragwnd"] = 0;
    // Bold text (`fontweight`, Nekopara cs2.exe setconf 0x77d770 -> settings +0xf900): every text
    // style the engine makes takes weight 700 with it on, else 400: the string planes' fonts
    // (kcTaskStringPlane 0x5e4530, its style fillers 0x634e30 / 0x635bf0 / 0x636a30) and the FE
    // STRING / NVSTRING / LOGSTRING objects' (0x74f2f0, `reset` 0x74f4e0), which keep following the
    // config's face and weight: a setconf marks the settings changed (+0x410) and the next frame
    // tells every task (message 0x13, 0x5dc080), whose strings re-read them (0x74f690) unless their
    // script set them (`font` / `fontind` the face 0x74e860, `weight <n>` the weight 0x74eeb0).
    engineConfig_["fontweight"] = fontBold_ ? 1 : 0;
    // The text edge (`fshadow <n>`, Nekopara cs2.exe getconf 0x77c800 / setconf 0x77d770: the running
    // settings' +0xf8f8 + 4 n, n at most 2, so `fshadow 2` is fontweight): 0 the edge all around
    // the glyph (+0xf8f8), 1 the copy right and down (+0xf8fc); their defaults startup.xml
    // FONT/shadow/around and rightdown (0x7202f0, +0xfd10 / +0xfd14, 1 when absent: 0x71d060).
    // Text styles take them with the face and weight, and FE strings keep following them (see
    // fontweight above; the FE `shadow` command that would end it, 0x74e8f0, is used by no game).
    engineConfig_["fshadow 0"] = config_.textAround ? 1 : 0;
    engineConfig_["fshadow 1"] = config_.textRightDown ? 1 : 0;
    // First-launch engine defaults: every sound group and character voice on, full volume.
    for (int g = 0; g < 3; ++g) engineConfig_["play " + std::to_string(g)] = 1;
    for (int c = 0; c < 32; ++c) engineConfig_["play 3 " + std::to_string(c)] = 1;
    engineConfig_["sysplay 0"] = 1;
    for (int c = 1; c < 8; ++c) engineConfig_["sysplay " + std::to_string(c)] = 0;
    for (const char* k : {"vol_master", "vol_bgm", "vol_se", "vol_voice", "vol_play"}) engineConfig_[k] = 100;
    // Reading settings: the engine's defaults (grisaia.exe 0x5aa7a0: messpd 4, autospd 4 with its
    // autopriset pause, waitpcm 1, contauto 0). With no autopriset the pause is the 50 / 50 the
    // Grisaia flow scripts write on first run.
    engineConfig_["messpd"] = 4;
    engineConfig_["autospd"] = 4;
    engineConfig_["autospd_b"] = autoPresets_.size() > 4 ? autoPresets_[4].first : 50;
    engineConfig_["autospd_c"] = autoPresets_.size() > 4 ? autoPresets_[4].second : 50;
    engineConfig_["waitpcm"] = 1;
    engineConfig_["skip_speed"] = 0;
    engineConfig_["contskip"] = 0;
    engineConfig_["contauto"] = 0;
    // The rest of the config block as the engine makes it (Nekopara cs2.exe 0x71d060, Amakano's
    // 0x6b4a80 the same; grisaia.exe 0x5ab0d0 for the fields it has), before startup.xml's MESSAGE /
    // EFFECT / GAME keys (0x720600: EFFECT/skipdraw is effcut, GAME/enable_scene_anm
    // enable_sceneanm, ...; no game sets these): effects are not cut, scene animations are on
    // (Nekopara's scene-animation setting starts on). Read text is coloured as startup.xml's
    // GAME/read/enable says (0x71ec40 reads it into this setting, which starts at 0; the saved
    // config's MESSAGE/enable_read comes after, 0x720600): on in Cyanotype, Grisaia, Hatsumira,
    // Himawari and Yukikoi, off in Amakano, LSG and Nekopara (Fruit's flow turns it on as well).
    engineConfig_["enable_readcol"] = readEnabled_ ? 1 : 0;
    engineConfig_["effcut"] = 0;
    engineConfig_["enable_sceneanm"] = 1;
    engineConfig_["stillread"] = 1;
    engineConfig_["fullskip"] = 0;
    engineConfig_["autocur"] = 1;
    engineConfig_["autodel"] = 1;
    engineConfig_["dorama"] = 0;
    engineConfig_["motcut"] = 0;
    engineConfig_["activewindow"] = 1;
    // autoleft (Nekopara 0x71d1e3, 0 like ctrlmode beside it): a left click during auto stops auto
    // (see GameSession::storyAction). Amakano 2's flow turns it on, Nekopara's config offers it.
    engineConfig_["autoleft"] = 0;
    // contvoice (0x71d143, the sound block's 0x119a0): voices play on past their message. Yukikoi's
    // flow turns it off on first run (its Voice Skip setting), Amakano 2's config offers it.
    engineConfig_["contvoice"] = 1;
    if (!languages_.empty()) {
        engineConfig_["lang_ui"] = defaultLanguage_;
        engineConfig_["lang_text_main"] = defaultLanguage_;
        // No sub language (-1, shown as OFF): the config defaults set ui 0, main 0, sub -1
        // (Nekopara 0x71d031..0x71d045, the same three stores in Cyanotype, LSG and Yukikoi); the
        // startup parser moves only ui and main to the language matching Windows' UI language
        // (0x71d420), and loading keeps -1 (0x71fc40). Measured on PC: the first language screen
        // shows Sub Language OFF.
        engineConfig_["lang_text_sub"] = -1;
    }
    applyLanguage();
    placeWindowLayouts();
    window_ = std::make_unique<WindowScript>(assets_, vars_, theme_, engineConfig_, this);
    window_->log = [this](const std::string& m) {
        if (player_.log) player_.log(m);
    };
    // Engine variable assignments (adv.xml): boot mode flag and first-scene string.
    Bytes adv;
    if (auto file = folder_->open("config/adv.xml")) adv = file->readAll();
    else if (assets_.find("adv.xml")) adv = assets_.read("adv.xml");
    if (!adv.empty()) {
        auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(adv.data()), adv.size())));
        if (const XmlNode* doc = root->child("document")) {
            bootFlag_ = doc->intValue("sysflag/boot", bootFlag_);
            bootString_ = doc->intValue("sysstr/boot", bootString_);
            player_.setKaisouBase(doc->intValue("sysflag/kaisou", 900));
            chvolFlag_ = doc->intValue("sysflag/chvol", chvolFlag_);
            movieVolFlag_ = doc->intValue("sysflag/movie_vol", movieVolFlag_);
            movieCancelFlag_ = doc->intValue("sysflag/movie_cancel", movieCancelFlag_);
            movieStr_ = doc->intValue("sysstr/movie", movieStr_);
            slModeFlag_ = doc->intValue("sysflag/slmode", slModeFlag_);
            slTypeFlag_ = doc->intValue("sysflag/sltype", slTypeFlag_);
            retTitleFlag_ = doc->intValue("sysflag/rettitle", retTitleFlag_);
            savePageNowFlag_ = doc->intValue("sysflag/savepage_now", savePageNowFlag_);
            savePageMaxFlag_ = doc->intValue("sysflag/savepage_max", savePageMaxFlag_);
            window_->setButtonMode(doc->intValue("etc/mesbtn/btnmode", 0) != 0);
            // etc/automag: pressing Auto while auto runs speeds it up (see stepAutoLevel).
            autoMag_ = doc->intValue("etc/automag", 0) != 0;
            // etc/erase: the menus' black fade (see openMenu); main.kcs reads both, 1 when absent.
            eraseConfig_ = doc->intValue("etc/erase/config", 1) != 0;
            eraseSave_ = doc->intValue("etc/erase/save", 1) != 0;
            // etc/meswnd_restore: the window back before the menus' fade back (see menuClosed);
            // main.kcs reads config and save with 1 as the default (log with 0), Amakano 1
            // m[0x80005840] +0 / +4 / +8.
            restoreConfig_ = doc->intValue("etc/meswnd_restore/config", 1) != 0;
            restoreSave_ = doc->intValue("etc/meswnd_restore/save", 1) != 0;
            logRetFlag_ = doc->intValue("sysflag/logret_pathindex", logRetFlag_);
            saveNewQuickFlag_ = doc->intValue("sysflag/savenew_quick", saveNewQuickFlag_);
            fsstrBase_ = doc->intValue("fsstr", fsstrBase_);
            autoStart_ = doc->intValue("save/autosave/start", autoStart_);
            autoCount_ = doc->intValue("save/autosave/count", autoCount_);
            quickStart_ = doc->intValue("save/quicksave/start", quickStart_);
            quickCount_ = doc->intValue("save/quicksave/count", quickCount_);
            thumbW_ = doc->intValue("save/thumbnail/width", thumbW_);
            thumbH_ = doc->intValue("save/thumbnail/height", thumbH_);
            saveStrLen_ = doc->intValue("save/strlen", saveStrLen_);
            // regident/reg00-03: the resident scripts (main.kcs 0x005b6c; a slot with no script is
            // off). sscript starts them as it starts (startResidents).
            residentDefs_.clear();
            for (int i = 0; i < 4; ++i) {
                char key[48];
                std::snprintf(key, sizeof key, "regident/reg%02d/script", i);
                ResidentDef d;
                d.script = doc->value(key).value_or("");
                std::snprintf(key, sizeof key, "regident/reg%02d/var/prmcnt", i);
                d.prmcnt = doc->intValue(key, 0);
                std::snprintf(key, sizeof key, "regident/reg%02d/var/prmstr", i);
                d.prmstr = doc->intValue(key, 0);
                residentDefs_.push_back(std::move(d));
            }
            // etc/cgreg/r00, r01, ...: main.kcs (Nekopara 0x004e30) keeps the first character of
            // each in system memory (0x800056d8), up to the first missing; a shown image whose name
            // starts with one of them is recorded for the gallery (sscript 0x095511). Amakano 2, LSG
            // and Nekopara have none: their pictures are recorded by `cgreg` / `cgflag` only.
            {
                std::string letters;
                for (int i = 0; i < 128; ++i) {
                    char key[32];
                    std::snprintf(key, sizeof key, "etc/cgreg/r%02d", i);
                    auto v = doc->value(key);
                    if (!v || v->empty()) break;
                    letters += (*v)[0];
                }
                player_.setCgRegLetters(std::move(letters));
            }
            dictionaryOn_ = doc->intValue("etc/dictionary/enable", 0) != 0;
            dictionaryFile_ = doc->value("etc/dictionary/file").value_or("");
            dictionaryVar_ = doc->intValue("etc/dictionary/var", -1);
            dictionaryFlag_ = doc->intValue("etc/dictionary/apend_flag", -1);
            dictionary_.setJoiners(doc->value("etc/dictionary/first").value_or(""), doc->value("etc/dictionary/between").value_or(""));
        }
    }
    loadDictionary();
}

void Game::loadEngineVersion() {
    // The engine is the game's main executable (sometimes shipped as .bin behind a small launcher):
    // the largest top-level program with a version resource. Launchers, boot menus and uninstallers
    // are far smaller.
    uint64_t best = 0;
    for (const auto& n : folder_->list()) {
        std::string l = toLowerAscii(n);
        if (!l.ends_with(".exe") && !l.ends_with(".bin")) continue;
        auto f = folder_->open(n);
        if (!f || f->size() <= best) continue;
        if (uint64_t v = peFileVersion(*f)) {
            best = f->size();
            engineVersion_ = v;
        }
    }
}

void Game::placeWindowLayouts() {
    // The engine draws the message window where its script puts the window plane. sscript tells a
    // new window where it goes with MES_POS (0x0038ec, the position saved from SSCRIPT_MOVE_WINDOW
    // 0x700, which the scripts send after `wndmenu MENUID_RESETMESPOS` and drags) before the script
    // starts, else with MES_SHIFT (0x0052de; mode 0 on an ordinary screen). Some scripts take the
    // position (`pl pos \0 \1`: Nekopara, Fruit, Cyanotype, Amakano, LSG), others put the plane at
    // their own PX, PY (Hatsumira, Eden, Yukikoi, Labyrinth) and Himawari's START then moves it onto
    // its 960x544 screen (its novel window moves its own plane in START, which a MES_SHIFT after START would
    // undo). The theme's origin is the window image laid at the screen's bottom; each style's
    // script runs here once on copies of the variables and the config as the window runner starts it
    // (MES_POS with that origin, then START), and the layout moves to where the plane ends up:
    // Hatsumira and Yukikoi 1 lower, Eden 1 right and 2 lower.
    for (auto& [style, L] : theme_.styles) {
        if (L.script.empty() || L.windowPlane.empty()) continue;
        VarStore vars = vars_;
        FesInstance::Config config = engineConfig_;
        for (int i = 0; i < 3; ++i) vars.setStr(900 + i, L.images[i]);
        auto fes = FesInstance::load(assets_, vars, L.script, 0, &config, nullptr);
        if (!fes) continue;
        if (fes->hasLabel("MES_POS")) fes->invoke("MES_POS", {L.originX, L.originY});
        else fes->setPosition(L.windowPlane, L.originX, L.originY);
        uint64_t frame = 0;
        while (!fes->startedUp() && frame < 600) fes->tick(++frame);
        int32_t x = 0, y = 0;
        if (!fes->startedUp() || !fes->objectPosition(L.windowPlane, frame, x, y)) continue;
        const int32_t dx = x - L.originX, dy = y - L.originY;
        if (dx == 0 && dy == 0) continue;
        L.originX += dx, L.originY += dy;
        for (IRect* r : {&L.text, &L.name, &L.faceClip}) r->x += dx, r->y += dy;
        L.faceX += dx, L.faceY += dy;
    }
}

void Game::loadUserFont() {
    Bytes xml;
    if (auto file = folder_->open("config/hist.xml")) xml = file->readAll();
    else if (assets_.find("hist.xml")) xml = assets_.read("hist.xml");
    std::string image = "userfont";
    if (!xml.empty()) {
        auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size())));
        if (const XmlNode* doc = root->child("document")) image = doc->value("IMAGE/ufont").value_or(image);
    }
    if (image.empty() || !assets_.find(image + ".hg3")) return;
    userFontImage_ = image;
    // The engine's default table, which the games' scripts also declare (`userfont 0x8740 1` ...).
    for (int k = 0; k < 9; ++k) {
        char sjis[2] = {static_cast<char>(0x87), static_cast<char>(0x40 + k)};
        std::string u = sjisToUtf8(std::string_view(sjis, 2));
        if (u.size() != 3) continue;  // a three-byte UTF-8 sequence (U+2460..)
        auto c = [&](size_t i) { return static_cast<uint32_t>(static_cast<uint8_t>(u[i])); };
        userFontGlyphs_[((c(0) & 0x0F) << 12) | ((c(1) & 0x3F) << 6) | (c(2) & 0x3F)] = k + 1;
    }
}

void Game::loadConfig() {
    Bytes xml;
    if (auto file = folder_->open("config/startup.xml")) {
        xml = file->readAll();
    } else if (assets_.find("startup.xml")) {
        xml = assets_.read("startup.xml");
    } else {
        return;
    }
    std::string text = decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
    auto root = parseXml(text);
    const XmlNode* doc = root->child("document");
    if (!doc) return;
    rawTitle_ = config_.title = doc->value("APP/title").value_or("");
    // <CURSOR> (Nekopara cs2.exe 0x71d67f..0x71d6ce: type, default, hx, hy, mode; grisaia.exe
    // 0x5ab6a8 reads no mode). The mode is the config's cursor_mode (1 at start, 0x71ce74).
    cursorType_ = doc->intValue("CURSOR/type", 0);
    cursorMode_ = doc->intValue("CURSOR/mode", 1);
    cursorPicture_ = doc->value("CURSOR/default").value_or("");
    cursorHx_ = doc->intValue("CURSOR/hx", 0);
    cursorHy_ = doc->intValue("CURSOR/hy", 0);
    // cursor.xml (config, 0x532c20): each entry a cursor file in the game folder, loaded with
    // LoadCursorFromFileW.
    Bytes cursorXml;
    if (auto file = folder_->open("config/cursor.xml")) cursorXml = file->readAll();
    else if (assets_.find("cursor.xml")) cursorXml = assets_.read("cursor.xml");
    if (!cursorXml.empty()) {
        auto croot = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(cursorXml.data()), cursorXml.size())));
        if (const XmlNode* cdoc = croot->child("document")) {
            for (const auto& c : cdoc->children) {
                if (!c->text.empty()) cursorFiles_[c->name] = c->text;
            }
        }
    }
    // <language>: L00, L01, ... with the asset prefix of each; <default> picks one.
    if (const XmlNode* lang = doc->child("language")) {
        for (const auto& l : lang->children) {
            if (auto prefix = l->value("prefix")) languages_.push_back({*prefix, l->value("lang").value_or(*prefix)});
        }
        defaultLanguage_ = std::clamp(lang->intValue("default", 0), 0, std::max(0, static_cast<int32_t>(languages_.size()) - 1));
        // <OPTTABLE>: the table of `#id#` strings (Nekopara's config.int/gamelangtbl_%LANG_M%.txt;
        // cs2.exe reads it at 0x71e856).
        optTable_ = lang->value("OPTTABLE").value_or("");
    }
    config_.width = doc->intValue("SCREEN/v_width", doc->intValue("SCREEN/width", config_.width));
    config_.height = doc->intValue("SCREEN/v_height", doc->intValue("SCREEN/height", config_.height));
    loadLayout();
    keyMap_ = KeyMap::parse(doc);
    if (const XmlNode* faces = doc->find("FONT/face")) {
        for (const auto& f : faces->children) {
            if (!f->text.empty()) fontFaces_.push_back(f->text);
        }
    }
    readEnabled_ = doc->intValue("GAME/read/enable", 0) != 0;
    fontProportional_ = doc->intValue("FONT/proportional", 1) != 0;  // 0x62c3e0, default 1
    // FONT/bold (Nekopara cs2.exe 0x7202f0, startup settings +0xfd18, 0 unless given): the default
    // of getconf / setconf `fontweight` (the running copy +0xf900; see textWeight).
    fontBold_ = doc->intValue("FONT/bold", 0) != 0;
    config_.textAround = doc->intValue("FONT/shadow/around", 1) != 0;
    config_.textRightDown = doc->intValue("FONT/shadow/rightdown", 1) != 0;
    if (auto c = doc->value("GAME/read/color")) {
        // Windows colour order, 0xAABBGGRR (0xffffe2b0 is light blue): to our ARGB.
        auto v = static_cast<uint32_t>(std::strtoul(c->c_str(), nullptr, 0));
        readColor_ = (v & 0xFF00FF00) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
    }
    player_.setLogLimit(static_cast<size_t>(std::max(1, doc->intValue("GAME/log/col", 1000))));
    logNameFirst_ = doc->value("GAME/log/nameformat/first").value_or("");
    logNameLast_ = doc->value("GAME/log/nameformat/last").value_or("");
    strFrameTenths_ = doc->intValue("GAME/strframe_frame", strFrameTenths_);
    strFrameCount_ = doc->intValue("GAME/strframe_count", strFrameCount_);
    if (const XmlNode* ap = doc->find("GAME/autopriset")) {
        for (const auto& p : ap->children) autoPresets_.emplace_back(p->intValue("base", 16), p->intValue("char", 9));
    }
    if (const XmlNode* g = doc->find("GAME/flag/global0")) {
        globalFrom_ = g->intValue("start", globalFrom_);
        globalTo_ = globalFrom_ + g->intValue("count", globalTo_ - globalFrom_);
    }
    // <VOICE>: v00.. in order; entries of the voice group are numbered as characters.
    if (const XmlNode* voice = doc->child("VOICE")) {
        int32_t character = 0;
        for (const auto& v : voice->children) {
            VoiceEntry e;
            e.head = toLowerAscii(v->value("head").value_or(""));
            e.archive = toLowerAscii(v->value("int").value_or(""));
            e.group = v->intValue("group", 0);
            e.mastervol = v->intValue("mastervol", 100);
            if (e.group == 2) e.character = character++;
            voices_.push_back(std::move(e));
        }
    }
    if (const XmlNode* c = doc->find("GAME/wndcolor")) {
        auto ch = [&](const char* n, int d) { return static_cast<uint32_t>(c->intValue(n, d) & 0xFF); };
        config_.windowColor = (ch("a", 204) << 24) | (ch("r", 0) << 16) | (ch("g", 12) << 8) | ch("b", 16);
    }
    config_.windowAlphaByte = doc->intValue("CONFIG_B/wnd_alpha", 1) != 0;
}

void Game::loadLayout() {
    Bytes xml;
    if (auto file = folder_->open("config/vram.xml")) xml = file->readAll();
    else if (assets_.find("vram.xml")) xml = assets_.read("vram.xml");
    else return;
    auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size())));
    const XmlNode* doc = root->child("document");
    if (!doc) return;
    ScenePlayer::Layout layout;
    layout.screenWidth = config_.width;
    layout.screenHeight = config_.height;
    layout.planeBaseX = doc->intValue("plane/pl00/base/x", config_.width / 2);
    layout.planeBaseY = doc->intValue("plane/pl00/base/y", config_.height / 2);
    layout.planePosX = doc->intValue("plane/pl00/pos/x", layout.planeBaseX);
    layout.planePosY = doc->intValue("plane/pl00/pos/y", layout.planeBaseY);
    if (const XmlNode* planes = doc->child("plane")) {
        for (const auto& node : planes->children) {
            if (node->name.size() <= 2 || !node->name.starts_with("pl")) continue;
            int32_t bx = node->intValue("base/x", layout.planeBaseX), by = node->intValue("base/y", layout.planeBaseY);
            layout.planes[std::atoi(node->name.c_str() + 2)] = {bx, by, node->intValue("pos/x", bx),
                                                                 node->intValue("pos/y", by), node->intValue("width", 0),
                                                                 node->intValue("height", 0), node->intValue("x", 0),
                                                                 node->intValue("y", 0), node->intValue("disp", 1) != 0};
        }
    }
    if (const XmlNode* images = doc->child("image")) {
        static const std::pair<const char*, Layer> kLayers[] = {
            {"bg", Layer::Bg}, {"cg", Layer::Cg}, {"eg", Layer::Eg}, {"fg", Layer::Fg}, {"fw", Layer::Fw}};
        for (const auto& node : images->children) {
            for (const auto& [prefix, layer] : kLayers) {
                if (node->name.size() > 2 && node->name.starts_with(prefix)) {
                    int32_t index = std::atoi(node->name.c_str() + 2);
                    layout.imagePlane[{layer, index}] = node->intValue("pl", 0);
                    layout.imageShift[{layer, index}] = {node->intValue("shift_x", 0), node->intValue("shift_y", 0)};
                    layout.imagePos[{layer, index}] = {node->intValue("pos/x", 0), node->intValue("pos/y", 0)};
                }
            }
        }
    }
    // E-mote banks (`<mpl><mplNN><pos>`): each bank is a layer of its own placed at this position
    // (sscript 0x0c32f5; the renderer draws it among the planes, not inside one).
    if (const XmlNode* mpl = doc->child("mpl")) {
        for (const auto& node : mpl->children) {
            if (node->name.size() <= 3 || !node->name.starts_with("mpl")) continue;
            int32_t index = std::atoi(node->name.c_str() + 3);
            layout.imagePos[{Layer::Mpl, index}] = {node->intValue("pos/x", 0), node->intValue("pos/y", 0)};
        }
    }
    loadEmoDef(layout.emo);
    player_.setLayout(std::move(layout));
}

void Game::loadEmoDef(ScenePlayer::Layout::EmoDef& emo) {
    // config.int/emodef.xml, named in cs2.exe: size/texscale and size/sNN (id, scale), ch/cNN (id)
    // with pNN (id, table: an emo_*.xml of emNN id / offset), emocon/emNN (name, pos, scale, tex).
    auto read = [&](const std::string& name) -> std::unique_ptr<XmlNode> {
        if (!assets_.find(name)) return nullptr;
        Bytes b = assets_.read(name);
        return parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(b.data()), b.size())));
    };
    auto root = read("emodef.xml");
    const XmlNode* doc = root ? root->child("document") : nullptr;
    if (!doc) return;
    if (const XmlNode* size = doc->child("size")) {
        emo.texScale = size->intValue("texscale", 100);
        for (const auto& n : size->children) {
            if (n->name.size() > 1 && n->name[0] == 's' && std::isdigit(static_cast<unsigned char>(n->name[1])))
                emo.sizes[n->intValue("id", 0)] = n->intValue("scale", 100);
        }
    }
    if (const XmlNode* con = doc->child("emocon")) {
        for (const auto& n : con->children) {
            if (!n->name.starts_with("em")) continue;
            ScenePlayer::Layout::EmoMark m;
            m.name = n->value("name").value_or("");
            m.x = n->intValue("pos/x", 0);
            m.y = n->intValue("pos/y", 0);
            m.scale = n->intValue("scale", 100);
            m.half = n->intValue("tex/half", 0) != 0;
            m.large = n->value("tex/l").value_or("");
            m.small = n->value("tex/s").value_or("");
            emo.marks.push_back(std::move(m));
        }
    }
    if (const XmlNode* ch = doc->child("ch")) {
        for (const auto& c : ch->children) {
            const int32_t cid = c->intValue("id", 0);
            for (const auto& pose : c->children) {
                if (pose->name.size() < 2 || pose->name[0] != 'p') continue;
                const int32_t pid = pose->intValue("id", 0);
                auto& table = emo.offsets[{cid, pid}];
                auto t = read(pose->value("table").value_or(""));
                const XmlNode* tdoc = t ? t->child("document") : nullptr;
                if (!tdoc) continue;
                for (const auto& e : tdoc->children) {
                    if (e->name.starts_with("em")) table[e->intValue("id", 0)] = {e->intValue("offset/x", 0), e->intValue("offset/y", 0)};
                }
            }
        }
    }
}

const Game::VoiceEntry* Game::voiceFor(const std::string& asset) const {
    auto ref = assets_.find(asset + ".ogg");
    if (!ref) return nullptr;
    std::string archive = toLowerAscii(ref->archive->name());
    if (archive.ends_with(".int")) archive.resize(archive.size() - 4);
    for (const auto& v : voices_) {
        if (v.archive == archive) return &v;
    }
    return nullptr;
}

float Game::volumeFor(int group, const VoiceEntry* v) const {
    auto conf = [&](const std::string& k, int32_t def) {
        auto it = engineConfig_.find(k);
        return it == engineConfig_.end() ? def : it->second;
    };
    float vol = (v ? v->mastervol : 100) / 100.0f;
    // `voiceoff`: a voice whose <VOICE> entry the scene muted does not play (REBELLIONS 0x08f5db).
    if (group == 2 && v && player_.voiceMuted(static_cast<size_t>(v - voices_.data()))) return 0;
    if (soundConf_ && group == 2 && v && v->character >= 0) {
        // soundconf.xml: the character's part under voice (id 2): play_id / volume_id 2 <c>.
        std::string key;
        if (const SoundPart* p = soundPart({}, {2, v->character}, key); p && key.find(' ') != std::string::npos) {
            if (!soundValue(*p, key, "play") || soundValue(*p, key, "mute")) return 0;
            vol *= std::clamp(soundValue(*p, key, "volume"), 0, 100) / 100.0f;
        }
        return vol;
    }
    if (group == 2 && v && v->character >= 0) {
        if (!conf("play 3 " + std::to_string(v->character), 1)) return 0;
        vol *= std::clamp(vars_.get(chvolFlag_ + v->character), 0, 100) / 100.0f;
    }
    return vol;
}

void Game::applyBusVolumes() {
    if (!mixer_) return;
    auto conf = [&](const std::string& k, int32_t def) {
        auto it = engineConfig_.find(k);
        return it == engineConfig_.end() ? def : it->second;
    };
    using Bus = audio::Mixer::Bus;
    if (soundConf_) {
        // soundconf.xml: MASTER over the parts bgm (0), se (1), voice (2) and movie (3), each
        // silent when its play is off or mute on.
        auto level = [&](const std::string& name, int32_t id) {
            std::string key;
            const SoundPart* p = name == "master" ? soundPart("master", {}, key) : soundPart({}, {id}, key);
            if (!p || !soundValue(*p, key, "play") || soundValue(*p, key, "mute")) return p ? 0.0f : 1.0f;
            return std::clamp(soundValue(*p, key, "volume"), 0, 100) / 100.0f;
        };
        const float m = level("master", 0);
        // BGM, SE and voices: each sound's part is its gain (updateSoundGains), so the buses carry
        // MASTER alone.
        mixer_->setBusVolume(Bus::Bgm, m);
        mixer_->setBusVolume(Bus::Se, m);
        mixer_->setBusVolume(Bus::Voice, m);
        mixer_->setBusVolume(Bus::Movie, m * level({}, 3));
        updateSoundGains();
        return;
    }
    // Newer config screens have a master volume over the groups (`vol_master`).
    const float master = std::clamp(conf("vol_master", 100), 0, 100) / 100.0f;
    mixer_->setBusVolume(Bus::Bgm, conf("play 0", 1) ? master * std::clamp(conf("vol_bgm", 100), 0, 100) / 100.0f : 0);
    mixer_->setBusVolume(Bus::Se, conf("play 1", 1) ? master * std::clamp(conf("vol_se", 100), 0, 100) / 100.0f : 0);
    mixer_->setBusVolume(Bus::Voice, conf("play 2", 1) ? master * std::clamp(conf("vol_voice", 100), 0, 100) / 100.0f : 0);
    // Movies: `vol_play`, the config screens' movie volume.
    mixer_->setBusVolume(Bus::Movie, master * std::clamp(conf("vol_play", 100), 0, 100) / 100.0f);
}

bool Game::cgEntrySeen(const std::string& entry) { return player_.cgEntrySeen(entry); }

bool Game::startMovie(const std::string& name, int32_t skipMode, int32_t volume) {
    // The scripts name a movie without its extension; the engine plays the kinds its DirectShow
    // setup knows (cs2.exe 0x6fcc40, by extension: mpg, mpa, mpv, dat, avi, mov, wav, wmv, webm,
    // ogv, hap), so take the first of those movie.int has. MPEG-1 decodes with pl_mpeg, the rest
    // with FFmpeg (Nekopara's WMV), both read from the archive as it plays. One that can't be read
    // is skipped.
    static constexpr const char* kKinds[] = {"mpg", "mpa", "mpv", "dat", "avi", "mov", "wav", "wmv", "webm", "ogv", "hap"};
    std::optional<AssetRef> ref;
    std::string ext;
    if (size_t dot = name.rfind('.'); dot != std::string::npos && dot + 1 < name.size()) {
        ref = assets_.find(name);
        ext = toLowerAscii(name.substr(dot + 1));
    }
    for (const char* k : kKinds) {
        if (ref) break;
        if ((ref = assets_.find(name + "." + k))) ext = k;
    }
    movie_ = Movie{};
    if (ref && ext == "mpg") movie_.mpeg = media::MpegVideo::open(ref->archive->openEntry(*ref->entry));
    else if (ref) movie_.ffmpeg = media::FfmpegVideo::open(ref->archive->openEntry(*ref->entry));
    if (!movie_.open()) {
        std::fprintf(stderr, "movie %s: %s\n", name.c_str(), ref ? "cannot be decoded" : "not found");
        return false;
    }
    movie_.skipMode = skipMode;
    movie_.seek(0);
    const bool audio = movie_.mpeg ? movie_.mpeg->hasAudio() : movie_.ffmpeg->hasAudio();
    if (mixer_ && audio) {
        const int rate = movie_.mpeg ? movie_.mpeg->sampleRate() : movie_.ffmpeg->sampleRate();
        auto source = movie_.mpeg ? movie_.mpeg->audioSource(false) : movie_.ffmpeg->audioSource(false);
        mixer_->playStream(audio::Mixer::Bus::Movie, 0, rate, std::move(source), volume / 100.0f);
    }
    return true;
}

void Game::endMovie() {
    if (!movie_.open()) return;
    movie_ = Movie{};
    if (mixer_) mixer_->stop(audio::Mixer::Bus::Movie, 0);
    if (std::exchange(kcsMovie_, false)) kcsExit_ = 0;  // movie.kcs ends with its movie
    else player_.movieFinished();
}

Game::~Game() {
    saveGlobal();
}

void Game::applyScriptDefaults() {
    // A first launch starts from the game's own settings defaults. Older games set them from the flow
    // script (SET_DEFAULTDATA, which runs anyway); newer ones keep them only in the settings screen's
    // reset (Love's Sweet Garnish's config.fes RESETCONF: master 100, BGM 80, SE 70, voices 60, ...,
    // what the original shows on a fresh install). Run that reset once, off screen, to seed them.
    for (const auto& archive : assets_.archives()) {
        for (const auto& e : archive->entries()) {
            std::string name = AssetIndex::normalizeName(e.name);
            if (!name.ends_with(".fes")) continue;
            std::string script = name.substr(0, name.size() - 4);
            auto inst = FesInstance::load(assets_, vars_, script, 0, &engineConfig_, this);
            if (!inst || !inst->hasLabel("RESETCONF")) continue;
            inst->invoke("RESETCONF");
            if (player_.log) player_.log("config defaults from " + script + ".fes RESETCONF");
            return;
        }
    }
}

void Game::setSaveDirectory(const std::string& dir) {
    saves_ = SaveStore(dir);
    loadedGlobal_ = saves_.loadGlobal();
    if (loadedGlobal_) {
        for (const auto& [k, v] : loadedGlobal_->config) engineConfig_[k] = v;
        if (auto f = loadedGlobal_->configText.find("font"); f != loadedGlobal_->configText.end()) setConfigFont(f->second);
        read_ = loadedGlobal_->read;
        player_.cgSeen() = loadedGlobal_->cgSeen;
        std::map<std::string, Dictionary::State> dic;
        for (const auto& [k, d] : loadedGlobal_->dictionary) dic[k] = {d.appear, d.updated, d.fresh};
        dictionary_.restore(dic);
        persisted_ = *loadedGlobal_;
    } else {
        applyScriptDefaults();
    }
    applyLanguage();
}

void Game::setLanguage(int32_t index) {
    if (languages_.empty()) return;
    index = std::clamp(index, 0, static_cast<int32_t>(languages_.size()) - 1);
    engineConfig_["lang_ui"] = index;
    engineConfig_["lang_text_main"] = index;
    applyLanguage();
}

void Game::applyLanguage() {
    if (languages_.empty()) return;
    auto pick = [&](const char* key) -> const Language& {
        int32_t i = conf(key, defaultLanguage_);
        if (i < 0 || i >= static_cast<int32_t>(languages_.size())) i = defaultLanguage_;
        return languages_[static_cast<size_t>(i)];
    };
    const std::string& ui = pick("lang_ui").prefix;
    const std::string& main = pick("lang_text_main").prefix;
    // The sub language (`lang_text_sub`, -1 none): shown under the story text when it differs.
    const int32_t s = conf("lang_text_sub", -1);
    const std::string sub = s >= 0 && s < static_cast<int32_t>(languages_.size()) && languages_[static_cast<size_t>(s)].prefix != main
                                ? languages_[static_cast<size_t>(s)].prefix
                                : std::string();
    player_.setTextLanguage(main, sub);
    if (ui == appliedUi_ && main == appliedMain_) return;
    appliedUi_ = ui;
    appliedMain_ = main;
    assets_.setPlaceholder("LANG", ui);
    assets_.setPlaceholder("LANG_M", main);
    loadDictionary();
    // Window images can be per language (`sys_meswnd_%LANG%`): measure the layout in this one.
    theme_ = loadWindowTheme(assets_, config_.width, config_.height);

    // The title's `$8000-0001$` names a string of the language's table (language_en.txt: the id on
    // one line, its text on the next); without one, the text after the id is the original.
    config_.title = rawTitle_;
    if (rawTitle_.size() > 2 && rawTitle_[0] == '$') {
        size_t end = rawTitle_.find('$', 1);
        if (end != std::string::npos) {
            std::string id = rawTitle_.substr(0, end + 1);
            config_.title = rawTitle_.substr(end + 1);
            if (auto file = folder_->open("language_" + ui + ".txt")) {
                Bytes data = file->readAll();
                std::string text = decodeGameText(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
                size_t p = text.find(id + "\r\n");
                if (p == std::string::npos) p = text.find(id + "\n");
                if (p != std::string::npos) {
                    size_t start = text.find('\n', p) + 1;
                    size_t stop = text.find_first_of("\r\n", start);
                    std::string value = text.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
                    if (!value.empty() && value != "$empty$" && value != "$nr$") config_.title = value;
                }
            }
        }
    }
}

// The cursor as cs2.exe sets it (the window message 0x40e, Nekopara 0x5316a0; grisaia.exe
// 0x4420c0 the same): with cursor_mode not 1, Windows' arrow; type 0, the CURSOR/default picture
// the engine draws itself, hotspot hx, hy (0x676be0, 0x67feb0); type 1, the cursor.xml cursor
// `xchgcursor` picked (0x46d), Windows' arrow for a name cursor.xml lacks; any other type (Eden's
// 2), the arrow the window class has.
const CursorImage* Game::cursor() {
    if (cursorMode_ != 1 || (cursorType_ != 0 && cursorType_ != 1)) return nullptr;
    const std::string key = cursorType_ == 0 ? std::string() : cursorName_;
    auto it = cursorCache_.find(key);
    if (it == cursorCache_.end()) {
        std::optional<CursorImage> img;
        try {
            if (cursorType_ == 0) {
                // `image.int/cursor.hg3`: the archive part is where the engine looks; the name is enough here.
                std::string name = cursorPicture_;
                if (size_t s = name.find_last_of("/\\"); s != std::string::npos) name = name.substr(s + 1);
                if (!name.empty() && assets_.find(name)) {
                    HgImage hg = HgImage::parse(assets_.read(name));
                    if (!hg.frames().empty()) {
                        RgbaImage px = hg.decodeFull(0);
                        CursorImage c;
                        c.width = static_cast<int>(px.width), c.height = static_cast<int>(px.height);
                        c.argb.resize(static_cast<size_t>(c.width) * c.height);
                        for (size_t i = 0; i < c.argb.size() && i * 4 + 3 < px.pixels.size(); ++i) {
                            const uint8_t* p = &px.pixels[i * 4];
                            c.argb[i] = static_cast<uint32_t>(p[3]) << 24 | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
                        }
                        c.hotX = cursorHx_, c.hotY = cursorHy_;
                        img = std::move(c);
                    }
                }
            } else if (auto f = cursorFiles_.find(key); f != cursorFiles_.end()) {
                std::string path = f->second;
                for (char& ch : path) {
                    if (ch == '\\') ch = '/';
                }
                if (auto file = folder_->open(path)) img = decodeCursorFile(file->readAll());
            }
        } catch (const Error&) {
            img.reset();
        }
        if (!img && player_.log) player_.log("cursor '" + (cursorType_ == 0 ? cursorPicture_ : key) + "': none, Windows' arrow");
        it = cursorCache_.emplace(key, std::move(img)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void Game::changeCursor(const std::string& name) { cursorName_ = name.empty() ? "default" : name; }

void Game::resetVars() {
    vars_.clear();
    // main.kcs (Nekopara's 0x53e) sets the 26 flags from adv.xml sysflag/chvol to 100 (native 211, the
    // engine's flag array, cs2.exe 0x728980) when the save folder had no `config` at start (cs2.exe
    // 0x714d70 at boot; native 938 answers it); the global flags below then hold what was saved.
    // `config` is the saved sound configuration (SOUNDGROUP / PART), which cs2.exe only rewrites at
    // exit when it existed (0x7126e0 -> 0x719df0) and never creates: a game with a sound configuration
    // (soundconf.xml) has it from its first start on PC, made outside the engine (the launcher, which
    // carries the same reader and writer). Those games take their voice volumes from soundconf and use
    // these flags for themselves from 0: Nekopara's config.fes keeps its last tab in $673 (100 opened
    // it on no page), its meswnd counts $672 up from 1. Fitted: the launcher writing `config` is
    // inferred, not traced.
    if (!soundConf_) {
        for (int i = 0; i < 26; ++i) vars_.set(chvolFlag_ + i, 100);
    }
    // Global flags from earlier plays (first-run setup, unlocks, settings kept in flags).
    if (loadedGlobal_) {
        for (const auto& [i, v] : loadedGlobal_->flags) vars_.set(i, v);
        for (const auto& [i, v] : loadedGlobal_->strings) vars_.setStr(i, v);
    }
}

void Game::loadDictionary() {
    // The engine keeps the table open for the run, in the text language (`tips_%LANG_M%`), and
    // gives the scripts its handle in <var>; the state carries over a change of language.
    if (!dictionaryOn_ || dictionaryFile_.empty()) return;
    std::string name = dictionaryFile_;
    if (size_t p = name.find("%LANG_M%"); p != std::string::npos) name.replace(p, 8, appliedMain_);
    name += ".qdb";
    if (name == dictionaryLoaded_) return;
    // Before the first table, the state comes from the global data read at start.
    std::map<std::string, Dictionary::State> keep;
    if (dictionary_.loaded()) {
        keep = dictionary_.state();
    } else if (loadedGlobal_) {
        for (const auto& [k, d] : loadedGlobal_->dictionary) keep[k] = {d.appear, d.updated, d.fresh};
    }
    if (!assets_.find(name) || !dictionary_.load(assets_.read(name))) {
        if (player_.log) player_.log("dictionary not loaded: " + name);
        return;
    }
    dictionary_.restore(keep);
    dictionaryLoaded_ = name;
    player_.setDictionary(&dictionary_, dictionaryFlag_);
    if (dictionaryVar_ >= 0) vars_.set(dictionaryVar_, 1);
}

GlobalData Game::collectGlobal() const {
    GlobalData g;
    for (const auto& [i, v] : vars_.ints()) {
        if (i >= globalFrom_ && i < globalTo_) g.flags[i] = v;
    }
    g.config = engineConfig_;
    if (configFontSet_) g.configText["font"] = configFont_;
    g.read = read_;
    g.cgSeen = const_cast<ScenePlayer&>(player_).cgSeen();
    for (const auto& [k, d] : dictionary_.state()) g.dictionary[k] = {d.appear, d.updated, d.fresh};
    return g;
}

void Game::saveGlobal() {
    GlobalData g = collectGlobal();
    if (g.flags == persisted_.flags && g.config == persisted_.config && g.read == persisted_.read &&
        g.cgSeen == persisted_.cgSeen && g.dictionary == persisted_.dictionary) {
        return;
    }
    saves_.saveGlobal(g);
    persisted_ = std::move(g);
    loadedGlobal_ = persisted_;
}

void Game::setThumbnail(int32_t w, int32_t h, Bytes rgba) {
    thumbnailW_ = w;
    thumbnailH_ = h;
    thumbnail_ = std::move(rgba);
}

std::string Game::optText(const std::string& text) {
    // `#<id>#<text>`: the option table's entry for the id in the story language, else the text.
    if (text.size() < 2 || text[0] != '#') return text;
    size_t end = text.find('#', 1);
    if (end == std::string::npos) return text;
    const std::string id = text.substr(1, end - 1), fallback = text.substr(end + 1);
    if (optTable_.empty()) return fallback;
    std::string path = optTable_;
    if (size_t p = path.find("%LANG_M%"); p != std::string::npos) path.replace(p, 8, appliedMain_);
    if (size_t slash = path.find_last_of("/\\"); slash != std::string::npos) path = path.substr(slash + 1);
    auto it = optTables_.find(path);
    if (it == optTables_.end()) {
        // Lines: `#<id>#`, then the text, then a blank line; `//` comments.
        std::map<std::string, std::string> table;
        // `config.int/<name>`: the loose config folder first (Nekopara ships it there), as adv.xml.
        std::string raw;
        if (auto file = folder_->open("config/" + path)) {
            Bytes bytes = file->readAll();
            raw.assign(bytes.begin(), bytes.end());
        } else if (assets_.find(path)) {
            Bytes bytes = assets_.read(path);
            raw.assign(bytes.begin(), bytes.end());
        }
        if (!raw.empty()) {
            std::string key;
            std::istringstream in(raw);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
                if (line.size() >= 2 && line.front() == '#' && line.back() == '#' && line.find('#', 1) == line.size() - 1) {
                    key = line.substr(1, line.size() - 2);
                } else if (!key.empty() && !line.empty() && !line.starts_with("//")) {
                    table[key] = line;
                    key.clear();
                }
            }
        }
        it = optTables_.emplace(path, std::move(table)).first;
    }
    auto e = it->second.find(id);
    return e == it->second.end() || e->second == "$empty$" || e->second == "$nr$" ? fallback : e->second;
}

std::optional<SaveFile> Game::captureSave() {
    auto r = player_.capture();
    if (!r) return std::nullopt;
    SaveFile f;
    f.time = static_cast<int64_t>(std::time(nullptr));
    // The save's strings, as sscript writes them (Nekopara 0x0ad23a, Fruit 0x083976: native 244).
    // 0x200: the speaker (the part after a `＠`) and the message, `%s%s`, cut to adv.xml
    // save/strlen bytes (native 1045 / 23); a `[Option]` mark at a choice is not added.
    const auto& w = player_.stage().window;
    std::string name = w.name;
    if (size_t at = name.find("\xEF\xBC\xA0"); at != std::string::npos) name = name.substr(at + 3);
    std::string msg = name + w.text.plain();
    std::replace(msg.begin(), msg.end(), '\n', ' ');
    if (saveStrLen_ > 0) {
        // Bytes in the engine's encoding: Shift-JIS where the text has it, else UTF-8.
        std::string sjis = utf8ToSjis(msg);
        const bool inSjis = sjisToUtf8(sjis) == msg;
        const std::string& bytes = inSjis ? sjis : msg;
        size_t cut = 0;
        while (cut < bytes.size()) {
            const auto c = static_cast<unsigned char>(bytes[cut]);
            size_t n = inSjis ? ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC) ? 2 : 1)
                              : (c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1);
            if (cut + n > static_cast<size_t>(saveStrLen_)) break;
            cut += n;
        }
        if (cut < bytes.size()) msg = inSjis ? sjisToUtf8(bytes.substr(0, cut)) : bytes.substr(0, cut);
    }
    f.strings[0x200] = msg;
    // 0x201: the scene title (`scene`, Nekopara's `scenetitle`), through the text processing
    // (native 392), whose `#id#` strings come from the option table.
    if (!r->title.empty()) f.strings[0x201] = optText(r->title);
    f.thumbWidth = thumbnailW_;
    f.thumbHeight = thumbnailH_;
    f.thumbnail = thumbnail_;
    f.resume = std::move(*r);
    return f;
}

bool Game::dataSave(int32_t slot, int32_t object) {
    if (!pendingSave_ || object == 0) return false;
    SaveFile f = *pendingSave_;
    f.time = static_cast<int64_t>(std::time(nullptr));
    saves_.put(slot, f, player_.frame());
    saveGlobal();
    return true;
}

void Game::boot() {
    // The engine's own list (config.int/cglist.dat), for the scenes' `cgflag <group>`; the FE
    // galleries read the list their `cgreg_init` names (FesInstance).
    if (auto ref = assets_.find("cglist.dat")) {
        try {
            player_.setCgList(ScenePlayer::parseCgList(ref->archive->read(*ref->entry)));
        } catch (const Error&) {
        }
    }
    resetVars();
    // main.kcs's start, the same in every game (Amakano 2 0x0005c5 -> 0x001d6d, 0x0005d1): with the
    // global data read (native 250, flags 512-1023), savepage_now and savepage_max not above 0 become
    // 1 and 10; then rettitle is cleared. The flag is saved with the others (main.kcs writes them at
    // message 0xFFFF0006, native 240) and set again on a return to the title, so without the clear
    // every boot after a first game looks like a return: Amakano 2's flow script skips its logo and
    // caution screens when that flag is set.
    if (loadedGlobal_) {
        if (vars_.get(savePageNowFlag_) <= 0) vars_.set(savePageNowFlag_, 1);
        if (vars_.get(savePageMaxFlag_) <= 0) vars_.set(savePageMaxFlag_, 10);
    }
    vars_.set(retTitleFlag_, 0);
    startFlow();
    if (!flow_) startNewGame();
}

void Game::startFlow() {
    flow_ = FesInstance::load(assets_, vars_, "flow", player_.frame(), &engineConfig_, this);
    if (!flow_) return;
    flow_->log = [this](const std::string& m) {
        if (player_.log) player_.log(m);
    };
}

void Game::startNewGame() {
    resetVars();
    vars_.set(bootFlag_, -1);
    execKcs("sscript");
}

void Game::playScene(const std::string& scene) {
    flow_.reset();
    startNewGame();
    // A game started from the title has run flow.fes's #START first, which sets variables the scenes
    // read (Amakano 2's stand positions). Run that much, with no host so
    // that it opens no screen, then drop it.
    if (auto boot = FesInstance::load(assets_, vars_, "flow", player_.frame())) boot->tick(player_.frame());
    // Debugging: NYAN_BOOT=<mode> starts the scene through sscript's boot in that mode (-5: the
    // extras' viewer), as the menus would.
    if (const char* mode = std::getenv("NYAN_BOOT")) {
        vars_.set(bootFlag_, std::atoi(mode));
        vars_.setStr(bootString_, scene);
        execKcs("sscript");
        return;
    }
    player_.start(scene);
}

void Game::execKcs(const std::string& name) {
    // `execkcs <program>` runs one of the game's kcs programs. sscript is the story; movie.kcs
    // plays the movie a screen script names (Amakano's logo screen: the opening); call_config.kcs runs
    // the config screen (Nekopara's title). Programs not emulated (export, ImgExport, tweet: file
    // exports and web links) end at once; they must not start the story.
    const std::string program = toLowerAscii(name);
    if (program != "sscript") {
        kcsExit_ = 0;
        if (program == "movie") {
            // adv.xml: the file in sysstr/movie, its volume in sysflag/movie_vol, whether a
            // click may cancel it in sysflag/movie_cancel.
            int32_t volume = std::clamp(vars_.get(movieVolFlag_), 0, 100);
            int32_t skip = vars_.get(movieCancelFlag_) ? 2 : 0;
            if (startMovie(vars_.str(movieStr_), skip, volume)) kcsMovie_ = true, kcsExit_ = -1;
            else if (player_.log) player_.log("execkcs movie: cannot play '" + vars_.str(movieStr_) + "'");
        } else if (program == "call_config" && assets_.find("call_config.kcs")) {
            // call_config.kcs (Nekopara): creates an FES task (native 66), loads config.fes into it
            // (449: message 0x14e), starts it (447 / 645 / 448: 0x14b-0x14d, 0x154), waits while its
            // exit code (452: message 0x17) is -1, deletes it (67) and exits with that code (453).
            // The caller sets the save/load mode flag itself. Not done: the
            // engine message 0xFFFF0011, which it passes on as MES_CHANGECONFIG (630: 0x151).
            if (!systemScreen_) {
                configScreen_ = saveScreen_ = logScreen_ = false;
                systemScreen_ = FesInstance::load(assets_, vars_, "config", player_.frame(), &engineConfig_, this);
                if (systemScreen_) {
                    systemScreen_->log = [this](const std::string& m) {
                        if (player_.log) player_.log(m);
                    };
                    kcsScreen_ = true;
                    kcsExit_ = -1;
                } else if (player_.log) {
                    player_.log("execkcs call_config: config.fes not found");
                }
            }
        } else if (player_.log) {
            player_.log("execkcs " + name + ": not emulated");
        }
        return;
    }
    // sscript boot modes (adv.xml): -1 new game, -2 recollection (the extras' replay: the scene named
    // in the boot string, ended by `end_of_kaisou`), -10 scene select,
    // >=0 load slot. Nekopara's sscript (0x0e5d55; Amakano 0x0f96b7) also has -3 and -4, -1 and -2
    // that keep the base image (no native 455 on it; Nekopara's title starts a game with -3), and
    // -5, the extras' viewer (`prm_view_*`, m[0x2c614]), whose right click goes to `cancellabel`.
    int32_t mode = vars_.get(bootFlag_);
    replaying_ = mode == -2 || mode == -4;  // sscript 0x0f97b7 / 0x0f9903: m[0x2f98c]
    player_.setReplaying(replaying_);
    viewing_ = mode == -5;
    std::string scene = vars_.str(bootString_);
    if (scene.empty()) scene = "start";
    if (vars_.get(955) == 0) vars_.set(955, config_.height);  // sprite baseline: bottom of the screen
    sceneRunning_ = true;
    baseImage_.clear();  // see GameSession::renderFrame (`baseimg`)
    // The window reads the settings itself as it starts (its START: STRLAYOUT, WNDALPHA); only
    // later changes are announced (MES_CHANGECONFIG). Announcing the ones made before the story
    // (the title's settings, the flow's first-start `setconf`s) ran Fruit's and Hatsumira's
    // CHANGECONFIG, whose `str skip` finished the first message (sscript is not running then).
    notifiedConfig_ = engineConfig_;
    startResidents();
    kcsExit_ = -1;
    if (mode >= 0) {
        // Load a save slot: the story resumes where it was saved; global flags stay current.
        if (auto r = saves_.resume(mode, player_.frame())) {
            if (player_.log) player_.log("load slot " + std::to_string(mode) + ": " + r->scene);
            player_.load(*r, globalFrom_, globalTo_);
            return;
        }
        if (player_.log) player_.log("load slot " + std::to_string(mode) + ": no save; starting the story");
    } else if (mode != -1 && mode != -2 && mode != -3 && mode != -4 && mode != -5 && player_.log) {
        player_.log("sscript boot mode " + std::to_string(mode) + " not supported yet");
    }
    player_.start(scene);
}

void Game::startResidents() {
    // sscript's start (Amakano 2 0x0f9f28 -> 0x0cbb97): each resident script runs in an FE task of
    // its own (native 66, type 34, "regscript"; the script loaded and started, 449 / 447 / 448) for
    // as long as sscript runs. Amakano 2's info.fes shows the date and notices this way, Happiness 2's
    // line2.fes the phone messages.
    residents_.clear();
    residents_.resize(residentDefs_.size());
    for (size_t i = 0; i < residentDefs_.size(); ++i) {
        if (residentDefs_[i].script.empty()) continue;
        residents_[i] = FesInstance::load(assets_, vars_, residentDefs_[i].script, player_.frame(), &engineConfig_, this);
        if (!residents_[i]) continue;
        residents_[i]->log = [this](const std::string& m) {
            if (player_.log) player_.log(m);
        };
        residentSkip_ = 0;
        residentLoads_ = player_.loadCount();
        residentJumps_ = player_.jumpCount();
        residentScreen_ = false;
        if (player_.log) player_.log("resident script " + std::to_string(i) + ": " + residentDefs_[i].script);
        // Its start runs now, before sscript goes on to load a slot (fitted: the task starts at
        // sscript's start, ahead of the load): line2.fes's START clears the phone's flags
        // ($21-$23), which the load then brings back for RS_LOAD to rebuild the phone from.
        residents_[i]->tick(player_.frame());
    }
}

void Game::residentLabel(const std::string& label, const std::vector<int32_t>& args) {
    for (auto& r : residents_) {
        if (r && r->hasLabel(label)) r->invoke(label, args);
    }
}

void Game::runResidentCommands() {
    // `rs <n> <params...>` (Amakano 2 sscript 0x084851): n from 0 to 3, a running slot; its
    // parameters go into string variables prmstr, prmstr + 1, ... (native 276), their count into
    // flag prmcnt (native 211), then the script runs RS_COMMAND (native 630).
    for (auto& c : player_.takeResidentCalls()) {
        if (c.slot < 0 || c.slot >= static_cast<int32_t>(residents_.size()) || !residents_[static_cast<size_t>(c.slot)]) continue;
        const ResidentDef& d = residentDefs_[static_cast<size_t>(c.slot)];
        for (size_t k = 0; k < c.params.size(); ++k) {
            std::string v = c.params[k];
            if (c.utf8) {
                // Script strings are Shift-JIS; text it can't hold stays UTF-8 (as names do).
                std::string sjis = utf8ToSjis(v);
                if (sjisToUtf8(sjis) == v) v = std::move(sjis);
            }
            vars_.setStr(d.prmstr + static_cast<int32_t>(k), v);
        }
        vars_.set(d.prmcnt, static_cast<int32_t>(c.params.size()));
        FesInstance& r = *residents_[static_cast<size_t>(c.slot)];
        if (r.hasLabel("RS_COMMAND")) r.invoke("RS_COMMAND");
    }
}

void Game::endKcs() {
    if (kcsMovie_) endMovie();
    residents_.clear();  // sscript's tasks end with it
    if (std::exchange(kcsScreen_, false)) systemScreen_.reset();  // the program's task is deleted
    sceneRunning_ = false;  // an open system screen closes at the end of this tick (it may be the caller)
    baseImage_.clear();
    if (menuStep_ != MenuStep::None) {  // a menu's fade ends with the story
        menuStep_ = MenuStep::None;
        menuWindowBack_ = false;
        window_->setMenuHidden(false);
    }
    player_.stop();
}

void Game::retTitle() {
    // The story (if running) ends and flow.fes starts over from #START, which reads the flags the
    // menus set: load a slot, quit, or go to the title. Deferred: the caller is still running.
    // (No global save here: those request flags are only meant to live until #START reads them.)
    endKcs();
    kcsExit_ = 0;
    vars_.set(retTitleFlag_, 1);
    restartFlow_ = flow_ != nullptr;
    movie_ = Movie{};  // the story is left: a movie in it just stops
    if (mixer_) {
        mixer_->stop(audio::Mixer::Bus::Bgm, -1, 500);
        mixer_->stop(audio::Mixer::Bus::Se, -1, 0);
        mixer_->stop(audio::Mixer::Bus::Voice, -1, 0);
        mixer_->stop(audio::Mixer::Bus::Movie, -1, 0);
    }
}

void Game::openSystemScreen(const std::string& name) {
    if (systemScreen_) return;
    configScreen_ = saveScreen_ = false;
    logScreen_ = false;
    configScreen_ = name == "config";
    saveScreen_ = name == "saveload";
    vars_.set(slModeFlag_, 1);  // opened in game
    pendingSave_ = captureSave();  // what `datasave ... _SAVEOBJ_` writes
    systemScreen_ = FesInstance::load(assets_, vars_, name, player_.frame(), &engineConfig_, this);
    if (!systemScreen_) {
        if (player_.log) player_.log("system screen not found: " + name);
        return;
    }
    systemScreen_->log = [this](const std::string& m) {
        if (player_.log) player_.log(m);
    };
}

bool Game::openChoiceScreen() {
    const auto& choices = player_.stage().choices;
    if (systemScreen_ || choices.empty()) return false;
    const WindowLayout* L = theme_.find(player_.stage().window.style);
    if (!L || L->selectScript.empty()) L = theme_.find("default");
    if (!L || L->selectScript.empty()) return false;
    // The engine hands the script the choice texts ($str<fsstr>+i), its images ($str920..) and
    // the number of choices (\0); the script exits with the one picked.
    for (size_t i = 0; i < choices.size() && i < 100; ++i) {
        std::string sjis = utf8ToSjis(choices[i].prompt);
        vars_.setStr(fsstrBase_ + static_cast<int32_t>(i), sjisToUtf8(sjis) == choices[i].prompt ? sjis : choices[i].prompt);
    }
    for (int i = 0; i < 4; ++i) vars_.setStr(920 + i, L->selectImages[i]);
    // `fselect <screen>` names its own script (Labyrinth's Dave's Station menu, image buttons).
    std::string script = L->selectScript;
    const std::string& own = player_.stage().choiceScript;
    if (!own.empty() && assets_.find(own + ".fes")) script = own;
    systemScreen_ = FesInstance::load(assets_, vars_, script, player_.frame(), &engineConfig_, this);
    if (!systemScreen_) return false;
    systemScreen_->setLocal(0, static_cast<int32_t>(choices.size()));
    systemScreen_->log = [this](const std::string& m) {
        if (player_.log) player_.log(m);
    };
    choiceScreen_ = true;
    return true;
}

void Game::openSaveScreen(bool load) {
    vars_.set(slTypeFlag_, load ? 1 : 0);
    openSystemScreen("saveload");
}

namespace {
// The menus' fade (sscript: the fade object m[0x80000000], native 69 messages 1 colours, 2 frames,
// 3 running, 4 a colour at once): 16 frames out; back, two steps of black and then 30 frames.
constexpr uint32_t kMenuFadeOut = 16, kMenuHold = 2, kMenuFadeIn = 30;
}  // namespace

void Game::openMenu(const std::string& name) {
    if (systemScreen_ || menuStep_ != MenuStep::None) return;
    const bool save = name == "saveload";
    auto open = [&] {
        if (save) openSaveScreen(menuLoad_);
        else openSystemScreen(name);
    };
    if (!sceneRunning_) {
        open();
        return;
    }
    // Both ways the window goes first (Cyanotype's state 19 steps 1 and 5: m[0x84ba4] = whether it
    // is up, MES_SHOW with 0) and comes back at the end (step 14: MES_SHOW with 1 if it was).
    window_->setMenuHidden(true);
    menuWindowBack_ = false;
    menuScreen_ = name;
    menuFrame_ = 0;
    menuStep_ = MenuStep::Open;
    if (save ? eraseSave_ : eraseConfig_) {
        menuStep_ = MenuStep::Out;  // steps 1-2: the screen opens once the fade reports done
        return;
    }
    open();
    if (!systemScreen_) menuClosed(save, -1);
}

void Game::openSaveMenu(bool load) {
    menuLoad_ = load;
    openMenu("saveload");
}

void Game::stepMenu() {
    // Step 3, the frame after the screen opened, takes the black away (message 4 with 0). The
    // story stays off under the screen (menuHidesStory), so what shows is the screen alone over
    // black: Amakano 1's saveload.fes fades its page in, config.fes its `pmask` out.
    menuBlack_ = false;
    if (menuStep_ == MenuStep::Out) {
        if (++menuFrame_ < kMenuFadeOut) return;
        // Step 2 opens the screen, under the black (one step a frame, state 19).
        menuStep_ = MenuStep::Open;
        menuBlack_ = true;
        if (menuScreen_ == "saveload") openSaveScreen(menuLoad_);
        else openSystemScreen(menuScreen_);
        if (!systemScreen_) menuClosed(menuScreen_ == "saveload", -1);
    } else if (menuStep_ == MenuStep::In) {
        if (++menuFrame_ < kMenuHold + kMenuFadeIn) return;
        menuStep_ = MenuStep::None;
        menuWindowBack_ = false;
        window_->setMenuHidden(false);
    }
}

void Game::menuClosed(bool save, int32_t code) {
    if (menuStep_ != MenuStep::Open) return;
    // Back to the story: black at once (step 7, message 4), the fade from it two steps on (step 9).
    // A load goes through its own fade (state 5), quitting and the title leave the story.
    const bool back = sceneRunning_ && !quitRequested_ && !restartFlow_ && !(save && code >= 0 && loading());
    if (back && (save ? eraseSave_ : eraseConfig_)) {
        menuStep_ = MenuStep::In;
        menuFrame_ = 0;
        // The step that starts the fade back first shows the window again if etc/meswnd_restore
        // says so (Amakano 1 config 0x0cfcb5, save / load 0x0c501b: MES_SHOW 1), so it comes in
        // with the story; otherwise after the fade (config step 14). PC: the window's strip fades
        // in at the story's rate.
        if (save ? restoreSave_ : restoreConfig_) {
            menuWindowBack_ = true;
            window_->setMenuHidden(false);
        }
        return;
    }
    menuStep_ = MenuStep::None;
    window_->setMenuHidden(false);
}

float Game::menuFade() const {
    if (menuBlack_) return 1.0f;
    if (menuStep_ == MenuStep::Out) return static_cast<float>(menuFrame_) / static_cast<float>(kMenuFadeOut);
    if (menuStep_ != MenuStep::In) return 0.0f;
    if (menuFrame_ < kMenuHold) return 1.0f;
    return 1.0f - static_cast<float>(menuFrame_ - kMenuHold) / static_cast<float>(kMenuFadeIn);
}

std::map<std::string, int32_t> Game::confGroup(const std::string& prefix) const {
    std::map<std::string, int32_t> out;
    for (auto it = engineConfig_.lower_bound(prefix); it != engineConfig_.end() && it->first.starts_with(prefix); ++it) {
        out[it->first.substr(prefix.size())] = it->second;
    }
    return out;
}

int32_t Game::conf(const std::string& key, int32_t fallback) const {
    auto it = engineConfig_.find(key);
    return it == engineConfig_.end() ? fallback : it->second;
}

int32_t Game::messageFrame() const {
    if (player_.sceneTextSpeed() >= 0) return player_.sceneTextSpeed();
    return std::clamp(conf("messpd", 4), 0, std::max(0, strFrameCount_ - 1));
}

void Game::configSet(const std::string& key) {
    // Newer engines take the auto-mode speed as one setting (`autospd`, a startup.xml autopriset
    // level) and fill in the pause it stands for: autospd_b the base, autospd_c the per-character
    // pause (Love's Sweet Garnish's settings read them back: level 4 -> 1.5 and 0.5).
    if (key != "autospd" || autoPresets_.empty()) return;
    auto it = engineConfig_.find("autospd");
    size_t n = static_cast<size_t>(std::clamp(it->second, 0, static_cast<int32_t>(autoPresets_.size()) - 1));
    engineConfig_["autospd_b"] = autoPresets_[n].first;
    engineConfig_["autospd_c"] = autoPresets_[n].second;
}

uint32_t Game::autoWaitFrames(size_t chars) const {
    // sscript 0x0115e6 (the same in every CatSystem2 sscript we have; Yukikoi 0x0162d0): chars * c
    // * 60 / 100 + b * 60 / 10 frames, with b / c the scene's `auto on` pause or else autospd_b /
    // autospd_c (setconf stores them as given; `autospd <n>` copies autopriset[n]). The settings'
    // pause is divided by the auto level (Yukikoi m[0x84444], Fruit m[0x47498]; see
    // stepAutoLevel): chars * c * 60 / (100 * level) + b * 60 / (10 * level). `wait key` uses it too.
    const bool scene = player_.sceneAuto();
    int64_t b = scene ? player_.sceneAutoBase() : conf("autospd_b", 50);
    int64_t c = scene ? player_.sceneAutoChar() : conf("autospd_c", 50);
    const int64_t level = scene ? 1 : autoLevel_;
    int64_t frames = static_cast<int64_t>(chars) * c * 60 / (100 * level) + b * 60 / (10 * level);
    return static_cast<uint32_t>(std::max<int64_t>(0, frames));
}

bool Game::autoDue() const {
    // `wait key` in auto mode: the auto delay from when it began, for no text (0x0899e3 against
    // m[0x2ba50], set from 0x0163e8), whether a voice plays or not.
    if (player_.state() == ScenePlayer::State::WaitKey)
        return player_.frame() >= autoFrom(player_.waitKeyFrame()) + autoWaitFrames(0);
    // sscript 0x0643fe / 0x064505: auto goes on once the clock reaches the deadline set as the
    // message went up, unless `waitpcm` is on and a voice still plays (0x06453e). The sscript also
    // holds while images or planes are busy (0x067aff, 0x08b64c); we hold until the text is out.
    if (player_.state() != ScenePlayer::State::Input) return false;
    const auto& w = player_.stage().window;
    if (w.revealed < w.revealTotal()) return false;
    if (player_.frame() < autoFrom(player_.inputFrame()) + autoWaitFrames(player_.inputChars())) return false;
    // Sped up (level 2 or more) auto does not wait for the voice (Yukikoi 0x089099).
    return !(autoLevel_ <= 1 && conf("waitpcm", 1) && voicePlaying());
}

uint64_t Game::autoWaitStart() const {
    return player_.state() == ScenePlayer::State::WaitKey ? player_.waitKeyFrame() : player_.inputFrame();
}

void Game::setModes(bool skip, bool autoMode, bool forceSkip) {
    skipMode_ = skip;
    forceSkip_ = forceSkip;
    if (autoMode != autoMode_) {
        // Auto starting or stopping puts the level back to 1 and tells the window MES_SETMAG 10 and
        // MES_AUTOMAG 0 when it starts (Yukikoi's sscript 0x0ed86b), -1 when a click or the engine
        // stops it (0x0ed4f5, 0x010f03); the Auto request stopping it sends neither (0x0ed82f).
        const int32_t resume = autoMode ? std::exchange(autoResumeLevel_, 0) : 0;
        autoLevel_ = resume > 0 ? resume : 1;
        autoTimedWait_ = UINT64_MAX;
        const bool quiet = std::exchange(autoQuietStop_, false) && !autoMode;
        if (window_ && resume > 0) {
            // Auto back after a `fex` screen: the window hears its level only with automag.
            if (autoMag_) {
                int32_t index = 0;
                while ((1 << index) < autoLevel_) ++index;
                window_->setMag(autoLevel_ >= 2 ? 20 : 10, index);
            }
            window_->setMagEachMessage(autoMag_ && autoLevel_ >= 2);
        } else if (window_) {
            if (!quiet) window_->setMag(10, autoMode ? 0 : -1);
            window_->setMagEachMessage(false);
        }
    }
    autoMode_ = autoMode;
}

void Game::stepAutoLevel(bool up) {
    // While auto runs, automag on: the Auto request (Yukikoi 0x0ed7c5) and request 24 (0x0ee3a1)
    // double the level below 32, request 25 (0x0ee433) halves it above 1; a change times the wait
    // again (0x0164ca: from where the timer started, the new pause; the timer then starts now).
    // Then MES_SETMAG 20 (10 at level 1) and MES_AUTOMAG with the level's index, which each
    // message repeats while the level is 2 or more (0x003cc1).
    if (up ? autoLevel_ < 32 : autoLevel_ > 1) {
        const uint64_t wait = autoWaitStart();
        if (autoTimedWait_ != wait) autoTimedWait_ = wait, autoTimerStart_ = wait;
        autoFrom_ = autoTimerStart_;
        autoTimerStart_ = player_.frame();
        autoLevel_ = up ? autoLevel_ * 2 : autoLevel_ / 2;
    }
    if (window_) {
        int32_t index = 0;
        while ((1 << index) < autoLevel_) ++index;
        window_->setMag(autoLevel_ >= 2 ? 20 : 10, index);
        window_->setMagEachMessage(autoLevel_ >= 2);
    }
}

uint32_t Game::skipInterval() const {
    return static_cast<uint32_t>(std::clamp(conf("skip_speed", 0), 0, 100) / 10);
}

void Game::loadSoundConf() {
    Bytes xml;
    if (auto file = folder_->open("config/soundconf.xml")) xml = file->readAll();
    else if (assets_.find("soundconf.xml")) xml = assets_.read("soundconf.xml");
    if (xml.empty()) return;
    auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size())));
    const XmlNode* doc = root->child("document");
    if (!doc) return;
    soundConf_ = true;
    // Keys as the loader reads them (0x6afa3e..): a missing value is -1, then the parent's;
    // mute / voicemute read as 0 when missing.
    static const std::pair<const char*, const char*> kFields[] = {
        {"volume", "volume"}, {"play", "play"}, {"sysplay", "sysplay"}, {"voicebgm", "voicevol"},
        {"mastervol", "mastervol"}, {"skipvol", "skipvol"}};
    auto read = [](const XmlNode& n, SoundPart& part, const SoundPart* parent) {
        part.tag = n.name;
        part.type = n.value("type").value_or("");
        part.head = toLowerAscii(n.value("head").value_or(""));  // lower-cased (0x718ca0 -> 0x6a7850)
        part.id = n.intValue("id", -1);
        for (const auto& [field, key] : kFields) {
            int32_t v = n.intValue(key, -1);
            part.def[field] = v == -1 && parent ? parent->def.at(field) : v;
        }
        part.def["mute"] = n.intValue("mute", 0);
        part.def["voicemute"] = n.intValue("voicemute", 0);
    };
    soundMaster_.tag = "master";
    soundMaster_.def = {{"volume", 100}, {"play", 1}, {"mute", 0}};
    if (const XmlNode* m = doc->child("MASTER")) {
        soundMaster_.def["volume"] = m->intValue("volume", 100);
        soundMaster_.def["play"] = m->intValue("play", 1);
        soundMaster_.def["mute"] = m->intValue("mute", 0);
    }
    const bool startupVoices = !voices_.empty();  // older games list them in startup.xml <VOICE>
    if (const XmlNode* partList = doc->child("PART")) {
        for (const auto& n : partList->children) {
            SoundPart top;
            read(*n, top, nullptr);
            int32_t group = n->intValue("group", -1);
            int32_t mastervol = n->intValue("mastervol", 100);
            if (const XmlNode* sub = n->child("part")) {
                for (const auto& c : sub->children) {
                    SoundPart child;
                    read(*c, child, &top);
                    top.parts.push_back(std::move(child));
                    // The character voices (group 2) also name each voice archive's prefix.
                    if (group == 2 && !startupVoices) {
                        VoiceEntry e;
                        e.head = toLowerAscii(c->value("head").value_or(""));
                        e.archive = toLowerAscii(c->value("int").value_or(""));
                        e.group = 2;
                        e.mastervol = mastervol;
                        e.character = top.parts.back().id;
                        if (!e.head.empty()) voices_.push_back(std::move(e));
                    }
                }
            }
            soundParts_.push_back(std::move(top));
        }
    }
}

const Game::SoundPart* Game::soundPart(const std::string& name, const std::vector<int32_t>& ids,
                                       std::string& key) const {
    if (ids.empty()) {
        if (name == "master") {
            key = "master";
            return &soundMaster_;
        }
        // By name (0x6b2490): the part whose tag or type it is.
        for (const auto& p : soundParts_) {
            if (p.tag == name || p.type == name) {
                key = std::to_string(p.id);
                return &p;
            }
        }
        return nullptr;
    }
    // By id path (0x6aae20): each id picks a part among the previous one's children.
    const std::vector<SoundPart>* list = &soundParts_;
    const SoundPart* found = nullptr;
    for (int32_t id : ids) {
        auto it = std::find_if(list->begin(), list->end(), [&](const SoundPart& p) { return p.id == id; });
        if (it == list->end()) continue;
        found = &*it;
        key += (key.empty() ? "" : " ") + std::to_string(id);
        list = &found->parts;
    }
    return found;
}

int32_t Game::soundValue(const SoundPart& part, const std::string& key, const std::string& field) const {
    auto it = engineConfig_.find("snd." + field + " " + key);
    if (it != engineConfig_.end()) return it->second;
    auto d = part.def.find(field);
    return d == part.def.end() ? 0 : d->second;
}

const Game::PartPath& Game::filePath(const std::string& name) const {
    auto it = filePaths_.find(name);
    if (it != filePaths_.end()) return it->second;
    // 0x71ae60: the name lower-cased (0x6a7850); 0x71af30: a part whose head (lower-cased when
    // loaded) is a prefix of it, the part before its children, the tree in file order, the first
    // match winning. Nekopara's `voice` comes before `bgvoice`, so its background voices count as
    // its heroines' voices.
    const std::string low = toLowerAscii(name);
    PartPath path;
    std::function<bool(const std::vector<SoundPart>&, const std::string&)> find = [&](const std::vector<SoundPart>& list,
                                                                                         const std::string& prefix) {
        for (const auto& p : list) {
            std::string key = prefix + (prefix.empty() ? "" : " ") + std::to_string(p.id);
            path.emplace_back(&p, key);
            if (!p.head.empty() && low.compare(0, p.head.size(), p.head) == 0) return true;
            if (find(p.parts, key)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!find(soundParts_, "")) path.clear();
    if (filePaths_.size() > 512) filePaths_.clear();
    return filePaths_.emplace(name, std::move(path)).first->second;
}

float Game::partLevel(const PartPath& path, bool voicevol) const {
    // 0x71b4e0 / 0x71b790: from 100, for the part and then each parent, x volume (voicevol) / 100
    // and x mastervol / 100; a muted one makes it 0. No part: 100.
    float level = 1.0f;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const auto& [p, key] = *it;
        if (soundValue(*p, key, "mute")) return 0.0f;
        const int32_t v = soundValue(*p, key, voicevol ? "voicebgm" : "volume");
        const int32_t mv = soundValue(*p, key, "mastervol");
        level *= std::clamp(v, 0, 100) / 100.0f * (mv < 0 ? 1.0f : std::clamp(mv, 0, 100) / 100.0f);
    }
    return level;
}

void Game::startSound(int bus, int channel, const std::string& name) {
    // sscript at a sound's start (0x0a093c): it ducks under voices when its part's voicevol is set
    // (native 947, >= 0; no part counts as 100), and keeps its part's skipvol (native 948, -1 none).
    PlayingSound s;
    s.path = filePath(name);
    if (!s.path.empty()) {
        const auto& [p, key] = s.path.back();
        s.duck = soundValue(*p, key, "voicebgm") >= 0;
        s.skipvol = soundValue(*p, key, "skipvol");
    } else {
        s.duck = true;
    }
    playingSounds_[{bus, channel}] = std::move(s);
    updateSoundGains();  // its level from the start
}

void Game::updateSoundGains() {
    if (!mixer_ || !soundConf_) return;
    // sscript 0x0a35e9, each frame: while a voice plays (its slots 12-21), a ducking sound takes
    // its parts' voicevol instead of volume (native 995 for 964); while skipping, x skipvol / 100
    // when set. A change is faded in over 8 frames (native 163). Settings changed from the config
    // screen reach playing sounds the same way.
    const bool voice = voicePlaying();
    const bool skipping = skipMode_ || forceSkip_;
    for (auto it = playingSounds_.begin(); it != playingSounds_.end();) {
        const auto bus = static_cast<audio::Mixer::Bus>(it->first.first);
        if (!mixer_->isPlaying(bus, it->first.second)) {
            it = playingSounds_.erase(it);
            continue;
        }
        PlayingSound& s = it->second;
        float g = partLevel(s.path, s.duck && voice);
        if (skipping && s.skipvol != -1) g *= std::clamp(s.skipvol, 0, 100) / 100.0f;
        if (g != s.gain) {
            if (std::getenv("NYAN_MEDIA_TRACE") && player_.log)
                player_.log("MEDIA gain bus " + std::to_string(it->first.first) + " ch " + std::to_string(it->first.second) + " " +
                            std::to_string(s.gain) + " -> " + std::to_string(g) + (voice ? " (voice)" : "") + (skipping ? " (skip)" : ""));
            mixer_->setGain(bus, it->first.second, g, s.gain < 0 ? 0 : 8 * 1000 / 60);
            s.gain = g;
        }
        ++it;
    }
}

std::optional<int32_t> Game::soundConf(const std::string& field, const std::string& name,
                                       const std::vector<int32_t>& ids) {
    if (!soundConf_) return std::nullopt;
    std::string key;
    if (const SoundPart* p = soundPart(name, ids, key)) return soundValue(*p, key, field);
    // No such part: the getters' starting values (volume 100, play / sysplay on, mute off).
    return field == "volume" ? 100 : field == "play" || field == "sysplay" ? 1 : 0;
}

bool Game::setSoundConf(const std::string& field, const std::string& name, const std::vector<int32_t>& ids,
                        int32_t value, bool notify) {
    if (!soundConf_) return false;
    std::string key;
    if (soundPart(name, ids, key)) engineConfig_["snd." + field + " " + key] = value;
    // setsndconfex (0x71eb00) then applies the settings to playing sound; ...nonotify does not.
    if (notify) applyBusVolumes();
    return true;
}

bool Game::resetSoundConf(const std::string& name, const std::vector<int32_t>& ids, bool notify) {
    // setsndconfex's reset (cs2.exe 0x783a20 -> 0x716910; master 0x716c40): the part's volume, play,
    // sysplay, voicebgm and mute go back to the values it was loaded with (soundconf.xml, a missing
    // one its parent's, 0x718ca0: the def map here), its children keeping theirs; then, as every
    // setsndconfex, the settings reach the playing sound (0x712760, 0x713f30).
    if (!soundConf_) return false;
    std::string key;
    if (soundPart(name, ids, key)) {
        for (const char* f : {"volume", "play", "sysplay", "voicebgm", "mute"}) engineConfig_.erase(std::string("snd.") + f + " " + key);
    }
    if (notify) applyBusVolumes();
    return true;
}

std::optional<int32_t> Game::pickSystemVoice(const std::vector<std::vector<int32_t>>& paths) {
    if (!soundConf_) return std::nullopt;
    // sysvoiceex (0x71fb00): the given parts with sysplay on, or the sysvoice part's characters
    // with sysplay == 1; one of them at random.
    std::vector<int32_t> pick;
    if (!paths.empty()) {
        for (const auto& path : paths) {
            if (!path.empty() && soundConf("sysplay", {}, path).value_or(0)) pick.push_back(path.back());
        }
    } else {
        for (const auto& p : soundParts_) {
            if (p.tag != "sysvoice" && p.type != "sysvoice") continue;
            for (const auto& c : p.parts) {
                if (soundValue(c, std::to_string(p.id) + " " + std::to_string(c.id), "sysplay") == 1) pick.push_back(c.id);
            }
        }
    }
    if (pick.empty()) return -1;
    return pick[static_cast<size_t>(randomBetween(0, static_cast<int32_t>(pick.size()) - 1))];
}

bool Game::soundPlaying(int kind, int32_t channel) {
    if (!mixer_) return false;
    using K = MediaCommand::Kind;
    const auto k = static_cast<K>(kind);
    const auto bus = k == K::PlayBgm ? audio::Mixer::Bus::Bgm : k == K::PlayVoice ? audio::Mixer::Bus::Voice : audio::Mixer::Bus::Se;
    return mixer_->isPlaying(bus, channel);
}

bool Game::bgmPlaying(int32_t channel) {
    return !mixer_ || mixer_->isPlaying(audio::Mixer::Bus::Bgm, channel);
}

bool Game::voicePlaying() const {
    if (!mixer_) return false;
    for (int ch = 0; ch < 32; ++ch) {
        if (mixer_->isPlaying(audio::Mixer::Bus::Voice, ch)) return true;
    }
    return false;
}

bool Game::hasQuickSave() {
    return quickCount_ > 0 && saves_.newest(quickStart_, quickStart_ + quickCount_ - 1) >= 0;
}

int32_t Game::rotatingSlot(int32_t start, int32_t count) {
    // The first empty slot of the range, else the oldest one is replaced.
    int32_t slot = -1;
    int64_t oldest = 0;
    for (int32_t s = start; s < start + count; ++s) {
        const SaveFile* existing = saves_.get(s);
        if (!existing) return s;
        if (slot < 0 || existing->time < oldest) {
            slot = s;
            oldest = existing->time;
        }
    }
    return slot;
}

void Game::autosave() {
    if (autoStart_ < 0 || autoCount_ <= 0) return;
    auto f = captureSave();
    if (!f) return;
    saves_.put(rotatingSlot(autoStart_, autoCount_), *f, player_.frame());
    if (player_.log) player_.log("autosave");
}

bool Game::isRead(const std::string& key) const {
    size_t colon = key.rfind(':');
    auto it = read_.find(key.substr(0, colon));
    if (it == read_.end()) return false;
    size_t line = std::strtoul(key.c_str() + colon + 1, nullptr, 10);
    return line / 8 < it->second.size() && (it->second[line / 8] >> (line % 8)) & 1;
}

void Game::markRead(const std::string& key) {
    size_t colon = key.rfind(':');
    auto& bits = read_[key.substr(0, colon)];
    size_t line = std::strtoul(key.c_str() + colon + 1, nullptr, 10);
    if (bits.size() <= line / 8) bits.resize(line / 8 + 1, 0);
    bits[line / 8] = static_cast<uint8_t>(bits[line / 8] | (1u << (line % 8)));
}

std::optional<int32_t> Game::exVar(const std::string& name) {
    if (name == "SKIP") return skipMode_ ? 1 : 0;
    if (name == "AUTO") return autoMode_ ? 1 : 0;
    if (name == "SKIP_FORCE") return forceSkip_ ? 1 : 0;
    if (name == "STILL_READ") return currentRead_ ? 1 : 0;
    if (name == "NAME") return player_.stage().window.name.empty() ? 0 : 1;
    return std::nullopt;
}

bool Game::request(int64_t code, int32_t a, int32_t b) {
    // Message log requests (`send 0xfffe000N <entry> <var>`), answered before the script continues.
    const auto& log = *backlog();
    auto entry = [&](int32_t i) -> const ScenePlayer::LogEntry* {
        return i >= 0 && static_cast<size_t>(i) < log.size() ? &log[static_cast<size_t>(i)] : nullptr;
    };
    switch (code) {
    case 0xfffe0000:  // play voice: the file named in string variable `a`
    case 0xfffe0003:  // (newer engines: the same, "CV" variant)
        playVoiceFile(vars_.str(a));
        return true;
    case 0xfffe0001:  // speaker name of entry `a` into string variable `b`, in GAME/log/nameformat
        vars_.setStr(b, entry(a) && !entry(a)->name.empty() ? logNameFirst_ + entry(a)->name + logNameLast_
                                                             : std::string());
        return true;
    case 0xfffe0002:  // speaker's name-table id of entry `a` into `b` (scripts check `play 3 <id>`)
        vars_.set(b, entry(a) ? nameId(entry(a)->speaker) : -1);
        return true;
    case 0x803: {
        // The text of scene step `a` into string variable `b` (Happiness 2 sscript 0x0126fe; its
        // phone shows each message this way, in the text language). Script strings are Shift-JIS;
        // text they can't hold stays UTF-8.
        bool utf8 = false;
        std::string text = player_.stepText(a, &utf8);
        if (utf8) {
            std::string sjis = utf8ToSjis(text);
            if (sjisToUtf8(sjis) == text) text = std::move(sjis);
        }
        vars_.setStr(b, text);
        return true;
    }
    default: return false;
    }
}

bool Game::request(FesInstance& from, int64_t code, int32_t a, int32_t b) {
    // touch.fes's requests (`_SYS_EX_OSAWARI*`): sscript's attach (0x0112ee), enter (the story
    // mode's 0x0dbbb5: mode 27, state 0) and mode 27's handler (0x0e39b5).
    const bool ready = touch_.state == TouchState::Idle || touch_.state == TouchState::Playing;
    switch (code) {
    case 0x780:
        if (touch_.state == TouchState::Off && touchEnabled_ && sceneRunning_) touch_.state = TouchState::Setup;
        return true;
    case 0x781:
        if (touch_.state != TouchState::Off) touch_.state = TouchState::Stop;
        return true;
    case 0x782:
        if (ready) {
            touch_.x = a, touch_.y = b;
            touch_.state = TouchState::Play;
        }
        return true;
    case 0x783:
        touchAttached_ = true;
        return true;
    case 0x784:
        // The hit test (0x0db643) answers into the script's \0 (native 645); outside idle and
        // playing it is left as the script set it.
        if (ready) {
            bool hit = false;
            for (int32_t k = 9; k >= 0 && !hit; --k) {
                hit = touch_.hit.count(k) && !player_.touchRows(k, touchPixel(k, a, b)).empty();
            }
            from.setLocal(0, hit ? 1 : 0);
        }
        return true;
    default: return request(code, a, b);
    }
}

uint32_t Game::touchPixel(int32_t bank, int32_t x, int32_t y) const {
    // Native 1026: the hit surface's pixel at the click (ARGB), 0 off the surface.
    auto it = touch_.hit.find(bank);
    if (it == touch_.hit.end() || x < 0 || y < 0 || x >= config_.width || y >= config_.height) return 0;
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(config_.width) + static_cast<size_t>(x);
    return i < it->second.size() ? it->second[i] : 0;
}

void Game::stepTouch() {
    auto log = [&](const std::string& m) {
        if (player_.log && std::getenv("NYAN_TOUCH_TRACE")) player_.log("touch: " + m);
    };
    if (touch_.state != TouchState::Off && !sceneRunning_) {
        touch_ = Touch{};
        window_->setTouchHidden(false);
        player_.setHeld(false);
        player_.resetActions(-1);
        return;
    }
    switch (touch_.state) {
    case TouchState::Off:
    case TouchState::Idle:
        break;
    case TouchState::Setup:
        // 0x0da68a: the scripts beside the window hear MES_ENTER_EX_OSAWARI, the window hides; each
        // touchable bank is drawn alone with its hit layer on and kept as its hit surface; then
        // the banks forget what their actions return to (native 1048, message 0x226).
        log("enter");
        window_->sideInvoke("MES_ENTER_EX_OSAWARI");
        window_->setTouchHidden(true);
        player_.setHeld(true);
        touch_.hit.clear();
        for (int32_t k : player_.touchBanks()) {
            if (!bankCapture_) break;
            player_.setHitLayer(k, true);
            touch_.hit[k] = bankCapture_(*player_.bank(k));
            player_.setHitLayer(k, false);
            log("bank " + std::to_string(k) + " hit surface");
        }
        for (int32_t k : player_.touchBanks()) player_.resetActions(k);
        touch_.voice = false;
        touch_.state = TouchState::Idle;
        break;
    case TouchState::Play:
        // 0x0db158: banks 9..0, the first whose surface's colour at the click has rows in its
        // `touch` table plays a random row: lip sync off (native 928), the action (1027), the voice
        // (0x0db984). (The Steam achievement it then unlocks is left out.)
        for (int32_t k = 9; k >= 0; --k) {
            if (!touch_.hit.count(k)) continue;
            const uint32_t c = touchPixel(k, touch_.x, touch_.y);
            char hex[16];
            std::snprintf(hex, sizeof hex, "%08x", c);
            log("colour [" + std::to_string(k) + "] : 0x" + hex);
            auto rows = player_.touchRows(k, c);
            if (rows.empty()) continue;
            const auto& row = rows[static_cast<size_t>(randomBetween(0, static_cast<int32_t>(rows.size()) - 1))];
            log("play action " + row.action + " (" + row.voice + ")");
            player_.stopLip(k);
            player_.playAction(k, row.action);
            playVoiceFile(row.voice);
            touch_.voice = true;
            break;
        }
        touch_.state = TouchState::Playing;
        break;
    case TouchState::Playing:
        if (!player_.actionsPlaying()) {
            log("action done");
            touch_.state = TouchState::Idle;
        }
        break;
    case TouchState::Stop:
        // 0x0db8ed: every bank's action to its `restore` step.
        player_.stopActions();
        touch_.state = TouchState::Stopping;
        break;
    case TouchState::Stopping:
        if (player_.actionsPlaying()) break;
        // 0x0daf3a: the reaction's voice stops, the surfaces go, the window is shown as it was.
        log("exit");
        if (touch_.voice && mixer_) mixer_->stop(audio::Mixer::Bus::Voice, -1);
        touch_ = Touch{};
        window_->setTouchHidden(false);
        player_.setHeld(false);
        break;
    }
}

const std::vector<ScenePlayer::LogEntry>* Game::backlog() {
    const auto& all = player_.backlog();
    // When the message on screen joins the log is the game's sscript's choice. From Labyrinth's on
    // (2.6.1.72..90: Labyrinth, Eden, Himawari, Nekopara, Hatsumira, LSG, Amakano 2, Cyanotype,
    // Yukikoi) the message line's handler adds it as it is shown (native 977, Cyanotype's 1082 ->
    // cs2.exe 0x650e50 -> the log's add, 0x72e8d0; Nekopara 0x012b53 -> 0x0bd87b), so the log
    // lists it at once. Fruit's (2.6.1.70) adds it only when the player leaves it (native 698 from
    // the wait handler, 0x092253 / 0x091e2d, and the skip / auto advance, 0x064723).
    bool listsCurrent = engineVersion_ >= kEngineLogsCurrentLine;
    size_t n = all.size() - (player_.lastEntryOnScreen() && !listsCurrent ? 1 : 0);
    if (backlogView_.size() != n ||
        (n && (backlogView_.back().text != all[n - 1].text || backlogView_.back().point != all[n - 1].point))) {
        backlogView_.assign(all.begin(), all.begin() + static_cast<long>(n));
    }
    return &backlogView_;
}

void Game::playVoiceFile(const std::string& voice) {
    if (player_.log && std::getenv("NYAN_FES_TRACE")) player_.log("play voice '" + voice + "'");
    if (voice.empty() || !mixer_) return;
    mixer_->stop(audio::Mixer::Bus::Voice, -1);
    if (auto ref = assets_.find(voice + ".ogg")) {
        if (soundConf_) {
            const PartPath& path = filePath(voice);
            if (!path.empty() && !soundValue(*path.back().first, path.back().second, "play")) return;
            mixer_->play(audio::Mixer::Bus::Voice, 0, ref->archive->read(*ref->entry), false, 0, 1.0f);
            startSound(static_cast<int>(audio::Mixer::Bus::Voice), 0, voice);
            return;
        }
        mixer_->play(audio::Mixer::Bus::Voice, 0, ref->archive->read(*ref->entry), false, 0, volumeFor(2, voiceFor(voice)));
    }
}

void Game::replayVoice() {
    if (player_.log && std::getenv("NYAN_FES_TRACE")) player_.log("replay voice " + player_.currentVoice());
    playVoiceFile(player_.currentVoice());
}

void Game::loadNameTable() {
    // nametable.csv: "<id>\t<name>\t<short>\t<format>[\t<another name>...]" per line. The loader
    // (0x8335d0) takes rows whose id starts with a digit (so `//` comments are skipped); a row's
    // names are column 1 and any from column 4 on.
    Bytes data;
    if (auto file = folder_->open("config/nametable.csv")) data = file->readAll();
    else if (assets_.find("nametable.csv")) data = assets_.read("nametable.csv");
    if (data.empty()) return;
    nameTableLoaded_ = true;
    std::string text = decodeGameText(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] < '0' || line[0] > '9') continue;
        std::vector<std::string> cells;
        for (size_t at = 0;;) {
            size_t tab = line.find('\t', at);
            cells.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        const int32_t id = std::atoi(cells[0].c_str());
        for (size_t c = 1; c < cells.size(); c = c == 1 ? 4 : c + 1) {
            std::string& name = cells[c];
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
            if (!name.empty()) nameTable_.emplace(name, id);  // the first row listing it wins
        }
    }
}

void Game::sendSpeaker() {
    const auto& w = player_.stage().window;
    sendSpeaker(w.speaker, w.name);
}

void Game::sendSpeaker(const std::string& speaker, const std::string& name) {
    // The speaker for the window (MES_SETNAME: name-table id, name in $str1001).
    sentSpeaker_ = speaker;
    sentName_ = name;
    // Script strings are Shift-JIS; a name it can't hold (a Chinese translation) stays UTF-8,
    // which the window's text drawing also takes.
    std::string sjis = utf8ToSjis(name);
    if (sjisToUtf8(sjis) != name) sjis = name;
    const int32_t id = nameId(speaker);
    if (player_.log && std::getenv("NYAN_FES_TRACE"))
        player_.log("name " + std::to_string(id) + " " + speaker + " @" + std::to_string(player_.frame()));
    window_->setName(id, sjis);
}

int32_t Game::nameId(const std::string& speaker) const {
    // sscript 0x000074 (the same in every game): -1 without a name table or for an empty name;
    // the part after a `＠`; the id of the first row listing it (natives 635/636: 0x833b60 looks
    // through each row's names); 999 when no row does. The key is the name line as the scene
    // writes it, untranslated and unprocessed (native 1037), so the voice plays no part.
    if (!nameTableLoaded_) return -1;
    std::string_view key = speaker;
    if (size_t at = key.find("\xEF\xBC\xA0"); at != std::string_view::npos) key = key.substr(at + 3);
    if (key.empty()) return -1;
    auto it = nameTable_.find(std::string(key));
    return it == nameTable_.end() ? 999 : it->second;
}

bool Game::openLog() {
    if (systemScreen_ || backlog()->empty()) return false;
    const WindowLayout* L = theme_.find(player_.stage().window.style);
    if (!L) L = theme_.find("default");
    if (!L || L->logScript.empty()) return false;
    for (int i = 0; i < 3; ++i) vars_.setStr(910 + i, L->logImages[i]);
    systemScreen_ = FesInstance::load(assets_, vars_, L->logScript, player_.frame(), &engineConfig_, this);
    if (!systemScreen_) return false;
    systemScreen_->setLocal(0, 1);  // `log setlog \0`: the engine's backlog
    logScreen_ = true;
    systemScreen_->log = [this](const std::string& m) {
        if (player_.log) player_.log(m);
    };
    return true;
}

bool Game::openExtraButton(int32_t n) {
    if (systemScreen_ || n < 0 || n >= 4) return false;
    const WindowLayout* L = theme_.find(player_.stage().window.style);
    if (!L) L = theme_.find("default");
    if (!L || L->extraScripts[n].empty()) return false;
    // sscript (0x0e156b): the button's images as $str910..913 (native 276), the window hidden.
    for (int k = 0; k < 4; ++k) vars_.setStr(910 + k, L->extraImages[n][k]);
    openSystemScreen(L->extraScripts[n]);
    if (!systemScreen_) return false;
    extraScreen_ = true;
    window_->setSuppressed(true);
    return true;
}

bool Game::quickSave() {
    if (quickStart_ < 0 || quickCount_ <= 0) return false;
    auto f = captureSave();
    if (!f) return false;
    int32_t slot = rotatingSlot(quickStart_, quickCount_);
    saves_.put(slot, *f, player_.frame());
    // sscript's quick save (Nekopara 0x0ab325) keeps the slot's place in the range in the
    // savenew_quick variable (native 211); the message window shows its notice when it changes.
    if (saveNewQuickFlag_ >= 0) vars_.set(saveNewQuickFlag_, slot - quickStart_);
    saveGlobal();
    return true;
}

bool Game::quickLoad() {
    if (!sceneRunning_ || quickCount_ <= 0) return false;
    int32_t slot = saves_.newest(quickStart_, quickStart_ + quickCount_ - 1);
    if (loading() || !saves_.exists(slot)) return false;
    loadThroughFade([this, slot] {
        if (auto r = saves_.resume(slot, player_.frame())) player_.load(*r, globalFrom_, globalTo_);
    });
    return true;
}

bool Game::returnToLastChoice() {
    if (!player_.lastChoice() || !sceneRunning_ || loading()) return false;
    loadThroughFade([this] {
        if (!player_.lastChoice()) return;
        ScenePlayer::Resume r = *player_.lastChoice();
        player_.restore(r, globalFrom_, globalTo_);
    });
    return true;
}

void Game::loadThroughFade(std::function<void()> load) {
    // sscript turns skip and auto off as the fade starts (m[0x80000090], m[0x8000009c]).
    fadeLoad_ = std::move(load);
    fadeStep_ = FadeStep::Out;
    fadeFrame_ = 0;
}

void Game::stepFade() {
    if (fadeStep_ == FadeStep::None) return;
    if (++fadeFrame_ < kLoadFadeFrames) return;
    fadeFrame_ = 0;
    if (fadeStep_ == FadeStep::Out) {
        if (auto load = std::exchange(fadeLoad_, nullptr)) load();
        fadeStep_ = FadeStep::In;
    } else {
        fadeStep_ = FadeStep::None;
    }
}

float Game::screenFade() const {
    float t = static_cast<float>(fadeFrame_) / static_cast<float>(kLoadFadeFrames);
    switch (fadeStep_) {
    case FadeStep::Out: return t;
    case FadeStep::In: return 1.0f - t;
    default: return 0.0f;
    }
}

// What the scene's lines told the window since it last heard: clicks (MES_CLEAR), `mesdraw`, name
// lines (MES_SETNAME), the speaker of the message now up and its read colour. Run in the frame's
// tick and again by the front end after it moves the story on outside it (skip's bursts), so a
// message never shows a frame with the speaker before it: sscript sends MES_SETNAME as it runs the
// name line, in the tick that puts the message up.
void Game::syncWindowEvents() {
    // The window hears the frame's clicks and name lines in the order they came (sscript's order:
    // a click's MES_CLEAR before the next message's MES_SETNAME, every name line's own).
    for (const auto& e : player_.takeWindowEvents()) {
        if (!sceneRunning_) continue;
        if (e.kind == ScenePlayer::WindowEvent::Kind::Click) window_->clicked(e.clears);
        else if (e.kind == ScenePlayer::WindowEvent::Kind::Draw) window_->setDraw(e.clears, engineVersion_ >= kEngineMesdrawClears);
        else sendSpeaker(e.speaker, e.name);
    }
    // Read-text tracking: a message counts as read once the player moves past it. Showing one that
    // was read before tells the window (MES_SETNAMECOLOR) to use the read colour if enabled.
    std::string key = player_.messageKey();
    if (key != currentKey_) {
        if (!currentKey_.empty()) markRead(currentKey_);
        currentKey_ = key;
        currentRead_ = !key.empty() && isRead(key);
        if (player_.log && std::getenv("NYAN_READ_TRACE")) player_.log("message " + key + (currentRead_ ? " read" : " new") + " @" + std::to_string(player_.frame()));
        if (!key.empty() && sceneRunning_) {
            sendSpeaker();
            bool colour = currentRead_ && conf("enable_readcol", 0) != 0;
            window_->setTextColor(colour ? readColor_ : 0xFFFFFFFF);
        }
    }
    // sscript sets the speaker as the message goes up (the name line, 0x00135c), so a `\@` message
    // still in its waits shows its own speaker rather than the last one waited on.
    if (const auto& w = player_.stage().window;
        sceneRunning_ && !w.text.empty() && (w.speaker != sentSpeaker_ || w.name != sentName_))
        sendSpeaker();
}

void Game::tick() {
    // The glossary's handle is the engine's: restored variables (global data, a save) never take
    // it away.
    if (dictionaryVar_ >= 0 && dictionary_.loaded() && vars_.get(dictionaryVar_) != 1) vars_.set(dictionaryVar_, 1);
    if (movie_.open()) {
        // The picture follows the clock (the sound runs on its own at the same rate).
        ++movie_.frames;
        if (movie_.seek(static_cast<double>(movie_.frames) / 60.0)) movie_.changed = true;
        if (movie_.ended()) endMovie();
    }
    applyLanguage();  // the options screen may have changed it
    player_.setEffectSpeed(conf("effspd", 0));
    player_.setEffectCut(conf("effcut", 0) != 0);
    player_.setSceneAnimations(conf("enable_sceneanm", 1) != 0);
    stepFade();
    stepMenu();
    player_.tick();
    window_->setDramaHidden(player_.dramaVoice());
    stepTouch();
    // The scene's own auto mode (`auto on`) runs whatever the player's auto button says.
    if (player_.sceneAuto() && !systemScreen_ && !loading() && !touchMode() && autoDue()) player_.advance();
    if (sceneRunning_ && player_.state() == ScenePlayer::State::Ended) {
        // The story returned to the title (`title` command or end of the route).
        sceneRunning_ = false;
        kcsExit_ = 0;
    }
    // `rettitle` from last frame's menus: the flow starts over before the old one reacts.
    auto restart = [&] {
        if (!restartFlow_) return;
        restartFlow_ = false;
        if (player_.log) player_.log("rettitle: flow restarts");
        startFlow();
    };
    restart();
    if (flow_) flow_->tick(player_.frame());
    restart();  // `rettitle` from a screen inside the flow itself
    if (sceneRunning_) {
        runResidentCommands();
        // sscript's other resident-script labels (Happiness 2 0x0cc8cd ... 0x0ccbd4): RS_SKIP with
        // the skip state in \0 as it changes (0x0cfa4b), RS_LOAD after a load (0x0d3502, 0x0d422f),
        // RS_JUMP after a jump (here a return to a backlog entry), RS_REDRAW when a system screen
        // closes (fitted: sscript sends it at five points around the FE tasks' sleep, native 647).
        const int32_t skip = skipMode_ || forceSkip_ ? 1 : 0;
        if (skip != residentSkip_) {
            residentSkip_ = skip;
            residentLabel("RS_SKIP", {skip});
        }
        if (player_.loadCount() != residentLoads_) {
            residentLoads_ = player_.loadCount();
            residentLabel("RS_LOAD");
            window_->setDraw(player_.stage().window.textVisible, false);  // the loaded `mesdraw`
        }
        if (player_.restoreCount() != windowRestores_) {
            // A load, a return to a backlog line or Previous Option: sscript redraws the window
            // (Himawari 0x0aa019 / 0x0d227e -> 0x0017ae: MES_CLEAR, then the restored message's text
            // and name). Without the clear the speaker before the load kept Himawari's plate up
            // over restored narration (MES_SETNAME -1 leaves it).
            windowRestores_ = player_.restoreCount();
            window_->redraw();
            if (!player_.stage().window.text.empty()) sendSpeaker();
        }
        if (player_.jumpCount() != residentJumps_) {
            residentJumps_ = player_.jumpCount();
            residentLabel("RS_JUMP");
        }
        const bool screen = systemScreen_ != nullptr;
        if (residentScreen_ && !screen) residentLabel("RS_REDRAW", {1});
        residentScreen_ = screen;
        for (auto& r : residents_) {
            if (r) r->tick(player_.frame());
        }
    }
    if (systemScreen_ && kcsScreen_) {
        systemScreen_->tick(player_.frame());
        if (systemScreen_->exited()) {
            kcsExit_ = systemScreen_->exitCode();
            kcsScreen_ = false;
            systemScreen_.reset();
            if (!restartFlow_) saveGlobal();
        }
    } else if (systemScreen_) {
        systemScreen_->tick(player_.frame());
        if (systemScreen_->exited() || !sceneRunning_) {
            int32_t code = systemScreen_->exited() ? systemScreen_->exitCode() : -1;
            systemScreen_.reset();
            const bool wasConfig = std::exchange(configScreen_, false);
            const bool wasSave = std::exchange(saveScreen_, false);
            if (std::exchange(extraScreen_, false)) window_->setSuppressed(false);
            if (choiceScreen_) {
                choiceScreen_ = false;
                if (code >= 0 && static_cast<size_t>(code) < player_.stage().choices.size()) chosen_ = code;
                else choiceScreenFailed_ = true;  // closed without a pick: fall back to the generic one
                logScreen_ = false;
                return;
            }
            pendingSave_.reset();
            if (!restartFlow_) saveGlobal();
            // In-game menus exit with -3 for Game End; the engine's own `config` screen with -2
            // (Amakano's sscript 0x0de969: native 9 posts the window's quit message; Labyrinth's
            // config.fes turns its save screen's -3 into -2).
            // Leaving the story keeps an autosave of where it was (the title's Continue).
            if (code == -3 || (code == -2 && wasConfig)) {
                if (sceneRunning_) autosave();
                quitRequested_ = true;
            }
            // The save/load screen opened by the engine exits with the slot to load. Other screens'
            // codes are not slots: the message log and config's Back exit with 0 (Amakano's sscript,
            // 0x0de905: the config screen's code only matters when it is -2, Game End).
            // The log screen's -2 goes back to the entry `log pathinfo` named (sscript 0x0c8784:
            // the path index from logret_pathindex, then a 16-frame fade out and back in).
            if (code == -2 && logScreen_ && logRetFlag_ >= 0 && sceneRunning_) {
                const auto id = static_cast<uint32_t>(vars_.get(logRetFlag_));
                loadThroughFade([this, id] { player_.returnToLog(id, globalFrom_, globalTo_); });
            }
            if (code >= 0 && wasSave && !logScreen_ && sceneRunning_ && saves_.exists(code)) {
                loadThroughFade([this, code] {
                    if (auto r = saves_.resume(code, player_.frame())) player_.load(*r, globalFrom_, globalTo_);
                });
            }
            logScreen_ = false;
            if (wasConfig || wasSave) menuClosed(wasSave, code);
        }
    }
    // Global data is also written now and then, so a crash or kill loses little.
    if (!restartFlow_ && player_.frame() >= lastGlobalCheck_ + 600) {
        lastGlobalCheck_ = player_.frame();
        saveGlobal();
    }
    syncWindowEvents();
    // Autosave when a choice comes up (and on the scene's `autosave` command).
    bool atChoice = player_.state() == ScenePlayer::State::Choice;
    // Choices show on the game's own choice screen where its window theme has one.
    if (atChoice && sceneRunning_ && !choiceScreen_ && !choiceScreenFailed_ && !systemScreen_ && chosen_ < 0) {
        if (!openChoiceScreen()) choiceScreenFailed_ = true;
    }
    if (!atChoice) {
        choiceScreenFailed_ = false;
        if (choiceScreen_) {  // the choice was settled elsewhere (a load, a skip): the screen goes
            systemScreen_.reset();
            choiceScreen_ = false;
        }
    }
    if (atChoice && !choiceSeen_) autosaveRequested_ = true;
    choiceSeen_ = atChoice;
    if (player_.takeAutosaveRequest()) autosaveRequested_ = true;
    if (player_.takeGlobalSaveRequest() && !restartFlow_) saveGlobal();  // `end_of_kaisou`
    const auto& w = player_.stage().window;
    bool waiting = player_.state() == ScenePlayer::State::Input && w.revealed >= w.revealTotal();
    if (extraScreen_ && !systemScreen_) {  // closed some other way (a load, the title)
        extraScreen_ = false;
        window_->setSuppressed(false);
    }
    if (sceneRunning_) {
        for (const auto& label : player_.takeWindowCalls()) window_->callLabel(label);
        window_->update(w, waiting, player_.frame(), player_.waitingAtPage());
        if (window_->takeTextSkip()) {  // the window script's `str skip`
            if (player_.log && std::getenv("NYAN_FES_TRACE")) player_.log("window: str skip, the whole message shown");
            player_.revealAll();
        }
        // The paw (sscript 0x0c53e3, each frame outside touch mode once touch.fes is attached): up
        // while the story runs and some character bank can be touched.
        if (touchAttached_ && !touchMode()) {
            touchEnabled_ = !systemScreen_ && !loading() && !player_.touchBanks().empty();
            window_->setTouchEnable(touchEnabled_);
        }
        // Settings changed (options screen): the window re-reads them (opacity, frame, ...).
        if (engineConfig_ != notifiedConfig_) {
            notifiedConfig_ = engineConfig_;
            window_->configChanged();
        }
        window_->setScrollSpeed(messageFrame());
    }
    applyBusVolumes();
    dispatchMedia();
}

void Game::dispatchMedia() {
    using K = MediaCommand::Kind;
    using Bus = audio::Mixer::Bus;
    auto commands = player_.takeMedia();
    for (FesInstance* f : {flow_.get(), systemScreen_.get(), window_->instance()}) {
        if (!f) continue;
        for (auto& m : f->takeMedia()) commands.push_back(std::move(m));
    }
    for (const auto& m : commands) {
        if (m.kind == K::PlayMovie) {
            if (!startMovie(m.name, m.channel, m.volume)) player_.movieFinished();
            continue;
        }
        if (std::getenv("NYAN_MEDIA_TRACE") && player_.log)
            player_.log("MEDIA kind " + std::to_string(static_cast<int>(m.kind)) + " ch " + std::to_string(m.channel) + " " + m.name +
                        " vol " + std::to_string(m.volume) + " fade " + std::to_string(m.fadeFrames));
        if (!mixer_) continue;
        uint32_t ms = m.fadeFrames * 1000 / 60;
        auto play = [&](Bus bus) {
            auto ref = assets_.find(m.name + ".ogg");
            if (!ref) return;
            int group = bus == Bus::Bgm ? 0 : bus == Bus::Se ? 1 : 2;
            // The channel's `vol` level the scene left it at (the sound record's +164).
            const float level = std::clamp(m.volume, 0, 100) / 100.0f;
            if (soundConf_) {
                // With soundconf.xml the sound's part decides (sscript: a part whose play is off is
                // not started, 0x0a3c5b, native 944); its level is the gain (updateSoundGains).
                const PartPath& path = filePath(m.name);
                if (!path.empty() && !soundValue(*path.back().first, path.back().second, "play")) return;
                mixer_->play(bus, m.channel, ref->archive->read(*ref->entry), m.loop, 0, level, m.crossfadeFrames * 1000 / 60);
                startSound(static_cast<int>(bus), m.channel, m.name);
                if (std::getenv("NYAN_MEDIA_TRACE") && player_.log)
                    player_.log("MEDIA " + m.name + ": part " + (path.empty() ? std::string("none") : path.back().first->tag) +
                                " level " + std::to_string(partLevel(path, false)));
                return;
            }
            const VoiceEntry* v = voiceFor(m.name);
            mixer_->play(bus, m.channel, ref->archive->read(*ref->entry), m.loop, 0, volumeFor(group, v) * level,
                         m.crossfadeFrames * 1000 / 60);
            if (std::getenv("NYAN_MEDIA_TRACE") && player_.log)
                player_.log("MEDIA " + m.name + ": volume " + std::to_string(volumeFor(group, v) * level));
        };
        switch (m.kind) {
        case K::PlayBgm: play(Bus::Bgm); break;
        case K::PlaySe: play(Bus::Se); break;
        case K::PlayVoice:
            // Every other voice ends, but those asked for in the same frame (MediaCommand::spareVoices).
            if (m.spareVoices == 0) {
                mixer_->stop(Bus::Voice, -1);
            } else {
                for (int32_t c = 0; c < 32; ++c) {
                    if (!(m.spareVoices >> c & 1)) mixer_->stop(Bus::Voice, c);
                }
            }
            play(Bus::Voice);
            break;
        case K::StopBgm: mixer_->stop(Bus::Bgm, m.channel, ms); break;
        case K::StopSe: mixer_->stop(Bus::Se, m.channel, ms); break;
        case K::StopVoice: mixer_->stop(Bus::Voice, m.channel, ms); break;
        case K::BgmVolume:
            mixer_->setVolume(Bus::Bgm, m.channel, m.volume / 100.0f, ms, m.fromVolume >= 0 ? m.fromVolume / 100.0f : -1);
            break;
        case K::SeVolume:
            mixer_->setVolume(Bus::Se, m.channel, m.volume / 100.0f, ms, m.fromVolume >= 0 ? m.fromVolume / 100.0f : -1);
            break;
        case K::BgmPan: mixer_->setPan(Bus::Bgm, m.channel, m.volume); break;
        case K::BgmPause: mixer_->setPaused(Bus::Bgm, m.channel, m.loop); break;
        case K::SePause: mixer_->setPaused(Bus::Se, m.channel, m.loop); break;
        case K::VoicePause: mixer_->setPaused(Bus::Voice, m.channel, m.loop); break;
        case K::SePan: mixer_->setPan(Bus::Se, m.channel, m.volume); break;
        case K::VoicePan: mixer_->setPan(Bus::Voice, m.channel, m.volume); break;
        case K::VoiceVolume:
            mixer_->setVolume(Bus::Voice, m.channel, m.volume / 100.0f, ms, m.fromVolume >= 0 ? m.fromVolume / 100.0f : -1);
            break;
        case K::PlayMovie: break;  // handled above
        }
    }
}

std::vector<Game::FontCandidate> Game::fontCandidates() const {
    return fontCandidates(assets_, *folder_, fontDirs_, addedFonts_);
}

std::vector<Game::FontCandidate> Game::fontCandidates(const AssetIndex& assets, const GameFolder& folder,
                                                      const std::vector<std::string>& dirs,
                                                      const std::vector<std::string>& added) {
    auto isFont = [](const std::string& n) {
        std::string l = toLowerAscii(n);
        return l.ends_with(".ttf") || l.ends_with(".otf") || l.ends_with(".ttc");
    };
    std::vector<FontCandidate> candidates;
    for (const auto& archive : assets.archives()) {
        for (const auto& e : archive->entries()) {
            if (!isFont(e.name)) continue;
            const KifArchive* a = archive.get();
            const KifEntry* entry = &e;
            candidates.push_back({e.name, true, [a, entry] { return a->read(*entry); }});
        }
    }
    const GameFolder* f = &folder;
    for (const auto& n : folder.listDir("font")) {
        if (isFont(n)) candidates.push_back({n, true, [f, n] {
            auto file = f->open("font/" + n);
            return file ? file->readAll() : Bytes{};
        }});
    }
    for (const auto& n : folder.list()) {
        if (isFont(n)) candidates.push_back({n, false, [f, n] {
            auto file = f->open(n);
            return file ? file->readAll() : Bytes{};
        }});
    }
    // User font folders: system fonts the game expects from Windows (e.g. MS Gothic).
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string path = e.path().string();
            if (!e.is_regular_file() || !isFont(path)) continue;
            candidates.push_back({e.path().filename().string(), false, [path] {
                auto file = FdFile::open(path);
                return file ? file->readAll() : Bytes{};
            }});
        }
    }
    // Files a script added (`fontcom add`), relative to the game folder.
    for (const auto& n : added) {
        candidates.push_back({n, true, [f, n] {
            auto file = f->open(n);
            return file ? file->readAll() : Bytes{};
        }});
    }
    return candidates;
}

std::vector<std::string> Game::missingFonts(const GameFolder& folder, const std::vector<std::string>& fontDirs) {
    const AssetIndex assets = AssetIndex::load(folder);
    Bytes xml;
    if (auto file = folder.open("config/startup.xml")) xml = file->readAll();
    else if (assets.find("startup.xml")) xml = assets.read("startup.xml");
    std::vector<std::string> faces;
    if (!xml.empty()) {
        auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size())));
        const XmlNode* doc = root->child("document");
        if (const XmlNode* list = doc ? doc->find("FONT/face") : nullptr) {
            for (const auto& c : list->children) {
                if (!c->text.empty()) faces.push_back(c->text);
            }
        }
    }
    // Each face once (startup.xml may name one twice, f00 and f01).
    std::vector<std::string> unique;
    for (const auto& f : faces) {
        if (std::none_of(unique.begin(), unique.end(), [&](const std::string& u) { return fontNameEquals(u, f); })) unique.push_back(f);
    }
    faces = std::move(unique);
    if (faces.empty()) return {};
    // As loadFont finds the message font: any of the faces, else the engine's own default.ttf.
    for (const auto& c : fontCandidates(assets, folder, fontDirs, {})) {
        if (c.bundled && toLowerAscii(c.name) == "default.ttf") return {};
        const Bytes data = c.read();
        for (int face = 0; face < fontFaceCount(data); ++face) {
            for (const auto& want : faces) {
                if (faceMatchScore(data, face, want, 400) >= 0) return {};
            }
        }
    }
    return faces;
}

Bytes Game::loadFont(int* faceOut, const std::string& wanted, int32_t weight, bool* fakeBold) const {
    if (wanted.empty()) faceTried_ = true;
    if (fakeBold) *fakeBold = false;
    std::vector<FontCandidate> candidates = fontCandidates();
    if (candidates.empty()) return {};
    // Bold asked for (CreateFontIndirectW with lfWeight 700) of a face that is not bold: Windows
    // emboldens it (the renderer's FontAtlas fakeBold). Wine's rule: a weight above 550 and a face
    // without the bold style.
    const auto chose = [&](const Bytes& data, int face) {
        if (faceOut) *faceOut = face;
        if (fakeBold) *fakeBold = weight > 550 && !fontFamilyInfo(data, face).bold;
    };
    // The engine creates its fonts for the Shift-JIS character set (Happiness 2 cs2.exe 0x5b1aa0:
    // lfCharSet 0x80, the face ＭＳ ゴシック until one is given). A face without it (Happiness 2's
    // Arial) is not what Windows' font mapper gives then: it takes a Shift-JIS face instead, which
    // on the tester's PC drew the phone's English in MS Gothic's proportional faces (fitted: which
    // face the mapper picks depends on the system's fonts). The same here when one is installed;
    // otherwise the face asked for stays.
    const auto shiftJisFace = [&](Bytes& data, int& face) {
        if (data.empty() || (fontFamilyInfo(data, face).codePages & (1u << 17))) return;
        for (const char* want : {"ＭＳ Ｐゴシック", "MS PGothic", "MS UI Gothic"}) {
            for (const auto& c : candidates) {
                Bytes d = c.read();
                for (int f = 0; f < fontFaceCount(d); ++f) {
                    if (faceMatchScore(d, f, want, weight) < 0) continue;
                    if (player_.log) player_.log(std::string("font: ") + c.name + " (" + want + ", for a face without Shift-JIS)");
                    data = std::move(d);
                    face = f;
                    return;
                }
            }
        }
    };

    // A font a script names (`layout fontind 1 <file>` / `layout font <face>`): that file, else that
    // face; nothing if the game has neither (the caller keeps its current font).
    if (!wanted.empty()) {
        // Script text is Shift-JIS (Yukikoi's `layout font 源柔ゴシックX Bold`, read from a UTF-8 script
        // and kept as Shift-JIS like its other lines); font tables name their faces in UTF-8.
        const std::string face = isValidUtf8(wanted) ? wanted : sjisToUtf8(wanted);
        std::string w = toLowerAscii(face);
        for (const auto& c : candidates) {
            std::string n = toLowerAscii(c.name);
            if (n == w || n.ends_with("/" + w)) {
                Bytes data = c.read();  // a font file the script names (FreeType): as it is
                chose(data, 0);
                return data;
            }
        }
        Bytes best;
        int bestFace = 0, bestScore = -1;
        for (const auto& c : candidates) {
            Bytes data = c.read();
            for (int f = 0; f < fontFaceCount(data); ++f) {
                const int score = faceMatchScore(data, f, face, weight);
                if (score < 0 || (bestScore >= 0 && score >= bestScore)) continue;
                bestScore = score, bestFace = f, best = data;
            }
            if (bestScore == 0) break;
        }
        if (bestScore >= 0) {
            shiftJisFace(best, bestFace);
            chose(best, bestFace);
        }
        return best;
    }
    // 1. The face startup.xml asks for, by any of the font's names.
    std::vector<std::optional<Bytes>> loaded(candidates.size());
    for (const auto& want : fontFaces_) {
        size_t bestCandidate = 0;
        int bestFace = 0, bestScore = -1;
        for (size_t ci = 0; ci < candidates.size() && bestScore != 0; ++ci) {
            if (!loaded[ci]) loaded[ci] = candidates[ci].read();
            const Bytes& data = *loaded[ci];
            for (int face = 0; face < fontFaceCount(data); ++face) {
                const int score = faceMatchScore(data, face, want, weight);
                if (score < 0 || (bestScore >= 0 && score >= bestScore)) continue;
                bestScore = score, bestFace = face, bestCandidate = ci;
            }
        }
        if (bestScore < 0) continue;
        if (player_.log) player_.log("font: " + candidates[bestCandidate].name + " (" + want + (weight > 550 ? ", bold" : "") + ")");
        Bytes data = *loaded[bestCandidate];
        shiftJisFace(data, bestFace);
        chose(data, bestFace);
        resolvedFace_ = want;
        return data;
    }
    // 2. A system font we don't have: the engine's own default.ttf if the game ships one. Otherwise
    // the platform's font stands in (closer to a Windows system font than an unrelated bundled one).
    for (const auto& c : candidates) {
        if (c.bundled && toLowerAscii(c.name) == "default.ttf") {
            if (player_.log) player_.log("font: " + c.name + " (engine default)");
            Bytes data = c.read();
            chose(data, 0);
            return data;
        }
    }
    if (player_.log) player_.log("font: configured face not found; using the platform font");
    return {};
}

std::vector<std::string> Game::fontList(int32_t charset) const {
    // `fontcom init [charset]` (cs2.exe 0x68fb40 -> 0x5b13d0): Windows' font families of that
    // charset (0x80 Shift-JIS when not given), enumerated twice (a count, then the names; the
    // callbacks 0x5b14b0 / 0x5b1500), leaving out vertical "@" faces and, unless startup.xml's
    // FONT/proportional is on (it is by default, settings +0x101d8 = 1), the proportional ones; for
    // Shift-JIS only TrueType faces. Nyanroid's installed fonts are the files it can draw: the
    // game's, its font folders' (Nyanroid's own MS Gothic among them) and those `fontcom add` gave.
    // Windows lists them in its own order; here by name, each family once.
    static const std::map<int32_t, int> kCodePageBit = {{0, 0}, {128, 17}, {134, 18}, {129, 19}, {136, 20}};
    const auto bit = kCodePageBit.find(charset);
    if (charset != -1 && bit == kCodePageBit.end()) return {};
    const std::string key = std::to_string(charset) + "/" + std::to_string(addedFonts_.size());
    if (auto it = fontLists_.find(key); it != fontLists_.end()) return it->second;
    std::vector<std::string> out;
    for (const auto& c : fontCandidates()) {
        const Bytes data = c.read();
        for (int f = 0; f < fontFaceCount(data); ++f) {
            if (!fontRenderable(data, f)) continue;
            const FontFamilyInfo info = fontFamilyInfo(data, f);
            if (info.family.empty() || info.family[0] == '@') continue;
            if (charset != -1 && !(info.codePages & (1u << bit->second))) continue;
            if (!fontProportional_ && !info.fixedPitch) continue;
            if (std::find(out.begin(), out.end(), info.family) == out.end()) out.push_back(info.family);
        }
    }
    std::sort(out.begin(), out.end());
    return fontLists_.emplace(key, std::move(out)).first->second;
}

void Game::addFontFile(const std::string& file) {
    // `fontcom add <file>` (AddFontResourceA): a font file, a path in the game folder.
    std::string n = file;
    std::replace(n.begin(), n.end(), '\\', '/');
    if (std::find(addedFonts_.begin(), addedFonts_.end(), n) == addedFonts_.end()) addedFonts_.push_back(n);
}

std::string Game::defaultFontFace() const {
    // `fontcom default` (+0xfcb4 of the startup settings): the face the message font came from.
    if (!faceTried_) loadFont();
    if (!resolvedFace_.empty()) return resolvedFace_;
    return fontFaces_.empty() ? std::string() : fontFaces_.front();
}

const std::string& Game::configFont() const {
    // getconf / setconf `font` (+0xfcb4 of the running settings): the startup face until changed.
    if (!configFontSet_) configFont_ = defaultFontFace(), configFontSet_ = true;
    return configFont_;
}

void Game::setConfigFont(const std::string& face) {
    configFont_ = face;
    configFontSet_ = true;
}

}  // namespace nyan::cs2
