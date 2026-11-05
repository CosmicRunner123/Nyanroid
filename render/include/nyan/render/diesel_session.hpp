#pragma once

#include <memory>

#include "nyan/render/session.hpp"

namespace nyan::render {

// The Diesel Engine's Session, for a folder detectEngine() calls Diesel (createSession() asks for it
// here). Throws nyan::Error if the game cannot be started.
std::unique_ptr<Session> createDieselSession(std::unique_ptr<GameFolder> folder, audio::Mixer* mixer,
                                             Session::Options options);

}  // namespace nyan::render
