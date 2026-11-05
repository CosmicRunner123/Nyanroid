// nyan-cli: desktop tool for inspecting game data with the Nyanroid core. Each engine's commands
// live in a file of their own (<engine>_commands.cpp); this file hands the command line to them.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "commands.hpp"
#include "nyan/common/file_source.hpp"

namespace {

// Every engine's commands, offered the command line in this order.
const nyan::cli::CommandSet* const kCommandSets[] = {
    &nyan::cli::kCatSystem2Commands,
    &nyan::cli::kWa2Commands,
    &nyan::cli::kDieselCommands,
};

void usage() {
    std::puts("Usage:");
    for (const auto* set : kCommandSets) set->usage();
}

// The engine of the game folder a command line names (its first argument), if it names one.
std::optional<nyan::Engine> engineOf(const std::vector<std::string>& args) {
    std::error_code ec;
    if (args.size() < 2 || !std::filesystem::is_directory(args[1], ec)) return std::nullopt;
    try {
        return nyan::detectEngine(nyan::DirectoryGameFolder(args[1]));
    } catch (const std::exception&) {
        return std::nullopt;  // the command reports what is wrong with the folder
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }
    try {
        // Engines may have commands of the same name (playall): the folder's own engine goes first.
        std::optional<nyan::Engine> engine = engineOf(args);
        for (bool own : {true, false}) {
            for (const auto* set : kCommandSets) {
                if ((set->engine == engine) != own) continue;
                if (std::optional<int> code = set->run(args)) return *code;
            }
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
