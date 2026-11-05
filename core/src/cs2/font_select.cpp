#include "nyan/cs2/font_select.hpp"

#include <algorithm>
#include <cctype>

#include "nyan/common/sjis.hpp"

namespace nyan::cs2 {
namespace {

uint16_t be16(const Bytes& d, size_t o) {
    return o + 2 <= d.size() ? static_cast<uint16_t>((d[o] << 8) | d[o + 1]) : 0;
}
uint32_t be32(const Bytes& d, size_t o) {
    return o + 4 <= d.size() ? (static_cast<uint32_t>(be16(d, o)) << 16) | be16(d, o + 2) : 0;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

size_t faceOffset(const Bytes& d, int face) {
    if (d.size() >= 12 && d[0] == 't' && d[1] == 't' && d[2] == 'c' && d[3] == 'f') {
        uint32_t n = be32(d, 8);
        if (face < 0 || static_cast<uint32_t>(face) >= n) return SIZE_MAX;
        return be32(d, 12 + 4 * static_cast<size_t>(face));
    }
    return face == 0 ? 0 : SIZE_MAX;
}

}  // namespace

int fontFaceCount(const Bytes& d) {
    if (d.size() < 12) return 0;
    if (d[0] == 't' && d[1] == 't' && d[2] == 'c' && d[3] == 'f') return static_cast<int>(be32(d, 8));
    uint32_t tag = be32(d, 0);
    bool font = tag == 0x00010000 || tag == 0x4F54544F /* OTTO */ || tag == 0x74727565 /* true */;
    return font ? 1 : 0;
}

namespace {

std::vector<std::string> faceNames(const Bytes& d, int face, bool familiesOnly) {
    std::vector<std::string> out;
    size_t base = faceOffset(d, face);
    if (base == SIZE_MAX) return out;
    uint16_t tables = be16(d, base + 4);
    size_t nameOff = 0, nameLen = 0;
    for (uint16_t i = 0; i < tables; ++i) {
        size_t rec = base + 12 + 16 * static_cast<size_t>(i);
        if (be32(d, rec) == 0x6E616D65 /* name */) {
            nameOff = be32(d, rec + 8);
            nameLen = be32(d, rec + 12);
        }
    }
    if (!nameOff || nameOff + nameLen > d.size()) return out;
    uint16_t count = be16(d, nameOff + 2);
    size_t strings = nameOff + be16(d, nameOff + 4);
    // Per language: family (1), subfamily (2), full name (4); typographic family (16) when present.
    struct Lang {
        std::string family, sub, full, typoFamily;
    };
    std::vector<std::pair<uint32_t, Lang>> langs;
    auto langOf = [&](uint32_t key) -> Lang& {
        for (auto& [k, l] : langs) {
            if (k == key) return l;
        }
        langs.push_back({key, {}});
        return langs.back().second;
    };
    for (uint16_t i = 0; i < count; ++i) {
        size_t rec = nameOff + 6 + 12 * static_cast<size_t>(i);
        uint16_t platform = be16(d, rec), encoding = be16(d, rec + 2), language = be16(d, rec + 4);
        uint16_t id = be16(d, rec + 6), len = be16(d, rec + 8), off = be16(d, rec + 10);
        if (id != 1 && id != 2 && id != 4 && id != 16) continue;
        size_t p = strings + off;
        if (p + len > d.size()) continue;
        std::string s;
        if (platform == 0 || platform == 3) {
            for (size_t k = 0; k + 1 < len; k += 2) {
                uint32_t cp = be16(d, p + k);
                if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < len) {
                    uint32_t lo = be16(d, p + k + 2);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    k += 2;
                }
                appendUtf8(s, cp);
            }
        } else if (platform == 1) {
            std::string raw(reinterpret_cast<const char*>(&d[p]), len);
            s = encoding == 1 ? sjisToUtf8(raw) : raw;  // Mac Japanese is Shift-JIS
        } else {
            continue;
        }
        Lang& l = langOf((static_cast<uint32_t>(platform) << 16) | language);
        if (id == 1) l.family = s;
        else if (id == 2) l.sub = s;
        else if (id == 4) l.full = s;
        else l.typoFamily = s;
    }
    auto add = [&](const std::string& s) {
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    for (const auto& [k, l] : langs) {
        if (familiesOnly) {
            add(l.family);
            add(l.typoFamily);
            continue;
        }
        add(l.full);
        add(l.family);
        add(l.typoFamily);
        if (!l.family.empty() && !l.sub.empty()) add(l.family + " " + l.sub);
        if (!l.typoFamily.empty() && !l.sub.empty()) add(l.typoFamily + " " + l.sub);
    }
    return out;
}

}  // namespace

std::vector<std::string> fontFaceNames(const Bytes& d, int face) {
    return faceNames(d, face, false);
}

std::vector<std::string> fontFamilyNames(const Bytes& d, int face) {
    return faceNames(d, face, true);
}

FontFamilyInfo fontFamilyInfo(const Bytes& d, int face) {
    FontFamilyInfo info;
    size_t base = faceOffset(d, face);
    if (base == SIZE_MAX) return info;
    auto table = [&](uint32_t tag, size_t& off, size_t& len) {
        uint16_t tables = be16(d, base + 4);
        for (uint16_t i = 0; i < tables; ++i) {
            size_t rec = base + 12 + 16 * static_cast<size_t>(i);
            if (be32(d, rec) != tag) continue;
            off = be32(d, rec + 8), len = be32(d, rec + 12);
            return off + len <= d.size();
        }
        return false;
    };
    size_t off = 0, len = 0;
    // The family (name ID 1): Windows Japanese, Windows US English, any Windows, Mac.
    bool japaneseName = false;
    if (table(0x6E616D65 /* name */, off, len) && len >= 6) {
        const uint16_t count = be16(d, off + 2);
        const size_t strings = off + be16(d, off + 4);
        int best = 99;
        for (uint16_t i = 0; i < count; ++i) {
            size_t rec = off + 6 + 12 * static_cast<size_t>(i);
            uint16_t platform = be16(d, rec), encoding = be16(d, rec + 2), language = be16(d, rec + 4);
            uint16_t id = be16(d, rec + 6), n = be16(d, rec + 8), at = be16(d, rec + 10);
            if (id != 1 || strings + at + n > d.size()) continue;
            if (platform == 3 && language == 0x0411) japaneseName = true;
            const int rank = platform == 3 ? (language == 0x0411 ? 0 : language == 0x0409 ? 1 : 2) : platform == 1 ? 3 : 99;
            if (rank >= best) continue;
            std::string s;
            const size_t p = strings + at;
            if (platform == 3 || platform == 0) {
                for (size_t k = 0; k + 1 < n; k += 2) {
                    uint32_t cp = be16(d, p + k);
                    if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < n) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (be16(d, p + k + 2) - 0xDC00u);
                        k += 2;
                    }
                    appendUtf8(s, cp);
                }
            } else {
                std::string raw(reinterpret_cast<const char*>(&d[p]), n);
                s = encoding == 1 ? sjisToUtf8(raw) : raw;
            }
            if (s.empty()) continue;
            best = rank;
            info.family = std::move(s);
        }
    }
    bool style = false;
    if (table(0x4F532F32 /* OS/2 */, off, len)) {
        if (be16(d, off) >= 1 && len >= 82) info.codePages = be32(d, off + 78);
        if (len >= 36 && d[off + 35] == 9) info.fixedPitch = true;  // PANOSE bProportion
        if (len >= 64) {
            if (const uint16_t w = be16(d, off + 4); w >= 1 && w <= 1000) info.weight = w;
            info.italic = (be16(d, off + 62) & 1) != 0;
            info.bold = (be16(d, off + 62) & 0x20) != 0;
            style = true;
        }
    }
    if (!style && table(0x68656164 /* head */, off, len) && len >= 46) {
        const uint16_t mac = be16(d, off + 44);
        if (mac & 1) info.weight = 700;
        info.italic = (mac & 2) != 0;
        info.bold = (mac & 1) != 0;
    }
    if (!info.codePages && japaneseName) info.codePages = 1u << 17;
    if (table(0x706F7374 /* post */, off, len) && len >= 16 && be32(d, off + 12) != 0) info.fixedPitch = true;
    return info;
}

bool fontRenderable(const Bytes& d, int face) {
    size_t base = faceOffset(d, face);
    if (base == SIZE_MAX) return false;
    uint16_t tables = be16(d, base + 4);
    for (uint16_t i = 0; i < tables; ++i) {
        uint32_t tag = be32(d, base + 12 + 16 * static_cast<size_t>(i));
        if (tag == 0x676C7966 /* glyf */ || tag == 0x43464620 /* "CFF " */ || tag == 0x43464632 /* CFF2 */) return true;
    }
    return false;
}

bool fontNameEquals(const std::string& a, const std::string& b) {
    auto norm = [](const std::string& s) {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            auto c = static_cast<unsigned char>(s[i]);
            if (c == ' ' || c == '-' || c == '_') continue;
            // Full-width space (U+3000) is a separator too.
            if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                static_cast<unsigned char>(s[i + 2]) == 0x80) {
                i += 2;
                continue;
            }
            r += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        }
        return r;
    };
    return norm(a) == norm(b);
}

}  // namespace nyan::cs2
