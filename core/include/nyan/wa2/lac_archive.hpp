#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nyan/common/file_source.hpp"

namespace nyan::wa2 {

struct LacEntry {
    std::string name;  // Shift-JIS, decoded (the archive stores each byte inverted)
    uint32_t size = 0;
    uint32_t offset = 0;
};

// A LAC sound archive (BGM.PAK, SE.PAK, VOICE.PAK; WA2.exe 0x45d3f0): `LAC\0`, the entry count, and
// entries of 40 bytes (name[32], size, offset). Names are stored with each byte inverted up to the
// first zero byte. Entries are stored as they are (Ogg Vorbis, WAV).
class LacArchive {
public:
    // nullptr if the file is not a LAC archive: no magic, or an entry table that would not fit in
    // it. (The engine reads the count and the entries without checking the magic.)
    static std::unique_ptr<LacArchive> open(std::unique_ptr<RandomAccessFile> file, std::string archiveName);

    const std::string& name() const { return name_; }
    const std::vector<LacEntry>& entries() const { return entries_; }

    // The entry of this name, either case. The engine binary-searches the table with a comparator
    // that folds ASCII letters (0x458620, 0x433d10); every table of this data is sorted that way, so
    // a case-insensitive lookup finds the same entries.
    const LacEntry* find(std::string_view name) const;
    // Its index; -1 if none.
    int indexOf(std::string_view name) const;
    Bytes read(const LacEntry& entry) const;
    // `size` bytes of an entry from `from` (clipped to the entry).
    Bytes readPart(const LacEntry& entry, uint32_t from, uint32_t size) const;

private:
    LacArchive() = default;

    std::string name_;
    std::unique_ptr<RandomAccessFile> file_;
    std::vector<LacEntry> entries_;
    std::unordered_map<std::string, int> index_;  // lower-case name -> the first entry
};

}  // namespace nyan::wa2
