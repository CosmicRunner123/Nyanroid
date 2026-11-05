#pragma once

#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/npk_archive.hpp"

namespace nyan::diesel {

// The game's archives mounted as the exe mounts them and as Mware searches them.
//
// The exe (muramasa_en.exe 0x401970, totono_en.exe 0x401b70) mounts, each with the archive key:
// script, cg, dx, font, sound, voice, particle and system (.npk, in that order; a missing one is
// skipped), then each patch/*.npk, by lower-cased name. Mware inserts every archive at the front of
// its list (0x100b7090: deque::emplace at begin()) and a lookup returns the first archive that has the
// name (0x100b81e0), so the last archive mounted wins: patch003 over patch002 over the base archives.
class Archives {
public:
    Archives(const GameFolder& folder, const NpkKey& key);

    struct Found {
        const NpkArchive* archive = nullptr;
        const NpkEntry* entry = nullptr;
    };
    // `name` as the archives store it ("media/cg/bg/x.png"); matched as Mware matches (ASCII case).
    std::optional<Found> find(std::string_view name) const;
    std::optional<Bytes> read(std::string_view name) const;
    // The entry as a file read piece by piece (NpkArchive::open); null if no archive has it.
    std::unique_ptr<RandomAccessFile> open(std::string_view name) const;
    bool exists(std::string_view name) const { return find(name).has_value(); }

    // In mount order (the last one is searched first). Not for use while a script may mount.
    const std::vector<std::unique_ptr<NpkArchive>>& archives() const { return archives_; }

    // Mounting while the game runs (SQApp.mountFile / unmountFile / isMounted: the file manager,
    // Mware's CFileManager vtable +0x10 / +0x14 / +0x18, 0x10008400 / 0x10008520 / 0x10008610). Its
    // one handler, the NPK one, takes names containing ".npk" (0x10006c60) and a 32-byte key
    // (0x10006c10); its mount (0x100b7090) refuses a name it has and a file that does not open, and
    // puts the archive first in the search. 0 or -1. Names are the paths as given (the folder's,
    // matched as the folder matches them); the exe's own mounts are listed as it names them.
    int mount(const std::string& name, const uint8_t* key, size_t keyLength);
    int unmount(const std::string& name);
    bool isMounted(const std::string& name) const;

private:
    const GameFolder* folder_ = nullptr;
    std::vector<std::unique_ptr<NpkArchive>> archives_;
    std::vector<std::string> names_;    // each archive's mount name (with archives_)
    std::vector<std::string> mounted_;  // the file manager's list (isMounted)
    // Unmounted archives, kept: entries found and files opened on them stay valid.
    std::vector<std::unique_ptr<NpkArchive>> retired_;
    mutable std::shared_mutex mutex_;
};

// Bytes in memory as a file.
class MemoryFile final : public RandomAccessFile {
public:
    explicit MemoryFile(Bytes data) : data_(std::move(data)) {}
    uint64_t size() const override { return data_.size(); }
    void readAt(uint64_t offset, void* dst, size_t length) const override;

private:
    Bytes data_;
};

// The archives the exe mounts, in its order, as folder-relative names ("patch/patch001.npk").
std::vector<std::string> mountList(const GameFolder& folder);

}  // namespace nyan::diesel
