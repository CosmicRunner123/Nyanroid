#include "nyan/wa2/exe_info.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nyan::wa2 {

namespace {

constexpr uint32_t kRtVersion = 16;

struct Section {
    uint32_t va, size, raw, rawSize;
};

// Appends UTF-16LE code units [p, end) up to a terminating zero as UTF-8.
std::string utf16ToUtf8(const uint8_t* p, const uint8_t* end) {
    std::string out;
    while (p + 1 < end) {
        uint32_t c = readLE16(p);
        p += 2;
        if (c == 0) break;
        if (c >= 0xd800 && c < 0xdc00 && p + 1 < end) {
            uint32_t lo = readLE16(p);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                p += 2;
            }
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xc0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | c >> 12);
            out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | c >> 18);
            out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
            out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return out;
}

// One block of a VS_VERSIONINFO tree (wLength, wValueLength, wType, key, value, children), whose
// String leaves go into `out`.
void versionBlock(const uint8_t* base, size_t at, size_t end, int depth, std::map<std::string, std::string>& out) {
    if (depth > 4 || at + 6 > end) return;
    size_t length = readLE16(base + at);
    size_t valueLength = readLE16(base + at + 2);
    uint16_t type = readLE16(base + at + 4);
    size_t blockEnd = std::min(end, at + length);
    if (length < 6) return;
    size_t p = at + 6;
    size_t keyStart = p;
    while (p + 1 < blockEnd && readLE16(base + p) != 0) p += 2;
    std::string key = utf16ToUtf8(base + keyStart, base + p);
    p = (p + 2 + 3) & ~size_t{3};
    if (depth == 3) {
        // A String: its value is text (type 1), wValueLength characters.
        if (type == 1 && valueLength > 0 && p < blockEnd) out[key] = utf16ToUtf8(base + p, base + blockEnd);
        return;
    }
    size_t valueBytes = type == 1 ? valueLength * 2 : valueLength;
    p = (p + valueBytes + 3) & ~size_t{3};
    // VS_VERSION_INFO > StringFileInfo > (language table) > String.
    if (depth == 1 && key != "StringFileInfo") return;
    while (p + 6 <= blockEnd) {
        size_t childLength = readLE16(base + p);
        if (childLength == 0) break;
        versionBlock(base, p, blockEnd, depth + 1, out);
        p = (p + childLength + 3) & ~size_t{3};
    }
}

}  // namespace

std::optional<ExeInfo> readExeInfo(ByteSpan image) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return std::nullopt;
    size_t pe = readLE32(&image[0x3c]);
    if (pe + 24 > image.size() || std::memcmp(&image[pe], "PE\0\0", 4) != 0) return std::nullopt;
    ExeInfo info;
    uint16_t sections = readLE16(&image[pe + 6]);
    info.timestamp = readLE32(&image[pe + 8]);
    uint16_t optSize = readLE16(&image[pe + 20]);
    size_t opt = pe + 24;
    if (opt + optSize > image.size() || optSize < 2) return info;
    uint16_t magic = readLE16(&image[opt]);
    size_t dirs = opt + (magic == 0x20b ? 112 : 96);
    if (dirs + 3 * 8 > opt + optSize) return info;
    uint32_t resourceRva = readLE32(&image[dirs + 2 * 8]);
    if (resourceRva == 0) return info;

    std::vector<Section> table;
    size_t s = opt + optSize;
    for (uint16_t i = 0; i < sections && s + 40 <= image.size(); ++i, s += 40) {
        table.push_back({readLE32(&image[s + 12]), readLE32(&image[s + 8]), readLE32(&image[s + 20]),
                         readLE32(&image[s + 16])});
    }
    auto offsetOf = [&](uint32_t rva) -> std::optional<size_t> {
        for (const auto& sec : table) {
            if (rva >= sec.va && rva < sec.va + std::max(sec.size, sec.rawSize)) {
                size_t off = sec.raw + (rva - sec.va);
                if (off < image.size()) return off;
            }
        }
        return std::nullopt;
    };
    auto root = offsetOf(resourceRva);
    if (!root) return info;
    const uint8_t* d = image.data();
    // A resource directory's first entry whose id is `id` (any entry if id < 0): the offset it
    // points to, relative to the resource root, and whether that is another directory.
    auto entry = [&](size_t dir, int64_t id, bool& isDir) -> std::optional<uint32_t> {
        if (*root + dir + 16 > image.size()) return std::nullopt;
        size_t n = readLE16(d + *root + dir + 12) + readLE16(d + *root + dir + 14);
        for (size_t k = 0; k < n; ++k) {
            size_t e = *root + dir + 16 + k * 8;
            if (e + 8 > image.size()) break;
            uint32_t nameOrId = readLE32(d + e), target = readLE32(d + e + 4);
            if (id >= 0 && ((nameOrId & 0x80000000u) || nameOrId != id)) continue;
            isDir = (target & 0x80000000u) != 0;
            return target & 0x7fffffffu;
        }
        return std::nullopt;
    };
    bool isDir = false;
    auto typeDir = entry(0, kRtVersion, isDir);
    if (!typeDir || !isDir) return info;
    auto nameDir = entry(*typeDir, -1, isDir);
    if (!nameDir || !isDir) return info;
    auto langData = entry(*nameDir, -1, isDir);
    if (!langData || isDir || *root + *langData + 8 > image.size()) return info;
    uint32_t dataRva = readLE32(d + *root + *langData);
    uint32_t dataSize = readLE32(d + *root + *langData + 4);
    auto data = offsetOf(dataRva);
    if (!data || *data + dataSize > image.size()) return info;
    versionBlock(d, *data, *data + dataSize, 0, info.strings);
    return info;
}

}  // namespace nyan::wa2
