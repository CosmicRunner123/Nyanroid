#include "nyan/cs2/kif_archive.hpp"

#include <algorithm>
#include <cstring>

#include "nyan/common/error.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/cs2/mersenne_twister.hpp"

namespace nyan::cs2 {
namespace {

constexpr size_t kEntrySize = 72;  // char[64] name, u32 offset, u32 length
constexpr const char* kKeyEntryName = "__key__.dat";

}  // namespace

int kifNameKey(uint32_t seed) {
    uint32_t mt = MersenneTwister::first(seed);
    return static_cast<uint8_t>((mt >> 24) + (mt >> 16) + (mt >> 8) + mt) % 52;
}

void unobfuscateKifName(std::string& name, uint32_t seed) { unobfuscateKifNameWithKey(name, kifNameKey(seed)); }

void unobfuscateKifNameWithKey(std::string& name, int key) {
    constexpr int kLength = 52;
    constexpr int kHalf = 26;
    for (char& ch : name) {
        auto c = static_cast<uint8_t>(ch);
        bool upper = c >= 'A' && c <= 'Z';
        bool lower = c >= 'a' && c <= 'z';
        if (upper || lower) {
            // Map A-Z -> 0..25, a-z -> 26..51, shift, then mirror back onto z..a, Z..A.
            int idx = upper ? c - 'A' : c - 'a' + kHalf;
            idx = (idx + key) % kLength;
            ch = static_cast<char>(idx < kHalf ? 'z' - idx : 'Z' - (idx - kHalf));
        }
        key = (key + 1) % kLength;
    }
}

std::vector<std::string> KifArchive::rawNames(const RandomAccessFile& file, bool& encrypted) {
    encrypted = false;
    if (file.size() < 8) return {};
    Bytes header = file.readAt(0, 8);
    if (std::memcmp(header.data(), "KIF\0", 4) != 0) return {};
    uint32_t count = readLE32(header.data() + 4);
    if (static_cast<uint64_t>(count) * kEntrySize + 8 > file.size()) return {};
    Bytes table = file.readAt(8, count * kEntrySize);
    ByteReader r(table, "KIF entry table");
    std::vector<std::string> names(count);
    for (uint32_t i = 0; i < count; ++i) {
        names[i] = r.fixedString(64);
        r.u32();
        r.u32();
        if (names[i] == kKeyEntryName) encrypted = true;
    }
    return names;
}

std::unique_ptr<KifArchive> KifArchive::open(std::unique_ptr<RandomAccessFile> file, std::string archiveName,
                                             std::optional<uint32_t> tocSeed) {
    auto archive = std::unique_ptr<KifArchive>(new KifArchive());
    archive->name_ = std::move(archiveName);

    Bytes header = file->readAt(0, 8);
    if (std::memcmp(header.data(), "KIF\0", 4) != 0) fail(archive->name_ + ": not a KIF archive");
    uint32_t count = readLE32(header.data() + 4);
    if (static_cast<uint64_t>(count) * kEntrySize + 8 > file->size()) {
        fail(archive->name_ + ": entry table larger than file");
    }

    Bytes table = file->readAt(8, count * kEntrySize);
    ByteReader r(table, "KIF entry table");

    struct RawEntry {
        std::string name;
        uint32_t offset, length;
    };
    std::vector<RawEntry> raw(count);
    int keyIndex = -1;
    for (uint32_t i = 0; i < count; ++i) {
        raw[i].name = r.fixedString(64);
        raw[i].offset = r.u32();
        raw[i].length = r.u32();
        if (keyIndex < 0 && raw[i].name == kKeyEntryName) keyIndex = static_cast<int>(i);
    }

    if (keyIndex >= 0) {
        if (!tocSeed) fail(archive->name_ + ": archive is encrypted but no V_CODE2 was found");
        uint32_t fileKey = MersenneTwister::first(raw[keyIndex].length);
        uint8_t keyBytes[4];
        writeLE32(keyBytes, fileKey);
        archive->blowfish_.emplace(ByteSpan(keyBytes, 4));
    }

    archive->entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<int>(i) == keyIndex) continue;
        KifEntry e{std::move(raw[i].name), raw[i].offset, raw[i].length};
        if (archive->blowfish_) {
            unobfuscateKifName(e.name, *tocSeed + i);
            uint32_t l = e.offset + i, rr = e.length;
            archive->blowfish_->decryptBlock(l, rr);
            e.offset = l;
            e.length = rr;
        }
        if (static_cast<uint64_t>(e.offset) + e.length > file->size()) {
            fail(archive->name_ + ": entry '" + e.name + "' lies outside the archive (bad key?)");
        }
        archive->entries_.push_back(std::move(e));
    }

    archive->file_ = std::move(file);
    return archive;
}

std::unique_ptr<KifArchive> KifArchive::openFolder(const GameFolder& folder, std::string dir,
                                                   const std::vector<std::string>& files) {
    auto archive = std::unique_ptr<KifArchive>(new KifArchive());
    archive->name_ = dir + "/";
    archive->folder_ = &folder;
    for (const auto& f : files) {
        // Scripts name files in Shift-JIS, as the archives store them; listings are UTF-8.
        std::string sjis = utf8ToSjis(f);
        archive->entries_.push_back({sjisToUtf8(sjis) == f ? sjis : f, 0, 0});
        archive->paths_.push_back(dir + "/" + f);
    }
    return archive;
}

std::string KifArchive::loosePath(const KifEntry& entry) const {
    const size_t i = static_cast<size_t>(&entry - entries_.data());
    if (i >= paths_.size()) throw Error("Not an entry of " + name_);
    return paths_[i];
}

Bytes KifArchive::read(const KifEntry& entry) const {
    if (folder_) {
        auto file = folder_->open(loosePath(entry));
        if (!file) throw Error("Cannot open " + loosePath(entry));
        return file->readAll();
    }
    Bytes data = file_->readAt(entry.offset, entry.length);
    if (blowfish_) blowfish_->decrypt(data.data(), data.size());
    return data;
}

namespace {

// An entry read in place. Blowfish runs over the entry's whole 8-byte blocks from its start (a
// shorter tail is stored plain), so a read decrypts the blocks it touches.
class EntryFile final : public RandomAccessFile {
public:
    EntryFile(const RandomAccessFile& file, const Blowfish* blowfish, KifEntry entry)
        : file_(file), blowfish_(blowfish), entry_(std::move(entry)) {}
    uint64_t size() const override { return entry_.length; }
    void readAt(uint64_t offset, void* dst, size_t length) const override {
        if (length == 0) return;
        if (offset + length > entry_.length) throw Error("Read past the end of " + entry_.name);
        auto* out = static_cast<uint8_t*>(dst);
        if (!blowfish_) {
            file_.readAt(entry_.offset + offset, out, length);
            return;
        }
        const uint64_t blocksEnd = entry_.length & ~uint64_t{7};
        const uint64_t begin = offset & ~uint64_t{7};
        const uint64_t end = std::min<uint64_t>(entry_.length, (offset + length + 7) & ~uint64_t{7});
        std::vector<uint8_t> buf(static_cast<size_t>(end - begin));
        file_.readAt(entry_.offset + begin, buf.data(), buf.size());
        blowfish_->decrypt(buf.data(), static_cast<size_t>(std::min(end, blocksEnd) - std::min(begin, blocksEnd)));
        std::memcpy(out, buf.data() + (offset - begin), length);
    }

private:
    const RandomAccessFile& file_;
    const Blowfish* blowfish_;
    KifEntry entry_;
};

}  // namespace

std::shared_ptr<RandomAccessFile> KifArchive::openEntry(const KifEntry& entry) const {
    if (folder_) return std::shared_ptr<RandomAccessFile>(folder_->open(loosePath(entry)));
    return std::make_shared<EntryFile>(*file_, blowfish_ ? &*blowfish_ : nullptr, entry);
}

}  // namespace nyan::cs2
