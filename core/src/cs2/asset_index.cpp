#include "nyan/cs2/asset_index.hpp"

#include <algorithm>
#include <cctype>

#include "nyan/cs2/kif_seed_search.hpp"
#include "nyan/cs2/psb.hpp"

namespace nyan::cs2 {
namespace {

bool isSjisLeadByte(uint8_t b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

bool isUpdateArchive(const std::string& lowerName) {
    return lowerName.starts_with("update") && lowerName.ends_with(".int");
}

}  // namespace

std::string AssetIndex::normalizeName(std::string_view sjis) {
    std::string out(sjis);
    for (size_t i = 0; i < out.size(); ++i) {
        auto b = static_cast<uint8_t>(out[i]);
        if (isSjisLeadByte(b)) {
            ++i;  // skip trail byte
        } else if (b >= 'A' && b <= 'Z') {
            out[i] = static_cast<char>(b - 'A' + 'a');
        } else if (b == '/') {
            out[i] = '\\';
        }
    }
    return out;
}

namespace {

std::vector<std::string> archiveNames(const GameFolder& folder) {
    std::vector<std::string> base, updates;
    for (const auto& name : folder.list()) {
        std::string lower = toLowerAscii(name);
        if (!lower.ends_with(".int")) continue;
        (isUpdateArchive(lower) ? updates : base).push_back(name);
    }
    auto byLower = [](const std::string& a, const std::string& b) { return toLowerAscii(a) < toLowerAscii(b); };
    std::sort(base.begin(), base.end(), byLower);
    std::sort(updates.begin(), updates.end(), byLower);
    base.insert(base.end(), updates.begin(), updates.end());
    return base;
}

// The names of the encrypted archive with the most entries, as stored, or none.
std::vector<std::string> largestEncryptedTable(const GameFolder& folder, const std::vector<std::string>& archives) {
    std::vector<std::string> largest;
    for (const auto& name : archives) {
        auto file = folder.open(name);
        if (!file) continue;
        bool encrypted = false;
        auto names = KifArchive::rawNames(*file, encrypted);
        if (encrypted && names.size() > largest.size()) largest = std::move(names);
    }
    return largest;
}

// A V_CODE as the exes store them: readable text ("NXT-N0VZCG13"). Decrypted from a protected
// exe's packed resources it is not.
bool readableCode(const std::string& code) {
    return !code.empty() && std::all_of(code.begin(), code.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

// The engine unobfuscates the names with the CRC of its exe's V_CODE2 (vcodeSeed). A protected
// exe keeps its resources packed, so the V_CODE2 read from the file is not readable and gives
// names that are not file names: then the seed is the one that does (found once, then cached).
std::optional<uint32_t> namesSeed(const GameFolder& folder, const std::vector<std::string>& archives,
                                  const std::optional<VCodes>& vcodes, bool& searched) {
    searched = false;
    if (vcodes && readableCode(vcodes->vcode2)) return vcodes->tocSeed();
    std::optional<uint32_t> seed;
    if (vcodes) seed = vcodes->tocSeed();
    const std::vector<std::string> table = largestEncryptedTable(folder, archives);
    if (table.empty() || (seed && kifSeedFits(table, *seed))) return seed;
    searched = true;
    if (auto found = findKifSeed(table)) return found;
    return seed;
}

}  // namespace

bool AssetIndex::needsSeedSearch(const GameFolder& folder) {
    const auto vcodes = findVCodes(folder);
    if (vcodes && readableCode(vcodes->vcode2)) return false;
    const std::vector<std::string> table = largestEncryptedTable(folder, archiveNames(folder));
    if (table.empty() || (vcodes && kifSeedFits(table, vcodes->tocSeed()))) return false;
    return !kifSeedCached(table);
}

AssetIndex AssetIndex::load(const GameFolder& folder) {
    AssetIndex index;
    index.vcodes_ = findVCodes(folder);
    const std::vector<std::string> base = archiveNames(folder);
    const std::optional<uint32_t> tocSeed = namesSeed(folder, base, index.vcodes_, index.tocSeedSearched_);
    index.tocSeed_ = tocSeed;

    for (const auto& name : base) {
        auto file = folder.open(name);
        if (!file || file->size() < 8) continue;
        index.archives_.push_back(KifArchive::open(std::move(file), name, tocSeed));
        const KifArchive* archive = index.archives_.back().get();
        for (const auto& entry : archive->entries()) {
            index.lookup_.insert_or_assign(normalizeName(entry.name), AssetRef{archive, &entry});
        }
    }
    if (index.archives_.empty()) fail("No KIF (.int) archives found in " + folder.displayName());
    // E-mote games ship the SDK's driver; its key opens their models' headers.
    for (const auto& name : folder.list()) {
        if (toLowerAscii(name) != "emotedriver.dll") continue;
        if (auto f = folder.open(name)) index.emoteKeys_ = Psb::keyCandidates(f->readAll());
    }
    return index;
}

uint32_t AssetIndex::emoteKey(ByteSpan model) const {
    if (emoteKey_) return *emoteKey_;
    for (uint32_t k : emoteKeys_) {
        if (Psb::keyFits(model, k)) return *(emoteKey_ = k);
    }
    return 0;
}

std::string AssetIndex::expand(std::string_view name) const {
    std::string out(name);
    if (placeholders_.empty()) return out;
    for (size_t p = out.find('%'); p != std::string::npos;) {
        size_t end = out.find('%', p + 1);
        if (end == std::string::npos) break;
        std::string key = out.substr(p + 1, end - p - 1);
        for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = placeholders_.find(key);
        if (it == placeholders_.end()) {
            p = end;  // not a placeholder: its closing % may open the next one
            continue;
        }
        out.replace(p, end - p + 1, it->second);
        p = out.find('%', p + it->second.size());
    }
    return out;
}

std::optional<AssetRef> AssetIndex::find(std::string_view name) const {
    if (name.find('%') != std::string_view::npos) {
        auto it = lookup_.find(normalizeName(expand(name)));
        if (it == lookup_.end()) return std::nullopt;
        return it->second;
    }
    auto it = lookup_.find(normalizeName(name));
    if (it == lookup_.end()) return std::nullopt;
    return it->second;
}

Bytes AssetIndex::read(std::string_view name) const {
    auto ref = find(name);
    if (!ref) fail("Asset not found: " + std::string(name));
    return ref->archive->read(*ref->entry);
}

}  // namespace nyan::cs2
