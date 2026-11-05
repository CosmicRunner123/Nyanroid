#include "nyan/diesel/npk_archive.hpp"

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>

#include <zlib.h>
#include <zstd.h>

namespace nyan::diesel {

namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr uint32_t kMaxName = 0x104;

struct Header {
    uint8_t iv[16];
    uint32_t count = 0;
    uint32_t indexSize = 0;
    bool npk2 = false;
};

std::optional<Header> readHeader(const RandomAccessFile& file) {
    if (file.size() < kHeaderSize) return std::nullopt;
    uint8_t h[kHeaderSize];
    file.readAt(0, h, kHeaderSize);
    const bool npk3 = std::memcmp(h, "NPK3", 4) == 0 && readLE32(h + 4) == 1;
    const bool npk2 = std::memcmp(h, "NPK2", 4) == 0 && readLE32(h + 4) == 2;
    if (!npk3 && !npk2) return std::nullopt;
    Header out;
    out.npk2 = npk2;
    std::memcpy(out.iv, h + 8, 16);
    out.count = readLE32(h + 0x18);
    out.indexSize = readLE32(h + 0x1c);
    if (out.indexSize % 16 != 0 || kHeaderSize + uint64_t(out.indexSize) > file.size()) return std::nullopt;
    return out;
}

// A raw deflate stream of `size` bytes into exactly `unpacked` bytes (NPK2's packed segments).
bool inflateRaw(const uint8_t* src, size_t size, uint8_t* dst, size_t unpacked) {
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = const_cast<uint8_t*>(src);
    z.avail_in = static_cast<uInt>(size);
    z.next_out = dst;
    z.avail_out = static_cast<uInt>(unpacked);
    int rc = inflate(&z, Z_FINISH);
    bool ok = rc == Z_STREAM_END && z.total_out == unpacked;
    inflateEnd(&z);
    return ok;
}

}  // namespace

bool isNpkArchive(const RandomAccessFile& file) { return readHeader(file).has_value(); }

std::string normalizeName(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool NpkArchive::keyOpens(const RandomAccessFile& file, const NpkKey& key) {
    auto h = readHeader(file);
    if (!h || h->indexSize < 32) return false;
    Aes256 aes(key);
    // The first block: the first entry's flag and name length, then printable name bytes.
    uint8_t first[16];
    file.readAt(kHeaderSize, first, 16);
    aes.decryptCbc(first, 1, h->iv);
    uint32_t nameLength = readLE16(first + 1);
    if (first[0] > 1 || nameLength == 0 || nameLength > kMaxName) return false;
    for (uint32_t i = 3; i < 16 && i < 3 + nameLength; ++i) {
        if (first[i] < 0x20 || first[i] == 0x7f) return false;
    }
    // The last block: PKCS#7 padding.
    uint8_t last[32];
    file.readAt(kHeaderSize + h->indexSize - 32, last, 32);
    uint8_t plain[16];
    aes.decryptBlock(last + 16, plain);
    for (int i = 0; i < 16; ++i) plain[i] ^= last[i];
    uint8_t pad = plain[15];
    if (pad == 0 || pad > 16) return false;
    for (int i = 16 - pad; i < 16; ++i) {
        if (plain[i] != pad) return false;
    }
    return true;
}

NpkArchive::NpkArchive(std::unique_ptr<RandomAccessFile> file, std::string name, const NpkKey& key)
    : file_(std::move(file)), name_(std::move(name)), aes_(key) {
    auto h = readHeader(*file_);
    if (!h) fail(name_ + ": not an NPK3 / NPK2 archive");
    std::memcpy(iv_, h->iv, 16);
    deflate_ = h->npk2;
    Bytes index = file_->readAt(kHeaderSize, h->indexSize);
    aes_.decryptCbc(index.data(), index.size() / 16, iv_);

    ByteReader r(index, name_ + " index");
    entries_.reserve(h->count);
    for (uint32_t i = 0; i < h->count; ++i) {
        NpkEntry e;
        e.flag = r.u8() != 0;
        uint32_t nameLength = r.u16();
        if (nameLength > kMaxName) fail(name_ + ": bad index (wrong key?)");
        auto n = r.span(nameLength);
        e.name.assign(reinterpret_cast<const char*>(n.data()), n.size());
        e.size = r.u32();
        auto digest = r.span(32);
        std::memcpy(e.sha256.data(), digest.data(), 32);
        uint32_t segments = r.u32();
        if (segments > (r.remaining() / 0x14)) fail(name_ + ": bad index (wrong key?)");
        e.segments.reserve(segments);
        for (uint32_t s = 0; s < segments; ++s) {
            NpkSegment seg;
            uint32_t lo = r.u32(), hi = r.u32();
            seg.offset = (uint64_t(hi) << 32) | lo;
            seg.alignedSize = r.u32();
            seg.size = r.u32();
            seg.unpackedSize = r.u32();
            if (seg.alignedSize % 16 != 0 || seg.offset + seg.alignedSize > file_->size() ||
                (!seg.compressed() && seg.unpackedSize > seg.alignedSize) ||
                (seg.compressed() && seg.size > seg.alignedSize)) {
                fail(name_ + ": bad segment in " + e.name);
            }
            e.segments.push_back(seg);
        }
        // Mware keeps the first entry of a name within one archive (0x100b69f0 inserts only when
        // the name's digest is not there yet).
        index_.emplace(normalizeName(e.name), entries_.size());
        entries_.push_back(std::move(e));
    }
}

const NpkEntry* NpkArchive::find(std::string_view normalizedName) const {
    auto it = index_.find(std::string(normalizedName));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

Bytes NpkArchive::readSegment(const NpkEntry& entry, size_t index) const {
    const NpkSegment& seg = entry.segments.at(index);
    Bytes buf(seg.alignedSize);
    file_->readAt(seg.offset, buf.data(), buf.size());
    aes_.decryptCbc(buf.data(), buf.size() / 16, iv_);
    if (!seg.compressed()) {
        if (seg.unpackedSize > buf.size()) fail(name_ + ": " + entry.name + ": segment shorter than it says");
        buf.resize(seg.unpackedSize);
        return buf;
    }
    Bytes out(seg.unpackedSize);
    if (deflate_) {
        if (!inflateRaw(buf.data(), seg.size, out.data(), out.size())) fail(name_ + ": " + entry.name + ": inflate failed");
        return out;
    }
    size_t got = ZSTD_decompress(out.data(), out.size(), buf.data(), seg.size);
    if (ZSTD_isError(got) || got != seg.unpackedSize) {
        fail(name_ + ": " + entry.name + ": zstd: " + (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short segment"));
    }
    return out;
}

namespace {

class EntryFile final : public RandomAccessFile {
public:
    EntryFile(const NpkArchive& archive, const NpkEntry& entry) : archive_(archive), entry_(entry) {
        uint64_t at = 0;
        for (const NpkSegment& s : entry.segments) {
            starts_.push_back(at);
            at += s.unpackedSize;
        }
        size_ = std::min<uint64_t>(at, entry.size);
    }
    uint64_t size() const override { return size_; }
    void readAt(uint64_t offset, void* dst, size_t length) const override {
        if (offset > size_ || length > size_ - offset) fail(entry_.name + ": read past the end");
        auto* out = static_cast<uint8_t*>(dst);
        std::lock_guard lock(mutex_);
        while (length > 0) {
            size_t i = static_cast<size_t>(std::upper_bound(starts_.begin(), starts_.end(), offset) - starts_.begin()) - 1;
            const Bytes& seg = segment(i);
            uint64_t in = offset - starts_[i];
            size_t n = static_cast<size_t>(std::min<uint64_t>(length, seg.size() - in));
            if (n == 0) fail(entry_.name + ": empty segment");
            std::memcpy(out, seg.data() + in, n);
            out += n, offset += n, length -= n;
        }
    }

private:
    static constexpr size_t kKept = 4;
    const Bytes& segment(size_t i) const {
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first != i) continue;
            cache_.splice(cache_.begin(), cache_, it);
            return cache_.front().second;
        }
        cache_.emplace_front(i, archive_.readSegment(entry_, i));
        if (cache_.size() > kKept) cache_.pop_back();
        return cache_.front().second;
    }
    const NpkArchive& archive_;
    const NpkEntry& entry_;
    std::vector<uint64_t> starts_;
    uint64_t size_ = 0;
    mutable std::mutex mutex_;
    mutable std::list<std::pair<size_t, Bytes>> cache_;
};

}  // namespace

std::unique_ptr<RandomAccessFile> NpkArchive::open(const NpkEntry& entry) const {
    return std::make_unique<EntryFile>(*this, entry);
}

Bytes NpkArchive::read(const NpkEntry& entry) const {
    Bytes out(entry.size);
    size_t at = 0;
    Bytes buf;
    for (const NpkSegment& seg : entry.segments) {
        if (at + seg.unpackedSize > out.size()) fail(name_ + ": segments of " + entry.name + " overrun its size");
        buf.resize(seg.alignedSize);
        file_->readAt(seg.offset, buf.data(), buf.size());
        aes_.decryptCbc(buf.data(), buf.size() / 16, iv_);
        if (seg.compressed() && deflate_) {
            if (!inflateRaw(buf.data(), seg.size, out.data() + at, seg.unpackedSize)) {
                fail(name_ + ": " + entry.name + ": inflate failed");
            }
        } else if (seg.compressed()) {
            size_t got = ZSTD_decompress(out.data() + at, seg.unpackedSize, buf.data(), seg.size);
            if (ZSTD_isError(got) || got != seg.unpackedSize) {
                fail(name_ + ": " + entry.name + ": zstd: " +
                     (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short segment"));
            }
        } else {
            std::memcpy(out.data() + at, buf.data(), seg.unpackedSize);
        }
        at += seg.unpackedSize;
    }
    if (at != out.size()) fail(name_ + ": segments of " + entry.name + " fall short of its size");
    return out;
}

}  // namespace nyan::diesel
