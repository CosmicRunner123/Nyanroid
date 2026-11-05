#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// system.npk's setting.ini: [sections] of KEY=VALUE lines; '#' starts a comment anywhere on a
// line; values may be in double quotes. Keys and values are kept as their bytes (the file is
// Shift-JIS: Totono's font face names). Read by the exe's CIniLoader (not yet compared).
class SettingIni {
public:
    explicit SettingIni(ByteSpan data);

    std::optional<std::string> get(const std::string& section, const std::string& key) const;
    int getInt(const std::string& section, const std::string& key, int fallback) const;
    // All KEY=VALUE pairs of a section, in file order ([Font]: face name -> font file).
    std::vector<std::pair<std::string, std::string>> section(const std::string& name) const;

private:
    struct Entry {
        std::string section, key, value;
    };
    std::vector<Entry> entries_;
};

}  // namespace nyan::diesel
