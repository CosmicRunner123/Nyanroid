#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/cs2/cst_scene.hpp"

namespace nyan::cs2 {

// A scene's translations (.cstl, "CatScene language", next to the .cst of the same name): for
// every message of the scene, its speaker and text in each language the game ships.
struct SceneTranslations {
    struct Text {
        std::string name, text;  // UTF-8 with CatSystem2 markup; 0x1F breaks the line
    };
    std::vector<std::string> languages;    // language prefixes ("cn", "en", "jp")
    std::vector<std::vector<Text>> entries;  // [message][language]

    static SceneTranslations parse(ByteSpan file);
    // Index of a language prefix in `languages`, -1 if absent.
    int languageIndex(std::string_view prefix) const;
};

// Replaces a scene's messages, speaker names and choice prompts with one language of its
// translation table (the replaced lines become UTF-8). Entries follow the scene's messages in
// order: a message starts at its speaker name or first text after an input wait (or at a name
// following text, which a `\@` left open) and runs to the next input wait; empty text starts none.
// Choice prompts follow in order, one entry per distinct prompt of a choice block.
// A scene already written in that language (most messages equal its entries) is left as is.
// A replaced name line keeps its own text in `own` (for the name table).
// Returns false if the scene was left unchanged.
// `sub` >= 0 also gives each message's first text line that language's text (`CstLine::sub`).
bool localizeScene(CstScene& scene, const SceneTranslations& table, int language, int sub = -1);

}  // namespace nyan::cs2
