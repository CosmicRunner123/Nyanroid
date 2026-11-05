#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// A parsed FE script (.fes): the CatSystem2 language for menus, screens, the message
// window and eyecatches. Text stays in its original encoding (Shift-JIS).
//
// Layout of a script:
//   #MACRO       NAME value               constants
//   #SUBCOMMAND  oneshot|continue name primitive p[def]... : [0] 0 [1] ...
//                                         object methods defined via engine primitives
//   #DEFINE      TYPE name[count]         object declarations (PLANE, IMAGE, BUTTON, ...)
//   #OBJECT      COL COL ...              property table; rows "object value value ..."
//   #PRELOAD     ...                      assets to load early
//   #START                                entry point
//   #LABEL                                code block; execution stops at the next label
//   #obj.EVENT                            event handler (FOCUS, PUSH_L, RELEASE_L, ...)
struct FesScript {
    struct SubCommand {
        // The first word, `oneshot` or `continue`, is only checked (cs2.exe 0x612380): whether a
        // motion loops is its motion script's own doing.
        std::string primitive;             // engine primitive, e.g. motMove
        std::vector<std::string> defaults; // parameter defaults, e.g. "30", "0", "100%"
        std::vector<std::string> mapping;  // primitive arguments: "[n]" = parameter n, else literal
    };
    // The UTF-8 text objects of newer engines (STRING8, LOGSTRING8, NVSTRING8, NVSTRINGUTF8) behave as
    // their plain counterparts; the text is UTF-8 rather than Shift-JIS, which the runtime detects.
    static std::string baseType(const std::string& t) {
        if (t.find("STRING") == std::string::npos) return t;
        if (t.ends_with("UTF8")) return t.substr(0, t.size() - 4);
        if (t.ends_with("8")) return t.substr(0, t.size() - 1);
        return t;
    }
    struct Declaration {
        std::string type;  // PLANE, IMAGE, BUTTON, STRING, SOUND, ... (baseType)
        std::string name;
        int count = 0;     // array size; 0 = a single object
    };
    struct Row {
        std::string object;  // as written: "pl[0]", "btn" (all elements), "min[1]"
        std::map<std::string, std::string> values;  // column -> value
    };
    struct Statement {
        std::string text;               // trimmed source line
        std::vector<std::string> tokens;  // whitespace-separated
    };

    std::map<std::string, std::string> macros;
    std::map<std::string, SubCommand> subcommands;
    std::vector<Declaration> declarations;
    std::vector<Row> rows;
    std::vector<std::string> preload;
    std::vector<Statement> code;
    std::map<std::string, size_t> labels;  // label (without '#') -> first statement index (first definition)
    std::vector<size_t> labelStarts;       // every label occurrence, ascending: block boundaries
    std::map<std::string, size_t> labelOccurrence;  // label -> its first definition's index in labelStarts

    // Statement index range of a label's block: [start, end).
    bool block(const std::string& label, size_t& start, size_t& end) const;

    static FesScript parse(std::string_view text);
};

}  // namespace nyan::cs2
