#include "nyan/cs2/asset_index.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "nyan/common/sjis.hpp"
#include "nyan/common/xml.hpp"
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

// A file type startup.xml lists (INT/intNN: int, ext, dir): its archive, extension and folder.
struct FileType {
    std::string ext, dir;
};

std::vector<FileType> fileTypes(const GameFolder& folder, const AssetIndex& index) {
    Bytes xml;
    if (auto f = folder.open("config/startup.xml")) xml = f->readAll();
    else if (index.find("startup.xml")) xml = index.read("startup.xml");
    std::vector<FileType> types;
    if (xml.empty()) return types;
    auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size())));
    const XmlNode* table = root ? root->find("document/INT") : nullptr;
    if (!table) return types;
    for (const auto& t : table->children) {
        if (!t->name.starts_with("int")) continue;
        FileType type{t->value("ext").value_or(""), t->value("dir").value_or("")};
        std::replace(type.dir.begin(), type.dir.end(), '\\', '/');
        if (!type.ext.empty() && !type.dir.empty()) types.push_back(std::move(type));
    }
    return types;
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
    index.addLooseFolders(folder);
    // E-mote games ship the SDK's driver; its key opens their models' headers.
    for (const auto& name : folder.list()) {
        if (toLowerAscii(name) != "emotedriver.dll") continue;
        if (auto f = folder.open(name)) index.emoteKeys_ = Psb::keyCandidates(f->readAll());
    }
    return index;
}

void AssetIndex::addLooseFolders(const GameFolder& folder) {
    // The engine registers each file type of startup.xml with its archive and its folder (cs2.exe
    // 0x48d0f0 -> 0x565680, Amakano 2) and the update??.INT patches in a list of their own
    // (0x565c00). Its open (0x566490) looks in the patches first (0x566da0), then, in mode 3 (the
    // mode its opens pass), in each type's folder ("<dir>\\<name>") before its archive. So a
    // loose file overrides the base archives but not the patches (Ikinari Anata ni Koishiteiru's
    // English name plates are loose; its English scenes are in the patches, older copies loose);
    // between two folders of one extension the earlier type's wins.
    std::vector<std::string> seen;
    std::set<std::string> loose;  // names a folder gave
    for (const FileType& t : fileTypes(folder, *this)) {
        const std::string dir = toLowerAscii(t.dir), ext = toLowerAscii(t.ext);
        if (std::find(seen.begin(), seen.end(), dir + "|" + ext) != seen.end()) continue;
        seen.push_back(dir + "|" + ext);
        std::vector<std::string> files;
        for (const auto& f : folder.listDir(t.dir)) {
            const size_t dot = f.rfind('.');
            if (dot != std::string::npos && toLowerAscii(f.substr(dot + 1)) == ext) files.push_back(f);
        }
        if (files.empty()) continue;
        archives_.push_back(KifArchive::openFolder(folder, t.dir, files));
        const KifArchive* archive = archives_.back().get();
        for (const auto& entry : archive->entries()) {
            std::string key = normalizeName(entry.name);
            if (auto it = lookup_.find(key); it != lookup_.end() && isUpdateArchive(toLowerAscii(it->second.archive->name()))) continue;
            if (loose.insert(key).second) lookup_.insert_or_assign(std::move(key), AssetRef{archive, &entry});
        }
    }
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
