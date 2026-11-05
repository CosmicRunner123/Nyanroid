#include "nyan/cs2/dictionary.hpp"

#include <cstring>

#include "nyan/common/zlib_util.hpp"

namespace nyan::cs2 {

namespace {
uint32_t u32(const Bytes& d, size_t o) {
    return o + 4 <= d.size() ? static_cast<uint32_t>(d[o]) | static_cast<uint32_t>(d[o + 1]) << 8 |
                                   static_cast<uint32_t>(d[o + 2]) << 16 | static_cast<uint32_t>(d[o + 3]) << 24
                             : 0;
}

std::string field(const Bytes& d, size_t o, size_t size) {
    if (o >= d.size()) return {};
    size = std::min(size, d.size() - o);
    const char* p = reinterpret_cast<const char*>(d.data() + o);
    return std::string(p, strnlen(p, size));
}

// The first character's code (cs2.exe 0x6a6fe0): a Shift-JIS lead byte with its trail, else the byte.
uint16_t firstChar(const std::string& s) {
    if (s.empty()) return 0;
    auto b = static_cast<uint8_t>(s[0]);
    bool lead = (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
    if (lead && s.size() >= 2) return static_cast<uint16_t>(b << 8 | static_cast<uint8_t>(s[1]));
    return b;
}
}  // namespace

bool Dictionary::load(const Bytes& kcdb) {
    if (kcdb.size() < 12 || std::memcmp(kcdb.data(), "KCDB", 4) != 0) return false;
    Bytes d;
    try {
        d = zlibInflate(ByteSpan(kcdb.data() + 12, kcdb.size() - 12), u32(kcdb, 8));
    } catch (...) {
        return false;
    }
    struct Column {
        std::string name;
        uint32_t type, size;
    };
    std::vector<Column> cols;
    size_t o = 4;
    for (uint32_t i = 0, n = u32(d, 0); i < n && o + 24 <= d.size(); ++i, o += 24) {
        cols.push_back({field(d, o, 16), u32(d, o + 16), u32(d, o + 20)});
    }
    const uint32_t rows = u32(d, o), rowSize = u32(d, o + 4);
    o += 8;
    entries_.clear();
    for (uint32_t r = 0; r < rows && o + rowSize <= d.size(); ++r, o += rowSize) {
        Entry e;
        std::string key;
        size_t p = o;
        for (const auto& c : cols) {
            if (c.type == 0) {
                if (c.name == "ID") e.id = static_cast<int32_t>(u32(d, p));
            } else {
                std::string v = field(d, p, c.size);
                if (c.name == "TITLE") e.title = v;
                else if (c.name == "TITLE_S") e.titleS = v;
                else if (c.name == "INDEX") key = v;
                else if (c.name.starts_with("IND")) {
                    int k = std::atoi(c.name.c_str() + 3);
                    if (k >= 1 && k <= 24) e.ind[static_cast<size_t>(k - 1)] = v;
                }
            }
            p += c.size;
        }
        // The engine maps a few characters of the key for its order (0x82aa80: kana tables);
        // the INDEX keys here are digits, which it leaves alone.
        if (!key.empty()) entries_[key] = std::move(e);
    }
    return !entries_.empty();
}

void Dictionary::reg(int group, const std::string& key) {
    auto it = entries_.find(key);
    if (it == entries_.end() || group < 0 || group >= 32) return;
    Entry& e = it->second;
    const uint32_t bit = 1u << group;
    if (e.appear == 0) {
        e.fresh = true;
        updated_ = true;
    }
    if (!(e.appear & bit)) {
        e.appear |= bit;
        e.updated |= bit;
        updated_ = true;
    }
}

bool Dictionary::find(const std::string& from, const std::string& to, int32_t n, std::string& key) const {
    // Entries in key order whose first character lies in [from, to]: the n-th of them.
    const uint16_t lo = firstChar(from), hi = firstChar(to);
    int32_t k = 0;
    auto it = entries_.begin();
    while (it != entries_.end() && !(lo <= firstChar(it->first) && firstChar(it->first) <= hi)) ++it;
    for (; it != entries_.end(); ++it, ++k) {
        if (firstChar(it->first) > hi) return false;
        if (k == n) {
            key = it->first;
            return true;
        }
    }
    return false;
}

uint32_t Dictionary::maxAppear(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return 0;
    uint32_t m = 0;
    for (size_t k = 0; k < it->second.ind.size(); ++k) {
        if (!it->second.ind[k].empty()) m |= 1u << k;
    }
    return m;
}

uint32_t Dictionary::appear(const std::string& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.appear & maxAppear(key);
}

bool Dictionary::title(const std::string& key, std::string& t, std::string& ts) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    t = it->second.title;
    ts = it->second.titleS;
    return true;
}

std::string Dictionary::ind(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return {};
    std::string out;
    for (size_t k = 0; k < it->second.ind.size(); ++k) {
        if ((it->second.appear >> k & 1) && !it->second.ind[k].empty()) out += first_ + it->second.ind[k] + between_;
    }
    return out;
}

bool Dictionary::update(const std::string& key, int32_t& fresh, int32_t& updated) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    fresh = it->second.fresh ? 1 : 0;
    updated = static_cast<int32_t>(it->second.updated);
    return true;
}

void Dictionary::updateClear(const std::string& key) {
    for (auto& [k, e] : entries_) {
        if (key.empty() || k == key) e.fresh = false, e.updated = 0;
    }
}

int32_t Dictionary::position(const std::string& key) const {
    int32_t n = 1;  // 0x82b4be: the first entry is 1; a key not found gives 0
    for (const auto& [k, e] : entries_) {
        if (k == key) return n;
        ++n;
    }
    return 0;
}

std::map<std::string, Dictionary::State> Dictionary::state() const {
    std::map<std::string, State> s;
    for (const auto& [k, e] : entries_) {
        if (e.appear || e.updated || e.fresh) s[k] = {e.appear, e.updated, e.fresh};
    }
    return s;
}

void Dictionary::restore(const std::map<std::string, State>& s) {
    for (auto& [k, e] : entries_) {
        auto it = s.find(k);
        e.appear = it == s.end() ? 0 : it->second.appear;
        e.updated = it == s.end() ? 0 : it->second.updated;
        e.fresh = it != s.end() && it->second.fresh;
    }
}

}  // namespace nyan::cs2
