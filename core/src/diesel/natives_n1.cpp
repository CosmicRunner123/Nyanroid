// The Song of Saya's own natives (Saya_en.exe, registered beside the exe's other own natives by the
// callback WinMain sets, 0x51a840 / 0x51b020 / 0x520700): the globals ConvertN1CalcString
// (0x51a950) and CheckFileExists (0x51af30), and the classes SQN1ScriptReader (the 2003 story's tag
// scripts read into lines of elements: CN1ScriptReader, read 0x521790) and SQIniFileReader (its .ini
// files: CIniFileReader, load 0x41ae60 -> parse 0x41b020). The game's own Squirrel interpreter
// (its system scripts) does the rest.

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "natives.hpp"

namespace nyan::diesel {

namespace {

// The length of the UTF-8 sequence a byte starts as the exe counts it (0x4946b0, 0x521b71): 1 for
// ASCII and continuation bytes, else 2 .. 6 by the lead byte's ones.
size_t sequenceLength(uint8_t b) {
    uint8_t n = static_cast<uint8_t>(~b);
    if (n & 0xc0) return 1;
    if (n & 0x20) return 2;
    if (n & 0x10) return 3;
    if (n & 0x08) return 4;
    if (n & 0x04) return 5;
    if (n & 0x02) return 6;
    return 1;
}

// 0x4946b0: single-byte characters to upper case (a .. z only, 0x55c103), UTF-8 sequences skipped.
void upperAscii(std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t n = sequenceLength(static_cast<uint8_t>(s[i]));
        if (n > 1) {
            i += n;
            continue;
        }
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = static_cast<char>(s[i] - 'a' + 'A');
        ++i;
    }
}

void pushString(HSQUIRRELVM v, const std::string& s) { sq_pushstring(v, s.c_str(), -1); }

// --- ConvertN1CalcString(expression): [operator, left, right] (0x51a950) ---------------------------
// The expression splits at its first operator character ("+-*/%=!<>", 0x5c70a4): the characters
// before it are the left value, the rest after it the right (the operator is that one character).
// Each part is null if empty, an integer if all its bytes are digits, 1 / 0 for TRUE / FALSE in any
// case, else the string as written. The game's calculator script makes the CALC from it.
void pushCalcValue(HSQUIRRELVM v, const std::string& s) {
    if (s.empty()) {
        sq_pushnull(v);
        return;
    }
    bool digits = true;
    for (char c : s) digits = digits && std::isdigit(static_cast<unsigned char>(c));
    if (digits) {
        sq_pushinteger(v, static_cast<SQInteger>(std::atoi(s.c_str())));
        return;
    }
    std::string upper = s;
    upperAscii(upper);
    if (upper == "TRUE") {
        sq_pushinteger(v, 1);
    } else if (upper == "FALSE") {
        sq_pushinteger(v, 0);
    } else {
        pushString(v, s);
    }
}

SQInteger convertN1CalcString(HSQUIRRELVM v) {
    static const char kOperators[] = "+-*/%=!<>";
    std::string in = argString(v, 2), left, op, right;
    size_t i = 0;
    for (; i < in.size(); ++i) {
        char c = in[i];
        if (std::strchr(kOperators, c) && c != '\0') {
            op += c;
            ++i;
            break;
        }
        left += c;
    }
    if (!op.empty() && i < in.size()) right = in.substr(i);
    sq_newarray(v, 0);
    if (op.empty()) {
        sq_pushnull(v);
    } else {
        pushString(v, op);
    }
    sq_arrayappend(v, -2);
    pushCalcValue(v, left);
    sq_arrayappend(v, -2);
    pushCalcValue(v, right);
    sq_arrayappend(v, -2);
    return 1;
}

// CheckFileExists(path): 0 if the file does not open through the app's file system, else what the
// opened file's vtable +0x10 returns (0x51af30; the slot is not read: Nyanroid gives the size).
// The game's text box script only tests it for truth (whether a face picture exists).
SQInteger checkFileExists(HSQUIRRELVM v) {
    auto data = host(v).readAnyFile(argString(v, 2));
    sq_pushinteger(v, data ? static_cast<SQInteger>(data->size()) : 0);
    return 1;
}

// --- SQIniFileReader ------------------------------------------------------------------------------
struct IniValue {
    int type = 0;  // 0 an integer, 1 a string (quoted in the file)
    int32_t number = 0;
    std::string text;
};

struct IniReader final : NativeObject {
    std::string name;
    std::map<std::string, std::map<std::string, IniValue>> sections;

    // CIniFileReader's parse (0x41b020), over the text as UTF-16 there; every delimiter is ASCII, so
    // over the UTF-8 bytes here. Carriage returns and tabs are skipped everywhere; '#' outside quotes
    // starts a comment to the line's end; '"' toggles quoting and stays in the text; '[' is dropped;
    // ']' makes the text so far the section, '=' the key; a line's end stores key = text when the
    // section, the key and the text are all there (a text in quotes: a string without them; else an
    // integer, _wtol). A key the section already has, or a text that opens a quote and does not end
    // with one, stops the parse (-2). The last line is stored only if a line end follows it.
    int parse(const std::string& text) {
        std::string token, section, key;
        bool comment = false, quoted = false;
        for (char c : text) {
            if (c == '\r' || c == '\t') continue;
            if (comment) {
                if (c != '\n') continue;
                comment = false;
            } else if (c == '"') {
                quoted = !quoted;
            }
            if (c == '[') continue;
            if (c == ']') {
                section = token;
                token.clear();
                continue;
            }
            if (c == '=') {
                key = token;
                token.clear();
                continue;
            }
            if (c == '\n') {
                if (!section.empty() && !key.empty() && !token.empty()) {
                    auto& keys = sections[section];
                    if (keys.count(key)) return -2;
                    IniValue value;
                    if (token.front() == '"') {
                        if (token.size() < 2 || token.back() != '"') return -2;
                        value.type = 1;
                        value.text = token.substr(1, token.size() - 2);
                    } else {
                        value.number = static_cast<int32_t>(std::strtol(token.c_str(), nullptr, 10));
                    }
                    keys.emplace(key, value);
                }
                key.clear();
                token.clear();
                quoted = false;
                continue;
            }
            if (c == '#' && !quoted) {
                comment = true;
                continue;
            }
            token += c;
        }
        return 0;
    }
    const IniValue* find(const std::string& s, const std::string& k) const {
        auto si = sections.find(s);
        if (si == sections.end()) return nullptr;
        auto ki = si->second.find(k);
        return ki == si->second.end() ? nullptr : &ki->second;
    }
};

NativeObject* makeIniReader(HSQUIRRELVM v) {
    auto* r = new IniReader;
    r->name = argString(v, 2);
    return r;
}

#define INI_SELF                         \
    IniReader* r = self<IniReader>(v);   \
    if (!r) return sq_throwerror(v, "SQIniFileReader: not an instance");

// load(path): 0, the file system's error (-1 here) if the file does not open, -2 if it does not
// parse (0x41ae60). What an earlier load read stays.
SQInteger iniLoad(HSQUIRRELVM v) {
    INI_SELF
    auto data = host(v).readAnyFile(argString(v, 2));
    if (!data) {
        sq_pushinteger(v, -1);
        return 1;
    }
    sq_pushinteger(v, r->parse(std::string(data->begin(), data->end())));
    return 1;
}
// getInt(section, key, default): the key's integer, else the default (a string too, 0x41b740).
SQInteger iniGetInt(HSQUIRRELVM v) {
    INI_SELF
    const IniValue* x = r->find(argString(v, 2), argString(v, 3));
    sq_pushinteger(v, x && x->type == 0 ? x->number : argInt(v, 4));
    return 1;
}
// getString(section, key, default): the key's string, else the default (an integer too, 0x41b850).
SQInteger iniGetString(HSQUIRRELVM v) {
    INI_SELF
    const IniValue* x = r->find(argString(v, 2), argString(v, 3));
    pushString(v, x && x->type == 1 ? x->text : argString(v, 4));
    return 1;
}

// --- SQN1ScriptReader -----------------------------------------------------------------------------
struct N1Element {
    std::string name;  // a tag's, upper-cased; "" for text
    std::string value;  // text's
    std::vector<std::pair<std::string, std::string>> attrs;  // names upper-cased
};
using N1Line = std::vector<N1Element>;

struct N1Reader final : NativeObject {
    std::string name;
    std::vector<N1Line> lines;

    // CN1ScriptReader's read (0x521790), a state machine over the file's bytes (UTF-8: the exe sets
    // its code page to UTF-8 at 0x40262b, so the text is used as it is; a leading byte-order mark is
    // skipped). States, kept on a stack: 0 text, 1 in quotes, 2 a "//" comment, 3 in a tag after its
    // name, 4 a tag's name, 6 an attribute's name, 7 after its '='.
    void read(const uint8_t* p, size_t len) {
        size_t i = len >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf ? 3 : 0;
        std::vector<int> st{0};
        N1Line elems;
        N1Element elem;
        std::string token, attrName;
        bool broken = false;  // the line holds a '<' in a tag or a '>' with no tag: dropped
        bool blank = true;    // the last line kept was empty (and at the start): blank lines collapse
        auto top = [&]() { return st.empty() ? -1 : st.back(); };
        // 0x522500: the element so far (text: the token, at the top level) kept if it holds anything.
        auto flush = [&](int state) {
            if (!token.empty() && state == 0) elem.value = token;
            upperAscii(elem.name);
            if (!elem.name.empty() || !elem.attrs.empty() || !elem.value.empty()) elems.push_back(elem);
            elem = N1Element{};
            token.clear();
        };
        auto clearLine = [&]() {
            elems.clear();
            elem = N1Element{};
            token.clear();
        };
        for (; i < len; ++i) {
            const char c = static_cast<char>(p[i]);
            if ((broken || top() == 2) && c != '\n') continue;
            size_t n = sequenceLength(p[i]);
            if (n > 1) {
                token.append(reinterpret_cast<const char*>(p + i), std::min(n, len - i));
                i += n - 1;
                continue;
            }
            if (c == '/' && !token.empty() && token.back() == '/') {
                token.pop_back();
                flush(top());
                st.push_back(2);
                continue;
            }
            if (top() == 1) {
                if (c == '"') {
                    st.pop_back();
                    if (top() == 7) {
                        upperAscii(attrName);
                        elem.attrs.emplace_back(attrName, token);
                        attrName.clear();
                        st.pop_back();
                        if (i + 1 < len && std::isspace(p[i + 1])) st.push_back(6);
                    }
                    token.clear();
                    continue;
                }
                if (c != '\0' && i != len - 1 && c != '\n') {
                    token += c;
                    continue;
                }
                st.pop_back();  // a line's end closes the quotes
            }
            if (c == '<') {
                if (top() != 0) broken = true;
                flush(top());
                st.push_back(4);
                continue;
            }
            if (c == '>') {
                int popped = top();
                if (!st.empty()) st.pop_back();
                if (popped != 3 && popped != 4) broken = true;
                if (popped == 4) elem.name = token;
                flush(popped);
                continue;
            }
            if (c == '\r') continue;
            if (c == '\n' || c == '\0' || i == len - 1) {
                // The line's end (0x521fd9).
                const int was = top();
                if (was == 2) st.pop_back();
                if (broken) {
                    st.assign(1, 0);
                    broken = false;
                } else if (was == 2) {
                    if (!elems.empty()) {
                        lines.push_back(elems);
                        blank = false;
                    }
                } else {
                    if (!token.empty()) {
                        if (top() == 0) token += c;
                    } else if (!elems.empty() && elems.back().name.empty()) {
                        token += '\n';
                    }
                    flush(top());
                    if (!elems.empty()) {
                        lines.push_back(elems);
                        blank = false;
                    } else {
                        if (!blank) lines.emplace_back();
                        blank = true;
                    }
                }
                clearLine();
                continue;
            }
            if (c == '\t' || c == ' ') {
                if (top() == 4) {
                    elem.name = token;
                    token.clear();
                    st.back() = 3;
                    st.push_back(6);
                } else if (top() == 3) {
                    st.push_back(6);
                } else if (c == ' ' && top() == 0) {
                    token += c;
                }
                continue;
            }
            if (c == '"' && top() == 7) {
                st.push_back(1);
                continue;
            }
            if (c == '=' && top() == 6) {
                attrName = token;
                token.clear();
                st.back() = 7;
                continue;
            }
            token += c;
        }
    }
};

NativeObject* makeN1Reader(HSQUIRRELVM v) {
    auto* r = new N1Reader;
    r->name = argString(v, 2);
    return r;
}

#define N1_SELF                          \
    N1Reader* r = self<N1Reader>(v);     \
    if (!r) return sq_throwerror(v, "SQN1ScriptReader: not an instance");

// read(path): 1, or 0 if the file does not open (0x51b280 -> 0x521790). The lines add to those read
// before.
SQInteger n1Read(HSQUIRRELVM v) {
    N1_SELF
    auto data = host(v).readAnyFile(argString(v, 2));
    if (!data) {
        sq_pushinteger(v, 0);
        return 1;
    }
    size_t before = r->lines.size();
    r->read(data->data(), data->size());
    auto& h = host(v);
    if (h.log) h.log("n1 read: " + argString(v, 2) + " (" + std::to_string(r->lines.size() - before) + " lines)");
    sq_pushinteger(v, 1);
    return 1;
}
// getLine(n): the line's elements as tables {name, value, attr = {NAME = value, ...}}, or null past
// the last line (0x51b330).
SQInteger n1GetLine(HSQUIRRELVM v) {
    N1_SELF
    SQInteger n = argInt(v, 2);
    if (n < 0 || static_cast<size_t>(n) >= r->lines.size()) {
        sq_pushnull(v);
        return 1;
    }
    const N1Line& line = r->lines[static_cast<size_t>(n)];
    sq_newarray(v, 0);
    for (const N1Element& e : line) {
        sq_newtable(v);
        sq_pushstring(v, "name", -1);
        pushString(v, e.name);
        sq_newslot(v, -3, SQFalse);
        sq_pushstring(v, "value", -1);
        pushString(v, e.value);
        sq_newslot(v, -3, SQFalse);
        sq_pushstring(v, "attr", -1);
        sq_newtable(v);
        for (const auto& [k, val] : e.attrs) {
            pushString(v, k);
            pushString(v, val);
            sq_rawset(v, -3);  // a later attribute of the same name replaces the earlier
        }
        sq_newslot(v, -3, SQFalse);
        sq_arrayappend(v, -2);
    }
    return 1;
}
// getLineNum(): the lines read (0x51b770).
SQInteger n1GetLineNum(HSQUIRRELVM v) {
    N1_SELF
    sq_pushinteger(v, static_cast<SQInteger>(r->lines.size()));
    return 1;
}

}  // namespace

std::vector<NativeImpl> n1Globals() { return {{"ConvertN1CalcString", convertN1CalcString}, {"CheckFileExists", checkFileExists}}; }

ClassNatives n1ScriptReaderNatives() {
    return {{{"read", n1Read}, {"getLine", n1GetLine}, {"getLineNum", n1GetLineNum}}, makeN1Reader};
}

ClassNatives iniFileReaderNatives() {
    return {{{"load", iniLoad}, {"getInt", iniGetInt}, {"getString", iniGetString}}, makeIniReader};
}

}  // namespace nyan::diesel
