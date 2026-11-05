#include "nyan/diesel/setting_ini.hpp"

#include <cstdlib>

#include "nyan/common/file_source.hpp"

namespace nyan::diesel {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

}  // namespace

SettingIni::SettingIni(ByteSpan data) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    // A leading UTF-8 byte-order mark is skipped (muramasa_en.exe 0x405093, Saya_en.exe 0x4047d3, in
    // the text reader the startup calls for it: 0x401ea9 -> 0x405790 -> 0x405020). The Song of
    // Saya's setting.ini starts with one.
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        // A comment starts at '#' outside quotes (Shift-JIS trail bytes are never '#' or '"').
        bool quoted = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) {
                line.resize(i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = toLowerAscii(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        entries_.push_back({section, key, value});
    }
}

std::optional<std::string> SettingIni::get(const std::string& section, const std::string& key) const {
    std::string s = toLowerAscii(section), k = toLowerAscii(key);
    for (const auto& e : entries_) {
        if (e.section == s && toLowerAscii(e.key) == k) return e.value;
    }
    return std::nullopt;
}

int SettingIni::getInt(const std::string& section, const std::string& key, int fallback) const {
    auto v = get(section, key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    long n = std::strtol(v->c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<int>(n) : fallback;
}

std::vector<std::pair<std::string, std::string>> SettingIni::section(const std::string& name) const {
    std::string s = toLowerAscii(name);
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : entries_) {
        if (e.section == s) out.emplace_back(e.key, e.value);
    }
    return out;
}

}  // namespace nyan::diesel
