#include "nyan/diesel/system_config.hpp"

#include <cstring>

namespace nyan::diesel {

namespace {

void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putF(Bytes& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}

// The file read as Mware's CFile::Read gives it: whole reads only.
struct Reader {
    ByteSpan d;
    size_t at = 0;
    bool take(void* out, size_t n) {
        if (at + n > d.size()) return false;
        std::memcpy(out, d.data() + at, n);
        at += n;
        return true;
    }
    // A C string compare of the n bytes read against `tag` (the exes' strcmp on a zeroed buffer).
    bool tag(const char* t, size_t n) {
        char buf[33] = {};
        if (!take(buf, n)) return false;
        return std::strcmp(buf, t) == 0;
    }
};

constexpr uint32_t kVersion = 0x101;  // muramasa_en.exe 0x5e1d8c; the reader requires it

}  // namespace

Bytes SystemConfig::serialize() const {
    Bytes b;
    const char* magic = "DIESEL";
    b.insert(b.end(), magic, magic + 7);
    put32(b, kVersion);
    put32(b, static_cast<uint32_t>(screenWidth));
    put32(b, static_cast<uint32_t>(screenHeight));
    for (int32_t v : {fullScreen, fixedAspect, frameRate, antiAliasType, sceneDegreeSizeRatio, useSoftFocus, useHDR,
                      useToneMapping, useDOF})
        put32(b, static_cast<uint32_t>(v));
    put32(b, 1);  // a constant the reader skips
    put32(b, static_cast<uint32_t>(useFog));
    putF(b, unknown68);
    for (int32_t v : {useShadow, useSelfShadow, shadowQuality, pointLightNum, unknown7c, unknown80})
        put32(b, static_cast<uint32_t>(v));
    b.insert(b.end(), launcherTag.begin(), launcherTag.end());
    b.push_back(0);
    for (int32_t o : launcherOptions) put32(b, static_cast<uint32_t>(o));
    return b;
}

void SystemConfig::deserialize(ByteSpan file) {
    Reader r{file};
    bool ok = [&] {
        uint32_t version = 0;
        if (!r.tag("DIESEL", 7) || !r.take(&version, 4) || version != kVersion) return false;
        int32_t skipped = 0;
        int32_t* fields[] = {&screenWidth, &screenHeight, &fullScreen, &fixedAspect, &frameRate, &antiAliasType,
                             &sceneDegreeSizeRatio, &useSoftFocus, &useHDR, &useToneMapping, &useDOF, &skipped,
                             &useFog};
        for (int32_t* f : fields) {
            if (!r.take(f, 4)) return false;
        }
        if (!r.take(&unknown68, 4)) return false;
        for (int32_t* f : {&useShadow, &useSelfShadow, &shadowQuality, &pointLightNum, &unknown7c, &unknown80}) {
            if (!r.take(f, 4)) return false;
        }
        return true;
    }();
    if (!ok) {
        // 0x445250's fallbacks.
        frameRate = 60;
        useSoftFocus = 1;
        antiAliasType = 2;
        sceneDegreeSizeRatio = 2;
        useShadow = 1;
        useSelfShadow = 1;
        shadowQuality = 2;
        useHDR = 0;
        useToneMapping = 0;
        useDOF = 1;
        useFog = 1;
        pointLightNum = 4;
        unknown68 = 4.0f;
        unknown7c = 1;
        unknown80 = 1;
        return;
    }
    userSetting = 1;
    // The launcher's options (0x4016f0): read if their tag matches, each until one is short.
    if (r.tag(launcherTag.c_str(), launcherTag.size() + 1)) {
        for (int32_t& o : launcherOptions) {
            if (!r.take(&o, 4)) break;
        }
    }
}

void SystemConfig::setLauncherOptionCount(int count) {
    launcherOptions.assign(static_cast<size_t>(count > 0 ? count : 0), 0);
    if (!launcherOptions.empty()) launcherOptions[0] = 1;
}

void SystemConfig::applyPreset(int preset) {
    switch (preset) {
        case 0:
            useSoftFocus = 1, antiAliasType = 8, sceneDegreeSizeRatio = 1, useShadow = 1, useSelfShadow = 1;
            shadowQuality = 3, useHDR = 1, useToneMapping = 1, useDOF = 1, pointLightNum = 16;
            break;
        case 1:
            useSoftFocus = 1, antiAliasType = 4, sceneDegreeSizeRatio = 2, useShadow = 1, useSelfShadow = 1;
            shadowQuality = 3, useHDR = 1, useToneMapping = 1, useDOF = 1, pointLightNum = 8;
            break;
        case 2:
            useSoftFocus = 1, sceneDegreeSizeRatio = 2, useShadow = 1, useSelfShadow = 1, shadowQuality = 2;
            useDOF = 1, pointLightNum = 4;
            useToneMapping = 0, useHDR = 0, antiAliasType = 2;
            break;
        case 3:
            useSoftFocus = 0, sceneDegreeSizeRatio = 0, useShadow = 0, useSelfShadow = 0, shadowQuality = 1;
            useDOF = 0, pointLightNum = 2;
            useToneMapping = 0, useHDR = 0, antiAliasType = 2;
            break;
        default:
            return;
    }
    unknown80 = 1;
    unknown7c = 1;
    unknown68 = 4.0f;
    useFog = 1;
    frameRate = 60;
}

void SystemConfig::launcherDefaults() {
    antiAliasType = 0;
    sceneDegreeSizeRatio = 0;
    useSoftFocus = 0;
    useHDR = 0;
    shadowQuality = 0;
    useSelfShadow = 0;
    useShadow = 0;
    useDOF = 0;
    pointLightNum = 0;
}

}  // namespace nyan::diesel
