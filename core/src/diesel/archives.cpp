#include "nyan/diesel/archives.hpp"

#include <cstring>

#include <algorithm>
#include <mutex>
#include <set>

namespace nyan::diesel {

std::vector<std::string> mountList(const GameFolder& folder) {
    // The exes' list (muramasa_en.exe 0x401970); Saya_en.exe's has N4_sys.npk where the others have
    // particle.npk (0x402003 .. 0x402279), and none has both.
    static const char* const kBase[] = {"script.npk", "cg.npk",       "dx.npk",     "font.npk",  "sound.npk",
                                        "voice.npk",  "particle.npk", "N4_sys.npk", "system.npk"};
    std::vector<std::string> out;
    for (const char* name : kBase) {
        if (!folder.resolve(name).empty()) out.push_back(name);  // named as the exe names it
    }
    // FindFirstFileA("patch/*.npk"), names lower-cased into an ordered set, mounted in its order.
    std::set<std::string> patches;
    for (const auto& n : folder.listDir("patch")) {
        std::string lower = toLowerAscii(n);
        if (lower.size() > 4 && lower.ends_with(".npk")) patches.insert(lower);
    }
    for (const auto& p : patches) out.push_back("patch/" + p);
    return out;
}

Archives::Archives(const GameFolder& folder, const NpkKey& key) : folder_(&folder) {
    for (const auto& name : mountList(folder)) {
        auto file = folder.open(name);
        if (!file) continue;
        archives_.push_back(std::make_unique<NpkArchive>(std::move(file), name, key));
        names_.push_back(name);
        mounted_.push_back(name);
    }
    if (archives_.empty()) fail(folder.displayName() + ": no archives");
}

int Archives::mount(const std::string& name, const uint8_t* key, size_t keyLength) {
    if (name.find(".npk") == std::string::npos || !key || keyLength != 32) return -1;
    std::unique_lock lock(mutex_);
    if (std::find(names_.begin(), names_.end(), name) != names_.end()) return -1;
    auto file = folder_->open(name);
    if (!file) return -1;
    NpkKey k;
    std::memcpy(k.data(), key, k.size());
    std::unique_ptr<NpkArchive> archive;
    try {
        archive = std::make_unique<NpkArchive>(std::move(file), name, k);
    } catch (const std::exception&) {
        return -1;  // Fitted: an index the key does not open refuses the mount
    }
    archives_.push_back(std::move(archive));
    names_.push_back(name);
    if (std::find(mounted_.begin(), mounted_.end(), name) == mounted_.end()) mounted_.push_back(name);
    return 0;
}

int Archives::unmount(const std::string& name) {
    if (name.find(".npk") == std::string::npos) return -1;
    std::unique_lock lock(mutex_);
    auto it = std::find(names_.begin(), names_.end(), name);
    if (it == names_.end()) return -1;
    size_t i = static_cast<size_t>(it - names_.begin());
    retired_.push_back(std::move(archives_[i]));
    archives_.erase(archives_.begin() + static_cast<std::ptrdiff_t>(i));
    names_.erase(it);
    std::erase(mounted_, name);
    return 0;
}

bool Archives::isMounted(const std::string& name) const {
    std::shared_lock lock(mutex_);
    return std::find(mounted_.begin(), mounted_.end(), name) != mounted_.end();
}

std::optional<Archives::Found> Archives::find(std::string_view name) const {
    std::shared_lock lock(mutex_);
    std::string key = normalizeName(name);
    for (auto it = archives_.rbegin(); it != archives_.rend(); ++it) {
        if (const NpkEntry* e = (*it)->find(key)) return Found{it->get(), e};
    }
    return std::nullopt;
}

std::unique_ptr<RandomAccessFile> Archives::open(std::string_view name) const {
    auto f = find(name);
    if (!f) return nullptr;
    return f->archive->open(*f->entry);
}

void MemoryFile::readAt(uint64_t offset, void* dst, size_t length) const {
    if (offset > data_.size() || length > data_.size() - offset) fail("memory file: read past the end");
    if (length > 0) std::memcpy(dst, data_.data() + offset, length);
}

std::optional<Bytes> Archives::read(std::string_view name) const {
    auto f = find(name);
    if (!f) return std::nullopt;
    return f->archive->read(*f->entry);
}

}  // namespace nyan::diesel
