#include "nyan/common/sjis.hpp"

#include <cstdint>
#include <unordered_map>

namespace nyan {
namespace {

#include "common/sjis_table.inc"

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Table index of a two-byte code's lead byte section (0 for single-byte codes).
size_t leadBase(uint8_t b) {
    if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
        switch (b >> 4) {
        case 0x8: return 0x100;
        case 0x9: return 0x1100;
        case 0xE: return 0x2100;
        default: return 0x3100;
        }
    }
    return 0;
}

uint32_t tableLookup(size_t index) {
    index <<= 1;
    if (index + 1 >= sizeof(kSjisTable)) return 0xFFFD;
    return (static_cast<uint32_t>(kSjisTable[index]) << 8) | kSjisTable[index + 1];
}

}  // namespace

std::string sjisToUtf8(std::string_view sjis) {
    std::string out;
    out.reserve(sjis.size() * 3 / 2);
    size_t i = 0;
    while (i < sjis.size()) {
        uint8_t b = static_cast<uint8_t>(sjis[i]);
        if (b < 0x80) {
            out += static_cast<char>(b);
            ++i;
            continue;
        }
        size_t base = leadBase(b);

        uint32_t cp;
        if (base == 0) {
            // Single byte (half-width katakana 0xA1-0xDF) or invalid lead byte.
            cp = tableLookup(b);
            ++i;
        } else {
            if (i + 1 >= sjis.size()) {
                cp = 0xFFFD;
                ++i;
            } else {
                cp = tableLookup(base + ((b & 0x0F) << 8) + static_cast<uint8_t>(sjis[i + 1]));
                i += 2;
            }
        }
        if (cp == 0 && b != 0) cp = 0xFFFD;
        appendUtf8(out, cp);
    }
    return out;
}

std::string utf8ToSjis(std::string_view utf8) {
    // Reverse of the decode table, built once; the first (lowest) Shift-JIS code wins.
    static const std::unordered_map<uint32_t, uint16_t> reverse = [] {
        std::unordered_map<uint32_t, uint16_t> m;
        for (uint32_t b = 0x80; b <= 0xFF; ++b) {
            if (size_t base = leadBase(static_cast<uint8_t>(b))) {
                for (uint32_t t = 0x40; t <= 0xFC; ++t) {
                    uint32_t cp = tableLookup(base + ((b & 0x0F) << 8) + t);
                    if (cp && cp != 0xFFFD) m.emplace(cp, static_cast<uint16_t>((b << 8) | t));
                }
            } else {
                uint32_t cp = tableLookup(b);
                if (cp && cp != 0xFFFD) m.emplace(cp, static_cast<uint16_t>(b));
            }
        }
        return m;
    }();
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        auto c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp;
        size_t n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6 && i + 1 < utf8.size()) cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F), n = 2;
        else if ((c >> 4) == 14 && i + 2 < utf8.size())
            cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F), n = 3;
        else cp = '?', n = (c >> 3) == 30 ? 4 : 1;
        i += n;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        auto it = reverse.find(cp);
        if (it == reverse.end()) {
            out += '?';
        } else if (it->second > 0xFF) {
            out += static_cast<char>(it->second >> 8);
            out += static_cast<char>(it->second & 0xFF);
        } else {
            out += static_cast<char>(it->second);
        }
    }
    return out;
}

std::string decodeGameText(std::string_view b) {
    if (b.starts_with("\xEF\xBB\xBF")) return std::string(b.substr(3));
    if (b.starts_with("<?xml")) {
        size_t end = b.find("?>");
        std::string decl(b.substr(0, end == std::string_view::npos ? 0 : end));
        for (char& c : decl) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (decl.find("utf-8") != std::string::npos || decl.find("utf8") != std::string::npos) return std::string(b);
        if (decl.find("encoding") != std::string::npos) return sjisToUtf8(b);
    }
    // No declaration: valid UTF-8 with multi-byte sequences is UTF-8.
    bool multibyte = false;
    for (size_t i = 0; i < b.size();) {
        auto c = static_cast<uint8_t>(b[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > b.size()) return sjisToUtf8(b);
        for (size_t k = 1; k < n; ++k) {
            if ((static_cast<uint8_t>(b[i + k]) >> 6) != 2) return sjisToUtf8(b);
        }
        multibyte |= n > 1;
        i += n;
    }
    return multibyte ? std::string(b) : sjisToUtf8(b);
}

bool isValidUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        }
        i += n;
    }
    return true;
}

}  // namespace nyan
