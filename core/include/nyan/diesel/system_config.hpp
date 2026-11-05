#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// The app's system state (muramasa_en.exe CAppSystemState at app+0xec: window and graphics
// settings) and the launcher's options (CNecroExConfigSerializer at app+0x134), kept in the
// save root's system.dat. Field comments give the exe's offsets.
struct SystemConfig {
    int32_t screenWidth = 1024;       // +0x38 (the constructor's: setting.ini SCREEN_WIDTH)
    int32_t screenHeight = 576;       // +0x3c
    int32_t fullScreen = 0;           // +0x40
    int32_t fixedAspect = 1;          // +0x44
    int32_t frameRate = 60;           // +0x48
    int32_t antiAliasType = 4;        // +0x4c
    int32_t sceneDegreeSizeRatio = 2; // +0x50
    int32_t useSoftFocus = 1;         // +0x54
    int32_t useHDR = 1;               // +0x58
    int32_t useToneMapping = 1;       // +0x5c
    int32_t useDOF = 1;               // +0x60
    int32_t useFog = 1;               // +0x64 (totono_en.exe's setFogEnable 0x511240 pairs it with the fog's switch)
    float unknown68 = 0.02f;          // +0x68
    int32_t useShadow = 1;            // +0x6c
    int32_t useSelfShadow = 1;        // +0x70
    int32_t shadowQuality = 1;        // +0x74
    int32_t pointLightNum = 2;        // +0x78
    int32_t unknown7c = 1;            // +0x7c
    int32_t unknown80 = 1;            // +0x80
    // The scene's effects as SQScene switches them (setSoftFocusEnable ... setFogEnable): 0 from the
    // constructor, not in system.dat (phantom.exe's +0x94 .. +0xa8).
    int32_t softFocusEnable = 0;      // +0x84
    int32_t hdrBloomEnable = 0;       // +0x8c
    int32_t toneMappingEnable = 0;    // +0x90
    int32_t dofEnable = 0;            // +0x94
    int32_t fogEnable = 0;            // +0x98
    int32_t userSetting = 0;          // +0x20: set when system.dat was read
    int32_t dirty = 0;                // +0x30: the frame loop applies the settings before the next frame
    // CNecroExConfigSerializer +0x20, +0x24 (IsEcoMode reads it), ... (GameData::launcherOptionCount
    // of them; the constructor's values 1, 0, 0).
    std::vector<int32_t> launcherOptions = {1, 0};
    std::string launcherTag = "JINGAIEX";  // GameData::launcherTag

    // The launcher options as many as the exe keeps.
    void setLauncherOptionCount(int count);

    // system.dat as 0x42f320 writes it: "DIESEL\0", version 0x101, the fields (0x444f80), then
    // the launcher's tag with its NUL and options (muramasa_en.exe 0x401650, totono_en.exe 0x401680,
    // phantom.exe 0x140001750).
    Bytes serialize() const;
    // 0x42f490: reads the fields (0x445250; a bad header or short file leaves the fields read so far
    // and sets its fallbacks), then the launcher's options if the fields were read.
    void deserialize(ByteSpan file);
    // applyPresetConfig (0x444d90): quality presets 0 (high) to 3 (low); others change nothing.
    void applyPreset(int preset);
    // What the launcher dialog's OK leaves (muramasa_en.exe 0x4067d0): the 3D options off.
    void launcherDefaults();
};

}  // namespace nyan::diesel
