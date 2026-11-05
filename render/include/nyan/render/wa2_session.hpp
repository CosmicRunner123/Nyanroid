#pragma once

#include <memory>
#include <string>

#include "nyan/render/session.hpp"

namespace nyan::wa2 {
class Game;
}

namespace nyan::render {

// White Album 2's Session, for a folder detectEngine() calls WA2 (createSession() asks for it here).
// Throws nyan::Error if the game cannot be started.
std::unique_ptr<Session> createWa2Session(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                          Session::Options options);

// The engine of a WA2 session (session->engine() == Engine::WA2), for the desktop's development options.
wa2::Game& wa2Game(Session& session);

}  // namespace nyan::render
