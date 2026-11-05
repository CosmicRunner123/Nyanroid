// The text globals over the engine's tokenizer (text_tokenizer.cpp): CountTextLength and
// TokenizeText (muramasa_en.exe 0x488460, 0x488980).

#include <algorithm>

#include "natives.hpp"
#include "nyan/diesel/text_tokenizer.hpp"

namespace nyan::diesel {

namespace {

void putInt(HSQUIRRELVM v, const char* key, SQInteger value) {
    sq_pushstring(v, key, -1);
    sq_pushinteger(v, value);
    sq_newslot(v, -3, SQFalse);
}
void putString(HSQUIRRELVM v, const char* key, const std::string& value) {
    sq_pushstring(v, key, -1);
    sq_pushstring(v, value.c_str(), static_cast<SQInteger>(value.size()));
    sq_newslot(v, -3, SQFalse);
}

// CountTextLength(text): {text, ruby} | null: the UTF-16 units of the text's text and of its rubies
// (RUBY tags' TEXT attributes), tabs and line breaks not counted; '&' escapes count their character.
// null if the "#{ruby}base#" shorthand is unclosed.
SQInteger countTextLength(HSQUIRRELVM v) {
    auto lines = tokenizeText(argString(v, 2), true);
    if (!lines) {
        sq_pushnull(v);
        return 1;
    }
    int text = 0, ruby = 0;
    for (const auto& line : *lines) {
        for (const auto& tok : line) {
            if (tok.name == "RUBY") {
                for (const auto& [name, value] : tok.attributes) {
                    if (name == "TEXT") {
                        ruby += countUtf16Units(value);
                        break;
                    }
                }
            }
            if (!tok.text.empty()) text += countUtf16Units(tok.text);
        }
    }
    sq_newtable(v);
    putInt(v, "text", text);
    putInt(v, "ruby", ruby);
    return 1;
}

// TokenizeText(text): array | null: the lines, each an array of {text, len} for text tokens (len in
// UTF-16 units without tabs and line breaks), tags also with tag = {name, param = {attribute: value},
// value}; '&' escapes kept.
SQInteger tokenizeTextNative(HSQUIRRELVM v) {
    auto lines = tokenizeText(argString(v, 2), false);
    if (!lines) {
        sq_pushnull(v);
        return 1;
    }
    sq_newarray(v, 0);
    for (const auto& line : *lines) {
        sq_newarray(v, 0);
        for (const auto& tok : line) {
            sq_newtable(v);
            std::string text = tok.name.empty() ? tok.text : std::string();
            putString(v, "text", text);
            putInt(v, "len", tok.name.empty() ? countUtf16Units(text) : 0);
            if (!tok.name.empty()) {
                sq_pushstring(v, "tag", -1);
                sq_newtable(v);
                putString(v, "name", tok.name);
                sq_pushstring(v, "param", -1);
                sq_newtable(v);
                for (const auto& [name, value] : tok.attributes) putString(v, name.c_str(), value);
                sq_newslot(v, -3, SQFalse);
                putString(v, "value", tok.text);
                sq_newslot(v, -3, SQFalse);
            }
            sq_arrayappend(v, -2);
        }
        sq_arrayappend(v, -2);
    }
    return 1;
}

}  // namespace

namespace {

// ResetNGCode() (0x488010 -> 0x446160): the start-up's pushes run again, so the three line-break
// tables get their defaults once more after what they hold (what was added stays).
SQInteger resetNGCode(HSQUIRRELVM v) {
    auto& h = host(v);
    auto again = [](std::vector<uint32_t>& t, const std::vector<uint32_t>& d) { t.insert(t.end(), d.begin(), d.end()); };
    again(h.lineBreaks.headOfLine, h.lineBreakDefaults.headOfLine);
    again(h.lineBreaks.endOfLine, h.lineBreakDefaults.endOfLine);
    again(h.lineBreaks.smallKana, h.lineBreakDefaults.smallKana);
    return 0;
}
// AddNGCode_HeadOfLine(text) / AddNGCode_EndOfLine(text) (0x488020 / 0x4881d0, registered without
// types): each UTF-16 unit of the string that the table does not hold joins it; null does nothing
// (anything else: the exe reads through a null pointer).
SQInteger addNGCode(HSQUIRRELVM v, std::vector<uint32_t>& table) {
    SQObjectType t = sq_gettype(v, 2);
    if (t == OT_NULL) return 0;
    if (t != OT_STRING) return sq_throwerror(v, "AddNGCode: not a string");
    for (char16_t c : utf8ToUtf16(argString(v, 2))) {
        if (std::find(table.begin(), table.end(), c) == table.end()) table.push_back(c);
    }
    return 0;
}
SQInteger addNGCodeHeadOfLine(HSQUIRRELVM v) { return addNGCode(v, host(v).lineBreaks.headOfLine); }
SQInteger addNGCodeEndOfLine(HSQUIRRELVM v) { return addNGCode(v, host(v).lineBreaks.endOfLine); }

// ConvertToUTF16(text) (0x488e10): a table {text = [...], ruby = [...]}: the UTF-16 units (as
// integers) of every token's text, and of every RUBY tag's text attribute, tabs and line breaks left
// out (the tokenizer dropping '&'s); null if the tokenizer refuses the text.
SQInteger convertToUtf16(HSQUIRRELVM v) {
    auto lines = tokenizeText(argString(v, 2), true);
    if (!lines) {
        sq_pushnull(v);
        return 1;
    }
    std::vector<char16_t> text, ruby;
    auto keep = [](std::vector<char16_t>& out, const std::string& s) {
        for (char16_t c : utf8ToUtf16(s)) {
            if (c != '\n' && c != '\r' && c != '\t') out.push_back(c);
        }
    };
    for (const auto& line : *lines) {
        for (const auto& tok : line) {
            if (tok.name == "RUBY") {
                for (const auto& a : tok.attributes) {
                    if (a.first != "TEXT") continue;
                    keep(ruby, a.second);
                    break;
                }
            }
            if (!tok.text.empty()) keep(text, tok.text);
        }
    }
    sq_newtable(v);
    for (auto [name, units] : {std::pair<const char*, std::vector<char16_t>*>{"text", &text}, {"ruby", &ruby}}) {
        sq_pushstring(v, name, -1);
        sq_newarray(v, 0);
        for (char16_t c : *units) {
            sq_pushinteger(v, c);
            sq_arrayappend(v, -2);
        }
        sq_newslot(v, -3, SQFalse);
    }
    return 1;
}

}  // namespace

std::vector<NativeImpl> textGlobals() {
    return {{"CountTextLength", countTextLength},
            {"TokenizeText", tokenizeTextNative},
            {"ResetNGCode", resetNGCode},
            {"AddNGCode_HeadOfLine", addNGCodeHeadOfLine},
            {"AddNGCode_EndOfLine", addNGCodeEndOfLine},
            {"ConvertToUTF16", convertToUtf16}};
}

}  // namespace nyan::diesel
