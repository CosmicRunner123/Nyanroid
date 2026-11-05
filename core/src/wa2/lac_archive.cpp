#include "nyan/wa2/lac_archive.hpp"

#include <algorithm>
#include <cstring>

#include "nyan/wa2/kcap_archive.hpp"

namespace nyan::wa2 {

namespace {

constexpr size_t kHeaderSize = 8;
constexpr size_t kEntrySize = 40;
constexpr size_t kNameSize = 32;

}  // namespace

std::unique_ptr<LacArchive> LacArchive::open(std::unique_ptr<RandomAccessFile> file, std::string archiveName) {
    if (!file || file->size() < kHeaderSize) return nullptr;
    Bytes header = file->readAt(0, kHeaderSize);
    if (std::memcmp(header.data(), "LAC\0", 4) != 0) return nullptr;
    uint32_t count = readLE32(header.data() + 4);
    if (kHeaderSize + static_cast<uint64_t>(count) * kEntrySize > file->size()) return nullptr;

    auto archive = std::unique_ptr<LacArchive>(new LacArchive());
    archive->name_ = std::move(archiveName);
    Bytes table = file->readAt(kHeaderSize, count * kEntrySize);
    // The engine inverts each entry's bytes from its start while they are not zero (it measures the
    // string again after each byte, so an inverted 0xff ends it too).
    for (size_t at = 0; at < table.size(); at += kEntrySize) {
        for (size_t i = at; i < table.size() && table[i] != 0; ++i) {
            table[i] = static_cast<uint8_t>(~table[i]);
            if (table[i] == 0) break;
        }
    }
    ByteReader r(table, "LAC entry table");
    archive->entries_.resize(count);
    for (auto& e : archive->entries_) {
        e.name = r.fixedString(kNameSize);
        e.size = r.u32();
        e.offset = r.u32();
    }
    for (size_t i = 0; i < archive->entries_.size(); ++i) {
        archive->index_.emplace(toLowerAscii(archive->entries_[i].name), static_cast<int>(i));
    }
    archive->file_ = std::move(file);
    return archive;
}

int LacArchive::indexOf(std::string_view name) const {
    auto it = index_.find(toLowerAscii(std::string(name)));
    return it == index_.end() ? -1 : it->second;
}

const LacEntry* LacArchive::find(std::string_view name) const {
    int i = indexOf(name);
    return i < 0 ? nullptr : &entries_[static_cast<size_t>(i)];
}

Bytes LacArchive::read(const LacEntry& entry) const {
    if (static_cast<uint64_t>(entry.offset) + entry.size > file_->size()) {
        fail(name_ + ": " + entry.name + " lies past the end of the archive");
    }
    return file_->readAt(entry.offset, entry.size);
}

Bytes LacArchive::readPart(const LacEntry& entry, uint32_t from, uint32_t size) const {
    if (from >= entry.size) return {};
    size = std::min(size, entry.size - from);
    if (static_cast<uint64_t>(entry.offset) + from + size > file_->size()) {
        fail(name_ + ": " + entry.name + " lies past the end of the archive");
    }
    return file_->readAt(entry.offset + from, size);
}

}  // namespace nyan::wa2
