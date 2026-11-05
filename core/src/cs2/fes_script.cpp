#include "nyan/cs2/fes_script.hpp"

#include "nyan/common/sjis.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace nyan::cs2 {
namespace {

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string t; in >> t;) out.push_back(t);
    return out;
}

// "p[30]" -> "30", "SX[100%]" -> "100%"
std::string defaultOf(const std::string& spec) {
    size_t open = spec.find('['), close = spec.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) return "0";
    return spec.substr(open + 1, close - open - 1);
}

// cs2.exe's statement router (0x7690d0) first runs each line through 0x766710: the line is lexed
// (0x878180) and every identifier the script's #MACRO section names becomes the macro's text, the
// rest kept as written. So a macro works as any word of a statement, not only in expressions
// (Cyanotype's config.fes: `strsj str`, then `text_str[0] strsj <text>`). A name right after `$`,
// `\`, `#` or `@` belongs to a variable or an index, not an identifier of its own; Shift-JIS trail
// bytes are skipped (a line kept in UTF-8 has none).
std::string expandMacros(const std::string& line, const std::map<std::string, std::string>& macros) {
    if (macros.empty()) return line;
    const bool utf8 = isValidUtf8(line);
    auto identStart = [](unsigned char c) { return std::isalpha(c) || c == '_'; };
    std::string out;
    size_t i = 0;
    bool changed = false;
    while (i < line.size()) {
        const auto c = static_cast<unsigned char>(line[i]);
        if (!utf8 && ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < line.size()) {
            out.append(line, i, 2);
            i += 2;
            continue;
        }
        if (identStart(c)) {
            size_t j = i + 1;
            while (j < line.size() && (identStart(static_cast<unsigned char>(line[j])) ||
                                       std::isdigit(static_cast<unsigned char>(line[j]))))
                ++j;
            const std::string word = line.substr(i, j - i);
            const char prev = i > 0 ? line[i - 1] : ' ';
            auto m = macros.find(word);
            if (m != macros.end() && prev != '$' && prev != '\\' && prev != '#' && prev != '@') {
                out += m->second;
                changed = true;
            } else {
                out += word;
            }
            i = j;
            continue;
        }
        out += line[i++];
    }
    return changed ? out : line;
}

}  // namespace

bool FesScript::block(const std::string& label, size_t& start, size_t& end) const {
    auto it = labels.find(label);
    if (it == labels.end()) return false;
    start = it->second;
    // A block ends at the next label, including repeated definitions of any label. Labels written
    // one after another are separate empty blocks (`#SLEEP` above `#btn.FOCUS` does nothing), not
    // aliases of the code below them: the engine keeps each label's lines in a list of its own
    // (YukikoiMelt.exe 0x6c39a0 looks a line up by label and index) and a run stops at its end
    // (0x6c8b50: -1 past the last line). Yukikoi's `#MES_SETMAG` over `#MES_AUTOMAG` does nothing.
    size_t k = labelOccurrence.at(label);
    end = k + 1 < labelStarts.size() ? labelStarts[k + 1] : code.size();
    return true;
}

FesScript FesScript::parse(std::string_view text) {
    FesScript s;
    enum class Section { None, Macro, SubCommand, Define, Object, Preload, Code } section = Section::None;
    std::vector<std::string> columns;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line.starts_with("//")) continue;

        if (line[0] == '#') {
            auto tok = split(line);
            const std::string& head = tok[0];
            if (head == "#MACRO") section = Section::Macro;
            else if (head == "#SUBCOMMAND") section = Section::SubCommand;
            else if (head == "#DEFINE") section = Section::Define;
            else if (head == "#PRELOAD") section = Section::Preload;
            else if (head == "#KEYBLOCK") section = Section::Preload;  // keyboard navigation table: data
            else if (head == "#OBJECT") {
                section = Section::Object;
                columns.assign(tok.begin() + 1, tok.end());
            } else {
                // A label or event handler: the block starts at the next statement.
                section = Section::Code;
                if (s.labels.emplace(head.substr(1), s.code.size()).second)  // first definition wins
                    s.labelOccurrence.emplace(head.substr(1), s.labelStarts.size());
                s.labelStarts.push_back(s.code.size());
            }
            continue;
        }

        auto tok = split(line);
        switch (section) {
        case Section::Macro:
            if (tok.size() >= 2) s.macros[tok[0]] = line.substr(line.find(tok[1], tok[0].size()));
            break;
        case Section::SubCommand: {
            // oneshot move motMove p[30] px[0] py[0] pz[0] : [0] 0 0 0 [1] [2] [3]
            if (tok.size() < 3) break;
            SubCommand sc;
            sc.primitive = tok[2];
            size_t i = 3;
            for (; i < tok.size() && tok[i] != ":"; ++i) sc.defaults.push_back(defaultOf(tok[i]));
            for (++i; i < tok.size(); ++i) sc.mapping.push_back(tok[i]);
            s.subcommands[tok[1]] = std::move(sc);
            break;
        }
        case Section::Define:
            if (tok.size() >= 2) {
                Declaration d;
                d.type = baseType(tok[0]);
                d.name = tok[1];
                size_t open = d.name.find('[');
                if (open != std::string::npos) {
                    d.count = std::atoi(d.name.c_str() + open + 1);
                    d.name = d.name.substr(0, open);
                }
                s.declarations.push_back(std::move(d));
            }
            break;
        case Section::Object: {
            Row row;
            row.object = tok[0];
            for (size_t i = 0; i < columns.size() && i + 1 < tok.size(); ++i) row.values[columns[i]] = tok[i + 1];
            s.rows.push_back(std::move(row));
            break;
        }
        case Section::Preload:
            s.preload.push_back(line);
            break;
        case Section::Code:
        case Section::None:
            s.code.push_back({line, std::move(tok)});
            break;
        }
    }
    for (auto& st : s.code) {
        std::string expanded = expandMacros(st.text, s.macros);
        if (expanded != st.text) st = {expanded, split(expanded)};
    }
    return s;
}

}  // namespace nyan::cs2
