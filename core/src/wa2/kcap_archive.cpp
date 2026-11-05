#include "nyan/wa2/kcap_archive.hpp"

#include <algorithm>
#include <cstring>

namespace nyan::wa2 {

namespace {

constexpr size_t kHeaderSize = 16;
constexpr size_t kEntrySize = 0x2c;
constexpr size_t kNameSize = 24;
constexpr size_t kPackedHeader = 8;  // packed size, unpacked size
// Sanity limit for a packed entry's unpacked size (the largest in the game, a font atlas, is 49 MB).
constexpr uint32_t kMaxUnpacked = 512u << 20;

// A Shift-JIS lead byte: the next byte belongs to the same character.
bool sjisLead(uint8_t c) {
    return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

char foldAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto c = static_cast<uint8_t>(a[i]);
        if (sjisLead(c) && i + 1 < a.size()) {
            if (a[i] != b[i] || a[i + 1] != b[i + 1]) return false;
            ++i;
            continue;
        }
        if (foldAscii(a[i]) != foldAscii(b[i])) return false;
    }
    return true;
}

std::unique_ptr<KcapArchive> KcapArchive::open(std::unique_ptr<RandomAccessFile> file, std::string archiveName) {
    if (!file || file->size() < kHeaderSize) return nullptr;
    Bytes header = file->readAt(0, kHeaderSize);
    if (std::memcmp(header.data(), "KCAP", 4) != 0) return nullptr;
    uint32_t count = readLE32(header.data() + 12);
    if (kHeaderSize + static_cast<uint64_t>(count) * kEntrySize > file->size()) {
        fail(archiveName + ": entry table larger than the file");
    }

    auto archive = std::unique_ptr<KcapArchive>(new KcapArchive());
    archive->name_ = std::move(archiveName);
    Bytes table = file->readAt(kHeaderSize, count * kEntrySize);
    ByteReader r(table, "KCAP entry table");
    archive->entries_.resize(count);
    for (auto& e : archive->entries_) {
        e.type = r.u32();
        e.name = r.fixedString(kNameSize);
        r.skip(8);
        e.offset = r.u32();
        e.size = r.u32();
    }
    archive->file_ = std::move(file);
    return archive;
}

const KcapEntry* KcapArchive::find(std::string_view name) const {
    for (const auto& e : entries_) {
        if (sameName(e.name, name)) return &e;
    }
    return nullptr;
}

uint32_t KcapArchive::unpackedSize(const KcapEntry& entry) const {
    if (!entry.packed() || entry.size < kPackedHeader) return entry.packed() ? 0 : entry.size;
    Bytes h = file_->readAt(entry.offset, kPackedHeader);
    return readLE32(h.data() + 4);
}

Bytes KcapArchive::readHead(const KcapEntry& entry, size_t n) const {
    if (static_cast<uint64_t>(entry.offset) + entry.size > file_->size()) {
        fail(name_ + ": " + entry.name + " lies past the end of the archive");
    }
    if (!entry.packed()) return file_->readAt(entry.offset, std::min<size_t>(entry.size, n));
    if (entry.size < kPackedHeader) return {};
    // A literal costs 9 bits, so 2n + 8 packed bytes always cover n unpacked ones.
    size_t take = std::min<size_t>(entry.size, kPackedHeader + 2 * n + 8);
    Bytes raw = file_->readAt(entry.offset, take);
    uint32_t unpacked = readLE32(raw.data() + 4);
    LzssResult result = lzssDecompress(ByteSpan(raw).subspan(kPackedHeader), std::min<size_t>(unpacked, n));
    result.data.resize(std::min<size_t>(result.produced, std::min<size_t>(unpacked, n)));
    return std::move(result.data);
}

Bytes KcapArchive::read(const KcapEntry& entry, LzssResult* report) const {
    if (static_cast<uint64_t>(entry.offset) + entry.size > file_->size()) {
        fail(name_ + ": " + entry.name + " lies past the end of the archive");
    }
    if (!entry.packed()) return file_->readAt(entry.offset, entry.size);
    // A packed entry too short for its header holds nothing (the engine would read past its
    // buffer; no lookup reaches the ones that exist, the folder names packed as empty files).
    if (entry.size < kPackedHeader) {
        if (report) *report = {};
        return {};
    }
    Bytes raw = file_->readAt(entry.offset, entry.size);
    // The input is the entry's size from the table less the header; the header's own packed size
    // is not read (0x437100).
    uint32_t unpacked = readLE32(raw.data() + 4);
    if (unpacked > kMaxUnpacked) fail(name_ + ": " + entry.name + " claims " + std::to_string(unpacked) + " bytes");
    LzssResult result = lzssDecompress(ByteSpan(raw).subspan(kPackedHeader), unpacked);
    if (report) {
        report->produced = result.produced;
        report->readStaleTail = result.readStaleTail;
    }
    // The engine's read returns the count the stream produced (0x437100 logs a mismatch with the
    // header and goes on); a stream that produced nothing counts as no file.
    result.data.resize(std::min<size_t>(result.produced, unpacked));
    return std::move(result.data);
}

}  // namespace nyan::wa2
