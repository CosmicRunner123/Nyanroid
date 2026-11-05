#include "nyan/wa2/system_data.hpp"

#include <cstring>

#include <zlib.h>

#include "nyan/common/file_source.hpp"
#include "nyan/common/zlib_util.hpp"

namespace nyan::wa2 {

namespace {

constexpr char kMagic[8] = {'N', 'Y', 'W', '2', 'S', 'Y', 'S', '1'};

// WA2.exe's table of script names (0x4be530), in its order.
const char* const kScriptNames[] = {
    "1001", "1002", "1003", "1004", "1005", "1006", "1007", "1008", "1008_020", "1008_030",
    "1008_040", "1008_050", "1009", "1009_020", "1009_030", "1010", "1010_020", "1010_030",
    "1010_040", "1010_050", "1010_060", "1010_070", "1011", "1011_020", "1011_030", "1012",
    "1012_020", "1012_030", "1013", "catch", "catch2", "catch3", "1012_030_2", "2001", "2002",
    "2003", "2004", "2005", "2006", "2007", "2008", "2009", "2010", "2011", "2012", "2013", "2014",
    "2015", "2016", "2017", "2018", "2019", "2020", "2021", "2022", "2023", "2024", "2025", "2026",
    "2027", "2028", "2029", "2030", "2031", "2032", "2033", "2301", "2302", "2303", "2304", "2305",
    "2306", "2307", "2308", "2309", "2310", "2311", "2312", "2313", "2314", "2315", "2316", "2317",
    "2318", "2319", "2320", "2321", "2322", "2401", "2402", "2403", "2404", "2405", "2406", "2407",
    "2408", "2409", "2410", "2411", "2412", "2413", "2501", "2502", "2503", "2504", "2505", "2506",
    "2507", "2508", "2509", "2510", "2511", "2512", "2513", "2514", "2515", "2516", "2517", "3001",
    "3002", "3003", "3004", "3005", "3006", "3007", "3008", "3009", "3010", "3011", "3012", "3013",
    "3014", "3014_2", "3014_3", "3015", "3016", "3017", "3018", "3019", "3020", "3021", "3022",
    "3023", "3024", "3101", "3102", "3103", "3104", "3105", "3106", "3107", "3108", "3109", "3110",
    "3111", "3201", "3202", "3203", "3204", "3205", "3206", "3207", "3208", "3209", "3210", "3211",
    "3901", "3902", "3903", "3904", "3905", "3906", "3907", "3908", "3909", "5000", "5001", "5002",
    "5003", "5004", "5100", "5101", "5102", "5103", "5104",
};

}  // namespace

bool SystemData::readBit(int script, int line) const {
    // 0x44df00: bit `line` of the script's 0x200 bytes, the most significant bit of a byte first.
    if (script < 0 || line < 0 || line >= 0x1000) return false;
    size_t at = static_cast<size_t>(script) * 0x200 + static_cast<size_t>(line / 8);
    if (at >= kReadBitsSize) return false;
    return (image_[at] >> (7 - line % 8)) & 1;
}

void SystemData::setReadBit(int script, int line) {
    if (script < 0 || line < 0 || line >= 0x1000) return;
    size_t at = static_cast<size_t>(script) * 0x200 + static_cast<size_t>(line / 8);
    if (at >= kReadBitsSize) return;
    image_[at] |= static_cast<uint8_t>(1 << (7 - line % 8));
    dirty = true;
}

int32_t SystemData::gameFlag(int n) const {
    if (n < 0 || n >= kGameFlagCount) return 0;
    return static_cast<int32_t>(readLE32(&image_[kGameFlagsAt + 4 * static_cast<size_t>(n)]));
}

void SystemData::setGameFlag(int n, int32_t v) {
    if (n < 0 || n >= kGameFlagCount) return;
    writeLE32(&image_[kGameFlagsAt + 4 * static_cast<size_t>(n)], static_cast<uint32_t>(v));
    dirty = true;
}

uint8_t SystemData::galleryFlag(int kind, int n) const {
    if (kind < 0 || kind > 1 || n < 0 || n >= static_cast<int>(kGalleryEach)) return 0;
    return image_[kGalleryAt + static_cast<size_t>(kind) * kGalleryEach + static_cast<size_t>(n)];
}

void SystemData::setGalleryFlag(int kind, int n, uint8_t v) {
    if (kind < 0 || kind > 1 || n < 0 || n >= static_cast<int>(kGalleryEach)) return;
    image_[kGalleryAt + static_cast<size_t>(kind) * kGalleryEach + static_cast<size_t>(n)] = v;
    dirty = true;
}

Bytes SystemData::encode() const {
    uLongf bound = compressBound(static_cast<uLong>(image_.size()));
    Bytes out(16 + bound);
    std::memcpy(out.data(), kMagic, 8);
    writeLE32(&out[8], static_cast<uint32_t>(image_.size()));
    uLongf packed = bound;
    if (compress2(&out[16], &packed, image_.data(), static_cast<uLong>(image_.size()), Z_BEST_SPEED) != Z_OK) {
        fail("system data: deflate failed");
    }
    writeLE32(&out[12], static_cast<uint32_t>(packed));
    out.resize(16 + packed);
    return out;
}

std::optional<SystemData> SystemData::decode(ByteSpan file) {
    if (file.size() < 16 || std::memcmp(file.data(), kMagic, 8) != 0) return std::nullopt;
    uint32_t size = readLE32(&file[8]), packed = readLE32(&file[12]);
    if (size != kSize || 16 + static_cast<size_t>(packed) > file.size()) return std::nullopt;
    try {
        SystemData d;
        d.image_ = zlibInflate(file.subspan(16, packed), size);
        return d;
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::optional<SystemData> SystemData::fromPcFile(ByteSpan file) {
    if (file.size() < kSize) return std::nullopt;
    SystemData d;
    std::memcpy(d.image_.data(), file.data(), kSize);
    return d;
}

int scriptIndex(const std::string& name) {
    int i = 0;
    for (const char* n : kScriptNames) {
        if (toLowerAscii(name) == toLowerAscii(n)) return i;
        ++i;
    }
    return 0;
}

}  // namespace nyan::wa2
