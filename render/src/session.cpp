#include "nyan/render/session.hpp"

#include "nyan/common/error.hpp"
#include "nyan/cs2/game.hpp"
#include "nyan/render/diesel_session.hpp"
#include "nyan/render/game_session.hpp"
#include "nyan/render/wa2_session.hpp"

namespace nyan::render {

std::unique_ptr<Session> createSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                       Session::Options options) {
    std::optional<Engine> engine = detectEngine(*folder);
    if (!engine) fail("No supported game found in " + folder->displayName());
    switch (*engine) {
    case Engine::CatSystem2: return std::make_unique<GameSession>(std::move(folder), mixer, std::move(options));
    case Engine::WA2: return createWa2Session(std::move(folder), mixer, std::move(options));
    case Engine::Diesel: return createDieselSession(std::move(folder), mixer, std::move(options));
    }
    fail(std::string("No session for ") + engineName(*engine));
}

std::vector<std::string> missingFonts(const GameFolder& folder, const std::vector<std::string>& fontDirectories) {
    if (detectEngine(folder) != Engine::CatSystem2) return {};
    return cs2::Game::missingFonts(folder, fontDirectories);
}

}  // namespace nyan::render
