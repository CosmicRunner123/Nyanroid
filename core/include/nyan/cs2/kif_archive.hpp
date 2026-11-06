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
};

// Unobfuscates a KIF entry name in place (Beaufort-style letter substitution).
void unobfuscateKifName(std::string& name, uint32_t seed);
// The substitution's first key for a seed (0-51), and the substitution from that key.
int kifNameKey(uint32_t seed);
void unobfuscateKifNameWithKey(std::string& name, int key);

}  // namespace nyan::cs2
