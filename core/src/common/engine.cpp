#include "nyan/common/engine.hpp"

#include <string>
#include <vector>

#include "nyan/diesel/game_data.hpp"
#include "nyan/wa2/assets.hpp"

namespace nyan {

namespace {

// True if a file directly in the folder has this extension (lower case, with the dot).
bool hasExtension(const std::vector<std::string>& names, const std::string& ext) {
    for (const auto& name : names) {
        if (toLowerAscii(name).ends_with(ext)) return true;
    }
    return false;
}

}  // namespace

const char* engineName(Engine engine) {
    switch (engine) {
    case Engine::CatSystem2: return "CatSystem2";
    case Engine::WA2: return "WA2";
    case Engine::Diesel: return "Diesel";
    }
    return "unknown";
}

std::optional<Engine> detectEngine(const GameFolder& folder) {
    const std::vector<std::string> names = folder.list();
    // One check per engine, the most specific first.
    // WA2: script.pak, a KCAP archive (it reads that one file's first bytes).
    if (wa2::isGameFolder(folder, names)) return Engine::WA2;
    // Diesel: script.npk, an NPK3 (or The Song of Saya's NPK2) archive, beside system.npk (it reads
    // script.npk's first bytes).
    if (diesel::isGameFolder(folder, names)) return Engine::Diesel;
    // CatSystem2: KIF archives (.int), which its asset index loads.
    if (hasExtension(names, ".int")) return Engine::CatSystem2;
    return std::nullopt;
}

}  // namespace nyan
