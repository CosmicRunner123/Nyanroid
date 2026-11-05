#include "nyan/cs2/psb.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

#include "nyan/common/error.hpp"
#include "nyan/common/zlib_util.hpp"

namespace nyan::cs2 {
namespace {

// Header encryption of PSB version 3+: the bytes after the 8-byte signature are XORed with an
// XorShift128 stream seeded with the key (four bytes per output word, low byte first).
void decryptHeader(uint8_t* d, size_t begin, size_t end, uint32_t key) {
    uint32_t x = 123456789, y = 362436069, z = 521288629, w = key, pad = 0;
    for (size_t i = begin; i < end; ++i) {
        if ((i - begin) % 4 == 0) {
            uint32_t t = x ^ (x << 11);
            x = y, y = z, z = w;
            w = w ^ (w >> 19) ^ t ^ (t >> 8);
            pad = w;
        }
        d[i] ^= static_cast<uint8_t>(pad >> (8 * ((i - begin) % 4)));
    }
}

Bytes unwrap(ByteSpan file) {
    if (file.size() >= 16 && std::memcmp(file.data(), "PSZ\0", 4) == 0) {
        return zlibInflate(file.subspan(16), readLE32(file.data() + 8));
    }
    return Bytes(file.begin(), file.end());
}

constexpr size_t kHeaderWords = 12;  // header length, 7 offsets, checksum, 3 more (version 4)

bool headerEncrypted(const Bytes& d) {
    return d.size() >= 8 + 4 * kHeaderWords && readLE16(d.data() + 4) >= 3 && (readLE16(d.data() + 6) & 1);
}

// Decrypts the header and only the header: its first word is its own length (44 bytes in version
// 3, nine words; 56 in version 4), and the bytes after it are the name table, not encrypted.
// Decrypting a version 3 file's 56 bytes garbled the table's first entries (Amakano 1's models).
void decryptWholeHeader(uint8_t* d, size_t size, uint32_t key) {
    uint8_t head[12];
    std::memcpy(head, d, sizeof head);
    decryptHeader(head, 8, 12, key);
    const size_t len = std::clamp<size_t>(readLE32(head + 8), 12, 8 + 4 * kHeaderWords);
    decryptHeader(d, 8, std::min(len, size), key);
}

bool plausible(const Bytes& d, size_t total) {
    uint32_t h[8];
    for (int k = 0; k < 8; ++k) h[k] = readLE32(d.data() + 8 + 4 * k);
    if (h[0] < 0x20 || h[0] > 0x80) return false;
    for (int k = 1; k < 8; ++k) {
        if (h[k] < h[0] || h[k] > total) return false;
    }
    return h[2] <= h[3] && h[4] <= h[6] && h[5] <= h[6];
}

class Reader {
public:
    Reader(const Bytes& d) : d_(d) {}

    std::vector<int64_t> array(size_t& p) const {
        uint8_t t = at(p);
        if (t < 0x0D || t > 0x14) fail("PSB: bad array");
        size_t n = t - 0x0C;
        uint64_t count = uint(p + 1, n);
        p += 1 + n;
        size_t e = at(p) - 0x0C;
        if (e < 1 || e > 8) fail("PSB: bad array entry size");
        ++p;
        if (count > (d_.size() - p) / e) fail("PSB: array overruns the file");
        std::vector<int64_t> v(count);
        for (size_t i = 0; i < count; ++i) v[i] = static_cast<int64_t>(uint(p + i * e, e));
        p += count * e;
        return v;
    }

    uint64_t uint(size_t p, size_t n) const {
        if (p + n > d_.size()) fail("PSB: value overruns the file");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(d_[p + i]) << (8 * i);
        return v;
    }
    int64_t sint(size_t p, size_t n) const {
        uint64_t v = uint(p, n);
        if (n < 8 && (v >> (8 * n - 1)) & 1) v |= ~0ull << (8 * n);
        return static_cast<int64_t>(v);
    }
    uint8_t at(size_t p) const {
        if (p >= d_.size()) fail("PSB: offset outside the file");
        return d_[p];
    }

private:
    const Bytes& d_;
};

}  // namespace

const PsbValue* PsbValue::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    auto it = std::lower_bound(object.begin(), object.end(), key,
                               [](const auto& kv, std::string_view k) { return kv.first < k; });
    return it != object.end() && it->first == key ? &it->second : nullptr;
}

const PsbValue& PsbValue::operator[](std::string_view key) const {
    static const PsbValue kNull;
    const PsbValue* v = get(key);
    return v ? *v : kNull;
}

double PsbValue::num(std::string_view key, double fallback) const {
    const PsbValue* v = get(key);
    return v ? v->number(fallback) : fallback;
}

std::vector<double> PsbValue::numbers() const {
    std::vector<double> out;
    if (type == Type::IntArray) {
        for (int64_t v : ints) out.push_back(static_cast<double>(v));
    } else if (type == Type::List) {
        for (const auto& v : list) out.push_back(v.number());
    }
    return out;
}

bool Psb::keyFits(ByteSpan file, uint32_t key) {
    // The header is all that is needed; the offsets are checked against the whole size.
    bool psz = file.size() >= 16 && std::memcmp(file.data(), "PSZ\0", 4) == 0;
    Bytes d = psz ? zlibInflatePrefix(file.subspan(16), 256) : Bytes(file.begin(), file.begin() + std::min<size_t>(file.size(), 256));
    size_t total = psz ? readLE32(file.data() + 8) : file.size();
    if (d.size() < 8 + 4 * kHeaderWords || std::memcmp(d.data(), "PSB\0", 4) != 0) return false;
    if (!headerEncrypted(d)) return plausible(d, total);
    decryptWholeHeader(d.data(), d.size(), key);
    return plausible(d, total);
}

std::vector<uint32_t> Psb::keyCandidates(ByteSpan lib) {
    // NUL-terminated runs of 6-10 digits (emotedriver.dll 3.9: "413413189").
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < lib.size()) {
        size_t j = i;
        while (j < lib.size() && std::isdigit(lib[j])) ++j;
        size_t n = j - i;
        if (n >= 6 && n <= 10 && j < lib.size() && lib[j] == 0) {
            uint64_t v = std::stoull(std::string(reinterpret_cast<const char*>(lib.data() + i), n));
            if (v <= 0xFFFFFFFFull && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(static_cast<uint32_t>(v));
        }
        i = j + 1;
    }
    return out;
}

std::optional<Psb> Psb::load(ByteSpan file, uint32_t key) {
    Psb psb;
    psb.data_ = unwrap(file);
    Bytes& d = psb.data_;
    if (d.size() < 8 + 4 * kHeaderWords || std::memcmp(d.data(), "PSB\0", 4) != 0) return std::nullopt;
    if (headerEncrypted(d)) decryptWholeHeader(d.data(), d.size(), key);
    if (!plausible(d, d.size())) return std::nullopt;
    uint32_t oNames = readLE32(d.data() + 12), oStrings = readLE32(d.data() + 16), oStrData = readLE32(d.data() + 20);
    uint32_t oChunkOff = readLE32(d.data() + 24), oChunkLen = readLE32(d.data() + 28), oChunkData = readLE32(d.data() + 32);
    uint32_t oRoot = readLE32(d.data() + 36);
    Reader r(d);

    // Names: a trie of three arrays (character offsets, the node table, each name's end node).
    size_t p = oNames;
    auto charset = r.array(p), nodes = r.array(p), ends = r.array(p);
    std::vector<std::string> names;
    names.reserve(ends.size());
    for (int64_t e : ends) {
        std::string s;
        int64_t b = e < static_cast<int64_t>(nodes.size()) ? nodes[static_cast<size_t>(e)] : 0;
        for (int guard = 0; b != 0 && guard < 4096; ++guard) {
            if (b < 0 || static_cast<size_t>(b) >= nodes.size()) fail("PSB: bad name table");
            int64_t c = nodes[static_cast<size_t>(b)];
            if (c < 0 || static_cast<size_t>(c) >= charset.size()) fail("PSB: bad name table");
            s.push_back(static_cast<char>(b - charset[static_cast<size_t>(c)]));
            b = c;
        }
        std::reverse(s.begin(), s.end());
        names.push_back(std::move(s));
    }
    p = oStrings;
    auto strOffs = r.array(p);
    std::vector<std::string> strings;
    strings.reserve(strOffs.size());
    for (int64_t o : strOffs) {
        size_t a = oStrData + static_cast<size_t>(o);
        size_t e = a;
        while (e < d.size() && d[e] != 0) ++e;
        strings.emplace_back(reinterpret_cast<const char*>(d.data() + std::min(a, d.size())), e - std::min(a, d.size()));
    }
    p = oChunkOff;
    auto cOff = r.array(p);
    p = oChunkLen;
    auto cLen = r.array(p);
    for (size_t i = 0; i < cOff.size() && i < cLen.size(); ++i) {
        uint64_t a = static_cast<uint64_t>(oChunkData) + static_cast<uint64_t>(cOff[i]);
        if (a + static_cast<uint64_t>(cLen[i]) > d.size()) fail("PSB: resource outside the file");
        psb.resources_.emplace_back(static_cast<uint32_t>(a), static_cast<uint32_t>(cLen[i]));
    }

    std::function<PsbValue(size_t, int)> value = [&](size_t q, int depth) -> PsbValue {
        if (depth > 256) fail("PSB: nesting too deep");
        PsbValue v;
        uint8_t t = r.at(q);
        if (t == 1) return v;
        if (t == 2 || t == 3) {
            v.type = PsbValue::Type::Bool;
            v.i = t == 3;
        } else if (t == 4) {
            v.type = PsbValue::Type::Int;
        } else if (t >= 5 && t <= 0x0C) {
            v.type = PsbValue::Type::Int;
            v.i = r.sint(q + 1, t - 4);
        } else if (t >= 0x0D && t <= 0x14) {
            v.type = PsbValue::Type::IntArray;
            size_t a = q;
            v.ints = r.array(a);
        } else if (t >= 0x15 && t <= 0x18) {
            v.type = PsbValue::Type::String;
            uint64_t k = r.uint(q + 1, t - 0x14);
            if (k < strings.size()) v.s = strings[k];
        } else if ((t >= 0x19 && t <= 0x1C) || (t >= 0x22 && t <= 0x25)) {
            v.type = PsbValue::Type::Resource;
            v.i = static_cast<int64_t>(r.uint(q + 1, t >= 0x22 ? t - 0x21 : t - 0x18));
        } else if (t == 0x1D) {
            v.type = PsbValue::Type::Float;
        } else if (t == 0x1E) {
            v.type = PsbValue::Type::Float;
            uint32_t bits = static_cast<uint32_t>(r.uint(q + 1, 4));
            float fv;
            std::memcpy(&fv, &bits, 4);
            v.f = fv;
        } else if (t == 0x1F) {
            v.type = PsbValue::Type::Float;
            uint64_t bits = r.uint(q + 1, 8);
            std::memcpy(&v.f, &bits, 8);
        } else if (t == 0x20) {
            v.type = PsbValue::Type::List;
            size_t a = q + 1;
            auto offs = r.array(a);
            v.list.reserve(offs.size());
            for (int64_t o : offs) v.list.push_back(value(a + static_cast<size_t>(o), depth + 1));
        } else if (t == 0x21) {
            v.type = PsbValue::Type::Object;
            size_t a = q + 1;
            auto keys = r.array(a);
            auto offs = r.array(a);
            v.object.reserve(keys.size());
            for (size_t k = 0; k < keys.size() && k < offs.size(); ++k) {
                if (keys[k] < 0 || static_cast<size_t>(keys[k]) >= names.size()) fail("PSB: bad member name");
                v.object.emplace_back(names[static_cast<size_t>(keys[k])], value(a + static_cast<size_t>(offs[k]), depth + 1));
            }
            std::sort(v.object.begin(), v.object.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        } else {
            fail("PSB: unknown value type " + std::to_string(t));
        }
        return v;
    };
    psb.root_ = value(oRoot, 0);
    return psb;
}

ByteSpan Psb::resource(size_t index) const {
    if (index >= resources_.size()) return {};
    return ByteSpan(data_.data() + resources_[index].first, resources_[index].second);
}

}  // namespace nyan::cs2
