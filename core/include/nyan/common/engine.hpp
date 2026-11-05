#pragma once

#include <optional>

#include "nyan/common/file_source.hpp"

namespace nyan {

// The visual-novel engines Nyanroid emulates. Where each one's code goes, and what else a new
// one hooks into, is in docs/adding-an-engine.md.
enum class Engine {
    CatSystem2,
    WA2,  // White Album 2's engine (Leaf)
    Diesel,  // Nitroplus's Diesel Engine (Full Metal Daemon Muramasa, You and Me and Her, ...)
};

// The engine's name as shown to the user ("CatSystem2").
const char* engineName(Engine engine);

// The engine that made the game in this folder, judged by its files; nullopt if Nyanroid knows
// none. Front ends check folders with this before they create a session for one.
std::optional<Engine> detectEngine(const GameFolder& folder);

}  // namespace nyan
