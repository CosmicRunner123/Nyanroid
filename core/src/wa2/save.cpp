// Saves (WA2.exe 0x424260 write, 0x424310 load, 0x423330 header, 0x423060 / 0x4234c0 capture,
// 0x423d10 restore), in Nyanroid's own file: save_NN.nsv / save_Q.nsv in the save folder.

#include "nyan/wa2/save.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <zlib.h>

#include "nyan/common/error.hpp"
#include "nyan/common/zlib_util.hpp"
#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

constexpr char kMagic[8] = {'N', 'Y', 'W', 'A', '2', 'S', 'A', 'V'};
constexpr uint32_t kVersion = 2;  // 1: without SaveRecord::timer

void writeChar(Writer& w, const SaveRecord::Char& c) {
    w.u8(c.used);
    w.i32(c.id);
    w.i32(c.pic);
    w.u8(c.posFloat);
    w.i32(c.posIndex);
    w.f32(c.pos);
    w.i32(c.z);
    w.i32(c.r);
    w.i32(c.alpha);
}

void readChar(Reader& r, SaveRecord::Char& c) {
    c.used = r.u8() != 0;
    c.id = r.i32();
    c.pic = r.i32();
    c.posFloat = r.u8() != 0;
    c.posIndex = r.i32();
    c.pos = r.f32();
    c.z = r.i32();
    c.r = r.i32();
    c.alpha = r.i32();
}

// The script global `name`'s value as 0x4509c0 reads it (0 if there is none).
int globalValue(const std::vector<GlobalVar>& globals, const std::string& name) {
    for (const GlobalVar& g : globals) {
        if (g.name == name) return g.type == 4 ? static_cast<int>(g.f) : g.i;
    }
    return 0;
}

}  // namespace

void SaveRecord::write(Writer& w) const {
    w.u8(shake.on);
    w.i32(shake.dir);
    w.i32(shake.amp);
    w.i32(shake.type);
    w.i32(shake.speed);
    w.u8(scene.shown);
    w.i32(scene.number);
    w.i32(scene.kind);
    w.i32(scene.panX);
    w.i32(scene.panY);
    w.f32(scene.zoomX);
    w.f32(scene.zoomY);
    w.i32(scene.rotation);
    w.i32(scene.timeMode);
    w.i32(scene.bgLevel);
    w.i32(scene.charLevel);
    w.str(scene.bgAmp);
    w.str(scene.bgEffect);
    w.str(scene.charEffect);
    for (int v : scene.tone) w.i32(v);
    for (const Char& c : scene.chars) writeChar(w, c);
    w.u8(message.window);
    w.i32(message.kind);
    w.i32(message.page);
    w.i32(message.reveal);
    w.i32(message.line);
    w.i32(message.novel);
    w.u8(message.nameShown);
    w.str(message.name);
    w.str(message.text);
    w.u8(choices.shown);
    w.i32(choices.id);
    w.i32(choices.script);
    w.i32(choices.count);
    for (const Choice& c : choices.options) {
        w.str(c.text);
        w.i32(c.flag);
        w.i32(c.value);
        w.i32(c.always);
    }
    w.u8(weather.on);
    w.i32(weather.type);
    w.i32(weather.wind);
    w.i32(weather.fall);
    w.i32(weather.count);
    w.i32(weather.mode);
    for (const Bgm& b : bgm) {
        w.i32(b.track);
        w.i32(b.state);
        w.i32(b.loop);
        w.i32(b.vol);
    }
    for (const Sep& s : sep) {
        w.u8(s.inUse);
        w.i32(s.num);
        w.i32(s.loop);
        w.i32(s.vol);
    }
    for (const Voice& v : voices) {
        w.u8(v.on);
        w.i32(v.chara);
        w.i32(v.scene);
        w.i32(v.num);
        w.i32(v.vol);
        w.i32(v.base);
    }
    w.u8(eroMode);
    w.i32(viScene);
    w.i32(vaOffset);
    w.i32(voiceSpeaker);
    for (const Picture& p : pictures) {
        w.u8(p.used);
        w.u8(p.disp);
        w.i32(p.kind);
        w.str(p.file);
        w.str(p.amp);
        w.str(p.group);
        w.i32(p.colourMode);
        w.i32(p.level);
        w.i32(p.blend);
        w.i32(p.mirror);
        w.i32(p.alpha);
        w.i32(p.r);
        w.i32(p.g);
        w.i32(p.b);
        w.i32(p.layer);
        w.i32(p.fixed);
        for (int v : {p.x, p.y, p.w, p.h, p.sx, p.sy, p.fullW, p.fullH}) w.i32(v);
        w.f32(p.zoom);
        w.i32(p.cx);
        w.i32(p.cy);
    }
    w.i32(year);
    w.i32(month);
    w.i32(day);
    w.i32(weekday);
    w.i32(timer);
}

bool SaveRecord::read(Reader& r, uint32_t version) {
    try {
        shake.on = r.u8() != 0;
        shake.dir = r.i32();
        shake.amp = r.i32();
        shake.type = r.i32();
        shake.speed = r.i32();
        scene.shown = r.u8() != 0;
        scene.number = r.i32();
        scene.kind = r.i32();
        scene.panX = r.i32();
        scene.panY = r.i32();
        scene.zoomX = r.f32();
        scene.zoomY = r.f32();
        scene.rotation = r.i32();
        scene.timeMode = r.i32();
        scene.bgLevel = r.i32();
        scene.charLevel = r.i32();
        scene.bgAmp = r.str();
        scene.bgEffect = r.str();
        scene.charEffect = r.str();
        for (int& v : scene.tone) v = r.i32();
        for (Char& c : scene.chars) readChar(r, c);
        message.window = r.u8() != 0;
        message.kind = r.i32();
        message.page = r.i32();
        message.reveal = r.i32();
        message.line = r.i32();
        message.novel = r.i32();
        message.nameShown = r.u8() != 0;
        message.name = r.str();
        message.text = r.str();
        choices.shown = r.u8() != 0;
        choices.id = r.i32();
        choices.script = r.i32();
        choices.count = r.i32();
        for (Choice& c : choices.options) {
            c.text = r.str();
            c.flag = r.i32();
            c.value = r.i32();
            c.always = r.i32();
        }
        weather.on = r.u8() != 0;
        weather.type = r.i32();
        weather.wind = r.i32();
        weather.fall = r.i32();
        weather.count = r.i32();
        weather.mode = r.i32();
        for (Bgm& b : bgm) {
            b.track = r.i32();
            b.state = r.i32();
            b.loop = r.i32();
            b.vol = r.i32();
        }
        for (Sep& s : sep) {
            s.inUse = r.u8() != 0;
            s.num = r.i32();
            s.loop = r.i32();
            s.vol = r.i32();
        }
        for (Voice& v : voices) {
            v.on = r.u8() != 0;
            v.chara = r.i32();
            v.scene = r.i32();
            v.num = r.i32();
            v.vol = r.i32();
            v.base = r.i32();
        }
        eroMode = r.u8() != 0;
        viScene = r.i32();
        vaOffset = r.i32();
        voiceSpeaker = r.i32();
        for (Picture& p : pictures) {
            p.used = r.u8() != 0;
            p.disp = r.u8() != 0;
            p.kind = r.i32();
            p.file = r.str();
            p.amp = r.str();
            p.group = r.str();
            p.colourMode = r.i32();
            p.level = r.i32();
            p.blend = r.i32();
            p.mirror = r.i32();
            p.alpha = r.i32();
            p.r = r.i32();
            p.g = r.i32();
            p.b = r.i32();
            p.layer = r.i32();
            p.fixed = r.i32();
            for (int* v : {&p.x, &p.y, &p.w, &p.h, &p.sx, &p.sy, &p.fullW, &p.fullH}) *v = r.i32();
            p.zoom = r.f32();
            p.cx = r.i32();
            p.cy = r.i32();
        }
        year = r.i32();
        month = r.i32();
        day = r.i32();
        weekday = r.i32();
        timer = version >= 2 ? r.i32() : 0;
        return true;
    } catch (const Error&) {
        return false;
    }
}

std::string saveExcerpt(const std::string& message) {
    // 0x423060: the buffer copied (1024 bytes), a leading "　" skipped, 26 bytes taken; then the walk.
    std::string src = message.substr(0, 0x400);
    if (src.size() >= 2 && static_cast<uint8_t>(src[0]) == 0x81 && static_cast<uint8_t>(src[1]) == 0x40) src.erase(0, 2);
    std::array<char, 0x40> out{};
    for (size_t i = 0; i < 0x1a && i < src.size(); ++i) out[i] = src[i];
    int open = 0;
    size_t i = 0;
    bool cut = false;
    while (true) {
        char c = out[i];
        if (c == '\0' || (c == '\\' && out[i + 1] == 'n') || c == '\n') break;
        auto u = static_cast<uint8_t>(c);
        // The engine's test for a double-byte lead: 0x80..0xb0 or 0xdd..0xff (signed-char ranges).
        bool lead = u >= 0x80 && (u <= 0xb0 || u >= 0xdd);
        if (lead) {
            if (i > 0x16) {
                cut = true;
                break;
            }
            i += 2;
        } else {
            if (c == '<') ++open;
            if (c == '>') --open;
            if (i > 0x17) {
                cut = true;
                break;
            }
            i += 1;
        }
    }
    for (int k = 0; k < open; ++k) out[i++] = '>';
    std::string s(out.data(), i);
    if (cut) s += "\x81\x63";  // …
    return s;
}

// ---- Game -------------------------------------------------------------------------------------------

std::string Game::savePath(int slot) const {
    if (saveDir_.empty()) return {};
    char name[32];
    if (slot < 0) std::snprintf(name, sizeof name, "save_Q.nsv");
    else std::snprintf(name, sizeof name, "save_%02d.nsv", slot);
    return (std::filesystem::path(saveDir_) / name).string();
}

int Game::saveSortKey() const {
    // 0x423060: FLG_章管理サブ + (FLG_00 + (month * 100 + day) * 10) * 10. Special Contents (0x422540):
    // (FLG_章管理サブ * 5 + 30) * 2, its load screen's row (key / 10) of sys_e1016.
    int sub = globalValue(globals_, "FLG_\x8f\xcd\x8a\xc7\x97\x9d\x83\x54\x83\x75");
    if (assets_.build() == Build::Special) return (sub * 5 + 0x1e) * 2;
    int flg = globalValue(globals_, "FLG_00");
    int date = effects_.calendarMonth() * 100 + effects_.calendarDay();
    return sub + (flg + date * 10) * 10;
}

bool Game::writeSave(int slot) {
    // 0x424260: the header (0x423060), the record (0x4234c0), then the scripts (0x417670).
    if (!vm_) return false;
    std::string path = savePath(slot);
    if (path.empty()) return false;
    Writer w;
    for (char c : kMagic) w.u8(static_cast<uint8_t>(c));
    w.u32(kVersion);
    w.i32(saveSortKey());
    std::time_t now = std::time(nullptr);
    std::tm t{};
#if defined(_WIN32)
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    std::array<uint16_t, 8> st = {static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint16_t>(t.tm_mon + 1),
                                  static_cast<uint16_t>(t.tm_wday), static_cast<uint16_t>(t.tm_mday),
                                  static_cast<uint16_t>(t.tm_hour), static_cast<uint16_t>(t.tm_min),
                                  static_cast<uint16_t>(t.tm_sec), 0};
    for (uint16_t v : st) w.u32(v);
    w.str(saveExcerpt(message_.text()));
    // The thumbnail (0xa61a90), deflated.
    RgbaImage thumb = thumbnail;
    if (thumb.width != 128 || thumb.height != 128 || thumb.pixels.size() != 128u * 128 * 4) {
        thumb = RgbaImage{128, 128, Bytes(128u * 128 * 4, 0)};
    }
    uLongf bound = compressBound(static_cast<uLong>(thumb.pixels.size()));
    Bytes packed(bound);
    if (compress2(packed.data(), &bound, thumb.pixels.data(), static_cast<uLong>(thumb.pixels.size()), Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    packed.resize(bound);
    w.u32(static_cast<uint32_t>(thumb.width));
    w.u32(static_cast<uint32_t>(thumb.height));
    w.bytes(packed);
    captureRecord().write(w);
    // 0x452310: the globals' values; 0x4523e0: the script objects.
    w.u32(static_cast<uint32_t>(globals_.size()));
    for (const GlobalVar& g : globals_) {
        w.str(g.name);
        w.i32(g.type);
        w.i32(g.i);
        w.f32(g.f);
        w.str(g.str);
    }
    vm_->save(w);
    Bytes data = w.take();
    std::error_code ec;
    std::filesystem::create_directories(saveDir_, ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            log(path + ": cannot write");
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        log(path + ": " + ec.message());
        return false;
    }
    return true;
}

std::optional<Bytes> Game::readSaveFile(int slot) const {
    std::string path = savePath(slot);
    if (path.empty()) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), kMagic, 8) != 0) return std::nullopt;
    return data;
}

namespace {

// The header of a save file; the reader is left at the record.
bool readHeader(Reader& r, SaveInfo& info, bool thumbnail) {
    for (int i = 0; i < 8; ++i) r.u8();
    info.version = r.u32();
    if (info.version < 1 || info.version > kVersion) return false;
    info.sortKey = r.i32();
    for (uint16_t& v : info.time) v = static_cast<uint16_t>(r.u32());
    info.text = r.str();
    int w = static_cast<int>(r.u32()), h = static_cast<int>(r.u32());
    Bytes packed = r.bytes();
    if (thumbnail && w > 0 && h > 0 && w <= 512 && h <= 512) {
        info.thumbnail = RgbaImage{w, h, zlibInflate(packed, static_cast<size_t>(w) * static_cast<size_t>(h) * 4)};
    }
    return true;
}

}  // namespace

std::optional<SaveInfo> Game::readSaveInfo(int slot, bool thumbnail) const {
    // 0x423330 / 0x423230.
    auto data = readSaveFile(slot);
    if (!data) return std::nullopt;
    try {
        Reader r(*data);
        SaveInfo info;
        if (!readHeader(r, info, thumbnail)) return std::nullopt;
        return info;
    } catch (const Error&) {
        return std::nullopt;
    }
}

bool Game::loadSave(int slot) {
    // 0x424310: the record restored (0x423d10), then a new VM with the globals and the scripts
    // (0x417760), the current script entered, the saved line marked read (0x405180).
    auto data = readSaveFile(slot);
    if (!data) {
        log("save " + std::to_string(slot) + ": cannot be read");
        return false;
    }
    SaveRecord rec;
    std::vector<GlobalVar> saved;
    Bytes vmState;
    try {
        Reader r(*data);
        SaveInfo info;
        if (!readHeader(r, info, false) || !rec.read(r, info.version)) {
            log("save " + std::to_string(slot) + ": unreadable");
            return false;
        }
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) {
            GlobalVar g;
            g.name = r.str();
            g.type = r.i32();
            g.i = r.i32();
            g.f = r.f32();
            g.str = r.str();
            saved.push_back(std::move(g));
        }
        // The rest is the VM's.
        Writer rest;
        while (!r.done()) rest.u8(r.u8());
        vmState = rest.take();
    } catch (const Error&) {
        log("save " + std::to_string(slot) + ": unreadable");
        return false;
    }
    restoreRecord(rec);
    vm_ = std::make_unique<ScriptVm>(*this, "script");
    loadGlobals();
    for (GlobalVar& g : globals_) {
        for (const GlobalVar& s : saved) {
            if (s.name == g.name && s.type == g.type) {
                g.i = s.i;
                g.f = s.f;
                g.str = s.str;
                break;
            }
        }
    }
    Reader vr(vmState);
    if (!vm_->restore(vr)) {
        log("save " + std::to_string(slot) + ": the scripts cannot be restored");
        vm_.reset();
        return false;
    }
    titleRequested_ = false;
    mode_ = Mode::Story;
    lastScript_ = currentScript();
    enterScript(lastScript_);
    markLine(rec.message.line);
    return true;
}

SaveRecord Game::captureRecord() const {
    // 0x4234c0
    SaveRecord rec;
    effects_.captureShake(rec.shake);
    scene_.capture(rec.scene);
    message_.capture(rec.message);
    rec.message.line = currentLine;
    // 0x404dc0
    rec.choices.shown = message_.choicesShown();
    rec.choices.id = choiceId;
    rec.choices.script = choiceScript;
    rec.choices.count = static_cast<int>(choiceOptions.size());
    for (size_t i = 0; i < choiceOptions.size() && i < 8; ++i) {
        const ChoiceOption& o = choiceOptions[i];
        rec.choices.options[i] = {o.text.substr(0, 0xff), static_cast<int8_t>(o.flag), static_cast<int8_t>(o.value),
                                  static_cast<int8_t>(o.always)};
    }
    effects_.captureWeather(rec.weather);
    sound_->capture(rec);
    effects_.capturePictures(rec.pictures);
    effects_.captureCalendar(rec.year, rec.month, rec.day, rec.weekday);
    if (assets_.build() == Build::Special) rec.timer = static_cast<int>(milliseconds() - timerStart);
    return rec;
}

void Game::restoreRecord(const SaveRecord& rec) {
    // 0x423d10, in its order.
    modes.autoMode = false;
    message_.resetAutoCount();
    modes.skip = false;
    scene_.restore(rec.scene);
    effects_.stopShake();
    if (rec.shake.on) effects_.shake(rec.shake.type, rec.shake.amp, 0, rec.shake.dir, rec.shake.speed);
    message_.restore(rec.message);
    // 0x405080
    message_.clearChoices();
    if (rec.choices.shown) {
        for (int i = 0; i < rec.choices.count && i < 8; ++i) {
            const SaveRecord::Choice& c = rec.choices.options[static_cast<size_t>(i)];
            addChoice(c.text, c.flag, c.value, c.always);
        }
        choiceShown = true;
        message_.showChoices(rec.choices.id, rec.choices.script);
    }
    if (rec.weather.on) {
        effects_.weather(rec.weather.type | 0x100, rec.weather.wind, rec.weather.fall, 0, rec.weather.count, 2, rec.weather.mode);
    } else {
        effects_.resetWeather(true);
    }
    sound_->restore(rec);
    if (assets_.build() == Build::Special) timerStart = milliseconds() - static_cast<uint32_t>(rec.timer);  // StartTimer(-timer)
    for (int i = 0; i < 32; ++i) effects_.restorePicture(i, rec.pictures[static_cast<size_t>(i)]);
    effects_.calendar(rec.year, rec.month, rec.day, rec.weekday, 0);
}

}  // namespace nyan::wa2
