#include "nyan/wa2/assets.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "nyan/common/sjis.hpp"

namespace nyan::wa2 {

namespace {

// The sound archives, as the engine names them ("%s.PAK"), in its order (0x405e10).
constexpr const char* kSoundArchives[] = {"BGM", "Se", "Voice"};
// The patch's archive, searched before every group's own (0x437610).
constexpr const char* kPatchArchive = "en.pak";

std::string lower(std::string_view s) {
    return toLowerAscii(std::string(s));
}

std::string stemOf(const std::string& fileName) {
    auto dot = fileName.rfind('.');
    return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

bool endsWithPak(const std::string& fileName) {
    return lower(fileName).ends_with(".pak");
}

}  // namespace

namespace {

// The link time of Special Contents' engine (both executables carry it).
constexpr uint32_t kSpecialLinkTime = 1514448908;  // 2017-12-28 08:15:08 UTC

// An executable's PE link time; 0 if it is not one.
uint32_t linkTime(const GameFolder& folder, const std::string& name) {
    auto f = folder.open(name);
    if (!f || f->size() < 0x40) return 0;
    uint8_t head[0x40];
    f->readAt(0, head, sizeof head);
    if (head[0] != 'M' || head[1] != 'Z') return 0;
    uint32_t pe = static_cast<uint32_t>(head[0x3c] | head[0x3d] << 8 | head[0x3e] << 16 | head[0x3f] << 24);
    if (pe + 12 > f->size()) return 0;
    uint8_t h[12];
    f->readAt(pe, h, sizeof h);
    if (std::memcmp(h, "PE\0\0", 4) != 0) return 0;
    return static_cast<uint32_t>(h[8] | h[9] << 8 | h[10] << 16 | h[11] << 24);
}

}  // namespace

Assets::Assets(std::unique_ptr<GameFolder> folder) : folder_(std::move(folder)) {
    icBundled_ = folder_->open("IC/bak.pak") != nullptr;
    for (const auto& n : folder_->list()) {
        if (lower(n).ends_with(".exe") && linkTime(*folder_, n) == kSpecialLinkTime) build_ = Build::Special;
    }
    auto addSound = [&](const std::string& path) {
        if (auto a = LacArchive::open(folder_->open(path), path)) sound_.push_back(std::move(a));
    };
    for (const char* n : kSoundArchives) addSound(std::string(n) + ".PAK");
    if (icBundled_) {
        for (const char* n : kSoundArchives) addSound(std::string("IC/") + n + ".PAK");
    }
}

const KcapArchive* Assets::archive(const std::string& path) const {
    std::lock_guard lock(mu_);
    std::string key = lower(path);
    auto it = archives_.find(key);
    if (it != archives_.end()) return it->second.get();
    // Name it as it is on disk ("IC/bak.pak"), whichever spelling asked first.
    std::string shown = path;
    auto slash = path.find('/');
    std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const auto& n : dir.empty() ? folder_->list() : folder_->listDir(dir)) {
        if (lower(n) == lower(file)) shown = dir.empty() ? n : dir + "/" + n;
    }
    auto a = KcapArchive::open(folder_->open(path), shown);
    return archives_.emplace(key, std::move(a)).first->second.get();
}

std::vector<std::string> Assets::chainPaths(std::string_view group) const {
    std::string g(group);
    return {kPatchArchive, g + ".pak", "IC/" + g + ".pak", "pak/" + g + ".pak"};
}

std::vector<const KcapArchive*> Assets::chain(std::string_view group) const {
    std::vector<const KcapArchive*> out;
    for (const auto& path : chainPaths(group)) {
        if (const KcapArchive* a = archive(path)) out.push_back(a);
    }
    return out;
}

std::optional<std::string> Assets::looseFile(std::string_view group, std::string_view name) const {
    // The engine reads `<group>\<name>` and takes it if it has any bytes (0x44c2c0 returns the
    // size). Platform file names are UTF-8.
    std::string path = sjisToUtf8(group.empty() ? name : std::string(group) + "/" + std::string(name));
    auto f = folder_->open(path);
    if (!f || f->size() == 0) return std::nullopt;
    return path;
}

std::optional<Assets::Hit> Assets::locate(std::string_view group, std::string_view name) const {
    if (auto path = looseFile(group, name)) return Hit{nullptr, nullptr, *path};
    if (group.empty()) return std::nullopt;
    for (const KcapArchive* a : chain(group)) {
        // The archive's first entry of the name; one without data sends the lookup to the next
        // archive, not to a later entry.
        const KcapEntry* e = a->find(name);
        if (e && e->size != 0 && (!e->packed() || a->unpackedSize(*e) != 0)) return Hit{a, e, {}};
    }
    return std::nullopt;
}

std::optional<Bytes> Assets::read(std::string_view group, std::string_view name) const {
    if (auto path = looseFile(group, name)) return folder_->open(*path)->readAll();
    if (group.empty()) return std::nullopt;
    for (const KcapArchive* a : chain(group)) {
        const KcapEntry* e = a->find(name);
        if (!e) continue;
        Bytes data = a->read(*e);
        if (!data.empty()) return data;
    }
    return std::nullopt;
}

std::optional<Bytes> Assets::readHead(std::string_view group, std::string_view name, size_t n) const {
    if (auto path = looseFile(group, name)) {
        auto f = folder_->open(*path);
        return f->readAt(0, std::min<uint64_t>(f->size(), n));
    }
    if (group.empty()) return std::nullopt;
    for (const KcapArchive* a : chain(group)) {
        const KcapEntry* e = a->find(name);
        if (!e) continue;
        Bytes data = a->readHead(*e, n);
        if (!data.empty()) return data;
    }
    return std::nullopt;
}

bool Assets::exists(std::string_view group, std::string_view name) const {
    if (!group.empty()) {
        std::string g(group);
        for (const std::string& path : {g + ".pak", "IC/" + g + ".pak", "pak/" + g + ".pak"}) {
            // 0x437010: any entry of the name, empty or not.
            if (const KcapArchive* a = archive(path); a && a->find(name)) return true;
        }
    }
    // 0x44c390: the file exists, whatever its size.
    std::string path = sjisToUtf8(group.empty() ? name : std::string(group) + "/" + std::string(name));
    return folder_->open(path) != nullptr;
}

std::vector<Assets::ArchiveInfo> Assets::kcapArchives() const {
    std::vector<ArchiveInfo> out;
    if (const KcapArchive* a = archive(kPatchArchive)) out.push_back({a->name(), {}, a});
    std::set<std::string> groups;
    auto collect = [&](const std::vector<std::string>& names) {
        for (const auto& n : names) {
            if (endsWithPak(n) && lower(n) != kPatchArchive) groups.insert(lower(stemOf(n)));
        }
    };
    collect(folder_->list());
    collect(folder_->listDir("IC"));
    collect(folder_->listDir("pak"));
    for (const auto& g : groups) {
        for (const auto& path : chainPaths(g)) {
            if (lower(path) == kPatchArchive) continue;
            if (const KcapArchive* a = archive(path)) out.push_back({a->name(), g, a});
        }
    }
    return out;
}

bool isGameFolder(const GameFolder& folder, const std::vector<std::string>& names) {
    for (const auto& n : names) {
        if (lower(n) != "script.pak") continue;
        auto f = folder.open(n);
        if (!f || f->size() < 4) return false;
        Bytes magic = f->readAt(0, 4);
        return std::memcmp(magic.data(), "KCAP", 4) == 0;
    }
    return false;
}

}  // namespace nyan::wa2
