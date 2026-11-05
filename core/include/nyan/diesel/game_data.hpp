#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/diesel/archives.hpp"
#include "nyan/diesel/exe_info.hpp"
#include "nyan/diesel/setting_ini.hpp"

namespace nyan::diesel {

// A Diesel Engine game's install: its exe, the archive key read from it, the mounted archives and
// system.npk's setting.ini.
struct GameData {
    const GameFolder* folder = nullptr;  // the install (must outlive this)
    ExeInfo exe;
    NpkKey key{};
    std::unique_ptr<Archives> archives;
    std::unique_ptr<SettingIni> settings;
    // The launcher's part of system.dat, from the exe's CNecroExConfigSerializer writer: its tag
    // ("JINGAIEX" in Muramasa and Totono, "TOGAINUEX" in Phantom of Inferno; written with its NUL)
    // and how many 4-byte options follow (2 in Muramasa, 3 in Totono, whose third is SetClearTrue's
    // flag, and 3 in Phantom).
    std::string launcherTag = "JINGAIEX";
    int launcherOptionCount = 2;
    // The XAudio2 that the game's Mware.dll plays its sounds with, from the DLLs it imports: 29 for
    // one importing XAudio2_8.dll (Muramasa, Phantom of Inferno; on Windows 10 and later a front
    // for XAudio2_9.dll, whose reverb runs it), 27 for one importing the DirectX SDK's
    // X3DAudio1_7.dll instead (Totono, whose Mware makes XAudio2 2.7 and its reverb through COM).
    int xaudioVersion = 29;
    // Mware.dll is x86-64 (Phantom of Inferno), so the XAudio2 it loads is too (System32's
    // XAudio2_9.dll, whose processing differs from the 32-bit build's).
    bool xaudio64 = false;

    // Throws nyan::Error if the folder is not a Diesel game or its archives do not open.
    static GameData open(const GameFolder& folder);

    // setting.ini [Init] SCREEN_WIDTH / SCREEN_HEIGHT (1024 x 576 in both games).
    int screenWidth() const;
    int screenHeight() const;
    // [Init] WINDOW_NAME, decoded from Shift-JIS.
    std::string windowName() const;
    // [Init] CODE_PAGE: true for "UTF8" (Totono), false for "SJIS" (Muramasa).
    bool utf8() const;
};

// detectEngine()'s check: script.npk and system.npk in the folder, script.npk an NPK3 or NPK2 archive.
bool isGameFolder(const GameFolder& folder, const std::vector<std::string>& names);

}  // namespace nyan::diesel
