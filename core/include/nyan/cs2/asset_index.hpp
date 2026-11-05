#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/cs2/kif_archive.hpp"
#include "nyan/cs2/vcodes.hpp"

namespace nyan::cs2 {

struct AssetRef {
    const KifArchive* archive;
    const KifEntry* entry;
};

// All KIF archives of one CatSystem2 game, merged into a single case-insensitive
// name lookup. Archives named updateNN.int are patches: they are applied last,
// in ascending order, and override same-named entries in the base archives.
class AssetIndex {
public:
    static AssetIndex load(const GameFolder& folder);

    const std::optional<VCodes>& vcodes() const { return vcodes_; }
    const std::vector<std::unique_ptr<KifArchive>>& archives() const { return archives_; }
    size_t size() const { return lookup_.size(); }

    // Names may hold `%NAME%` placeholders (see setPlaceholder); they are expanded first.
    std::optional<AssetRef> find(std::string_view name) const;
    // Reads an asset by name; throws nyan::Error if missing.
    Bytes read(std::string_view name) const;

    // Engine placeholders in asset names, e.g. %LANG% -> "en" (startup.xml <language>).
    void setPlaceholder(const std::string& name, const std::string& value) { placeholders_[name] = value; }
    // `name` with its known `%NAME%` placeholders replaced (case-insensitive).
    std::string expand(std::string_view name) const;

    // The E-mote model key, if the game has E-mote (its driver keeps the key as a decimal string:
    // the candidates are tried on the first model). 0 if none fits.
    uint32_t emoteKey(ByteSpan model) const;

    // Lower-cases ASCII letters in a Shift-JIS string without touching trail bytes.
    static std::string normalizeName(std::string_view sjis);

private:
    std::optional<VCodes> vcodes_;
    std::vector<std::unique_ptr<KifArchive>> archives_;
    std::unordered_map<std::string, AssetRef> lookup_;
    std::unordered_map<std::string, std::string> placeholders_;  // upper-case name -> value
    std::vector<uint32_t> emoteKeys_;
    mutable std::optional<uint32_t> emoteKey_;
};

}  // namespace nyan::cs2
