#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyan::cs2 {

// An encrypted KIF archive's names are unobfuscated with a seed the engine takes from its exe
// (the CRC of V_CODE2). A protected exe keeps its resources packed (Ikinari Anata ni
// Koishiteiru's), so the seed read from the file is wrong; these find the seed from the names.
// `rawNames` is an archive's table as stored (KifArchive::rawNames): name i goes with seed + i.

// Whether `seed` gives the names known file extensions (9 in 10 of those that have one).
bool kifSeedFits(const std::vector<std::string>& rawNames, uint32_t seed);

// The seed that gives the names known file extensions, from a cache or by trying all 2^32 on
// every core (a few seconds on a PC). A seed found is kept in memory and, when a cache directory
// is set, in a file there.
std::optional<uint32_t> findKifSeed(const std::vector<std::string>& rawNames);

// Whether findKifSeed would have to search (nothing cached for these names).
bool kifSeedCached(const std::vector<std::string>& rawNames);

// The folder found seeds are kept in between runs (the app's data folder); empty: memory only.
void setKifSeedCacheDirectory(std::string directory);

}  // namespace nyan::cs2
