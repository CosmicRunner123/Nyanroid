#include "nyan/diesel/game_data.hpp"

#include <cstring>

#include "nyan/common/sjis.hpp"

namespace nyan::diesel {

bool isGameFolder(const GameFolder& folder, const std::vector<std::string>& names) {
    bool system = false;
    std::string script;
    for (const auto& n : names) {
        std::string l = toLowerAscii(n);
        if (l == "system.npk") system = true;
        if (l == "script.npk") script = n;
    }
    if (!system || script.empty()) return false;
    auto f = folder.open(script);
    if (!f || f->size() < 8) return false;
    return isNpkArchive(*f);  // NPK3, or NPK2 (The Song of Saya)
}

namespace {

// CNecroExConfigSerializer's writer (vtable slot 6) writes its tag and then each option with a
// length of 4 for CFile::Write (32-bit exes: `push 4`, 6A 04; 64-bit: `mov r8d, 4`, 41 B8 04 00 00
// 00); its reader (slot 7) follows it. The tag is the first string the writer references (32-bit:
// `push imm32`, 68; 64-bit: `lea rdx, [rip + d]`, 48 8D 15).
void readLauncherFormat(const ExeInfo& exe, std::string& tag, int& count) {
    auto vt = exe.vtable(".?AVCNecroExConfigSerializer@@");
    if (!vt) return;
    size_t ptr = exe.pointerSize();
    auto writer = exe.readPointer(*vt + 6 * ptr), reader = exe.readPointer(*vt + 7 * ptr);
    if (!writer || !reader || *reader <= *writer || *reader - *writer > 0x400) return;
    auto from = exe.offsetOf(*writer), to = exe.offsetOf(*reader);
    if (!from || !to) return;
    auto stringAt = [&](uint64_t va) -> std::string {
        auto o = exe.offsetOf(va);
        if (!o) return {};
        std::string s;
        for (size_t i = *o; i < exe.image.size() && exe.image[i] != 0 && s.size() < 32; ++i) {
            if (exe.image[i] < 0x20 || exe.image[i] > 0x7e) return {};
            s.push_back(static_cast<char>(exe.image[i]));
        }
        return s;
    };
    int lengths = 0;
    std::string found;
    for (size_t o = *from; o + 6 < *to; ++o) {
        const uint8_t* c = &exe.image[o];
        if (exe.is64) {
            if (c[0] == 0x41 && c[1] == 0xb8 && readLE32(c + 2) == 4) ++lengths;
            if (found.empty() && c[0] == 0x48 && c[1] == 0x8d && c[2] == 0x15) {
                auto at = exe.addressOf(o);
                if (at) found = stringAt(*at + 7 + static_cast<int64_t>(static_cast<int32_t>(readLE32(c + 3))));
            }
        } else {
            if (c[0] == 0x6a && c[1] == 0x04) ++lengths;
            if (found.empty() && c[0] == 0x68) found = stringAt(readLE32(c + 1));
        }
    }
    if (lengths > 0) count = lengths;
    if (!found.empty()) tag = found;
}

// GameData::xaudioVersion (29 unless Mware.dll is there and imports X3DAudio1_7.dll without
// XAudio2_8.dll) and xaudio64 (Mware.dll is PE32+; without it, the exe's bitness).
void readXAudio(const GameFolder& folder, GameData& g) {
    g.xaudio64 = g.exe.is64;
    auto dll = readPe(folder, "Mware.dll");
    if (!dll) return;
    g.xaudio64 = dll->is64;
    bool x28 = false, x3d17 = false;
    for (const auto& n : dll->imports()) {
        std::string l = toLowerAscii(n);
        x28 = x28 || l == "xaudio2_8.dll";
        x3d17 = x3d17 || l == "x3daudio1_7.dll";
    }
    g.xaudioVersion = x3d17 && !x28 ? 27 : 29;
}

}  // namespace

GameData GameData::open(const GameFolder& folder) {
    GameData g;
    g.folder = &folder;
    auto exe = findExe(folder);
    if (!exe) fail(folder.displayName() + ": no Diesel Engine executable (an exe importing Mware.dll)");
    g.exe = std::move(*exe);
    auto probe = folder.open("system.npk");
    if (!probe) fail(folder.displayName() + ": no system.npk");
    g.key = findArchiveKey(g.exe, *probe);
    g.archives = std::make_unique<Archives>(folder, g.key);
    auto ini = g.archives->read("setting.ini");
    if (!ini) fail(folder.displayName() + ": no setting.ini in system.npk");
    g.settings = std::make_unique<SettingIni>(*ini);
    readLauncherFormat(g.exe, g.launcherTag, g.launcherOptionCount);
    readXAudio(folder, g);
    return g;
}

int GameData::screenWidth() const { return settings->getInt("Init", "SCREEN_WIDTH", 1024); }
int GameData::screenHeight() const { return settings->getInt("Init", "SCREEN_HEIGHT", 576); }

std::string GameData::windowName() const {
    auto v = settings->get("Init", "WINDOW_NAME");
    return v ? sjisToUtf8(*v) : std::string();
}

bool GameData::utf8() const {
    auto v = settings->get("Init", "CODE_PAGE");
    return v && toLowerAscii(*v) == "utf8";
}

}  // namespace nyan::diesel
