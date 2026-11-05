#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

// The engine's system data, Sys.sav, kept as an image of the file (WA2.exe 0x405180 / 0x405370 /
// 0x4052e0 write it in place): read-text bits at 0 (0x200 bytes per script index, one bit per line,
// most significant bit first), the gallery flags at 0x80000 (event pictures, then H scenes, 1000000
// bytes each), the game flags at 0x268480 (1024 ints).
class SystemData {
public:
    static constexpr size_t kReadBitsSize = 0x80000;
    static constexpr size_t kGalleryAt = 0x80000;
    static constexpr size_t kGalleryEach = 1000000;
    static constexpr size_t kGameFlagsAt = 0x268480;
    static constexpr int kGameFlagCount = 0x400;
    static constexpr size_t kSize = kGameFlagsAt + 4 * kGameFlagCount;

    SystemData() : image_(kSize, 0) {}

    bool readBit(int script, int line) const;
    void setReadBit(int script, int line);
    int32_t gameFlag(int n) const;
    void setGameFlag(int n, int32_t v);
    uint8_t galleryFlag(int kind, int n) const;
    void setGalleryFlag(int kind, int n, uint8_t v);

    // Nyanroid's file: a magic, the image's size, the image deflated.
    Bytes encode() const;
    static std::optional<SystemData> decode(ByteSpan file);
    // A PC Sys.sav: the image as it is.
    static std::optional<SystemData> fromPcFile(ByteSpan file);

    bool dirty = false;  // changed since it was last written

private:
    Bytes image_;
};

// The script's index in the engine's table of script names (WA2.exe 0x4be530, 185 names, compared
// ignoring case by 0x40e960); 0 for scripts the table does not name (the patch's added scripts and
// parts share index 0, the prologue's read-text bits, as on PC).
int scriptIndex(const std::string& name);

}  // namespace nyan::wa2
