#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/wa2/lzss.hpp"

namespace nyan::wa2 {

struct KcapEntry {
    // 0: stored; anything else: LZSS. The patch's en.pak marks folder names with 0xcccccccc.
    uint32_t type = 0;
    std::string name;  // Shift-JIS, as stored
    uint32_t offset = 0;
    uint32_t size = 0;  // bytes in the archive (a packed entry's 8-byte header included)

    // en.pak's folder names: the engine treats them as entries no lookup matches, so an archive's
    // names form one flat set whatever folder the packer put them in.
    bool folder() const { return type == 0xccccccccu; }
    bool packed() const { return type != 0; }
};

// A KCAP (*.pak) archive (WA2.exe 0x437100): a 16-byte header (`KCAP`, two words the engine does
// not read, the entry count) and entries of 0x2c bytes (type, name[24], two words the engine does
// not read, offset, size). Entry data is read from the file when asked for.
class KcapArchive {
public:
    // nullptr if the file is not a KCAP archive: the engine checks the magic and nothing else.
    static std::unique_ptr<KcapArchive> open(std::unique_ptr<RandomAccessFile> file, std::string archiveName);

    const std::string& name() const { return name_; }
    const std::vector<KcapEntry>& entries() const { return entries_; }

    // The first entry whose name matches, as the engine's lstrcmpiA compares them (0x437100,
    // 0x437010); nullptr if none does.
    const KcapEntry* find(std::string_view name) const;
    // A packed entry's unpacked size (its header's second word); a stored entry's size.
    uint32_t unpackedSize(const KcapEntry& entry) const;
    // An entry's data: the stored bytes, or what the LZSS stream produces, up to the size its header
    // gives. `report`, if given, receives the decoder's result (sizes, stale ring reads) without the
    // data.
    Bytes read(const KcapEntry& entry, LzssResult* report = nullptr) const;
    // At most the first `n` bytes of an entry, unpacking only as far as they need.
    Bytes readHead(const KcapEntry& entry, size_t n) const;

private:
    KcapArchive() = default;

    std::string name_;
    std::unique_ptr<RandomAccessFile> file_;
    std::vector<KcapEntry> entries_;
};

// Whether two Shift-JIS names are the same as the engine's lstrcmpiA finds them: ASCII letters
// match either case, double-byte characters only as they are.
bool sameName(std::string_view a, std::string_view b);

}  // namespace nyan::wa2
