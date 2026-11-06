#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/cs2/blowfish.hpp"

namespace nyan::cs2 {

struct KifEntry {
    std::string name;  // Shift-JIS, as stored in the archive (after unobfuscation)
    uint32_t offset = 0;
    uint32_t length = 0;
};

// A KIF (*.int) archive. Entry data is read lazily from the underlying file.
class KifArchive {
public:
    // `tocSeed` (from V_CODE2) is required only if the archive is encrypted.
    static std::unique_ptr<KifArchive> open(std::unique_ptr<RandomAccessFile> file, std::string archiveName,
                                            std::optional<uint32_t> tocSeed);

    // A game folder's subfolder taken as an archive of the files given (names as listed), each
    // read whole when asked for: the engine opens a file type's file from its folder before its
    // archive (AssetIndex::load). `folder` must outlive the archive.
    static std::unique_ptr<KifArchive> openFolder(const GameFolder& folder, std::string dir,
                                                  const std::vector<std::string>& files);
    bool loose() const { return folder_ != nullptr; }

    // The entry names as the table stores them (obfuscated when the archive is encrypted: name i
    // is unobfuscated with the names' seed + i), and whether the archive is encrypted.
    static std::vector<std::string> rawNames(const RandomAccessFile& file, bool& encrypted);

    const std::string& name() const { return name_; }
    const std::vector<KifEntry>& entries() const { return entries_; }
    bool encrypted() const { return blowfish_.has_value(); }

    // Reads and decrypts an entry's data.
    Bytes read(const KifEntry& entry) const;
    // The entry as a file read on demand, decrypted as it is read (for large ones, such as
    // movies). The archive must outlive it; reads may come from several threads.
    std::shared_ptr<RandomAccessFile> openEntry(const KifEntry& entry) const;

private:
    KifArchive() = default;

    std::string name_;
    std::unique_ptr<RandomAccessFile> file_;
    std::vector<KifEntry> entries_;
    std::optional<Blowfish> blowfish_;
    const GameFolder* folder_ = nullptr;  // a loose folder: entry i is the file paths_[i]
    std::vector<std::string> paths_;
    std::string loosePath(const KifEntry& entry) const;
};

// Unobfuscates a KIF entry name in place (Beaufort-style letter substitution).
void unobfuscateKifName(std::string& name, uint32_t seed);
// The substitution's first key for a seed (0-51), and the substitution from that key.
int kifNameKey(uint32_t seed);
void unobfuscateKifNameWithKey(std::string& name, int key);

}  // namespace nyan::cs2
