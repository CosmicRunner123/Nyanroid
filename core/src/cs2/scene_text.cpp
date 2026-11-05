#include "nyan/cs2/scene_text.hpp"

#include <cctype>

#include "nyan/common/file_source.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/cs2/message_text.hpp"

namespace nyan::cs2 {
namespace {

// "FF-encoded" integer: bytes are summed while they are 0xFF.
uint32_t readFFInt(ByteReader& r) {
    uint32_t v = 0;
    for (uint8_t c;;) {
        c = r.u8();
        v += c;
        if (c != 0xFF) return v;
    }
}

std::string readFFString(ByteReader& r) {
    uint32_t n = readFFInt(r);
    auto s = r.span(n);
    return std::string(s.begin(), s.end());
}

bool endsWithContinue(std::string_view s) {
    return s.size() >= 2 && s.substr(s.size() - 2) == "\\@";
}

// Table text as scene markup: 0x1F (and a stray newline) become `\n`.
std::string toMarkup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\x1F' || c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

// For comparing a scene's own text with a table entry: markup line breaks, no `\@`.
std::string comparable(std::string s) {
    for (size_t p; (p = s.find("\\@")) != std::string::npos;) s.erase(p, 2);
    return s;
}

// Offset of a choice line's prompt ("<n> <scene> <prompt>", after any `if (...)` conditions), or
// npos if the command is not a choice entry.
size_t choicePromptOffset(std::string_view text) {
    size_t i = 0;
    auto skipSpace = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    };
    for (;;) {
        skipSpace();
        if (text.substr(i, 2) != "if") break;
        size_t open = text.find_first_not_of(" \t", i + 2);
        if (open == std::string_view::npos || text[open] != '(') break;
        int depth = 0;
        size_t close = open;
        for (; close < text.size(); ++close) {
            if (text[close] == '(') ++depth;
            else if (text[close] == ')' && --depth == 0) break;
        }
        if (close >= text.size()) return std::string_view::npos;
        i = close + 1;
    }
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return std::string_view::npos;
    for (int token = 0; token < 2; ++token) {
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        skipSpace();
    }
    return i < text.size() ? i : std::string_view::npos;
}

}  // namespace

SceneTranslations SceneTranslations::parse(ByteSpan file) {
    ByteReader r(file, "CSTL");
    if (r.fixedString(4) != "CSTL") fail("Not a scene translation (.cstl) file");
    r.skip(4);
    SceneTranslations t;
    uint32_t langs = readFFInt(r);
    for (uint32_t i = 0; i < langs; ++i) t.languages.push_back(readFFString(r));
    uint32_t count = readFFInt(r);
    t.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<Text> e;
        e.reserve(langs);
        for (uint32_t l = 0; l < langs; ++l) {
            Text x;
            x.name = readFFString(r);
            x.text = readFFString(r);
            e.push_back(std::move(x));
        }
        t.entries.push_back(std::move(e));
    }
    return t;
}

int SceneTranslations::languageIndex(std::string_view prefix) const {
    for (size_t i = 0; i < languages.size(); ++i) {
        if (toLowerAscii(languages[i]) == toLowerAscii(std::string(prefix))) return static_cast<int>(i);
    }
    return -1;
}

bool localizeScene(CstScene& scene, const SceneTranslations& table, int language, int sub) {
    if (language < 0 || static_cast<size_t>(language) >= table.languages.size()) language = -1;
    if (sub < 0 || static_cast<size_t>(sub) >= table.languages.size() || sub == language) sub = -1;
    if (language < 0 && sub < 0) return false;
    auto& lines = scene.lines();

    // Which entry each name/message/choice line belongs to (-1: none).
    std::vector<int32_t> entryOf(lines.size(), -1);
    std::vector<bool> firstText(lines.size(), false);  // the message's first text line
    int32_t next = 0, current = -1;
    bool open = false, hasText = false;
    std::map<std::string, int32_t> blockPrompts;  // choice prompt -> entry, per choice block
    bool inChoices = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const CstLine& line = lines[i];
        switch (line.type) {
        case CstLineType::Name:
            if (!open || hasText) current = next++, open = true, hasText = false;
            entryOf[i] = current;
            break;
        case CstLineType::Message:
            if (!open) {
                if (line.text.empty()) break;
                current = next++, open = true, hasText = false;
            }
            entryOf[i] = current;
            firstText[i] = !hasText;
            hasText = true;
            break;
        case CstLineType::Input:
        case CstLineType::Page:
            open = false;
            break;
        case CstLineType::Command: {
            std::string_view t = line.text;
            if (t.starts_with("fselect")) {
                inChoices = true;
                blockPrompts.clear();
                break;
            }
            if (!inChoices) break;
            size_t p = choicePromptOffset(t);
            if (p == std::string_view::npos) break;
            std::string prompt(t.substr(p));
            auto [it, added] = blockPrompts.emplace(prompt, next);
            if (added) ++next;
            entryOf[i] = it->second;
            break;
        }
        default:
            break;
        }
    }

    auto entryIn = [&](int32_t e, int lang) -> const SceneTranslations::Text* {
        if (e < 0 || lang < 0 || static_cast<size_t>(e) >= table.entries.size()) return nullptr;
        const auto& row = table.entries[static_cast<size_t>(e)];
        return static_cast<size_t>(lang) < row.size() ? &row[static_cast<size_t>(lang)] : nullptr;
    };
    auto entry = [&](int32_t e) { return entryIn(e, language); };
    bool changed = false;
    if (sub >= 0) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].type != CstLineType::Message || !firstText[i]) continue;
            if (const auto* x = entryIn(entryOf[i], sub); x && !x->text.empty()) {
                lines[i].sub = toMarkup(x->text);
                changed = true;
            }
        }
    }
    if (language < 0) return changed;

    // A scene written in this language already: keep its own lines (they carry the authors'
    // exact markup and waits).
    {
        std::map<int32_t, std::string> own;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].type == CstLineType::Message && entryOf[i] >= 0) own[entryOf[i]] += sjisToUtf8(lines[i].text);
        }
        size_t same = 0;
        for (const auto& [e, text] : own) {
            if (const auto* x = entry(e); x && comparable(toMarkup(x->text)) == comparable(text)) ++same;
        }
        if (!own.empty() && same * 2 > own.size()) return changed;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto* x = entry(entryOf[i]);
        if (!x) continue;
        CstLine& line = lines[i];
        switch (line.type) {
        case CstLineType::Name:
            if (x->name.empty()) break;
            if (line.own.empty()) line.own = line.utf8 ? line.text : decodeGameText(line.text);
            line.text = toMarkup(x->name);
            line.utf8 = true;
            changed = true;
            break;
        case CstLineType::Message: {
            // The message's text goes in its first line; later lines (continuations) keep only
            // their `\@`, so the script still pauses where it did.
            bool cont = endsWithContinue(line.text);
            std::string text = firstText[i] ? toMarkup(x->text) : std::string();
            if (cont && !endsWithContinue(text)) text += "\\@";
            line.text = std::move(text);
            line.utf8 = true;
            changed = true;
            break;
        }
        case CstLineType::Command: {
            size_t p = choicePromptOffset(line.text);
            if (p == std::string::npos || x->text.empty()) break;
            line.text = line.text.substr(0, p) + toMarkup(x->text);
            line.utf8 = true;
            changed = true;
            break;
        }
        default:
            break;
        }
    }
    return changed;
}

}  // namespace nyan::cs2
