#include "nyan/wa2/script.hpp"

#include <cstring>
#include <sstream>

#include "nyan/common/sjis.hpp"

namespace nyan::wa2 {

namespace {

constexpr size_t kFncRecord = 0x78;

bool sjisLead(uint8_t c) {
    return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

// Operands after the sub-op of a Control instruction, by sub-op (0x4532f0); -1: a switch's table.
int controlOperands(uint32_t sub) {
    switch (sub) {
    case 2: return 2;   // if: else, end
    case 3: return 1;   // else: its end
    case 5: return 4;   // for: end, condition, step, body
    case 6: return 1;   // loop: end
    case 7: return -1;  // switch: count, (target, value) pairs, end
    case 12: return 1;  // goto
    default: return sub <= 16 ? 0 : -2;
    }
}

}  // namespace

void FunctionTable::load(ByteSpan d) {
    if (d.size() < 4) return;
    int id = static_cast<int>(readLE32(d.data()));
    size_t at = 4;
    while (at + kFncRecord <= d.size()) {
        ScriptFunction f;
        f.id = id;
        const uint8_t* r = d.data() + at;
        size_t len = 0;
        while (len < 0x20 && r[len]) ++len;
        f.name.assign(reinterpret_cast<const char*>(r), len);
        int argc = r[0x20];
        f.flag = r[0x21];
        f.returnType = static_cast<int>(readLE32(r + 0x24));
        for (int i = 0; i < argc && 0x28 + i < static_cast<int>(kFncRecord); ++i) f.argTypes.push_back(r[0x28 + i]);
        at += kFncRecord;
        for (int i = 0; i < argc; ++i) {
            size_t comma = at;
            while (comma < d.size() && d[comma] != ',') ++comma;
            f.defaults.emplace_back(reinterpret_cast<const char*>(d.data() + at), comma - at);
            at = comma + 1;
        }
        functions_[id] = std::move(f);
        ++id;
    }
}

const ScriptFunction* FunctionTable::find(int id) const {
    auto it = functions_.find(id);
    return it == functions_.end() ? nullptr : &it->second;
}

const ScriptFunction* FunctionTable::find(std::string_view name) const {
    for (const auto& [id, f] : functions_) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

Script Script::parse(Bytes data) {
    if (data.size() < 12 || std::memcmp(data.data(), "LSCR", 4) != 0) fail("not an LSCR script");
    Script s;
    uint32_t count = readLE32(&data[8]);
    if (12 + static_cast<uint64_t>(count) * 8 > data.size()) fail("LSCR: label table past the end");
    for (uint32_t i = 0; i < count; ++i) {
        s.labels[static_cast<int>(readLE32(&data[12 + 8 * i]))] = readLE32(&data[16 + 8 * i]);
    }
    s.codeStart = 12 + 8 * count;
    s.data = std::move(data);
    return s;
}

uint32_t Script::word(uint32_t offset) const {
    if (offset + 4 > data.size()) fail("script read past the end at " + std::to_string(offset));
    return readLE32(&data[offset]);
}

std::vector<std::string> splitStrings(std::string_view t) {
    std::vector<std::string> out;
    size_t i = 0;
    // 0x452f20 over 0x44d950: a field starting with the comma is empty; after a non-empty field the
    // comma that ends it is skipped.
    while (i < t.size()) {
        if (t[i] == ',') {
            out.emplace_back();
            ++i;
            continue;
        }
        std::string field;
        while (i < t.size() && t[i] != ',') {
            if (sjisLead(static_cast<uint8_t>(t[i])) && i + 1 < t.size()) field += t[i++];
            field += t[i++];
        }
        out.push_back(std::move(field));
        if (i < t.size()) ++i;  // the comma ending it
    }
    out.emplace_back();
    return out;
}

Instruction decodeInstruction(const Script& s, uint32_t offset) {
    Instruction in;
    in.offset = offset;
    uint32_t op = s.word(offset);
    in.op = static_cast<Op>(op);
    uint32_t at = offset + 4;
    auto take = [&] {
        uint32_t w = s.word(at);
        at += 4;
        in.operands.push_back(w);
        return w;
    };
    switch (in.op) {
    case Op::Control: {
        uint32_t sub = take();
        int n = controlOperands(sub);
        if (n == -2) fail("unknown control sub-op " + std::to_string(sub) + " at " + std::to_string(offset));
        if (n == -1) {
            uint32_t cases = take();
            for (uint32_t i = 0; i < cases; ++i) take(), take();
            take();
        } else {
            for (int i = 0; i < n; ++i) take();
        }
        break;
    }
    case Op::Global:
    case Op::Local:
    case Op::String:
    case Op::Call:
    case Op::Operator: take(); break;
    case Op::Push: take(), take(); break;
    default: fail("unknown opcode " + std::to_string(op) + " at " + std::to_string(offset));
    }
    return in;
}

namespace {

std::string quote(const std::string& sjis) {
    std::string u = sjisToUtf8(sjis);
    std::string out = "\"";
    for (char c : u) {
        if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out + "\"";
}

const char* binaryName(uint32_t op) {
    static const char* names[] = {"=",  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "==", "<",  ">",  "<=",
                                  ">=", "&&", "||", "!=", "+",  "-",  "*",  "/",  "%",  "&",  "|"};
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}

}  // namespace

std::string disassemble(const Script& s, const std::vector<std::string>& strings, const FunctionTable& functions) {
    std::ostringstream out;
    std::map<uint32_t, std::vector<int>> labelsAt;
    for (const auto& [id, off] : s.labels) labelsAt[off].push_back(id);
    std::vector<std::string> stack;
    auto pop = [&] {
        if (stack.empty()) return std::string("<empty>");
        std::string v = std::move(stack.back());
        stack.pop_back();
        return v;
    };
    auto line = [&](uint32_t off, const std::string& text) { out << off << ": " << text << "\n"; };
    uint32_t at = s.codeStart;
    while (at < s.data.size()) {
        if (auto it = labelsAt.find(at); it != labelsAt.end()) {
            for (int id : it->second) out << "label " << id << ":\n";
        }
        Instruction in = decodeInstruction(s, at);
        const auto& o = in.operands;
        switch (in.op) {
        case Op::Global: stack.push_back("g" + std::to_string(o[0])); break;
        case Op::Local: stack.push_back("L" + std::to_string(o[0])); break;
        case Op::String:
            stack.push_back(o[0] < strings.size() ? quote(strings[o[0]]) : "str#" + std::to_string(o[0]));
            break;
        case Op::Push:
            if (o[0] == 4) {
                float f;
                std::memcpy(&f, &o[1], 4);
                std::ostringstream v;
                v << f << "f";
                stack.push_back(v.str());
            } else {
                stack.push_back(std::to_string(static_cast<int32_t>(o[1])) + (o[0] == 3 ? "" : "<t" + std::to_string(o[0]) + ">"));
            }
            break;
        case Op::Operator: {
            uint32_t op = o[0];
            if (op <= 0x16) {
                std::string b = pop(), a = pop();
                stack.push_back(op <= 7 ? a + " " + binaryName(op) + " " + b : "(" + a + " " + binaryName(op) + " " + b + ")");
            } else if (op == 0x17) {
                stack.push_back("-" + pop());
            } else if (op == 0x18) {
                stack.push_back("!" + pop());
            } else if (op == 0x19 || op == 0x1a) {
                stack.push_back(pop() + (op == 0x19 ? "++" : "--"));
            } else if (op == 0x1b) {
                std::string index = pop();
                std::string base = pop();
                stack.push_back(base + "[" + index + "]");
            } else if (op == 0x1e) {
                line(in.offset, pop());
            } else {
                line(in.offset, "operator " + std::to_string(op));
            }
            break;
        }
        case Op::Call: {
            const ScriptFunction* f = functions.find(static_cast<int>(o[0]));
            size_t argc = f ? f->argTypes.size() : 0;
            std::vector<std::string> args(argc);
            for (size_t i = argc; i-- > 0;) args[i] = pop();
            std::string call = (f ? f->name : "fn" + std::to_string(o[0])) + "(";
            for (size_t i = 0; i < argc; ++i) call += (i ? ", " : "") + args[i];
            stack.push_back(call + ")");
            break;
        }
        case Op::Control: {
            uint32_t sub = o[0];
            std::string t;
            switch (sub) {
            case 0: t = "nop"; break;
            case 1: t = "end"; break;
            case 2: t = "if-block else@" + std::to_string(o[1]) + " end@" + std::to_string(o[2]); break;
            case 3: t = "else end@" + std::to_string(o[1]); break;
            case 4: t = "else-skip"; break;
            case 5: t = "for-block end@" + std::to_string(o[1]) + " cond@" + std::to_string(o[2]) + " step@" + std::to_string(o[3]) + " body@" + std::to_string(o[4]); break;
            case 6: t = "loop-block end@" + std::to_string(o[1]); break;
            case 7: {
                t = "switch-block";
                for (uint32_t i = 0; i < o[1]; ++i) {
                    int32_t v = static_cast<int32_t>(o[3 + 2 * i]);
                    t += (v == INT32_MIN ? " default" : " case " + std::to_string(v)) + "@" + std::to_string(o[2 + 2 * i]);
                }
                t += " end@" + std::to_string(o.back());
                break;
            }
            case 8: t = "debug-break"; break;
            case 9: t = "debug-trace"; break;
            case 10: t = "break"; break;
            case 11: t = "continue"; break;
            case 12: t = "goto @" + std::to_string(o[1]); break;
            case 13: t = "if (" + pop() + ")"; break;
            case 14: t = "for-test (" + pop() + ")"; break;
            case 15: t = "while (" + pop() + ")"; break;
            case 16: t = "switch (" + pop() + ")"; break;
            }
            line(in.offset, t);
            break;
        }
        }
        at += in.size();
    }
    if (!stack.empty()) {
        out << "; left on the stack:";
        for (const auto& v : stack) out << " " << v;
        out << "\n";
    }
    return out.str();
}

}  // namespace nyan::wa2
