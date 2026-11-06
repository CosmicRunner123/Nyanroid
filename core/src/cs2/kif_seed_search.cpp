#include "nyan/cs2/kif_seed_search.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "nyan/common/file_source.hpp"
#include "nyan/cs2/kif_archive.hpp"

namespace nyan::cs2 {
namespace {

// The file types the games keep in their archives: those of the 15 CatSystem2 games tried, the
// movie kinds the engine plays and the common picture formats.
constexpr const char* kExtensions[] = {
    "hg3", "hg2",  "ogg", "lpb", "cst", "cstl", "tag",  "lv",  "anm", "dmy", "kcs", "psz", "psb", "fes",
    "zt",  "pfl",  "xml", "kx2", "dif", "inc",  "dat",  "csv", "txt", "tbl", "plist", "cnf", "qdb", "cl",
    "ttf", "otf",  "json", "inf", "def", "bat", "ini",  "lst", "frm", "png", "jpg", "bmp", "wav", "mpg",
    "mpa", "mpv",  "avi", "mov", "wmv", "webm", "ogv",  "hap", "mp4"};

bool hasExtension(const std::string& name) {
    size_t dot = name.rfind('.');
    return dot != std::string::npos && dot > 0 && dot + 1 < name.size();
}

bool knownExtension(const std::string& name) {
    if (!hasExtension(name)) return false;
    const std::string ext = toLowerAscii(name.substr(name.rfind('.') + 1));
    return std::any_of(std::begin(kExtensions), std::end(kExtensions), [&](const char* e) { return ext == e; });
}

bool isKeyEntry(const std::string& name) { return name == "__key__.dat"; }

// MersenneTwister(seed).next() from the only state words it reads: 0 (its top bit), 1 and 397,
// which are the Knuth generator's values 0, 2, 3, 794 and 795 from the seed.
struct Lcg {
    uint32_t a, c;
};
constexpr Lcg lcgJump(int steps) {
    Lcg j{1, 0};
    for (int i = 0; i < steps; ++i) j = {j.a * 69069u, j.c * 69069u + 1};
    return j;
}
constexpr Lcg kJ2 = lcgJump(2), kJ3 = lcgJump(3), kJ794 = lcgJump(794), kJ795 = lcgJump(795);

inline int nameKey(uint32_t s) {
    const uint32_t w1 = ((kJ2.a * s + kJ2.c) & 0xFFFF0000u) | ((kJ3.a * s + kJ3.c) >> 16);
    const uint32_t w397 = ((kJ794.a * s + kJ794.c) & 0xFFFF0000u) | ((kJ795.a * s + kJ795.c) >> 16);
    uint32_t y = (s & 0x80000000u) | (w1 & 0x7FFFFFFFu);
    y = w397 ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0);
    y ^= y >> 11;
    y ^= (y << 7) & 0x9D2C5680u;
    y ^= (y << 15) & 0xEFC60000u;
    y ^= y >> 18;
    return static_cast<uint8_t>((y >> 24) + (y >> 16) + (y >> 8) + y) % 52;
}

uint64_t tableHash(const std::vector<std::string>& rawNames) {
    uint64_t h = 14695981039346656037ull;
    for (const auto& n : rawNames) {
        for (char c : n) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
        h = (h ^ '\n') * 1099511628211ull;
    }
    return h;
}

std::mutex gCacheMutex;
std::map<uint64_t, uint32_t> gCache;
std::string gCacheDirectory;

std::string cacheFile() { return gCacheDirectory.empty() ? std::string() : gCacheDirectory + "/archive-seeds.txt"; }

std::optional<uint32_t> cached(uint64_t hash) {
    std::lock_guard lock(gCacheMutex);
    if (auto it = gCache.find(hash); it != gCache.end()) return it->second;
    const std::string path = cacheFile();
    if (path.empty()) return std::nullopt;
    std::ifstream in(path);
    unsigned long long h = 0;
    unsigned s = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (std::sscanf(line.c_str(), "%llx %x", &h, &s) == 2 && h == hash) return gCache[hash] = s;
    }
    return std::nullopt;
}

void remember(uint64_t hash, uint32_t seed) {
    std::lock_guard lock(gCacheMutex);
    gCache[hash] = seed;
    const std::string path = cacheFile();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(gCacheDirectory, ec);
    std::ofstream out(path, std::ios::app);
    char line[32];
    std::snprintf(line, sizeof line, "%016llx %08x\n", static_cast<unsigned long long>(hash), seed);
    out << line;
}

// One entry the search tries: its table index and the first keys (bits 0-51) that give it a
// known extension.
struct Probe {
    uint32_t index;
    uint64_t keys;
};

}  // namespace

bool kifSeedFits(const std::vector<std::string>& rawNames, uint32_t seed) {
    size_t named = 0, known = 0;
    for (size_t i = 0; i < rawNames.size(); ++i) {
        if (isKeyEntry(rawNames[i]) || !hasExtension(rawNames[i])) continue;
        std::string n = rawNames[i];
        unobfuscateKifName(n, seed + static_cast<uint32_t>(i));
        ++named;
        known += knownExtension(n);
    }
    return named > 0 && known * 10 >= named * 9;
}

bool kifSeedCached(const std::vector<std::string>& rawNames) { return cached(tableHash(rawNames)).has_value(); }

void setKifSeedCacheDirectory(std::string directory) {
    std::lock_guard lock(gCacheMutex);
    gCacheDirectory = std::move(directory);
}

std::optional<uint32_t> findKifSeed(const std::vector<std::string>& rawNames) {
    const uint64_t hash = tableHash(rawNames);
    if (auto seed = cached(hash)) return seed;

    // The entries to try, the most telling (fewest fitting keys) first. A name decodes letter by
    // letter from its first key, so each entry allows only the keys that end it in a known
    // extension: one or two of the 52, mostly.
    std::vector<Probe> probes;
    for (size_t i = 0; i < rawNames.size(); ++i) {
        if (isKeyEntry(rawNames[i]) || !hasExtension(rawNames[i])) continue;
        uint64_t keys = 0;
        for (int k = 0; k < 52; ++k) {
            std::string n = rawNames[i];
            unobfuscateKifNameWithKey(n, k);
            if (knownExtension(n)) keys |= uint64_t{1} << k;
        }
        const int fits = std::popcount(keys);
        if (fits >= 1 && fits <= 4) probes.push_back({static_cast<uint32_t>(i), keys});
        if (probes.size() == 16) break;
    }
    if (probes.size() < 8) return std::nullopt;
    std::stable_sort(probes.begin(), probes.end(),
                     [](const Probe& a, const Probe& b) { return std::popcount(a.keys) < std::popcount(b.keys); });

    // All 2^32 seeds in chunks handed out to every core; the first seed that fits every probe and
    // then the whole table ends the search.
    constexpr uint64_t kChunk = uint64_t{1} << 20;
    constexpr uint64_t kChunks = (uint64_t{1} << 32) / kChunk;
    std::atomic<uint64_t> nextChunk{0};
    std::atomic<bool> done{false};
    std::optional<uint32_t> found;
    std::mutex foundMutex;
    auto work = [&] {
        for (uint64_t c; !done.load(std::memory_order_relaxed) && (c = nextChunk.fetch_add(1)) < kChunks;) {
            const uint32_t first = static_cast<uint32_t>(c * kChunk);
            for (uint32_t k = 0; k < kChunk; ++k) {
                const uint32_t s = first + k;
                bool fits = true;
                for (const Probe& p : probes) {
                    if (!((p.keys >> nameKey(s + p.index)) & 1)) {
                        fits = false;
                        break;
                    }
                }
                if (!fits || !kifSeedFits(rawNames, s)) continue;
                std::lock_guard lock(foundMutex);
                if (!found) found = s;
                done = true;
                return;
            }
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (found) remember(hash, *found);
    return found;
}

}  // namespace nyan::cs2
