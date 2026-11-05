#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/wa2/kcap_archive.hpp"
#include "nyan/wa2/lac_archive.hpp"

namespace nyan::wa2 {

// The game's files as WA2.exe finds them. Pictures, scripts and fonts belong to archive groups
// named after their KCAP archives (`bak`, `char`, `grp`, `fnt`, `script`); sound is in LAC archives.
// Names are Shift-JIS, as the scripts and the engine spell them. Thread-safe.
// The engine builds Nyanroid follows, told apart by the folder's executable (its link time):
// WA2.exe (the main game, either 2011 build) and WA2_Special.exe / WA2_Special_en.exe (Special
// Contents, 2017-12-28). Where they differ, the code asks for the build.
enum class Build { Wa2, Special };

class Assets {
public:
    explicit Assets(std::unique_ptr<GameFolder> folder);

    const GameFolder& folder() const { return *folder_; }

    // Where a file of a group comes from: an archive entry, or (archive null) a loose file.
    struct Hit {
        const KcapArchive* archive = nullptr;
        const KcapEntry* entry = nullptr;
        std::string loosePath;  // UTF-8, relative to the game folder
    };
    // The engine's lookup (0x437610), the first place that has data: the loose file
    // `<group>/<name>`, the patch's en.pak (searched for every group), `<group>.pak`,
    // `IC/<group>.pak`, `pak/<group>.pak`. (After those the engine tries the folder of an
    // introductory chapter installed on its own, from the registry; there is no registry here.)
    // An empty group means the loose file `name` alone. nullopt if nothing has it.
    std::optional<Hit> locate(std::string_view group, std::string_view name) const;
    // The file locate() finds; nullopt if there is none (the engine logs an error).
    std::optional<Bytes> read(std::string_view group, std::string_view name) const;
    // The first `n` bytes of what read() finds (the lookup as read's).
    std::optional<Bytes> readHead(std::string_view group, std::string_view name, size_t n) const;
    // Whether the engine's existence check (0x4374b0) finds `name`: `<group>.pak`, `IC/<group>.pak`,
    // `pak/<group>.pak`, then the loose file. It does not look in en.pak.
    bool exists(std::string_view group, std::string_view name) const;

    // The KCAP archives a group's lookup reads after the loose file, in order (absent ones left out).
    std::vector<const KcapArchive*> chain(std::string_view group) const;

    struct ArchiveInfo {
        std::string path;   // relative to the game folder, as found
        std::string group;  // the group it serves; empty for en.pak, which serves every group
        const KcapArchive* archive = nullptr;
    };
    // Every KCAP archive in the folder, IC/ and pak/: en.pak first, then by group in lookup order.
    std::vector<ArchiveInfo> kcapArchives() const;

    // The sound archives in the engine's order (0x405e10, opened by 0x45d3f0): BGM, Se, Voice, then
    // the introductory chapter's IC/BGM, IC/Se, IC/Voice when it is bundled. A missing one is left
    // out, as the engine leaves it out of its numbering.
    const std::vector<std::unique_ptr<LacArchive>>& soundArchives() const { return sound_; }
    // The extended edition carries the introductory chapter in IC/; the engine checks for
    // IC/bak.pak at startup (0x432840).
    // Special Contents' engine keeps that flag (0x4bf380) at 1 whatever the folder holds.
    bool introductoryChapterBundled() const { return icBundled_ || build_ == Build::Special; }
    Build build() const { return build_; }

private:
    // The KCAP archive at `path` (opened once); nullptr if absent or not KCAP.
    const KcapArchive* archive(const std::string& path) const;
    std::vector<std::string> chainPaths(std::string_view group) const;
    std::optional<std::string> looseFile(std::string_view group, std::string_view name) const;

    std::unique_ptr<GameFolder> folder_;
    bool icBundled_ = false;
    Build build_ = Build::Wa2;
    std::vector<std::unique_ptr<LacArchive>> sound_;
    mutable std::mutex mu_;
    mutable std::map<std::string, std::unique_ptr<KcapArchive>> archives_;  // by lower-case path
};

// Whether `folder` holds a game of this engine: a KCAP archive named script.pak, where the engine
// keeps its scripts and their function tables (0x4178b0). `names` is folder.list().
bool isGameFolder(const GameFolder& folder, const std::vector<std::string>& names);

}  // namespace nyan::wa2
