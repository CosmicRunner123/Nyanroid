#pragma once

// nyan-cli's commands come in one set per engine (<engine>_commands.cpp); main.cpp offers a
// command line to each set in turn.

#include <optional>
#include <string>
#include <vector>

#include "nyan/common/engine.hpp"

namespace nyan::cli {

struct CommandSet {
    Engine engine;
    // Prints this set's lines of the usage text.
    void (*usage)();
    // Runs args[0] if it is one of this set's commands and the arguments fit it: its exit code;
    // nullopt otherwise. Errors are thrown.
    std::optional<int> (*run)(const std::vector<std::string>& args);
};

extern const CommandSet kCatSystem2Commands;  // cs2_commands.cpp
extern const CommandSet kWa2Commands;         // wa2_commands.cpp
extern const CommandSet kDieselCommands;      // diesel_commands.cpp

}  // namespace nyan::cli
